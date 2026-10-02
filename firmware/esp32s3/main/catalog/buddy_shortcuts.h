#pragma once
#include "host_os.h"

#define BUDDY_SHORTCUT_PLATFORMS 3
#define BUDDY_SHORTCUT_WINDOW_MS 2000u
#define BUDDY_SHORTCUT_SAVE_QUIET_MS 250u
enum { BUDDY_SHORTCUT_DEFAULT, BUDDY_SHORTCUT_RALT, BUDDY_SHORTCUT_RCTRL,
       BUDDY_SHORTCUT_RSHIFT, BUDDY_SHORTCUT_FN };
/* Stable storage order: Windows, macOS, Linux. Zero retains the App/default. */
typedef struct { uint8_t value[BUDDY_SHORTCUT_PLATFORMS]; } buddy_shortcuts_t;
typedef struct {
    uint64_t held;
    uint32_t generation, session, started;
    uint8_t steps;
} buddy_shortcut_gesture_t;

int buddy_shortcut_platform(buddy_host_os_t host);
buddy_host_os_t buddy_shortcut_host(unsigned platform);
bool buddy_shortcut_valid(buddy_host_os_t host, unsigned shortcut);
/* Observes complete logical key state without consuming/changing any key.
 * Returns DEFAULT until a complete valid gesture; releases are not presses. */
uint8_t buddy_shortcut_observe(buddy_shortcut_gesture_t *state, uint32_t generation,
                              uint32_t session, uint64_t keys, uint32_t now);
