/* ATVV 1.0 voice engine for the Xiaomi RC003 profile.
 *
 * Handles control message parse/build, the audio stream state machine and
 * encoded delivery, and reports adapter-level voice events to the product
 * server. All calls share the cooperative task context. write_tx submits
 * only; incoming controls are dispatched after it returns. Sink data is
 * borrowed for the duration of the callback, never retained by the server.
 * Platform independent: GATT writes go through a small sink.
 */
#ifndef RC003_ATVV_H
#define RC003_ATVV_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "rbp/defs.h"
#include "rbp/audio.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ATVV_MAX_FRAME 512
/* Local policy, not an ATVV deadline. Idle RC003 latency permits listening
 * only every 625 ms. Allow multiple opportunities, not a 1 s reply race. */
#define ATVV_OPEN_WAIT_MS 5000u
/* Local recovery deadline, shorter than the production sink stall timeout. */
#define ATVV_SYNC_WAIT_MS 1500u

typedef struct {
    void *user;
    /* write to ATVV TX characteristic */
    bool (*write_tx)(void *user, const uint8_t *data, uint16_t len);
    /* adapter events out */
    void (*on_source_begin)(void *user);
    void (*on_started)(void *user, const rbp_audio_format_t *format);
    void (*on_encoded)(void *user, const uint8_t *data, uint16_t len);
    void (*on_format)(void *user, const rbp_audio_format_t *format);
    void (*on_ended)(void *user, uint8_t reason /* RBP_END_* */);
    bool (*want_voice)(void *user);
    void (*on_fault)(void *user, uint8_t reason); /* delivery abort, source may still run */
} rc003_atvv_sink_t;

typedef struct {
    uint8_t version_hi, version_lo; /* 0x01,0x00 for 1.0; 0x00,0x04 for 0.4e */
    uint8_t codecs;                 /* bit0 8k, bit1 16k */
    uint8_t interaction;            /* 0 on-request, 1 ptt, 3 htt */
    uint16_t frame_size;
    bool request_dle;
} atvv_caps_t;

typedef struct {
    rc003_atvv_sink_t sink;

    /* negotiated */
    bool caps_valid;
    uint8_t requested_interaction; /* 0 normally; 1 for isolated PTT probing */
    bool rejected_stream;
    atvv_caps_t caps;
    uint32_t sample_rate;
    bool announced;
    uint8_t codec_config[4];

    /* stream state */
    bool stream_active;
    uint8_t remote_stream_id;
    uint16_t frame_bytes;
    bool failed, closing, open_pending, open_queued, close_queued;
    uint32_t last_extend_ms, open_deadline_ms;
    uint16_t frame_no;      /* remote frame counter (big endian on wire) */
    bool decoder_ready;
    bool saw_sync, waiting_sync;
    uint32_t sync_wait_since;
    uint32_t last_audio_ms; /* for extend keepalive + progress */
    uint32_t now_ms;

    /* Verified device profile: announce a fresh 0/0 seed at each START.
     * This is profile configuration, not a previous stream decoder state. */
    bool profile_init_without_sync;
} rc003_atvv_t;

void rc003_atvv_init(rc003_atvv_t *a, const rc003_atvv_sink_t *sink);

/* Build helpers (testable without a link) */
uint16_t atvv_build_get_caps(uint8_t *out /* >=6 */);
uint16_t atvv_build_mic_open(uint8_t *out /* >=2 */);
uint16_t atvv_build_mic_close(uint8_t *out /* >=2 */, uint8_t stream_id);
uint16_t atvv_build_mic_extend(uint8_t *out /* >=2 */, uint8_t stream_id);

/* Control notification from ATVV CTL characteristic.
 * now_ms is the monotonic board time.  Returns false when the message is
 * invalid for the current state (still safe to continue). */
bool rc003_atvv_on_control(rc003_atvv_t *a, const uint8_t *data, uint16_t len);

/* Audio notification from ATVV AUDIO characteristic. */
void rc003_atvv_on_audio(rc003_atvv_t *a, const uint8_t *data, uint16_t len);

/* Periodic housekeeping: MIC_EXTEND keepalive during long streams.
 * Returns true when an EXTEND was due and written. */
bool rc003_atvv_tick(rc003_atvv_t *a, uint32_t now_ms);

/* Ask the remote to stop the current stream (MIC_CLOSE). */
bool rc003_atvv_request_stop(rc003_atvv_t *a);
bool rc003_atvv_request_start(rc003_atvv_t *a);

/* parse-only helpers (exposed for tests) */
bool atvv_parse_caps(const uint8_t *data, uint16_t len, atvv_caps_t *out);

#ifdef __cplusplus
}
#endif
#endif
