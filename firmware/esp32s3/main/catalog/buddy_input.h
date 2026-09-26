#pragma once
#include "device_model.h"
#include "host_os.h"
#include "s3_arbiter.h"
/* Indexed by stable logical key ID, never by HID report bit position. */
#define BUDDY_KEYS 64
#define BUDDY_MAP_VERSION 1
/* disabled, USB keyboard, consumer bitmap index, PTT keyboard, host action */
enum { BM_DISABLED, BM_KEYBOARD, BM_CONSUMER, BM_VOICE, BM_ACTION, BM_VOICE_PRESET, BM_TOGGLE_VOICE_MODE };
typedef struct {
  uint8_t kind, modifiers;
  uint16_t value;
} buddy_binding_t;
typedef struct {
  uint32_t version, revision;
  buddy_binding_t key[BUDDY_KEYS];
} buddy_map_t;
typedef struct {
  buddy_arbiter_t arbiter;
  buddy_map_t map[BUDDY_SLOTS];
  bool management;
  bool voice_mode_toggled[BUDDY_SLOTS];
} buddy_input_t;
void buddy_map_default(buddy_map_t *map);
bool buddy_binding_valid(unsigned key, buddy_binding_t b);
void buddy_input_init(buddy_input_t *input);
uint64_t buddy_input_keys(buddy_input_t *input, unsigned slot,
                          uint32_t generation, uint64_t logical);
buddy_binding_t buddy_input_voice_binding(const buddy_input_t *input, unsigned slot);
void buddy_input_keyboard(const buddy_input_t *input, uint8_t report[8]);
/* Consumer bits: volume+, volume-, mute, play/pause, next, previous, stop,
 * home. */

void buddy_voice_keyboard(buddy_binding_t binding, buddy_host_os_t host, uint8_t report[8]);
