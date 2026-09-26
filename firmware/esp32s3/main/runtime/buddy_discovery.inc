/* Physical scan lifetime is independent of the user's discovery session. */
static bool stop_scan(void) {
  if (scanning) {
    int rc = ble_gap_disc_cancel();
    if (rc && rc != BLE_HS_EALREADY) { fault(255, 2, rc); return false; }
    scanning = false;
    ++scan_generation; /* Ignore completion queued by the cancelled scan. */
  }
  if (raw_identity_scan) {
    int rc = ble_hs_pvcy_set_resolve_enabled(1);
    if (rc) { fault(255, 42, rc); return false; }
    raw_identity_scan = false;
  }
  return true;
}
/* Shared discovery for pairing, probing and reconnect. BLE host task only. */
#define BUDDY_MIN_UNBOUND_DISCOVERY_RSSI (-90)
static bool zero_address(const ble_addr_t *a) {
  const uint8_t zero[6]={0};
  return !memcmp(a->val,zero,sizeof zero);
}
static void remove_privacy_placeholder(void) {
  if(own_address!=0)return;
  extern int ble_hs_pvcy_remove_entry(uint8_t,const uint8_t *);
  const uint8_t zero[6]={0};
  int rc=ble_hs_pvcy_remove_entry(0,zero);
  /* HCI Unknown Connection Identifier means the entry is already absent. */
  if(rc!=0x202)fault(255,35,rc);
}
static void scan_start(bool manual, uint32_t duration) {
  if (!synced || connecting_slot >= 0 || standalone_busy() ||
      buddy_update_busy())
    return;
  if (!stop_scan()) return;
  if (manual) { manual_scan = true; scan_until = now_ms() + duration; }
  remove_privacy_placeholder();
  if(privacy_repair_pending && own_address==0) {
    int rc=ble_hs_pvcy_set_resolve_enabled(0);
    fault(255,41,rc);
    if(!rc) {raw_identity_scan=true;if(duration>1500)duration=1500;}
  }
  privacy_repair_pending=false;

  struct ble_gap_disc_params p = {
      .itvl = 48, .window = 32, .passive = 0, .filter_duplicates = 0};
  int rc = ble_gap_disc(own_address, duration, &p, gap, (void *)(uintptr_t)++scan_generation);
  fault(255, 7, rc);
  scanning = !rc;
  next_scan = now_ms() + 1000;
}
static bool valid_name(char *out, const uint8_t *p, unsigned n) {
  if (!n || n >= 48)
    return false;
  for (unsigned i = 0; i < n;) {
    unsigned k = 1;
    uint32_t cp = p[i];
    if (cp >= 0xc2 && cp <= 0xdf) {
      k = 2;
      cp &= 31;
    } else if (cp >= 0xe0 && cp <= 0xef) {
      k = 3;
      cp &= 15;
    } else if (cp >= 0xf0 && cp <= 0xf4) {
      k = 4;
      cp &= 7;
    } else if (cp < 32 || cp >= 128)
      return false;
    if (i + k > n)
      return false;
    for (unsigned j = 1; j < k; j++) {
      if ((p[i + j] & 0xc0) != 0x80)
        return false;
      cp = (cp << 6) | (p[i + j] & 63);
    }
    if ((k == 3 && cp < 0x800) || (k == 4 && cp < 0x10000) || cp > 0x10ffff ||
        (cp >= 0xd800 && cp <= 0xdfff))
      return false;
    i += k;
  }
  memcpy(out, p, n);
  out[n] = 0;
  return true;
}
/* Advertisement and scan response can carry company/name separately. Give
 * potential names a cache entry; require all fields before declaring known. */
static bool possible_model_name(const char *name) {
  if (!name[0])
    return false;
  return model_match_name(name, -1, true) != -1;
}
static void on_scan(const struct ble_gap_disc_desc *d) {
  scan_diag.total++;
  int bound = identity_slot(&d->addr);
  if(bound >= 0)scan_diag.bound[bound]++;
  if(bound >= 0 || zero_address(&d->addr)) {
    scan_diag.last_ms=now_ms();scan_diag.last_address=d->addr;
    scan_diag.last_rssi=d->rssi;scan_diag.last_event=d->event_type;
  }
  /* A controller-resolved all-zero identity is never a connectable peer.
   * Repair outside this GAP callback, then collect the real advertisement. */
  if(zero_address(&d->addr)) {
    scan_diag.zero++;
    if(own_address==0)privacy_repair_pending=true;
    return;
  }


  if (d->rssi > 0 ||
      (bound < 0 && d->rssi < BUDDY_MIN_UNBOUND_DISCOVERY_RSSI)) {
    scan_diag.weak++;
    return;
  }
  /* A real public identity is usable for both reconnect and new pairing.
   * Requiring an existing bond here makes deleted devices undiscoverable.
   * Keep unresolved private addresses out; model validation still occurs
   * after connection, never by substituting an address based on the name. */
  if(raw_identity_scan && d->addr.type!=0)return;
  struct ble_hs_adv_fields f;
  if (ble_hs_adv_parse_fields(&f, d->data, d->length_data)) {
    scan_diag.malformed++;
    return;
  }
  char name[48] = {0};
  if (f.name_len)
    valid_name(name, f.name, f.name_len);
  bool connectable = d->event_type == BLE_HCI_ADV_RPT_EVTYPE_ADV_IND ||
                     d->event_type == BLE_HCI_ADV_RPT_EVTYPE_DIR_IND;
  unsigned i;
  for (i = 0; i < CANDIDATES; i++)
    if (candidates[i].id && buddy_address_equal(&candidates[i].address, &d->addr))
      break;
  if (i == CANDIDATES) {
    bool response = d->event_type == BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP && name[0];
    if (!connectable && !response && !buddy_probe_active())
      return;
    for (i = 0; i < CANDIDATES; i++)
      if (!candidates[i].id || now_ms() - candidates[i].seen > 15000)
        break;
    if (i == CANDIDATES && (buddy_probe_active() || possible_model_name(name) ||
                            identity_slot(&d->addr) >= 0)) {
      uint32_t oldest = 0;
      for (unsigned j = 0; j < CANDIDATES; j++)
        if (!candidates[j].known && identity_slot(&candidates[j].address) < 0) {
          uint32_t age = now_ms() - candidates[j].seen;
          if (i == CANDIDATES || age > oldest) {
            i = j;
            oldest = age;
          }
        }
    }
    if (i == CANDIDATES)
      return;
    memset(&candidates[i], 0, sizeof candidates[i]);
    candidates[i].company = -1;
    candidates[i].address = d->addr;
    candidates[i].id = ++next_candidate;
    if (!candidates[i].id)
      candidates[i].id = ++next_candidate;
  }
  /* A shortened ADV name must not overwrite the complete SCAN_RSP name. */
  if (name[0] && (f.name_is_complete || !candidates[i].name_complete)) {
    snprintf(candidates[i].name, sizeof candidates[i].name, "%s", name);
    candidates[i].name_complete = f.name_is_complete;
  }
  if (d->length_data <= 31) {
    bool response = d->event_type == BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP;
    memcpy(response ? candidates[i].response : candidates[i].adv, d->data,
           d->length_data);
    if (response)
      candidates[i].response_len = d->length_data;
    else
      candidates[i].adv_len = d->length_data;
  }
  candidates[i].seen = now_ms();
  candidates[i].rssi = d->rssi;
  if (f.mfg_data_len >= 2)
    candidates[i].company = f.mfg_data[0] | ((unsigned)f.mfg_data[1] << 8);
  for (unsigned j=0;j<f.num_uuids16;j++)
    if (f.uuids16[j].value==0x1812) candidates[i].hid_hint=true;
  candidates[i].known = identity_slot(&d->addr) >= 0 || candidates[i].hid_hint ||
      model_match_name(candidates[i].name, candidates[i].company, false) != -1;
  if (d->event_type != BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP)
    candidates[i].connectable = connectable;

}
