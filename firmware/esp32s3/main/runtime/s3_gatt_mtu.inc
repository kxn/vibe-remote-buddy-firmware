/* Shared with the host regression harness; uses the real platform flow. */
static int mtu(void *u, uint16_t n) {
  s3_gatt_t *g = u;
  uint16_t actual=ble_att_mtu(g->connection);
  if(actual>23) {
    rbp_gatt_evt_t e={.type=RBP_GATT_EVT_MTU_UPDATED,.mtu=actual};emit(g,&e);return 0;
  }
  int rc=ble_att_set_preferred_mtu(n);
  if(rc)return submitted(g,rc);
  rc=ble_gattc_exchange_mtu(g->connection,mtu_cb,token(g));
  if(rc==BLE_HS_EALREADY) {
    actual=ble_att_mtu(g->connection);
    if(actual>23){rbp_gatt_evt_t e={.type=RBP_GATT_EVT_MTU_UPDATED,.mtu=actual};emit(g,&e);return 0;}
    return RBP_GATT_RETRY;
  }
  return submitted(g,rc);
}
