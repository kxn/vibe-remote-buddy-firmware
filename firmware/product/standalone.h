#ifndef RBP_STANDALONE_H
#define RBP_STANDALONE_H
#include "rbp_server.h"

/* Task context, except render/reset_pcm which the USB ISR also calls. */
#ifdef RBP_MULTI_STANDALONE
void standalone_peer_voice_key(rbp_server_t *server,bool down,uint32_t now);
#else
#define standalone_peer_voice_key(server,down,now) standalone_voice_key(down,now)
#endif
bool standalone_wanted(void);
void standalone_voice(const rbp_voice_evt_t *ev, uint32_t now);
void standalone_keys(const rbp_device_profile_t *profile, uint64_t bits);
void standalone_voice_key(bool down,uint32_t now);
void standalone_abort(void);
void standalone_tick(uint32_t now);
bool standalone_take_stop(void);
bool standalone_busy(void);
void standalone_reset_pcm(void);
uint8_t standalone_render(uint8_t *out); /* <= 34 bytes, always initialized */
/* GET_STATS tag 9, versioned LE u32 words; no audio or key payloads. */
#define STANDALONE_STATS_BYTES 72
void standalone_stats(uint8_t out[STANDALONE_STATS_BYTES]);
#ifdef S3_CODEC_METRICS
/* Diagnostic-only BUDDY_STATS index 15: decoder timing, stack and queue. */
#define STANDALONE_CODEC_METRICS_WORDS 12
void standalone_codec_metrics(uint32_t out[STANDALONE_CODEC_METRICS_WORDS]);
#endif
/* GET_STATS tag 10: HID transport counters and register snapshot. */
#define STANDALONE_USB_STATS_BYTES 64
void standalone_usb_stats(uint8_t out[STANDALONE_USB_STATS_BYTES]);
/* USB transport; locking excludes USB only, never the radio. */
uint32_t standalone_usb_lock(void);
void standalone_usb_unlock(uint32_t irq);
bool standalone_usb_ready(void);
bool standalone_usb_recording(void);
uint32_t standalone_usb_generation(void);
bool standalone_usb_hid(const uint8_t *report, uint8_t len);
void standalone_usb_release(void);

#endif
