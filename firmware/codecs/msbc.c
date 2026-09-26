#include "msbc.h"
#include <string.h>
bool buddy_msbc_init(buddy_msbc_t *s) {
 memset(s,0,sizeof *s);
 return OI_CODEC_SBC_DecoderReset(&s->context,s->data.data,sizeof s->data,1,1,0,1)==OI_OK;
}
bool buddy_msbc_decode(buddy_msbc_t *s,const uint8_t *frame,unsigned n,int16_t *pcm) {
 if(!frame || n!=57 || frame[0]!=0xad || frame[1] || frame[2])return false;
 OI_UINT32 len=n,bytes=240;const OI_BYTE *p=frame;
 return OI_CODEC_SBC_DecodeFrame(&s->context,&p,&len,pcm,&bytes)==OI_OK && len==0 && bytes==240;
}
