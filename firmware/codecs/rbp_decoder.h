#ifndef RBP_AUDIO_DECODER_H
#define RBP_AUDIO_DECODER_H
#include "rbp/audio.h"
#include "ima_decoder.h"
#include "ico_cleanroom/ico_cleanroom.h"
#ifdef __cplusplus
extern "C" {
#endif
enum {RBP_DECODE_OK=0,RBP_DECODE_UNSUPPORTED=1,RBP_DECODE_CONFIG=2,
      RBP_DECODE_CAPACITY=3,RBP_DECODE_INPUT=4};
enum {RBP_DECODER_ABI_VERSION=3};
/* Caller-owned, no allocation/threads. Reinitialize at each decoder epoch.
 * ABI version/size are explicit; config pointers are never retained. */
typedef struct {
    uint32_t size,abi_version;
    uint32_t max_unit_bytes,sample_rate;
    ima_state_t ima;
    uint8_t ready;
    /* Caller-owned state; reset at every codec epoch. */
    uint32_t codec_id;
    ico_cleanroom_decoder ico_cleanroom;
} rbp_decoder_t;
#if defined(_WIN32) && defined(RBP_DECODER_EXPORTS)
#define RBP_DECODER_API __declspec(dllexport)
#else
#define RBP_DECODER_API
#endif
RBP_DECODER_API int rbp_decoder_init(rbp_decoder_t *decoder,uint32_t size,const rbp_audio_format_t *format);
/* Input is one complete coding unit; capacity/output are int16 sample frames.
 * Failure never partially decodes. The output buffer belongs to the caller. */
RBP_DECODER_API int rbp_decoder_decode(rbp_decoder_t *decoder,const uint8_t *encoded,uint32_t bytes,
                       int16_t *pcm,uint32_t capacity,uint32_t *written);
/* One known missing ICO unit (20 ms); unsupported for continuous IMA. */
RBP_DECODER_API int rbp_decoder_conceal(rbp_decoder_t *decoder,int16_t *pcm,uint32_t capacity,uint32_t *written);
#ifdef __cplusplus
}
#endif
#endif
