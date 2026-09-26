#include "faults.h"
/* RBP/3.0: bounded encoded-audio records; no board-side decoder. */
#include "rbp_server.h"
#include "../debug/trace.h"
#include "device_model.h"
#include "rbp/frame.h"
#include "rbp/tlv.h"
#include <string.h>
#ifdef RBP_STANDALONE
#include "standalone.h"
#endif

/* buffer sizes may be tuned per build target (host tests vs 32 KiB MCU) */
#ifndef RBP_OUT_RING_SIZE
#define RBP_OUT_RING_SIZE 2048u
#endif
#ifndef RBP_AUDIO_POOL_SIZE
#define RBP_AUDIO_POOL_SIZE 2048u
#endif
#ifndef RBP_CTL_SLOTS
#define RBP_CTL_SLOTS     6u
#endif
#ifndef RBP_MEDIA_DEPTH
#define RBP_MEDIA_DEPTH   16u
#endif

#define OUT_RING_SIZE RBP_OUT_RING_SIZE
#define AUDIO_POOL_SIZE RBP_AUDIO_POOL_SIZE
#define CTL_SLOTS     RBP_CTL_SLOTS
#define MEDIA_DEPTH   RBP_MEDIA_DEPTH
#ifndef RBP_CTL_BYTES
#define RBP_CTL_BYTES (CTL_SLOTS * RBP_MAX_PAYLOAD)
#endif

#define HEARTBEAT_TIMEOUT_MS 5000u
#define CANDIDATE_TTL_MS     60000u
#define PAIR_DEADLINE_MS     60000u
#define FORGET_DEADLINE_MS   5000u
#define VOICE_STOP_MS        2000u
/* Local no-audio policy (ATVVoice also defaults to 5 s), not codec validation. */
#define VOICE_FIRST_AUDIO_MS 5000u
#define VOICE_PROGRESS_MS    2000u

#define CTL_FLUSH_BURST 4u

/* media marker types */
#define MEDIA_START  1u
#define MEDIA_FORMAT 2u
#define MEDIA_END    3u

/* async op kinds */
#define OPK_PAIR   1u
#define OPK_FORGET 2u

typedef struct {
    uint8_t used;
    uint8_t kind;
    uint16_t status;
    uint16_t opcode;
    uint32_t request_id;
    uint32_t connection_id;
    uint32_t session;
    uint16_t len;
    uint16_t offset;
    uint32_t order;
} ctl_slot_t;

typedef struct {
    uint32_t stream, connection, order;
    uint16_t opcode, len;
} media_marker_t;

typedef struct {
    uint8_t used;
    uint8_t buf[RBP_KEYS_STRUCT_SIZE];
    uint32_t order;
    uint32_t connection;
} key_item_t;

struct rbp_server {
    rbp_server_cfg_t cfg;

    uint32_t now_ms;
    uint32_t order_counter;

    /* output */
    uint8_t out_ring[OUT_RING_SIZE];
    size_t out_head, out_len;

    /* input framing */
    rbp_rxparser_t rx;
    rbp_rx_event_ctx_t rx_ctx;

    /* control queue */
    ctl_slot_t ctl[CTL_SLOTS];
    uint8_t ctl_payload[RBP_CTL_BYTES];
    uint16_t ctl_used;

    /* session */
    bool session_active;
    uint32_t session_id;
    uint32_t session_counter, connection_counter;
    rbp_txseq_t txseq;
    uint32_t last_req_ms;
    uint32_t req_hwm;
    bool have_client_seq;
    uint32_t client_tx_hwm;

    /* async ops */
    bool op_active;
    bool pair_ready_pending;
    uint32_t op_id;
    uint8_t op_kind;
    uint32_t op_deadline_ms;
    uint32_t op_peer_id;
    uint32_t op_connection_id;
    struct {
        bool used;
        uint32_t op_id;
        uint16_t result;
        uint32_t peer_id;
        uint32_t connection_id;
        bool uncertain;
    } ops_done[RBP_MAX_OPERATION_RESULTS];
    uint8_t ops_done_head, ops_done_count;

    /* find */
    bool find_active;
    bool find_valid;
    uint32_t search_id;
    uint32_t find_end_ms;
    rbp_candidate_t cand[RBP_MAX_CANDIDATES];
    uint8_t cand_count;

    /* prompt */
    bool prompt_active;
    bool pair_cancel_requested;
    uint32_t prompt_id;
    uint8_t prompt_method;
    uint32_t prompt_number;
    uint32_t prompt_expiry_ms;

    /* device model */
    rbp_link_state_t link_state;
    uint32_t connection_id;
    rbp_peer_record_t peer;
    bool peer_valid;
    uint8_t battery;
    uint8_t charging;
    uint8_t voice_state;
    uint8_t voice_interaction;
    uint32_t voice_sample_rate;
    bool reconnect_paused;
    char link_msg[48];

    /* keys */
    bool events_enabled;
    uint64_t pressed_bits;
    uint32_t input_seq;
    uint64_t keys_captured_us;
    key_item_t key_q[RBP_KEY_QUEUE_DEPTH];
    uint8_t key_q_head, key_q_count;

    /* voice */
    bool voice_enabled;
    bool voice_denied; /* explicit host refusal; distinct from pending negotiation */
    uint32_t voice_resume_peer; /* explicit enable intent, scoped to session + bond */
    bool waiting_idle;
    uint32_t stream_id;
    uint32_t next_stream_id;
    uint32_t frame_seq;
    uint64_t delivered_samples, delivered_bytes, source_samples, source_bytes;
    uint32_t delivered_units, delivered_epoch, source_units, epoch;
    uint32_t unit_offset, unit_size, unit_samples, format_max_unit;
    uint32_t source_started_ms;
    const rbp_audio_caps_t *voice_caps;
    uint32_t accepted_max_unit;
    uint8_t accepted_codecs[96], accepted_len;
    bool delivering;
    bool ending;
    uint32_t stream_connection;
    bool stop_requested;
    uint32_t stop_deadline_ms;
    bool capture_limited;
    uint32_t capture_deadline_ms;
    bool voice_ok_deferred;
    uint16_t voice_ok_deferred_opcode;
    uint32_t voice_ok_deferred_req;
    uint32_t voice_ok_deferred_conn;
    bool voice_ok_deferred_is_enable;

    uint8_t audio_pool[AUDIO_POOL_SIZE];
    size_t audio_head, audio_len;

    media_marker_t media_q[MEDIA_DEPTH];
    uint8_t media_head, media_count;

    /* source tracking */
    bool source_active;
#ifdef RBP_DEBUG
    bool debug_delivery;
#endif
    uint32_t first_audio_deadline;
    uint32_t progress_deadline;
    bool got_audio_this_stream;

    /* stats */
    uint32_t stat_protocol_errors;
    uint32_t stat_input_resets;
    uint32_t stat_voice_overruns;
    uint32_t stat_voice_errors;
};

/* ===================== helpers ===================== */

static uint32_t next_order(rbp_server_t *s) { return ++s->order_counter; }
static uint64_t now_us64(const rbp_server_t *s) { return (uint64_t)s->now_ms * 1000u; }

static uint32_t rng_u32(rbp_server_t *s)
{
    uint32_t v = s->cfg.rng ? s->cfg.rng() : 0;
    if (v == 0) v = 1;
    return v;
}

static bool ring_write(uint8_t *ring, size_t cap, size_t *head, size_t *len,
                       const uint8_t *data, size_t n)
{
    if (cap - *len < n) return false;
    size_t tail = *head + *len;
    if (tail >= cap) tail -= cap;
    size_t first = cap - tail;
    if (first > n) first = n;
    memcpy(ring + tail, data, first);
    if (n > first) memcpy(ring, data + first, n - first);
    *len += n;
    return true;
}

static void copy_name(char *dst, size_t cap, const char *src)
{
    size_t i = 0;
    if (src)
        for (; src[i] && i + 1 < cap; i++) dst[i] = src[i];
    dst[i] = 0;
}

/* ===================== forward declarations ===================== */

static void ctl_enqueue(rbp_server_t *s, uint8_t kind, uint16_t opcode,
                        uint16_t status, uint32_t request_id,
                        uint32_t connection_id, const uint8_t *payload, uint16_t len);
static void send_device_state(rbp_server_t *s, uint32_t conn_ctx);
static void op_complete(rbp_server_t *s, uint32_t op_id, uint16_t result,
                        uint32_t peer_id, uint32_t connection_id, bool uncertain);
static void voice_finish_stream(rbp_server_t *s, uint8_t reason, bool clear_pending);
static void session_stop_delivery(rbp_server_t *s);
static void session_teardown(rbp_server_t *s);
static void session_create(rbp_server_t *s);
static void device_info_tlv(rbp_server_t *s, rbp_tlv_writer_t *w);
static bool out_try_frame(rbp_server_t *s, const rbp_header_t *h,
                          const uint8_t *payload, uint16_t plen);
static void out_drain(rbp_server_t *s);
static bool ctl_peek_order(const rbp_server_t *s, uint32_t *order);
static bool ctl_pop(rbp_server_t *s);
static void ctl_clear(rbp_server_t *s);
static bool key_push(rbp_server_t *s, uint8_t kind, uint8_t reason,
                     uint64_t bits, uint64_t captured_us);
static void keys_emit(rbp_server_t *s, uint8_t kind, uint8_t reason,
                      uint64_t bits, uint64_t captured_us);
static bool keys_flush_one(rbp_server_t *s);
static bool media_flush_one(rbp_server_t *s);

size_t rbp_server_buffer_size(void) { return sizeof(rbp_server_t); }

rbp_server_t *rbp_server_firmware_instance(void) {
    static rbp_server_t instance;
    return &instance;
}

static void session_stop_delivery(rbp_server_t *s)
{
    s->delivering = false;
    s->ending = false;
    s->stream_id = 0;
    s->media_head = 0;
    s->media_count = 0;
    s->audio_head = 0;
    s->audio_len = 0;
    s->accepted_len=0;s->accepted_max_unit=0;
    s->voice_resume_peer=0;
    s->voice_ok_deferred = false;
    s->key_q_head = 0;
    s->key_q_count = 0;
    s->events_enabled = false;
    s->voice_enabled = false;
    s->voice_denied = false;
    s->waiting_idle = false;
    s->stop_requested = false;
}

static void request_pair_cancel(rbp_server_t *s) {
    if(s->pair_cancel_requested)return;
    s->pair_cancel_requested=true;s->prompt_active=false;
    s->cfg.backend.pair_cancel(s->cfg.backend.user);
}

static void session_teardown(rbp_server_t *s)
{
    DT(DT_SESSION,DT_INFO,2,s->session_id,s->source_active,s->op_active,s->out_len);
    /* Backend calls may synchronously report completion. Withdraw the
     * old session and operation ownership before invoking any of them. */
    bool cancel_pair=(s->op_active && s->op_kind==OPK_PAIR) || s->prompt_active;
    bool stop_find=s->find_active;
    bool stop_source=(s->source_active || s->voice_enabled) && !s->stop_requested;
#ifdef RBP_STANDALONE
    stop_source=false;
#endif
    bool was_stopping=s->source_active && s->stop_requested;
    uint32_t stop_deadline=s->stop_deadline_ms;
    if(s->op_kind==OPK_PAIR)s->op_active=false;
    s->prompt_active=false;s->find_active=false;
    s->session_active=false;s->session_id=0;
    ctl_clear(s);
    /* Preserve at most the remainder of one already-started wire frame. */
    for(size_t i=0;i<s->out_len;i++) {
        if(s->out_ring[(s->out_head+i)%OUT_RING_SIZE]==0) {s->out_len=i+1;break;}
    }
    session_stop_delivery(s);
    if(s->source_active) {
        s->stop_requested=true;
        s->stop_deadline_ms=was_stopping?stop_deadline:s->now_ms+VOICE_STOP_MS;
    }
    s->ops_done_head = 0;
    s->ops_done_count = 0;
    if(cancel_pair)request_pair_cancel(s);
    if(stop_find)s->cfg.backend.stop_find(s->cfg.backend.user);
    if(stop_source)s->cfg.backend.voice_request_stop(s->cfg.backend.user);
}
static void session_create(rbp_server_t *s)
{
    session_teardown(s);
    s->session_active = true;
    if(s->session_counter==UINT32_MAX) {s->session_active=false;return;}
    s->session_id=++s->session_counter;
    rbp_txseq_init(&s->txseq);
    s->req_hwm = 0;
    s->have_client_seq = false;
    s->client_tx_hwm = 0;
    s->last_req_ms = s->now_ms;
}

static bool out_try_frame(rbp_server_t *s, const rbp_header_t *h,
                          const uint8_t *payload, uint16_t plen)
{
    uint8_t tmp[RBP_MAX_ENCODED];
    rbp_txseq_t next=s->txseq;
    if(!next.next_tx_seq || next.next_tx_seq==UINT32_MAX) {session_teardown(s);return false;}
    if(OUT_RING_SIZE-s->out_len<RBP_HEADER_SIZE+plen+6u)return false;
    size_t n = rbp_frame_encode(h, &next, payload, plen, tmp);
    if (n == 0) return false;
    if (OUT_RING_SIZE - s->out_len < n) return false;
    ring_write(s->out_ring, OUT_RING_SIZE, &s->out_head, &s->out_len, tmp, n);
    s->txseq=next;
    return true;
}

static void out_drain(rbp_server_t *s)
{
    while (s->out_len > 0) {
        size_t chunk = s->out_len > 64 ? 64 : s->out_len;
        /* never let one out() call span the ring wrap */
        if (chunk > OUT_RING_SIZE - s->out_head) chunk = OUT_RING_SIZE - s->out_head;
        size_t sent = s->cfg.out(s->cfg.out_user, s->out_ring + s->out_head, chunk);
        if (sent == 0) break;
        s->out_head = (s->out_head + sent) % OUT_RING_SIZE;
        s->out_len -= sent;
        if (sent < chunk) break;
    }
}

static bool ctl_peek_order(const rbp_server_t *s, uint32_t *order)
{
    const ctl_slot_t *best = NULL;
    for (unsigned i = 0; i < CTL_SLOTS; i++) {
        const ctl_slot_t *sl = &s->ctl[i];
        if (sl->used && (!best || sl->order < best->order)) best = sl;
    }
    if (!best) return false;
    *order = best->order;
    return true;
}

static bool ctl_pop(rbp_server_t *s)
{
    uint32_t order = 0;
    if (!ctl_peek_order(s, &order)) return false;
    for (unsigned i = 0; i < CTL_SLOTS; i++) {
        ctl_slot_t *sl = &s->ctl[i];
        if (sl->used && sl->order == order) {
            rbp_header_t h;
            memset(&h, 0, sizeof(h));
            h.kind = sl->kind;
            h.session_id = sl->session;
            h.opcode = sl->opcode;
            h.status = sl->status;
            h.request_id = sl->request_id;
            h.connection_id = sl->connection_id;
            if (!out_try_frame(s, &h, s->ctl_payload+sl->offset, sl->len)) return false;
            sl->used = 0;
            memmove(s->ctl_payload+sl->offset,s->ctl_payload+sl->offset+sl->len,s->ctl_used-sl->offset-sl->len);
            s->ctl_used-=sl->len;
            for(unsigned j=0;j<CTL_SLOTS;j++)if(s->ctl[j].used && s->ctl[j].offset>=sl->offset+sl->len)s->ctl[j].offset-=sl->len;
            return true;
        }
    }
    return false;
}

static void ctl_clear(rbp_server_t *s)
{
    for (unsigned i = 0; i < CTL_SLOTS; i++) s->ctl[i].used = 0;
    s->ctl_used=0;
}

static bool key_push(rbp_server_t *s, uint8_t kind, uint8_t reason,
                     uint64_t bits, uint64_t captured_us)
{
    if (s->key_q_count >= RBP_KEY_QUEUE_DEPTH) return false;
    uint8_t idx = (uint8_t)((s->key_q_head + s->key_q_count) % RBP_KEY_QUEUE_DEPTH);
    s->key_q[idx].used = 1;
    memset(s->key_q[idx].buf, 0, RBP_KEYS_STRUCT_SIZE);
    for (int i = 0; i < 4; i++) s->key_q[idx].buf[i] = (uint8_t)(s->input_seq >> (8 * i));
    for (int i = 0; i < 8; i++) s->key_q[idx].buf[4 + i] = (uint8_t)(captured_us >> (8 * i));
    for (int i = 0; i < 8; i++) s->key_q[idx].buf[12 + i] = (uint8_t)(bits >> (8 * i));
    s->key_q[idx].buf[20] = kind;
    s->key_q[idx].buf[21] = reason;
    s->key_q[idx].order = next_order(s);
    s->key_q[idx].connection=s->connection_id;
    s->key_q_count++;
    return true;
}

static void keys_emit(rbp_server_t *s, uint8_t kind, uint8_t reason,
                      uint64_t bits, uint64_t captured_us)
{
    if (!s->events_enabled) return; /* delivery gated by EVENTS_ENABLE */
    if (!key_push(s, kind, reason, bits, captured_us)) {
        /* overflow: clear queue, emit reset then recovery snapshot */
        s->key_q_head = 0;
        s->key_q_count = 0;
        s->stat_input_resets++;
        s->input_seq++;
        (void)key_push(s, RBP_KEYS_KIND_RESET, RBP_KEYS_REASON_OVERFLOW, 0, captured_us);
        s->input_seq++;
        (void)key_push(s, RBP_KEYS_KIND_SNAPSHOT, RBP_KEYS_REASON_NONE,
                       s->pressed_bits, captured_us);
    }
}

void rbp_server_on_keys(rbp_server_t *s, const rbp_keys_report_t *rep)
{
#ifdef RBP_STANDALONE
    standalone_keys(s->cfg.profile,rep->pressed_bits);
#endif
    switch (rep->kind) {
    case RBP_KEYS_KIND_PHYSICAL:
        s->pressed_bits = rep->pressed_bits;
        s->input_seq++;
        s->keys_captured_us = rep->captured_us;
        keys_emit(s, RBP_KEYS_KIND_PHYSICAL, rep->reason, rep->pressed_bits,
                  rep->captured_us);
        break;
    case RBP_KEYS_KIND_RESET:
        s->input_seq++;
        s->keys_captured_us = rep->captured_us;
        keys_emit(s, RBP_KEYS_KIND_RESET, rep->reason, 0, rep->captured_us);
        if (rep->pressed_bits) {
            s->pressed_bits = rep->pressed_bits;
            s->input_seq++;
            keys_emit(s, RBP_KEYS_KIND_SNAPSHOT, RBP_KEYS_REASON_NONE,
                      rep->pressed_bits, rep->captured_us);
        } else {
            s->pressed_bits = 0;
        }
        break;
    case RBP_KEYS_KIND_SNAPSHOT:
        s->pressed_bits = rep->pressed_bits;
        s->keys_captured_us = rep->captured_us;
        keys_emit(s, RBP_KEYS_KIND_SNAPSHOT, RBP_KEYS_REASON_NONE,
                  rep->pressed_bits, rep->captured_us);
        break;
    default:
        break;
    }
}
static bool keys_flush_one(rbp_server_t *s)
{
    if (s->key_q_count == 0) return false;
    key_item_t *it = &s->key_q[s->key_q_head];
    rbp_header_t h;
    memset(&h, 0, sizeof(h));
    h.kind = RBP_KIND_EVENT;
    h.session_id = s->session_id;
    h.opcode = RBP_OP_KEYS_STATE_EV;
    h.connection_id = it->connection;
    if (!out_try_frame(s, &h, it->buf, RBP_KEYS_STRUCT_SIZE)) return false;
    it->used = 0;
    s->key_q_head = (uint8_t)((s->key_q_head + 1) % RBP_KEY_QUEUE_DEPTH);
    s->key_q_count--;
    return true;
}

static void send_device_state(rbp_server_t *s, uint32_t conn_ctx)
{
    rbp_tlv_writer_t w;
    rbp_tlv_writer_init(&w);
    device_info_tlv(s, &w);
    ctl_enqueue(s, RBP_KIND_EVENT, RBP_OP_DEVICE_STATE_EV, 0, 0, conn_ctx, w.buf, w.len);
}

static void op_complete(rbp_server_t *s, uint32_t op_id, uint16_t result,
                        uint32_t peer_id, uint32_t connection_id, bool uncertain)
{
    uint8_t idx = (uint8_t)((s->ops_done_head + s->ops_done_count) % RBP_MAX_OPERATION_RESULTS);
    if (s->ops_done_count < RBP_MAX_OPERATION_RESULTS) {
        s->ops_done_count++;
    } else {
        s->ops_done_head = (uint8_t)((s->ops_done_head + 1) % RBP_MAX_OPERATION_RESULTS);
    }
    s->ops_done[idx].used = 1;
    s->ops_done[idx].op_id = op_id;
    s->ops_done[idx].result = result;
    s->ops_done[idx].peer_id = peer_id;
    s->ops_done[idx].connection_id = connection_id;
    s->ops_done[idx].uncertain = uncertain;
    rbp_tlv_writer_t w;
    rbp_tlv_writer_init(&w);
    rbp_tlv_put_u32(&w, 1, op_id);
    rbp_tlv_put_u8(&w, 2, RBP_OPSTATE_COMPLETED);
    rbp_tlv_put_u16(&w, 3, result);
    rbp_tlv_put_u32(&w, 4, peer_id);
    rbp_tlv_put_u32(&w, 5, connection_id);
    rbp_tlv_put_bool(&w, 6, uncertain);
    ctl_enqueue(s, RBP_KIND_EVENT, RBP_OP_OPERATION_EV, 0, 0, connection_id, w.buf, w.len);
}

void rbp_server_init(rbp_server_t *s,const rbp_server_cfg_t *cfg,void *buf,size_t buf_len) {
    (void)buf;(void)buf_len;memset(s,0,sizeof *s);s->cfg=*cfg;
    if(!s->cfg.max_capture_ms || s->cfg.max_capture_ms>120000)s->cfg.max_capture_ms=120000;
    if(!s->cfg.max_keys || s->cfg.max_keys>RBP_MAX_KEYS)s->cfg.max_keys=RBP_MAX_KEYS;
    rbp_rxparser_init(&s->rx,0);s->battery=255;s->voice_state=RBP_VOICE_ABSENT;
    s->voice_interaction=RBP_VI_UNAVAILABLE;s->link_state=RBP_LINK_UNBOUND;
    rbp_peer_record_t rec;
    if(s->cfg.store.load && s->cfg.store.load(s->cfg.store.user,&rec)) {
        s->peer=rec;s->peer_valid=rec.peer_id!=0;
        s->link_state=s->peer_valid?RBP_LINK_DISCONNECTED:RBP_LINK_UNBOUND;
    }
}
static void ctl_enqueue(rbp_server_t *s,uint8_t kind,uint16_t opcode,uint16_t status,
                        uint32_t request_id,uint32_t connection_id,const uint8_t *payload,uint16_t len) {
    ctl_slot_t *slot=NULL;
    for(unsigned i=0;i<CTL_SLOTS;i++)if(!s->ctl[i].used){slot=&s->ctl[i];break;}
    if(!slot || len>RBP_MAX_PAYLOAD || len>RBP_CTL_BYTES-s->ctl_used) {
        s->stat_protocol_errors++;session_teardown(s);return;
    }
    *slot=(ctl_slot_t){0};slot->used=1;slot->kind=kind;slot->opcode=opcode;slot->status=status;
    slot->request_id=request_id;slot->connection_id=connection_id;slot->session=s->session_id;
    slot->len=len;slot->offset=s->ctl_used;slot->order=next_order(s);
    if(len)memcpy(s->ctl_payload+s->ctl_used,payload,len);
    s->ctl_used+=len;
}
static void device_info_tlv(rbp_server_t *s,rbp_tlv_writer_t *w) {
    rbp_tlv_put_u32(w,1,s->connection_id);rbp_tlv_put_u32(w,2,s->peer_valid?s->peer.peer_id:0);
    rbp_tlv_put_u8(w,3,(uint8_t)s->link_state);
    const rbp_device_profile_t *p=s->link_state==RBP_LINK_READY?s->cfg.profile:NULL;
    rbp_tlv_put_text(w,4,p?p->model_id:"");
    rbp_tlv_put_text(w,5,p?p->display_name:(s->peer_valid?s->peer.name:""));
    rbp_tlv_put_u32(w,6,p?p->catalog_revision:0);rbp_tlv_put_u8(w,7,p?p->key_count:0);
    rbp_tlv_put_u8(w,8,s->voice_state);rbp_tlv_put_u32(w,9,s->voice_state==RBP_VOICE_READY?s->voice_sample_rate:0);
    rbp_tlv_put_u8(w,10,s->battery);rbp_tlv_put_u8(w,11,s->charging);
    rbp_tlv_put_bool(w,12,s->voice_enabled);rbp_tlv_put_bool(w,13,s->delivering);
    rbp_tlv_put_u32(w,14,s->delivering?s->stream_id:0);rbp_tlv_put_bool(w,15,s->waiting_idle);
    rbp_tlv_put_u16(w,16,(s->link_state==RBP_LINK_ERROR || s->voice_state==RBP_VOICE_FAILED)?RBP_STATUS_DEVICE_ERROR:
                         s->voice_state==RBP_VOICE_UNSUPPORTED?RBP_STATUS_VOICE_UNAVAILABLE:RBP_STATUS_OK);
    if(s->link_msg[0])rbp_tlv_put_text(w,17,s->link_msg);
    rbp_tlv_put_u8(w,18,s->voice_interaction);
    rbp_tlv_put_u32(w,19,s->voice_state!=RBP_VOICE_ABSENT?s->cfg.max_capture_ms:0);
}

static void dispatch_request(rbp_server_t *,const rbp_header_t *,const uint8_t *,uint16_t);
static void process_frame(rbp_server_t *,const rbp_header_t *,const uint8_t *,uint16_t,bool);
static void server_rx_event(rbp_rx_event_ctx_t *ctx,void *user) {
    rbp_server_t *s=user;
    if(ctx->event==RBP_RX_FRAME) {
        DT(DT_SESSION,DT_INFO,7,ctx->header.opcode,ctx->header.tx_seq,ctx->header.session_id,ctx->crc_ok);
        process_frame(s,&ctx->header,ctx->payload,ctx->payload_len,ctx->crc_ok);
    }
    else if(ctx->event!=RBP_RX_NONE && s->session_active){s->stat_protocol_errors++;session_teardown(s);}
}
void rbp_server_on_usb_rx(rbp_server_t *s,const uint8_t *data,size_t len,uint32_t now_ms) {
    s->now_ms=now_ms;rbp_rx_drain(&s->rx,data,len,now_ms,&s->rx_ctx,server_rx_event,s);
}
static uint32_t audio_u32(const uint8_t *p) {
    return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}
static void audio_put(uint8_t *p,uint64_t n,unsigned len) {while(len--){*p++=(uint8_t)n;n>>=8;}}
static uint64_t sample_add(uint64_t a,uint32_t b) {
    return a==UINT64_MAX || b==UINT32_MAX || UINT64_MAX-a<=b ? UINT64_MAX : a+b;
}
static void audio_read(const rbp_server_t *s,size_t offset,uint8_t *out,size_t len) {
    size_t pos=(s->audio_head+offset)%AUDIO_POOL_SIZE,first=AUDIO_POOL_SIZE-pos;
    if(first>len)first=len;
    memcpy(out,s->audio_pool+pos,first);
    if(len>first)memcpy(out+first,s->audio_pool,len-first);
}
void rbp_server_set_voice_caps(rbp_server_t *s,const rbp_audio_caps_t *caps) {s->voice_caps=caps;}
static bool codec_accepted(rbp_server_t *s,const rbp_audio_format_t *f) {
    if(!f || !f->codec_id || f->codec_id>=0x80000000u || !f->codec_revision ||
       !f->sample_rate || f->sample_rate>384000 || !f->channels || f->channels>8 ||
       f->config_len>RBP_AUDIO_CONFIG_MAX || (f->config_len && !f->config) ||
       !f->max_unit_bytes || f->max_unit_bytes>s->accepted_max_unit || !s->voice_caps ||
       f->max_unit_bytes>s->voice_caps->max_unit_bytes)return false;
    bool advertised=false;
    for(unsigned i=0;i<s->voice_caps->count;i++)
        if(s->voice_caps->codecs[i].id==f->codec_id && s->voice_caps->codecs[i].revision==f->codec_revision)advertised=true;
    if(!advertised)return false;
    for(unsigned i=0;i<s->accepted_len;i+=6)
        if(audio_u32(s->accepted_codecs+i)==f->codec_id &&
           (uint16_t)(s->accepted_codecs[i+4]|(s->accepted_codecs[i+5]<<8))==f->codec_revision)return true;
    return false;
}
static bool media_enqueue(rbp_server_t *s,uint16_t op,const uint8_t *p,uint16_t len) {
    if(s->media_count>=MEDIA_DEPTH || AUDIO_POOL_SIZE-s->audio_len<len)rbp_server_flush_output(s);
    if(!s->session_active || s->media_count>=MEDIA_DEPTH || AUDIO_POOL_SIZE-s->audio_len<len)return false;
    unsigned idx=(s->media_head+s->media_count)%MEDIA_DEPTH;
    s->media_q[idx]=(media_marker_t){s->stream_id,s->stream_connection,next_order(s),op,len};
    ring_write(s->audio_pool,AUDIO_POOL_SIZE,&s->audio_head,&s->audio_len,p,len);
    s->media_count++;return true;
}
/* Compact only the current stream's uncommitted media. Earlier END and
 * pending START (including its copied config) retain ownership and order. */
static void media_discard_current(rbp_server_t *s) {
    uint8_t payload[RBP_MAX_PAYLOAD];size_t read=0,write=0;unsigned kept=0;
    for(unsigned i=0;i<s->media_count;i++) {
        media_marker_t m=s->media_q[(s->media_head+i)%MEDIA_DEPTH];
        audio_read(s,read,payload,m.len);read+=m.len;
        if(m.stream==s->stream_id && m.opcode!=RBP_OP_VOICE_STARTED_EV)continue;
        ring_write(s->audio_pool,AUDIO_POOL_SIZE,&s->audio_head,&write,payload,m.len);
        s->media_q[(s->media_head+kept++)%MEDIA_DEPTH]=m;
    }
    s->media_count=(uint8_t)kept;s->audio_len=write;
}
static void voice_finish_stream(rbp_server_t *s,uint8_t reason,bool clear_pending) {
    if(!s->delivering)return;
    if(!clear_pending && s->unit_offset){reason=RBP_END_INVALID_ENCODED_DATA;clear_pending=true;}
    if(s->ending && !clear_pending)return;
    if(clear_pending)media_discard_current(s);
    rbp_tlv_writer_t w;rbp_tlv_writer_init(&w);
    rbp_tlv_put_u32(&w,1,s->stream_id);rbp_tlv_put_u8(&w,2,reason);
    rbp_tlv_put_u64(&w,3,clear_pending?s->delivered_bytes:s->source_bytes);
    rbp_tlv_put_u64(&w,4,now_us64(s));
    rbp_tlv_put_u32(&w,5,clear_pending?s->delivered_units:s->source_units);
    rbp_tlv_put_u64(&w,6,clear_pending?s->delivered_samples:s->source_samples);
    rbp_tlv_put_u32(&w,7,clear_pending?s->delivered_epoch:s->epoch);
    if(!media_enqueue(s,RBP_OP_VOICE_ENDED_EV,w.buf,w.len)){session_teardown(s);return;}
    s->ending=true;
    if(clear_pending){s->delivering=false;s->ending=false;s->stream_id=0;}
}
static void voice_fault(rbp_server_t *s,uint8_t reason) {
    bool had_stream=s->delivering;
    s->stat_voice_errors++;
    if(reason==RBP_END_BUFFER_OVERRUN)s->stat_voice_overruns++;
    voice_finish_stream(s,reason,true);
    if(s->source_active && !s->stop_requested) {
        s->stop_requested=true;s->stop_deadline_ms=s->now_ms+VOICE_STOP_MS;
        s->cfg.backend.voice_request_stop(s->cfg.backend.user);
    }
    if(reason==RBP_END_UNSUPPORTED_FORMAT || !had_stream) {
        s->voice_resume_peer=0;
        s->voice_enabled=false;
        s->voice_state=reason==RBP_END_UNSUPPORTED_FORMAT?RBP_VOICE_UNSUPPORTED:RBP_VOICE_FAILED;
        send_device_state(s,s->connection_id);
    }
}
static bool queue_format(rbp_server_t *s,const rbp_audio_format_t *f,bool start) {
    if(!codec_accepted(s,f)){voice_fault(s,RBP_END_UNSUPPORTED_FORMAT);return false;}
    if(s->unit_offset || s->epoch==UINT32_MAX){voice_fault(s,RBP_END_SOURCE_DATA_LOST);return false;}
    rbp_tlv_writer_t w;rbp_tlv_writer_init(&w);uint32_t epoch=start?1:s->epoch+1;
    rbp_tlv_put_u32(&w,1,s->stream_id);rbp_tlv_put_u32(&w,2,epoch);
    rbp_tlv_put_u32(&w,3,f->codec_id);rbp_tlv_put_u16(&w,4,f->codec_revision);
    rbp_tlv_put_u32(&w,5,f->sample_rate);rbp_tlv_put_u8(&w,6,f->channels);
    rbp_tlv_put_bytes(&w,7,f->config,f->config_len);rbp_tlv_put_u32(&w,8,f->max_unit_bytes);
    rbp_tlv_put_u64(&w,9,s->source_samples);
    rbp_tlv_put_u64(&w,10,start?(uint64_t)s->source_started_ms*1000:now_us64(s));
    rbp_tlv_put_u32(&w,11,s->source_units+1);
    if(!media_enqueue(s,start?RBP_OP_VOICE_STARTED_EV:RBP_OP_VOICE_FORMAT_EV,w.buf,w.len))return false;
    s->epoch=epoch;s->format_max_unit=f->max_unit_bytes;return true;
}
static bool media_flush_one(rbp_server_t *s) {
    if(!s->session_active || !s->media_count)return false;
    media_marker_t m=s->media_q[s->media_head];
    if(OUT_RING_SIZE-s->out_len<RBP_HEADER_SIZE+m.len+6u)return false;
    uint8_t payload[RBP_MAX_PAYLOAD];audio_read(s,0,payload,m.len);
    rbp_header_t h;memset(&h,0,sizeof h);h.kind=m.opcode==RBP_OP_VOICE_DATA?RBP_KIND_AUDIO:RBP_KIND_EVENT;
    h.opcode=m.opcode;h.session_id=s->session_id;h.connection_id=m.connection;
    if(!out_try_frame(s,&h,payload,m.len))return false;
    if(m.stream==s->stream_id) {
        if(m.opcode==RBP_OP_VOICE_DATA) {
            s->delivered_bytes+=m.len-RBP_AUDIO_DATA_HEADER;
            if(audio_u32(payload+20)+m.len-RBP_AUDIO_DATA_HEADER==audio_u32(payload+16)) {
                s->delivered_units++;s->delivered_samples=sample_add(s->delivered_samples,audio_u32(payload+32));
            }
        } else if(m.opcode==RBP_OP_VOICE_STARTED_EV || m.opcode==RBP_OP_VOICE_FORMAT_EV) {
            s->delivered_epoch=audio_u32(payload+12); /* TLV tag 2's u32 */
        } else if(m.opcode==RBP_OP_VOICE_ENDED_EV) {
            s->delivering=false;s->ending=false;s->stream_id=0;
        }
    }
    s->audio_head=(s->audio_head+m.len)%AUDIO_POOL_SIZE;s->audio_len-=m.len;
    s->media_head=(uint8_t)((s->media_head+1)%MEDIA_DEPTH);s->media_count--;return true;
}
void rbp_server_on_voice(rbp_server_t *s,const rbp_voice_evt_t *ev,uint32_t now_ms) {
    s->now_ms=now_ms;
#ifdef RBP_STANDALONE
    standalone_voice(ev,now_ms);
    return; /* USB Audio owns capture; CDC sessions only own management. */
#endif
    switch(ev->type) {
    case RBP_VOICE_EVT_SOURCE_BEGIN:
        s->source_active=true;s->source_started_ms=now_ms;s->got_audio_this_stream=false;
        s->first_audio_deadline=now_ms+VOICE_FIRST_AUDIO_MS;s->progress_deadline=now_ms+VOICE_PROGRESS_MS;
        s->capture_deadline_ms=now_ms+s->cfg.max_capture_ms;s->stop_requested=false;s->capture_limited=false;
        break;
    case RBP_VOICE_EVT_START:
        s->voice_sample_rate=ev->u.start.sample_rate;
        if(!s->source_active) { /* adapters must report physical source start first */
            voice_fault(s,RBP_END_INVALID_ENCODED_DATA);break;
        }
        if(!s->voice_enabled || !s->session_active || s->waiting_idle){
            s->stop_requested=true;s->stop_deadline_ms=now_ms+VOICE_STOP_MS;
            s->cfg.backend.voice_request_stop(s->cfg.backend.user);break;
        }
        if(s->delivering){s->stat_voice_overruns++;voice_finish_stream(s,RBP_END_BUFFER_OVERRUN,true);}
        if(!codec_accepted(s,&ev->u.start)){voice_fault(s,RBP_END_UNSUPPORTED_FORMAT);break;}
        if(s->next_stream_id==UINT32_MAX){session_teardown(s);break;}
        s->stream_id=++s->next_stream_id;s->stream_connection=s->connection_id;
        s->frame_seq=0;s->epoch=0;s->source_units=s->delivered_units=0;s->unit_offset=0;
        s->source_bytes=s->delivered_bytes=s->source_samples=s->delivered_samples=0;
        s->delivered_epoch=1;s->delivering=true;s->ending=false;
        if(!queue_format(s,&ev->u.start,true)) {
            /* No START was queued: do not invent an END for this identity. */
            s->delivering=false;s->stream_id=0;voice_fault(s,RBP_END_BUFFER_OVERRUN);
        }
        break;
    case RBP_VOICE_EVT_FORMAT:
        s->voice_sample_rate=ev->u.format.sample_rate;
        if(s->delivering && !s->ending && !queue_format(s,&ev->u.format,false) && s->delivering)voice_fault(s,RBP_END_BUFFER_OVERRUN);
        break;
    case RBP_VOICE_EVT_ENCODED: {
        if(ev->u.encoded.erasures_before){voice_fault(s,RBP_END_SOURCE_DATA_LOST);break;}
        if(!s->source_active)break;
        s->got_audio_this_stream=true;s->progress_deadline=now_ms+VOICE_PROGRESS_MS;
        if(!s->delivering || s->ending)break;
        uint32_t size=ev->u.encoded.unit_size,offset=ev->u.encoded.offset,samples=ev->u.encoded.samples;
        uint16_t len=ev->u.encoded.len;const uint8_t *bytes=ev->u.encoded.data;
        if(size>s->format_max_unit){voice_fault(s,RBP_END_UNSUPPORTED_FORMAT);break;}
        if(!len || !bytes || !size || offset!=s->unit_offset || offset>size || len>size-offset ||
           (offset && (size!=s->unit_size || samples!=s->unit_samples)) || s->source_units==UINT32_MAX) {
            voice_fault(s,RBP_END_INVALID_ENCODED_DATA);break;
        }
        s->unit_size=size;s->unit_samples=samples;
        while(len && s->delivering) {
            uint16_t take=len>256?256:len;
            uint8_t payload[RBP_AUDIO_DATA_HEADER+256];
            if(s->frame_seq==UINT32_MAX){voice_fault(s,RBP_END_INVALID_ENCODED_DATA);break;}
            audio_put(payload,s->stream_id,4);audio_put(payload+4,s->frame_seq+1,4);
            audio_put(payload+8,s->epoch,4);audio_put(payload+12,s->source_units+1,4);
            audio_put(payload+16,size,4);audio_put(payload+20,s->unit_offset,4);
            audio_put(payload+24,s->source_samples,8);audio_put(payload+32,samples,4);audio_put(payload+36,0,4);
            memcpy(payload+40,bytes,take);
            if(!media_enqueue(s,RBP_OP_VOICE_DATA,payload,40+take)){voice_fault(s,RBP_END_BUFFER_OVERRUN);break;}
            s->frame_seq++;s->source_bytes+=take;s->unit_offset+=take;bytes+=take;len-=take;
            if(s->unit_offset==size){s->unit_offset=0;s->source_units++;s->source_samples=sample_add(s->source_samples,samples);}
        }
        break;
    }
    case RBP_VOICE_EVT_FAULT:voice_fault(s,ev->u.end.reason);break;
    case RBP_VOICE_EVT_END: {
        s->source_active=false;s->waiting_idle=false;uint8_t reason=ev->u.end.reason;
        if(reason==RBP_END_NORMAL && s->capture_limited)reason=RBP_END_CAPTURE_LIMIT;
        else if(s->stop_requested && reason==RBP_END_NORMAL)reason=RBP_END_REQUESTED_STOP;
        bool drain=reason==RBP_END_NORMAL || reason==RBP_END_REQUESTED_STOP || reason==RBP_END_CAPTURE_LIMIT;
        voice_finish_stream(s,reason,!drain);break;
    }
    }
}

void rbp_server_on_voice_state(rbp_server_t *s, uint8_t voice_state,
                               uint8_t interaction, uint32_t sample_rate)
{
    if (s->voice_state != voice_state || s->voice_sample_rate != sample_rate ||
        s->voice_interaction != interaction) {
        s->voice_state = voice_state;
        s->voice_interaction = interaction;
        s->voice_sample_rate = sample_rate;
        if (voice_state != RBP_VOICE_READY && s->voice_enabled) {
            s->voice_enabled = false;
        }
        if(voice_state==RBP_VOICE_READY && s->session_active && s->peer_valid &&
           s->voice_resume_peer==s->peer.peer_id && !s->reconnect_paused && s->voice_caps &&
           s->accepted_max_unit>=s->voice_caps->max_unit_bytes) {
            bool compatible=false;
            for(unsigned i=0;i<s->accepted_len;i+=6)for(unsigned j=0;j<s->voice_caps->count;j++)
                if(audio_u32(s->accepted_codecs+i)==s->voice_caps->codecs[j].id &&
                   (uint16_t)(s->accepted_codecs[i+4]|(s->accepted_codecs[i+5]<<8))==s->voice_caps->codecs[j].revision)compatible=true;
            if(compatible){s->voice_enabled=true;s->waiting_idle=s->source_active;}
        }
        send_device_state(s, s->connection_id);
    }
}

/* ===================== radio inputs ===================== */

static uint32_t new_connection_id(rbp_server_t *s)
{
    if(s->connection_counter==UINT32_MAX)return 0;
    return ++s->connection_counter;
}

void rbp_server_on_link(rbp_server_t *s, rbp_link_state_t state, uint32_t connection_id,
                        uint32_t now_ms)
{
    s->now_ms = now_ms;
    if(state==RBP_LINK_READY && s->op_active && s->op_kind==OPK_PAIR) {
        s->pair_ready_pending=true;state=RBP_LINK_INITIALIZING;
    }
    if(state==RBP_LINK_DISCONNECTED || state==RBP_LINK_UNBOUND ||
       state==RBP_LINK_ERROR || state==RBP_LINK_UNSUPPORTED)s->pair_ready_pending=false;
    rbp_link_state_t old = s->link_state;
    uint32_t ctx_id = s->connection_id;

    if(!s->connection_id && (state==RBP_LINK_CONNECTING || state==RBP_LINK_PAIRING ||
       state==RBP_LINK_INITIALIZING || state==RBP_LINK_READY)) {
        /* Optional explicit logical id is used by test radios, never a WCH handle. */
        s->connection_id=connection_id?connection_id:new_connection_id(s);
        s->input_seq=0;s->pressed_bits=0;s->keys_captured_us=now_us64(s);
        s->link_msg[0]=0;
        if(!s->connection_id)state=RBP_LINK_ERROR;
    }

    s->link_state = state;
#ifdef RBP_STANDALONE
    if(state==RBP_LINK_DISCONNECTED || state==RBP_LINK_UNBOUND ||
       state==RBP_LINK_ERROR || state==RBP_LINK_UNSUPPORTED ||
       (old==RBP_LINK_READY && state!=RBP_LINK_READY))standalone_abort();
#endif

    if (old == RBP_LINK_READY && state != RBP_LINK_READY) {
        if (s->delivering)
            voice_finish_stream(s, RBP_END_LINK_LOST, true);
        rbp_keys_report_t rep;
        memset(&rep, 0, sizeof(rep));
        rep.kind = RBP_KEYS_KIND_RESET;
        rep.reason = RBP_KEYS_REASON_LINK_LOST;
        rep.captured_us = now_us64(s);
        rbp_server_on_keys(s, &rep);
        s->events_enabled = false;
    }
    if (state == RBP_LINK_DISCONNECTED || state == RBP_LINK_UNBOUND ||
        state == RBP_LINK_ERROR || state == RBP_LINK_UNSUPPORTED) {
        s->connection_id = 0;
        s->source_active=false;s->stop_requested=false;s->waiting_idle=false;
        s->voice_enabled=false;s->voice_state=RBP_VOICE_ABSENT;s->voice_sample_rate=0;
        s->voice_interaction=RBP_VI_UNAVAILABLE;s->battery=255;s->charging=0;
    }
    send_device_state(s, ctx_id);
}

void rbp_server_on_link_message(rbp_server_t *s, const char *msg)
{
    copy_name(s->link_msg, sizeof(s->link_msg), msg);
}

void rbp_server_on_scan_candidate(rbp_server_t *s, const rbp_candidate_t *cand)
{
    if (!s->find_active) return;
    for (uint8_t i = 0; i < s->cand_count; i++) {
        rbp_candidate_t *c = &s->cand[i];
        if (c->candidate_id == cand->candidate_id) {
            c->signal = cand->signal;
            if(cand->support>c->support)c->support=cand->support;
            if(cand->name_len){copy_name(c->name, sizeof(c->name), cand->name);c->name_len=cand->name_len;}
            return;
        }
    }
    if (s->cand_count >= RBP_MAX_CANDIDATES) return;
    rbp_candidate_t *c = &s->cand[s->cand_count++];
    *c = *cand;
    copy_name(c->name, sizeof(c->name), cand->name);
    c->name_len = cand->name_len;
}

void rbp_server_on_scan_done(rbp_server_t *s, uint8_t reason, uint32_t now_ms)
{
    s->now_ms = now_ms;
    if (!s->find_active) return;
    s->find_active = false;
    s->find_end_ms = now_ms;
    s->find_valid = true;
    rbp_tlv_writer_t w;
    rbp_tlv_writer_init(&w);
    rbp_tlv_put_u32(&w, 1, s->search_id);
    rbp_tlv_put_u8(&w, 2, reason);
    ctl_enqueue(s, RBP_KIND_EVENT, RBP_OP_FIND_DONE_EV, 0, 0, 0, w.buf, w.len);
}

void rbp_server_on_pair_prompt(rbp_server_t *s, uint8_t method, uint32_t number,
                               uint32_t remaining_ms)
{
    if(!s->op_active || s->op_kind!=OPK_PAIR || s->pair_cancel_requested)return;
    int32_t operation_remaining=(int32_t)(s->op_deadline_ms-s->now_ms);
    if(operation_remaining<=0)return;
    if(remaining_ms>(uint32_t)operation_remaining)remaining_ms=(uint32_t)operation_remaining;
    s->prompt_active = true;
    do {
        s->prompt_id = rng_u32(s);
    } while (s->prompt_id == 0);
    s->prompt_method = method;
    s->prompt_number = number;
    s->prompt_expiry_ms = s->now_ms + remaining_ms;
    rbp_tlv_writer_t w;
    rbp_tlv_writer_init(&w);
    rbp_tlv_put_u32(&w, 1, s->op_id);
    rbp_tlv_put_u32(&w, 2, s->prompt_id);
    rbp_tlv_put_u8(&w, 3, method);
    if (method == RBP_PROMPT_CONFIRM_NUMBER || method == RBP_PROMPT_DISPLAY_PASSKEY)
        rbp_tlv_put_u32(&w, 4, number);
    rbp_tlv_put_u32(&w, 5, remaining_ms);
    ctl_enqueue(s, RBP_KIND_EVENT, RBP_OP_PAIR_PROMPT_EV, 0, 0, s->op_connection_id,
                w.buf, w.len);
}

void rbp_server_on_pair_done(rbp_server_t *s, uint16_t status, bool peer_committed,
                             const rbp_peer_record_t *peer, uint32_t connection_id)
{
    bool publish_ready=s->pair_ready_pending;
    s->pair_ready_pending=false;
    if(status==RBP_STATUS_OK && !peer_committed)status=RBP_STATUS_STORAGE_FAILED;
    s->prompt_active = false;
    bool uncertain = false;

    if (status == RBP_STATUS_OK) {
        if(!peer || !peer->peer_id)status=RBP_STATUS_PAIRING_FAILED;
        else {
            rbp_peer_record_t committed=*peer;committed.auto_reconnect=true;
            if(s->cfg.store.save && !s->cfg.store.save(s->cfg.store.user,&committed)) {
                status=RBP_STATUS_STORAGE_FAILED;uncertain=true;
            } else {
                s->peer=committed;s->peer_valid=true;
                /* A successful new bond starts a new reconnect intent.
                 * FORGET/DISCONNECT paused the previous binding only.
                 * Clear after durable commit, never on pairing failure. */
                s->reconnect_paused=false;
            }
        }
    }

    if (s->op_active && s->op_kind == OPK_PAIR) {
        s->op_active = false;
        uint32_t pid = (status == RBP_STATUS_OK && s->peer_valid) ? s->peer.peer_id : 0;
        op_complete(s, s->op_id, status, pid, s->connection_id, uncertain);
    }

    if (status == RBP_STATUS_OK) {
        if(publish_ready)rbp_server_on_link(s,RBP_LINK_READY,connection_id,s->now_ms);
        else if(s->link_state!=RBP_LINK_READY)rbp_server_on_link(s, RBP_LINK_INITIALIZING, connection_id, s->now_ms);
        else send_device_state(s,s->connection_id);
    } else {
        copy_name(s->link_msg, sizeof(s->link_msg), "pairing failed");
        rbp_server_on_link(s,RBP_LINK_ERROR,0,s->now_ms);
    }
}

void rbp_server_on_forget_done(rbp_server_t *s, uint16_t status, bool uncertain)
{
    if(status==RBP_STATUS_OK && (!s->cfg.store.clear || !s->cfg.store.clear(s->cfg.store.user))) {
        status=RBP_STATUS_STORAGE_FAILED;uncertain=true;
    }
    if (status == RBP_STATUS_OK) {
        s->peer_valid = false;
        memset(&s->peer, 0, sizeof(s->peer));
        rbp_server_on_link(s,RBP_LINK_UNBOUND,0,s->now_ms);
    }
    if (s->op_active && s->op_kind == OPK_FORGET) {
        s->op_active = false;
        op_complete(s, s->op_id, status, 0, 0, uncertain);
    }
    if(status!=RBP_STATUS_OK)send_device_state(s, 0);
}

void rbp_server_on_peer_changed(rbp_server_t *s, const rbp_peer_record_t *peer)
{
    if(!s->peer_valid || s->peer.peer_id!=peer->peer_id)s->voice_resume_peer=0;
    s->peer = *peer;
    s->peer_valid = peer->peer_id != 0;
}

void rbp_server_on_battery(rbp_server_t *s, uint8_t level, uint8_t charging)
{
    if (s->battery != level || s->charging != charging) {
        s->battery = level;
        s->charging = charging;
        send_device_state(s, s->connection_id);
    }
}

void rbp_server_on_usb_gone(rbp_server_t *s, uint32_t now_ms)
{
    s->now_ms = now_ms;
    session_teardown(s);
    s->out_head = 0;
    s->out_len = 0;
    ctl_clear(s);
}

/* ===================== tick ===================== */

void rbp_server_flush_output(rbp_server_t *s)
{
    out_drain(s);
    /* scheduling: globally ordered heads with a control-burst fairness cap */
    uint8_t ctl_sent = 0;
    for (;;) {
        uint32_t ctl_o = 0, key_o = 0, media_o = 0;
        bool has_ctl = ctl_peek_order(s, &ctl_o);
        bool has_key = s->key_q_count > 0;
        bool has_media = s->media_count > 0;
        if (!has_ctl && !has_key && !has_media) break;
        key_o = has_key ? s->key_q[s->key_q_head].order : 0;
        media_o = has_media ? s->media_q[s->media_head].order : 0;
        uint32_t min_o = 0;
        bool min_is_ctl = false, min_is_key = false;
        if (has_ctl) { min_o = ctl_o; min_is_ctl = true; }
        else if (!has_key && !has_media) break;
        if (has_key && (min_o == 0 || key_o < min_o)) { min_o = key_o; min_is_ctl = false; min_is_key = true; }
        if (has_media && (min_o == 0 || media_o < min_o)) { min_o = media_o; min_is_ctl = false; min_is_key = false; }
        if (min_is_ctl && ctl_sent >= CTL_FLUSH_BURST && (has_key || has_media)) {
            /* fairness: give keys/media a turn */
            if (has_key && (!has_media || key_o <= media_o)) {
                if (!keys_flush_one(s)) break;
            } else {
                if (!media_flush_one(s)) break;
            }
            continue;
        }
        bool ok;
        if (min_is_ctl) {
            ok = ctl_pop(s);
            ctl_sent++;
        } else if (min_is_key) {
            ok = keys_flush_one(s);
        } else {
            ok = media_flush_one(s);
        }
        if (!ok) break;
    }

    out_drain(s);
}

void rbp_server_tick(rbp_server_t *s, uint32_t now_ms)
{
#ifdef RBP_DEBUG
    dt_time(now_ms);
    static uint32_t next_metrics;
    static uint16_t peak_out,peak_audio,peak_keys,peak_media;
    if(s->out_len>peak_out)peak_out=s->out_len;
    if(s->audio_len>peak_audio)peak_audio=s->audio_len;
    if(s->key_q_count>peak_keys)peak_keys=s->key_q_count;
    if(s->media_count>peak_media)peak_media=s->media_count;
    if((int32_t)(now_ms-next_metrics)>=0) {
        DT(DT_SESSION,DT_INFO,6,(s->out_len<<16)|s->audio_len,((uint32_t)peak_out<<16)|peak_audio,
           (s->key_q_count<<16)|s->media_count,((uint32_t)peak_keys<<16)|peak_media);
        next_metrics=now_ms+5000;
    }
#endif
    s->now_ms = now_ms;
    rbp_rx_event_ctx_t timeout_ctx;
    if(rbp_rxparser_feed(&s->rx,NULL,0,now_ms,&timeout_ctx)==RBP_RX_TIMEOUT) {
        DT(DT_SESSION,DT_ERROR,4,s->session_id,now_ms,0,0);
        s->stat_protocol_errors++;session_teardown(s);
    }

    if (s->session_active &&
        (uint32_t)(now_ms - s->last_req_ms) > HEARTBEAT_TIMEOUT_MS) {
        DT(DT_SESSION,DT_ERROR,3,s->session_id,now_ms,s->last_req_ms,0);
        session_teardown(s);
    }

    if (s->find_active && (int32_t)(now_ms - s->find_end_ms) >= 0) {
        s->find_active = false;
        s->find_valid = true;
        s->cfg.backend.stop_find(s->cfg.backend.user);
        rbp_tlv_writer_t w;
        rbp_tlv_writer_init(&w);
        rbp_tlv_put_u32(&w, 1, s->search_id);
        rbp_tlv_put_u8(&w, 2, RBP_FIND_DONE_EXPIRED);
        ctl_enqueue(s, RBP_KIND_EVENT, RBP_OP_FIND_DONE_EV, 0, 0, 0, w.buf, w.len);
    }

    if (s->find_valid && !s->find_active &&
        (uint32_t)(now_ms - s->find_end_ms) > CANDIDATE_TTL_MS) {
        s->find_valid = false;
        s->cand_count = 0;
    }

    if (s->prompt_active && (int32_t)(now_ms - s->prompt_expiry_ms) >= 0) {
        s->prompt_active = false;
        request_pair_cancel(s);
    }

    if (s->op_active && (int32_t)(now_ms - s->op_deadline_ms) >= 0) {
        uint16_t result = RBP_STATUS_TIMEOUT;
        bool uncertain = true;
        if (s->op_kind == OPK_FORGET) result = RBP_STATUS_STORAGE_FAILED;
        s->op_active = false;
        if (s->op_kind == OPK_PAIR) request_pair_cancel(s);
        op_complete(s, s->op_id, result, s->op_peer_id, s->op_connection_id, uncertain);
    }

    if (s->source_active) {
        if (s->stop_requested && (int32_t)(now_ms - s->stop_deadline_ms) >= 0) {
            s->stop_deadline_ms=now_ms+VOICE_STOP_MS;
            s->stat_voice_errors++;
            s->voice_enabled=false;s->voice_state=RBP_VOICE_FAILED;
            /* Commit the failure outcome before a backend can synchronously
             * report link loss and reset the stream identity. */
            voice_finish_stream(s,RBP_END_DEVICE_ERROR,true);
            send_device_state(s,s->connection_id);
            s->cfg.backend.disconnect(s->cfg.backend.user);
        } else if (!s->stop_requested && s->source_active &&
                   (int32_t)(now_ms - s->capture_deadline_ms) >= 0) {
            /* enforce max_capture_ms: stop the source, end as capture_limit */
            s->capture_limited = true;
            s->stop_requested = true;
            s->stop_deadline_ms = now_ms + VOICE_STOP_MS;
            s->cfg.backend.voice_request_stop(s->cfg.backend.user);
        } else if (s->source_active && !s->stop_requested) {
            if (!s->got_audio_this_stream &&
                (int32_t)(now_ms - s->first_audio_deadline) >= 0) {
                DT(DT_SESSION,DT_ERROR,8,s->stream_id,now_ms,s->first_audio_deadline,0);
                voice_fault(s,RBP_END_DEVICE_ERROR);
            } else if (s->got_audio_this_stream &&
                       (int32_t)(now_ms - s->progress_deadline) >= 0) {
                s->stat_voice_errors++;
                voice_finish_stream(s, RBP_END_DEVICE_ERROR, true);
                s->stop_requested=true;s->stop_deadline_ms=now_ms+VOICE_STOP_MS;
                s->cfg.backend.voice_request_stop(s->cfg.backend.user);
            }
        }
    }

    if (s->voice_ok_deferred && s->session_active && !s->delivering &&
        s->media_count == 0) {
        rbp_tlv_writer_t w;
        rbp_tlv_writer_init(&w);
        if (s->voice_ok_deferred_is_enable) {
            rbp_tlv_put_bool(&w, 1, false);
            rbp_tlv_put_bool(&w, 2, false);
        }
        ctl_enqueue(s, RBP_KIND_RESPONSE, s->voice_ok_deferred_opcode, RBP_STATUS_OK,
                    s->voice_ok_deferred_req, s->voice_ok_deferred_conn,
                    w.buf, w.len);
        s->voice_ok_deferred = false;
    }

    rbp_server_flush_output(s);
#ifdef RBP_DEBUG
    /* Separate typed extension, never printf into CDC; normal traffic wins.
     * At most four records per tick, never consume on output backpressure. */
    if(s->session_active && s->debug_delivery && !s->out_len) {
        struct {uint32_t version,dropped,highwater;dt_record records[4];} packet;
        uint32_t seq,drop,high;
        uint8_t n=dt_peek(packet.records,4);
        dt_stats(&seq,&drop,&high);
        packet.version=1;packet.dropped=drop;packet.highwater=high;
        if(n) {
            rbp_header_t h;memset(&h,0,sizeof h);
            h.kind=RBP_KIND_EVENT;h.opcode=0x7f00;h.session_id=s->session_id;
            if(out_try_frame(s,&h,(uint8_t*)&packet,12+n*sizeof(dt_record)))dt_consume(n);
            out_drain(s);
        }
    }
#endif
}

/* ===================== request processing ===================== */

static void respond_error(rbp_server_t *s, const rbp_header_t *req, uint16_t status,
                          const char *msg, bool uncertain)
{
    uint8_t payload[144];
    uint16_t len = 0;
    if (msg || uncertain) {
        rbp_tlv_writer_t w;
        rbp_tlv_writer_init(&w);
        if (msg) rbp_tlv_put_text(&w, RBP_TAG_MESSAGE, msg);
        if (uncertain) rbp_tlv_put_bool(&w, RBP_TAG_UNCERTAIN, true);
        if (w.len <= sizeof(payload)) {
            memcpy(payload, w.buf, w.len);
            len = w.len;
        }
    }
    ctl_enqueue(s, RBP_KIND_RESPONSE, req->opcode, status, req->request_id,
                req->connection_id, payload, len);
}

static bool conn_ready_for(const rbp_server_t *s, const rbp_header_t *h)
{
    if (s->link_state != RBP_LINK_READY) return false;
    if (!h->connection_id || h->connection_id != s->connection_id) return false;
    return true;
}

static void handle_hello(rbp_server_t *s, const rbp_header_t *h,
                         const uint8_t *payload, uint16_t len)
{
    rbp_tlv_reader_t r;
    bool found = false;
    const uint8_t *nonce = NULL;
    uint16_t nonce_len = 0;
    rbp_tlv_reader_init(&r, payload, len);
    rbp_tlv_err_t e = rbp_tlv_get_bytes(&r, 1, &nonce, &nonce_len, &found);

    bool header_shape_ok = (h->kind==RBP_KIND_REQUEST && h->flags==0 &&
                            h->status==0 && h->session_id==0 && h->connection_id==0 &&
                            h->request_id==1 && h->tx_seq==1 && h->payload_size==len &&
                            h->magic0 == RBP_MAGIC0 && h->magic1 == RBP_MAGIC1 &&
                            h->header_size == RBP_HEADER_SIZE && h->reserved == 0);

    if(!header_shape_ok) {s->stat_protocol_errors++;return;}
    if (header_shape_ok && (h->major != RBP_MAJOR || h->minor != RBP_MINOR)) {
        rbp_header_t rh;
        memset(&rh, 0, sizeof(rh));
        rh.kind = RBP_KIND_RESPONSE;
        rh.session_id = 0;
        rh.tx_seq = 1;
        rh.request_id = 1;
        rh.opcode = RBP_OP_HELLO;
        rh.status = RBP_STATUS_VERSION_MISMATCH;
        rbp_tlv_writer_t w;
        rbp_tlv_writer_init(&w);
        if (e == RBP_TLV_OK && found && nonce_len == 16)
            rbp_tlv_put_bytes(&w, 3, nonce, 16);
        rbp_txseq_t seq;
        rbp_txseq_init(&seq);
        uint8_t tmp[RBP_MAX_ENCODED];
        size_t n = rbp_frame_encode(&rh, &seq, w.buf, w.len, tmp);
        ring_write(s->out_ring, OUT_RING_SIZE, &s->out_head, &s->out_len, tmp, n);
        out_drain(s);
        return;
    }

    if (e != RBP_TLV_OK || !found || nonce_len != 16) {
        respond_error(s, h, RBP_STATUS_INVALID_ARGUMENT, "nonce required", false);
        out_drain(s);
        return;
    }
    if (h->session_id != 0) {
        respond_error(s, h, RBP_STATUS_INVALID_ARGUMENT, "session must be 0", false);
        out_drain(s);
        return;
    }

    session_create(s);
    s->req_hwm = h->request_id;
    s->client_tx_hwm=1;s->have_client_seq=true;

    rbp_tlv_writer_t w;
    rbp_tlv_writer_init(&w);
    rbp_tlv_put_bytes(&w, 1, nonce, 16);
    rbp_tlv_put_bytes(&w, 2, s->cfg.bridge_uid, 16);
    rbp_tlv_put_text(&w, 3, s->cfg.firmware_version ? s->cfg.firmware_version : "0.0");
    rbp_tlv_put_u8(&w, 4, (uint8_t)s->cfg.max_keys);
    rbp_tlv_put_u16(&w, 5, RBP_MAX_PAYLOAD);
    uint32_t features = RBP_FEAT_ENCODED_VOICE | RBP_FEAT_INTERACTIVE_PAIR |
                        RBP_FEAT_BOND_SAVE | RBP_FEAT_AUTO_RECONNECT;
#ifdef RBP_STANDALONE
    features &= ~RBP_FEAT_ENCODED_VOICE;
#endif
    rbp_tlv_put_u32(&w, 6, features);
    ctl_enqueue(s, RBP_KIND_RESPONSE, RBP_OP_HELLO, RBP_STATUS_OK, h->request_id, 0,
                w.buf, w.len);
    out_drain(s);
}

static void process_frame(rbp_server_t *s, const rbp_header_t *h,
                          const uint8_t *payload, uint16_t len, bool crc_ok)
{
    if (!crc_ok) {
        if (s->session_active) {
            s->stat_protocol_errors++;
            session_teardown(s);
        }
        return;
    }
    if(h->payload_size!=len || h->flags!=0 || h->reserved!=0 || h->status!=0) {
        s->stat_protocol_errors++;if(s->session_active)session_teardown(s);return;
    }
    if (h->kind != RBP_KIND_REQUEST) {
        if (s->session_active) s->stat_protocol_errors++;
        return;
    }

    if (h->opcode == RBP_OP_HELLO) {
        handle_hello(s, h, payload, len);
        return;
    }

    if(h->major!=RBP_MAJOR || h->minor!=RBP_MINOR) {if(s->session_active)session_teardown(s);return;}
    if (!s->session_active || h->session_id != s->session_id) {
        return;
    }

    if(h->tx_seq==0 || s->client_tx_hwm==UINT32_MAX || h->tx_seq!=s->client_tx_hwm+1) {
        s->stat_protocol_errors++;session_teardown(s);return;
    }
    s->client_tx_hwm=h->tx_seq;

    s->last_req_ms = s->now_ms;

    if (h->request_id == 0) {
        respond_error(s, h, RBP_STATUS_INVALID_ARGUMENT, "request_id required", false);
        return;
    }
    if (h->request_id <= s->req_hwm) {
        respond_error(s, h, RBP_STATUS_DUPLICATE, NULL, false);
        return;
    }
    s->req_hwm = h->request_id;

    dispatch_request(s, h, payload, len);
}

static void op_begin(rbp_server_t *s, uint8_t kind, const rbp_header_t *h)
{
    s->op_active = true;
    s->op_kind = kind;
    s->pair_cancel_requested=false;
    s->pair_ready_pending=false;
    s->op_id = h->request_id;
    s->op_connection_id = 0;
    s->op_peer_id = 0;
}

static uint16_t accepted_payload(uint8_t *buf, uint32_t op_id)
{
    uint16_t n = 0;
    buf[n++] = 1;
    buf[n++] = RBP_TLV_T_U32;
    buf[n++] = 4;
    buf[n++] = 0;
    for (int k = 0; k < 4; k++) buf[n++] = (uint8_t)(op_id >> (8 * k));
    return n;
}

static void dispatch_request(rbp_server_t *s, const rbp_header_t *h,
                             const uint8_t *payload, uint16_t len)
{
    rbp_tlv_reader_t rd;
    rbp_tlv_reader_init(&rd, payload, len);

    if(h->opcode<0x200 && h->connection_id) {
        respond_error(s,h,RBP_STATUS_INVALID_ARGUMENT,"management connection must be zero",false);return;
    }
    /* Validate the entire envelope, including fields after a known tag and
     * unknown extensions. Typed getters below validate known field values. */
    while(rd.pos<rd.len) {
        uint8_t tag,type;const uint8_t *value;uint16_t size;
        if(rbp_tlv_next(&rd,&tag,&type,&value,&size)!=RBP_TLV_OK) {
            respond_error(s,h,RBP_STATUS_INVALID_ARGUMENT,"malformed TLV",false);return;
        }
    }
    rbp_tlv_reader_init(&rd,payload,len);

#ifdef RBP_STANDALONE
    if(h->opcode==RBP_OP_VOICE_ENABLE || h->opcode==RBP_OP_VOICE_START ||
       h->opcode==RBP_OP_VOICE_STOP || h->opcode==RBP_OP_GET_VOICE_CAPS) {
        respond_error(s,h,RBP_STATUS_UNSUPPORTED,"audio owned by USB microphone",false);return;
    }
#endif
    switch (h->opcode) {
#ifdef RBP_DEBUG
    case 0x7f01: {
        uint32_t mask=0;uint8_t level=0;bool have_mask=false,have_level=false;
        if(h->connection_id || rbp_tlv_get_u32(&rd,1,&mask,&have_mask)!=RBP_TLV_OK ||
           rbp_tlv_get_u8(&rd,2,&level,&have_level)!=RBP_TLV_OK || !have_mask || !have_level || mask>255 || level>4) {
            respond_error(s,h,RBP_STATUS_INVALID_ARGUMENT,"debug config",false);return;
        }
        dt_config(mask,level);
        s->debug_delivery=mask!=0 && level!=0;
        rbp_tlv_writer_t w;rbp_tlv_writer_init(&w);uint32_t seq,drop,high;dt_stats(&seq,&drop,&high);
        rbp_tlv_put_u32(&w,1,seq);rbp_tlv_put_u32(&w,2,drop);rbp_tlv_put_u32(&w,3,high);
        ctl_enqueue(s,RBP_KIND_RESPONSE,h->opcode,0,h->request_id,0,w.buf,w.len);return;
    }
#endif
    case RBP_OP_PING: {
        uint32_t cookie = 0;
        bool found = false;
        rbp_tlv_err_t e = rbp_tlv_get_u32(&rd, 1, &cookie, &found);
        if (e != RBP_TLV_OK || !found) {
            respond_error(s, h, RBP_STATUS_INVALID_ARGUMENT, "cookie required", false);
            return;
        }
        rbp_tlv_writer_t w;
        rbp_tlv_writer_init(&w);
        rbp_tlv_put_u32(&w, 1, cookie);
        rbp_tlv_put_u64(&w, 2, now_us64(s));
        ctl_enqueue(s, RBP_KIND_RESPONSE, h->opcode, RBP_STATUS_OK, h->request_id,
                    h->connection_id, w.buf, w.len);
        return;
    }
    case RBP_OP_GET_DEVICE: {
        rbp_tlv_writer_t w;
        rbp_tlv_writer_init(&w);
        device_info_tlv(s, &w);
        ctl_enqueue(s, RBP_KIND_RESPONSE, h->opcode, RBP_STATUS_OK, h->request_id,
                    h->connection_id, w.buf, w.len);
        return;
    }
    case RBP_OP_GET_PEER: {
        rbp_tlv_writer_t w;
        rbp_tlv_writer_init(&w);
        rbp_tlv_put_u32(&w, 1, s->peer_valid ? s->peer.peer_id : 0);
        rbp_tlv_put_text(&w, 2, s->peer_valid ? s->peer.name : "");
#ifdef RBP_STANDALONE
        rbp_tlv_put_bool(&w,3,s->peer_valid);
#else
        rbp_tlv_put_bool(&w, 3, s->peer_valid ? s->peer.auto_reconnect : false);
#endif
        ctl_enqueue(s, RBP_KIND_RESPONSE, h->opcode, RBP_STATUS_OK, h->request_id,
                    h->connection_id, w.buf, w.len);
        return;
    }
    case RBP_OP_GET_OPERATION: {
        uint32_t op_id = 0;
        bool found = false;
        rbp_tlv_err_t e = rbp_tlv_get_u32(&rd, 1, &op_id, &found);
        if (e != RBP_TLV_OK || !found) {
            respond_error(s, h, RBP_STATUS_INVALID_ARGUMENT, "operation_id required", false);
            return;
        }
        rbp_tlv_writer_t w;
        rbp_tlv_writer_init(&w);
        if (s->op_active && s->op_id == op_id) {
            rbp_tlv_put_u32(&w, 1, op_id);
            rbp_tlv_put_u8(&w, 2, RBP_OPSTATE_PENDING);
            rbp_tlv_put_u16(&w, 3, RBP_STATUS_ACCEPTED);
            rbp_tlv_put_u32(&w, 4, s->op_peer_id);
            rbp_tlv_put_u32(&w, 5, s->op_connection_id);
            rbp_tlv_put_bool(&w, 6, false);
            ctl_enqueue(s, RBP_KIND_RESPONSE, h->opcode, RBP_STATUS_OK, h->request_id,
                        h->connection_id, w.buf, w.len);
            return;
        }
        for (uint8_t i = 0; i < s->ops_done_count; i++) {
            uint8_t idx = (uint8_t)((s->ops_done_head + i) % RBP_MAX_OPERATION_RESULTS);
            if (s->ops_done[idx].op_id == op_id) {
                rbp_tlv_put_u32(&w, 1, op_id);
                rbp_tlv_put_u8(&w, 2, RBP_OPSTATE_COMPLETED);
                rbp_tlv_put_u16(&w, 3, s->ops_done[idx].result);
                rbp_tlv_put_u32(&w, 4, s->ops_done[idx].peer_id);
                rbp_tlv_put_u32(&w, 5, s->ops_done[idx].connection_id);
                rbp_tlv_put_bool(&w, 6, s->ops_done[idx].uncertain);
                ctl_enqueue(s, RBP_KIND_RESPONSE, h->opcode, RBP_STATUS_OK,
                            h->request_id, h->connection_id, w.buf, w.len);
                return;
            }
        }
        respond_error(s, h, RBP_STATUS_NOT_FOUND, NULL, false);
        return;
    }
    case RBP_OP_GET_STATS: {
        rbp_tlv_writer_t w;
        rbp_tlv_writer_init(&w);
        rbp_tlv_put_u32(&w, 1, s->stat_protocol_errors);
        rbp_tlv_put_u32(&w, 2, s->stat_input_resets);
        rbp_tlv_put_u32(&w, 3, s->stat_voice_overruns);
        rbp_tlv_put_u32(&w, 4, s->stat_voice_errors);
        rbp_tlv_put_u16(&w, 5, s->cfg.reset_reason);
        rbp_fault_t faults[RBP_FAULT_CAPACITY];uint32_t sequence,evicted;
        uint8_t count=rbp_fault_snapshot(faults,&sequence,&evicted);
        uint8_t bytes[RBP_FAULT_CAPACITY*24];
        for(uint8_t i=0;i<count;i++) {
            uint32_t words[]={faults[i].sequence,(uint32_t)faults[i].domain|((uint32_t)faults[i].stage<<16),faults[i].code,faults[i].context,faults[i].count,faults[i].board_ms};
            for(unsigned j=0;j<6;j++)for(unsigned k=0;k<4;k++)bytes[i*24+j*4+k]=(uint8_t)(words[j]>>(k*8));
        }
        rbp_tlv_put_u32(&w,6,sequence);rbp_tlv_put_bytes(&w,7,bytes,count*24);rbp_tlv_put_u32(&w,8,evicted);
#ifdef RBP_STANDALONE
        uint8_t pipeline[STANDALONE_STATS_BYTES];standalone_stats(pipeline);
        rbp_tlv_put_bytes(&w,9,pipeline,sizeof pipeline);
        standalone_usb_stats(pipeline);
        rbp_tlv_put_bytes(&w,10,pipeline,STANDALONE_USB_STATS_BYTES);
#endif
        ctl_enqueue(s, RBP_KIND_RESPONSE, h->opcode, RBP_STATUS_OK, h->request_id,
                    h->connection_id, w.buf, w.len);
        return;
    }
    case RBP_OP_GOODBYE: {
        uint32_t closing_session=s->session_id;
        size_t committed_output=s->out_len;
        session_teardown(s);
        /* Frames already assigned tx_seq must finish before the final reply,
         * otherwise GOODBYE would itself create a sequence gap. */
        s->out_len=committed_output;
        ctl_enqueue(s, RBP_KIND_RESPONSE, h->opcode, RBP_STATUS_OK, h->request_id,
                    h->connection_id, NULL, 0);
        s->ctl[0].session=closing_session; /* teardown emptied every slot */
        return;
    }

    case RBP_OP_FIND_START: {
        uint16_t dur = 0;
        bool found = false;
        rbp_tlv_err_t e = rbp_tlv_get_u16(&rd, 1, &dur, &found);
        if (e != RBP_TLV_OK || !found || dur < 1 || dur > 30000) {
            respond_error(s, h, RBP_STATUS_INVALID_ARGUMENT, "duration 1..30000", false);
            return;
        }
        if (s->op_active) {
            respond_error(s, h, RBP_STATUS_BUSY, NULL, false);
            return;
        }
        if (s->link_state == RBP_LINK_CONNECTING || s->link_state == RBP_LINK_PAIRING ||
            s->link_state == RBP_LINK_READY || s->link_state == RBP_LINK_INITIALIZING) {
            respond_error(s, h, RBP_STATUS_BAD_STATE, "link active", false);
            return;
        }
        s->find_active = true;
        s->find_valid = false;
        s->cand_count = 0;
        do {
            s->search_id = rng_u32(s);
        } while (s->search_id == 0);
        s->find_end_ms = s->now_ms + dur;
        uint16_t scan_status=s->cfg.backend.start_find(s->cfg.backend.user, dur);
        if(scan_status!=RBP_STATUS_OK) {
            s->find_active=false;s->find_valid=false;s->cand_count=0;
            respond_error(s,h,scan_status,"discovery did not start",false);
            return;
        }
        rbp_tlv_writer_t w;
        rbp_tlv_writer_init(&w);
        rbp_tlv_put_u32(&w, 1, s->search_id);
        ctl_enqueue(s, RBP_KIND_RESPONSE, h->opcode, RBP_STATUS_OK, h->request_id,
                    h->connection_id, w.buf, w.len);
        return;
    }
    case RBP_OP_FIND_LIST: {
        uint32_t sid = 0;
        uint8_t cursor = 0;
        bool f1 = false, f2 = false;
        rbp_tlv_err_t e1 = rbp_tlv_get_u32(&rd, 1, &sid, &f1);
        rbp_tlv_err_t e2 = rbp_tlv_get_u8(&rd, 2, &cursor, &f2);
        if (e1 != RBP_TLV_OK || e2 != RBP_TLV_OK || !f1 || !f2) {
            respond_error(s, h, RBP_STATUS_INVALID_ARGUMENT, "args", false);
            return;
        }
        if (sid != s->search_id || (!s->find_active && !s->find_valid)) {
            respond_error(s, h, RBP_STATUS_NOT_FOUND, "search", false);
            return;
        }
        if (cursor >= RBP_MAX_CANDIDATES) {
            respond_error(s, h, RBP_STATUS_INVALID_ARGUMENT, "cursor", false);
            return;
        }
        uint8_t entries[500];
        uint16_t elen = 1;
        uint8_t count = 0;
        uint8_t i = cursor;
        for (; i < s->cand_count && count < 8; i++) {
            rbp_candidate_t *c = &s->cand[i];
            uint8_t rec[64];
            uint16_t rl = 0;
            for (int k = 0; k < 4; k++) rec[rl++] = (uint8_t)(c->candidate_id >> (8 * k));
            rec[rl++] = c->support;
            rec[rl++] = c->signal;
            rec[rl++] = c->name_len;
            for (uint8_t k = 0; k < c->name_len; k++) rec[rl++] = (uint8_t)c->name[k];
            if ((size_t)elen + rl + 1 > sizeof(entries)) break;
            memcpy(entries + elen, rec, rl);
            elen += rl;
            count++;
        }
        entries[0] = count;
        uint8_t next_cursor = (i >= s->cand_count) ? 255 : i;
        rbp_tlv_writer_t w;
        rbp_tlv_writer_init(&w);
        rbp_tlv_put_u8(&w, 1, next_cursor);
        rbp_tlv_put_bytes(&w, 2, entries, elen);
        ctl_enqueue(s, RBP_KIND_RESPONSE, h->opcode, RBP_STATUS_OK, h->request_id,
                    h->connection_id, w.buf, w.len);
        return;
    }
    case RBP_OP_FIND_STOP: {
        uint32_t sid = 0;
        bool found = false;
        rbp_tlv_err_t e = rbp_tlv_get_u32(&rd, 1, &sid, &found);
        if (e != RBP_TLV_OK || !found) {
            respond_error(s, h, RBP_STATUS_INVALID_ARGUMENT, "search_id required", false);
            return;
        }
        if (sid != s->search_id || !s->find_active) {
            respond_error(s, h, RBP_STATUS_NOT_FOUND, "search", false);
            return;
        }
        s->find_active = false;
        s->find_valid = true;       /* candidates stay valid for the TTL */
        s->find_end_ms = s->now_ms; /* candidate TTL starts at the stop */
        ctl_enqueue(s, RBP_KIND_RESPONSE, h->opcode, RBP_STATUS_OK, h->request_id,
                    h->connection_id, NULL, 0);
        s->cfg.backend.stop_find(s->cfg.backend.user);
        rbp_tlv_writer_t fw;
        rbp_tlv_writer_init(&fw);
        rbp_tlv_put_u32(&fw, 1, s->search_id);
        rbp_tlv_put_u8(&fw, 2, RBP_FIND_DONE_USER_STOP);
        ctl_enqueue(s, RBP_KIND_EVENT, RBP_OP_FIND_DONE_EV, 0, 0, 0, fw.buf, fw.len);
        return;
    }
    case RBP_OP_PAIR_BEGIN: {
        uint32_t sid = 0, cid = 0;
        bool f1 = false, f2 = false;
        rbp_tlv_err_t e1 = rbp_tlv_get_u32(&rd, 1, &sid, &f1);
        rbp_tlv_err_t e2 = rbp_tlv_get_u32(&rd, 2, &cid, &f2);
        if (e1 != RBP_TLV_OK || e2 != RBP_TLV_OK || !f1 || !f2) {
            respond_error(s, h, RBP_STATUS_INVALID_ARGUMENT, "args", false);
            return;
        }
        if (s->op_active) {
            respond_error(s, h, RBP_STATUS_BUSY, NULL, false);
            return;
        }
        if (s->link_state == RBP_LINK_READY) {
            respond_error(s, h, RBP_STATUS_BAD_STATE, "already connected", false);
            return;
        }
        if (s->peer_valid) {
            respond_error(s, h, RBP_STATUS_BUSY, "bond slot full", false);
            return;
        }
        bool have_cand = false;
        for (uint8_t i = 0; i < s->cand_count; i++)
            if (s->cand[i].candidate_id == cid) have_cand = true;
        /* a candidate seen during the live scan is pairable (takeover);
         * find_valid only gates candidates reported after the scan ended */
        if (sid != s->search_id || !have_cand ||
            (!s->find_valid && !s->find_active)) {
            respond_error(s, h, RBP_STATUS_NOT_FOUND, "candidate", false);
            return;
        }
        if (s->find_active) {
            s->find_active = false;
            s->cfg.backend.stop_find(s->cfg.backend.user);
            rbp_tlv_writer_t w;
            rbp_tlv_writer_init(&w);
            rbp_tlv_put_u32(&w, 1, s->search_id);
            rbp_tlv_put_u8(&w, 2, RBP_FIND_DONE_PAIR_TAKEOVER);
            ctl_enqueue(s, RBP_KIND_EVENT, RBP_OP_FIND_DONE_EV, 0, 0, 0, w.buf, w.len);
        }
        op_begin(s, OPK_PAIR, h);
        s->op_deadline_ms = s->now_ms + PAIR_DEADLINE_MS;
        uint8_t pl[8];
        uint16_t plen = accepted_payload(pl, h->request_id);
        ctl_enqueue(s, RBP_KIND_RESPONSE, h->opcode, RBP_STATUS_ACCEPTED,
                    h->request_id, h->connection_id, pl, plen);
        s->cfg.backend.pair_begin(s->cfg.backend.user,cid);
        return;
    }
    case RBP_OP_PAIR_REPLY: {
        uint32_t op_id = 0, pid = 0;
        bool accept = false, f_acc = false;
        uint32_t passkey = 0;
        bool f_op = false, f_pid = false, f_pk = false;
        rbp_tlv_err_t e1 = rbp_tlv_get_u32(&rd, 1, &op_id, &f_op);
        rbp_tlv_err_t e2 = rbp_tlv_get_u32(&rd, 2, &pid, &f_pid);
        rbp_tlv_err_t e3 = rbp_tlv_get_bool(&rd, 3, &accept, &f_acc);
        rbp_tlv_err_t e4 = rbp_tlv_get_u32(&rd, 4, &passkey, &f_pk);
        if (e1 != RBP_TLV_OK || e2 != RBP_TLV_OK || e3 != RBP_TLV_OK || e4 != RBP_TLV_OK ||
            !f_op || !f_pid || !f_acc) {
            respond_error(s, h, RBP_STATUS_INVALID_ARGUMENT, "args", false);
            return;
        }
        if (f_pk && passkey > 999999) {
            respond_error(s, h, RBP_STATUS_INVALID_ARGUMENT, "passkey range", false);
            return;
        }
        if (!s->op_active || s->op_kind != OPK_PAIR || op_id != s->op_id ||
            !s->prompt_active || pid != s->prompt_id) {
            respond_error(s, h, RBP_STATUS_NOT_FOUND, "prompt", false);
            return;
        }
        if (f_pk && s->prompt_method != RBP_PROMPT_ENTER_PASSKEY) {
            respond_error(s, h, RBP_STATUS_INVALID_ARGUMENT, "passkey not expected", false);
            return;
        }
        if(s->prompt_method==RBP_PROMPT_DISPLAY_PASSKEY) {
            respond_error(s,h,RBP_STATUS_INVALID_ARGUMENT,"display only; cancel operation to reject",false);return;
        }
        if (accept && !f_pk && s->prompt_method == RBP_PROMPT_ENTER_PASSKEY) {
            respond_error(s, h, RBP_STATUS_INVALID_ARGUMENT, "passkey required", false);
            return;
        }
        s->prompt_active = false;
        ctl_enqueue(s, RBP_KIND_RESPONSE, h->opcode, RBP_STATUS_OK, h->request_id,
                    h->connection_id, NULL, 0);
        s->cfg.backend.pair_reply(s->cfg.backend.user, accept, f_pk, passkey);
        return;
    }
    case RBP_OP_PAIR_CANCEL: {
        uint32_t op_id = 0;
        bool found = false;
        rbp_tlv_err_t e = rbp_tlv_get_u32(&rd, 1, &op_id, &found);
        if (e != RBP_TLV_OK || !found) {
            respond_error(s, h, RBP_STATUS_INVALID_ARGUMENT, "operation_id required", false);
            return;
        }
        if (!s->op_active || s->op_kind!=OPK_PAIR || op_id != s->op_id) {
            respond_error(s, h, RBP_STATUS_NOT_FOUND, "operation", false);
            return;
        }
        s->prompt_active = false;
        ctl_enqueue(s, RBP_KIND_RESPONSE, h->opcode, RBP_STATUS_OK, h->request_id,
                    h->connection_id, NULL, 0);
        request_pair_cancel(s);
        return;
    }
    case RBP_OP_FORGET_PEER: {
        uint32_t pid = 0;
        bool found = false;
        rbp_tlv_err_t e = rbp_tlv_get_u32(&rd, 1, &pid, &found);
        if (e != RBP_TLV_OK || !found) {
            respond_error(s, h, RBP_STATUS_INVALID_ARGUMENT, "peer_id required", false);
            return;
        }
        if (s->op_active) {
            respond_error(s, h, RBP_STATUS_BUSY, NULL, false);
            return;
        }
        if (!s->peer_valid || pid != s->peer.peer_id) {
            respond_error(s, h, RBP_STATUS_NOT_FOUND, "peer", false);
            return;
        }
        op_begin(s, OPK_FORGET, h);
        s->op_deadline_ms = s->now_ms + FORGET_DEADLINE_MS;
        s->reconnect_paused=true;
        uint8_t pl[8];
        uint16_t plen = accepted_payload(pl, h->request_id);
        ctl_enqueue(s, RBP_KIND_RESPONSE, h->opcode, RBP_STATUS_ACCEPTED,
                    h->request_id, h->connection_id, pl, plen);
        s->cfg.backend.forget_peer(s->cfg.backend.user);
        return;
    }
    case RBP_OP_SET_RECONNECT: {
        uint32_t pid = 0;
        bool enabled = false, f_en = false;
        bool f_pid = false;
        rbp_tlv_err_t e1 = rbp_tlv_get_u32(&rd, 1, &pid, &f_pid);
        rbp_tlv_err_t e2 = rbp_tlv_get_bool(&rd, 2, &enabled, &f_en);
        if (e1 != RBP_TLV_OK || e2 != RBP_TLV_OK || !f_pid || !f_en) {
            respond_error(s, h, RBP_STATUS_INVALID_ARGUMENT, "args", false);
            return;
        }
        if (!s->peer_valid || pid != s->peer.peer_id) {
            respond_error(s, h, RBP_STATUS_NOT_FOUND, "peer", false);
            return;
        }
#ifdef RBP_STANDALONE
        if(!enabled) {
            respond_error(s,h,RBP_STATUS_UNSUPPORTED,"automatic reconnect is fixed on",false);return;
        }
#endif
        if (s->peer.auto_reconnect == enabled) {
            if(enabled)s->reconnect_paused=false;
            ctl_enqueue(s, RBP_KIND_RESPONSE, h->opcode, RBP_STATUS_OK, h->request_id,
                        h->connection_id, NULL, 0);
            return;
        }
        bool previous=s->peer.auto_reconnect;
        s->peer.auto_reconnect = enabled;
        if (!s->cfg.store.save || !s->cfg.store.save(s->cfg.store.user, &s->peer)) {
            s->peer.auto_reconnect=previous;
            respond_error(s, h, RBP_STATUS_STORAGE_FAILED, NULL, false);
            return;
        }
        if (enabled) s->reconnect_paused = false;
        ctl_enqueue(s, RBP_KIND_RESPONSE, h->opcode, RBP_STATUS_OK, h->request_id,
                    h->connection_id, NULL, 0);
        return;
    }
    case RBP_OP_CONNECT_PEER: {
        if(s->op_active) {respond_error(s,h,RBP_STATUS_BUSY,NULL,false);return;}
        uint32_t pid=0;bool found=false;
        if(rbp_tlv_get_u32(&rd,1,&pid,&found)!=RBP_TLV_OK || !found) {respond_error(s,h,RBP_STATUS_INVALID_ARGUMENT,"peer_id required",false);return;}
        if(!s->peer_valid || s->peer.peer_id!=pid) {respond_error(s,h,RBP_STATUS_NOT_FOUND,"peer",false);return;}
        if (s->link_state == RBP_LINK_READY) {
            respond_error(s, h, RBP_STATUS_BAD_STATE, "already connected", false);
            return;
        }
        if (s->link_state == RBP_LINK_CONNECTING || s->link_state == RBP_LINK_PAIRING ||
            s->link_state == RBP_LINK_INITIALIZING) {
            ctl_enqueue(s, RBP_KIND_RESPONSE, h->opcode, RBP_STATUS_OK, h->request_id,
                        h->connection_id, NULL, 0);
            return;
        }
        if (!s->peer_valid) {
            respond_error(s, h, RBP_STATUS_NOT_FOUND, "peer", false);
            return;
        }
        s->reconnect_paused = false;
        s->cfg.backend.connect_peer(s->cfg.backend.user);
        ctl_enqueue(s, RBP_KIND_RESPONSE, h->opcode, RBP_STATUS_OK, h->request_id,
                    h->connection_id, NULL, 0);
        return;
    }
    case RBP_OP_DISCONNECT: {
        s->reconnect_paused = true;
        s->voice_resume_peer = 0;
        if (s->link_state == RBP_LINK_CONNECTING || s->link_state == RBP_LINK_PAIRING ||
            s->link_state == RBP_LINK_INITIALIZING || s->link_state == RBP_LINK_READY) {
            s->cfg.backend.disconnect(s->cfg.backend.user);
        }
        ctl_enqueue(s, RBP_KIND_RESPONSE, h->opcode, RBP_STATUS_OK, h->request_id,
                    h->connection_id, NULL, 0);
        return;
    }

    case RBP_OP_KEY_CATALOG: {
        if (!conn_ready_for(s, h)) {
            respond_error(s, h,
                          s->link_state == RBP_LINK_READY ? RBP_STATUS_BAD_STATE
                                                          : RBP_STATUS_LINK_LOST,
                          NULL, false);
            return;
        }
        uint8_t cursor = 0;
        bool found = false;
        rbp_tlv_err_t e = rbp_tlv_get_u8(&rd, 1, &cursor, &found);
        if (e == RBP_TLV_ERR_MALFORMED || (e == RBP_TLV_ERR_TYPE && found)) {
            respond_error(s, h, RBP_STATUS_INVALID_ARGUMENT, "tlv", false);
            return;
        }
        const rbp_device_profile_t *prof = s->cfg.profile;
        if (!found) cursor = 0;
        if (!prof) {
            respond_error(s, h, RBP_STATUS_BAD_STATE, "catalog", false);
            return;
        }
        if (cursor >= prof->key_count) {
            respond_error(s, h, RBP_STATUS_INVALID_ARGUMENT, "cursor", false);
            return;
        }
        uint8_t entries[500];
        uint16_t elen = 1;
        uint8_t count = 0;
        uint8_t i = cursor;
        for (; i < prof->key_count && count < 12; i++) {
            uint8_t rec[40];
            const char *name = prof->keys[i].name;
            uint8_t nl = 0;
            while (name[nl] && nl < 32) nl++;
            uint16_t rl = (uint16_t)(1 + 2 + 1 + nl);
            if ((size_t)elen + rl + 1 > sizeof(entries)) break;
            rec[0] = i;
            rec[1] = (uint8_t)(prof->keys[i].key_id & 0xFF);
            rec[2] = (uint8_t)(prof->keys[i].key_id >> 8);
            rec[3] = nl;
            memcpy(rec + 4, name, nl);
            memcpy(entries + elen, rec, rl);
            elen += rl;
            count++;
        }
        entries[0] = count;
        uint8_t next_cursor = (i >= prof->key_count) ? 255 : i;
        rbp_tlv_writer_t w;
        rbp_tlv_writer_init(&w);
        rbp_tlv_put_u32(&w, 1, prof->catalog_revision);
        rbp_tlv_put_u8(&w, 2, next_cursor);
        rbp_tlv_put_bytes(&w, 3, entries, elen);
        ctl_enqueue(s, RBP_KIND_RESPONSE, h->opcode, RBP_STATUS_OK, h->request_id,
                    h->connection_id, w.buf, w.len);
        return;
    }
    case RBP_OP_KEYS_SNAPSHOT: {
        if (!conn_ready_for(s, h)) {
            respond_error(s, h,
                          s->link_state == RBP_LINK_READY ? RBP_STATUS_BAD_STATE
                                                          : RBP_STATUS_LINK_LOST,
                          NULL, false);
            return;
        }
        uint8_t st[RBP_KEYS_STRUCT_SIZE];
        memset(st, 0, sizeof(st));
        for (int i = 0; i < 4; i++) st[i] = (uint8_t)(s->input_seq >> (8 * i));
        for (int i = 0; i < 8; i++) st[4 + i] = (uint8_t)(s->keys_captured_us >> (8 * i));
        for (int i = 0; i < 8; i++) st[12 + i] = (uint8_t)(s->pressed_bits >> (8 * i));
        st[20] = RBP_KEYS_KIND_SNAPSHOT;
        ctl_enqueue(s, RBP_KIND_RESPONSE, h->opcode, RBP_STATUS_OK, h->request_id,
                    h->connection_id, st, RBP_KEYS_STRUCT_SIZE);
        return;
    }
    case RBP_OP_EVENTS_ENABLE: {
        if (!conn_ready_for(s, h)) {
            respond_error(s, h,
                          s->link_state == RBP_LINK_READY ? RBP_STATUS_BAD_STATE
                                                          : RBP_STATUS_LINK_LOST,
                          NULL, false);
            return;
        }
        bool enabled = false;
        bool found = false;
        rbp_tlv_err_t e = rbp_tlv_get_bool(&rd, 1, &enabled, &found);
        if (e != RBP_TLV_OK || !found) {
            respond_error(s, h, RBP_STATUS_INVALID_ARGUMENT, "enabled required", false);
            return;
        }
        if (enabled) {
            ctl_enqueue(s, RBP_KIND_RESPONSE, h->opcode, RBP_STATUS_OK, h->request_id,
                        h->connection_id, NULL, 0);
            s->events_enabled = true;
            keys_emit(s, RBP_KEYS_KIND_SNAPSHOT, RBP_KEYS_REASON_NONE,
                      s->pressed_bits, s->keys_captured_us);
        } else {
            /* spec: stop delivery and clear pending KEYS_STATE, then OK */
            s->events_enabled = false;
            s->key_q_head = 0;
            s->key_q_count = 0;
            ctl_enqueue(s, RBP_KIND_RESPONSE, h->opcode, RBP_STATUS_OK, h->request_id,
                        h->connection_id, NULL, 0);
        }
        return;
    }

    case RBP_OP_GET_VOICE_CAPS: {
        if(!h->connection_id || h->connection_id!=s->connection_id){respond_error(s,h,RBP_STATUS_LINK_LOST,NULL,false);return;}
        rbp_tlv_writer_t w;rbp_tlv_writer_init(&w);uint8_t list[96];unsigned count=0;
        const rbp_audio_caps_t *c=s->voice_caps;
        if(s->voice_state==RBP_VOICE_READY && c && c->count<=16) {
            count=c->count;
            for(unsigned i=0;i<count;i++){audio_put(list+i*6,c->codecs[i].id,4);audio_put(list+i*6+4,c->codecs[i].revision,2);}
        }
        rbp_tlv_put_bytes(&w,1,list,count*6);rbp_tlv_put_u32(&w,2,count?c->max_unit_bytes:0);
        rbp_tlv_put_u16(&w,3,RBP_AUDIO_CONFIG_MAX);
        ctl_enqueue(s,RBP_KIND_RESPONSE,h->opcode,RBP_STATUS_OK,h->request_id,h->connection_id,w.buf,w.len);return;
    }
    case RBP_OP_VOICE_ENABLE: {
        bool enabled = false;
        bool found = false;
        rbp_tlv_err_t e = rbp_tlv_get_bool(&rd, 1, &enabled, &found);
        if (e != RBP_TLV_OK || !found) {
            respond_error(s, h, RBP_STATUS_INVALID_ARGUMENT, "enabled required", false);
            return;
        }
        /* Disable can revoke session intent while the radio is absent.
         * Enable still requires READY and the exact current connection. */
        if ((enabled && !conn_ready_for(s,h)) ||
            (!enabled && h->connection_id && h->connection_id!=s->connection_id)) {
            respond_error(s,h,s->link_state==RBP_LINK_READY?RBP_STATUS_BAD_STATE:RBP_STATUS_LINK_LOST,NULL,false);return;
        }
        const uint8_t *codecs=NULL;uint16_t codec_len=0;bool has_codecs=false,has_max=false;uint32_t max_unit=0;
        if(rbp_tlv_get_bytes(&rd,2,&codecs,&codec_len,&has_codecs)!=RBP_TLV_OK ||
           rbp_tlv_get_u32(&rd,3,&max_unit,&has_max)!=RBP_TLV_OK ||
           (!enabled && (has_codecs || has_max))) {
            respond_error(s,h,RBP_STATUS_INVALID_ARGUMENT,NULL,false);return;
        }
        if(enabled) {
            if(!has_codecs || !has_max || !codec_len || codec_len>96 || codec_len%6 || !max_unit || max_unit>RBP_AUDIO_UNIT_MAX) {
                respond_error(s,h,RBP_STATUS_INVALID_ARGUMENT,NULL,false);return;
            }
            uint32_t last_id=0;uint16_t last_rev=0;bool intersects=false;
            for(unsigned i=0;i<codec_len;i+=6) {
                uint32_t id=audio_u32(codecs+i);uint16_t rev=codecs[i+4]|(codecs[i+5]<<8);
                if(!id || id>=0x80000000u || !rev || id<last_id || (id==last_id && rev<=last_rev)) {
                    respond_error(s,h,RBP_STATUS_INVALID_ARGUMENT,NULL,false);return;
                }
                last_id=id;last_rev=rev;
                if(s->voice_caps)for(unsigned j=0;j<s->voice_caps->count;j++)
                    if(s->voice_caps->codecs[j].id==id && s->voice_caps->codecs[j].revision==rev)intersects=true;
            }
            if(s->voice_state==RBP_VOICE_READY && (!intersects || !s->voice_caps || max_unit<s->voice_caps->max_unit_bytes)) {
                respond_error(s,h,RBP_STATUS_UNSUPPORTED,NULL,false);return;
            }
            if((s->delivering || s->waiting_idle) && (s->accepted_len!=codec_len || s->accepted_max_unit!=max_unit || memcmp(s->accepted_codecs,codecs,codec_len))) {
                respond_error(s,h,RBP_STATUS_BUSY,NULL,false);return;
            }
        }
        if (enabled) {
            if (s->voice_state != RBP_VOICE_READY) {
                respond_error(s, h, RBP_STATUS_VOICE_UNAVAILABLE, NULL, false);
                return;
            }
            memcpy(s->accepted_codecs,codecs,codec_len);s->accepted_len=(uint8_t)codec_len;s->accepted_max_unit=max_unit;
            s->voice_denied=false;
            s->voice_resume_peer=s->peer_valid?s->peer.peer_id:0;
            if (!s->voice_enabled) {
                s->voice_enabled = true;
                s->waiting_idle = s->source_active;
            }
            rbp_tlv_writer_t w;
            rbp_tlv_writer_init(&w);
            rbp_tlv_put_bool(&w, 1, true);
            rbp_tlv_put_bool(&w, 2, s->waiting_idle);
            ctl_enqueue(s, RBP_KIND_RESPONSE, h->opcode, RBP_STATUS_OK, h->request_id,
                        h->connection_id, w.buf, w.len);
        } else {
            s->accepted_len=0;s->accepted_max_unit=0;
            s->voice_resume_peer=0;
            s->voice_denied=true;
            bool was = s->voice_enabled;
            s->voice_enabled = false;
            s->waiting_idle = false;
            if (!s->delivering && (s->source_active || was)) {
                s->cfg.backend.voice_request_stop(s->cfg.backend.user);
            }
            if (s->delivering) {
                voice_finish_stream(s, RBP_END_CONSUMER_DISABLED, true);
                s->voice_ok_deferred = true;
                s->voice_ok_deferred_opcode = h->opcode;
                s->voice_ok_deferred_req = h->request_id;
                s->voice_ok_deferred_conn = h->connection_id;
                s->voice_ok_deferred_is_enable = true;
                if (was) s->cfg.backend.voice_request_stop(s->cfg.backend.user);
                return;
            }
            rbp_tlv_writer_t w;
            rbp_tlv_writer_init(&w);
            rbp_tlv_put_bool(&w, 1, false);
            rbp_tlv_put_bool(&w, 2, false);
            ctl_enqueue(s, RBP_KIND_RESPONSE, h->opcode, RBP_STATUS_OK, h->request_id,
                        h->connection_id, w.buf, w.len);
        }
        return;
    }
    case RBP_OP_VOICE_START: {
        if(!conn_ready_for(s,h)){respond_error(s,h,RBP_STATUS_LINK_LOST,NULL,false);return;}
        if(s->voice_state!=RBP_VOICE_READY){respond_error(s,h,RBP_STATUS_VOICE_UNAVAILABLE,NULL,false);return;}
        if(!s->voice_enabled){respond_error(s,h,RBP_STATUS_BAD_STATE,"voice not enabled",false);return;}
        if(s->source_active || s->delivering || s->ending || s->waiting_idle){
            respond_error(s,h,RBP_STATUS_BUSY,"previous capture not idle",false);return;
        }
        if(!s->cfg.backend.voice_request_start){respond_error(s,h,RBP_STATUS_UNSUPPORTED,NULL,false);return;}
        uint16_t status=s->cfg.backend.voice_request_start(s->cfg.backend.user);
        if(status!=RBP_STATUS_OK){respond_error(s,h,status,NULL,false);return;}
        ctl_enqueue(s,RBP_KIND_RESPONSE,h->opcode,RBP_STATUS_OK,h->request_id,h->connection_id,NULL,0);
        return;
    }
    case RBP_OP_VOICE_STOP: {
        if (!conn_ready_for(s, h)) {
            respond_error(s, h,
                          s->link_state == RBP_LINK_READY ? RBP_STATUS_BAD_STATE
                                                          : RBP_STATUS_LINK_LOST,
                          NULL, false);
            return;
        }
        uint32_t sid = 0;
        bool found = false;
        rbp_tlv_err_t e = rbp_tlv_get_u32(&rd, 1, &sid, &found);
        if (e != RBP_TLV_OK || !found) {
            respond_error(s, h, RBP_STATUS_INVALID_ARGUMENT, "stream_id required", false);
            return;
        }
        if (!s->delivering || sid != s->stream_id) {
            respond_error(s, h, RBP_STATUS_NOT_FOUND, "stream", false);
            return;
        }
        if (!s->stop_requested) {
            s->stop_requested = true;
            s->stop_deadline_ms = s->now_ms + VOICE_STOP_MS;
            s->cfg.backend.voice_request_stop(s->cfg.backend.user);
        }
        ctl_enqueue(s, RBP_KIND_RESPONSE, h->opcode, RBP_STATUS_OK, h->request_id,
                    h->connection_id, NULL, 0);
        return;
    }

    default:
        respond_error(s, h, RBP_STATUS_UNSUPPORTED, NULL, false);
        return;
    }
}

/* ===================== introspection ===================== */

void rbp_server_get_info(const rbp_server_t *s, rbp_server_info_t *out)
{
    out->session_active = s->session_active;
    out->session_id = s->session_id;
    out->connection_id = s->connection_id;
    out->link_state = s->link_state;
    out->events_enabled = s->events_enabled;
    out->voice_enabled = s->voice_enabled;
    out->waiting_idle = s->waiting_idle;
    out->voice_state = s->voice_state;
    out->stream_id = s->stream_id;
}

bool rbp_server_voice_wanted(const rbp_server_t *s) {
#ifdef RBP_STANDALONE
    (void)s;return standalone_wanted();
#else
    return s->session_active && s->voice_enabled && !s->waiting_idle;
#endif
}
bool rbp_server_should_reconnect(const rbp_server_t *s) {
#ifdef RBP_STANDALONE
    return s->peer_valid && !s->reconnect_paused && !s->op_active;
#else
    return s->peer_valid && s->peer.auto_reconnect && !s->reconnect_paused && !s->op_active;
#endif
}

void rbp_server_set_profile(rbp_server_t *s,const rbp_device_profile_t *profile) {
    if(s->cfg.profile==profile)return;
    s->cfg.profile=profile;send_device_state(s,s->connection_id);
}
void rbp_server_adapter_failed(rbp_server_t *s,const char *reason) {
    copy_name(s->link_msg,sizeof s->link_msg,reason);
    rbp_server_on_link(s,RBP_LINK_ERROR,0,s->now_ms);
    s->cfg.backend.disconnect(s->cfg.backend.user);
}

/* A bounded adapter-local preroll may wait for initial codec negotiation.
 * Never authorize capture with no session, after explicit disable, or while
 * the current stream is being drained. This token fences USB session changes. */
uint32_t rbp_server_voice_capture_session(const rbp_server_t *s) {
#ifdef RBP_STANDALONE
    return s->peer_valid && standalone_wanted() ? s->connection_id : 0;
#else
    return s->session_active && s->peer_valid && !s->voice_denied && !s->waiting_idle &&
        s->voice_state!=RBP_VOICE_UNSUPPORTED && s->voice_state!=RBP_VOICE_FAILED
        ?s->session_id:0;
#endif
}
