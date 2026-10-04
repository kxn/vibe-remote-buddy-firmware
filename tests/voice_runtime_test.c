/* Drive the production voice lifetime with a deterministic decoder/USB clock.
 * No host microphone means queued PCM cannot ever drain through USB. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "s3_runtime.h"
#include "standalone.h"
#include "rbp_decoder.h"

static uint32_t clock_ms;
int64_t esp_timer_get_time(void) { return (int64_t)clock_ms * 1000; }
buddy_host_os_t s3_host_os(void) { return BUDDY_HOST_MACOS; }
uint32_t s3_host_session(void) { return BUDDY_HOST_MACOS; }
int rbp_decoder_init(rbp_decoder_t *d, uint32_t size, const rbp_audio_format_t *f) {
    (void)size; d->codec_id = f->codec_id; return RBP_DECODE_OK;
}
int rbp_decoder_decode(rbp_decoder_t *d, const uint8_t *in, uint32_t size,
                       int16_t *out, uint32_t cap, uint32_t *written) {
    (void)d; (void)in; assert(cap >= size * 2);
    *written = size * 2;
    for (unsigned i = 0; i < *written; i++) out[i] = 123;
    return RBP_DECODE_OK;
}
int rbp_decoder_conceal(rbp_decoder_t *d, int16_t *out, uint32_t cap, uint32_t *n) {
    (void)d; (void)out; (void)cap; (void)n; assert(0); return RBP_DECODE_INPUT;
}
static void tick(uint32_t now) { clock_ms = now; standalone_tick(now); }
static unsigned pending(void) {
    uint32_t decoded, queued;
    assert(!s3_probe_audio_state(&decoded, &queued));
    return queued;
}
static void expect_key(bool down) {
    uint8_t report[8], expected[8] = {0};
    if (down) expected[1] = 1;
    s3_hid_desired(report);
    assert(!memcmp(report, expected, 8));
}
static void begin(uint32_t now, bool open) {
    standalone_abort();
    s3_take_stops();
    s3_probe_audio_reset();
    s3_peer_link(0, 1);
    s3_usb_state(true, open);
    tick(now);
    s3_peer_voice_key(0, 1, true, now + 1);
    expect_key(true);
    assert(s3_peer_wanted(0, 1));
    rbp_voice_evt_t e = {.type = RBP_VOICE_EVT_SOURCE_BEGIN};
    s3_peer_voice(0, 1, &e, now + 2);
    e = (rbp_voice_evt_t){.type = RBP_VOICE_EVT_FORMAT,
        .u.format = {.codec_id = RBP_CODEC_IMA_HI, .sample_rate = 16000}};
    s3_peer_voice(0, 1, &e, now + 3);
    uint8_t encoded[120] = {0};
    e = (rbp_voice_evt_t){.type = RBP_VOICE_EVT_ENCODED,
        .u.encoded = {.data = encoded, .len = sizeof encoded}};
    s3_peer_voice(0, 1, &e, now + 4);
    assert(pending() == 240);
}
static void release(uint32_t now) {
    s3_peer_voice_key(0, 1, false, now);
    rbp_voice_evt_t e = {.type = RBP_VOICE_EVT_END,
        .u.end.reason = RBP_END_NORMAL};
    s3_peer_voice(0, 1, &e, now + 1);
}
static void no_microphone(uint32_t start) {
    begin(start, false);
    release(start + 1000);
    tick(start + 1249);
    assert(pending() == 240); /* Brief grace for a late microphone open. */
    tick(start + 1250);
    tick(start + 1350);
    expect_key(false);
    assert(!standalone_busy() && !pending());
    s3_peer_voice_key(0, 1, true, start + 2000);
    assert(s3_peer_wanted(0, 1) && !s3_peer_rejected(0));
    expect_key(true);
}
static void microphone_tail(void) {
    begin(10000, true);
    release(11000);
    tick(11350);
    assert(pending() == 240); /* A real consumer keeps every tail sample. */
    expect_key(true);
    int16_t pcm[240];
    s3_pcm_read(pcm, 240);
    for (unsigned i = 0; i < 240; i++) assert(pcm[i] == 123);
    tick(11351); tick(11450);
    expect_key(true);
    tick(11451);
    expect_key(false);
    assert(!standalone_busy());
}
static void microphone_opens_late(void) {
    begin(20000, false);
    release(21000);
    tick(21200);
    s3_usb_state(true, true);
    tick(21250);
    assert(pending() == 240);
    int16_t pcm[240]; s3_pcm_read(pcm, 240);
    tick(21251); tick(21351);
    expect_key(false);
}
static void microphone_closes_early(void) {
    begin(30000, true);
    release(31000);
    s3_usb_state(true, false);
    tick(31250); tick(31350);
    expect_key(false);
    assert(!standalone_busy());
}
int main(void) {
    s3_runtime_init();
    no_microphone(1000);
    microphone_tail();
    microphone_opens_late();
    microphone_closes_early();
    no_microphone(UINT32_MAX - 1100);
    puts("Voice release, absent/late/closed microphone and USB tail passed.");
    return 0;
}
