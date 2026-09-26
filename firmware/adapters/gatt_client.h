/* Abstract GATT client interface used by device adapters.
 *
 * The CH582F build maps this onto the WCH BLE library procedures; the host
 * simulator maps it onto a fake ATT server so the whole discovery and voice
 * path is testable without hardware.
 *
 * All calls are non-blocking: they start a procedure and return 0, or return
 * a negative error immediately.  Exactly ONE procedure may be in flight at a
 * time (the adapter serializes).  Results are delivered through
 * rbp_gatt_events (dispatched by the platform layer into the same task).
 */
#ifndef RBP_GATT_CLIENT_H
#define RBP_GATT_CLIENT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Immediate submission result: RETRY means NOT accepted by the stack.
 * Retry only that submission, never a write already accepted with OK. */
#define RBP_GATT_RETRY (-2)

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RBP_GATT_EVT_MTU_UPDATED = 0,   /* mtu */
    RBP_GATT_EVT_SERVICE_FOUND,     /* svc_start, svc_end (by-uuid search) */
    RBP_GATT_EVT_SERVICE_NOT_FOUND,
    RBP_GATT_EVT_CHARS_FOUND,       /* chars via read-by-type; iterated */
    RBP_GATT_EVT_DESC_FOUND,        /* desc_handle, uuid16 (iterated) */
    RBP_GATT_EVT_READ_RSP,          /* value/len (chunk of long read) */
    RBP_GATT_EVT_WRITE_DONE,
    RBP_GATT_EVT_NOTIFY,            /* value/len + value_handle */
    RBP_GATT_EVT_PROC_DONE,         /* procedure complete (last response) */
    RBP_GATT_EVT_PROC_ERROR,        /* status; procedure failed */
    RBP_GATT_EVT_BEARER_FAILED,     /* transaction state untrustworthy; disconnect */
} rbp_gatt_evt_type_t;

typedef struct {
    rbp_gatt_evt_type_t type;
    uint16_t handle;      /* value handle (notify/read) */
    uint16_t svc_start;
    uint16_t svc_end;
    uint16_t desc_handle;
    uint16_t uuid16;      /* char/desc uuid when 16-bit */
    uint16_t mtu;
    uint16_t offset;      /* long read offset of this chunk */
    uint8_t status;       /* ATT error code for PROC_ERROR */
    bool proc_complete;   /* for iterated responses: last one */
    const uint8_t *value;
    uint16_t len;
} rbp_gatt_evt_t;

typedef struct rbp_gatt_client {
    void *user;
    int (*exchange_mtu)(void *user, uint16_t client_rx_mtu);
    int (*disc_service_by_uuid16)(void *user, uint16_t uuid16);
    /* 128-bit UUIDs are little-endian on air (uuid[0] = LSB). */
    int (*disc_service_by_uuid128)(void *user, const uint8_t *uuid16le);
    int (*read_chars_by_uuid16)(void *user, uint16_t start, uint16_t end, uint16_t uuid16);
    int (*read_chars_by_uuid128)(void *user, uint16_t start, uint16_t end,
                                 const uint8_t *uuid16le);
    int (*disc_char_descs)(void *user, uint16_t start, uint16_t end);
    int (*read_value)(void *user, uint16_t handle);
    int (*read_long_value)(void *user, uint16_t handle, uint16_t offset);
    int (*write_value)(void *user, uint16_t handle, const uint8_t *val, uint16_t len);
    /* Accepted command has no ATT response/completion event. */
    int (*write_command)(void *user, uint16_t handle, const uint8_t *val, uint16_t len);
} rbp_gatt_client_t;

/* 16-bit UUIDs we care about */
#define RBP_UUID16_HID_SERVICE            0x1812u
#define RBP_UUID16_BATTERY_SERVICE        0x180Fu
#define RBP_UUID16_DEVICE_INFO_SERVICE    0x180Au
#define RBP_UUID16_ATVV_SERVICE           0xF617u /* 128-bit base AB5E0001-...-AF01F617B664; matched by full uuid in platform */
#define RBP_UUID16_HID_INFORMATION        0x2A4Au
#define RBP_UUID16_HID_REPORT_MAP         0x2A4Bu
#define RBP_UUID16_HID_CONTROL_POINT      0x2A4Cu
#define RBP_UUID16_HID_REPORT             0x2A4Du
#define RBP_UUID16_PROTOCOL_MODE          0x2A4Eu
#define RBP_UUID16_BATTERY_LEVEL          0x2A19u
#define RBP_UUID16_REPORT_REFERENCE       0x2908u
#define RBP_UUID16_CCCD                   0x2902u

#ifdef __cplusplus
}
#endif
#endif
