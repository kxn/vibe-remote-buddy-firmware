#include "buddy_bond_storage.h"
#include "rbp/frame.h"
#include <stdlib.h>
#include <string.h>
static const uint8_t magic[8] = {'R','B','N','D',1,0,0,0};
static void put(uint8_t *p, uint64_t v, unsigned n) {
  for (unsigned i=0;i<n;i++) { p[i]=(uint8_t)v;v>>=8; }
}
static uint64_t get(const uint8_t *p,unsigned n) {
  uint64_t v=0;for(unsigned i=0;i<n;i++)v|=(uint64_t)p[i]<<(8*i);return v;
}
static bool sec_valid(const struct ble_store_value_sec *s,const ble_addr_t *a) {
  return s->peer_addr.type==a->type && !memcmp(s->peer_addr.val,a->val,6) &&
    (!s->ltk_present || (s->key_size>=7 && s->key_size<=16));
}
static void pack_sec(uint8_t *p,const struct ble_store_value_sec *s) {
  p[0]=s->key_size;p[1]=s->ltk_present | (s->irk_present<<1) |
    (s->csrk_present<<2) | (s->authenticated<<3) | (s->sc<<4);
  put(p+2,s->ediv,2);put(p+4,s->rand_num,8);
  memcpy(p+12,s->ltk,16);memcpy(p+28,s->irk,16);memcpy(p+44,s->csrk,16);
  put(p+60,s->sign_counter,4);
}
static bool unpack_sec(const uint8_t *p,struct ble_store_value_sec *s,const ble_addr_t *a) {
  if(p[1]&~31u)return false;
  memset(s,0,sizeof *s);s->peer_addr=*a;s->key_size=p[0];
  s->ltk_present=p[1]&1;s->irk_present=(p[1]>>1)&1;s->csrk_present=(p[1]>>2)&1;
  s->authenticated=(p[1]>>3)&1;s->sc=(p[1]>>4)&1;
  s->ediv=(uint16_t)get(p+2,2);s->rand_num=get(p+4,8);
  memcpy(s->ltk,p+12,16);memcpy(s->irk,p+28,16);memcpy(s->csrk,p+44,16);
  s->sign_counter=(uint32_t)get(p+60,4);return sec_valid(s,a);
}
static bool bond_valid(const record_t *r,const buddy_bond_t *b) {
  if(!r->peer_id)return !b->our_present && !b->peer_present;
  return (b->our_present || b->peer_present) &&
    (!b->our_present || sec_valid(&b->our,&r->address)) &&
    (!b->peer_present || sec_valid(&b->peer,&r->address)) &&
    ((b->our_present && b->our.ltk_present) || (b->peer_present && b->peer.ltk_present));
}
uint8_t *buddy_binding_blob_encode(const record_t *r,const buddy_bond_t *b,size_t *n) {
  *n=0;if(!bond_valid(r,b))return NULL;
  char *record=buddy_record_encode(r);if(!record)return NULL;
  size_t len=strlen(record)+1;
  if(len>16384){free(record);return NULL;}
  uint8_t *v=calloc(1,len+146);
  if(v){
    memcpy(v,magic,8);put(v+8,len,4);v[12]=1;
    v[13]=b->our_present | (b->peer_present<<1);
    if(b->our_present)pack_sec(v+14,&b->our);
    if(b->peer_present)pack_sec(v+78,&b->peer);
    memcpy(v+142,record,len);put(v+142+len,rbp_crc32c(v,142+len),4);*n=len+146;
  }
  free(record);return v;
}
static bool decode_blob(const uint8_t *v,size_t n,record_t *r,buddy_bond_t *b,bool *legacy) {
  *legacy=false;memset(b,0,sizeof *b);
  if(!n || n>BUDDY_BINDING_BLOB_MAX)return false;
  if(v[0]=='{'){
    *legacy=true;return buddy_record_decode((const char *)v,n,r);
  }
  if(n<147 || memcmp(v,magic,8) || get(v+8,4)!=n-146 || v[12]!=1 || (v[13]&~3u) ||
     get(v+n-4,4)!=rbp_crc32c(v,n-4) || !buddy_record_decode((const char *)v+142,n-146,r))return false;
  b->our_present=v[13]&1;b->peer_present=(v[13]>>1)&1;
  if(b->our_present && !unpack_sec(v+14,&b->our,&r->address))return false;
  if(b->peer_present && !unpack_sec(v+78,&b->peer,&r->address))return false;
  return bond_valid(r,b);
}
bool buddy_binding_blob_decode(const uint8_t *v,size_t n,record_t *r,buddy_bond_t *b,bool *legacy) {
  record_t *temporary=calloc(1,sizeof *temporary);
  if(!temporary)return false;
  buddy_bond_t bond;bool old=false;
  bool ok=decode_blob(v,n,temporary,&bond,&old);
  if(ok){*r=*temporary;*b=bond;*legacy=old;}
  free(temporary);return ok;
}
bool buddy_bond_capture(const ble_addr_t *a,buddy_bond_t *b) {
  memset(b,0,sizeof *b);struct ble_store_key_sec key={.peer_addr=*a};
  int our=ble_store_read_our_sec(&key,&b->our),peer=ble_store_read_peer_sec(&key,&b->peer);
  if((our && our!=BLE_HS_ENOENT)||(peer && peer!=BLE_HS_ENOENT))return false;
  b->our_present=!our;b->peer_present=!peer;
  return (!our && b->our.ltk_present) || (!peer && b->peer.ltk_present);
}
int buddy_bond_restore(const buddy_bond_t *b) {
  /* Config-store deletion does not remove the controller resolving entry.
   * Replace it explicitly before write_peer_sec adds the committed IRK. */
  if (b->peer_present && b->peer.irk_present) {
    extern int ble_hs_pvcy_remove_entry(uint8_t,const uint8_t *);
    int rc=ble_hs_pvcy_remove_entry(b->peer.peer_addr.type,b->peer.peer_addr.val);
    if (rc && rc!=0x202) return rc; /* HCI unknown identifier: already absent */
  }
  int rc=b->our_present?ble_store_write_our_sec(&b->our):0;
  if(!rc && b->peer_present)rc=ble_store_write_peer_sec(&b->peer);
  return rc;
}
/* ESP-IDF's config store remains the SDK's RAM key database. These explicit
 * integration points disable independent per-peer NVS writes; commit_ready
 * persists both key halves with the complete slot as one atomic NVS blob.
 * Global local-IRK persistence is deliberately separate from peer membership. */
int __wrap_ble_store_config_persist_our_secs(void){return 0;}
int __wrap_ble_store_config_persist_peer_secs(void){return 0;}
int __wrap_ble_store_config_persist_cccds(void){return 0;}
int __wrap_ble_store_config_persist_rpa_recs(void){return 0;}
int __wrap_ble_store_persist_peer_records(void){return 0;}
