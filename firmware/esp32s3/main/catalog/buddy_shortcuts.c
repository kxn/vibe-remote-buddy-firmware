#include "buddy_shortcuts.h"
#include "rbp/defs.h"

int buddy_shortcut_platform(buddy_host_os_t host) {
    switch (host) {
    case BUDDY_HOST_WINDOWS: return 0;
    case BUDDY_HOST_MACOS: return 1;
    case BUDDY_HOST_LINUX: return 2;
    default: return -1;
    }
}
buddy_host_os_t buddy_shortcut_host(unsigned platform) {
    const buddy_host_os_t hosts[] = {BUDDY_HOST_WINDOWS, BUDDY_HOST_MACOS, BUDDY_HOST_LINUX};
    return platform < BUDDY_SHORTCUT_PLATFORMS ? hosts[platform] : BUDDY_HOST_UNKNOWN;
}
bool buddy_shortcut_valid(buddy_host_os_t host, unsigned shortcut) {
    return buddy_shortcut_platform(host) >= 0 && shortcut <= BUDDY_SHORTCUT_FN &&
           (shortcut != BUDDY_SHORTCUT_FN || host == BUDDY_HOST_MACOS);
}
uint8_t buddy_shortcut_observe(buddy_shortcut_gesture_t *s, uint32_t generation,
                              uint32_t session, uint64_t keys, uint32_t now) {
    if (s->generation != generation || s->session != session) {
        s->steps = 0;
        if (s->generation != generation) s->held = 0;
        s->generation = generation;
        s->session = session;
    }
    uint64_t pressed = keys & ~s->held;
    s->held = keys;
    buddy_host_os_t host = buddy_host_session_os(session);
    if (!generation || buddy_shortcut_platform(host) < 0) { s->steps = 0; return 0; }
    if (s->steps && (uint32_t)(now - s->started) > BUDDY_SHORTCUT_WINDOW_MS) s->steps = 0;
    /* Chords, including a new key while Power is held, cancel the sequence. */
    if (keys && (keys & (keys - 1))) { s->steps = 0; return 0; }
    if (!pressed) return 0;
    if (pressed == (UINT64_C(1) << RBP_KEY_POWER)) {
        if (s->steps == 1) s->steps = 2;
        else { s->steps = 1; s->started = now; }
        return 0;
    }
    bool armed = s->steps == 2;
    s->steps = 0;
    if (!armed) return 0;
    uint8_t shortcut = pressed == (UINT64_C(1) << RBP_KEY_UP) ? BUDDY_SHORTCUT_RALT :
                       pressed == (UINT64_C(1) << RBP_KEY_LEFT) ? BUDDY_SHORTCUT_RCTRL :
                       pressed == (UINT64_C(1) << RBP_KEY_RIGHT) ? BUDDY_SHORTCUT_RSHIFT :
                       pressed == (UINT64_C(1) << RBP_KEY_DOWN) ? BUDDY_SHORTCUT_FN : 0;
    return buddy_shortcut_valid(host, shortcut) ? shortcut : 0;
}
