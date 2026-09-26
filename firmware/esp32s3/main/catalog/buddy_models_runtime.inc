/* Owned by the BLE task. Published models never move in memory. */
static void model_prepare(unsigned index) {
  buddy_model_t *m=&models[index];
  model_profiles[index]=buddy_model_profile(m,model_keys[index],NULL,m->id);
}
static void models_load(void) {
  if (!models) models = calloc(BUDDY_MODELS, sizeof *models);
  if (!models) abort();
  memset(models, 0, BUDDY_MODELS * sizeof *models);
  memset(model_crc, 0, sizeof model_crc);
  for (unsigned i = 0; i < BUDDY_MODELS; i++) {
    char key[12];
    snprintf(key, sizeof key, "model%u", i);
    size_t n = 0;
    int rc = nvs_get_str(storage, key, NULL, &n);
    char *text = NULL;
    if (rc == ESP_ERR_NVS_NOT_FOUND) {
      if (i >= 2)
        continue;
      text = strdup(buddy_builtin_models[i]);
    } else if (!rc && n && n <= BUDDY_MODEL_BYTES) {
      text = malloc(n);
      if (text && nvs_get_str(storage, key, text, &n)) {
        free(text);
        text = NULL;
      }
    }
    if (!text) {
      fault(255, 40, rc ? rc : ESP_ERR_NO_MEM);
      abort();
    }
    if (i >= 2 && !text[0]) {
      free(text);
      continue;
    }
    cJSON *j = cJSON_Parse(text);
    bool ok = buddy_model_parse(j, &models[i]);
    cJSON_Delete(j);
    if (!ok) {
      free(text);
      fault(255, 40, ESP_ERR_INVALID_VERSION);
      abort();
    }
    model_prepare(i);
    model_crc[i] = rbp_crc32c((uint8_t *)text, strlen(text));
    free(text);
  }
}
/* A current catalog and complete binding snapshots do not depend on legacy NVS. */
static void models_bootstrap(void) {
  if (!buddy_catalog_active()) models_load();
}
static buddy_model_store_t model_store(void){return (buddy_model_store_t){buddy_catalog_active(),models,BUDDY_MODELS};}
static const buddy_model_t *model_at(int index) {
 static buddy_model_t cache;return buddy_model_store_get(model_store(),index,&cache)?&cache:NULL;
}
static int model_match_name(const char *name,int company,bool ignore_company) {
  const buddy_catalog_t *db = buddy_catalog_active();
  if (db) {
    static struct { char name[48]; int company,result; uint32_t generation; bool ignore; } cache[24];
    static unsigned next;
    for (unsigned i=0;i<24;i++)
      if(cache[i].generation==db->generation&&cache[i].company==company&&cache[i].ignore==ignore_company&&!strcmp(cache[i].name,name))return cache[i].result;
    int result=buddy_model_store_match(model_store(), name, company, ignore_company);
    unsigned i=next++%24;snprintf(cache[i].name,sizeof cache[i].name,"%s",name);cache[i].company=company;cache[i].result=result;cache[i].generation=db->generation;cache[i].ignore=ignore_company;
    return result;
  }
  return buddy_model_store_match(model_store(),name,company,ignore_company);
}
static int model_index(const char *id) {return buddy_model_store_find(model_store(),id);}
static const buddy_model_t *slot_model(const slot_t *s) {
  if (s->record.definition.id[0]) return &s->record.definition;
  int i = model_index(s->record.model_id);
  return model_at(i);
}
static const buddy_model_t *active_model(const slot_t *s) {
  if(s->record.peer_id&&s->record.definition.id[0])return &s->record.definition;
  return s->pairing ? model_at(s->pending_model) : slot_model(s);
}
static bool bounded_model_json(const char *s) {
  unsigned depth = 0;
  bool quote = false, escape = false;
  for (; *s; s++) {
    if (quote) {
      if (escape)
        escape = false;
      else if (*s == '\\')
        escape = true;
      else if (*s == '"')
        quote = false;
      continue;
    }
    if (*s == '"')
      quote = true;
    else if (*s == '{' || *s == '[') {
      if (++depth > 8)
        return false;
    } else if (*s == '}' || *s == ']') {
      if (!depth)
        return false;
      depth--;
    }
  }
  return !depth && !quote;
}
static bool model_referenced(unsigned index) {
  for (unsigned i = 0; i < SLOTS; i++) {
    const slot_t *s = &slots[i];
    if (s->record.peer_id && !strcmp(s->record.model_id, models[index].id))
      return true;
    if ((s->conn != NONE || s->connecting || s->pairing) &&
        s->adapter.variant.profile == &model_profiles[index])
      return true;
  }
  return false;
}
static uint16_t models_command(uint16_t op, const cJSON *q, cJSON *j) {
  uint32_t index, offset;
  const char *text;
  if (buddy_catalog_active()) {
    if (op != BUDDY_MODEL_GET) return RBP_STATUS_UNSUPPORTED;
    if (!buddy_u32(q, "index", &index)) return RBP_STATUS_INVALID_ARGUMENT;
    const buddy_model_t *m = model_at((int)index);
    if (!m) return RBP_STATUS_NOT_FOUND;
    cJSON_AddStringToObject(j,"id",m->id);cJSON_AddNumberToObject(j,"revision",m->revision);
    return 0;
  }
  if (op == BUDDY_MODEL_GET) {
    if (!buddy_u32(q, "index", &index) || index >= BUDDY_MODELS)
      return RBP_STATUS_INVALID_ARGUMENT;
    if (!models[index].id[0])
      return RBP_STATUS_NOT_FOUND;
    cJSON_AddStringToObject(j, "id", models[index].id);
    cJSON_AddNumberToObject(j, "crc", model_crc[index]);
    cJSON_AddNumberToObject(j, "revision", models[index].revision);
    return 0;
  }
  if (op == BUDDY_MODEL_BEGIN) {
    if (!buddy_u32(q, "length", &offset) || !offset ||
        offset >= sizeof model_transfer.bytes)
      return RBP_STATUS_INVALID_ARGUMENT;
    model_transfer.length = offset;
    model_transfer.used = 0;
    model_transfer.at = now_ms();
    model_transfer.token = esp_random() | 1;
    cJSON_AddNumberToObject(j, "token", model_transfer.token);
    return 0;
  }
  if (op == BUDDY_MODEL_DELETE) {
    if (!buddy_u32(q, "index", &index) || index < 2 || index >= BUDDY_MODELS)
      return RBP_STATUS_INVALID_ARGUMENT;
    if (!models[index].id[0])
      return RBP_STATUS_NOT_FOUND;
    if (standalone_busy() || pairing_busy() || operation.pending ||
        connecting_slot >= 0)
      return RBP_STATUS_BUSY;
    if (model_referenced(index))
      return RBP_STATUS_BAD_STATE;
    char key[20];
    snprintf(key, sizeof key, "model%u", (unsigned)index);
    int rc = nvs_set_str(storage, key, "");
    if (!rc)
      rc = nvs_commit(storage);
    if (rc) {
      fault(255, 41, rc);
      cJSON_AddNumberToObject(j, "sdk_error", rc);
      return RBP_STATUS_STORAGE_FAILED;
    }
    memset(&models[index], 0, sizeof models[index]);
    model_crc[index] = 0;
    memset(candidates, 0, sizeof candidates);
    scan_epoch++;
    return 0;
  }
  if (!buddy_u32(q, "token", &index) || !model_transfer.token ||
      index != model_transfer.token || now_ms() - model_transfer.at > 10000)
    return RBP_STATUS_BAD_STATE;
  model_transfer.at = now_ms();
  if (op == BUDDY_MODEL_ABORT) {
    model_transfer.token = 0;
    return 0;
  }
  if (op == BUDDY_MODEL_DATA) {
    text = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(q, "hex"));
    if (!buddy_u32(q, "offset", &offset) || offset != model_transfer.used ||
        !text || !strlen(text) || strlen(text) > 384 || strlen(text) % 2)
      return RBP_STATUS_INVALID_ARGUMENT;
    size_t n = strlen(text) / 2;
    if (n > model_transfer.length - model_transfer.used)
      return RBP_STATUS_INVALID_ARGUMENT;
    for (unsigned i = 0; i < n; i++) {
      unsigned v = 0;
      for (unsigned k = 0; k < 2; k++) {
        char c = text[i * 2 + k];
        if (c >= '0' && c <= '9')
          v = v * 16 + c - '0';
        else if (c >= 'a' && c <= 'f')
          v = v * 16 + c - 'a' + 10;
        else
          return RBP_STATUS_INVALID_ARGUMENT;
      }
      model_transfer.bytes[offset + i] = v;
    }
    model_transfer.used += n;
    cJSON_AddNumberToObject(j, "offset", model_transfer.used);
    return 0;
  }
  if (op != BUDDY_MODEL_COMMIT)
    return RBP_STATUS_UNSUPPORTED;
  if (standalone_busy() || operation.pending || pairing_busy() ||
      connecting_slot >= 0)
    return RBP_STATUS_BUSY;
  if (model_transfer.used != model_transfer.length)
    return RBP_STATUS_BAD_STATE;
  model_transfer.bytes[model_transfer.used] = 0;
  if (strlen(model_transfer.bytes) != model_transfer.used ||
      !bounded_model_json(model_transfer.bytes))
    return RBP_STATUS_INVALID_ARGUMENT;
  const char *end;
  cJSON *body = cJSON_ParseWithLengthOpts(model_transfer.bytes,
                                          model_transfer.used + 1, &end, 1);
  buddy_model_t *m = calloc(1, sizeof *m);
  if (!m) {
    cJSON_Delete(body);
    return RBP_STATUS_RESOURCE_LIMIT;
  }
  bool ok = buddy_model_parse(body, m);
  cJSON_Delete(body);
  if (!ok) {
    free(m);
    return RBP_STATUS_INVALID_ARGUMENT;
  }
  int target = model_index(m->id);
  if (target >= 0 && buddy_model_equal(&models[target], m)) {
    free(m);
    model_transfer.token = 0;
    return 0;
  }
  if (target >= 0) {
    if (model_referenced(target) && !buddy_model_compatible(&models[target], m)) {
      free(m);
      return RBP_STATUS_BAD_STATE;
    }
    if (m->revision <= models[target].revision) {
      free(m);
      return RBP_STATUS_VERSION_MISMATCH;
    }
  } else {
    for (unsigned i = 0; i < BUDDY_MODELS; i++)
      if (!models[i].id[0]) {
        target = i;
        break;
      }
  }
  if (target < 0) {
    free(m);
    return RBP_STATUS_RESOURCE_LIMIT;
  }
  char key[20];
  snprintf(key, sizeof key, "model%u", (unsigned)target);
  int rc = nvs_set_str(storage, key, model_transfer.bytes);
  if (!rc)
    rc = nvs_commit(storage);
  if (rc) {
    fault(255, 41, rc);
    cJSON_AddNumberToObject(j, "sdk_error", rc);
    free(m);
    return RBP_STATUS_STORAGE_FAILED;
  }
  models[target] = *m;
  free(m);
  model_prepare(target);
  model_crc[target] =
      rbp_crc32c((uint8_t *)model_transfer.bytes, model_transfer.used);
  model_transfer.token = 0;
  memset(candidates, 0, sizeof candidates);
  scan_epoch++;
  return 0;
}
