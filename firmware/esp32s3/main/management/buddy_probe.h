#pragma once
#include "buddy_management.h"
#include "host/ble_hs.h"
bool buddy_probe_active(void);
/* Binding records, not the SDK bond table, own device membership. */
bool buddy_identity_is_bound(const ble_addr_t *address);
bool buddy_probe_link_busy(void);
bool buddy_probe_scanning(void);
void buddy_probe_tick(uint32_t now, bool managed);
void buddy_probe_reset(void);
uint16_t buddy_probe_command(uint16_t op, const cJSON *q, cJSON *j);
int buddy_probe_connect(uint8_t own, const ble_addr_t *address);

void buddy_probe_voice_detach(void);
void buddy_probe_voice_tick(uint32_t now);
void buddy_probe_voice_notify(uint16_t handle,const uint8_t *data,unsigned len);
void buddy_probe_voice_sdk(uint16_t connection,int code);
uint16_t buddy_probe_voice_command(uint16_t op,const cJSON *q,cJSON *j,uint16_t conn);

/* Same encrypted connection only; immutable HID metadata, never stream/control state. */
bool buddy_probe_cached_value(uint16_t conn, uint16_t handle, const uint8_t **data, uint16_t *len);

/* Atomically transfer an idle encrypted link; caller installs the production owner. */
int buddy_probe_take(int (*callback)(struct ble_gap_event *, void *), void *arg, struct ble_gap_conn_desc *out);
