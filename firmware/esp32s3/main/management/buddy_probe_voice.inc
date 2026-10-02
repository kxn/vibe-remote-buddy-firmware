/* Probe uses the production USB microphone sink; it never emits HID keys. */

static uint32_t pv_buffered;
static slot_t pv_slot;
static bool pv_initialized, pv_active, pv_armed, pv_source,
    pv_release, pv_stop;
static uint32_t pv_samples, pv_capture, pv_started, pv_press, pv_progress,
    pv_prepare, pv_ready_at, pv_rate, pv_codec;
static unsigned pv_peak, pv_decode_error, pv_end_reason;
static int pv_sdk_error;

static char pv_error[96];
static bool pv_learn, pv_ptt;
static unsigned pv_report, pv_usage;
/* Bounded fragment metadata, not a recording. Freeze on capture failure so
 * missing notifications can be distinguished from rejected frame trailers. */
static struct { uint32_t time; uint16_t handle, length; uint8_t bytes[16], saved; } pv_trace[64];
static unsigned pv_trace_count, pv_audio_trace_count;
static void pv_fail(const char *reason) {
  if (!pv_error[0])
    snprintf(pv_error, sizeof pv_error, "%s", reason);
  pv_armed = false;
  pv_source = false;
  pv_stop = true;
}
void buddy_probe_voice_detach(void) {
  if (pv_initialized) {
    s3_gatt_detach(&pv_slot.gatt);
    rc003_adapter_detach(&pv_slot.adapter);
  }
  pv_active = pv_armed = pv_source = false;
  pv_stop = false;
  pv_slot.model.voice_down = false;
  standalone_abort();
}
void buddy_probe_voice_sdk(uint16_t conn, int rc) {
  if (pv_active && pv_slot.conn == conn && rc)
    pv_sdk_error = rc;
}
void buddy_probe_voice_notify(uint16_t h, const uint8_t *data, unsigned len) {
  bool fc = h && h == pv_slot.adapter.unicom.fc && len == 20;
  bool ctl = h && h == pv_slot.adapter.atvv_ctl_handle;
  bool audio = h && h == pv_slot.adapter.atvv_audio_handle;
  bool legacy_audio = false;
  if(pv_active && pv_slot.adapter.legacy.selected) {
    for(unsigned i=0;i<pv_slot.adapter.char_count;i++) {
      const rc003_report_char_t *c=&pv_slot.adapter.chars[i];
      if(c->value_handle!=h || !c->subscribed)continue;
      if(c->report_id==5)ctl=true;
      if(c->report_id>=6 && c->report_id<=8)audio=legacy_audio=true;
    }
  }
  /* Preserve negotiation and first audio headers without copying the stream. */
  if (pv_active && pv_armed && (fc || ctl || (audio && pv_audio_trace_count < 4))) {
    unsigned i = pv_trace_count++ % 64;
    pv_trace[i].time = now_ms();
    pv_trace[i].handle = h;
    pv_trace[i].length = len;
    /* Legacy audio needs only its two-byte transport header, no mSBC body. */
    unsigned limit=legacy_audio?2:16;
    pv_trace[i].saved = fc ? 10 : (len < limit ? len : limit);
    if (fc) {
      memcpy(pv_trace[i].bytes, data, 4);
      memcpy(pv_trace[i].bytes + 4, data + 14, 6);
    } else {
      memcpy(pv_trace[i].bytes, data, pv_trace[i].saved);
    }
    if (audio) pv_audio_trace_count++;
  }
  if (pv_active && pv_armed && pv_learn && pv_slot.adapter.voice_ready) {
    rc003_adapter_t *a = &pv_slot.adapter;
    /* Learn on the board before the production adapter handles this same
     * notification. Never round-trip a start command through the desktop. */
    if (a->unicom.selected) {
      pv_report = 248; pv_usage = 1;
    } else if (!pv_report && !pv_release) {
      for (unsigned i=0; i<a->char_count; i++) {
        unsigned report=a->chars[i].report_id, usage=0, count=0;
        if (a->chars[i].value_handle!=h || a->chars[i].report_type!=1) continue;
        if(report==1 && len==8 && !data[0] && !data[1]) {
          for(unsigned k=2;k<8;k++) if(data[k]) {usage=data[k];count++;}
          if(usage<=3)count=0;
        } else if(report==3 && len==2) {usage=data[0]|((unsigned)data[1]<<8);count=usage?1:0;}
        if(count==1) {
          pv_report=report;pv_usage=usage;
          a->variant.raw_count=1;
          a->variant.raw_keys[0].report=report;
          a->variant.raw_keys[0].usage=usage;
          a->variant.raw_keys[0].key=RBP_KEY_VOICE;
        }
      }
      if(pv_ptt && h==a->atvv_ctl_handle && len==4 && data[0]==4 && data[1]==1) {
        pv_usage=4;
        if(!pv_press)pv_press=now_ms();
      }
      if(h==a->atvv_ctl_handle && len==1 && data[0]==0x08) {
        pv_usage=8; /* Protocol trigger, not an invented HID report. */
        if(!pv_press)pv_press=now_ms();
      }
    }
  }
  if (pv_active)
    s3_gatt_notify(&pv_slot.gatt, h, data, len);
}
static void pv_key(bool down, uint32_t now) {
  pv_slot.model.voice_down = down;
  if (!down && pv_press)
    pv_release = true;
  if (!pv_armed)
    return;
  if (down && !pv_press)
    pv_press = now;
}
static void pv_voice(const rbp_voice_evt_t *e, uint32_t now) {
  if (!pv_armed) return;
  if(e->type==RBP_VOICE_EVT_SOURCE_BEGIN){pv_source=true;pv_started=pv_progress=now;}
  if(e->type==RBP_VOICE_EVT_FORMAT || e->type==RBP_VOICE_EVT_START){pv_rate=e->u.format.sample_rate;pv_codec=e->u.format.codec_id;}
  /* The very same decoder and PCM ring used by ordinary USB microphone input.
   * Probe owns the BLE link; no keyboard shortcut is generated. */
  standalone_voice(e,now);
  if(e->type==RBP_VOICE_EVT_FAULT ||
     (e->type==RBP_VOICE_EVT_END && e->u.end.reason!=RBP_END_NORMAL)) {
    pv_end_reason=e->u.end.reason;
    char reason[64];
    snprintf(reason,sizeof reason,"voice protocol fault (reason=%u)",pv_end_reason);
    pv_fail(reason);
    return;
  }
  unsigned fault=s3_probe_audio_state(&pv_samples,&pv_buffered);
  if(fault){pv_decode_error=fault;pv_fail("audio decode failed");return;}
  if(e->type==RBP_VOICE_EVT_ENCODED)pv_progress=now;
  if(e->type==RBP_VOICE_EVT_END){if(pv_learn && !pv_report && (pv_usage==8 || pv_usage==4))pv_release=true;pv_source=false;pv_armed=false;pv_end_reason=e->u.end.reason;if(pv_end_reason!=RBP_END_NORMAL)pv_fail("voice ended abnormally");}
}
void buddy_probe_voice_tick(uint32_t now) {
  if(!pv_active)return;
  s3_gatt_poll(&pv_slot.gatt);
  rc003_adapter_tick(&pv_slot.adapter,now);
  if(pv_slot.adapter.voice_ready&&!pv_ready_at)pv_ready_at=now;
  if(pv_armed&&!pv_slot.adapter.voice_ready&&(int32_t)(now-pv_prepare)>65000)pv_fail("voice initialization timeout");
  if(pv_armed&&pv_press&&pv_ready_at&&!pv_source&&(int32_t)(now-((int32_t)(pv_press-pv_ready_at)>0?pv_press:pv_ready_at))>3000)pv_fail("no audio stream");
  if(pv_armed&&pv_source&&(int32_t)(now-pv_progress)>2500)pv_fail("audio stalled");
  unsigned fault=s3_probe_audio_state(&pv_samples,&pv_buffered);
  if(pv_armed&&fault){pv_decode_error=fault;pv_fail("audio decode failed");}
  if(pv_stop){pv_stop=false;rc003_adapter_mic_stop(&pv_slot.adapter);standalone_abort();}
}
static bool pv_idle(void) {
  return !pv_armed && !pv_source && !pv_stop && !pv_slot.model.voice_down &&
         !pv_slot.adapter.unicom.active && !pv_slot.adapter.unicom.in_flight &&
         !pv_slot.adapter.atvv.stream_active &&
         !pv_slot.adapter.atvv.open_pending &&
         !pv_slot.adapter.atvv.open_queued &&
         !pv_slot.adapter.atvv.close_queued;
}
uint16_t buddy_probe_voice_command(uint16_t op, const cJSON *q, cJSON *j,
                                   uint16_t conn) {
  if (op == BUDDY_PROBE_VOICE_STATUS) {
    cJSON_AddNumberToObject(j, "interaction", pv_slot.adapter.atvv.caps.interaction);
    cJSON_AddBoolToObject(j, "ptt_requested", pv_ptt);
    cJSON_AddNumberToObject(j, "trigger_report", pv_report);
    cJSON_AddNumberToObject(j, "trigger_usage", pv_usage);
    cJSON_AddBoolToObject(j, "active", pv_active);
    cJSON_AddBoolToObject(j, "idle", pv_idle());
    cJSON_AddBoolToObject(j, "ready",
                          pv_active && pv_slot.adapter.voice_ready &&
                              !pv_slot.model.error[0]);
    cJSON_AddBoolToObject(j, "armed", pv_armed);
    cJSON_AddBoolToObject(j, "recording", pv_source);
    cJSON_AddBoolToObject(j, "released", pv_release);
    cJSON_AddNumberToObject(j, "capture", pv_capture);
    s3_probe_audio_state(&pv_samples,&pv_buffered);
    cJSON_AddNumberToObject(j, "pending_samples", pv_buffered);
    cJSON_AddNumberToObject(j, "samples", pv_samples);
    cJSON_AddNumberToObject(j, "rate", pv_rate);
    cJSON_AddNumberToObject(j, "codec", pv_codec);
    cJSON_AddNumberToObject(j, "peak", pv_peak);
    pv_peak = 0;
    cJSON_AddNumberToObject(j, "decode_error", pv_decode_error);
    cJSON_AddNumberToObject(j, "sdk_error", pv_sdk_error);
    cJSON_AddNumberToObject(j, "end_reason", pv_end_reason);
    cJSON_AddStringToObject(j, "adapter_error", pv_slot.model.error);
    cJSON_AddNumberToObject(j, "adapter_state", pv_slot.adapter.state);
    cJSON_AddNumberToObject(j, "prepare_ms", pv_active ? (pv_ready_at ? pv_ready_at : now_ms()) - pv_prepare : 0);
    cJSON_AddStringToObject(j, "error",
                            pv_error[0] ? pv_error : pv_slot.model.error);
    return 0;
  }
  if (op == BUDDY_PROBE_VOICE_CANCEL) {
    pv_armed = pv_source = false;
    pv_buffered = 0;
      pv_release = false;
    if (pv_active)
      pv_stop = true;
    return 0;
  }
  if (op == BUDDY_PROBE_VOICE_READ) {
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(q, "transport"))) {
      cJSON_AddNumberToObject(j, "capture", pv_capture);
      cJSON_AddNumberToObject(j, "samples", pv_samples);

      cJSON_AddNumberToObject(j, "buffered", pv_buffered);
      /* Keep optional details off the near-full, 512-byte status response. */
      cJSON_AddNumberToObject(j, "protocol_mode_length", pv_slot.adapter.protocol_mode_length==UINT16_MAX?-1:pv_slot.adapter.protocol_mode_length);
      cJSON_AddNumberToObject(j, "protocol_mode_value", pv_slot.adapter.protocol_mode_value==UINT8_MAX?-1:pv_slot.adapter.protocol_mode_value);
      cJSON_AddBoolToObject(j, "protocol_mode_unverified", pv_slot.adapter.protocol_mode_unverified);
      return 0;
    }
    uint32_t diagnostic;
    if (buddy_u32(q, "diagnostic", &diagnostic)) {
      if (pv_armed || pv_source) return RBP_STATUS_BUSY;
      unsigned count = pv_trace_count < 64 ? pv_trace_count : 64;
      if (diagnostic >= count) return RBP_STATUS_NOT_FOUND;
      unsigned i = (pv_trace_count - count + diagnostic) % 64;
      char hex[33] = {0};
      for (unsigned k=0;k<pv_trace[i].saved;k++)
        snprintf(hex+2*k,3,"%02x",pv_trace[i].bytes[k]);
      cJSON_AddNumberToObject(j,"handle",pv_trace[i].handle);
      cJSON_AddNumberToObject(j,"length",pv_trace[i].length);
      cJSON_AddNumberToObject(j,"index",diagnostic);
      cJSON_AddNumberToObject(j,"time_ms",pv_trace[i].time);
      cJSON_AddStringToObject(j,"fragment",hex);
      return 0;
    }
    return RBP_STATUS_UNSUPPORTED; /* Audio uses UAC, not management packets. */
  }
  if (op != BUDDY_PROBE_VOICE_ARM)
    return RBP_STATUS_UNSUPPORTED;
  uint32_t family, crc, report, usage;
  bool ptt = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(q, "ptt"));
  bool learn = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(q, "learn"));
  if (!buddy_u32(q, "family", &family) || (family != 1 && family != 2 && family != 3) ||
      (ptt && (family!=1 || !learn)) ||
      !buddy_u32(q, "map_crc", &crc) || !crc ||
      !buddy_u32(q, "report", &report) ||
      (!learn && report != 1 && report != 3 && !(family == 2 && report == 248)) ||
      !buddy_u32(q, "usage", &usage) || (!learn && !usage) ||
      (learn && (report || usage)) ||
      usage > (report == 1 ? 255 : 65535) || (report == 248 && usage != 1))
    return RBP_STATUS_INVALID_ARGUMENT;
  if (pv_active && (pv_slot.adapter.variant.family != family ||
                    pv_slot.adapter.variant.map_crc != crc ||
                    pv_learn != learn || pv_ptt != ptt ||
                    (!learn && (pv_slot.adapter.variant.raw_keys[0].report != report ||
                    pv_slot.adapter.variant.raw_keys[0].usage != usage)))) {
    if(!pv_idle())return RBP_STATUS_BUSY;
    buddy_probe_voice_detach();
  }
  if (pv_active && pv_slot.model.error[0]) {
    cJSON_AddStringToObject(j, "error", pv_slot.model.error);
    cJSON_AddNumberToObject(j, "sdk_error", pv_sdk_error);
    return RBP_STATUS_DEVICE_ERROR;
  }
  if (!pv_idle())
    return RBP_STATUS_BUSY;
  standalone_abort();
  s3_probe_audio_reset();
  pv_samples = pv_started = pv_press = pv_peak = pv_decode_error =
      pv_end_reason = pv_rate = pv_codec = 0;
  pv_learn=learn;pv_ptt=ptt;pv_report=report;pv_usage=usage;
  if(learn) {pv_slot.adapter.variant.raw_count=0;memset(pv_slot.adapter.keys_by_report,0,sizeof pv_slot.adapter.keys_by_report);}
  pv_sdk_error = 0;
  pv_error[0] = 0;
  pv_release = false;
  pv_capture++;
  pv_buffered = 0;
  pv_trace_count = pv_audio_trace_count = 0;
  pv_slot.model.generation = pv_capture;
  pv_armed = true;
  pv_prepare = now_ms();
  pv_ready_at = pv_active && pv_slot.adapter.voice_ready ? pv_prepare : 0;
  if (!pv_active) {
    if (!pv_initialized) {
      s3_gatt_init(&pv_slot.gatt, 4);
      pv_initialized = true;
    }
    memset(&pv_slot.model, 0, sizeof pv_slot.model);
    pv_slot.model.slot = 4;
    pv_slot.model.generation = pv_capture;
    rc003_adapter_init(&pv_slot.adapter, s3_gatt_client(), &pv_slot.gatt,
                       &pv_slot.model);
    pv_slot.adapter.atvv.requested_interaction=ptt?1:0;
    pv_slot.adapter.variant.family = family;
    pv_slot.adapter.variant.map_crc = crc;
    pv_slot.adapter.variant.raw_count = (learn || report == 248) ? 0 : 1;
    pv_slot.adapter.variant.raw_keys[0].report = report;
    pv_slot.adapter.variant.raw_keys[0].usage = usage;
    pv_slot.adapter.variant.raw_keys[0].key = RBP_KEY_VOICE;
    pv_slot.conn = conn;
    pv_active = true;
    s3_gatt_attach(&pv_slot.gatt, &pv_slot.adapter, conn);
    rc003_adapter_start(&pv_slot.adapter, now_ms());
  }
  cJSON_AddNumberToObject(j, "capture", pv_capture);
  return 0;
}
