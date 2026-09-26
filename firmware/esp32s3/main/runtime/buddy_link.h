#pragma once
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include <string.h>
/* Shared identity and connection policy for normal and diagnostic ownership. */
static inline bool buddy_address_equal(const ble_addr_t *a, const ble_addr_t *b) {
  return (a->type & 1) == (b->type & 1) && !memcmp(a->val, b->val, 6);
}
/* RAM security records and the controller resolving list are one live view.
 * The SDK store-delete helper alone leaves the latter behind. */
static inline int buddy_bond_remove_ram(const ble_addr_t *a) {
  extern int ble_hs_pvcy_remove_entry(uint8_t,const uint8_t *);
  const uint8_t zero[6]={0};
  if (memcmp(a->val,zero,6)) {
    int rc=ble_hs_pvcy_remove_entry(a->type,a->val);
    if (rc && rc!=0x202) return rc;
  }
  return ble_store_util_delete_peer(a);
}
/* A successful terminate request is not a released connection. Only a GAP
 * completion or the host's ENOTCONN result permits the owner to release it. */
static inline int buddy_link_close(bool connecting, uint16_t connection) {
  if (connecting) return ble_gap_conn_cancel();
  if (connection == BLE_HS_CONN_HANDLE_NONE) return BLE_HS_ENOTCONN;
  return ble_gap_terminate(connection, BLE_ERR_REM_USER_CONN_TERM);
}
static inline int buddy_link_connect(uint8_t own, const ble_addr_t *a,
    int (*callback)(struct ble_gap_event *, void *), void *token) {
  const struct ble_gap_conn_params params = {.scan_itvl=16, .scan_window=16,
    .itvl_min=12, .itvl_max=24, .latency=0, .supervision_timeout=400};
  return ble_gap_connect(own, a, 5000, &params, callback, token);
}
