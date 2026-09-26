#ifndef RBP_AUDIO_H
#define RBP_AUDIO_H
#include <stdint.h>
#define RBP_CODEC_IMA_HI 1u
#define RBP_CODEC_IFLYTEK_ICO 2u
#define RBP_CODEC_MSBC 3u
#define RBP_AUDIO_CONFIG_MAX 256u
#define RBP_AUDIO_UNIT_MAX 65536u
#define RBP_AUDIO_DATA_HEADER 40u
#define RBP_AUDIO_FRAGMENT_MAX 472u
typedef struct { uint32_t id; uint16_t revision; } rbp_codec_t;
typedef struct {
    const rbp_codec_t *codecs;
    uint32_t max_unit_bytes;
    uint8_t count;
} rbp_audio_caps_t;
typedef struct {
    uint32_t codec_id, sample_rate, max_unit_bytes;
    uint16_t codec_revision, config_len;
    uint8_t channels;
    const uint8_t *config; /* borrowed during the callback; server copies */
} rbp_audio_format_t;
#endif
