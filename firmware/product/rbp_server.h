/* RBP/3.0 product server.
 *
 * Owns the USB protocol session, the logical device model and the one-stream
 * voice delivery policy described in docs/wire-protocol.md.  Hardware
 * independent: the CH582F build wires it to TMOS/BLE/USB, the host simulator
 * wires it to a fake radio; both go through the interfaces below.
 *
 * Ownership: product, central and adapter run on the same cooperative
 * TMOS execution thread (different task IDs, no concurrent task execution).
 * ISRs must not call this API. Backend calls may synchronously report
 * completion, so retire state before calling them. Simulator: single thread.
 * Transport out() only accepts/copies bytes; it must not reenter the server.
 */
#ifndef RBP_SERVER_H
#define RBP_SERVER_H
#include "rbp/audio.h"

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "rbp/defs.h"
#include "rbp/frame.h"
#include "rbp/tlv.h"
#include "device_model.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rbp_server rbp_server_t;

#define RBP_SESSION_IDLE 0u

/* -------- persistent peer record (store owns the flash/memory) -------- */

typedef struct {
    uint32_t peer_id;              /* 0 = none; stable while bound */
    char name[48];
    bool auto_reconnect;
} rbp_peer_record_t;

typedef struct {
    void *user;
    bool (*load)(void *user, rbp_peer_record_t *out);   /* false = none */
    bool (*save)(void *user, const rbp_peer_record_t *rec);
    bool (*clear)(void *user);
} rbp_store_t;

/* -------- radio backend (implemented per platform) -------- */

typedef struct {
    uint32_t candidate_id;
    uint8_t support;      /* 0 possible, 1 model match */
    uint8_t signal;       /* 0 unknown, 1 weak, 2 mid, 3 strong */
    char name[48];
    uint8_t name_len;
} rbp_candidate_t;

typedef struct rbp_radio_backend {
    void *user;
    uint16_t (*start_find)(void *user, uint16_t duration_ms); /* RBP status; OK only if started */
    void (*stop_find)(void *user);
    void (*pair_begin)(void *user, uint32_t candidate_id);
    void (*pair_reply)(void *user, bool accept, bool has_passkey, uint32_t passkey);
    void (*pair_cancel)(void *user);
    void (*forget_peer)(void *user);
    void (*connect_peer)(void *user);
    void (*disconnect)(void *user);
    void (*voice_request_stop)(void *user); /* best-effort mic stop on device */
    /* Queue only; no synchronous callbacks. Return public status.
     * Radio owns bounded pending-start lifetime until START/failure. */
    uint16_t (*voice_request_start)(void *user);
} rbp_radio_backend_t;

/* -------- link state fed by radio layer -------- */

typedef enum {
    RBP_LINK_UNBOUND = 0,
    RBP_LINK_DISCONNECTED,
    RBP_LINK_CONNECTING,
    RBP_LINK_PAIRING,
    RBP_LINK_INITIALIZING,
    RBP_LINK_READY,
    RBP_LINK_UNSUPPORTED,
    RBP_LINK_ERROR,
} rbp_link_state_t;

/* -------- inputs from the adapter -------- */

typedef struct {
    uint32_t connection_id;   /* 0 if no link */
    uint8_t kind;             /* RBP_KEYS_KIND_* */
    uint8_t reason;           /* RBP_KEYS_REASON_* */
    uint64_t pressed_bits;    /* bitmap by slot */
    uint64_t captured_us;
} rbp_keys_report_t;

typedef enum {
    RBP_VOICE_EVT_SOURCE_BEGIN=0,
    RBP_VOICE_EVT_START,
    RBP_VOICE_EVT_ENCODED,
    RBP_VOICE_EVT_FORMAT,
    RBP_VOICE_EVT_FAULT,
    RBP_VOICE_EVT_END,
} rbp_voice_evt_type_t;
typedef struct {
    rbp_voice_evt_type_t type;
    union {
        rbp_audio_format_t start, format;
        struct { const uint8_t *data; uint16_t len; uint32_t unit_size, offset, samples; uint8_t erasures_before; } encoded; /* Local ICO PLC metadata, not wire payload. */
        struct { uint8_t reason; } end;
    } u;
} rbp_voice_evt_t;
/* Capabilities remain owned by the attached adapter until link loss. */
void rbp_server_set_voice_caps(rbp_server_t *s,const rbp_audio_caps_t *caps);

/* -------- server configuration -------- */

typedef struct {
    rbp_radio_backend_t backend;
    rbp_store_t store;
    /* transport sink: encoded frame bytes for the CDC TX path.  Must accept
     * everything eventually; returns bytes accepted (0 = busy, retried). */
    size_t (*out)(void *user, const uint8_t *data, size_t len);
    void *out_user;
    /* deterministic randomness for ids (tmos_rand on target) */
    uint32_t (*rng)(void);
    const rbp_device_profile_t *profile; /* device catalog when adapted */
    uint16_t max_keys;                   /* <= 64 */
    uint32_t max_capture_ms;             /* default 120000 */
    uint8_t bridge_uid[16];              /* stable product identity */
    const char *firmware_version;
    uint16_t reset_reason;               /* GET_STATS reset_reason enum */
} rbp_server_cfg_t;

/* -------- lifecycle -------- */

/* The server object is statically sized; allocate sizeof(rbp_server_t) or
 * use the accessor below.  buf/buf_len are reserved for future dynamic
 * sizing and may be NULL/0. */
size_t rbp_server_buffer_size(void);
#define rbp_server_object_size rbp_server_buffer_size
void rbp_server_init(rbp_server_t *s, const rbp_server_cfg_t *cfg, void *buf, size_t buf_len);

/* Drive periodic work: timeouts, queue flushing, output drain. */
void rbp_server_tick(rbp_server_t *s, uint32_t now_ms);
/* Nonblocking task-context output pump; no timers, radio calls or debug traffic. */
void rbp_server_flush_output(rbp_server_t *s);

/* -------- USB input -------- */

/* Feed raw bytes received on CDC OUT (any chunking; may contain several or
 * partial frames). */
void rbp_server_on_usb_rx(rbp_server_t *s, const uint8_t *data, size_t len, uint32_t now_ms);
/* USB reset/suspend/cable pull: session dies (binding kept). */
void rbp_server_on_usb_gone(rbp_server_t *s, uint32_t now_ms);

/* -------- inputs from radio/adapter -------- */

void rbp_server_on_scan_candidate(rbp_server_t *s, const rbp_candidate_t *cand);
void rbp_server_on_scan_done(rbp_server_t *s, uint8_t reason /* RBP_FIND_DONE_* */,
                             uint32_t now_ms);
void rbp_server_on_pair_prompt(rbp_server_t *s, uint8_t method, uint32_t number,
                               uint32_t remaining_ms);
void rbp_server_on_pair_done(rbp_server_t *s, uint16_t status, bool peer_committed,
                             const rbp_peer_record_t *peer, uint32_t connection_id);
void rbp_server_on_forget_done(rbp_server_t *s, uint16_t status, bool uncertain);
void rbp_server_on_link(rbp_server_t *s, rbp_link_state_t state, uint32_t connection_id,
                        uint32_t now_ms);
void rbp_server_on_link_message(rbp_server_t *s, const char *msg);
void rbp_server_on_peer_changed(rbp_server_t *s, const rbp_peer_record_t *peer);
void rbp_server_on_keys(rbp_server_t *s, const rbp_keys_report_t *rep);
void rbp_server_on_voice(rbp_server_t *s, const rbp_voice_evt_t *ev, uint32_t now_ms);
void rbp_server_on_voice_state(rbp_server_t *s, uint8_t voice_state,
                               uint8_t interaction, uint32_t sample_rate);
void rbp_server_on_battery(rbp_server_t *s, uint8_t level, uint8_t charging);

/* -------- introspection for tests/simulator -------- */

typedef struct {
    bool session_active;
    uint32_t session_id;
    uint32_t connection_id;
    rbp_link_state_t link_state;
    bool events_enabled;
    bool voice_enabled;
    bool waiting_idle;
    uint8_t voice_state;
    uint32_t stream_id;
} rbp_server_info_t;

void rbp_server_set_profile(rbp_server_t *s,const rbp_device_profile_t *profile);
void rbp_server_adapter_failed(rbp_server_t *s,const char *reason);
bool rbp_server_voice_wanted(const rbp_server_t *s);
uint32_t rbp_server_voice_capture_session(const rbp_server_t *s);
bool rbp_server_should_reconnect(const rbp_server_t *s);
void rbp_server_get_info(const rbp_server_t *s, rbp_server_info_t *out);

#ifdef __cplusplus
}
#endif
#endif /* RBP_SERVER_H */
