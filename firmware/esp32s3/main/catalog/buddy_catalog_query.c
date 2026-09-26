#include "buddy_match.h"
#include "buddy_catalog_query.h"
#include "buddy_model_wire.h"
#include <stdlib.h>
#include <string.h>
bool buddy_catalog_model(const buddy_catalog_t *c, uint32_t index,
                         buddy_model_t *out) {
  buddy_catalog_entry_t e;
  if (!buddy_catalog_entry(c, index, &e) || e.length > BUDDY_MODEL_WIRE_MAX)
    return false;
  uint8_t *bytes = malloc(BUDDY_MODEL_WIRE_MAX);
  if (!bytes)
    return false;
  size_t length;
  bool ok = buddy_catalog_model_bytes(c, &e, bytes, BUDDY_MODEL_WIRE_MAX, &length) &&
            buddy_model_unpack(bytes, length, out);
  free(bytes);
  return ok;
}
int buddy_catalog_find(const buddy_catalog_t *c, const char *id) {
  if (!c || !id)
    return -1;
  uint32_t hash = buddy_catalog_hash(id, strlen(id)), lo = 0,
           hi = c->model_count;
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2;
    buddy_catalog_entry_t e;
    if (!buddy_catalog_entry(c, mid, &e))
      return -1;
    if (e.hash < hash)
      lo = mid + 1;
    else
      hi = mid;
  }
  for (; lo < c->model_count; lo++) {
    buddy_catalog_entry_t e;
    uint8_t prefix[59];
    if (!buddy_catalog_entry(c, lo, &e) || e.hash != hash)
      break;
    size_t n = e.length < sizeof prefix ? e.length : sizeof prefix;
    if (n < 11 || !buddy_catalog_object(c, e.offset, prefix, n))
      return -1;
    unsigned length = prefix[10];
    if (length == strlen(id) && length <= n - 11 &&
        !memcmp(prefix + 11, id, length))
      return (int)lo;
  }
  return -1;
}
#include "buddy_catalog_keys.inc"
static cJSON *compact_value(const uint8_t **cursor, const uint8_t *end, unsigned depth, unsigned *budget) {
  if (!depth || !*budget || *cursor >= end) return NULL;
  --*budget;
  unsigned type=*(*cursor)++;
  if (type==0) {
    if (end-*cursor<4) return NULL;
    const uint8_t *p=*cursor; *cursor+=4;
    return cJSON_CreateNumber((uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24));
  }
  if (type>5 || end-*cursor<2) return NULL;
  unsigned n=(*cursor)[0]|((unsigned)(*cursor)[1]<<8); *cursor+=2;
  if (type==5) {
    if(n>32 || (size_t)(end-*cursor)<2u*n) return NULL;
    cJSON *array=cJSON_CreateArray(); if(!array)return NULL;
    for(unsigned i=0;i<n;i++) {
      unsigned id=*(*cursor)++, kind=*(*cursor)++;
      cJSON *item=cJSON_CreateObject();
      if(kind<1 || kind>3 || !item) {cJSON_Delete(item);cJSON_Delete(array);return NULL;}
      if(!cJSON_AddNumberToObject(item,"id",id) || !cJSON_AddNumberToObject(item,"type",kind) || !cJSON_AddItemToArray(array,item)) {cJSON_Delete(item);cJSON_Delete(array);return NULL;}
    }
    return array;
  }
  if (type==1 || type==4) {
    if ((size_t)(end-*cursor)<n || n>4096 || (type==1 && memchr(*cursor,0,n))) return NULL;
    char *text=malloc((type==4?2u*n:n)+1);
    if (!text) return NULL;
    if (type==4) { const char *hex="0123456789abcdef"; for(unsigned i=0;i<n;i++){text[2*i]=hex[(*cursor)[i]>>4];text[2*i+1]=hex[(*cursor)[i]&15];} }
    else memcpy(text,*cursor,n);
    text[type==4?2u*n:n]=0; *cursor+=n;
    cJSON *v=cJSON_CreateString(text); free(text); return v;
  }
  if (n>*budget) return NULL;
  cJSON *v=type==2?cJSON_CreateArray():cJSON_CreateObject();
  if (!v) return NULL;
  for(unsigned i=0;i<n;i++) {
    const char *key=NULL;
    if(type==3) {
      if(*cursor>=end || **cursor>=sizeof(compact_keys)/sizeof(compact_keys[0])) goto bad;
      key=compact_keys[*(*cursor)++];
      if(cJSON_GetObjectItemCaseSensitive(v,key)) goto bad;
    }
    cJSON *child=compact_value(cursor,end,depth-1,budget);
    if(!child) goto bad;
    bool ok=type==2?cJSON_AddItemToArray(v,child):cJSON_AddItemToObject(v,key,child);
    if(!ok){cJSON_Delete(child);goto bad;}
  }
  return v;
bad:cJSON_Delete(v);return NULL;
}
static cJSON *object_json(const buddy_catalog_t *c, uint32_t at) {
  uint8_t b[4];
  if (!buddy_catalog_object(c, at, b, 4))
    return NULL;
  uint32_t n = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
               ((uint32_t)b[3] << 24);
  if (!n || n > 8192)
    return NULL;
  char *text = malloc(n + 1);
  if (!text)
    return NULL;
  cJSON *j = NULL;
  if (buddy_catalog_object(c, at + 4, text, n)) {
    text[n] = 0;
    const char *end = text;
    if (c->format == 2) {
      const uint8_t *cursor=(const uint8_t *)text; unsigned budget=256;
      j=compact_value(&cursor,cursor+n,8,&budget); end=(const char *)cursor;
    } else j = cJSON_ParseWithLengthOpts(text, n + 1, &end, 1);
    if (end != text + n) {
      cJSON_Delete(j);
      j = NULL;
    }
  }
  free(text);
  return j;
}
int buddy_catalog_match_name(const buddy_catalog_t *c, const char *name,
                             int company, bool ignore_company) {
  if (!c || !name || !name[0])
    return -1;
  size_t n = strlen(name);
  if (n >= 48)
    return -1;
  int found = -1;
  /* Exact and prefix hints use the same sorted index. At most 47 digest
   * searches, independent of catalog size; callers cache unchanged adverts. */
  for (size_t length = 1; length <= n; length++) {
    uint32_t hash = buddy_catalog_hash(name, length),
             i = buddy_catalog_lower_bound(c, false, hash);
    for (; i < c->hint_count; i++) {
      buddy_catalog_lookup_t e;
      if (!buddy_catalog_lookup(c, false, i, &e) || e.hash != hash)
        break;
      cJSON *j = object_json(c, e.offset);
      if (!j)
        continue;
      const char *exact =
          cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "name"));
      const char *prefix =
          cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "prefix"));
      const cJSON *vendor = cJSON_GetObjectItemCaseSensitive(j, "company");
      bool match = (!vendor || cJSON_IsNumber(vendor)) && buddy_hint_matches(exact,prefix,vendor?(int)vendor->valuedouble:-1,name,company,ignore_company);
      cJSON_Delete(j);
      if (match) {
        if (found >= 0 && found != (int)e.model)
          return -2;
        found = (int)e.model;
      }
    }
  }
  return found;
}
static int nib(char c) {
  return c >= '0' && c <= '9'   ? c - '0'
         : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                : -1;
}
int buddy_catalog_identify(const buddy_catalog_t *c, const uint8_t *map,
                           size_t n, int selected,
                           buddy_catalog_evidence_fn evidence, void *context) {
  if (!c || !map || !n)
    return -1;
  uint32_t hash = buddy_catalog_hash(map, n),
           i = buddy_catalog_lower_bound(c, true, hash);
  int found = -1;
  for (; i < c->fingerprint_count; i++) {
    buddy_catalog_lookup_t e;
    if (!buddy_catalog_lookup(c, true, i, &e) || e.hash != hash)
      break;
    if (selected >= 0 && e.model != (uint32_t)selected)
      continue;
    cJSON *j = object_json(c, e.offset);
    const cJSON *m = cJSON_GetObjectItemCaseSensitive(j, "report_map");
    const char *hex =
        cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m, "hex"));
    bool match = hex && strlen(hex) == n * 2;
    for (size_t k = 0; match && k < n; k++) {
      int a = nib(hex[k * 2]), b = nib(hex[k * 2 + 1]);
      match = a >= 0 && b >= 0 && map[k] == (uint8_t)((a << 4) | b);
    }
    if (match && evidence)
      match = evidence(context, j);
    cJSON_Delete(j);
    if (match) {
      if (found >= 0 && found != (int)e.model)
        return -2;
      found = (int)e.model;
    }
  }
  return found;
}
int buddy_catalog_match_map(const buddy_catalog_t *c, const uint8_t *map,
                            size_t n, int selected) {
  return buddy_catalog_identify(c, map, n, selected, NULL, NULL);
}

bool buddy_catalog_validate_lookups(const buddy_catalog_t *c) {
  for(unsigned type=0;type<2;type++) {
    unsigned count=type?c->fingerprint_count:c->hint_count;
    for(unsigned i=0;i<count;i++) {
      buddy_catalog_lookup_t e;
      if(!buddy_catalog_lookup(c,type!=0,i,&e)) return false;
      cJSON *j=object_json(c,e.offset); bool ok=cJSON_IsObject(j);
      if(type) {
        const cJSON *m=cJSON_GetObjectItemCaseSensitive(j,"report_map");
        const char *hex=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m,"hex"));
        const cJSON *len=cJSON_GetObjectItemCaseSensitive(m,"length");
        size_t n=hex?strlen(hex)/2:0;
        ok=ok&&hex&&n&&n<=4096&&strlen(hex)==2*n&&cJSON_IsNumber(len)&&len->valuedouble==n;
        uint8_t *map=ok?malloc(n):NULL; ok=ok&&map;
        for(size_t k=0;ok&&k<n;k++){int a=nib(hex[2*k]),b=nib(hex[2*k+1]);ok=a>=0&&b>=0;if(ok)map[k]=(a<<4)|b;}
        if(ok)ok=buddy_catalog_hash(map,n)==e.hash;
        free(map);
      } else {
        const char *name=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j,"name"));
        const char *prefix=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j,"prefix"));
        const char *text=name?name:prefix;
        ok=ok&&((name!=NULL)!=(prefix!=NULL))&&text&&strlen(text)>0&&strlen(text)<48&&buddy_catalog_hash(text,strlen(text))==e.hash;
      }
      cJSON_Delete(j); if(!ok)return false;
    }
  }
  return true;
}
