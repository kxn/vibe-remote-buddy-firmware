/* One binding transaction pipeline: new links, reconnect and probe handoff. */
static bool persist_bundle(unsigned s, const record_t *r, const buddy_bond_t *bond) {
  char key[12];
  snprintf(key, sizeof key, "binding%u", s);
  size_t length;
  uint8_t *encoded = buddy_binding_blob_encode(r, bond, &length);
  int rc = encoded ? nvs_set_blob(storage, key, encoded, length) : ESP_ERR_NO_MEM;
  free(encoded);
  if (!rc)
    rc = nvs_commit(storage);
  fault(s, 1, rc);
  return !rc;
}
static bool persist(unsigned s, const record_t *r) {
  const buddy_bond_t empty={0};
  return persist_bundle(s,r,r->peer_id?&slots[s].bond:&empty);
}
static void operation_json(cJSON *j) {
  cJSON_AddNumberToObject(j, "operation_id", operation.id);
  cJSON_AddStringToObject(j, "kind", operation.kind);
  cJSON_AddBoolToObject(j, "pending", operation.pending);
  cJSON_AddNumberToObject(j, "slot", operation.slot);
  cJSON_AddNumberToObject(j, "peer_id", operation.peer);
  cJSON_AddNumberToObject(j, "result", operation.result);
  cJSON_AddBoolToObject(j, "uncertain", operation.uncertain);
  if (operation.model_error)
    cJSON_AddNumberToObject(j, "model_error", operation.model_error);
}
static void complete(slot_t *s, uint16_t result, bool uncertain) {
  if (!operation.pending || operation.slot != s->model.slot)
    return;
  operation.pending = false;
  operation.result = result;
  operation.peer = s->record.peer_id;
  operation.uncertain = uncertain;
  cJSON *j = cJSON_CreateObject();
  operation_json(j);
  buddy_management_event(BUDDY_OPERATION_EVENT, j);
}
static void begin_operation(slot_t *s, const char *kind) {
  operation.id = ++operation.next;
  if (!operation.id)
    operation.id = ++operation.next;
  operation.slot = s->model.slot;
  operation.peer = s->record.peer_id;
  operation.pending = true;
  operation.uncertain = false;
  operation.model_error = 0;
  memset(&operation.target, 0, sizeof operation.target);
  operation.model_id[0] = 0;
  operation.result = RBP_STATUS_ACCEPTED;
  snprintf(operation.kind, sizeof operation.kind, "%s", kind);
}
static void cleanup_identity(slot_t *s) {
  if (!s->identity_seen) return;
  if (!buddy_identity_is_bound(&s->identity)) {
    int rc = buddy_bond_remove_ram(&s->identity);
    if (rc && rc != BLE_HS_ENOENT) {
      fault(s->model.slot, 20, rc);
      s->close_retry = now_ms() + 1000;
      return;
    }
  } else if (s->failed || s->pairing || s->commit_pending) {
    /* Aborted SMP may have replaced the RAM keys of an existing binding. */
    for (unsigned i = 0; i < SLOTS; i++) {
      slot_t *owner = &slots[i];
      if (!owner->record.peer_id ||
          !buddy_address_equal(&owner->record.address, &s->identity)) continue;
      int rc = buddy_bond_remove_ram(&s->identity);
      if (!rc || rc == BLE_HS_ENOENT) rc = buddy_bond_restore(&owner->bond);
      if (rc) {
        fault(s->model.slot, 44, rc);
        s->failed = true;
        s->close_retry = now_ms() + 1000;
        return;
      }
    }
  }
  s->identity_seen = false;
}
/* Both GAP completion and verified host-side absence finish through here. */
static void release_link(slot_t *s, int reason) {
  s->disconnected_ms = now_ms();
  s->disconnect_reason = reason;
  s3_peer_disconnected(s->model.slot, now_ms());
  s3_gatt_detach(&s->gatt);
  rc003_adapter_detach(&s->adapter);
  s->conn = NONE;
  s->connecting = false;
  if (connecting_slot == (int)s->model.slot) connecting_slot = -1;
  cleanup_identity(s);
  s->provisional_id = 0;
  s->model.keys = 0;
  s->model.voice_down = false;
  s->model.voice_state = 0;
  s->model.battery = 255;
  if (s->pairing) complete(s, RBP_STATUS_LINK_LOST, false);
  s->pairing = s->commit_pending = false;
  set_state(s, s->record.peer_id ? RBP_LINK_DISCONNECTED : RBP_LINK_UNBOUND);
  s->retry_at = now_ms() + 300;
}
static void terminate(slot_t *s) {
  s->close_retry = now_ms() + 1000;
  int rc = buddy_link_close(s->connecting, s->conn);
  if (!s->connecting && rc == BLE_HS_ENOTCONN) {
    release_link(s, rc);
    return;
  }
  if (rc != BLE_HS_EALREADY) fault(s->model.slot, s->connecting ? 3 : 4, rc);
}
static void fail_pair(slot_t *s, uint16_t code) {
  if (s->pairing) {
    complete(s, code, code == RBP_STATUS_STORAGE_FAILED);
    s->pairing = false;
    s->commit_pending = false;
  }
  s->failed = true;
  terminate(s);
}
static void finish_forget(slot_t *s) {
  if (standalone_busy() || (int32_t)(now_ms() - s->retry_at) < 0)
    return;
  if (!s->record.cleanup_pending) {
    record_t *tombstone = &transaction_record;
    memset(tombstone,0,sizeof *tombstone);
    tombstone->version=RECORD_VERSION;tombstone->cleanup_pending=true;tombstone->address=s->record.address;
    buddy_map_default(&tombstone->map);
    if (!persist(s->model.slot, tombstone)) {
      s->forgetting = false;
      complete(s, RBP_STATUS_STORAGE_FAILED, true);
      return;
    }
    s->record = *tombstone;
    s->cache_hash = 0;
    s->model.keys = 0;
    s->model.profile = &RBP_PROFILE_RC003;
    rc003_adapter_init(&s->adapter, s3_gatt_client(), &s->gatt, &s->model);
    s3_voice_mode_reset(s->model.slot);
    s3_mapping(s->model.slot, &tombstone->map);
    set_state(s, RBP_LINK_UNBOUND);
  }
  int rc = buddy_bond_remove_ram(&s->record.address);
  if (rc == BLE_HS_ENOENT)
    rc = 0;
  fault(s->model.slot, 5, rc);
  if (!rc) {
    record_t *empty = &transaction_record; *empty = s->record;
    empty->cleanup_pending = false;
    memset(&empty->address, 0, sizeof empty->address);
    if (persist(s->model.slot, empty)) {
      s->record = *empty;
      s->forgetting = false;
      complete(s, RBP_STATUS_OK, false);
      return;
    }
  }
  s->retry_at = now_ms() + 1000;
  if ((int32_t)(now_ms() - s->deadline) >= 0)
    complete(s, RBP_STATUS_STORAGE_FAILED, true);
  /* The tombstone remains durable and this slot unavailable until cleanup.
   * A reboot resumes the same transaction rather than reviving a binding. */
}
static bool resolve_connected_model(rc003_adapter_t *, uint32_t);
static bool fingerprint_evidence(void *context, const cJSON *required) {
  slot_t *s = context;
  const s3_identity_t *identity = &s->gatt.identity;
  if (!identity->complete)
    return false;
  const cJSON *services =
      cJSON_GetObjectItemCaseSensitive(required, "services");
  if (services && !cJSON_IsArray(services))
    return false;
  for (const cJSON *v = services ? services->child : NULL; v; v = v->next) {
    const char *name = cJSON_GetStringValue(v);
    if (!name)
      return false;
    bool found = false;
    for (unsigned i = 0; i < identity->service_count; i++)
      if (!strcmp(name, identity->services[i]))
        found = true;
    if (!found)
      return false;
  }
  const cJSON *pnp = cJSON_GetObjectItemCaseSensitive(required, "pnp");
  if (pnp) {
    if (!identity->pnp_valid || !cJSON_IsObject(pnp))
      return false;
    const char *fields[] = {"source", "vendor", "product", "version"};
    unsigned values[] = {identity->pnp[0],
                         identity->pnp[1] | ((unsigned)identity->pnp[2] << 8),
                         identity->pnp[3] | ((unsigned)identity->pnp[4] << 8),
                         identity->pnp[5] | ((unsigned)identity->pnp[6] << 8)};
    for (unsigned i = 0; i < 4; i++) {
      const cJSON *v = cJSON_GetObjectItemCaseSensitive(pnp, fields[i]);
      if (v && (!cJSON_IsNumber(v) || v->valuedouble != values[i]))
        return false;
    }
  }
  const cJSON *reports = cJSON_GetObjectItemCaseSensitive(required, "reports");
  if (reports && !cJSON_IsArray(reports))
    return false;
  if (s->adapter.ready)
    for (const cJSON *v = reports ? reports->child : NULL; v; v = v->next) {
      uint32_t id, type;
      if (!buddy_u32(v, "id", &id) || !buddy_u32(v, "type", &type))
        return false;
      bool found = false;
      for (unsigned i = 0; i < s->adapter.char_count; i++)
        if (s->adapter.chars[i].report_id == id &&
            s->adapter.chars[i].report_type == type)
          found = true;
      if (!found)
        return false;
    }
  return true;
}
static void configure_model(slot_t *s) {
  s->adapter.variant.resolve = s->pairing ? resolve_connected_model : NULL;
  const buddy_model_t *variant = active_model(s);
  const rbp_device_profile_t *profile = variant ? &s->owned_profile : NULL;
  if (variant) {
    s->owned_profile=buddy_model_profile(variant,s->owned_keys,s->owned_labels,s->record.model_id);
  }
  if (s->adapter.variant.profile != profile) {
    s->adapter.cache_valid = false;
    s->adapter.cache_safe = false;
  }
  s->adapter.variant.profile = profile;
  if (variant)
    s->model.profile = s->adapter.variant.profile;
  s->adapter.variant.family = variant ? variant->family : 0;
  s->adapter.variant.map_crc = variant ? variant->map_crc : 0;
  s->adapter.variant.raw_count = variant ? variant->raw_count : 0;
  for (unsigned i = 0; variant && i < variant->raw_count; i++) {
    s->adapter.variant.raw_keys[i].report = variant->raw[i].report;
    s->adapter.variant.raw_keys[i].usage = variant->raw[i].usage;
    s->adapter.variant.raw_keys[i].key = variant->raw[i].key;
  }
}
static bool resolve_connected_model(rc003_adapter_t *a, uint32_t crc) {
  if (!a->server || a->server->slot >= SLOTS)
    return false;
  slot_t *s = &slots[a->server->slot];
  if (s->record.peer_id && s->record.definition.id[0]) {
    if (s->record.definition.map_crc != crc)
      return false;
    configure_model(s);
    return true;
  }
  int selected = s->requested_model;
  if (selected < 0 && s->record.peer_id)
    selected = model_index(s->record.model_id);
  int model=buddy_model_store_identify(model_store(),a->map_buf,a->map_len,crc,selected,fingerprint_evidence,s);
  if (model < 0) {
    if (operation.pending && operation.slot == s->model.slot)
      operation.model_error = model == -2 ? 2 : s->requested_model >= 0 ? 3 : 1;
    return false;
  }
  s->pending_model = model;
  configure_model(s);
  /* One model may accept several complete Maps. Pin the observed variant,
   * never the first fingerprint in the public resource. */
  s->adapter.variant.map_crc = crc;
  return true;
}
static void remember_pair_intent(slot_t *s, const ble_addr_t *a,
                                 int requested) {
  if (operation.pending && operation.slot == s->model.slot &&
      !strcmp(operation.kind, "pair")) {
    operation.target = *a;
    const buddy_model_t *m = model_at(requested);
    snprintf(operation.model_id, sizeof operation.model_id, "%s",
             m ? m->id : "");
  }
}
static void prepare_link(slot_t *s, const ble_addr_t *a, bool pairing,
                         int requested) {
  s->pending_model = -1;
  s->requested_model = requested;
  s->cache_checked = false;
  s->cache_retry = 0;
  s->link_started_ms = now_ms();
  s->ready_after_ms = 0;

  s->verifying_identity = pairing && s->record.peer_id && identity_slot(a) < 0;
  s->identity_seen = false;
  s->repair_attempted = false;
  s->provisional_id = 0;
  s->target = *a;
  s->pairing = pairing;
  s->failed = false;
  s->model.error[0] = 0;
  s->commit_pending = false;
  s->deadline = now_ms() + 30000;
  s->model.generation = (s->model.generation + 1) & (UINT32_MAX >> 2);
  if (!s->model.generation)
    s->model.generation++;
  s3_peer_link(s->model.slot, s->model.generation);
  configure_model(s);
}
static int connect_model(slot_t *s, const ble_addr_t *a, bool pairing,
                         int requested) {
  if (bound_count() > BUDDY_MAX_REMOTES || link_count() >= BUDDY_MAX_REMOTES ||
      (!s->record.peer_id && bound_count() >= BUDDY_MAX_REMOTES))
    return BLE_HS_ENOMEM;
  remember_pair_intent(s, a, requested);
  if (!synced || connecting_slot >= 0 || s->conn != NONE || s->forgetting)
    return BLE_HS_EBUSY;
  if (!stop_scan())
    return BLE_HS_EBUSY;
  prepare_link(s, a, pairing, requested);
  s->connecting = true;
  connecting_slot = s->model.slot;
  uintptr_t token = ((uintptr_t)s->model.generation << 2) | s->model.slot;
  int rc = buddy_link_connect(own_address, a, gap, (void *)token);
  if (rc) {
    fault(s->model.slot, 6, rc);
    s->connecting = false;
    connecting_slot = -1;
    s->retry_at = now_ms() + 1000;
    fail_pair(s, RBP_STATUS_PAIRING_FAILED);
    return rc;
  }
  set_state(s, pairing ? RBP_LINK_PAIRING : RBP_LINK_CONNECTING);
  return 0;
}
static int connect_address(slot_t *s, const ble_addr_t *a, bool pairing) {
  return connect_model(s, a, pairing, -1);
}
static void commit_ready(slot_t *s) {
  if (!s->commit_pending || !s->adapter.ready || s->failed || standalone_busy())
    return;
  if (!s->record.peer_id && bound_count() >= BUDDY_MAX_REMOTES) {
    fail_pair(s, RBP_STATUS_RESOURCE_LIMIT);
    return;
  }
  const buddy_catalog_t *db = buddy_catalog_active();
  if (db && !s->record.peer_id &&
      buddy_catalog_identify(db, s->adapter.map_buf, s->adapter.map_len,
                             s->pending_model, fingerprint_evidence,
                             s) != s->pending_model) {
    fail_pair(s, RBP_STATUS_UNSUPPORTED);
    return;
  }
  record_t *record = &transaction_record; *record = s->record;
  record->version = RECORD_VERSION;
  record->peer_id = s->provisional_id;
  record->address = s->identity;
  if (!record->name[0])
    for (unsigned i = 0; i < CANDIDATES; i++)
      if (buddy_address_equal(&candidates[i].address, &s->target)) {
        snprintf(record->name, sizeof record->name, "%s", candidates[i].name);
        break;
      }
  record->model = s->adapter.legacy.selected   ? 3
                 : s->adapter.unicom.selected ? 2
                                              : 1;
  const buddy_model_t *variant = active_model(s);
  if (variant) {
    if (variant->family != record->model) {
      fail_pair(s, RBP_STATUS_UNSUPPORTED);
      return;
    }
    snprintf(record->model_id, sizeof record->model_id, "%s", variant->id);
    if (!s->record.peer_id) {
      record->map = variant->defaults;
      record->overrides = 0;
    }
    if (!record->definition.id[0]) {
      record->definition = *variant;
      if (db) record->definition.map_crc = rbp_crc32c(s->adapter.map_buf, s->adapter.map_len);
    }
  }
  buddy_bond_t bond;
  if (!buddy_bond_capture(&s->identity,&bond)) {
    fail_pair(s, RBP_STATUS_PAIRING_FAILED);
    return;
  }
  record->cache_len = rc003_adapter_cache_export(
      &s->adapter, record->cache, sizeof record->cache);
  if (!persist_bundle(s->model.slot, record, &bond)) {
    fail_pair(s, RBP_STATUS_STORAGE_FAILED);
    return;
  }
  s->record = *record;
  s->bond = bond;
  s->cache_hash = record->cache_len ? rbp_crc32c(record->cache, record->cache_len) : 0;
  s->cache_checked = true;
  s->migrate_binding = false;
  s3_mapping(s->model.slot, &record->map);
  s->commit_pending = s->pairing = false;
  rc003_adapter_commit_bond(&s->adapter, record->peer_id);
  set_state(s, RBP_LINK_READY);
  complete(s, RBP_STATUS_OK, false);
  changed(s->model.slot);
}
static void identity_ready(s3_gatt_t *g, bool ok) {
  if (g->slot >= SLOTS)
    return;
  slot_t *s = &slots[g->slot];
  if (!ok) {
    fail_pair(s, RBP_STATUS_PAIRING_FAILED);
    return;
  }
  rc003_adapter_start_bound(&s->adapter, now_ms(), s->provisional_id);
}
static void accept_encrypted(slot_t *s, const struct ble_gap_conn_desc *d) {
  if (s->identity_seen) {
    if (!buddy_address_equal(&s->identity, &d->peer_id_addr))
      fail_pair(s, RBP_STATUS_PAIRING_FAILED);
    return; /* Duplicate security events must not restart GATT/MTU/voice. */
  }
  s->identity = d->peer_id_addr;
  s->identity_seen = true;
  int existing = identity_slot(&d->peer_id_addr);
  /* Never commit a duplicate identity into another slot, including resolved
   * RPA. */
  if (existing >= 0 && existing != (int)s->model.slot) {
    slot_t *target = &slots[existing];
    if (target->conn != NONE || target->connecting || target->forgetting) {
      fail_pair(s, RBP_STATUS_DUPLICATE);
      return;
    }
    target->model.generation =
        (target->model.generation + 1) & (UINT32_MAX >> 2);
    if (!target->model.generation)
      target->model.generation++;
    uintptr_t new_token = ((uintptr_t)target->model.generation << 2) | existing;
    int update = ble_gap_set_event_cb(s->conn, gap, (void *)new_token);
    if (update) {
      fault(s->model.slot, 18, update);
      fail_pair(s, RBP_STATUS_DEVICE_ERROR);
      return;
    }
    target->conn = s->conn;
    target->target = s->target;
    target->pairing = s->pairing;
    target->pending_model = s->pending_model;
    target->requested_model = s->requested_model;
    configure_model(target);
    target->failed = false;
    target->deadline = s->deadline;
    if (operation.pending && operation.slot == s->model.slot)
      operation.slot = existing;
    s3_peer_link(s->model.slot, 0);
    s3_gatt_detach(&s->gatt);
    rc003_adapter_detach(&s->adapter);
    s->conn = NONE;
    s->pairing = s->commit_pending = false;
    set_state(s, s->record.peer_id ? RBP_LINK_DISCONNECTED : RBP_LINK_UNBOUND);
    s->identity_seen = false;
    s = target;
    s->identity = d->peer_id_addr;
    s->identity_seen = true;
    s3_peer_link(existing, s->model.generation);
    s3_gatt_attach(&s->gatt, &s->adapter, s->conn);
  }
  if (s->record.peer_id &&
      !buddy_address_equal(&s->record.address, &d->peer_id_addr)) {
    fail_pair(s, s->verifying_identity ? RBP_STATUS_RESOURCE_LIMIT
                                       : RBP_STATUS_PAIRING_FAILED);
    return;
  }
  s->identity = d->peer_id_addr;
  s->provisional_id = s->record.peer_id ? s->record.peer_id : new_peer_id();
  s->commit_pending = s->pairing;
  set_state(s, RBP_LINK_INITIALIZING);
  if (s->pairing && buddy_catalog_active())
    s3_gatt_identify(&s->gatt, identity_ready);
  else
    rc003_adapter_start_bound(&s->adapter, now_ms(), s->provisional_id);
}
