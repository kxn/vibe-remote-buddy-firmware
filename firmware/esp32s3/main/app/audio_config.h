#pragma once
/* Batch eight full-speed bulk packets per CDC transfer. The descriptor's
 * maximum packet remains 64 bytes; this is the software transfer buffer. */
#define CFG_TUD_CDC_TX_EPSIZE 512
/* Extend esp_tinyusb's supported config without editing managed sources. */
#define CFG_TUD_AUDIO 1
#define CFG_TUD_AUDIO_ENABLE_EP_IN 1
#define CFG_TUD_AUDIO_FUNC_1_SAMPLE_RATE 16000
#define CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_TX 2
#define CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX 1
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX 34
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ 512
