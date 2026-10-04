#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "buddy_input.h"

#define KEY(k) (UINT64_C(1) << (k))
static uint8_t observe(buddy_shortcut_gesture_t *s, uint64_t keys, uint32_t time) {
    return buddy_shortcut_observe(s, 1, 8u | BUDDY_HOST_WINDOWS, keys, time);
}
static uint8_t sequence(buddy_shortcut_gesture_t *s, uint32_t session,
                        unsigned key, uint32_t start, uint32_t finish) {
    assert(!buddy_shortcut_observe(s, 1, session, KEY(RBP_KEY_POWER), start));
    assert(!buddy_shortcut_observe(s, 1, session, 0, start + 30));
    assert(!buddy_shortcut_observe(s, 1, session, KEY(RBP_KEY_POWER), start + 100));
    assert(!buddy_shortcut_observe(s, 1, session, 0, start + 130));
    return buddy_shortcut_observe(s, 1, session, KEY(key), finish);
}
static void gesture_edges(void) {
    const unsigned keys[] = {RBP_KEY_UP, RBP_KEY_LEFT, RBP_KEY_RIGHT, RBP_KEY_DOWN};
    for (unsigned h = 0; h < BUDDY_SHORTCUT_PLATFORMS; h++) {
        buddy_host_os_t host = buddy_shortcut_host(h);
        for (unsigned k = 0; k < 4; k++) {
            buddy_shortcut_gesture_t s = {0};
            unsigned expected = k == 3 && host != BUDDY_HOST_MACOS ? 0 : k + 1;
            assert(sequence(&s, 8u | host, keys[k], 0, 2000) == expected);
            assert(!buddy_shortcut_observe(&s, 1, 8u | host, KEY(keys[k]), 2001));
            s = (buddy_shortcut_gesture_t){0};
            assert(!sequence(&s, 8u | host, keys[k], 0, 2001));
        }
    }
    buddy_shortcut_gesture_t s = {0};
    assert(sequence(&s, 8u | BUDDY_HOST_WINDOWS, RBP_KEY_LEFT, UINT32_MAX - 100, 1899) == BUDDY_SHORTCUT_RCTRL);
    s = (buddy_shortcut_gesture_t){0};
    assert(!sequence(&s, 8u | BUDDY_HOST_UNKNOWN, RBP_KEY_LEFT, 0, 300));
    s = (buddy_shortcut_gesture_t){0};
    assert(!sequence(&s, 8u | BUDDY_HOST_OTHER, RBP_KEY_LEFT, 0, 300));

    /* Holding and repeat reports do not create another Power down edge. */
    s = (buddy_shortcut_gesture_t){0};
    observe(&s, KEY(RBP_KEY_POWER), 0);
    for (unsigned t = 10; t <= 500; t += 10) assert(!observe(&s, KEY(RBP_KEY_POWER), t));
    observe(&s, 0, 510);
    assert(!observe(&s, KEY(RBP_KEY_LEFT), 520));
    /* An unrelated key, a chord, and three Powers are not the requested sequence. */
    const uint64_t interruptions[] = {KEY(RBP_KEY_OK), KEY(RBP_KEY_POWER) | KEY(RBP_KEY_LEFT), KEY(RBP_KEY_POWER)};
    for (unsigned i = 0; i < 3; i++) {
        s = (buddy_shortcut_gesture_t){0};
        observe(&s, KEY(RBP_KEY_POWER), 0); observe(&s, 0, 10);
        observe(&s, KEY(RBP_KEY_POWER), 20); observe(&s, 0, 30);
        observe(&s, interruptions[i], 40); observe(&s, 0, 50);
        assert(!observe(&s, KEY(RBP_KEY_LEFT), 60));
    }
    /* USB epoch, OS, and BLE link changes invalidate an armed sequence. */
    for (unsigned i = 0; i < 3; i++) {
        s = (buddy_shortcut_gesture_t){0};
        observe(&s, KEY(RBP_KEY_POWER), 0); observe(&s, 0, 10);
        observe(&s, KEY(RBP_KEY_POWER), 20); observe(&s, 0, 30);
        uint32_t session = i == 0 ? 16u | BUDDY_HOST_WINDOWS : i == 1 ? 8u | BUDDY_HOST_MACOS : 8u | BUDDY_HOST_WINDOWS;
        assert(!buddy_shortcut_observe(&s, i == 2 ? 2 : 1, session, KEY(RBP_KEY_LEFT), 40));
    }
}

static void feed(buddy_input_t *i, unsigned slot, unsigned key, uint32_t session, uint32_t now) {
    buddy_input_keys_observed(i, slot, 1, key ? KEY(key) : 0, session, now);
}
static void choose(buddy_input_t *i, unsigned slot, unsigned key, buddy_host_os_t host, uint32_t start) {
    uint32_t session = 8u | host;
    feed(i, slot, RBP_KEY_POWER, session, start); feed(i, slot, 0, session, start + 50);
    feed(i, slot, RBP_KEY_POWER, session, start + 100); feed(i, slot, 0, session, start + 150);
    feed(i, slot, key, session, start + 200); feed(i, slot, 0, session, start + 250);
}
static void platform_isolation(void) {
    buddy_input_t i;
    buddy_input_init(&i);
    buddy_arbiter_link(&i.arbiter, 0, 1); buddy_arbiter_link(&i.arbiter, 1, 1);
    choose(&i, 0, RBP_KEY_LEFT, BUDDY_HOST_WINDOWS, 0);
    assert(i.shortcuts.value[0] == BUDDY_SHORTCUT_RCTRL && i.shortcut_dirty == 1);
    assert(!i.shortcuts.value[1] && !i.shortcuts.value[2]);
    choose(&i, 1, RBP_KEY_DOWN, BUDDY_HOST_MACOS, 1000);
    choose(&i, 0, RBP_KEY_RIGHT, BUDDY_HOST_LINUX, 2000);
    assert(i.shortcuts.value[0] == 2 && i.shortcuts.value[1] == 4 && i.shortcuts.value[2] == 3);
    assert(i.shortcut_dirty == 7);
    choose(&i, 0, RBP_KEY_DOWN, BUDDY_HOST_WINDOWS, 3000);
    choose(&i, 0, RBP_KEY_DOWN, BUDDY_HOST_LINUX, 4000);
    assert(i.shortcuts.value[0] == 2 && i.shortcuts.value[2] == 3);
    i.shortcut_dirty = 0;
    choose(&i, 0, RBP_KEY_LEFT, BUDDY_HOST_WINDOWS, 5000);
    assert(i.shortcut_dirty == 0); /* no repeated flash write for same choice */
    choose(&i, 0, RBP_KEY_UP, BUDDY_HOST_WINDOWS, 6000);
    assert(i.shortcut_dirty == 1);
    assert(!buddy_input_shortcut_save_ready(&i, 6499));
    assert(buddy_input_shortcut_save_ready(&i, 6500));
    feed(&i, 1, RBP_KEY_OK, 9, 6501);
    assert(!buddy_input_shortcut_save_ready(&i, 8000));
    feed(&i, 1, 0, 9, 8100);
    assert(!buddy_input_shortcut_save_ready(&i, 8349));
    assert(buddy_input_shortcut_save_ready(&i, 8350));

    const uint8_t expected[][8] = {{0x40}, {0, 1}, {0x20}};
    const buddy_binding_t bindings[] = {{BM_VOICE_PRESET, 0, 1}, {BM_VOICE_PRESET, 0, 2},
                                       {BM_VOICE, 9, 4}, {BM_VOICE, 0, 44}};
    for (unsigned p = 0; p < 3; p++) for (unsigned b = 0; b < 4; b++) {
        uint8_t out[8];
        buddy_voice_keyboard_override(bindings[b], buddy_shortcut_host(p), &i.shortcuts, out);
        assert(!memcmp(out, expected[p], 8));
    }
    uint8_t original[8], overridden[8];
    buddy_voice_keyboard(bindings[2], BUDDY_HOST_OTHER, original);
    buddy_voice_keyboard_override(bindings[2], BUDDY_HOST_OTHER, &i.shortcuts, overridden);
    assert(!memcmp(original, overridden, 8));
    buddy_voice_keyboard_override((buddy_binding_t){0}, BUDDY_HOST_MACOS, &i.shortcuts, overridden);
    assert(!memcmp(overridden, (uint8_t[8]){0}, 8));
}

static void bystander_outputs(void) {
    buddy_input_t observed, original;
    buddy_input_init(&observed);
    buddy_arbiter_link(&observed.arbiter, 0, 1);
    observed.map[0].key[RBP_KEY_POWER] = (buddy_binding_t){BM_KEYBOARD, 1, 4};
    original = observed;
    const unsigned seq[] = {RBP_KEY_POWER, RBP_KEY_POWER, 0, RBP_KEY_POWER, 0, RBP_KEY_LEFT, 0};
    for (unsigned j = 0; j < sizeof seq / sizeof *seq; j++) {
        uint64_t bits = seq[j] ? KEY(seq[j]) : 0;
        uint64_t a = buddy_input_keys_observed(&observed, 0, 1, bits, 9, 100 * j);
        uint64_t b = buddy_input_keys(&original, 0, 1, bits);
        uint8_t x[8], y[8]; buddy_input_keyboard(&observed, x); buddy_input_keyboard(&original, y);
        assert(a == b && !memcmp(x, y, 8));
        assert(buddy_arbiter_media(&observed.arbiter) == buddy_arbiter_media(&original.arbiter));
    }
    assert(observed.shortcuts.value[0] == BUDDY_SHORTCUT_RCTRL);
    /* Host actions still occur twice when Power is mapped to an App action. */
    buddy_input_init(&observed); buddy_arbiter_link(&observed.arbiter, 0, 1);
    observed.management = true;
    observed.map[0].key[RBP_KEY_POWER] = (buddy_binding_t){BM_ACTION, 0, 65535};
    unsigned actions = 0;
    for (unsigned j = 0; j < sizeof seq / sizeof *seq; j++)
        if (buddy_input_keys_observed(&observed, 0, 1, seq[j] ? KEY(seq[j]) : 0, 9, 100 * j)) actions++;
    assert(actions == 2 && observed.shortcuts.value[0] == BUDDY_SHORTCUT_RCTRL);
    /* Two remotes cannot assemble one gesture. Stale generations are ignored. */
    buddy_input_init(&observed);
    buddy_arbiter_link(&observed.arbiter, 0, 1); buddy_arbiter_link(&observed.arbiter, 1, 1);
    feed(&observed, 0, RBP_KEY_POWER, 9, 0); feed(&observed, 0, 0, 9, 20);
    feed(&observed, 1, RBP_KEY_POWER, 9, 40); feed(&observed, 1, 0, 9, 60);
    assert(!buddy_input_keys_observed(&observed, 0, 99, KEY(RBP_KEY_POWER), 9, 70));
    feed(&observed, 0, RBP_KEY_LEFT, 9, 80);
    assert(!observed.shortcut_dirty);
    assert(buddy_arbiter_voice(&observed.arbiter, 0, 1, true));
    choose(&observed, 1, RBP_KEY_RIGHT, BUDDY_HOST_WINDOWS, 1000);
    assert(!observed.shortcut_dirty && !buddy_input_shortcut_save_ready(&observed, 2000));
}
int main(void) {
    gesture_edges(); platform_isolation(); bystander_outputs();
    puts("Gesture boundaries, platform isolation, overrides and unchanged key output passed.");
    return 0;
}
