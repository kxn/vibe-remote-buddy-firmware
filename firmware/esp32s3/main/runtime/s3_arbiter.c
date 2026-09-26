#include "s3_arbiter.h"
#include <string.h>
void buddy_arbiter_init(buddy_arbiter_t *a){memset(a,0,sizeof *a);a->owner=BUDDY_NO_OWNER;}
static bool current(const buddy_arbiter_t *a,unsigned s,uint32_t g){return s<BUDDY_SLOTS&&g&&a->generation[s]==g;}
void buddy_arbiter_release(buddy_arbiter_t *a,unsigned s,uint32_t g){
 if(!current(a,s,g)||a->owner!=s)return;
 a->owner=BUDDY_NO_OWNER;
 /* A press rejected while busy cannot take over without a new down edge. */
}
void buddy_arbiter_link(buddy_arbiter_t *a,unsigned s,uint32_t g){
 if(s>=BUDDY_SLOTS)return;
 if(a->owner==s)a->owner=BUDDY_NO_OWNER;
 a->generation[s]=g;a->keys[s]=a->suppressed[s]=0;a->media[s]=0;
 a->down[s]=a->rejected[s]=false;
}
bool buddy_arbiter_voice(buddy_arbiter_t *a,unsigned s,uint32_t g,bool down){
 if(!current(a,s,g))return false;
 bool edge=down&&!a->down[s];a->down[s]=down;
 if(!down){a->rejected[s]=false;return a->owner==s;}
 if(a->rejected[s])return false;
 if(!edge)return a->owner==s;
 if(a->owner!=BUDDY_NO_OWNER){a->rejected[s]=true;return false;}
 a->owner=s;
 for(unsigned i=0;i<BUDDY_SLOTS;i++)a->suppressed[i]|=a->keys[i];
 return true;
}
void buddy_arbiter_keys(buddy_arbiter_t *a,unsigned s,uint32_t g,uint64_t keys,uint16_t media){
 if(!current(a,s,g))return;
 a->suppressed[s]&=keys;
 if(a->owner!=BUDDY_NO_OWNER)a->suppressed[s]|=keys;
 a->keys[s]=keys;a->media[s]=media;
}
void buddy_arbiter_keyboard(const buddy_arbiter_t *a,uint8_t out[8]){
 memset(out,0,8);
 if(a->owner!=BUDDY_NO_OWNER){if(a->down[a->owner]&&!a->rejected[a->owner])out[0]=0x40;return;}
 uint64_t keys=0;for(unsigned i=0;i<BUDDY_SLOTS;i++)keys|=a->keys[i]&~a->suppressed[i];
 unsigned n=0;for(unsigned i=0;i<64;i++)if(keys&((uint64_t)1<<i)){
  if(n==6){memset(out+2,1,6);return;}out[2+n++]=(uint8_t)(i+0x28);
 }
}
uint16_t buddy_arbiter_media(const buddy_arbiter_t *a){uint16_t bits=0;for(unsigned i=0;i<BUDDY_SLOTS;i++)bits|=a->media[i];return bits;}
