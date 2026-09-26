#pragma once
#include "buddy_catalog_query.h"
#include <string.h>
/* One read interface; legacy storage is isolated behind this compatibility adapter. */
typedef struct {const buddy_catalog_t *catalog;const buddy_model_t *legacy;unsigned count;} buddy_model_store_t;
static inline bool buddy_model_store_get(buddy_model_store_t s,int index,buddy_model_t *out){
 if(index<0)return false;
 if(s.catalog)return buddy_catalog_model(s.catalog,index,out);
 if(!s.legacy||(unsigned)index>=s.count||!s.legacy[index].id[0])return false;
 *out=s.legacy[index];return true;
}
static inline int buddy_model_store_find(buddy_model_store_t s,const char *id){
 if(s.catalog)return buddy_catalog_find(s.catalog,id);
 for(unsigned i=0;s.legacy&&i<s.count;i++)if(s.legacy[i].id[0]&&!strcmp(s.legacy[i].id,id))return i;
 return -1;
}
static inline int buddy_model_store_match(buddy_model_store_t s,const char *name,int company,bool ignore){
 return s.catalog?buddy_catalog_match_name(s.catalog,name,company,ignore):s.legacy?buddy_model_match_hints(s.legacy,s.count,name,company,ignore):-1;
}
static inline int buddy_model_store_identify(buddy_model_store_t s,const uint8_t *map,size_t len,uint32_t crc,int selected,buddy_catalog_evidence_fn evidence,void *context){
 return s.catalog?buddy_catalog_identify(s.catalog,map,len,selected,evidence,context):s.legacy?buddy_model_resolve(s.legacy,s.count,crc,selected):-1;
}
