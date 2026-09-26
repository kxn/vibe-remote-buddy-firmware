/* RBP/3.0 frame layer: CRC32C, COBS, header pack/parse.
 *
 * Frame on the wire:  COBS(header + payload + CRC32C-u32LE) + 0x00
 * CRC covers header+payload, reflected polynomial 0x82F63B78,
 * init/xorout 0xFFFFFFFF, check("123456789") == 0xE3069283.
 */
#ifndef RBP_FRAME_H
#define RBP_FRAME_H

#include "rbp/defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* CRC32C (Castagnoli, reflected 0x82F63B78). */
uint32_t rbp_crc32c(const uint8_t *data, size_t len);

/* COBS: encode src (len bytes) into dst.  dst needs len + len/254 + 2 bytes.
 * Returns bytes written (>= len+1). */
size_t rbp_cobs_encode(const uint8_t *src, size_t len, uint8_t *dst);

/* COBS: decode one complete record from src (len bytes, no trailing 0x00).
 * Returns decoded length written to dst (dst needs len bytes), or 0 on error. */
size_t rbp_cobs_decode(const uint8_t *src, size_t len, uint8_t *dst);

/* 32B little-endian header layout */
typedef struct {
    uint8_t magic0, magic1;
    uint8_t major, minor;
    uint8_t kind;
    uint8_t flags;
    uint16_t header_size;
    uint32_t session_id;
    uint32_t tx_seq;
    uint32_t request_id;
    uint16_t opcode;
    uint16_t status;
    uint16_t payload_size;
    uint16_t reserved;
    uint32_t connection_id;
} rbp_header_t;

/* Pack header into 32 bytes. */
void rbp_header_pack(const rbp_header_t *h, uint8_t out[RBP_HEADER_SIZE]);

/* Parse 32 bytes into h.  Returns false if magic/header_size/reserved bad. */
bool rbp_header_parse(const uint8_t in[RBP_HEADER_SIZE], rbp_header_t *h);

/* Frame builder: streaming encoder with deferred tx_seq assignment.
 *
 * The spec requires tx_seq to be allocated when the frame is actually
 * handed to the transport (queued frames may be dropped without a gap).
 * Usage:
 *   rbp_txframe_t f; rbp_txframe_begin(&f, header-without-tx_seq);
 *   rbp_txframe_payload(&f, bytes, len);            (total <= 512)
 *   rbp_txframe_commit(&f, &seq, out);              (assigns tx_seq)
 *   transport_write(out, n); transport_write(&DELIM, 1);
 */
typedef struct rbp_txseq {
    uint32_t next_tx_seq; /* starts at 1 per direction */
} rbp_txseq_t;

typedef struct {
    rbp_header_t hdr;      /* tx_seq filled at commit */
    uint8_t payload[RBP_MAX_PAYLOAD];
    uint16_t payload_len;
} rbp_txframe_t;

void rbp_txseq_init(rbp_txseq_t *s);
void rbp_txframe_begin(rbp_txframe_t *f, const rbp_header_t *hdr);
bool rbp_txframe_payload(rbp_txframe_t *f, const uint8_t *data, size_t len);
/* Assigns tx_seq, computes CRC and COBS-encodes into out.
 * out must have RBP_MAX_ENCODED bytes space.  Returns wire length
 * or 0 on overflow.  The caller then writes out[0..ret) followed by 0x00. */
size_t rbp_txframe_commit(rbp_txframe_t *f, rbp_txseq_t *seq, uint8_t *out);

/* Convenience: encode a complete frame (header + payload) in one go. */
size_t rbp_frame_encode(const rbp_header_t *hdr, rbp_txseq_t *seq,
                        const uint8_t *payload, size_t payload_len, uint8_t *out);

/* Frame receiver state machine: feed raw stream bytes (which may include
 * 0x00 delimiters and garbage), receive complete frames.
 *
 * Policy (docs/wire-protocol.md section 2):
 *  - empty delimiters ignored;
 *  - non-zero run > 551 bytes or half-frame older than 1000 ms -> drop up to
 *    next delimiter (frame_error reported);
 *  - CRC/magic/version checks are done by the caller via rbp_frame_decode.
 */
typedef struct {
    uint8_t state; /* internal rx_state_t */
    uint8_t buf[RBP_COBS_RECV_MAX]; /* current non-zero run (no delimiter) */
    size_t len;
    uint32_t frame_start_ms; /* monotonic ms when run started */
} rbp_rxparser_t;

typedef enum {
    RBP_RX_NONE = 0,          /* no complete frame yet */
    RBP_RX_FRAME,             /* frame decoded, ctx->frame valid */
    RBP_RX_OVERFLOW,          /* run too long; dropped up to delimiter */
    RBP_RX_TIMEOUT,           /* half-frame stale; dropped */
    RBP_RX_GARBAGE,           /* delimiter found but header/CRC bad; dropped */
} rbp_rx_event_t;

typedef struct {
    rbp_rx_event_t event;
    /* Valid when event == RBP_RX_FRAME: */
    rbp_header_t header;
    /* Borrowed until the next feed/init on this parser. Copy when retaining
     * an event beyond its callback. This is a C ABI change, not a wire change. */
    const uint8_t *payload;
    uint16_t payload_len;
    /* CRC ok (caller still must check session/kind etc.) */
    bool crc_ok;
    /* If non-zero, unconsumed bytes of the previous feed; caller must call
     * feed(p, resume, resume_len, ...) again until resume_len == 0 or
     * event == RBP_RX_NONE.  The buffer must stay valid (zero-copy). */
    const uint8_t *resume;
    size_t resume_len;
} rbp_rx_event_ctx_t;

void rbp_rxparser_init(rbp_rxparser_t *p, uint32_t now_ms);
/* feed bytes; each call processes until input is exhausted or one event is
 * produced.  If ctx->resume_len != 0 after an event, feed the resume range
 * again (same memory).  now_ms is a monotonic millisecond clock used for
 * the half-frame timeout. */
rbp_rx_event_t rbp_rxparser_feed(rbp_rxparser_t *p, const uint8_t *data,
                                 size_t len, uint32_t now_ms,
                                 rbp_rx_event_ctx_t *ctx);

/* Convenience pump: repeatedly feeds (data,len) and invokes on_frame for
 * each valid frame; handles the resume cursor correctly.  on_event receives
 * every event (incl. frames) when non-NULL.  Returns after the whole range
 * is consumed. */
typedef void (*rbp_rx_event_fn)(rbp_rx_event_ctx_t *ctx, void *user);
void rbp_rx_drain(rbp_rxparser_t *p, const uint8_t *data, size_t len,
                  uint32_t now_ms, rbp_rx_event_ctx_t *ctx,
                  rbp_rx_event_fn on_event, void *user);

#ifdef __cplusplus
}
#endif
#endif /* RBP_FRAME_H */
