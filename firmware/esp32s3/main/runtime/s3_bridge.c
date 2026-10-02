#include "buddy_model_store.h"
#include "buddy_link.h"
/* All BLE, persistence, adapter and management work runs on NimBLE's queue.
 * USB accesses only copied buffers/snapshots in s3_runtime, never a slot. */
#include "buddy_management.h"
#include "buddy_probe.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "faults.h"
#include "hci_probe.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "s3_gatt.h"
#include "s3_runtime.h"
#include "standalone.h"
#include <stdio.h>
#include <string.h>
#define SLOTS BUDDY_SLOTS
#define CANDIDATES 24
#define NONE BLE_HS_CONN_HANDLE_NONE
#include "buddy_models.h"
#include "buddy_record.h"
#include "buddy_bond_storage.h"
#include "buddy_update.h"
#include "buddy_catalog_store.h"
#include "buddy_catalog_query.h"
#include "esp_app_desc.h"
extern size_t s3_rx_read(uint8_t *, size_t);
void ble_store_config_init(void);
struct rbp_server {
  unsigned slot;
  uint32_t generation;
  const rbp_device_profile_t *profile;
  uint8_t battery, charging, voice_state;
  rbp_link_state_t state;
  uint64_t keys;
  bool voice_down;
  char error[80];
};
typedef struct {
  struct rbp_server model;
  rc003_adapter_t adapter;
  s3_gatt_t gatt;
  record_t record;
  buddy_bond_t bond;
  bool migrate_binding;
  rbp_device_profile_t owned_profile;
  rbp_key_def_t owned_keys[63];
  char owned_labels[63][48];
  int pending_model, requested_model;
  uint16_t conn;
  uint32_t retry_at, deadline, cache_hash, cache_retry, close_retry;
  uint32_t link_started_ms, ready_after_ms, disconnected_ms, disconnect_reason;
#ifdef S3_HCI_PROBE
  struct ble_gap_upd_params requested_params;
  bool requested_valid;
  uint32_t request_ms, request_result, notify_bytes, notify_count;
#endif
  bool cache_checked;
  bool pairing, forgetting, connecting, commit_pending, failed,
      repair_attempted, verifying_identity, identity_seen;
  ble_addr_t target, identity;
  uint32_t provisional_id;

} slot_t;
static slot_t slots[SLOTS];
#ifdef S3_HCI_PROBE
static uint32_t diagnostic_hold_mask, diagnostic_hold_until;
static bool diagnostic_held(unsigned slot, uint32_t now) {
  if ((int32_t)(now-diagnostic_hold_until)>=0) diagnostic_hold_mask=0;
  return (diagnostic_hold_mask & (1u<<slot))!=0;
}
#endif
/* BLE-task transaction scratch; never published until persist succeeds. */
static record_t transaction_record;
static nvs_handle_t storage;
static buddy_model_t *models;
static uint32_t model_crc[BUDDY_MODELS];
static rbp_device_profile_t model_profiles[BUDDY_MODELS];
static rbp_key_def_t model_keys[BUDDY_MODELS][63];
static struct {
  char bytes[BUDDY_MODEL_BYTES];
  uint32_t length, used, token, at;
} model_transfer;
static buddy_model_store_t model_store(void);
static void models_load(void);
static void models_bootstrap(void);
static int model_index(const char *);
static const buddy_model_t *slot_model(const slot_t *);
static const buddy_model_t *active_model(const slot_t *);
static const buddy_model_t *model_at(int);
static int model_match_name(const char *,int,bool);
static struct {
  ble_addr_t address;
  char name[48];
  uint32_t id, seen, cooldown;
  int rssi, company;
  bool known, connectable, name_complete, hid_hint;
  uint8_t adv[31], response[31], adv_len, response_len;
} candidates[CANDIDATES];
static uint32_t next_candidate, scan_epoch, scan_until, serial_epoch;
static uint8_t own_address;
static bool synced, scanning, manual_scan, privacy_repair_pending;
static bool sync_restore_pending;
static uint32_t sync_retry;
static void sync_cb(void);
static int connecting_slot = -1;
static uint32_t next_scan;
static bool had_management;
static bool raw_identity_scan;
static uint32_t scan_generation;
extern int ble_hs_pvcy_set_resolve_enabled(int);
/* Fixed-size scan diagnostics, recorded before filtering. No per-packet logging. */
static struct {
  uint32_t total, zero, weak, malformed, bound[SLOTS], last_ms;
  ble_addr_t last_address;
  int last_rssi;
  uint8_t last_event;
} scan_diag;
static struct ble_npl_callout timer;
static struct {
  uint32_t id, next, peer;
  unsigned slot;
  uint16_t result;
  bool pending, uncertain;
  char kind[12];
  ble_addr_t target;
  char model_id[BUDDY_MODEL_ID];
  uint8_t model_error;
} operation;
static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static void fault(unsigned s, unsigned stage, int rc) {
  if (rc)
    rbp_fault_record(RBP_FAULT_SDK, 0x1100 + stage, (uint32_t)rc, s);
}
static int gap(struct ble_gap_event *, void *);

static int identity_slot(const ble_addr_t *a) {
  for (unsigned s = 0; s < SLOTS; s++)
    if (slots[s].record.peer_id && buddy_address_equal(a, &slots[s].record.address))
      return s;
  return -1;
}
bool buddy_identity_is_bound(const ble_addr_t *a) {
  for (unsigned i = 0; i < SLOTS; i++)
    if ((slots[i].record.peer_id || slots[i].record.cleanup_pending) &&
        buddy_address_equal(a, &slots[i].record.address)) return true;
  return false;
}
static uint32_t new_peer_id(void) {
  for (;;) {
    uint32_t id = esp_random() | 1u;
    bool duplicate = false;
    for (unsigned i = 0; i < SLOTS; i++)
      if (slots[i].record.peer_id == id)
        duplicate = true;
    if (!duplicate)
      return id;
  }
}
static unsigned bound_count(void) {
  unsigned n = 0;
  for (unsigned i = 0; i < SLOTS; i++) n += slots[i].record.peer_id != 0;
  return n;
}
static unsigned link_count(void) {
  unsigned n = buddy_probe_link_busy() ? 1 : 0;
  for (unsigned i = 0; i < SLOTS; i++)
    n += slots[i].conn != NONE || slots[i].connecting;
  return n;
}
static int free_slot(void) {
  if (bound_count() >= BUDDY_MAX_REMOTES) return -1;
  for (unsigned s = 0; s < SLOTS; s++)
    if (!slots[s].record.peer_id && !slots[s].pairing && !slots[s].forgetting &&
        slots[s].conn == NONE && !slots[s].connecting && !slots[s].identity_seen)
      return s;
  return -1;
}
/* Preserve persistent records while verifying a re-pairing privacy address. */
static bool pairing_busy(void) {
  for (unsigned i = 0; i < SLOTS; i++)
    if (slots[i].pairing)
      return true;
  return false;
}
static int pairing_slot(const ble_addr_t *address) {
  if (bound_count() > BUDDY_MAX_REMOTES || link_count() >= BUDDY_MAX_REMOTES) return -1;
  int found = identity_slot(address);
  if (found >= 0)
    return found;
  found = free_slot();
  if (found >= 0)
    return found;
  for (unsigned i = 0; i < SLOTS; i++)
    if (slots[i].record.peer_id && slots[i].conn == NONE && !slots[i].connecting && !slots[i].forgetting &&
        !slots[i].pairing && !slots[i].identity_seen)
      return i;
  return -1;
}
static void changed(unsigned s) {
  cJSON *j = cJSON_CreateObject();
  cJSON_AddNumberToObject(j, "slot", s);
  cJSON_AddNumberToObject(j, "peer_id", slots[s].record.peer_id);
  cJSON_AddNumberToObject(j, "generation", slots[s].model.generation);
  cJSON_AddNumberToObject(j, "state", slots[s].model.state);
  buddy_management_event(BUDDY_CHANGED, j);
}
static void set_state(slot_t *s, rbp_link_state_t state) {
  if (s->model.state != state) {
    if (state == RBP_LINK_READY)
      s->ready_after_ms = now_ms() - s->link_started_ms;
    else
      s->cache_checked = false;
    s->model.state = state;
    changed(s->model.slot);
  }
}
static bool stop_scan(void);
#include "buddy_binding.inc"
#include "buddy_snapshot.inc"
#include "buddy_discovery.inc"
/* Host-task-only bounded control trace. No audio payload or flash writes. */
#ifdef S3_HCI_PROBE
typedef struct {
  uint32_t sequence, time, slot, event, a, b, c;
} link_trace_t;
static link_trace_t link_trace[64];
static uint32_t link_sequence;
void s3_link_trace(unsigned slot, unsigned event, uint32_t a, uint32_t b,
                   uint32_t c) {
  uint32_t seq = ++link_sequence;
  link_trace[(seq - 1) % 64] =
      (link_trace_t){seq, now_ms(), slot, event, a, b, c};
}
#endif
static int gap(struct ble_gap_event *e, void *arg) {
  if (e->type == BLE_GAP_EVENT_DISC) {
    if (scanning && (uintptr_t)arg == scan_generation)
      on_scan(&e->disc);
    return 0;
  }
  if (e->type == BLE_GAP_EVENT_DISC_COMPLETE) {
    if ((uintptr_t)arg != scan_generation) return 0;
    scanning = false;
    next_scan = now_ms() + 100;
    return 0;
  }
  uintptr_t token = (uintptr_t)arg;
  slot_t *s = &slots[token & 3];
  if ((token >> 2) != s->model.generation)
    return 0;
  switch (e->type) {
  case BLE_GAP_EVENT_CONN_UPDATE_REQ:
  case BLE_GAP_EVENT_L2CAP_UPDATE_REQ:
#ifdef S3_HCI_PROBE
    s->requested_params = *e->conn_update_req.peer_params;
    s->requested_valid = true;
    s->request_ms = now_ms();
#endif
    s3_link_trace(s->model.slot, 5, e->conn_update_req.peer_params->itvl_min,
                  e->conn_update_req.peer_params->itvl_max,
                  e->conn_update_req.peer_params->latency);
    /* Accept the peer proposal through NimBLE; do not impose a radio grid. */
    break;
  case BLE_GAP_EVENT_CONN_UPDATE: {
    if (e->conn_update.conn_handle != s->conn)
      break;
#ifdef S3_HCI_PROBE
    struct ble_gap_conn_desc d;
    int rc = ble_gap_conn_find(s->conn, &d);
    s3_link_trace(s->model.slot, 6, e->conn_update.status, rc ? 0 : d.conn_itvl,
                  rc ? 0 : d.conn_latency);
#endif
    if (e->conn_update.status) {
      fault(s->model.slot, 30, e->conn_update.status);
    }
    break;
  }
  case BLE_GAP_EVENT_CONNECT:
#ifdef S3_HCI_PROBE
    s->requested_valid = false;
    s->notify_bytes = s->notify_count = 0;
    s->request_ms = s->request_result = 0;
#endif
    s->connecting = false;
    if (connecting_slot == (int)s->model.slot)
      connecting_slot = -1;
    if (e->connect.status) {
      fault(s->model.slot, 8, e->connect.status);
      fail_pair(s, RBP_STATUS_PAIRING_FAILED);
      set_state(s,
                s->record.peer_id ? RBP_LINK_DISCONNECTED : RBP_LINK_UNBOUND);
      s->retry_at = now_ms() + 1500;
      next_scan = now_ms();
      if (s->forgetting)
        finish_forget(s);
      break;
    }
    s->conn = e->connect.conn_handle;
    s3_gatt_attach(&s->gatt, &s->adapter, s->conn);
    if (s->forgetting || s->failed) {
      terminate(s);
      break;
    }
    {
      int rc = ble_gap_security_initiate(s->conn);
      fault(s->model.slot, 9, rc);
      if (rc)
        fail_pair(s, RBP_STATUS_PAIRING_FAILED);
    }
    break;
  case BLE_GAP_EVENT_ENC_CHANGE: {
    if (e->enc_change.conn_handle != s->conn)
      break;
    if (s->forgetting || s->failed) {
      terminate(s);
      break;
    }
    struct ble_gap_conn_desc d;
    int rc = ble_gap_conn_find(s->conn, &d);
    fault(s->model.slot, 10, e->enc_change.status ? e->enc_change.status : rc);
    /* NimBLE also emits this while tearing down an initial link before its
     * configured connection reattempt. Do not cancel that reattempt by issuing
     * terminate here. GAP CONNECT/DISCONNECT (or the existing deadline) owns
     * the final outcome; this notification is not an authentication failure. */
    if (e->enc_change.status == BLE_HS_ENOTCONN)
      break;
    /* NimBLE ble_sm_enc_event_rx maps the controller status through
     * BLE_HS_HCI_ERR. Only proven missing keys permit bond repair; a timeout,
     * interference or generic authentication failure must not erase a bond. */
    if (!rc && e->enc_change.status == BLE_HS_HCI_ERR(BLE_ERR_PINKEY_MISSING) &&
        !s->repair_attempted && s->record.peer_id &&
        buddy_address_equal(&s->record.address, &d.peer_id_addr) &&
        (!buddy_probe_active() &&
         (s->pairing || (!buddy_management_active() && !pairing_busy())))) {
      s->repair_attempted = true;
      int deleted = buddy_bond_remove_ram(&d.peer_id_addr);
      fault(s->model.slot, 23, deleted);
      if (!deleted || deleted == BLE_HS_ENOENT) {
        s->pairing = true;
        s->adapter.cache_valid = false;
        s->cache_hash = 0;
        int restart = ble_gap_security_initiate(s->conn);
        fault(s->model.slot, 24, restart);
        if (!restart) {
          set_state(s, RBP_LINK_PAIRING);
          break;
        }
      }
    }
    if (rc || e->enc_change.status || !d.sec_state.encrypted ||
        (s->pairing && !d.sec_state.bonded)) {
      fail_pair(s, RBP_STATUS_PAIRING_FAILED);
      break;
    }
    accept_encrypted(s, &d);
    break;
  }
  case BLE_GAP_EVENT_DISCONNECT:
    if (e->disconnect.conn.conn_handle != s->conn)
      break;
    fault(s->model.slot, 11, e->disconnect.reason);
    release_link(s, e->disconnect.reason);
    if (s->forgetting)
      finish_forget(s);
    break;
  case BLE_GAP_EVENT_NOTIFY_RX: {
    if (e->notify_rx.conn_handle != s->conn || s->failed || s->forgetting)
      break;
    uint8_t data[512];
    unsigned n = OS_MBUF_PKTLEN(e->notify_rx.om);
#ifdef S3_HCI_PROBE
    s->notify_bytes += n;
    s->notify_count++;
#endif
    if (n > sizeof data || os_mbuf_copydata(e->notify_rx.om, 0, n, data)) {
      fault(s->model.slot, 12, BLE_HS_EBADDATA);
      terminate(s);
      break;
    }
    if (e->notify_rx.attr_handle == s->adapter.atvv_ctl_handle ||
        e->notify_rx.attr_handle == s->adapter.unicom.f8)
      s3_link_trace(s->model.slot, 3, e->notify_rx.attr_handle,
                    n ? data[0] | ((uint32_t)(n > 1 ? data[1] : 0) << 8) |
                            ((uint32_t)(n > 2 ? data[2] : 0) << 16) |
                            ((uint32_t)(n > 3 ? data[3] : 0) << 24)
                      : 0,
                    n);
    for (unsigned i = 0; i < s->adapter.char_count; i++) {
      if (s->adapter.chars[i].value_handle == e->notify_rx.attr_handle &&
          s->adapter.chars[i].report_type == 1 &&
          (s->adapter.chars[i].report_id == 1 || s->adapter.chars[i].report_id == 3)) {
        uint32_t first = 0;
        for (unsigned k = 0; k < n && k < 4; k++) first |= (uint32_t)data[k] << (8*k);
        s3_link_trace(s->model.slot, 7, s->adapter.chars[i].report_id, first, n);
        break;
      }
    }
    hci_probe_app(s->conn, e->notify_rx.attr_handle, data, n);
    s3_gatt_notify(&s->gatt, e->notify_rx.attr_handle, data, n);
    break;
  }
  case BLE_GAP_EVENT_REPEAT_PAIRING:
    if (buddy_probe_active())
      return BLE_GAP_REPEAT_PAIRING_IGNORE;
    if (s->forgetting || s->failed)
      return BLE_GAP_REPEAT_PAIRING_IGNORE;
    if (!buddy_probe_active() &&
        (s->pairing || (!buddy_management_active() && !pairing_busy()))) {
      struct ble_gap_conn_desc d;
      int rc = ble_gap_conn_find(s->conn, &d);
      if (!rc && (!s->record.peer_id ||
                  buddy_address_equal(&s->record.address, &d.peer_id_addr) ||
                  (s->verifying_identity && identity_slot(&d.peer_id_addr) >= 0 &&
                   slots[identity_slot(&d.peer_id_addr)].conn == NONE))) {
        rc = buddy_bond_remove_ram(&d.peer_id_addr);
        fault(s->model.slot, 13, rc);
        if (!rc || rc == BLE_HS_ENOENT) {
          s->adapter.cache_valid = false;
          s->pairing = true;
          return BLE_GAP_REPEAT_PAIRING_RETRY;
        }
      }
    }
    return BLE_GAP_REPEAT_PAIRING_IGNORE;
  case BLE_GAP_EVENT_PASSKEY_ACTION:
    fail_pair(s, RBP_STATUS_UNSUPPORTED);
    break;
  default:
    break;
  }
  return 0;
}
#include "buddy_probe_voice.inc"
/* Adapter sinks have an explicit slot context; no global "selected device". */
void rbp_server_set_profile(rbp_server_t *m, const rbp_device_profile_t *p) {
  if(m==&pv_slot.model){m->profile=p;return;}
  const buddy_model_t *variant = active_model(&slots[m->slot]);
  m->profile = variant ? &slots[m->slot].owned_profile
               : !strcmp(p->model_id, "xiaomi.rc003") ? &RBP_PROFILE_RC003_VOICE
                                                      : p;
}
void rbp_server_set_voice_caps(rbp_server_t *m, const rbp_audio_caps_t *caps) {
  (void)m;
  (void)caps;
}
void rbp_server_on_link(rbp_server_t *m, rbp_link_state_t state,
                        uint32_t connection, uint32_t now) {
  (void)connection;
  (void)now;
  if(m==&pv_slot.model){m->state=state;return;}
  if(state==RBP_LINK_READY && slots[m->slot].commit_pending)return;
  set_state(&slots[m->slot], state);
}
void rbp_server_adapter_failed(rbp_server_t *m, const char *reason) {
  if(m==&pv_slot.model){pv_fail(reason);snprintf(m->error,sizeof m->error,"%s",reason);return;}
  hci_probe_freeze();
  snprintf(m->error, sizeof m->error, "%s", reason);
  set_state(&slots[m->slot], RBP_LINK_ERROR);
  fail_pair(&slots[m->slot], RBP_STATUS_DEVICE_ERROR);
}
void rbp_server_on_battery(rbp_server_t *m, uint8_t b, uint8_t c) {
  m->battery = b;
  m->charging = c;
}
void rbp_server_on_voice_state(rbp_server_t *m, uint8_t state,
                               uint8_t interaction, uint32_t rate) {
  (void)interaction;
  (void)rate;
  m->voice_state = state;
}
void standalone_peer_voice_key(rbp_server_t *m, bool down, uint32_t now) {
  if(m==&pv_slot.model){pv_key(down,now);return;}
  s3_link_trace(m->slot, 1, down, s3_voice_owner(), m->generation);
  if (buddy_update_busy() || buddy_probe_active() || slots[m->slot].commit_pending)
    return;
  m->voice_down = down;
  s3_peer_voice_key(m->slot, m->generation, down, now);
  /* Stop discovery in this callback, before the adapter submits MIC_START.
   * Waiting for the next scheduler tick leaves an avoidable overlap. Existing
   * connections remain live; only discovery/initiation yields to voice. */
  if (standalone_busy())
    stop_scan();
}
bool rbp_server_voice_wanted(const rbp_server_t *m) {
  if(m==&pv_slot.model)return pv_active&&pv_armed;
  return !buddy_update_busy() && !buddy_probe_active() && !slots[m->slot].commit_pending && s3_peer_wanted(m->slot, m->generation);
}
uint32_t rbp_server_voice_capture_session(const rbp_server_t *m) {
  return rbp_server_voice_wanted(m) ? m->generation : 0;
}
void rbp_server_on_voice(rbp_server_t *m, const rbp_voice_evt_t *e,
                         uint32_t now) {
  if(m==&pv_slot.model){pv_voice(e,now);return;}
  if (e->type != RBP_VOICE_EVT_ENCODED)
    s3_link_trace(m->slot, 2, e->type, s3_voice_owner(),
                  e->type == RBP_VOICE_EVT_START ||
                          e->type == RBP_VOICE_EVT_FORMAT
                      ? e->u.format.codec_id
                      : 0);
  if(e->type==RBP_VOICE_EVT_SOURCE_BEGIN && rbp_server_voice_wanted(m))hci_probe_reset();
  s3_peer_voice(m->slot, m->generation, e, now);
  if(e->type==RBP_VOICE_EVT_END && s3_voice_owner()==m->slot)hci_probe_freeze();
  if (standalone_busy())
    stop_scan();
}
void rbp_server_on_keys(rbp_server_t *m, const rbp_keys_report_t *r) {
  if(m==&pv_slot.model)return;
  if (buddy_update_busy() || slots[m->slot].commit_pending)
    return;
  uint64_t logical = 0;
  for (unsigned k = 0; k < m->profile->key_count; k++)
    if (r->pressed_bits & (UINT64_C(1) << k))
      logical |= UINT64_C(1) << m->profile->keys[k].key_id;
  const buddy_model_t *variant = active_model(&slots[m->slot]);
  if (variant) {
    uint64_t mask = 0;
    for (unsigned k = 0; k < variant->key_count; k++)
      mask |= UINT64_C(1) << variant->keys[k];
    logical &= mask;
  }
  m->keys = logical;
  uint64_t actions = s3_peer_keys(m->slot, m->generation, logical);
  for (unsigned k = 1; k < 64; k++)
    if (actions & (UINT64_C(1) << k)) {
      cJSON *j = cJSON_CreateObject();
      cJSON_AddNumberToObject(j, "slot", m->slot);
      cJSON_AddNumberToObject(j, "peer_id", slots[m->slot].record.peer_id);
      cJSON_AddNumberToObject(j, "generation", m->generation);
      cJSON_AddNumberToObject(j, "key", k);
      cJSON_AddNumberToObject(j, "action",
                              slots[m->slot].record.map.key[k].value);
      cJSON_AddNumberToObject(j, "time_ms", now_ms());
      buddy_management_event(BUDDY_ACTION, j);
    }
}
static void schedule(uint32_t now) {
  /* Never choose which of the user's legacy bindings to discard or activate. */
  if (bound_count() > BUDDY_MAX_REMOTES) { stop_scan(); return; }
  if (link_count() >= BUDDY_MAX_REMOTES) { stop_scan(); return; }
  if(privacy_repair_pending && !raw_identity_scan && connecting_slot<0 && !buddy_probe_link_busy()) {
    stop_scan();
    /* scan_start removes only the SDK placeholder, preserving real bonds. */
  }

  if (buddy_probe_active()) {
    if (standalone_busy())
      stop_scan();
    if (buddy_probe_scanning() && !standalone_busy() && !scanning &&
        (int32_t)(now - next_scan) >= 0)
      scan_start(false, 30000);
    return;
  }
  /* The single radio is shared by all links and discovery. Never spend scan
   * windows or initiate another link while a voice owner is capturing/draining.
   * Check before operation.pending: a manual scan must also yield immediately.
   */
  if (standalone_busy() || buddy_update_busy() || buddy_catalog_store_busy()) {
    stop_scan();
    return;
  }
  if (!synced || connecting_slot >= 0 || operation.pending)
    return;
  /* Process observed advertisements before blind reconnect attempts. */
  for (unsigned i = 0; i < CANDIDATES; i++) {
    if (!candidates[i].id || !candidates[i].connectable ||
        now - candidates[i].seen > 1500 ||
        (int32_t)(now - candidates[i].cooldown) < 0)
      continue;
    int target = identity_slot(&candidates[i].address);
    bool fresh = false;
    if (target < 0 && !buddy_management_active() && !pairing_busy() &&
        candidates[i].known && !standalone_busy()) {
      target = pairing_slot(&candidates[i].address);
      fresh = target >= 0;
    }
    if (target < 0)
      continue;
    slot_t *s = &slots[target];
#ifdef S3_HCI_PROBE
    if(diagnostic_held(target,now))continue;
#endif
    if (s->conn != NONE || s->connecting || s->forgetting ||
        (int32_t)(now - s->retry_at) < 0)
      continue;
    candidates[i].cooldown = now + 10000;
    connect_address(s, &candidates[i].address, fresh);
    return;
  }
  /* NimBLE restores bonded IRKs before sync_cb. Resolved identities and
   * directed advertisements above drive reconnect, rather than spending
   * successive connection timeouts on every sleeping/offsite remote. */
  bool need_scan = free_slot() >= 0;
  for (unsigned i = 0; i < SLOTS; i++)
    if (slots[i].record.peer_id && slots[i].conn == NONE
#ifdef S3_HCI_PROBE
        && !diagnostic_held(i,now)
#endif
        )
      need_scan = true;
  if (!need_scan && !manual_scan) {
    stop_scan();
    return;
  }

  if (!scanning && (int32_t)(now - next_scan) >= 0) {
    if (manual_scan && (int32_t)(scan_until - now) > 0)
      scan_start(false, scan_until - now);
    else
      scan_start(false, 1500);
  }
}
static void pump(struct ble_npl_event *event) {
  (void)event;
  uint32_t now = now_ms();
  if (sync_restore_pending && (int32_t)(now - sync_retry) >= 0) sync_cb();
  rbp_fault_set_time(now);
  uint32_t epoch = s3_cdc_epoch();
  if (epoch != serial_epoch) {
    serial_epoch = epoch;
    if (!buddy_probe_active() || stop_scan())
      buddy_probe_tick(now, false);
    buddy_management_init();
  }
  uint8_t data[256];
  size_t n = s3_rx_read(data, sizeof data);
  if (n)
    buddy_management_rx(data, n, now);
  /* Commands may timestamp a newly armed capture after this pump began. */
  now = now_ms();
  buddy_management_tick(now);
  bool managed = buddy_management_active();
  if (managed || !buddy_probe_active() || stop_scan())
    buddy_probe_tick(now, managed);
  if (managed && !had_management)
    for (unsigned i = 0; i < SLOTS; i++)
      if (slots[i].pairing && (!operation.pending || operation.slot != i))
        fail_pair(&slots[i], RBP_STATUS_CANCELLED);
  had_management = managed;
  /* Cleanup must run during OTA quiescing too, before its early return. */
  for (unsigned i = 0; i < SLOTS; i++) {
    slot_t *s = &slots[i];
    if ((s->failed || s->forgetting) && (s->conn != NONE || s->connecting) &&
        (int32_t)(now - s->close_retry) >= 0) terminate(s);
    if (s->conn == NONE && !s->connecting && s->identity_seen &&
        (int32_t)(now - s->close_retry) >= 0) cleanup_identity(s);
  }
  bool quiet = connecting_slot < 0;
  for (unsigned i = 0; i < SLOTS; i++)
    if (slots[i].conn != NONE)
      quiet = false;
  buddy_update_tick(now, managed, synced, quiet);
  buddy_catalog_store_tick(now, managed);
  snapshot_tick(now,managed);
  if (buddy_update_busy()) {
    stop_scan();
    if (connecting_slot >= 0)
      ble_gap_conn_cancel();
    for (unsigned i = 0; i < SLOTS; i++)
      if (slots[i].conn != NONE && !slots[i].failed) {
        slots[i].failed = true;
        terminate(&slots[i]);
      }
    ble_npl_callout_reset(&timer, ble_npl_time_ms_to_ticks32(2));
    return;
  }
  standalone_tick(now);
  uint8_t stops = s3_take_stops();
  for (unsigned i = 0; i < SLOTS; i++)
    if (stops & (1u << i))
      rc003_adapter_mic_stop(&slots[i].adapter);
  for (unsigned i = 0; i < SLOTS; i++) {
    slot_t *s = &slots[i];
    if (s->forgetting && s->conn == NONE && !s->connecting)
      finish_forget(s);
    if (s->conn != NONE && !s->failed && !s->forgetting) {
      s3_gatt_poll(&s->gatt);
      if(s->gatt.identity_stage) continue;
      rc003_adapter_tick(&s->adapter, now);
      if (s->adapter.ready) {
        if (s->commit_pending)
          commit_ready(s);
      } else if ((int32_t)(now - s->deadline) >= 0)
        fail_pair(s, RBP_STATUS_TIMEOUT);
    }
    if (!s->adapter.ready)
      s->cache_checked = false;
    if (!s->cache_checked && s->adapter.ready && s->record.peer_id && !s->commit_pending &&
        !standalone_busy() && !s->failed &&
        (int32_t)(now - s->cache_retry) >= 0) {
      uint8_t cache[1200];
      size_t len = rc003_adapter_cache_export(&s->adapter, cache, sizeof cache);
      uint32_t hash = len ? rbp_crc32c(cache, len) : 0;
      if (hash && hash != s->cache_hash) {
        record_t *record = &transaction_record; *record = s->record;
        record->cache_len = len;
        memcpy(record->cache, cache, len);
        record->model = s->adapter.legacy.selected ? 3 : s->adapter.unicom.selected ? 2 : 1;
        if (persist(i, record)) {
          s->record = *record;
          s->cache_hash = hash;
          s->cache_checked = true;
        } else
          s->cache_retry = now + 5000;
      } else
        s->cache_checked = true;
    }
  }
  if (manual_scan && (int32_t)(now - scan_until) >= 0) {
    stop_scan();
    manual_scan = false;
  }
  schedule(now);
  ble_npl_callout_reset(&timer, ble_npl_time_ms_to_ticks32(2));
}
static void sync_cb(void) {
  synced = false;
  sync_restore_pending = true;
  sync_retry = now_ms() + 1000;
  /* Import the old split store once, only for real slot records. */
  for (unsigned i=0;i<SLOTS;i++) {
    slot_t *s=&slots[i];
    if(s->migrate_binding && s->record.peer_id) {
      buddy_bond_t bond;
      if(buddy_bond_capture(&s->record.address,&bond)) {
        if(!persist_bundle(i,&s->record,&bond)) { synced=false;return; }
        s->bond=bond;s->migrate_binding=false;
      } else fault(i,43,BLE_HS_ENOENT);
    }
  }
  /* Rebuild the SDK RAM view from complete slot blobs; legacy orphan keys
   * and keys from aborted in-memory transactions have no authority. */
  ble_addr_t peers[8];
  int num = 0;
  int read_rc = ble_store_util_bonded_peers(peers, &num, 8);
  fault(255, 21, read_rc);
  if (!read_rc)
    for (int i = 0; i < num; i++)
      {
        int rc = buddy_bond_remove_ram(&peers[i]);
        fault(255, 22, rc);
        if(rc && rc!=BLE_HS_ENOENT){synced=false;return;}
      }
  if(read_rc){synced=false;return;}
  for(unsigned i=0;i<SLOTS;i++)if(slots[i].record.peer_id) {
    int restore=buddy_bond_restore(&slots[i].bond);
    if(restore){fault(i,44,restore);synced=false;return;}
  }

  int rc = ble_hs_id_infer_auto(0, &own_address);
  fault(255, 14, rc);

  /* Public-address central never uses NimBLE's all-zero local-RPA placeholder.
   * Keep every real bonded peer entry; only remove the SDK placeholder. */
  if(!rc)remove_privacy_placeholder();
  synced = !rc;
  sync_restore_pending = !synced;
}
static void reset_cb(int reason) {
  sync_restore_pending = false;
  fault(255, 15, reason);
  buddy_probe_reset();
  synced = scanning = manual_scan = false;
  connecting_slot = -1;
  for (unsigned i = 0; i < SLOTS; i++) {
    slot_t *s = &slots[i];
    s3_peer_link(i, 0);
    s3_gatt_detach(&s->gatt);
    rc003_adapter_detach(&s->adapter);
    s->conn = NONE;
    s->connecting = s->pairing = s->commit_pending = false;
    s->retry_at = now_ms() + 1000;
    if (!s->forgetting)
      complete(s, RBP_STATUS_LINK_LOST, true);
    set_state(s, s->record.peer_id ? RBP_LINK_DISCONNECTED : RBP_LINK_UNBOUND);
  }
}
static int store_full(struct ble_store_status_event *event, void *arg) {
  (void)event;
  (void)arg;
  fault(255, 19, BLE_HS_ENOMEM);
  return BLE_HS_ENOMEM;
}
static void host_task(void *arg) {
  (void)arg;
  nimble_port_run();
  nimble_port_freertos_deinit();
}
void s3_bridge_start(void) {
  ESP_ERROR_CHECK(nvs_flash_init());
  ESP_ERROR_CHECK(nvs_open("remote-s3", NVS_READWRITE, &storage));
  buddy_management_init();
  buddy_catalog_store_init();
  models_bootstrap();
  for (unsigned i = 0; i < SLOTS; i++) {
    slot_t *s = &slots[i];
    s->conn = NONE;
    s->pending_model = -1;
    s->model.slot = i;
    s->model.battery = 255;
    s->record.version = RECORD_VERSION;
    buddy_map_default(&s->record.map);
    char key[12];
    snprintf(key, sizeof key, "binding%u", i);
    size_t n = 0;
    bool legacy_record = false;
    int rc = nvs_get_blob(storage, key, NULL, &n);
    if (rc == ESP_ERR_NVS_NOT_FOUND) {
      legacy_record = true;
      snprintf(key, sizeof key, "slot%u", i);
      rc = nvs_get_str(storage, key, NULL, &n);
    }
    if (rc != ESP_ERR_NVS_NOT_FOUND) {
      if (rc || !n || n > BUDDY_BINDING_BLOB_MAX) {
        fault(i, 16, rc ? rc : ESP_ERR_INVALID_SIZE);
        abort();
      }
      char *text = malloc(n);
      if (!text)
        abort();
      rc = legacy_record ? nvs_get_str(storage, key, text, &n) : nvs_get_blob(storage, key, text, &n);
      bool ok = !rc && buddy_binding_blob_decode((const uint8_t *)text, n,
                    &s->record, &s->bond, &s->migrate_binding);
      free(text);
      if (!ok) {
        fault(i, 16, rc ? rc : ESP_ERR_INVALID_VERSION);
        abort();
      }
    }
    if (buddy_record_repair_catalog_voice(&s->record) && !s->migrate_binding) {
      /* Persist the repaired snapshot with its existing bond in one transaction. */
      if (!persist_bundle(i, &s->record, &s->bond)) abort();
    }
    if (s->record.peer_id && !s->record.definition.id[0]) {
      if (!models) models_load();
      int old = -1;
      for(unsigned k=0;k<BUDDY_MODELS;k++)if(!strcmp(models[k].id,s->record.model_id)){old=(int)k;break;}
      if (old < 0 || models[old].family != s->record.model) {
        fault(i, 40, ESP_ERR_INVALID_VERSION); abort();
      }
      s->record.definition = models[old];
      s->record.version = RECORD_VERSION;
      /* Old records carry effective values, not provenance. Preserve each
       * as an explicit override, even when it equals today's defaults. */
      for (unsigned k = 0; k < s->record.definition.key_count; k++)
        s->record.overrides |= UINT64_C(1) << s->record.definition.keys[k];
      s->migrate_binding=true; /* Commit once with the imported pairing keys. */
    }
    if (s->record.peer_id &&
        (!slot_model(s) || slot_model(s)->family != s->record.model)) {
      fault(i, 40, ESP_ERR_INVALID_VERSION);
      abort();
    }
    s->forgetting = s->record.cleanup_pending;
    s3_mapping(i, &s->record.map);
    s->model.profile =
        s->record.model == 2 ? &RBP_PROFILE_UNICOM : &RBP_PROFILE_RC003_VOICE;
    s->model.state =
        s->record.peer_id ? RBP_LINK_DISCONNECTED : RBP_LINK_UNBOUND;
    s3_gatt_init(&s->gatt, i);
    rc003_adapter_init(&s->adapter, s3_gatt_client(), &s->gatt, &s->model);
    configure_model(s);
    if (s->record.peer_id && s->record.cache_len &&
        rc003_adapter_cache_import(&s->adapter, s->record.peer_id,
                                   s->record.cache, s->record.cache_len))
      s->cache_hash = rbp_crc32c(s->record.cache, s->record.cache_len);
  }
  ESP_ERROR_CHECK(buddy_update_storage_ready()?0:ESP_ERR_INVALID_VERSION);
  if(buddy_catalog_active()){free(models);models=NULL;}
  ESP_ERROR_CHECK(nimble_port_init());
  ble_hs_cfg.sync_cb = sync_cb;
  ble_hs_cfg.reset_cb = reset_cb;
  ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
  ble_hs_cfg.sm_bonding = 1;
  ble_hs_cfg.sm_sc = 1;
  ble_hs_cfg.sm_our_key_dist =
      BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
  ble_hs_cfg.sm_their_key_dist = ble_hs_cfg.sm_our_key_dist;
  ble_store_config_init();
  ble_hs_cfg.store_status_cb = store_full;
  ble_att_set_preferred_mtu(247);
  ble_npl_callout_init(&timer, nimble_port_get_dflt_eventq(), pump, NULL);
  ble_npl_callout_reset(&timer, ble_npl_time_ms_to_ticks32(2));
  nimble_port_freertos_init(host_task);
}
static slot_t *request_slot(const cJSON *q, bool peer_check) {
  uint32_t i, id;
  if (!buddy_u32(q, "slot", &i) || i >= SLOTS)
    return NULL;
  slot_t *s = &slots[i];
  if (peer_check &&
      (!buddy_u32(q, "peer_id", &id) || !id || id != s->record.peer_id))
    return NULL;
  return s;
}
static void binding_json(cJSON *j, unsigned key, buddy_binding_t b) {
  cJSON_AddNumberToObject(j, "key", key);
  cJSON_AddNumberToObject(j, "kind", b.kind);
  cJSON_AddNumberToObject(j, "modifiers", b.modifiers);
  cJSON_AddNumberToObject(j, "value", b.value);
}
#include "buddy_models_runtime.inc"
uint16_t buddy_command(uint16_t op, const cJSON *q, cJSON *j) {
  if(op>=0x466&&op<=0x46a)return snapshot_command(op,q,j);
  if(snapshot_transfer.token&&op!=BUDDY_INFO&&op!=BUDDY_SLOT&&op!=BUDDY_STATS&&op!=BUDDY_MAP_GET)return RBP_STATUS_BUSY;
#ifdef S3_HCI_PROBE
  /* Temporary diagnostic admission mask, automatically expires; no NVS write. */
  if(op==0x4f1) {
    uint32_t mask,seconds;
    if(!buddy_u32(q,"mask",&mask)||mask>=16||
       !buddy_u32(q,"seconds",&seconds)||seconds>600|| (mask&&!seconds))
      return RBP_STATUS_INVALID_ARGUMENT;
    if(standalone_busy()||buddy_probe_active()||buddy_update_busy()||pairing_busy())
      return RBP_STATUS_BUSY;
    diagnostic_hold_mask=mask;
    diagnostic_hold_until=now_ms()+seconds*1000;
    for(unsigned i=0;i<SLOTS;i++)if((mask&(1u<<i))&&
        (slots[i].conn!=NONE||slots[i].connecting)) {
      slots[i].failed=true;
      terminate(&slots[i]);
    }
    cJSON_AddNumberToObject(j,"held_mask",mask);
    cJSON_AddNumberToObject(j,"expires_ms",diagnostic_hold_until);
    return RBP_STATUS_ACCEPTED;
  }
  /* Explicit diagnostic link-only disconnect. Never remove a binding or key.
   * Existing GAP completion and discovery own release and reconnection. */
  if(op==0x4f0) {
    slot_t *s=request_slot(q,true);
    if(!s)return RBP_STATUS_INVALID_ARGUMENT;
    if(standalone_busy()||buddy_probe_active()||buddy_update_busy()||
       pairing_busy()||s->pairing||s->forgetting||s->commit_pending)
      return RBP_STATUS_BUSY;
    if(s->conn==NONE||s->model.state!=RBP_LINK_READY)return RBP_STATUS_BAD_STATE;
    s->failed=true;
    terminate(s);
    cJSON_AddNumberToObject(j,"generation",s->model.generation);
    return RBP_STATUS_ACCEPTED;
  }
#endif

  if(op==BUDDY_DB_ADOPT_DEFAULTS){
    uint32_t index,peer,revision;
    if(!buddy_u32(q,"slot",&index)||index>=SLOTS||!buddy_u32(q,"peer_id",&peer)||!buddy_u32(q,"revision",&revision))return RBP_STATUS_INVALID_ARGUMENT;
    slot_t *s=&slots[index];
    if(!peer||s->record.peer_id!=peer||s->record.map.revision!=revision)return RBP_STATUS_BAD_STATE;
    if(standalone_busy()||s->pairing||s->forgetting||buddy_catalog_store_busy()||buddy_update_busy())return RBP_STATUS_BUSY;
    const buddy_model_t *next=model_at(model_index(s->record.model_id));
    if(!next)return RBP_STATUS_NOT_FOUND;
    const buddy_model_t *old=&s->record.definition;
    if(next->family!=old->family||next->map_crc!=old->map_crc||next->raw_count!=old->raw_count)return RBP_STATUS_VERSION_MISMATCH;
    for(unsigned i=0;i<old->raw_count;i++){
      bool found=false;for(unsigned k=0;k<next->raw_count;k++)if(old->raw[i].report==next->raw[k].report&&old->raw[i].usage==next->raw[k].usage&&old->raw[i].key==next->raw[k].key)found=true;
      if(!found)return RBP_STATUS_VERSION_MISMATCH;
    }
    for(unsigned i=1;i<BUDDY_KEYS;i++)if((s->record.overrides&(UINT64_C(1)<<i))&&!buddy_model_has_key(next,i))return RBP_STATUS_VERSION_MISMATCH;
    record_t *record = &transaction_record; *record = s->record;record->definition=*next;
    for(unsigned i=1;i<BUDDY_KEYS;i++)if(!(record->overrides&(UINT64_C(1)<<i)))record->map.key[i]=next->defaults.key[i];
    record->map.revision=revision+1;if(!record->map.revision)record->map.revision=1;
    if(!persist(index,record))return RBP_STATUS_STORAGE_FAILED;
    s->record=*record;configure_model(s);s3_mapping(index,&record->map);cJSON_AddNumberToObject(j,"revision",record->map.revision);changed(index);return 0;
  }
  if (op >= BUDDY_DB_STATUS && op <= BUDDY_DB_ABORT) {
    bool quiet = !standalone_busy() && !operation.pending && !pairing_busy() && connecting_slot < 0;
    uint16_t status = buddy_catalog_store_command(op, q, j, now_ms(), quiet);
    if (!status && op == BUDDY_DB_COMMIT) { free(models);models=NULL;memset(candidates, 0, sizeof candidates); scan_epoch++; }
    return status;
  }
  if (op >= BUDDY_UPDATE_STATUS && op <= BUDDY_UPDATE_ABORT) {
    if (op == BUDDY_UPDATE_BEGIN &&
        (buddy_catalog_store_busy() || buddy_probe_active() || standalone_busy() || operation.pending ||
         pairing_busy()))
      return RBP_STATUS_BUSY;
    return buddy_update_command(op, q, j, now_ms());
  }
  if (buddy_update_busy() && op != BUDDY_INFO && op != BUDDY_SLOT &&
      op != BUDDY_STATS)
    return RBP_STATUS_BUSY;
  if (buddy_catalog_store_busy() && op != BUDDY_INFO && op != BUDDY_SLOT && op != BUDDY_STATS && op != BUDDY_MAP_GET)
    return RBP_STATUS_BUSY;
  if (op == BUDDY_PROBE_ADOPT) {
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(q, "model_id"));
    int selected = id ? model_index(id) : -1;
    int target = free_slot();
    if (selected < 0) return RBP_STATUS_INVALID_ARGUMENT;
    if (target < 0) return RBP_STATUS_RESOURCE_LIMIT;
    if (operation.pending || connecting_slot >= 0 || standalone_busy()) return RBP_STATUS_BUSY;
    if (!stop_scan()) return RBP_STATUS_BUSY;
    slot_t *s = &slots[target];
    uint32_t generation = ((s->model.generation + 1) & (UINT32_MAX >> 2));
    if (!generation) generation = 1;
    void *token = (void *)(uintptr_t)((generation << 2) | target);
    struct ble_gap_conn_desc d;
    int rc = buddy_probe_take(gap, token, &d);
    if (rc) { cJSON_AddNumberToObject(j, "sdk_error", rc); return RBP_STATUS_BAD_STATE; }
    begin_operation(s, "pair");
    remember_pair_intent(s, &d.peer_id_addr, selected);
    prepare_link(s, &d.peer_id_addr, true, selected);
    s->conn = d.conn_handle;
    s3_gatt_attach(&s->gatt, &s->adapter, s->conn);
    /* Enter the SAME identity validation, adapter initialization and durable
     * commit as a new encrypted connection. No scan or second SMP exchange. */
    accept_encrypted(s, &d);
    cJSON_AddNumberToObject(j, "operation_id", operation.id);
    return RBP_STATUS_ACCEPTED;
  }
  if (op == BUDDY_PROBE_BEGIN) {
    if (bound_count() >= BUDDY_MAX_REMOTES || link_count() >= BUDDY_MAX_REMOTES)
      return RBP_STATUS_RESOURCE_LIMIT;
    if (!synced || connecting_slot >= 0 || operation.pending ||
        pairing_busy() || standalone_busy())
      return RBP_STATUS_BUSY;
    for(unsigned i=0;i<SLOTS;i++)
      if(!slots[i].record.peer_id && (slots[i].conn!=NONE || slots[i].identity_seen ||
                                     slots[i].forgetting)) return RBP_STATUS_BUSY;
    /* Do not open a session while a previous unbound link is still closing. */
    if (free_slot() < 0) {
      for (unsigned i = 0; i < SLOTS; i++)
        if (!slots[i].record.peer_id) return RBP_STATUS_BUSY;
      return RBP_STATUS_RESOURCE_LIMIT;
    }
    if (!stop_scan()) return RBP_STATUS_BUSY;
    uint16_t result = buddy_probe_command(op, q, j);
    if (!result) {
      manual_scan = false;
      memset(candidates, 0, sizeof candidates);
      scan_epoch++;
      next_scan = now_ms();
    }
    return result;
  }
  if (op == BUDDY_PROBE_CONNECT) {
    uint32_t id, epoch;
    if (!buddy_probe_active() || !buddy_u32(q, "candidate_id", &id) ||
        !buddy_u32(q, "scan_epoch", &epoch) || epoch != scan_epoch)
      return RBP_STATUS_BAD_STATE;
    if (free_slot() < 0 || link_count() >= BUDDY_MAX_REMOTES)
      return RBP_STATUS_RESOURCE_LIMIT;
    if (standalone_busy())
      return RBP_STATUS_BUSY;
    for (unsigned i = 0; i < CANDIDATES; i++)
      if (candidates[i].id == id && now_ms() - candidates[i].seen < 15000) {
        if (!candidates[i].connectable ||
            identity_slot(&candidates[i].address) >= 0)
          return RBP_STATUS_BAD_STATE;
        if (!stop_scan()) return RBP_STATUS_BUSY;
        int rc = buddy_probe_connect(own_address, &candidates[i].address);
        cJSON_AddNumberToObject(j, "sdk_error", rc);
        return rc ? RBP_STATUS_DEVICE_ERROR : 0;
      }
    return RBP_STATUS_NOT_FOUND;
  }
  if (op == BUDDY_PROBE_END && buddy_probe_active() && !stop_scan())
    return RBP_STATUS_BUSY;
  if (op >= BUDDY_PROBE_END && op <= BUDDY_PROBE_MTU)
    return buddy_probe_command(op, q, j);
  if (buddy_probe_active() && op != BUDDY_INFO && op != BUDDY_SLOT &&
      op != BUDDY_STATS && op != BUDDY_CANDIDATE && op != BUDDY_OPERATION &&
      op != BUDDY_CATALOG && op != BUDDY_MAP_GET &&
      !(op >= BUDDY_MODEL_GET && op <= BUDDY_MODEL_ABORT))
    return RBP_STATUS_BUSY;
  if (op >= BUDDY_MODEL_GET && op <= BUDDY_MODEL_DELETE)
    return models_command(op, q, j);
  uint32_t index;
  if (op == BUDDY_INFO) {
    buddy_update_info(j);
    cJSON_AddNumberToObject(j,"catalog_api",2);
    cJSON_AddNumberToObject(j, "probe_api", 1);
    cJSON_AddNumberToObject(j, "lifecycle_api", 2);
    cJSON_AddNumberToObject(j, "probe_voice_api", 4);
    cJSON_AddNumberToObject(j, "model_api", 1);
    cJSON_AddNumberToObject(j, "model_capacity", BUDDY_MODELS);
    cJSON_AddNumberToObject(j, "voice_presets", 1);
    cJSON_AddNumberToObject(j, "voice_toggle", 1);
    cJSON_AddNumberToObject(j, "host_os", s3_host_os());
    char firmware[48];
    snprintf(firmware, sizeof firmware, "buddy-%s",
             esp_app_get_description()->version);
    cJSON_AddStringToObject(j, "firmware", firmware);
    cJSON_AddNumberToObject(j, "slots", SLOTS);
    cJSON_AddNumberToObject(j, "max_remotes", BUDDY_MAX_REMOTES);
    cJSON_AddBoolToObject(j, "capacity_exceeded", bound_count() > BUDDY_MAX_REMOTES);
    cJSON_AddNumberToObject(j, "voice_owner", s3_voice_owner());
    cJSON_AddBoolToObject(j, "manual_pairing", buddy_management_active());
    cJSON_AddBoolToObject(j, "scanning", scanning);
    cJSON_AddNumberToObject(j, "scan_epoch", scan_epoch);
    cJSON_AddNumberToObject(j, "free_slot", free_slot());
    return 0;
  }
  if (op == BUDDY_OPERATION) {
    operation_json(j);
    return 0;
  }
  if (op == BUDDY_SCAN) {
    if (connecting_slot >= 0 || operation.pending || standalone_busy())
      return RBP_STATUS_BUSY;
    if (!buddy_u32(q, "duration_ms", &index) || index < 1000 || index > 30000)
      return RBP_STATUS_INVALID_ARGUMENT;
    if (!manual_scan) {
      memset(candidates, 0, sizeof candidates);
      scan_epoch++;
    }
    manual_scan = true;
    scan_until = now_ms() + index;
    if (!scanning) scan_start(false, index);
    cJSON_AddNumberToObject(j, "scan_epoch", scan_epoch);
    return scanning ? RBP_STATUS_OK : RBP_STATUS_DEVICE_ERROR;
  }
  if (op == BUDDY_CANDIDATE) {
    bool cursor = cJSON_GetObjectItemCaseSensitive(q, "cursor") != NULL;
    if (!buddy_u32(q, cursor ? "cursor" : "index", &index) || index >= CANDIDATES)
      return RBP_STATUS_INVALID_ARGUMENT;
    if (cursor) {
      while (index < CANDIDATES && (!candidates[index].id || now_ms() - candidates[index].seen > 15000)) index++;
      cJSON_AddNumberToObject(j, "next", index < CANDIDATES ? index + 1 : CANDIDATES);
      if (index == CANDIDATES) return 0;
    }
    if (!candidates[index].id || now_ms() - candidates[index].seen > 15000)
      return RBP_STATUS_NOT_FOUND;
    if (buddy_probe_active()) {
      char address[18], raw[63];
      snprintf(
          address, sizeof address, "%02x:%02x:%02x:%02x:%02x:%02x",
          candidates[index].address.val[5], candidates[index].address.val[4],
          candidates[index].address.val[3], candidates[index].address.val[2],
          candidates[index].address.val[1], candidates[index].address.val[0]);
      cJSON_AddStringToObject(j, "address", address);
      cJSON_AddNumberToObject(j, "address_type",
                              candidates[index].address.type);
      if (!cursor) {
      for (unsigned n = 0; n < candidates[index].adv_len; n++)
        snprintf(raw + 2 * n, 3, "%02x", candidates[index].adv[n]);
      raw[2 * candidates[index].adv_len] = 0;
      cJSON_AddStringToObject(j, "adv", raw);
      for (unsigned n = 0; n < candidates[index].response_len; n++)
        snprintf(raw + 2 * n, 3, "%02x", candidates[index].response[n]);
      raw[2 * candidates[index].response_len] = 0;
      cJSON_AddStringToObject(j, "response", raw);
      }
    }
    cJSON_AddBoolToObject(j, "connectable", candidates[index].connectable);
    cJSON_AddNumberToObject(j, "candidate_id", candidates[index].id);
    cJSON_AddNumberToObject(j, "scan_epoch", scan_epoch);
    cJSON_AddStringToObject(j, "name", candidates[index].name);
    cJSON_AddNumberToObject(j, "rssi", candidates[index].rssi);
    cJSON_AddBoolToObject(j, "known", candidates[index].known);
    int mi = model_match_name(candidates[index].name, candidates[index].company, false);
    if (!buddy_probe_active()) {
      cJSON_AddBoolToObject(j, "ambiguous", mi == -2);
      cJSON_AddStringToObject(j, "model", mi >= 0 && model_at(mi) ? model_at(mi)->id : "");
    }
    cJSON_AddNumberToObject(j, "company", candidates[index].company);
    cJSON_AddNumberToObject(j, "bound_slot",
                            identity_slot(&candidates[index].address));
    cJSON_AddNumberToObject(j, "age_ms", now_ms() - candidates[index].seen);
    return 0;
  }
  if (op == BUDDY_PAIR) {
    uint32_t epoch;
    if (!buddy_u32(q, "candidate_id", &index) ||
        !buddy_u32(q, "scan_epoch", &epoch) || epoch != scan_epoch)
      return RBP_STATUS_INVALID_ARGUMENT;
    if (operation.pending || connecting_slot >= 0 || pairing_busy() ||
        standalone_busy())
      return RBP_STATUS_BUSY;
    for (unsigned i = 0; i < CANDIDATES; i++)
      if (candidates[i].id == index && now_ms() - candidates[i].seen < 15000) {
        if(!candidates[i].connectable)return RBP_STATUS_BAD_STATE;
        int requested=-1;
        const cJSON *selected=cJSON_GetObjectItemCaseSensitive(q,"model_id");
        if(selected){if(!cJSON_IsString(selected)||(requested=model_index(selected->valuestring))<0)return RBP_STATUS_INVALID_ARGUMENT;}
        int target = pairing_slot(&candidates[i].address);
        if (target < 0)
          return RBP_STATUS_RESOURCE_LIMIT;
        slot_t *s = &slots[target];
        if (s->conn != NONE)
          return RBP_STATUS_BAD_STATE;
        begin_operation(s, "pair");
        manual_scan = false;
        int rc = connect_model(s, &candidates[i].address, true,requested);
        cJSON_AddNumberToObject(j, "operation_id", operation.id);
        if (rc && operation.pending)
          complete(s, RBP_STATUS_DEVICE_ERROR, false);
        return rc ? RBP_STATUS_DEVICE_ERROR : RBP_STATUS_ACCEPTED;
      }
    return RBP_STATUS_NOT_FOUND;
  }
  if (op == BUDDY_RETRY) {
    if (!buddy_u32(q, "operation_id", &index) || index != operation.id ||
        strcmp(operation.kind, "pair")) return RBP_STATUS_NOT_FOUND;
    if (operation.pending || connecting_slot >= 0 || pairing_busy() || standalone_busy())
      return RBP_STATUS_BUSY;
    if (!operation.result) return RBP_STATUS_BAD_STATE;
    ble_addr_t target = operation.target;
    int requested = operation.model_id[0] ? model_index(operation.model_id) : -1;
    if (operation.model_id[0] && requested < 0) return RBP_STATUS_NOT_FOUND;
    int slot = pairing_slot(&target);
    if (slot < 0) return RBP_STATUS_RESOURCE_LIMIT;
    slot_t *s = &slots[slot];
    if (s->conn != NONE || s->forgetting) return RBP_STATUS_BAD_STATE;
    begin_operation(s, "pair");
    int rc = connect_model(s, &target, true, requested);
    if (rc && operation.pending) complete(s, RBP_STATUS_DEVICE_ERROR, false);
    cJSON_AddNumberToObject(j, "operation_id", operation.id);
    return rc ? RBP_STATUS_DEVICE_ERROR : RBP_STATUS_ACCEPTED;
  }
  if (op == BUDDY_SCAN_STOP) {
    if (!stop_scan()) return RBP_STATUS_BUSY;
    manual_scan = false;
    return 0;
  }
  if (op == BUDDY_CANCEL) {
    if (!buddy_u32(q, "operation_id", &index) || !operation.pending ||
        index != operation.id)
      return RBP_STATUS_NOT_FOUND;
    slot_t *target = &slots[operation.slot];
    if (!target->pairing)
      return RBP_STATUS_BAD_STATE;
    fail_pair(target, RBP_STATUS_CANCELLED);
    return 0;
  }
  if (op == BUDDY_STATS) {
    if (!buddy_u32(q, "index", &index))
      return RBP_STATUS_INVALID_ARGUMENT;
    /* Enumeration evidence and the requested Globe level, without USB I/O.
     * The desired report is a separate runtime snapshot, not an OS key ack. */
    if (index == 80) { /* 16..79 are reserved for S3_HCI_PROBE link traces. */
      buddy_host_probe_t probe;
      buddy_host_os_t host = s3_host_probe_snapshot(&probe);
      uint8_t desired[8];
      s3_hid_desired(desired);
      cJSON_AddNumberToObject(j, "host_os", host);
      cJSON_AddNumberToObject(j, "strings", probe.strings);
      cJSON_AddNumberToObject(j, "short2", probe.short2);
      cJSON_AddNumberToObject(j, "short4", probe.short4);
      cJSON_AddNumberToObject(j, "full255", probe.full255);
      cJSON_AddBoolToObject(j, "frozen", probe.frozen);
      cJSON_AddBoolToObject(j, "globe_requested", (desired[1] & 1) != 0);
      return 0;
    }
#ifdef S3_CODEC_METRICS
    if (index == 15) {
      uint32_t m[STANDALONE_CODEC_METRICS_WORDS];
      standalone_codec_metrics(m);
      const char *names[STANDALONE_CODEC_METRICS_WORDS] = {
          "version", "ima_calls", "ima_total_us", "ima_max_us",
          "ico_calls", "ico_total_us", "ico_max_us",
          "msbc_calls", "msbc_total_us", "msbc_max_us",
          "stack_min_bytes", "queue_high_water_samples"};
      for (unsigned i = 0; i < STANDALONE_CODEC_METRICS_WORDS; i++)
        cJSON_AddNumberToObject(j, names[i], m[i]);
      return 0;
    }
#endif
    /* Read-only identity evidence from the production connection. Never starts
     * discovery, changes a bond, or races another GATT transaction. */
    if (index == 14) {
      uint32_t slot, peer, generation, offset;
      if (!buddy_u32(q,"slot",&slot) || slot>=SLOTS ||
          !buddy_u32(q,"peer_id",&peer) || !peer ||
          !buddy_u32(q,"generation",&generation) ||
          !buddy_u32(q,"offset",&offset)) return RBP_STATUS_INVALID_ARGUMENT;
      slot_t *s=&slots[slot];
      if (s->record.peer_id!=peer || s->model.generation!=generation ||
          !s->adapter.ready || !s->adapter.map_len) return RBP_STATUS_BAD_STATE;
      if (offset>=s->adapter.map_len) return RBP_STATUS_INVALID_ARGUMENT;
      unsigned count=s->adapter.map_len-offset;
      if(count>64)count=64;
      char hex[129];
      for(unsigned i=0;i<count;i++)snprintf(hex+2*i,3,"%02x",s->adapter.map_buf[offset+i]);
      cJSON_AddNumberToObject(j,"length",s->adapter.map_len);
      cJSON_AddNumberToObject(j,"offset",offset);
      cJSON_AddStringToObject(j,"hex",hex);
      cJSON_AddNumberToObject(j,"crc32c",rbp_crc32c(s->adapter.map_buf,s->adapter.map_len));
      cJSON_AddNumberToObject(j,"report_count",s->adapter.char_count);
      cJSON_AddBoolToObject(j,"atvv",s->adapter.atvv_present);
      cJSON_AddNumberToObject(j,"atvv_tx",s->adapter.atvv_tx_handle);
      cJSON_AddNumberToObject(j,"atvv_audio",s->adapter.atvv_audio_handle);
      cJSON_AddNumberToObject(j,"atvv_control",s->adapter.atvv_ctl_handle);
      uint32_t report;
      if(buddy_u32(q,"report_index",&report)) {
        if(report>=s->adapter.char_count)return RBP_STATUS_INVALID_ARGUMENT;
        rc003_report_char_t *c=&s->adapter.chars[report];
        cJSON_AddNumberToObject(j,"report_id",c->report_id);
        cJSON_AddNumberToObject(j,"report_type",c->report_type);
        cJSON_AddNumberToObject(j,"handle",c->value_handle);
        cJSON_AddNumberToObject(j,"properties",c->properties);
      }
      return 0;
    }
    if (index == 13) {
      cJSON_AddNumberToObject(j,"total",scan_diag.total);
      cJSON_AddNumberToObject(j,"zero",scan_diag.zero);
      cJSON_AddNumberToObject(j,"weak",scan_diag.weak);
      cJSON_AddNumberToObject(j,"malformed",scan_diag.malformed);
      for(unsigned i=0;i<SLOTS;i++) {
        char key[16];snprintf(key,sizeof key,"slot%u",i);
        cJSON_AddNumberToObject(j,key,scan_diag.bound[i]);
      }
      char address[18];
      snprintf(address,sizeof address,"%02x:%02x:%02x:%02x:%02x:%02x",
        scan_diag.last_address.val[5],scan_diag.last_address.val[4],scan_diag.last_address.val[3],
        scan_diag.last_address.val[2],scan_diag.last_address.val[1],scan_diag.last_address.val[0]);
      cJSON_AddStringToObject(j,"last_address",address);
      cJSON_AddNumberToObject(j,"last_type",scan_diag.last_address.type);
      cJSON_AddNumberToObject(j,"last_ms",scan_diag.last_ms);
      cJSON_AddNumberToObject(j,"last_rssi",scan_diag.last_rssi);
      cJSON_AddNumberToObject(j,"last_event",scan_diag.last_event);
      return 0;
    }
    if (index == 12) {
      uint32_t v[5];
      hci_probe_status(v);
      const char *names[5] = {"count", "frozen", "acl", "continuations",
                              "malformed"};
      for (unsigned i = 0; i < 5; i++)
        cJSON_AddNumberToObject(j, names[i], v[i]);
      return 0;
    }
    if (index >= 128 && index < 1152) {
      hci_probe_entry_t e;
      if (!hci_probe_get(index - 128, &e))
        return RBP_STATUS_NOT_FOUND;
      cJSON_AddNumberToObject(j, "ordinal", e.ordinal);
      cJSON_AddNumberToObject(j, "time_us", e.time_us);
      cJSON_AddNumberToObject(j, "layer", e.layer);
      cJSON_AddNumberToObject(j, "connection", e.connection);
      cJSON_AddNumberToObject(j, "attribute", e.attribute);
      cJSON_AddNumberToObject(j, "sequence", e.sequence);
      cJSON_AddNumberToObject(j, "part", e.part);
      cJSON_AddNumberToObject(j, "length", e.length);
      cJSON_AddNumberToObject(j, "hash", e.hash);
      return 0;
    }
    if (index >= 7 && index < 11) {
      slot_t *s = &slots[index - 7];
      struct ble_gap_conn_desc d;
      cJSON_AddNumberToObject(j, "slot", s->model.slot);
      cJSON_AddNumberToObject(j, "generation", s->model.generation);
      cJSON_AddNumberToObject(j, "ready_after_ms", s->ready_after_ms);
      cJSON_AddNumberToObject(j, "disconnected_ms", s->disconnected_ms);
      cJSON_AddNumberToObject(j, "disconnect_reason", s->disconnect_reason);
      cJSON_AddBoolToObject(j, "cache_valid", s->adapter.cache_valid);
#ifdef S3_HCI_PROBE
      cJSON_AddNumberToObject(j,"observed_ms",now_ms());
      cJSON_AddBoolToObject(j,"reconnect_held",diagnostic_held(s->model.slot,now_ms()));
      cJSON_AddNumberToObject(j,"notify_bytes",s->notify_bytes);
      cJSON_AddNumberToObject(j,"notify_count",s->notify_count);
      if(s->requested_valid) {
        cJSON *r=cJSON_AddObjectToObject(j,"requested");
        cJSON_AddNumberToObject(r,"min_interval",s->requested_params.itvl_min);
        cJSON_AddNumberToObject(r,"max_interval",s->requested_params.itvl_max);
        cJSON_AddNumberToObject(r,"latency",s->requested_params.latency);
        cJSON_AddNumberToObject(r,"timeout",s->requested_params.supervision_timeout);
        cJSON_AddNumberToObject(r,"min_ce_len",s->requested_params.min_ce_len);
        cJSON_AddNumberToObject(r,"max_ce_len",s->requested_params.max_ce_len);
        cJSON_AddNumberToObject(r,"time_ms",s->request_ms);
        cJSON_AddNumberToObject(r,"submit_result",s->request_result);
      }
#endif
      if (s->conn == NONE)
        return 0;
      int rc = ble_gap_conn_find(s->conn, &d);
      if (rc) {
        fault(s->model.slot, 31, rc);
        return RBP_STATUS_DEVICE_ERROR;
      }
      cJSON_AddNumberToObject(j, "connection", s->conn);
      cJSON_AddNumberToObject(j, "interval", d.conn_itvl);
      cJSON_AddNumberToObject(j, "latency", d.conn_latency);
      cJSON_AddNumberToObject(j, "timeout", d.supervision_timeout);
      if (!s->adapter.unicom.selected && s->adapter.atvv.caps_valid)
        cJSON_AddNumberToObject(j, "atvv_interaction",
                                s->adapter.atvv.caps.interaction);
      return 0;
    }
#ifdef S3_HCI_PROBE
    if (index >= 16 && index < 80) {
      uint32_t offset = index - 16;
      if (offset >= link_sequence || offset >= 64)
        return RBP_STATUS_NOT_FOUND;
      link_trace_t *v = &link_trace[(link_sequence - 1 - offset) % 64];
      cJSON_AddNumberToObject(j, "sequence", v->sequence);
      cJSON_AddNumberToObject(j, "time_ms", v->time);
      cJSON_AddNumberToObject(j, "slot", v->slot);
      cJSON_AddNumberToObject(j, "event", v->event);
      cJSON_AddNumberToObject(j, "a", v->a);
      cJSON_AddNumberToObject(j, "b", v->b);
      cJSON_AddNumberToObject(j, "c", v->c);
      return 0;
    }
#endif
    if (!index) {
      cJSON_AddNumberToObject(j, "heap_free",
                              heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
      cJSON_AddNumberToObject(
          j, "heap_min", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
      cJSON_AddNumberToObject(
          j, "heap_largest",
          heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
      cJSON_AddNumberToObject(j, "psram_free",
                              heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
      return 0;
    }
    if (index == 5 || index == 6) {
      uint8_t words[STANDALONE_STATS_BYTES];
      unsigned bytes =
          index == 5 ? STANDALONE_STATS_BYTES : STANDALONE_USB_STATS_BYTES;
      if (index == 5)
        standalone_stats(words);
      else
        standalone_usb_stats(words);
      cJSON *array = cJSON_AddArrayToObject(j, "words");
      for (unsigned k = 0; k < bytes; k += 4) {
        uint32_t v = words[k] | ((uint32_t)words[k + 1] << 8) |
                     ((uint32_t)words[k + 2] << 16) |
                     ((uint32_t)words[k + 3] << 24);
        cJSON_AddItemToArray(array, cJSON_CreateNumber(v));
      }
      return 0;
    }
    rbp_fault_t f[RBP_FAULT_CAPACITY];
    uint32_t seq, evicted;
    unsigned n = rbp_fault_snapshot(f, &seq, &evicted);
    if (index > n)
      return RBP_STATUS_NOT_FOUND;
    rbp_fault_t *v = &f[index - 1];
    cJSON_AddNumberToObject(j, "sequence", v->sequence);
    cJSON_AddNumberToObject(j, "domain", v->domain);
    cJSON_AddNumberToObject(j, "stage", v->stage);
    cJSON_AddNumberToObject(j, "code", v->code);
    cJSON_AddNumberToObject(j, "context", v->context);
    cJSON_AddNumberToObject(j, "count", v->count);
    cJSON_AddNumberToObject(j, "time_ms", v->board_ms);
    cJSON_AddNumberToObject(j, "evicted", evicted);
    return 0;
  }
  if (op != BUDDY_SLOT && op != BUDDY_UNBIND && op != BUDDY_CATALOG &&
      op != BUDDY_MAP_GET && op != BUDDY_MAP_SET && op != BUDDY_MAP_RESET)
    return RBP_STATUS_UNSUPPORTED;
  slot_t *s = request_slot(q, op != BUDDY_SLOT);
  if (!s)
    return RBP_STATUS_INVALID_ARGUMENT;
  if (op == BUDDY_SLOT) {
    cJSON_AddNumberToObject(j, "slot", s->model.slot);
    cJSON_AddNumberToObject(j, "peer_id", s->record.peer_id);
    cJSON_AddNumberToObject(j, "generation", s->model.generation);
    cJSON_AddNumberToObject(j, "state", s->model.state);
    cJSON_AddStringToObject(j, "name", s->record.name);
    const buddy_model_t *variant = slot_model(s);
    cJSON_AddStringToObject(j, "model",
                            variant ? variant->id : s->model.profile->model_id);
    char keys_hex[17];
    snprintf(keys_hex, sizeof keys_hex, "%016llx",
             (unsigned long long)s->model.keys);
    cJSON_AddStringToObject(j, "keys_hex", keys_hex);
    cJSON_AddNumberToObject(j, "battery", s->model.battery);
    cJSON_AddNumberToObject(j, "voice_state", s->model.voice_state);
    cJSON_AddBoolToObject(j, "voice_down", s->model.voice_down);
    cJSON_AddBoolToObject(j, "voice_rejected", s3_peer_rejected(s->model.slot));
    cJSON_AddNumberToObject(j, "map_revision", s->record.map.revision);
    cJSON_AddStringToObject(j, "error", s->model.error);
    return 0;
  }
  if (op == BUDDY_UNBIND) {
    if (operation.pending || s->pairing)
      return RBP_STATUS_BUSY;
    begin_operation(s, "unbind");
    s->forgetting = true;
    s->retry_at = now_ms();
    s->deadline = now_ms() + 10000;
    s3_peer_link(s->model.slot, 0);
    if (s->conn != NONE || s->connecting)
      terminate(s);
    else
      finish_forget(s);
    cJSON_AddNumberToObject(j, "operation_id", operation.id);
    return RBP_STATUS_ACCEPTED;
  }
  if (op == BUDDY_CATALOG) {
    const buddy_model_t *variant = slot_model(s);
    unsigned count = variant ? variant->key_count : s->model.profile->key_count;
    if (!buddy_u32(q, "index", &index) || index >= count)
      return RBP_STATUS_NOT_FOUND;
    unsigned key =
        variant ? variant->keys[index] : s->model.profile->keys[index].key_id;
    const char *name = "Key";
    for (unsigned k = 0; k < s->model.profile->key_count; k++)
      if (s->model.profile->keys[k].key_id == key)
        name = s->model.profile->keys[k].name;
    buddy_map_t defaults;
    if (variant)
      defaults = variant->defaults;
    else
      buddy_map_default(&defaults);
    binding_json(j, key, defaults.key[key]);
    cJSON_AddStringToObject(j, "name", name);
    cJSON_AddBoolToObject(j,"snapshot_name",variant && variant->labels[key][0]);
    cJSON_AddNullToObject(j, "layout");
    cJSON_AddStringToObject(j, "model",
                            variant ? variant->id : s->model.profile->model_id);
    cJSON_AddNumberToObject(j, "count", count);
    return 0;
  }
  if (op == BUDDY_MAP_GET) {
    if (!buddy_u32(q, "key", &index) || !index || index >= BUDDY_KEYS)
      return RBP_STATUS_INVALID_ARGUMENT;
    binding_json(j, index, s->record.map.key[index]);
    cJSON_AddNumberToObject(j, "revision", s->record.map.revision);
    cJSON_AddBoolToObject(j, "overridden", (s->record.overrides & (UINT64_C(1) << index)) != 0);
    const buddy_model_t *definition=slot_model(s);
    if(definition){cJSON *d=cJSON_AddObjectToObject(j,"default");if(d)binding_json(d,index,definition->defaults.key[index]);}
    return 0;
  }
  if (op == BUDDY_MAP_SET || op == BUDDY_MAP_RESET) {
    uint32_t revision;
    if (!buddy_u32(q, "revision", &revision))
      return RBP_STATUS_INVALID_ARGUMENT;
    if (revision != s->record.map.revision)
      return RBP_STATUS_BAD_STATE;
    if (standalone_busy() || s->pairing || s->forgetting)
      return RBP_STATUS_BUSY;
    record_t *record = &transaction_record; *record = s->record;
    if (op == BUDDY_MAP_RESET) {
      const buddy_model_t *variant = slot_model(s);
      if (cJSON_GetObjectItemCaseSensitive(q, "key")) {
        if (!buddy_u32(q, "key", &index) || !variant || !buddy_model_has_key(variant, index))
          return RBP_STATUS_INVALID_ARGUMENT;
        record->map.key[index] = variant->defaults.key[index];
        record->overrides &= ~(UINT64_C(1) << index);
      } else if (variant) {
        record->map = variant->defaults;
        record->overrides = 0;
      } else {
        buddy_map_default(&record->map);
        record->overrides = 0;
      }
    } else {
      uint32_t kind, mod, value;
      if (!buddy_u32(q, "key", &index) || !buddy_u32(q, "kind", &kind) ||
          !buddy_u32(q, "modifiers", &mod) || !buddy_u32(q, "value", &value) ||
          kind > 255 || mod > 255 || value > 65535)
        return RBP_STATUS_INVALID_ARGUMENT;
      buddy_binding_t b = {kind, mod, value};
      if (!buddy_binding_valid(index, b))
        return RBP_STATUS_INVALID_ARGUMENT;
      bool found = false;
      for (unsigned k = 0; k < s->model.profile->key_count; k++)
        if (s->model.profile->keys[k].key_id == index)
          found = true;
      if (slot_model(s) && !buddy_model_has_key(slot_model(s), index))
        found = false;
      if (!found)
        return RBP_STATUS_NOT_FOUND;
      record->map.key[index] = b;
      record->overrides |= UINT64_C(1) << index;
    }
    record->map.revision = s->record.map.revision + 1;
    if (!record->map.revision)
      record->map.revision = 1;
    if (!persist(s->model.slot, record))
      return RBP_STATUS_STORAGE_FAILED;
    s->record = *record;
    s3_mapping(s->model.slot, &record->map);
    cJSON_AddNumberToObject(j, "revision", record->map.revision);
    changed(s->model.slot);
    return 0;
  }
  return RBP_STATUS_UNSUPPORTED;
}

#ifdef S3_HCI_PROBE
int __real_ble_gap_update_params(uint16_t connection, const struct ble_gap_upd_params *params);
int __wrap_ble_gap_update_params(uint16_t connection, const struct ble_gap_upd_params *params) {
  int rc=__real_ble_gap_update_params(connection,params);
  for(unsigned i=0;i<SLOTS;i++)if(slots[i].conn==connection) {
    slots[i].request_result=(uint32_t)rc;
    s3_link_trace(i,8,(uint32_t)rc,
      params->itvl_min|((uint32_t)params->itvl_max<<16),
      params->latency|((uint32_t)params->supervision_timeout<<16));
    if(rc)fault(i,31,rc);

    break;
  }
  return rc;
}
#endif
