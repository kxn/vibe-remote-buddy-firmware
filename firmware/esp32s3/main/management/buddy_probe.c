#include "buddy_link.h"
/* Single BLE-host-task owner. Voice uses the isolated production adapter sink.
 */
#include "buddy_probe.h"
#include "esp_timer.h"
#include "faults.h"
#include "host/ble_gatt.h"
#include "host/util/util.h"
#include <stdio.h>
#include <string.h>
#define ATTRS 192
#define VALUES 8192
#define REPORTS 128

#define PNONE BLE_HS_CONN_HANDLE_NONE
typedef struct {
  uint16_t handle, end, parent, def, offset, length;
  uint8_t kind, properties;
  char uuid[37];
  bool complete;
} probe_attr_t;
typedef struct {
  uint32_t seq, time;
  uint16_t handle, length;
  #ifdef S3_HCI_PROBE
  uint8_t data[128];
#else
  uint8_t data[32];
#endif
} probe_report_t;
static struct {
  bool active, closing, connecting, pending, encrypted, voice;
  uint16_t conn;
  uint32_t generation, deadline, sequence, close_retry, link_generation;
  int error, cleanup_error, disconnect_reason;
  uint16_t error_handle;
  char error_phase[20];
  char phase[20];
  ble_addr_t bonds[8];
  int bond_count;
  probe_attr_t attrs[ATTRS];
  unsigned count, cursor, reading;
  uint8_t values[VALUES];
  unsigned used;
  probe_report_t reports[REPORTS];
} p = {.conn = PNONE};
static uint32_t pn(void) { return esp_timer_get_time() / 1000; }

static void phase(const char *s) { snprintf(p.phase, sizeof p.phase, "%s", s); }
static void err(int e) {
  p.error = e;
  if (e) {
    snprintf(p.error_phase, sizeof p.error_phase, "%s", p.phase);
    p.error_handle = !strcmp(p.phase, "reading") && p.reading < p.count
                         ? p.attrs[p.reading].handle
                         : 0;
  } else {
    p.error_phase[0] = 0;
    p.error_handle = 0;
  }
  if (e)
    rbp_fault_record(RBP_FAULT_SDK, 0x1180, e, p.conn);
}
static void *token(void) {
  p.pending = true;
  p.deadline = pn() + 15000;
  return (void *)(uintptr_t)++p.generation;
}
static bool current(uint16_t c, void *arg) {
  return p.active && !p.closing && p.pending && p.conn == c &&
         (uintptr_t)arg == p.generation;
}
static int submitted(int rc) {
  if (rc) {
    p.pending = false;
    err(rc);
    phase("connected");
  }
  return rc;
}
static void done(int rc) {
  p.pending = false;
  err(rc);
  phase("connected");
}
static probe_attr_t *add(unsigned kind, unsigned h, const ble_uuid_t *u) {
  if (p.count == ATTRS) {
    err(BLE_HS_ENOMEM);
    return NULL;
  }
  probe_attr_t *a = &p.attrs[p.count++];
  memset(a, 0, sizeof *a);
  a->kind = kind;
  a->handle = h;
  ble_uuid_to_str(u, a->uuid);
  return a;
}
static bool uuid(const probe_attr_t *a, unsigned u) {
  char s[8];
  snprintf(s, sizeof s, "0x%04x", u);
  return !strcmp(a->uuid, s) ||
         (strlen(a->uuid) == 4 && strtoul(a->uuid, NULL, 16) == u);
}
static int desc_cb(uint16_t, const struct ble_gatt_error *, uint16_t,
                   const struct ble_gatt_dsc *, void *);
static void next_desc(void) {
  while (p.cursor < p.count && p.attrs[p.cursor].kind != 2)
    p.cursor++;
  if (p.cursor == p.count) {
    done(p.error);
    return;
  }
  probe_attr_t *a = &p.attrs[p.cursor];
  unsigned end = 65535;
  for (unsigned i = 0; i < p.count; i++) {
    probe_attr_t *b = &p.attrs[i];
    if (b->kind == 1 && b->handle <= a->handle && b->end >= a->handle &&
        b->end < end)
      end = b->end;
    if (b->kind == 2 && b->def > a->handle && b->def - 1 < end)
      end = b->def - 1;
  }
  if (end <= a->handle) {
    p.cursor++;
    next_desc();
    return;
  }
  phase("descriptors");
  submitted(ble_gattc_disc_all_dscs(p.conn, a->handle, end, desc_cb, token()));
}
static int desc_cb(uint16_t c, const struct ble_gatt_error *e, uint16_t ch,
                   const struct ble_gatt_dsc *d, void *arg) {
  (void)ch;
  if (!current(c, arg))
    return 0;
  if (!e->status && d) {
    probe_attr_t *a = add(3, d->handle, &d->uuid.u);
    if (!a) {
      done(BLE_HS_ENOMEM);
      return BLE_HS_ENOMEM;
    }
    a->parent = p.attrs[p.cursor].handle;
  } else if (e->status == BLE_HS_EDONE) {
    p.cursor++;
    next_desc();
  } else
    done(e->status);
  return 0;
}
static int chr_cb(uint16_t c, const struct ble_gatt_error *e,
                  const struct ble_gatt_chr *d, void *arg) {
  if (!current(c, arg))
    return 0;
  if (!e->status && d) {
    probe_attr_t *a = add(2, d->val_handle, &d->uuid.u);
    if (!a) {
      done(BLE_HS_ENOMEM);
      return BLE_HS_ENOMEM;
    }
    a->def = d->def_handle;
    a->properties = d->properties;
  } else if (e->status == BLE_HS_EDONE) {
    p.cursor = 0;
    next_desc();
  } else
    done(e->status);
  return 0;
}
static int svc_cb(uint16_t c, const struct ble_gatt_error *e,
                  const struct ble_gatt_svc *d, void *arg) {
  if (!current(c, arg))
    return 0;
  if (!e->status && d) {
    probe_attr_t *a = add(1, d->start_handle, &d->uuid.u);
    if (!a) {
      done(BLE_HS_ENOMEM);
      return BLE_HS_ENOMEM;
    }
    a->end = d->end_handle;
  } else if (e->status == BLE_HS_EDONE) {
    phase("characteristics");
    submitted(ble_gattc_disc_all_chrs(c, 1, 65535, chr_cb, token()));
  } else
    done(e->status);
  return 0;
}
static int read_cb(uint16_t c, const struct ble_gatt_error *e,
                   struct ble_gatt_attr *d, void *arg) {
  if (!current(c, arg))
    return 0;
  probe_attr_t *a = &p.attrs[p.reading];
  if (e->status == BLE_HS_EDONE) {
    a->complete = true;
    done(0);
    return 0;
  }
  if (e->status) {
    done(e->status);
    return 0;
  }
  unsigned n = OS_MBUF_PKTLEN(d->om);
  if (n > VALUES - p.used || a->length + n > 2048) {
    done(BLE_HS_ENOMEM);
    return BLE_HS_ENOMEM;
  }
  if (os_mbuf_copydata(d->om, 0, n, p.values + p.used)) {
    done(BLE_HS_EBADDATA);
    return BLE_HS_EBADDATA;
  }
  p.used += n;
  a->length += n;
  p.deadline = pn() + 15000;
  return 0;
}
static int write_cb(uint16_t c, const struct ble_gatt_error *e,
                    struct ble_gatt_attr *d, void *arg) {
  (void)d;
  if (current(c, arg))
    done(e->status);
  return 0;
}
#ifdef S3_HCI_PROBE
static int probe_mtu_cb(uint16_t c, const struct ble_gatt_error *e,
                        uint16_t mtu, void *arg) {
  (void)mtu;
  if (current(c,arg)) done(e->status);
  return 0;
}
#endif
static void cleanup(void) {
  ble_addr_t peers[8];
  int n = 0;
  int rc = ble_store_util_bonded_peers(peers, &n, 8);
  if (!rc)
    for (int i = 0; i < n; i++) {
      bool old = false;
      for (int j = 0; j < p.bond_count; j++)
        if (buddy_address_equal(&peers[i], &p.bonds[j]))
          old = true;
      if (!old && !buddy_identity_is_bound(&peers[i])) {
        rc = buddy_bond_remove_ram(&peers[i]);
        if (rc == BLE_HS_ENOENT) rc = 0;
        if (rc)
          break;
      }
    }
  p.cleanup_error = rc;
  if (rc) {
    err(rc);
    return;
  }
  p.active = p.closing = false;
  phase("idle");
}
static void stop(void) {
  if (!p.active)
    return;
  buddy_probe_voice_detach();
  p.voice = false;
  p.closing = true;
  p.close_retry = pn() + 1000;
  phase("closing");
  p.generation++;
  p.pending = false;
  int rc = buddy_link_close(p.connecting, p.conn);
  if (!p.connecting && rc == BLE_HS_ENOTCONN) {
    p.conn = PNONE;
    p.encrypted = false;
    cleanup();
    return;
  }
  if (rc && rc != BLE_HS_EALREADY)
    err(rc);
}
static int gap_cb(struct ble_gap_event *e, void *arg) {
  if (!p.active || (uintptr_t)arg != p.link_generation)
    return 0;
  if (e->type == BLE_GAP_EVENT_CONNECT) {
    p.connecting = false;
    if (e->connect.status) {
      err(e->connect.status);
      phase("scanning");
      if (p.closing)
        cleanup();
      return 0;
    }
    p.conn = e->connect.conn_handle;
    if (p.closing) {
      stop();
      return 0;
    }
    struct ble_gap_conn_desc d;
    if (!ble_gap_conn_find(p.conn, &d))
        if (buddy_identity_is_bound(&d.peer_id_addr)) {
          err(BLE_HS_EALREADY);
          stop();
          return 0;
        }
    phase("connected");
  } else if (e->type == BLE_GAP_EVENT_DISCONNECT) {
    buddy_probe_voice_detach();
    p.voice = false;
    p.disconnect_reason = e->disconnect.reason;
    p.conn = PNONE;
    p.pending = false;
    p.encrypted = false;
    p.generation++;
    if (!p.error)
      err(e->disconnect.reason);
    if (p.closing)
      cleanup();
    else
      phase("disconnected");
  } else if (e->type == BLE_GAP_EVENT_ENC_CHANGE) {
    bool requested = !strcmp(p.phase, "pairing");
    if (requested)
      p.pending = false;
    if (requested || e->enc_change.status)
      err(e->enc_change.status);
    struct ble_gap_conn_desc d;
    if (!ble_gap_conn_find(p.conn, &d)) {
      p.encrypted = d.sec_state.encrypted;
        if (buddy_identity_is_bound(&d.peer_id_addr)) {
          err(BLE_HS_EALREADY);
          stop();
          return 0;
        }
    }
    if (requested)
      phase("connected");
  } else if (e->type == BLE_GAP_EVENT_REPEAT_PAIRING)
    return BLE_GAP_REPEAT_PAIRING_IGNORE;
  else if (e->type == BLE_GAP_EVENT_PASSKEY_ACTION) {
    err(BLE_HS_ENOTSUP);
    stop();
  } else if (e->type == BLE_GAP_EVENT_NOTIFY_RX && !p.closing) {
    if (p.voice) {
      uint8_t data[512];
      unsigned n = OS_MBUF_PKTLEN(e->notify_rx.om);
      if (n > sizeof data || os_mbuf_copydata(e->notify_rx.om, 0, n, data)) {
        err(BLE_HS_EBADDATA);
        return 0;
      }
      buddy_probe_voice_notify(e->notify_rx.attr_handle, data, n);
    }
    bool key = false;
    for (unsigned i = 0; i < p.count; i++)
      if (p.attrs[i].kind == 3 &&
          p.attrs[i].parent == e->notify_rx.attr_handle &&
          uuid(&p.attrs[i], 0x2908) && p.attrs[i].complete &&
          p.attrs[i].length == 2) {
        const uint8_t *v = p.values + p.attrs[i].offset;
        key = v[1] == 1 && (v[0] == 1 || v[0] == 3 || v[0] == 248);
#ifdef S3_HCI_PROBE
        if(p.encrypted && (v[1]==1 || v[1]==3)) key=true;
#endif
      }

    if (!key)
      return 0;
    probe_report_t *r = &p.reports[p.sequence % REPORTS];
    memset(r, 0, sizeof *r);
    r->seq = ++p.sequence;
    r->time = pn();
    r->handle = e->notify_rx.attr_handle;
    r->length = OS_MBUF_PKTLEN(e->notify_rx.om);
    unsigned n = r->length < sizeof r->data ? r->length : sizeof r->data;
    if (os_mbuf_copydata(e->notify_rx.om, 0, n, r->data))
      err(BLE_HS_EBADDATA);
  }
  return 0;
}
int buddy_probe_take(int (*callback)(struct ble_gap_event *, void *), void *arg,
                     struct ble_gap_conn_desc *out) {
  if (!p.active || p.closing || p.connecting || p.pending || p.conn == PNONE)
    return BLE_HS_EBUSY;
  int rc = ble_gap_conn_find(p.conn, out);
  if (rc) return rc;
  if (!out->sec_state.encrypted || !out->sec_state.bonded) return BLE_HS_ENOTSUP;
  rc = ble_gap_set_event_cb(p.conn, callback, arg);
  if (rc) return rc;
  buddy_probe_voice_detach();
  p.voice = p.active = p.closing = false;
  p.conn = PNONE;
  ++p.generation;
  ++p.link_generation;
  phase("idle");
  return 0;
}
bool buddy_probe_active(void) { return p.active; }
bool buddy_probe_scanning(void) {
  return p.active && !p.closing && !buddy_probe_link_busy();
}
bool buddy_probe_link_busy(void) { return p.connecting || p.conn != PNONE; }
int buddy_probe_connect(uint8_t own, const ble_addr_t *a) {
  if (!p.active || p.closing || buddy_probe_link_busy())
    return BLE_HS_EBUSY;
  if (buddy_identity_is_bound(a)) return BLE_HS_EALREADY;
  /* A failed/uncommitted pairing may leave a bond behind, including across
   * power loss. It is not a binding. Repair only this selected unbound peer;
   * failed deletion must block connection rather than lose the cleanup task. */
  for (int i = 0; i < p.bond_count; i++)
    if (buddy_address_equal(a, &p.bonds[i])) {
      int rc = buddy_bond_remove_ram(a);
      if (rc && rc != BLE_HS_ENOENT) { err(rc); return rc; }
      memmove(&p.bonds[i], &p.bonds[i+1],
              (p.bond_count-i-1) * sizeof p.bonds[0]);
      p.bond_count--;
      break;
    }
  p.count = p.used = p.sequence = 0;
  memset(p.reports, 0, sizeof p.reports);
  p.encrypted = false;
  err(0);
  p.disconnect_reason = 0;
  phase("connecting");
  int rc = buddy_link_connect(own, a, gap_cb,
                            (void *)(uintptr_t)++p.link_generation);
  if (!rc) {
    p.connecting = true;
    p.deadline = pn() + 10000;
    phase("connecting");
  } else
    err(rc);
  return rc;
}
void buddy_probe_tick(uint32_t now, bool managed) {
  if (!p.active)
    return;
  if (!managed && !p.closing)
    stop();
  if (p.closing) {
    if ((int32_t)(now - p.close_retry) >= 0)
      stop();
    return;
  }
  buddy_probe_voice_tick(now);
  if ((p.pending || p.connecting) && (int32_t)(now - p.deadline) >= 0) {
    err(BLE_HS_ETIMEOUT);
    stop();
  }
}
void buddy_probe_reset(void) {
  buddy_probe_voice_detach();
  if (!p.active)
    return;
  p.conn = PNONE;
  p.connecting = p.pending = false;
  p.generation++;
  p.link_generation++;
  p.closing = true;
}
static void hex(cJSON *j, const char *key, const uint8_t *v, unsigned n) {
  char text[257];
  if (n > 128)
    n = 128;
  for (unsigned i = 0; i < n; i++)
    snprintf(text + 2 * i, 3, "%02x", v[i]);
  text[2 * n] = 0;
  cJSON_AddStringToObject(j, key, text);
}
uint16_t buddy_probe_command(uint16_t op, const cJSON *q, cJSON *j) {
  uint32_t index, offset;
  if (op == BUDDY_PROBE_BEGIN) {
    if (p.active)
      return RBP_STATUS_BUSY;
    ble_addr_t peers[8];
    int n = 0, rc = ble_store_util_bonded_peers(peers, &n, 8);
    if (rc) {
      cJSON_AddNumberToObject(j, "sdk_error", rc);
      return RBP_STATUS_DEVICE_ERROR;
    }
    uint32_t gen = p.generation + 1, link = p.link_generation + 1;
    memset(&p, 0, sizeof p);
    p.generation = gen;
    p.link_generation = link;
    p.conn = PNONE;
    p.active = true;
    p.bond_count = n;
    memcpy(p.bonds, peers, n * sizeof *peers);
    phase("scanning");
    return 0;
  }
  if (op == BUDDY_PROBE_STATUS) {
    cJSON_AddBoolToObject(j, "active", p.active);
    cJSON_AddStringToObject(j, "phase", p.phase);
    cJSON_AddBoolToObject(j, "connected", p.conn != PNONE);
    cJSON_AddBoolToObject(j, "pending", p.pending || p.connecting);
    cJSON_AddBoolToObject(j, "encrypted", p.encrypted);
    cJSON_AddNumberToObject(j, "sdk_error", p.error);
    cJSON_AddStringToObject(j, "error_phase", p.error_phase);
    cJSON_AddNumberToObject(j, "error_handle", p.error_handle);
    cJSON_AddNumberToObject(j, "disconnect_reason", p.disconnect_reason);
    cJSON_AddNumberToObject(j, "cleanup_error", p.cleanup_error);
    cJSON_AddNumberToObject(j, "attributes", p.count);
    cJSON_AddNumberToObject(j, "sequence", p.sequence);
#ifdef S3_HCI_PROBE
    cJSON_AddNumberToObject(j, "mtu", p.conn==PNONE?0:ble_att_mtu(p.conn));
#endif
    return 0;
  }
  if (op == BUDDY_PROBE_END) {
    stop();
    return 0;
  }
  if (!p.active || p.closing)
    return RBP_STATUS_BAD_STATE;
  if (op == BUDDY_PROBE_ATTR) {
    if (p.pending)
      return RBP_STATUS_BUSY;
    if (!buddy_u32(q, "index", &index) || index >= p.count ||
        !buddy_u32(q, "offset", &offset))
      return RBP_STATUS_INVALID_ARGUMENT;
    probe_attr_t *a = &p.attrs[index];
    if (offset > a->length)
      return RBP_STATUS_INVALID_ARGUMENT;
    cJSON_AddNumberToObject(j, "index", index);
    cJSON_AddNumberToObject(j, "kind", a->kind);
    cJSON_AddNumberToObject(j, "handle", a->handle);
    cJSON_AddNumberToObject(j, "parent", a->parent);
    cJSON_AddNumberToObject(j, "end", a->end);
    cJSON_AddNumberToObject(j, "properties", a->properties);
    cJSON_AddStringToObject(j, "uuid", a->uuid);
    cJSON_AddNumberToObject(j, "length", a->length);
    cJSON_AddBoolToObject(j, "complete", a->complete);
    hex(j, "hex", p.values + a->offset + offset, a->length - offset);
    return 0;
  }
  if (op == BUDDY_PROBE_REPORT) {
    if (!buddy_u32(q, "after", &index))
      return RBP_STATUS_INVALID_ARGUMENT;
    if (index >= p.sequence)
      return RBP_STATUS_NOT_FOUND;
    uint32_t next = index + 1;
    if (p.sequence - next >= REPORTS)
      next = p.sequence - REPORTS + 1;
    probe_report_t *r = &p.reports[(next - 1) % REPORTS];
    cJSON_AddNumberToObject(j, "sequence", next);
    cJSON_AddNumberToObject(j, "lost", next - index - 1);
    cJSON_AddNumberToObject(j, "time_ms", r->time);
    cJSON_AddNumberToObject(j, "handle", r->handle);
    cJSON_AddNumberToObject(j, "length", r->length);
    hex(j, "hex", r->data, r->length < sizeof r->data ? r->length : sizeof r->data);
    return 0;
  }
  if (op >= BUDDY_PROBE_VOICE_ARM && op <= BUDDY_PROBE_VOICE_CANCEL) {
    if (p.pending || p.conn == PNONE || !p.encrypted)
      return RBP_STATUS_BAD_STATE;
    uint16_t rc = buddy_probe_voice_command(op, q, j, p.conn);
    if (!rc && op == BUDDY_PROBE_VOICE_ARM)
      p.voice = true;
    return rc;
  }
  if (p.voice)
    return RBP_STATUS_BUSY;
  if (p.pending || p.conn == PNONE)
    return RBP_STATUS_BAD_STATE;
  int rc = 0;
#ifdef S3_HCI_PROBE
  if(op==BUDDY_PROBE_RAW_WRITE) {
    const cJSON *value=cJSON_GetObjectItemCaseSensitive(q,"hex");
    if(!p.encrypted || !buddy_u32(q,"index",&index) || index>=p.count ||
       !cJSON_IsString(value) || !value->valuestring) return RBP_STATUS_INVALID_ARGUMENT;
    probe_attr_t *a=&p.attrs[index];
    bool allowed=false;
    for(unsigned i=0;i<p.count;i++) {
      probe_attr_t *d=&p.attrs[i];
      if(d->kind==3 && d->parent==a->handle && uuid(d,0x2908) &&
         d->complete && d->length==2 && p.values[d->offset+1]==3 &&
         p.values[d->offset]==4) allowed=true;
    }
    if(a->kind!=2 || !uuid(a,0x2a4d) || !(a->properties&8) || !allowed)
      return RBP_STATUS_UNSUPPORTED;
    const char *h=value->valuestring;
    uint8_t bytes[20];
    if(strlen(h)!=40)return RBP_STATUS_INVALID_ARGUMENT;
    for(unsigned i=0;i<20;i++) {
      unsigned v=0;
      for(unsigned k=0;k<2;k++) {
        char c=h[i*2+k];unsigned n;
        if(c>='0' && c<='9')n=c-'0';
        else if(c>='a' && c<='f')n=c-'a'+10;
        else if(c>='A' && c<='F')n=c-'A'+10;
        else return RBP_STATUS_INVALID_ARGUMENT;
        v=v*16+n;
      }
      bytes[i]=v;
    }
    /* Only the TV-derived microphone command, never arbitrary feature/OTA data. */
    for(unsigned i=0;i<20;i++)
      if(bytes[i]!=(i==3?0x80:i==4?bytes[4]:0) || (i==4 && bytes[4]>1))
        return RBP_STATUS_INVALID_ARGUMENT;
    phase("writing");
    rc=submitted(ble_gattc_write_flat(p.conn,a->handle,bytes,20,write_cb,token()));
  } else if(op==BUDDY_PROBE_MTU) {
    if(!p.encrypted)return RBP_STATUS_BAD_STATE;
    phase("mtu");
    rc=ble_att_set_preferred_mtu(120);
    if(!rc)rc=submitted(ble_gattc_exchange_mtu(p.conn,probe_mtu_cb,token()));
    else done(rc);
    /* Restore the production default; the exchange request was already built. */
    ble_att_set_preferred_mtu(247);
  } else
#endif
  if (op == BUDDY_PROBE_SECURITY) {
    if (p.encrypted) {
      err(0);
      return 0;
    }
    phase("pairing");
    token();
    rc = submitted(ble_gap_security_initiate(p.conn));
  } else if (op == BUDDY_PROBE_DISCOVER) {
    p.count = p.used = 0;
    err(0);
    phase("services");
    rc = submitted(ble_gattc_disc_all_svcs(p.conn, svc_cb, token()));
  } else if (op == BUDDY_PROBE_READ) {
    if (!buddy_u32(q, "index", &index) || index >= p.count)
      return RBP_STATUS_INVALID_ARGUMENT;
    probe_attr_t *a = &p.attrs[index];
    if (a->kind == 1)
      return RBP_STATUS_INVALID_ARGUMENT;
    if (a->complete) {
      err(0);
      return 0;
    }
    /* Reclaim failed tail reads before retry. Reads are serialized. */
    if (a->length && a->offset + a->length == p.used)
      p.used = a->offset;
    a->offset = p.used;
    a->length = 0;
    p.reading = index;
    phase("reading");
    rc = submitted(ble_gattc_read_long(p.conn, a->handle, 0, read_cb, token()));
  } else if (op == BUDDY_PROBE_SUBSCRIBE) {
    if (!buddy_u32(q, "index", &index) || index >= p.count)
      return RBP_STATUS_INVALID_ARGUMENT;
    probe_attr_t *a = &p.attrs[index];
    if (a->kind != 2 || !uuid(a, 0x2a4d) || !(a->properties & 0x10))
      return RBP_STATUS_UNSUPPORTED;
    unsigned cccd = 0;
    bool input = false;
    for (unsigned i = 0; i < p.count; i++) {
      probe_attr_t *d = &p.attrs[i];
      if (d->kind != 3 || d->parent != a->handle)
        continue;
      if (uuid(d, 0x2902))
        cccd = d->handle;
      if (uuid(d, 0x2908) && d->complete && d->length == 2 &&
          p.values[d->offset + 1] == 1)
        input = true;
    }

#ifdef S3_HCI_PROBE
    if(p.encrypted && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(q,"raw"))) {
      for(unsigned i=0;i<p.count;i++) {
        probe_attr_t *d=&p.attrs[i];
        if(d->kind==3 && d->parent==a->handle && uuid(d,0x2908) &&
           d->complete && d->length==2 && p.values[d->offset+1]==3)input=true;
      }
    }
#endif
    if (!cccd || !input)
      return RBP_STATUS_BAD_STATE;
    uint8_t value[2] = {1, 0};
    phase("subscribing");
    rc = submitted(
        ble_gattc_write_flat(p.conn, cccd, value, 2, write_cb, token()));
  } else
    return RBP_STATUS_UNSUPPORTED;
  cJSON_AddNumberToObject(j, "sdk_error", rc);
  return rc ? RBP_STATUS_DEVICE_ERROR : RBP_STATUS_OK;
}

bool buddy_probe_cached_value(uint16_t conn, uint16_t handle, const uint8_t **data, uint16_t *len) {
  if (!p.active || p.closing || p.pending || !p.encrypted || p.conn != conn) return false;
  for (unsigned i=0; i<p.count; i++) {
    const probe_attr_t *a=&p.attrs[i];
    if (a->handle==handle && a->complete && (uuid(a,0x2a4b) || uuid(a,0x2908))) {
      *data=p.values+a->offset; *len=a->length; return true;
    }
  }
  return false;
}
