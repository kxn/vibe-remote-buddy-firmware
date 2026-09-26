#pragma once
#include "cJSON.h"
#include "rbp/frame.h"
/* RBP/3 framing, Buddy management API 1. No legacy single-peer emulation. */
enum {
  BUDDY_HELLO = 0x400,
  BUDDY_PING,
  BUDDY_CLOSE,
  BUDDY_INFO,
  BUDDY_SLOT,
  BUDDY_SCAN,
  BUDDY_CANDIDATE,
  BUDDY_PAIR,
  BUDDY_UNBIND,
  BUDDY_OPERATION,
  BUDDY_CATALOG,
  BUDDY_MAP_GET,
  BUDDY_MAP_SET,
  BUDDY_MAP_RESET,
  BUDDY_STATS,
  BUDDY_CANCEL,
  BUDDY_SCAN_STOP,
  BUDDY_RETRY,
  BUDDY_MODEL_GET = 0x430,
  BUDDY_MODEL_BEGIN,
  BUDDY_MODEL_DATA,
  BUDDY_MODEL_COMMIT,
  BUDDY_MODEL_ABORT,
  BUDDY_MODEL_DELETE,
  BUDDY_PROBE_BEGIN = 0x440,
  BUDDY_PROBE_END,
  BUDDY_PROBE_STATUS,
  BUDDY_PROBE_CONNECT,
  BUDDY_PROBE_SECURITY,
  BUDDY_PROBE_DISCOVER,
  BUDDY_PROBE_ATTR,
  BUDDY_PROBE_READ,
  BUDDY_PROBE_SUBSCRIBE,
  BUDDY_PROBE_REPORT,
  BUDDY_PROBE_VOICE_ARM,
  BUDDY_PROBE_VOICE_STATUS,
  BUDDY_PROBE_VOICE_READ,
  BUDDY_PROBE_VOICE_CANCEL,
  BUDDY_PROBE_RAW_WRITE, /* diagnostic build only */
  BUDDY_PROBE_MTU, /* diagnostic build only */
  BUDDY_PROBE_ADOPT = 0x450,
  BUDDY_ACTION = 0x480,
  BUDDY_CHANGED,
  BUDDY_OPERATION_EVENT,
  BUDDY_PROBE_AUDIO
};
void buddy_management_init(void);
void buddy_management_rx(const uint8_t *data, size_t len, uint32_t now);
void buddy_management_tick(uint32_t now);
void buddy_management_close(void);
bool buddy_management_active(void);
void buddy_management_event(uint16_t opcode, cJSON *body); /* consumes body */
bool buddy_management_audio(cJSON *body); /* consumes body; reserves control capacity */
uint16_t buddy_command(uint16_t opcode, const cJSON *request, cJSON *response);
/* Strict unsigned integer validation: no coercion, booleans or fractions. */
bool buddy_u32(const cJSON *obj, const char *key, uint32_t *out);
