#include "s3_gatt.h"
#include "buddy_probe.h"
#include "esp_timer.h"
#include "faults.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include <string.h>

static s3_gatt_t *contexts[S3_GATT_SLOTS];
static void *token(s3_gatt_t *g) {
  g->operation = (g->operation + 1) & (UINTPTR_MAX >> 3);
  return (void *)((g->operation << 3) | g->slot);
}
static s3_gatt_t *current(uint16_t c, void *arg) {
  uintptr_t v = (uintptr_t)arg;
  s3_gatt_t *g = (v & 7) < S3_GATT_SLOTS ? contexts[v & 7] : NULL;
  return g && g->connection == c && g->operation == (v >> 3) ? g : NULL;
}
static void emit(s3_gatt_t *g, rbp_gatt_evt_t *e) {
  g->adapter->now_ms = esp_timer_get_time() / 1000;
  rc003_adapter_on_gatt(g->adapter, e);
}
static void error_event(s3_gatt_t *g, int status) {
  buddy_probe_voice_sdk(g->connection, status);
  rbp_fault_record(RBP_FAULT_SDK, 0x1000, (uint32_t)status, g->connection);
  rbp_gatt_evt_t e = {.type = status >= BLE_HS_ERR_ATT_BASE &&
                                      status < BLE_HS_ERR_ATT_BASE + 256
                                  ? RBP_GATT_EVT_PROC_ERROR
                                  : RBP_GATT_EVT_BEARER_FAILED,
                      .status = (uint8_t)status};
  emit(g, &e);
}
static int submitted(s3_gatt_t *g, int rc) {
  if (!rc)
    return 0;
  buddy_probe_voice_sdk(g->connection, rc);
  rbp_fault_record(RBP_FAULT_SDK, 0x1001, (uint32_t)rc, g->connection);
  return rc == BLE_HS_EBUSY || rc == BLE_HS_ENOMEM || rc == BLE_HS_EAGAIN
             ? RBP_GATT_RETRY
             : -1;
}
static void identity_finish(s3_gatt_t *g, bool ok) {
  g->identity_stage = 0;
  g->identity.complete = ok;
  void (*done)(s3_gatt_t *, bool) = g->identity_done;
  g->identity_done = NULL;
  if (done)
    done(g, ok);
}
static int identity_pnp_cb(uint16_t c, const struct ble_gatt_error *err,
                           struct ble_gatt_attr *attr, void *arg) {
  s3_gatt_t *g = current(c, arg);
  if (!g || !g->identity_stage)
    return 0;
  if (!err->status && attr) {
    if (OS_MBUF_PKTLEN(attr->om) != 7 || g->identity.pnp_valid ||
        os_mbuf_copydata(attr->om, 0, 7, g->identity.pnp)) {
      identity_finish(g, false);
      return 0;
    }
    g->identity.pnp_valid = true;
  } else if (err->status == BLE_HS_EDONE ||
             err->status == BLE_HS_ATT_ERR(BLE_ATT_ERR_ATTR_NOT_FOUND))
    identity_finish(g, true);
  else
    identity_finish(g, false);
  return 0;
}
static int identity_services_cb(uint16_t c, const struct ble_gatt_error *err,
                                const struct ble_gatt_svc *svc, void *arg) {
  s3_gatt_t *g = current(c, arg);
  if (!g || !g->identity_stage)
    return 0;
  if (!err->status && svc) {
    if (g->identity.service_count >= 32) {
      identity_finish(g, false);
      return 0;
    }
    ble_uuid_to_str(&svc->uuid.u,
                    g->identity.services[g->identity.service_count++]);
  } else if (err->status == BLE_HS_EDONE)
    g->identity_stage = 3;
  else
    identity_finish(g, false);
  return 0;
}
void s3_gatt_identify(s3_gatt_t *g, void (*done)(s3_gatt_t *, bool)) {
  memset(&g->identity, 0, sizeof g->identity);
  g->identity_stage = 1;
  g->identity_done = done;
  g->identity_deadline = (uint32_t)(esp_timer_get_time() / 1000) + 5000;
}
static void identity_poll(s3_gatt_t *g) {
  if ((int32_t)((uint32_t)(esp_timer_get_time() / 1000) -
                g->identity_deadline) >= 0) {
    identity_finish(g, false);
    return;
  }
  int rc = 0;
  if (g->identity_stage == 1) {
    g->identity_stage = 2;
    rc = ble_gattc_disc_all_svcs(g->connection, identity_services_cb, token(g));
    if (rc)
      g->identity_stage = 1;
  } else if (g->identity_stage == 3) {
    ble_uuid16_t uuid = BLE_UUID16_INIT(0x2a50);
    g->identity_stage = 4;
    rc = ble_gattc_read_by_uuid(g->connection, 1, 65535, &uuid.u,
                                identity_pnp_cb, token(g));
    if (rc)
      g->identity_stage = 3;
  }
  if (rc && rc != BLE_HS_EBUSY && rc != BLE_HS_ENOMEM && rc != BLE_HS_EAGAIN)
    identity_finish(g, false);
}
static int mtu_cb(uint16_t c, const struct ble_gatt_error *err, uint16_t mtu,
                  void *arg) {
  s3_gatt_t *g = current(c, arg);
  if (!g)
    return 0;
  if (err->status) {
    error_event(g, err->status);
    return 0;
  }
  rbp_gatt_evt_t e = {.type = RBP_GATT_EVT_MTU_UPDATED, .mtu = mtu};
  emit(g, &e);
  return 0;
}
static int service_cb(uint16_t c, const struct ble_gatt_error *err,
                      const struct ble_gatt_svc *s, void *arg) {
  s3_gatt_t *g = current(c, arg);
  if (!g)
    return 0;
  rbp_gatt_evt_t e = {0};
  if (!err->status && s) {
    g->found = true;
    e.type = RBP_GATT_EVT_SERVICE_FOUND;
    e.svc_start = s->start_handle;
    e.svc_end = s->end_handle;
  } else if (err->status == BLE_HS_EDONE)
    e.type = g->found ? RBP_GATT_EVT_PROC_DONE : RBP_GATT_EVT_SERVICE_NOT_FOUND;
  else {
    error_event(g, err->status);
    return 0;
  }
  emit(g, &e);
  return 0;
}
static int char_cb(uint16_t c, const struct ble_gatt_error *err,
                   const struct ble_gatt_chr *ch, void *arg) {
  s3_gatt_t *g = current(c, arg);
  if (!g)
    return 0;
  rbp_gatt_evt_t e = {0};
  uint8_t value[19];
  if (!err->status && ch) {
    value[0] = ch->properties;
    value[1] = ch->val_handle;
    value[2] = ch->val_handle >> 8;
    unsigned n = ch->uuid.u.type == BLE_UUID_TYPE_16 ? 2 : 16;
    if (n == 2) {
      value[3] = ch->uuid.u16.value;
      value[4] = ch->uuid.u16.value >> 8;
    } else
      memcpy(value + 3, ch->uuid.u128.value, 16);
    e.type = RBP_GATT_EVT_CHARS_FOUND;
    e.handle = ch->def_handle;
    e.value = value;
    e.len = n + 3;
  } else if (err->status == BLE_HS_EDONE)
    e.type = RBP_GATT_EVT_PROC_DONE;
  else {
    error_event(g, err->status);
    return 0;
  }
  emit(g, &e);
  return 0;
}
static int desc_cb(uint16_t c, const struct ble_gatt_error *err, uint16_t ch,
                   const struct ble_gatt_dsc *d, void *arg) {
  (void)ch;
  s3_gatt_t *g = current(c, arg);
  if (!g)
    return 0;
  rbp_gatt_evt_t e = {0};
  if (!err->status && d) {
    e.type = RBP_GATT_EVT_DESC_FOUND;
    e.desc_handle = d->handle;
    e.uuid16 = ble_uuid_u16(&d->uuid.u);
  } else if (err->status == BLE_HS_EDONE)
    e.type = RBP_GATT_EVT_PROC_DONE;
  else {
    error_event(g, err->status);
    return 0;
  }
  emit(g, &e);
  return 0;
}
static int read_cb(uint16_t c, const struct ble_gatt_error *err,
                   struct ble_gatt_attr *a, void *arg) {
  s3_gatt_t *g = current(c, arg);
  if (!g)
    return 0;
  if (err->status) {
    error_event(g, err->status);
    return 0;
  }
  uint8_t value[512];
  unsigned len = OS_MBUF_PKTLEN(a->om);
  if (len > sizeof value || os_mbuf_copydata(a->om, 0, len, value)) {
    error_event(g, BLE_HS_EBADDATA);
    return 0;
  }
  rbp_gatt_evt_t e = {.type = RBP_GATT_EVT_READ_RSP,
                      .handle = a->handle,
                      .offset = 0,
                      .value = value,
                      .len = len,
                      .proc_complete = len < (unsigned)ble_att_mtu(c) - 1};
  emit(g, &e);
  return 0;
}
static int long_cb(uint16_t c, const struct ble_gatt_error *err,
                   struct ble_gatt_attr *a, void *arg) {
  s3_gatt_t *g = current(c, arg);
  if (!g)
    return 0;
  rbp_gatt_evt_t e = {.type = RBP_GATT_EVT_READ_RSP, .offset = g->read_offset};
  uint8_t value[512];
  if (err->status == BLE_HS_EDONE) {
    e.proc_complete = true;
    emit(g, &e);
    return 0;
  }
  if (err->status) {
    error_event(g, err->status);
    return 0;
  }
  unsigned n = OS_MBUF_PKTLEN(a->om);
  if (n > sizeof value || os_mbuf_copydata(a->om, 0, n, value)) {
    error_event(g, BLE_HS_EBADDATA);
    return 0;
  }
  e.handle = a->handle;
  e.value = value;
  e.len = n;
  g->read_offset += n;
  emit(g, &e);
  return 0;
}
static int write_cb(uint16_t c, const struct ble_gatt_error *err,
                    struct ble_gatt_attr *a, void *arg) {
  (void)a;
  s3_gatt_t *g = current(c, arg);
  if (!g)
    return 0;
  if (err->status)
    error_event(g, err->status);
  else {
    rbp_gatt_evt_t e = {.type = RBP_GATT_EVT_WRITE_DONE};
    emit(g, &e);
  }
  return 0;
}
#include "s3_gatt_mtu.inc"
static int service16(void *u, uint16_t id) {
  s3_gatt_t *g = u;
  ble_uuid16_t uuid = BLE_UUID16_INIT(id);
  g->found = false;
  return submitted(g, ble_gattc_disc_svc_by_uuid(g->connection, &uuid.u,
                                                 service_cb, token(g)));
}
static int service128(void *u, const uint8_t *id) {
  s3_gatt_t *g = u;
  ble_uuid128_t uuid = {.u.type = BLE_UUID_TYPE_128};
  memcpy(uuid.value, id, 16);
  g->found = false;
  return submitted(g, ble_gattc_disc_svc_by_uuid(g->connection, &uuid.u,
                                                 service_cb, token(g)));
}
static int chars16(void *u, uint16_t a, uint16_t b, uint16_t id) {
  s3_gatt_t *g = u;
  ble_uuid16_t uuid = BLE_UUID16_INIT(id);
  return submitted(g, ble_gattc_disc_chrs_by_uuid(g->connection, a, b, &uuid.u,
                                                  char_cb, token(g)));
}
static int chars128(void *u, uint16_t a, uint16_t b, const uint8_t *id) {
  s3_gatt_t *g = u;
  ble_uuid128_t uuid = {.u.type = BLE_UUID_TYPE_128};
  memcpy(uuid.value, id, 16);
  return submitted(g, ble_gattc_disc_chrs_by_uuid(g->connection, a, b, &uuid.u,
                                                  char_cb, token(g)));
}
static int descs(void *u, uint16_t a, uint16_t b) {
  s3_gatt_t *g = u;
  return submitted(
      g, ble_gattc_disc_all_dscs(g->connection, a - 1, b, desc_cb, token(g)));
}
// Replay only metadata already read on this live probe connection. Completion
// is deferred to the host tick, preserving the adapter's asynchronous contract.
static bool cached_read(s3_gatt_t *g, uint16_t h, uint16_t offset,
                        bool long_read) {
  const uint8_t *data;
  uint16_t len;
  if (g->slot != 4 ||
      !buddy_probe_cached_value(g->connection, h, &data, &len) || offset > len)
    return false;
  token(g);
  g->cached_handle = h;
  g->cached_offset = offset;
  g->cached_long = long_read;
  return true;
}
void s3_gatt_poll(s3_gatt_t *g) {
  if (g->identity_stage) {
    identity_poll(g);
    return;
  }
  if (!g->cached_handle)
    return;
  uint16_t h = g->cached_handle, off = g->cached_offset;
  bool lng = g->cached_long;
  g->cached_handle = 0;
  const uint8_t *data;
  uint16_t len;
  if (!buddy_probe_cached_value(g->connection, h, &data, &len) || off > len) {
    error_event(g, BLE_HS_EBADDATA);
    return;
  }
  // Ordinary ATT read is bounded to MTU-1. Long read delivers full cached
  // value.
  unsigned n = len - off, limit = ble_att_mtu(g->connection) - 1;
  if (!lng && n > limit)
    n = limit;
  rbp_gatt_evt_t e = {.type = RBP_GATT_EVT_READ_RSP,
                      .handle = h,
                      .offset = off,
                      .value = data + off,
                      .len = n,
                      .proc_complete = lng || n < limit};
  emit(g, &e);
}
static int read_value(void *u, uint16_t h) {
  s3_gatt_t *g = u;
  if (cached_read(g, h, 0, false))
    return 0;
  return submitted(g, ble_gattc_read(g->connection, h, read_cb, token(g)));
}
static int read_long(void *u, uint16_t h, uint16_t off) {
  s3_gatt_t *g = u;
  if (cached_read(g, h, off, true))
    return 0;
  g->read_offset = off;
  return submitted(
      g, ble_gattc_read_long(g->connection, h, off, long_cb, token(g)));
}
static int write_value(void *u, uint16_t h, const uint8_t *v, uint16_t n) {
  s3_gatt_t *g = u;
  return submitted(
      g, ble_gattc_write_flat(g->connection, h, v, n, write_cb, token(g)));
}
static int write_command(void *u, uint16_t h, const uint8_t *v, uint16_t n) {
  s3_gatt_t *g = u;
  int rc = ble_gattc_write_no_rsp_flat(g->connection, h, v, n);
  s3_link_trace(g->slot, 4, ((uint32_t)g->connection << 16) | h,
                n ? v[0] | ((uint32_t)(n > 1 ? v[1] : 0) << 8) : 0, rc);
  return submitted(g, rc);
}
static const rbp_gatt_client_t api = {.exchange_mtu = mtu,
                                      .disc_service_by_uuid16 = service16,
                                      .disc_service_by_uuid128 = service128,
                                      .read_chars_by_uuid16 = chars16,
                                      .read_chars_by_uuid128 = chars128,
                                      .disc_char_descs = descs,
                                      .read_value = read_value,
                                      .read_long_value = read_long,
                                      .write_value = write_value,
                                      .write_command = write_command};
const rbp_gatt_client_t *s3_gatt_client(void) { return &api; }
void s3_gatt_init(s3_gatt_t *g, unsigned slot) {
  memset(g, 0, sizeof *g);
  g->slot = slot;
  g->connection = BLE_HS_CONN_HANDLE_NONE;
  contexts[slot] = g;
}
void s3_gatt_attach(s3_gatt_t *g, rc003_adapter_t *a, uint16_t c) {
  g->identity_stage = 0;
  g->identity_done = NULL;
  memset(&g->identity, 0, sizeof g->identity);
  g->cached_handle = 0;
  g->adapter = a;
  g->connection = c;
  g->operation = (g->operation + 1) & (UINTPTR_MAX >> 3);
}
void s3_gatt_detach(s3_gatt_t *g) {
  g->identity_stage = 0;
  g->identity_done = NULL;
  g->cached_handle = 0;
  g->connection = BLE_HS_CONN_HANDLE_NONE;
  g->operation = (g->operation + 1) & (UINTPTR_MAX >> 3);
}
void s3_gatt_notify(s3_gatt_t *g, uint16_t h, const uint8_t *v, uint16_t n) {
  rbp_gatt_evt_t e = {
      .type = RBP_GATT_EVT_NOTIFY, .handle = h, .value = v, .len = n};
  emit(g, &e);
}
