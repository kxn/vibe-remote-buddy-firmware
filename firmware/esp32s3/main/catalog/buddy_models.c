#include "buddy_match.h"
#include "buddy_models.h"
#include "device_model.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
static bool integer(const cJSON *j, unsigned max, unsigned *out) {
  if (!cJSON_IsNumber(j) || !isfinite(j->valuedouble) || j->valuedouble < 0 ||
      j->valuedouble > max || floor(j->valuedouble) != j->valuedouble)
    return false;
  *out = (unsigned)j->valuedouble;
  return true;
}
static const cJSON *get(const cJSON *j, const char *k) {
  return cJSON_GetObjectItemCaseSensitive(j, k);
}
static bool number(const cJSON *j, const char *k, unsigned max, unsigned *out) {
  return integer(get(j, k), max, out);
}
static bool string(const cJSON *j, char *dst, size_t size) {
  const char *s = cJSON_GetStringValue(j);
  if (!s || !*s || strlen(s) >= size)
    return false;
  strcpy(dst, s);
  return true;
}
static bool fields(const cJSON *j, const char *allowed) {
  if (!cJSON_IsObject(j))
    return false;
  for (const cJSON *a = j->child; a; a = a->next) {
    char token[64];
    if (!a->string || strlen(a->string) > 60)
      return false;
    snprintf(token, sizeof token, "|%s|", a->string);
    if (!strstr(allowed, token))
      return false;
  }
  return true;
}
static bool unique(const cJSON *j) {
  if (cJSON_IsObject(j))
    for (const cJSON *a = j->child; a; a = a->next)
      for (const cJSON *b = a->next; b; b = b->next)
        if (!strcmp(a->string, b->string))
          return false;
  for (const cJSON *a = j ? j->child : NULL; a; a = a->next)
    if (!unique(a))
      return false;
  return true;
}
bool buddy_model_has_key(const buddy_model_t *m, unsigned key) {
  for (unsigned i = 0; i < m->key_count; i++)
    if (m->keys[i] == key)
      return true;
  return false;
}
bool buddy_model_valid(const buddy_model_t *m) {
  if(!m || !memchr(m->id,0,sizeof m->id) || !m->id[0] || !m->revision || m->family<1 || m->family>3 ||
     !m->match_count || m->match_count>8 || !m->key_count || m->key_count>63 || m->raw_count>64)return false;
  for(const char *p=m->id;*p;p++)if(!((*p>='a'&&*p<='z')||(*p>='0'&&*p<='9')||*p=='.'||*p=='-'||*p=='_'))return false;
  for(unsigned i=0;i<m->match_count;i++){
    const buddy_match_t *b=&m->matches[i];
    if(!memchr(b->name,0,sizeof b->name)||!memchr(b->prefix,0,sizeof b->prefix)||!!b->name[0]==!!b->prefix[0]||b->company < -1||b->company>65535)return false;
  }
  uint64_t seen=0;
  for(unsigned i=0;i<m->key_count;i++){
    unsigned k=m->keys[i];if(!k||k>=BUDDY_KEYS||(seen&(UINT64_C(1)<<k))||!buddy_binding_valid(k,m->defaults.key[k])||!memchr(m->labels[k],0,48))return false;
    seen|=UINT64_C(1)<<k;
  }
  if(!(seen&(UINT64_C(1)<<RBP_KEY_VOICE)))return false;
  for(unsigned i=0;i<m->raw_count;i++){
    const buddy_raw_key_t *b=&m->raw[i];
    if(!b->key||b->key>=BUDDY_KEYS||!(seen&(UINT64_C(1)<<b->key))||!b->usage||(b->report!=1&&b->report!=3)||(b->report==1&&b->usage>255))return false;
    for(unsigned j=0;j<i;j++)if(m->raw[j].report==b->report&&m->raw[j].usage==b->usage)return false;
  }
  return true;
}
rbp_device_profile_t buddy_model_profile(const buddy_model_t *m,rbp_key_def_t *keys,char (*labels)[48],const char *id) {
  const rbp_device_profile_t *base=m->family==2?&RBP_PROFILE_UNICOM:&RBP_PROFILE_RC003_VOICE;
  for(unsigned i=0;i<m->key_count;i++) {
    unsigned k=m->keys[i];const char *label=m->labels[k];
    if(!*label){label="Key";for(unsigned j=0;j<base->key_count;j++)if(base->keys[j].key_id==k)label=base->keys[j].name;}
    if(labels){snprintf(labels[i],48,"%s",label);label=labels[i];}
    keys[i]=(rbp_key_def_t){k,label};
  }
  return (rbp_device_profile_t){id,id,1,m->key_count,keys};
}
bool buddy_model_parse(const cJSON *j, buddy_model_t *out) {
  buddy_model_t m = {0};
  unsigned v;
  if (!fields(j, "|schema|id|revision|family|matches|keys|raw|map_crc|") ||
      !unique(j) || !number(j, "schema", 1, &v) || v != 1 ||
      !string(get(j, "id"), m.id, sizeof m.id))
    return false;
  if (!number(j, "revision", UINT32_MAX, &v) || !v)
    return false;
  m.revision = v;
  if (!number(j, "family", 3, &v) || !v)
    return false;
  m.family = v;
  if (!number(j, "map_crc", UINT32_MAX, &v))
    return false;
  m.map_crc = v;
  const cJSON *a = get(j, "matches");
  int n = cJSON_GetArraySize(a);
  if (!cJSON_IsArray(a) || n < 1 || n > 8)
    return false;
  m.match_count = n;
  for (unsigned i = 0; i < m.match_count; i++) {
    const cJSON *r = cJSON_GetArrayItem(a, i);
    buddy_match_t *b = &m.matches[i];
    b->company = -1;
    if (!fields(r, "|name|prefix|company|"))
      return false;
    if (get(r, "name") && !string(get(r, "name"), b->name, sizeof b->name))
      return false;
    if (get(r, "prefix") &&
        !string(get(r, "prefix"), b->prefix, sizeof b->prefix))
      return false;
    if (get(r, "company")) {
      if (!number(r, "company", 65535, &v))
        return false;
      b->company = v;
    }
  }
  a = get(j, "keys");
  n = cJSON_GetArraySize(a);
  if (!cJSON_IsArray(a) || n < 1 || n > 63)
    return false;
  m.defaults.version = BUDDY_MAP_VERSION;
  m.defaults.revision = 1;
  for (int i = 0; i < n; i++) {
    const cJSON *r = cJSON_GetArrayItem(a, i);
    if (!fields(r, "|id|default|") || !number(r, "id", 63, &v) || !v)
      return false;
    unsigned key = v;
    const cJSON *d = get(r, "default");
    unsigned x[3];
    if (!cJSON_IsArray(d) || cJSON_GetArraySize(d) != 3)
      return false;
    for (unsigned k = 0; k < 3; k++)
      if (!integer(cJSON_GetArrayItem(d, k), k == 2 ? 65535 : 255, &x[k]))
        return false;
    buddy_binding_t b = {x[0], x[1], x[2]};
    m.keys[m.key_count++] = key;
    m.defaults.key[key] = b;
  }
  a = get(j, "raw");
  n = cJSON_GetArraySize(a);
  if (!cJSON_IsArray(a) || n > 64)
    return false;
  m.raw_count = n;
  for (int i = 0; i < n; i++) {
    const cJSON *r = cJSON_GetArrayItem(a, i);
    unsigned report, usage, key;
    if (!fields(r, "|report|usage|key|") ||
        !number(r, "report", 255, &report) ||
        !number(r, "usage", 65535, &usage) || !usage ||
        !number(r, "key", 63, &key) || !buddy_model_has_key(&m, key))
      return false;
    m.raw[i] = (buddy_raw_key_t){report, usage, key};
  }
  if(!buddy_model_valid(&m))return false;
  *out = m;
  return true;
}
static bool model_equal(const buddy_model_t *a, const buddy_model_t *b, bool defaults) {
  if (strcmp(a->id, b->id) || (defaults && a->revision != b->revision) ||
      a->map_crc != b->map_crc || a->family != b->family ||
      a->match_count != b->match_count || a->key_count != b->key_count ||
      a->raw_count != b->raw_count)
    return false;
  for (unsigned i = 0; i < a->match_count; i++)
    if (strcmp(a->matches[i].name, b->matches[i].name) ||
        strcmp(a->matches[i].prefix, b->matches[i].prefix) ||
        a->matches[i].company != b->matches[i].company)
      return false;
  for (unsigned i = 0; i < a->key_count; i++) {
    if (a->keys[i] != b->keys[i])
      return false;
    buddy_binding_t x = a->defaults.key[a->keys[i]],
                    y = b->defaults.key[b->keys[i]];
    if (defaults && (x.kind != y.kind || x.modifiers != y.modifiers || x.value != y.value))
      return false;
  }
  for (unsigned i = 0; i < a->raw_count; i++)
    if (a->raw[i].report != b->raw[i].report ||
        a->raw[i].usage != b->raw[i].usage || a->raw[i].key != b->raw[i].key)
      return false;
  return true;
}
bool buddy_model_equal(const buddy_model_t *a, const buddy_model_t *b) { return model_equal(a,b,true); }
bool buddy_model_compatible(const buddy_model_t *a, const buddy_model_t *b) { return model_equal(a,b,false); }
int buddy_model_match_hints(const buddy_model_t *models,unsigned count,const char *name,int company,bool ignore_company) {
 int found=-1;
 for(unsigned i=0;i<count;i++)for(unsigned j=0;j<models[i].match_count;j++){
  const buddy_match_t *m=&models[i].matches[j];
  if(buddy_hint_matches(m->name,m->prefix,m->company,name,company,ignore_company)){
    found=buddy_match_merge(found,(int)i);if(found==-2)return found;
  }
 }
 return found;
}
int buddy_model_match(const buddy_model_t *models,unsigned count,const char *name,int company){return buddy_model_match_hints(models,count,name,company,false);}

/* Historical schema-1 builtins omitted their measured CRC. This immutable
 * migration applies only to their exact IDs, never to arbitrary same-family models. */
static uint32_t effective_crc(const buddy_model_t *m) {
  if(m->map_crc)return m->map_crc;
  if(m->family==1 && !strcmp(m->id,"xiaomi.rc003"))return 0x6bd7daad;
  if(m->family==2 && !strcmp(m->id,"unicom.sample-28"))return 0x8186b163;
  return 0;
}
int buddy_model_resolve(const buddy_model_t *m,unsigned count,uint32_t crc,int selected) {
  if(!crc)return -1;
  if(selected>=0)return (unsigned)selected<count&&m[selected].id[0]&&effective_crc(&m[selected])==crc?selected:-1;
  int found=-1;
  for(unsigned i=0;i<count;i++)if(m[i].id[0]&&effective_crc(&m[i])==crc) {
    if(found>=0)return -2;
    found=i;
  }
  return found;
}
