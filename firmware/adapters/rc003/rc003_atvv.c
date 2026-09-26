#include "../../product/faults.h"
#include "rc003_atvv.h"
#include <string.h>
#include "../../debug/trace.h"

/* ---- wire opcodes (ATVV 1.0) ---- */
#define ATVV_AUDIO_STOP     0x00u
#define ATVV_AUDIO_START    0x04u
#define ATVV_START_SEARCH   0x08u
#define ATVV_AUDIO_SYNC     0x0Au
#define ATVV_CAPS_RESP      0x0Bu
#define ATVV_MIC_OPEN_ERROR 0x0Cu

#define ATVV_MIC_OPEN 0x0Cu
#define ATVV_MIC_CLOSE 0x0Du
#define ATVV_MIC_EXTEND 0x0Eu

/* error codes */
#define ATVV_ERR_BUSY 0x0F80u

void rc003_atvv_init(rc003_atvv_t *a, const rc003_atvv_sink_t *sink)
{
    memset(a, 0, sizeof(*a));
    a->sink = *sink;
    a->profile_init_without_sync = false; /* no unverified initial state */
}

uint16_t atvv_build_get_caps(uint8_t *out)
{
    /* GET_CAPS: version 1.0 (BE), legacy 0x0003, interaction models 0x03 */
    out[0] = 0x0A;
    out[1] = 0x01;
    out[2] = 0x00;
    out[3] = 0x00;
    out[4] = 0x03;
#ifdef RBP_MULTI_STANDALONE
    /* ATVV 1.0 section 3: the central grants audio ownership before OPEN. */
    out[5] = 0x00;
#else
    out[5] = 0x03;
#endif
    return 6;
}

uint16_t atvv_build_mic_open(uint8_t *out)
{
    out[0] = ATVV_MIC_OPEN;
    out[1] = 0x01; /* ATVV Capture mode */
    return 2;
}

uint16_t atvv_build_mic_close(uint8_t *out, uint8_t stream_id)
{
    out[0] = ATVV_MIC_CLOSE;
    out[1] = stream_id;
    return 2;
}

uint16_t atvv_build_mic_extend(uint8_t *out, uint8_t stream_id)
{
    out[0] = ATVV_MIC_EXTEND;
    out[1] = stream_id;
    return 2;
}

bool atvv_parse_caps(const uint8_t *d,uint16_t len,atvv_caps_t *out)
{
    if(!d || !out || len<9 || d[0]!=ATVV_CAPS_RESP || d[1]!=1 || d[2]!=0)return false;
    uint16_t size=((uint16_t)d[5]<<8)|d[6];
    if(!d[3] || (d[3]&~3) || (d[4]!=0 && d[4]!=1 && d[4]!=3) ||
       !size || size>ATVV_MAX_FRAME || (d[7]&~1u) || d[8])return false;
    memset(out,0,sizeof *out);out->version_hi=1;out->codecs=d[3];
    out->interaction=d[4];out->frame_size=size;
    /* Byte 7 bit 0 requests DLE, byte 8 alone is reserved. Bytes after 8
     * are optional remote firmware data, not a malformed CAPS response. */
    out->request_dle=(d[7]&1)!=0;return true;
}
static uint32_t codec_to_rate(uint8_t c) {return c==1?8000:c==2?16000:0;}
static void fault(rc003_atvv_t *a,uint8_t reason) {
    rbp_fault_record(RBP_FAULT_ATVV,1,reason,a->frame_no);
    DT(DT_ATVV,DT_ERROR,3,reason,a->frame_no,a->frame_bytes,a->remote_stream_id);
    if(a->failed)return;
    a->failed=true;a->decoder_ready=false;
    if(a->sink.on_fault)a->sink.on_fault(a->sink.user,reason);
    else if(a->sink.on_ended)a->sink.on_ended(a->sink.user,reason);
    rc003_atvv_request_stop(a);
}
static void await_sync(rc003_atvv_t *a, uint8_t reason) {
    if(!a->waiting_sync) {
        a->waiting_sync=true;
        a->sync_wait_since=a->now_ms;
        rbp_fault_record(RBP_FAULT_ATVV,ATVV_AUDIO_SYNC,reason,a->frame_no);
    }
    a->decoder_ready=false;
}
static bool sync_expired(rc003_atvv_t *a) {
    return a->waiting_sync && a->now_ms-a->sync_wait_since>=ATVV_SYNC_WAIT_MS;
}
static void announce_format(rc003_atvv_t *a) {
    rbp_audio_format_t f={RBP_CODEC_IMA_HI,a->sample_rate,ATVV_MAX_FRAME,1,4,1,a->codec_config};
    if(a->announced) a->sink.on_format(a->sink.user,&f);
    else {a->announced=true;a->sink.on_started(a->sink.user,&f);}
}
bool rc003_atvv_on_control(rc003_atvv_t *a,const uint8_t*d,uint16_t len) {
    DT(DT_ATVV,DT_INFO,1,(d && len)?d[0]:255,len,a->stream_active,a->now_ms);
    if(d && len)DT_BLOB(DT_ATVV,10,d,len);
    if(!d || !len)return false;
    switch(d[0]) {
    case ATVV_CAPS_RESP: {
        /* ATVV 1.0 has one wire layout. An audio decoder seed profile does
         * not authorize padding missing fields or swapping codec/model. */
        if(a->stream_active || !atvv_parse_caps(d,len,&a->caps))return false;
#ifdef RBP_MULTI_STANDALONE
        if(a->caps.interaction != 0 && !(a->requested_interaction==1 && a->caps.interaction==1)) {
            rbp_fault_record(RBP_FAULT_ATVV,ATVV_CAPS_RESP,a->caps.interaction,0);
            return false; /* Peer must honor the advertised interaction models. */
        }
#endif
        a->caps_valid=true;a->sample_rate=(a->caps.codecs&2)?16000:8000;return true;
    }
    case ATVV_AUDIO_START: {
        if(a->stream_active) {fault(a,RBP_END_SOURCE_DATA_LOST);return false;}
        if(len!=4 || !a->caps_valid || !codec_to_rate(d[2]) || !(a->caps.codecs&d[2]) ||
           (d[1]!=0 && d[1]!=1 && d[1]!=3) ||
           (d[1]==0 ? d[3]!=0 : (!d[3] || d[3]>0x80)) ||
           (d[1]!=0 && d[1]!=a->caps.interaction))return false;
        bool stop_after_open=a->closing && a->open_pending;
        a->stream_active=true;a->remote_stream_id=d[3];a->frame_no=0;a->frame_bytes=0;
        a->sample_rate=codec_to_rate(d[2]);a->decoder_ready=false;a->saw_sync=false;
        a->waiting_sync=!a->profile_init_without_sync;a->sync_wait_since=a->now_ms;
        a->failed=false;a->closing=false;a->open_pending=false;a->open_queued=false;a->close_queued=false;
        a->last_audio_ms=a->last_extend_ms=a->now_ms;
        /* RC003 profile: reference receivers reset to 0/0 on each capture.
         * Other profiles still require a trusted SYNC, not an assumed seed. */
        a->announced=false;
        a->rejected_stream=d[1]!=0 && (!a->sink.want_voice || !a->sink.want_voice(a->sink.user));
        if(a->rejected_stream) {rc003_atvv_request_stop(a);return true;}
        if(a->sink.on_source_begin)a->sink.on_source_begin(a->sink.user);
        if(a->profile_init_without_sync){memset(a->codec_config,0,4);a->decoder_ready=true;announce_format(a);}
        if(stop_after_open)rc003_atvv_request_stop(a);
        return true;
    }
    case ATVV_AUDIO_SYNC: {
        if(!a->stream_active || a->failed || a->rejected_stream)return false;
        if(sync_expired(a)) {fault(a,RBP_END_SOURCE_DATA_LOST);return false;}
        if(len!=7 || !codec_to_rate(d[1]) || !(a->caps.codecs&d[1]) || d[6]>88) {
            await_sync(a,RBP_END_INVALID_ENCODED_DATA);return false;
        }
        uint16_t no=((uint16_t)d[2]<<8)|d[3];
        /* First SYNC establishes the origin. Later forward gaps and partial
         * frames recover at this authoritative seed, without guessing bytes.
         * Old/backward syncs cannot rewind an active stream (wrap is valid). */
        uint16_t gap=(uint16_t)(no-a->frame_no);
        if(a->announced && gap>=0x8000u) {
            await_sync(a,RBP_END_SOURCE_DATA_LOST);return false;
        }
        if(a->announced && (gap || a->frame_bytes))
            rbp_fault_record(RBP_FAULT_ATVV,ATVV_AUDIO_SYNC,RBP_END_SOURCE_DATA_LOST,gap);
        a->frame_no=no;a->frame_bytes=0;a->waiting_sync=false;
        a->codec_config[0]=d[5];a->codec_config[1]=d[4];a->codec_config[2]=d[6];a->codec_config[3]=0;
        a->decoder_ready=true;a->saw_sync=true;a->sample_rate=codec_to_rate(d[1]);
        announce_format(a);
        return true;
    }
    case ATVV_AUDIO_STOP: {
        if(len!=2 || !a->stream_active)return false;
        uint8_t reason=(d[1]==0 || d[1]==2 || d[1]==4)?RBP_END_NORMAL:RBP_END_DEVICE_ERROR;
        if(reason!=RBP_END_NORMAL)rbp_fault_record(RBP_FAULT_ATVV,ATVV_AUDIO_STOP,d[1],0);
        a->stream_active=false;a->decoder_ready=false;a->closing=false;a->close_queued=false;
        if(a->rejected_stream){a->rejected_stream=false;return true;}
        a->sink.on_ended(a->sink.user,reason);return true;
    }
    case ATVV_START_SEARCH: {
        return len==1 && rc003_atvv_request_start(a);
    }
    case ATVV_MIC_OPEN_ERROR:
        if(len!=3)return false;
        if((((uint16_t)d[1]<<8)|d[2])!=ATVV_ERR_BUSY)
            rbp_fault_record(RBP_FAULT_ATVV,ATVV_MIC_OPEN_ERROR,((uint16_t)d[1]<<8)|d[2],a->stream_active);
        DT(DT_ATVV,DT_INFO,4,((uint16_t)d[1]<<8)|d[2],a->open_pending,a->stream_active,a->remote_stream_id);
        if(a->open_pending && !a->stream_active) {
            a->open_queued=false; /* submitted request is never replayed */
            if((((uint16_t)d[1]<<8)|d[2])==ATVV_ERR_BUSY)return true;
            a->open_pending=false;a->closing=false;
            if(a->sink.on_fault)a->sink.on_fault(a->sink.user,RBP_END_DEVICE_ERROR);
            return true;
        }
        a->open_pending=false;a->open_queued=false;a->closing=false;
        if(a->stream_active && (((uint16_t)d[1]<<8)|d[2])!=ATVV_ERR_BUSY)fault(a,RBP_END_DEVICE_ERROR);
        return true;
    default:return false;
    }
}
void rc003_atvv_on_audio(rc003_atvv_t *a,const uint8_t*d,uint16_t len) {
    DT(DT_ATVV,DT_DETAIL,2,len,a->frame_no,a->frame_bytes,a->decoder_ready);
    if(!a->stream_active || a->failed || a->rejected_stream)return;
    if(!d || !len || len>ATVV_MAX_FRAME || !a->caps_valid || !a->caps.frame_size) {
        await_sync(a,RBP_END_INVALID_ENCODED_DATA);return;
    }
    if(!a->decoder_ready) {await_sync(a,RBP_END_INVALID_ENCODED_DATA);return;}
    a->last_audio_ms=a->now_ms;
    uint32_t bytes=a->frame_bytes+len;
    a->frame_no=(uint16_t)(a->frame_no+bytes/a->caps.frame_size);
    a->frame_bytes=(uint16_t)(bytes%a->caps.frame_size);
    a->sink.on_encoded(a->sink.user,d,len);
}
bool rc003_atvv_tick(rc003_atvv_t *a,uint32_t now_ms) {
    a->now_ms=now_ms;
    if(a->open_pending && (int32_t)(now_ms-a->open_deadline_ms)>=0) {
        a->open_pending=false;a->open_queued=false;a->closing=false;
        if(a->sink.on_fault)a->sink.on_fault(a->sink.user,RBP_END_DEVICE_ERROR);
    }
    if(a->open_queued) {
        uint8_t cmd[2];atvv_build_mic_open(cmd);
        if(a->sink.write_tx(a->sink.user,cmd,2))a->open_queued=false;
    }
    if(a->close_queued) {
        if((int32_t)(now_ms-a->open_deadline_ms)>=0) {
            a->close_queued=false;fault(a,RBP_END_DEVICE_ERROR);return false;
        }
        return rc003_atvv_request_stop(a);
    }
    if(a->failed && a->stream_active && !a->closing)return rc003_atvv_request_stop(a);
    if(!a->stream_active || a->closing || a->failed)return false;
    if(sync_expired(a)) {fault(a,RBP_END_SOURCE_DATA_LOST);return false;}
    if(now_ms-a->last_extend_ms>=5000) {
        uint8_t cmd[2];atvv_build_mic_extend(cmd,a->remote_stream_id);
        if(a->sink.write_tx(a->sink.user,cmd,2)) {a->last_extend_ms=now_ms;return true;}
    }
    return false;
}
bool rc003_atvv_request_stop(rc003_atvv_t *a) {
    if(a->open_pending && !a->stream_active) {
        if(a->open_queued){a->open_queued=false;a->open_pending=false;}
        else a->closing=true;
        return true;
    }
    if(!a->stream_active)return false;
    if(a->closing)return true;
    uint8_t cmd[2];atvv_build_mic_close(cmd,a->remote_stream_id);
    bool ok=a->sink.write_tx(a->sink.user,cmd,2);
    if(ok){a->closing=true;a->close_queued=false;}
    else {if(!a->close_queued)a->open_deadline_ms=a->now_ms+1000;a->close_queued=true;}
    return ok;
}
bool rc003_atvv_request_start(rc003_atvv_t *a) {
    if(!a->caps_valid || a->stream_active || a->open_pending ||
       !a->sink.want_voice || !a->sink.want_voice(a->sink.user))return false;
    uint8_t cmd[2];atvv_build_mic_open(cmd);
    a->open_queued=!a->sink.write_tx(a->sink.user,cmd,2);
    a->open_pending=true;a->closing=false;a->open_deadline_ms=a->now_ms+ATVV_OPEN_WAIT_MS;return true;
}
