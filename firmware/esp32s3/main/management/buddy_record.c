#include "buddy_record.h"
#include "buddy_model_wire.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
static bool num(const cJSON *j, const char *key, uint32_t max, uint32_t *v) {
  const cJSON *n = cJSON_GetObjectItemCaseSensitive(j, key);
  if (!cJSON_IsNumber(n) || !isfinite(n->valuedouble) || n->valuedouble < 0 ||
      n->valuedouble > max || floor(n->valuedouble) != n->valuedouble)
    return false;
  *v = (uint32_t)n->valuedouble;
  return true;
}
static void hex(char *out, const uint8_t *in, size_t n) {
  const char *h = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) {
    out[2 * i] = h[in[i] >> 4];
    out[2 * i + 1] = h[in[i] & 15];
  }
  out[2 * n] = 0;
}
static int nib(char c) {
  return c >= '0' && c <= '9'   ? c - '0'
         : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                : -1;
}
static bool unhex(const char *s, uint8_t *p, size_t n) {
  if (!s || strlen(s) != 2 * n)
    return false;
  for (size_t i = 0; i < n; i++) {
    int a = nib(s[2 * i]), b = nib(s[2 * i + 1]);
    if (a < 0 || b < 0)
      return false;
    p[i] = (a << 4) | b;
  }
  return true;
}
char *buddy_record_encode(const record_t *r) {
  if (r->cache_len > sizeof r->cache || !memchr(r->name, 0, sizeof r->name) ||
      !memchr(r->model_id, 0, sizeof r->model_id))
    return NULL;
  cJSON *j = cJSON_CreateObject();
  if (!j)
    return NULL;
  char address[13], cache[2401];
  hex(address, r->address.val, 6);
  hex(cache, r->cache, r->cache_len);
  if (!cJSON_AddNumberToObject(j, "schema",
                               r->definition.id[0] ? RECORD_VERSION : 1) ||
      !cJSON_AddNumberToObject(j, "peer", r->peer_id) ||
      !cJSON_AddNumberToObject(j, "address_type", r->address.type) ||
      !cJSON_AddStringToObject(j, "address", address) ||
      !cJSON_AddStringToObject(j, "name", r->name) ||
      !cJSON_AddNumberToObject(j, "model", r->model) ||
      !cJSON_AddNumberToObject(j, "map_schema", BUDDY_MAP_VERSION) ||
      !cJSON_AddNumberToObject(j, "revision", r->map.revision) ||
      !cJSON_AddStringToObject(j, "model_id", r->model_id) ||
      !cJSON_AddStringToObject(j, "cache", cache))
    goto failed;
  if (!cJSON_AddBoolToObject(j, "cleanup_pending", r->cleanup_pending))
    goto failed;
  if (r->definition.id[0]) {
    uint8_t *bytes = malloc(BUDDY_MODEL_WIRE_MAX);
    char *encoded = malloc(2 * BUDDY_MODEL_WIRE_MAX + 1);
    if (!bytes || !encoded) {
      free(bytes);
      free(encoded);
      goto failed;
    }
    size_t n = buddy_model_pack(&r->definition, bytes, BUDDY_MODEL_WIRE_MAX);
    if (!n) {
      free(bytes);
      free(encoded);
      goto failed;
    }
    hex(encoded, bytes, n);
    bool definition_ok =
        cJSON_AddStringToObject(j, "definition", encoded) != NULL;
    free(bytes);
    free(encoded);
    if (!definition_ok ||
        !cJSON_AddNumberToObject(j, "override_lo", (uint32_t)r->overrides) ||
        !cJSON_AddNumberToObject(j, "override_hi",
                                 (uint32_t)(r->overrides >> 32)))
      goto failed;
  }
  cJSON *keys = cJSON_AddArrayToObject(j, "keys");
  if (!keys)
    goto failed;
  for (unsigned i = 0; i < BUDDY_KEYS; i++) {
    int v[] = {r->map.key[i].kind, r->map.key[i].modifiers,
               r->map.key[i].value};
    cJSON *key = cJSON_CreateIntArray(v, 3);
    if (!key)
      goto failed;
    if (!cJSON_AddItemToArray(keys, key)) {
      cJSON_Delete(key);
      goto failed;
    }
  }
  char *out = cJSON_PrintUnformatted(j);
  cJSON_Delete(j);
  return out;
failed:
  cJSON_Delete(j);
  return NULL;
}
static bool decode_into(const char *text, size_t length, record_t *out) {
  if (!text || !length || length > 16384 || text[length - 1] != 0)
    return false;
  const char *end = NULL;
  cJSON *j = cJSON_ParseWithLengthOpts(text, length, &end, 1);
  record_t *r = out;
  uint8_t *definition_bytes = NULL;
  uint32_t v;
  bool ok = false;
  if (!cJSON_IsObject(j) || end != text + length - 1)
    goto done;
  for (cJSON *a = j->child; a; a = a->next)
    for (cJSON *b = a->next; b; b = b->next)
      if (!strcmp(a->string, b->string))
        goto done;
  if (!num(j, "schema", UINT32_MAX, &v) || (v != 1 && v != RECORD_VERSION))
    goto done;
  r->version = v;
  if (!num(j, "peer", UINT32_MAX, &r->peer_id) ||
      !num(j, "address_type", 3, &v))
    goto done;
  r->address.type = v;
  const cJSON *cleanup = cJSON_GetObjectItemCaseSensitive(j, "cleanup_pending");
  if (cleanup && !cJSON_IsBool(cleanup))
    goto done;
  r->cleanup_pending = cJSON_IsTrue(cleanup);
  if (r->cleanup_pending && r->peer_id)
    goto done;
  if (!unhex(
          cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "address")),
          r->address.val, 6))
    goto done;
  const char *name =
      cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "name"));
  if (!name || strlen(name) >= sizeof r->name)
    goto done;
  strcpy(r->name, name);
  if (!num(j, "model", 3, &v) || (r->peer_id && v == 0))
    goto done;
  r->model = v;
  const char *model_id =
      cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "model_id"));
  if (model_id && *model_id) {
    if (strlen(model_id) >= sizeof r->model_id)
      goto done;
    strcpy(r->model_id, model_id);
  } else if (r->peer_id)
    strcpy(r->model_id, r->model == 2 ? "unicom.hid_ico.v1" : "xiaomi.rc003");
  if (!num(j, "map_schema", UINT32_MAX, &v) || v != BUDDY_MAP_VERSION)
    goto done;
  r->map.version = v;
  if (!num(j, "revision", UINT32_MAX, &r->map.revision) || !r->map.revision)
    goto done;
  cJSON *keys = cJSON_GetObjectItemCaseSensitive(j, "keys");
  if (!cJSON_IsArray(keys) || cJSON_GetArraySize(keys) != BUDDY_KEYS)
    goto done;
  for (unsigned i = 0; i < BUDDY_KEYS; i++) {
    cJSON *key = cJSON_GetArrayItem(keys, i);
    if (!cJSON_IsArray(key) || cJSON_GetArraySize(key) != 3)
      goto done;
    uint32_t v[3];
    for (unsigned k = 0; k < 3; k++) {
      cJSON *n = cJSON_GetArrayItem(key, k);
      if (!cJSON_IsNumber(n) || !isfinite(n->valuedouble) ||
          n->valuedouble < 0 || n->valuedouble > (k == 2 ? 65535 : 255) ||
          floor(n->valuedouble) != n->valuedouble)
        goto done;
      v[k] = n->valuedouble;
    }
    r->map.key[i] = (buddy_binding_t){v[0], v[1], v[2]};
    if (i ? !buddy_binding_valid(i, r->map.key[i]) : (v[0] || v[1] || v[2]))
      goto done;
  }
  const char *cache =
      cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "cache"));
  if (!cache || strlen(cache) > 2400 || strlen(cache) % 2)
    goto done;
  r->cache_len = strlen(cache) / 2;
  if (!unhex(cache, r->cache, r->cache_len))
    goto done;
  if (r->version == 2) {
    const char *s =
        cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "definition"));
    uint32_t lo, hi;
    definition_bytes = malloc(BUDDY_MODEL_WIRE_MAX);
    if (!definition_bytes)
      goto done;
    if (!s || strlen(s) % 2 || strlen(s) > BUDDY_MODEL_WIRE_MAX * 2 ||
        !unhex(s, definition_bytes, strlen(s) / 2) ||
        !buddy_model_unpack(definition_bytes, strlen(s) / 2, &r->definition) ||
        strcmp(r->model_id, r->definition.id) ||
        r->model != r->definition.family ||
        !num(j, "override_lo", UINT32_MAX, &lo) ||
        !num(j, "override_hi", UINT32_MAX, &hi))
      goto done;
    r->overrides = (uint64_t)lo | ((uint64_t)hi << 32);
    uint64_t allowed = 0;
    for (unsigned i = 0; i < r->definition.key_count; i++)
      allowed |= UINT64_C(1) << r->definition.keys[i];
    if (r->overrides & ~allowed)
      goto done;
  }
  ok = true;
done:
  free(definition_bytes);
  cJSON_Delete(j);
  return ok;
}

bool buddy_record_decode(const char *text, size_t length, record_t *out) {
  if (!out)
    return false;
  record_t *value = calloc(1, sizeof *value);
  if (!value)
    return false;
  bool ok = decode_into(text, length, value);
  if (ok)
    *out = *value;
  free(value);
  return ok;
}

/* Catalog-v2 migration omitted the measured HID F5 Voice mapping. Repair only
 * that complete, known defective table; never merge a live catalog into slots. */
bool buddy_record_repair_catalog_voice(record_t *r) {
  static const buddy_raw_key_t old[] = {
    {1,102,1},{1,82,3},{1,81,4},{1,80,5},{1,79,6},{1,40,7},
    {1,241,8},{1,74,9},{1,101,10},{1,53,11},{1,128,12},{1,129,13}
  };
  buddy_model_t *m=&r->definition;
  bool rc003=!strcmp(r->model_id,"xiaomi.rc003");
  bool legacy=!strcmp(r->model_id,"xiaomi.legacy-32ba");
  unsigned family=rc003?1:3, count=rc003?12:11;
  if(!r->peer_id || r->cleanup_pending || (!rc003 && !legacy) || r->model!=family ||
     strcmp(m->id,r->model_id) || m->family!=family ||
     m->map_crc!=(rc003?0x6bd7daad:0x2c56d124) ||
     m->raw_count!=count || m->key_count!=count+1) return false;
  uint64_t keys=0;for(unsigned i=0;i<m->key_count;i++) keys|=UINT64_C(1)<<m->keys[i];
  if(keys!=(rc003?0x3ffe:0x37fe))return false;
  for(unsigned i=0;i<12;i++) {
    if(legacy && old[i].key==11)continue;
    bool found=false;
    for(unsigned j=0;j<count;j++) if(m->raw[j].report==old[i].report &&
      m->raw[j].usage==old[i].usage && m->raw[j].key==old[i].key) found=true;
    if(!found)return false;
  }
  m->raw[m->raw_count++]=(buddy_raw_key_t){1,62,RBP_KEY_VOICE};
  return true;
}
