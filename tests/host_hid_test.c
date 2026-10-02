/* Exercise the production host classifier, binding resolver and USB descriptor.
 * These checks cannot establish how macOS or Doubao handles a real Fn key. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "buddy_input.h"
#include "report_map.h"
#include "usb_hid_report.h"

static void string_request(buddy_host_probe_t *p, uint16_t length, uint32_t now) {
    buddy_host_setup(p, 0x80, 6, 0x0301, length, now);
}

static void host_detection(void) {
    buddy_host_probe_t p;
    buddy_host_reset(&p);
    assert(buddy_host_classify(&p, 1000, true) == BUDDY_HOST_UNKNOWN);
    /* Audio/class/vendor requests and non-string descriptors are not evidence. */
    buddy_host_setup(&p, 0xa1, 6, 0x0301, 2, 0);
    buddy_host_setup(&p, 0xc0, 6, 0x0301, 2, 0);
    buddy_host_setup(&p, 0x80, 6, 0x0100, 2, 0);
    buddy_host_setup(&p, 0x80, 1, 0x0301, 2, 0);
    assert(p.strings == 0);

    const uint16_t mac[] = {2, 12, 2, 24, 255};
    for (unsigned i = 0; i < sizeof mac / sizeof *mac; ++i)
        string_request(&p, mac[i], 100 + i);
    assert(buddy_host_classify(&p, 404, false) == BUDDY_HOST_UNKNOWN);
    assert(buddy_host_classify(&p, 403, true) == BUDDY_HOST_UNKNOWN);
    assert(buddy_host_classify(&p, 404, true) == BUDDY_HOST_MACOS);
    assert(p.frozen && p.short2 == 2 && p.full255 == 1 && p.short4 == 0);
    string_request(&p, 4, 500);
    assert(p.strings == 5 && buddy_host_classify(&p, 800, true) == BUDDY_HOST_MACOS);

    /* This Mac's real enumeration has 13 strings, six 2-byte headers and no
     * 255-byte reads. The optional full-length probe must not gate shortcuts. */
    buddy_host_reset(&p);
    p.strings = 13; p.short2 = 6; p.last = 100;
    assert(buddy_host_classify(&p, 399, true) == BUDDY_HOST_UNKNOWN);
    assert(buddy_host_classify(&p, 400, false) == BUDDY_HOST_UNKNOWN);
    assert(buddy_host_classify(&p, 400, true) == BUDDY_HOST_MACOS);
    assert(p.frozen && !p.full255);

    /* Headers alone, incomplete pairs and Windows-like short reads are not
     * enough to recognize the additional Mac enumeration pattern. */
    buddy_host_reset(&p);
    p.strings = p.short2 = 6;
    assert(buddy_host_classify(&p, 300, true) == BUDDY_HOST_OTHER);
    p.strings = 5; p.short2 = 3;
    assert(buddy_host_classify(&p, 300, true) == BUDDY_HOST_OTHER);
    p.strings = 6; p.short4 = 1;
    assert(buddy_host_classify(&p, 300, true) == BUDDY_HOST_OTHER);
    p.short4 = 0;
    assert(buddy_host_classify(&p, 300, true) == BUDDY_HOST_MACOS);

    /* USB reset / moving from Mac to Windows must discard the frozen result. */
    buddy_host_reset(&p);
    string_request(&p, 4, 0);
    string_request(&p, 255, 10);
    string_request(&p, 255, 20);
    assert(buddy_host_classify(&p, 320, true) == BUDDY_HOST_WINDOWS);
    assert(p.frozen);
    buddy_host_reset(&p);
    assert(!p.frozen && !p.strings && p.result == BUDDY_HOST_UNKNOWN);

    /* Sparse and conflicting evidence must not enable Globe. */
    string_request(&p, 255, 0);
    assert(buddy_host_classify(&p, 300, true) == BUDDY_HOST_OTHER);
    assert(!p.frozen);
    string_request(&p, 2, 301);
    string_request(&p, 2, 302);
    string_request(&p, 24, 303);
    string_request(&p, 4, 304);
    assert(buddy_host_classify(&p, 604, true) == BUDDY_HOST_OTHER);

    buddy_host_reset(&p);
    for (unsigned i = 0; i < 3; i++) string_request(&p, 255, i);
    assert(buddy_host_classify(&p, 301, true) == BUDDY_HOST_UNKNOWN);
    assert(buddy_host_classify(&p, 302, true) == BUDDY_HOST_LINUX && p.frozen);
    buddy_host_reset(&p);
    string_request(&p, 255, 0); string_request(&p, 255, 1); string_request(&p, 12, 2);
    assert(buddy_host_classify(&p, 302, true) == BUDDY_HOST_OTHER);

    /* Enumeration quiet time also works across the millisecond wraparound. */
    buddy_host_reset(&p);
    for (unsigned i = 0; i < sizeof mac / sizeof *mac; ++i)
        string_request(&p, mac[i], UINT32_MAX - 100);
    assert(buddy_host_classify(&p, 100, true) == BUDDY_HOST_UNKNOWN);
    assert(buddy_host_classify(&p, 199, true) == BUDDY_HOST_MACOS);
    buddy_host_reset(&p);
    p.strings = p.short2 = UINT16_MAX;
    string_request(&p, 2, 0);
    assert(p.strings == UINT16_MAX && p.short2 == UINT16_MAX);
}

static void voice_bindings(void) {
    buddy_map_t map;
    buddy_map_default(&map);
    buddy_binding_t doubao = map.key[RBP_KEY_VOICE];
    assert(doubao.kind == BM_VOICE_PRESET && doubao.value == 1);
    const uint8_t globe[8] = {0, 1}, right_alt[8] = {0x40};
    const uint8_t wechat[8] = {9}, custom[8] = {2, 0, 4};
    const uint8_t meeting[8] = {0, 0, 44}, release[8] = {0};
    uint8_t out[8];
    buddy_voice_keyboard(doubao, BUDDY_HOST_MACOS, out);
    assert(memcmp(out, globe, sizeof out) == 0);
    const buddy_host_os_t other[] = {BUDDY_HOST_WINDOWS, BUDDY_HOST_UNKNOWN, BUDDY_HOST_OTHER, BUDDY_HOST_LINUX};
    for (unsigned i = 0; i < sizeof other / sizeof *other; ++i) {
        buddy_voice_keyboard(doubao, other[i], out);
        assert(memcmp(out, right_alt, sizeof out) == 0);
    }
    buddy_voice_keyboard((buddy_binding_t){BM_VOICE_PRESET, 0, 2}, BUDDY_HOST_MACOS, out);
    assert(memcmp(out, globe, sizeof out) == 0);
    buddy_voice_keyboard((buddy_binding_t){BM_VOICE_PRESET, 0, 2}, BUDDY_HOST_WINDOWS, out);
    assert(memcmp(out, wechat, sizeof out) == 0);
    buddy_voice_keyboard((buddy_binding_t){BM_VOICE, 2, 4}, BUDDY_HOST_MACOS, out);
    assert(memcmp(out, custom, sizeof out) == 0);
    buddy_voice_keyboard((buddy_binding_t){BM_VOICE, 0, 44}, BUDDY_HOST_MACOS, out);
    assert(memcmp(out, meeting, sizeof out) == 0);
    buddy_voice_keyboard((buddy_binding_t){BM_VOICE_PRESET, 0, 99}, BUDDY_HOST_MACOS, out);
    assert(memcmp(out, release, sizeof out) == 0);
    buddy_voice_keyboard((buddy_binding_t){BM_DISABLED, 0, 0}, BUDDY_HOST_MACOS, out);
    assert(memcmp(out, release, sizeof out) == 0);
}

static void report_layout(void) {
    hogp_report_map_t map;
    assert(hogp_report_map_parse(buddy_hid_report_descriptor,
                               sizeof buddy_hid_report_descriptor, &map));
    assert(map.uses_report_ids && map.input_count == 2);
    assert(map.inputs[0].report_id == 1 && map.inputs[0].bytes == 8);
    assert(map.inputs[1].report_id == 2 && map.inputs[1].bytes == 2);
    unsigned globe = 0, modifiers = 0, keys = 0, consumers = 0;
    const uint16_t media[] = {0xe9, 0xea, 0xe2, 0xcd, 0xb5, 0xb6, 0xb7, 0x223};
    for (unsigned i = 0; i < map.field_count; ++i) {
        const hogp_field_t *f = &map.fields[i];
        if (f->report_id == 1 && f->page == 0x00ff) {
            assert(f->usage_min == 0x03 && f->usage_max == 0x03);
            assert(f->offset == 8 && f->size == 1 && f->count == 1 && f->variable);
            ++globe;
        } else if (f->report_id == 1 && f->page == 7 && f->variable) {
            assert(f->offset < 8 && f->size == 1 && f->count == 1);
            assert(f->usage_min == 0xe0 + f->offset);
            ++modifiers;
        } else if (f->report_id == 1 && f->page == 7 && !f->variable) {
            assert(f->offset == 16 && f->size == 8 && f->count == 6);
            assert(f->usage_min == 0 && f->usage_max == 0xdf);
            ++keys;
        } else if (f->report_id == 2) {
            assert(f->page == 0x0c && f->offset < 8 && f->size == 1 && f->count == 1);
            assert(f->usage_min == media[f->offset] && f->variable);
            ++consumers;
        } else assert(0);
    }
    assert(globe == 1 && modifiers == 8 && keys == 1 && consumers == 8);
}

int main(void) {
    host_detection();
    voice_bindings();
    report_layout();
    puts("Host detection, voice bindings and USB native Fn report layout passed.");
    return 0;
}
