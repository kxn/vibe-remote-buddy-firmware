/* Xiaomi RC003 device adapter: HOGP discovery, key decoding, ATVV wiring.
 *
 * The adapter owns the GATT discovery state machine (one procedure in
 * flight, bounded timeouts) and translates HID + ATVV notifications into
 * product-server inputs.  Platform independent via rbp_gatt_client_t.
 */
#ifndef RC003_ADAPTER_H
#define RC003_ADAPTER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "../gatt_client.h"
#include "../hogp/report_map.h"
#include "rc003_atvv.h"
#include "rbp_server.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RC003_MAX_REPORT_CHARS 32
#ifndef RBP_ATT_MTU
#define RBP_ATT_MTU 23
#endif
#if RBP_ATT_MTU < 23 || RBP_ATT_MTU > 247
#error RBP_ATT_MTU must be between 23 and 247
#endif

typedef struct {
    uint16_t value_handle;   /* report characteristic value handle */
    uint16_t cccd_handle;    /* 0 when not found */
    uint16_t ref_handle;     /* report reference descriptor handle */
    uint8_t report_id;
    uint8_t report_type;     /* 1 = input */
    uint8_t properties;
    uint8_t usage_page;      /* from report map when known */
    bool subscribed;
} rc003_report_char_t;

typedef struct rc003_adapter rc003_adapter_t;

typedef struct {
    bool (*resolve)(rc003_adapter_t *, uint32_t map_crc);
    const rbp_device_profile_t *profile;
    uint8_t family, raw_count;
    uint32_t map_crc;
    struct {uint8_t report;uint16_t usage;uint8_t key;} raw_keys[64];
} rc003_variant_t;

struct rc003_adapter {
    const rbp_gatt_client_t *gatt;
    void *gatt_user;
    rbp_server_t *server;

    rc003_variant_t variant; /* immutable model configuration; retained across session resets */

    /* discovery state */
    uint16_t negotiated_mtu;
    uint8_t state;
    uint8_t retry;
    uint32_t deadline_ms;
    uint32_t init_deadline_ms;
    uint16_t svc_start, svc_end;
    uint16_t protocol_mode_handle;
    uint16_t hid_info_handle;
    uint8_t hid_flags;             /* HIDS: bit 0 remote wake, bit 1 normally connectable */
    bool hid_info_valid;
    uint16_t map_value_handle;
    uint16_t map_len;
    uint8_t map_buf[HOGP_MAX_MAP_SIZE];
    hogp_report_map_t map;

    rc003_report_char_t chars[RC003_MAX_REPORT_CHARS];
    uint8_t char_count;
    int8_t cur_char;               /* char being discovered/subscribed */

    uint16_t bat_value_handle, bat_cccd_handle;
    bool atvv_tx_request, atvv_write_pending;
    uint32_t atvv_write_deadline;
    uint16_t atvv_tx_handle, atvv_ctl_handle, atvv_ctl_cccd, atvv_audio_handle,
        atvv_audio_cccd;
    bool atvv_present;

    bool ready;                    /* keys usable */
    bool voice_ready;
    bool rc003_boot_layout;        /* exact observed Map/profile, not length guessing */

    uint64_t keys_by_report[RC003_MAX_REPORT_CHARS];
    bool map_long_started, desc_boundary, voice_down_seen, voice_key_verified;
    uint16_t service_changed_handle, service_changed_cccd;
    uint64_t pressed_bits;

    /* Only this measured Unicom profile; discovery buffers are shared. */
    struct {
        bool selected, hello, active, started, closing, previous_valid, down, pending_voice, announced;
        uint8_t part, command, in_flight;
        uint16_t fb, fc, f8, fd, fd_cccd, sequence, previous;
        uint32_t command_deadline, refresh_ms, last_audio_ms, close_ms;
        uint8_t body[48];
        uint8_t pre_loss[8];
        uint8_t preroll[320], pre_head, pre_count; /* 8 complete ICO units / 160ms */
        uint32_t capture_session, capture_ms;
    } unicom;
    struct {
        bool selected, down, active, started, closing, sequence_valid;
        uint8_t command, in_flight, sequence, end_reason;
        uint16_t control;
        uint32_t deadline, last_audio, close_ms, capture_session, command_ms;
    } legacy;
    rc003_atvv_t atvv;
    uint32_t now_ms;

    uint8_t failed_stage;          /* for diagnostics */
    uint8_t last_att_error;        /* retained until next initialization step */
    uint32_t cache_peer_id;
    bool cache_safe, cache_valid, restoring, pending_search, caps_requested;
};

void rc003_adapter_init(rc003_adapter_t *a, const rbp_gatt_client_t *gatt, void *gatt_user,
                        rbp_server_t *server);

/* candidate matching for scan results; returns support level
 * (0 = no evidence, 1 = model match). */
uint8_t rc003_adapter_match(const char *name, uint8_t name_len, bool hid_uuid_in_adv);

/* start async initialization on a fresh link */
void rc003_adapter_start(rc003_adapter_t *a, uint32_t now_ms);
/* Only after restoring encryption for this committed bond; zero disables cache. */
void rc003_adapter_start_bound(rc003_adapter_t *a, uint32_t now_ms, uint32_t peer_id);

/* GATT event pump from the platform layer */
void rc003_adapter_on_gatt(rc003_adapter_t *a, const rbp_gatt_evt_t *evt);

/* periodic housekeeping (timeouts, MIC_EXTEND) */
void rc003_adapter_tick(rc003_adapter_t *a, uint32_t now_ms);

/* link is gone: reset state */
void rc003_adapter_detach(rc003_adapter_t *a);
/* Called only after this link's new bond has been durably committed. */
void rc003_adapter_commit_bond(rc003_adapter_t *a,uint32_t peer_id);

size_t rc003_adapter_cache_export(const rc003_adapter_t *a,uint8_t *data,size_t cap);
bool rc003_adapter_cache_import(rc003_adapter_t *a,uint32_t peer,const uint8_t *data,size_t len);

/* best-effort: send ATVV MIC_CLOSE for the active stream; false if the
 * ATVV channel is not up (client then falls back to the local timeout) */
bool rc003_adapter_mic_stop(rc003_adapter_t *a);
uint16_t rc003_adapter_mic_start(rc003_adapter_t *a);

#ifdef __cplusplus
}
#endif
#endif
