#ifdef RBP_WITH_MSBC
#include "msbc.h"
static buddy_msbc_t msbc;
static unsigned msbc_bad_frames;
#endif
#include "s3_runtime.h"
#include "esp_heap_caps.h"
#include "faults.h"
#include "freertos/FreeRTOS.h"
#ifdef S3_CODEC_METRICS
#include "freertos/task.h"
#include "esp_timer.h"
#endif
#include "rbp_decoder.h"
#include "standalone.h"
#include "../../adapters/rc003/unicom_limits.h"
#include <string.h>

#define PCM_SAMPLES 48000u
typedef struct {
  uint8_t data[8192];
  unsigned head, tail, count;
} byte_ring_t;
static byte_ring_t rx, tx;
static portMUX_TYPE transport_guard = portMUX_INITIALIZER_UNLOCKED;
static uint32_t cdc_epoch;
static bool cdc_connected;
static portMUX_TYPE guard = portMUX_INITIALIZER_UNLOCKED;
static int16_t *pcm;
static unsigned head, tail, count;
static uint8_t keyboard[8];
static uint16_t consumer;
static uint32_t audio_epoch;
static bool mounted, recording, usb_fault, voice_down, blocked, source, seeded,
    stop;
static uint32_t generation, observed_generation, started, progress, ended,
    press_ms;
static uint32_t capture, received, decoded, consumed, underruns, concealed;
static rbp_decoder_t decoder;
static unsigned source_rate;
static int16_t resample_previous;
static bool resample_valid;
#ifdef S3_CODEC_METRICS
typedef struct {
  uint32_t calls;
  uint32_t total_us;
  uint32_t max_us;
} decoder_metric_t;
static decoder_metric_t ima_metric, ico_metric, msbc_metric;
static uint32_t decoder_stack_min_bytes, queue_high_water_samples;
#endif
#ifdef RBP_MULTI_STANDALONE
static buddy_input_t input;
static uint8_t stop_slots;
/* Physical key state and the USB shortcut lifetime are deliberately separate.
 */
static bool draining, detached, voice_hold;
static uint8_t voice_shortcut[8];
static uint32_t drain_started, empty_since;
#define DRAIN_TIMEOUT_MS 5000u
#define USB_TAIL_MS 100u
static void begin_drain(uint32_t now) {
  if (!draining) {
    draining = true;
    drain_started = now;
    empty_since = 0;
  }
}
static void refresh_hid(void) {
  buddy_input_keyboard(&input, keyboard);
  if (voice_hold && input.arbiter.owner != BUDDY_NO_OWNER) {
    memcpy(keyboard, voice_shortcut, sizeof keyboard);
  }
  consumer = buddy_arbiter_media(&input.arbiter);
}
#endif
void s3_runtime_init(void) {
  pcm = heap_caps_malloc(PCM_SAMPLES * sizeof *pcm,
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  configASSERT(pcm);
#ifdef RBP_MULTI_STANDALONE
  buddy_input_init(&input);
#endif
}
static size_t ring_write(byte_ring_t *r, const uint8_t *p, size_t n) {
  portENTER_CRITICAL(&transport_guard);
  size_t len = sizeof r->data - r->count;
  if (len > n)
    len = n;
  for (size_t i = 0; i < len; i++) {
    r->data[r->head] = p[i];
    r->head = (r->head + 1) % sizeof r->data;
  }
  r->count += len;
  portEXIT_CRITICAL(&transport_guard);
  return len;
}
static size_t ring_read(byte_ring_t *r, uint8_t *p, size_t n) {
  portENTER_CRITICAL(&transport_guard);
  size_t len = r->count;
  if (len > n)
    len = n;
  for (size_t i = 0; i < len; i++) {
    p[i] = r->data[r->tail];
    r->tail = (r->tail + 1) % sizeof r->data;
  }
  r->count -= len;
  portEXIT_CRITICAL(&transport_guard);
  return len;
}
size_t s3_cdc_receive(const uint8_t *p, size_t n) {
  return ring_write(&rx, p, n);
}
size_t s3_rx_read(uint8_t *p, size_t n) { return ring_read(&rx, p, n); }
size_t s3_tx_write(const uint8_t *p, size_t n) { return ring_write(&tx, p, n); }
size_t s3_cdc_output(uint8_t *p, size_t n) { return ring_read(&tx, p, n); }
void s3_cdc_state(bool active) {
  portENTER_CRITICAL(&transport_guard);
  if (cdc_connected && !active) {
    cdc_epoch++;
    rx.head = rx.tail = rx.count = tx.head = tx.tail = tx.count = 0;
  }
  cdc_connected = active;
  portEXIT_CRITICAL(&transport_guard);
}
uint32_t s3_cdc_epoch(void) {
  portENTER_CRITICAL(&transport_guard);
  uint32_t n = cdc_epoch;
  portEXIT_CRITICAL(&transport_guard);
  return n;
}
void s3_usb_state(bool connected, bool active) {
  portENTER_CRITICAL(&guard);
  if (mounted != connected) {
    mounted = connected;
    generation++;
  }
  recording = active;
  portEXIT_CRITICAL(&guard);
}
void s3_hid_desired(uint8_t report[8]) {
  portENTER_CRITICAL(&guard);
  memcpy(report, keyboard, 8);
  portEXIT_CRITICAL(&guard);
}
uint16_t s3_consumer_desired(void) {
  portENTER_CRITICAL(&guard);
  uint16_t v = consumer;
  portEXIT_CRITICAL(&guard);
  return v;
}
uint32_t s3_audio_epoch(void) {
  portENTER_CRITICAL(&guard);
  uint32_t v = audio_epoch;
  portEXIT_CRITICAL(&guard);
  return v;
}
void s3_hid_fault(void) {
  portENTER_CRITICAL(&guard);
  usb_fault = true;
  memset(keyboard, 0, 8);
  portEXIT_CRITICAL(&guard);
}
unsigned s3_pcm_read(int16_t *out, unsigned n) {
  portENTER_CRITICAL(&guard);
  for (unsigned i = 0; i < n; i++) {
    if (count) {
      out[i] = pcm[tail];
      tail = (tail + 1) % PCM_SAMPLES;
      count--;
      consumed++;
    } else {
      out[i] = 0;
      if (source)
        underruns++;
    }
  }
  portEXIT_CRITICAL(&guard);
  return n;
}
void standalone_abort(void) {
  portENTER_CRITICAL(&guard);
  if (source)
    stop = true;
  source = seeded = false;
  head = tail = count = 0;
  audio_epoch++;
  consumer = 0;
  if (voice_down)
    blocked = true;
  memset(keyboard, 0, 8);
#ifdef RBP_MULTI_STANDALONE
  draining = detached = voice_hold = false;
  unsigned owner = input.arbiter.owner;
  if (owner != BUDDY_NO_OWNER) {
    stop_slots |= 1u << owner;
    input.arbiter.rejected[owner] = input.arbiter.down[owner];
    buddy_arbiter_release(&input.arbiter, owner,
                          input.arbiter.generation[owner]);
  }
  voice_down = false;
  refresh_hid();
#endif
  portEXIT_CRITICAL(&guard);
}
bool standalone_wanted(void) {
  portENTER_CRITICAL(&guard);
  bool result = mounted;
  portEXIT_CRITICAL(&guard);
  return result;
}
bool standalone_busy(void) {
  portENTER_CRITICAL(&guard);
  bool result = source || count || voice_down;
#ifdef RBP_MULTI_STANDALONE
  result = result || input.arbiter.owner != BUDDY_NO_OWNER;
#endif
  portEXIT_CRITICAL(&guard);
  return result;
}
bool standalone_take_stop(void) {
  bool result = stop;
  stop = false;
  return result;
}
void standalone_voice_key(bool down, uint32_t now) {
  portENTER_CRITICAL(&guard);
  if (!down)
    blocked = false;
  if (down && !voice_down)
    press_ms = now;
  voice_down = down;
  memset(keyboard, 0, 8);
  if (down && !blocked && mounted)
    keyboard[0] = 0x40;
  portEXIT_CRITICAL(&guard);
}
void standalone_keys(const rbp_device_profile_t *p, uint64_t bits) {
  uint8_t key = 0;
  uint16_t media = 0;
  if (p)
    for (unsigned i = 0; i < p->key_count && i < 64; i++)
      if (bits & ((uint64_t)1 << i))
        switch (p->keys[i].key_id) {
        case RBP_KEY_UP:
          key = 0x52;
          break;
        case RBP_KEY_DOWN:
          key = 0x51;
          break;
        case RBP_KEY_LEFT:
          key = 0x50;
          break;
        case RBP_KEY_RIGHT:
          key = 0x4f;
          break;
        case RBP_KEY_OK:
          key = 0x28;
          break;
        case RBP_KEY_BACK:
          key = 0x29;
          break;
        case RBP_KEY_VOLUME_UP:
          media = 0xe9;
          break;
        case RBP_KEY_VOLUME_DOWN:
          media = 0xea;
          break;
        default:
          break;
        }
  portENTER_CRITICAL(&guard);
  consumer = media;
  if (!voice_down)
    keyboard[2] = key;
  portEXIT_CRITICAL(&guard);
}
static unsigned probe_audio_fault;
void s3_probe_audio_reset(void) { probe_audio_fault=0; }
unsigned s3_probe_audio_state(uint32_t *samples,uint32_t *pending) {
  portENTER_CRITICAL(&guard); *samples=decoded; *pending=count; portEXIT_CRITICAL(&guard); return probe_audio_fault;
}
static void fail(unsigned stage, unsigned code) {
  probe_audio_fault=code;
  rbp_fault_record(RBP_FAULT_STANDALONE, stage, code, count);
  standalone_abort();
}
void standalone_voice(const rbp_voice_evt_t *e, uint32_t now) {
  if (e->type == RBP_VOICE_EVT_SOURCE_BEGIN) {
    portENTER_CRITICAL(&guard);
    head = tail = count = 0;
    audio_epoch++;
    source = true;
    seeded = false;
    source_rate = 0;
    resample_valid = false;
    capture++;
    received = decoded = consumed = underruns = concealed = 0;
#ifdef S3_CODEC_METRICS
    memset(&ima_metric, 0, sizeof ima_metric);
    memset(&ico_metric, 0, sizeof ico_metric);
    memset(&msbc_metric, 0, sizeof msbc_metric);
    decoder_stack_min_bytes = UINT32_MAX;
    queue_high_water_samples = 0;
#endif
    started = progress = now;
    ended = 0;
    portEXIT_CRITICAL(&guard);
    return;
  }
  if (e->type == RBP_VOICE_EVT_END) {
    ended = now;
    portENTER_CRITICAL(&guard);
    source = false;
#ifdef RBP_MULTI_STANDALONE
    if (input.arbiter.owner != BUDDY_NO_OWNER) {
      voice_down = false;
      begin_drain(now);
    }
#endif
    portEXIT_CRITICAL(&guard);
    if (e->u.end.reason != RBP_END_NORMAL)
      fail(3, e->u.end.reason);
    return;
  }
  if (e->type == RBP_VOICE_EVT_FAULT) {
    fail(3, e->u.end.reason);
    return;
  }
  if (!source)
    return;
  if (e->type == RBP_VOICE_EVT_START || e->type == RBP_VOICE_EVT_FORMAT) {
    const rbp_audio_format_t *f = &e->u.format;
    if ((f->sample_rate != 16000 && f->sample_rate != 8000) ||
        (
#ifdef RBP_WITH_MSBC
        f->codec_id==RBP_CODEC_MSBC ?
          (f->codec_revision!=1 || f->sample_rate!=16000 || f->channels!=1 || f->max_unit_bytes!=57 || f->config_len || !buddy_msbc_init(&msbc)) :
#endif
        rbp_decoder_init(&decoder, sizeof decoder, f) != RBP_DECODE_OK)) {
      fail(1, RBP_END_UNSUPPORTED_FORMAT);
      return;
    }
    if(source_rate != f->sample_rate) resample_valid = false;
    decoder.codec_id=f->codec_id;
#ifdef RBP_WITH_MSBC
    msbc_bad_frames=0;
#endif
    source_rate = f->sample_rate;
    seeded = true;
    return;
  }
  if (e->type == RBP_VOICE_EVT_ENCODED) {
    unsigned n = e->u.encoded.len, lost = e->u.encoded.erasures_before;
    if (!seeded || n > 512 || !e->u.encoded.data ||
        (decoder.codec_id == RBP_CODEC_IFLYTEK_ICO && lost > UNICOM_ICO_MAX_CONCEALED_UNITS) ||
        (decoder.codec_id != RBP_CODEC_IFLYTEK_ICO && lost > 5) ||
        (lost && !((decoder.codec_id == RBP_CODEC_IFLYTEK_ICO && n == 40) || (decoder.codec_id == RBP_CODEC_MSBC && n == 57)))) {
      fail(1, RBP_END_INVALID_ENCODED_DATA);
      return;
    }
    portENTER_CRITICAL(&guard);
    unsigned expected = decoder.codec_id == RBP_CODEC_IFLYTEK_ICO ? 320 * (lost + 1) : decoder.codec_id==RBP_CODEC_MSBC ? 120*(lost+1) : 2 * n;
    unsigned factor = source_rate == 8000 ? 2 : 1;
    bool room = PCM_SAMPLES - count >= expected * factor;
    portEXIT_CRITICAL(&guard);
    if (!room) {
      fail(2, RBP_END_BUFFER_OVERRUN);
      return;
    }
    int16_t samples[1024];
    for (unsigned unit = 0; unit <= lost; unit++) {
      uint32_t written = 0;
      int rc;
#ifdef S3_CODEC_METRICS
      int64_t decode_start_us = esp_timer_get_time();
#endif
#ifdef RBP_WITH_MSBC
      if(decoder.codec_id==RBP_CODEC_MSBC) {
        written=120;
        if(unit<lost) {memset(samples,0,240);rc=RBP_DECODE_OK;}
        else if(buddy_msbc_decode(&msbc,e->u.encoded.data,n,samples)){msbc_bad_frames=0;rc=RBP_DECODE_OK;}
        else {if(++msbc_bad_frames>5){fail(1,RBP_END_INVALID_ENCODED_DATA);return;}rbp_fault_record(RBP_FAULT_STANDALONE,36,1,n);memset(samples,0,240);buddy_msbc_init(&msbc);rc=RBP_DECODE_OK;}
      } else
#endif
      rc = unit < lost ? rbp_decoder_conceal(&decoder, samples, 1024, &written) :
          rbp_decoder_decode(&decoder, e->u.encoded.data, n, samples, 1024, &written);
      if (rc != RBP_DECODE_OK) { fail(1, RBP_END_INVALID_ENCODED_DATA); return; }
#ifdef S3_CODEC_METRICS
      uint32_t decode_elapsed_us = (uint32_t)(esp_timer_get_time() - decode_start_us);
      uint32_t stack_bytes = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
#endif
      portENTER_CRITICAL(&guard);
      for (unsigned i = 0; i < written; i++) {
        /* Causal 2x linear interpolation, preserving state across BLE chunks.
         * UAC remains 16 kHz for all remotes. First sample is held once. */
        if(factor == 2) {
          int16_t midpoint = resample_valid ?
              (int16_t)(((int32_t)resample_previous + samples[i]) / 2) : samples[i];
          pcm[head] = midpoint;head = (head + 1) % PCM_SAMPLES;
          resample_previous = samples[i];resample_valid = true;
        }
        pcm[head] = samples[i];head = (head + 1) % PCM_SAMPLES;
      }
      count += written * factor;decoded += written * factor;
      if(unit < lost)concealed++;else received += n;
#ifdef S3_CODEC_METRICS
      decoder_metric_t *metric = decoder.codec_id == RBP_CODEC_MSBC ?
          &msbc_metric : decoder.codec_id == RBP_CODEC_IFLYTEK_ICO ?
              &ico_metric : &ima_metric;
      metric->calls++;
      metric->total_us += decode_elapsed_us;
      if (decode_elapsed_us > metric->max_us)
        metric->max_us = decode_elapsed_us;
      if (stack_bytes < decoder_stack_min_bytes)
        decoder_stack_min_bytes = stack_bytes;
      if (count > queue_high_water_samples)
        queue_high_water_samples = count;
#endif
      progress = now;
      portEXIT_CRITICAL(&guard);
    }
  }
}
void standalone_tick(uint32_t now) {
#ifdef RBP_MULTI_STANDALONE
  portENTER_CRITICAL(&guard);
  unsigned owner = input.arbiter.owner;
  bool drain_timeout = draining && now - drain_started >= DRAIN_TIMEOUT_MS;
  if (draining && !source && !count && (ended || now - drain_started >= 1000)) {
    if (!empty_since)
      empty_since = now;
  } else
    empty_since = 0;
  /* The last PCM read only enters TinyUSB's FIFO. Allow its bounded 8 ms
   * queue plus host scheduling margin to pass before releasing the shortcut. */
  if (owner != BUDDY_NO_OWNER && draining && !source && !count && empty_since &&
      now - empty_since >= USB_TAIL_MS) {
    voice_hold = draining = false;
    voice_down = false;
    if (detached)
      buddy_arbiter_link(&input.arbiter, owner, 0);
    else {
      input.arbiter.rejected[owner] = input.arbiter.down[owner];
      buddy_arbiter_release(&input.arbiter, owner,
                            input.arbiter.generation[owner]);
    }
    detached = false;
    refresh_hid();
    drain_timeout = false;
  }
  portEXIT_CRITICAL(&guard);
#endif
  portENTER_CRITICAL(&guard);
  bool changed = observed_generation != generation, bad = usb_fault;
  observed_generation = generation;
  usb_fault = false;
  bool active = source, down = voice_down, connected = mounted,
       open = recording;
  portEXIT_CRITICAL(&guard);
  if (changed || bad) {
    if (bad)
      rbp_fault_record(RBP_FAULT_STANDALONE, 8, 3, 0);
    standalone_abort();
    return;
  }
#ifdef RBP_MULTI_STANDALONE
  if (drain_timeout) {
    fail(9, RBP_END_CONSUMER_DISABLED);
    return;
  }
#endif
  /* Bound a key that never starts a source, not a healthy recording.
   * Live streams are bounded by progress and consumer/link health instead. */
  if ((down && !active && !ended && now - press_ms > 65000) ||
      (active && (!connected || now - progress > 2000)))
    fail(4, RBP_END_DEVICE_ERROR);
  /* Physical release closes the IME microphone before the remote's
   * final audio/stop response. That normal drain is not consumer failure. */
  else if (active && down && !open && now - started > 2000)
    fail(5, RBP_END_CONSUMER_DISABLED);
}
void standalone_stats(uint8_t out[STANDALONE_STATS_BYTES]) {
  portENTER_CRITICAL(&guard);
  uint32_t v[] = {1,
                  capture,
                  started,
                  ended,
                  received,
                  decoded,
                  concealed,
                  consumed,
                  consumed,
                  underruns,
                  count,
                  0,
                  count,
                  0,
                  0,
                  0,
                  (source ? 1u : 0u) | (voice_down ? 16u : 0u) |
                      (blocked ? 32u : 0u),
                  recording};
  portEXIT_CRITICAL(&guard);
  for (unsigned i = 0; i < 18; i++)
    for (unsigned j = 0; j < 4; j++)
      out[4 * i + j] = v[i] >> (8 * j);
}

#ifdef S3_CODEC_METRICS
void standalone_codec_metrics(uint32_t out[STANDALONE_CODEC_METRICS_WORDS]) {
  portENTER_CRITICAL(&guard);
  out[0] = 1;
  out[1] = ima_metric.calls;
  out[2] = ima_metric.total_us;
  out[3] = ima_metric.max_us;
  out[4] = ico_metric.calls;
  out[5] = ico_metric.total_us;
  out[6] = ico_metric.max_us;
  out[7] = msbc_metric.calls;
  out[8] = msbc_metric.total_us;
  out[9] = msbc_metric.max_us;
  out[10] = decoder_stack_min_bytes == UINT32_MAX ? 0 : decoder_stack_min_bytes;
  out[11] = queue_high_water_samples;
  portEXIT_CRITICAL(&guard);
}
#endif

#ifdef RBP_MULTI_STANDALONE
uint8_t s3_take_stops(void) {
  uint8_t mask = stop_slots;
  stop_slots = 0;
  stop = false;
  return mask;
}
bool s3_peer_rejected(unsigned s) {
  portENTER_CRITICAL(&guard);
  bool b = s < BUDDY_SLOTS && input.arbiter.rejected[s];
  portEXIT_CRITICAL(&guard);
  return b;
}
void s3_peer_disconnected(unsigned s, uint32_t now) {
  if (s >= BUDDY_SLOTS)
    return;
  portENTER_CRITICAL(&guard);
  bool preserve = input.arbiter.owner == s && (source || count || draining);
  if (preserve) {
    source = false;
    ended = now;
    voice_down = false;
    detached = true;
    begin_drain(now);
    input.arbiter.down[s] = false;
    input.arbiter.keys[s] = input.arbiter.suppressed[s] = 0;
    input.arbiter.media[s] = 0;
    stop_slots &= ~(1u << s);
    refresh_hid();
  }
  portEXIT_CRITICAL(&guard);
  if (!preserve)
    s3_peer_link(s, 0);
}
void s3_peer_link(unsigned s, uint32_t g) {
  if (s >= BUDDY_SLOTS)
    return;
  if (s3_voice_owner() == s)
    standalone_abort();
  portENTER_CRITICAL(&guard);
  stop_slots &= ~(1u << s);
  buddy_arbiter_link(&input.arbiter, s, g);
  refresh_hid();
  portEXIT_CRITICAL(&guard);
}
unsigned s3_voice_owner(void) {
  portENTER_CRITICAL(&guard);
  unsigned s = input.arbiter.owner;
  portEXIT_CRITICAL(&guard);
  return s;
}
void s3_peer_voice_key(unsigned s, uint32_t g, bool down, uint32_t now) {
  if (s >= BUDDY_SLOTS)
    return;
  portENTER_CRITICAL(&guard);
  /* A failed capture's queued protocol STOP must be dispatched before this
   * same remote can acquire another capture. Never apply an old STOP to it. */
  if (down && (stop_slots & (1u << s)) && input.arbiter.generation[s] == g) {
    input.arbiter.down[s] = true;
    input.arbiter.rejected[s] = true;
    refresh_hid();
    portEXIT_CRITICAL(&guard);
    return;
  }
  if (down && draining && input.arbiter.owner == s &&
      input.arbiter.generation[s] == g) {
    input.arbiter.down[s] = true;
    input.arbiter.rejected[s] = true;
    portEXIT_CRITICAL(&guard);
    return;
  }
  bool was = input.arbiter.down[s];
  bool accepted;
  if (down && !was && source && !draining && input.arbiter.owner == s &&
      input.arbiter.generation[s] == g && !input.arbiter.rejected[s]) {
    /* Confirm the physical edge of a source-first acquisition. */
    input.arbiter.down[s] = true;
    accepted = true;
  } else
    accepted = buddy_arbiter_voice(&input.arbiter, s, g, down);
  if (accepted) {
    if (down && !was)
      for (unsigned i = 0; i < BUDDY_SLOTS; i++)
        input.arbiter.media[i] &= 7;
    if (down && !was)
      ended = 0;
    voice_down = down;
    if (down && !was) {
      if (!voice_hold) buddy_voice_keyboard(buddy_input_voice_binding(&input, s), s3_host_os(), voice_shortcut);
      voice_hold = true;
      detached = false;
      blocked = false;
    }
    if (!down && was)
      begin_drain(now);
    if (down && !was)
      press_ms = now;
  }
  refresh_hid();
  portEXIT_CRITICAL(&guard);
}
bool s3_peer_wanted(unsigned s, uint32_t g) {
  portENTER_CRITICAL(&guard);
  bool ok = s < BUDDY_SLOTS && g && input.arbiter.generation[s] == g &&
            mounted && !detached && input.arbiter.owner == s &&
            !input.arbiter.rejected[s];
  portEXIT_CRITICAL(&guard);
  return ok;
}
void s3_peer_voice(unsigned s, uint32_t g, const rbp_voice_evt_t *e,
                   uint32_t now) {
  portENTER_CRITICAL(&guard);
  /* ATVV AUDIO_START and HID reports are independent notification streams.
   * A validated native source start may arrive before the physical down edge.
   * Acquire the same arbiter without fabricating a physical key state. */
  if (e->type == RBP_VOICE_EVT_SOURCE_BEGIN && s < BUDDY_SLOTS && g &&
      input.arbiter.generation[s] == g && mounted) {
    if (input.arbiter.owner == BUDDY_NO_OWNER && !input.arbiter.rejected[s] &&
        !(stop_slots & (1u << s))) {
      if (buddy_arbiter_voice(&input.arbiter, s, g, true)) {
        input.arbiter.down[s] = false;
        voice_down = false;
        if (!voice_hold) buddy_voice_keyboard(buddy_input_voice_binding(&input, s), s3_host_os(), voice_shortcut);
        voice_hold = true;
        detached = draining = blocked = false;
        press_ms = now;
        ended = 0;
        for (unsigned i = 0; i < BUDDY_SLOTS; i++)
          input.arbiter.media[i] &= 7;
        refresh_hid();
      }
    }
    if (input.arbiter.owner != s || input.arbiter.rejected[s]) {
      input.arbiter.rejected[s] = true;
      stop_slots |= 1u << s;
    }
  }
  bool tail_event = draining && !detached && source && s < BUDDY_SLOTS && g &&
                    input.arbiter.generation[s] == g &&
                    input.arbiter.owner == s &&
                    e->type != RBP_VOICE_EVT_SOURCE_BEGIN;
  portEXIT_CRITICAL(&guard);
  if (s3_peer_wanted(s, g) || tail_event)
    standalone_voice(e, now);
}
uint64_t s3_peer_keys(unsigned s, uint32_t g, uint64_t bits) {
  portENTER_CRITICAL(&guard);
  uint64_t actions = buddy_input_keys(&input, s, g, bits);
  refresh_hid();
  portEXIT_CRITICAL(&guard);
  return actions;
}
void s3_peer_abort(unsigned s, uint32_t g) {
  portENTER_CRITICAL(&guard);
  bool owner = s < BUDDY_SLOTS && g == input.arbiter.generation[s] &&
               input.arbiter.owner == s;
  portEXIT_CRITICAL(&guard);
  if (owner)
    standalone_abort();
}
void s3_voice_mode_reset(unsigned s) {
  if (s >= BUDDY_SLOTS) return;
  portENTER_CRITICAL(&guard);
  input.voice_mode_toggled[s] = false;
  portEXIT_CRITICAL(&guard);
}
void s3_mapping(unsigned s, const buddy_map_t *m) {
  if (s >= BUDDY_SLOTS)
    return;
  portENTER_CRITICAL(&guard);
  buddy_binding_t old = input.map[s].key[RBP_KEY_VOICE], next = m->key[RBP_KEY_VOICE];
  if (old.kind != next.kind || old.modifiers != next.modifiers || old.value != next.value)
    input.voice_mode_toggled[s] = false;
  input.map[s] = *m;
  input.arbiter.suppressed[s] |= input.arbiter.keys[s];
  input.arbiter.media[s] = 0;
  refresh_hid();
  portEXIT_CRITICAL(&guard);
}
void s3_management(bool active) {
  portENTER_CRITICAL(&guard);
  input.management = active;
  portEXIT_CRITICAL(&guard);
}
#endif
