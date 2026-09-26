#pragma once
#include <stdint.h>
#include <stdbool.h>
#define BUDDY_MAX_REMOTES 2
/* Stable storage/protocol IDs: keep old four-slot bindings deletable on upgrade.
 * This is not the connection or new-binding capacity. */
#define BUDDY_SLOTS 4
#define BUDDY_NO_OWNER 255
/* Pure state machine: caller serializes writes and snapshots on both cores. */
typedef struct {
 uint32_t generation[BUDDY_SLOTS];
 uint8_t owner;
 bool down[BUDDY_SLOTS],rejected[BUDDY_SLOTS];
 uint64_t keys[BUDDY_SLOTS],suppressed[BUDDY_SLOTS];
 uint16_t media[BUDDY_SLOTS];
} buddy_arbiter_t;
void buddy_arbiter_init(buddy_arbiter_t *a);
void buddy_arbiter_link(buddy_arbiter_t *a,unsigned slot,uint32_t generation);
bool buddy_arbiter_voice(buddy_arbiter_t *a,unsigned slot,uint32_t generation,bool down);
void buddy_arbiter_keys(buddy_arbiter_t *a,unsigned slot,uint32_t generation,uint64_t keys,uint16_t media);
void buddy_arbiter_release(buddy_arbiter_t *a,unsigned slot,uint32_t generation);
void buddy_arbiter_keyboard(const buddy_arbiter_t *a,uint8_t out[8]);
uint16_t buddy_arbiter_media(const buddy_arbiter_t *a);
