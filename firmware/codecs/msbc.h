#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "oi_codec_sbc.h"
typedef struct { OI_CODEC_SBC_DECODER_CONTEXT context; OI_CODEC_SBC_CODEC_DATA_MONO data; } buddy_msbc_t;
bool buddy_msbc_init(buddy_msbc_t *s);
bool buddy_msbc_decode(buddy_msbc_t *s,const uint8_t *frame,unsigned n,int16_t *pcm);
