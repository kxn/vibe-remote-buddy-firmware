#include "rbp/frame.h"
#include <string.h>

/* ---------------- CRC32C ---------------- */

/* Read-only table stays in flash on the MCU. */
static const uint32_t crc32c_table[256] = {
0x00000000u, 0xf26b8303u, 0xe13b70f7u, 0x1350f3f4u, 0xc79a971fu, 0x35f1141cu, 0x26a1e7e8u, 0xd4ca64ebu,
0x8ad958cfu, 0x78b2dbccu, 0x6be22838u, 0x9989ab3bu, 0x4d43cfd0u, 0xbf284cd3u, 0xac78bf27u, 0x5e133c24u,
0x105ec76fu, 0xe235446cu, 0xf165b798u, 0x030e349bu, 0xd7c45070u, 0x25afd373u, 0x36ff2087u, 0xc494a384u,
0x9a879fa0u, 0x68ec1ca3u, 0x7bbcef57u, 0x89d76c54u, 0x5d1d08bfu, 0xaf768bbcu, 0xbc267848u, 0x4e4dfb4bu,
0x20bd8edeu, 0xd2d60dddu, 0xc186fe29u, 0x33ed7d2au, 0xe72719c1u, 0x154c9ac2u, 0x061c6936u, 0xf477ea35u,
0xaa64d611u, 0x580f5512u, 0x4b5fa6e6u, 0xb93425e5u, 0x6dfe410eu, 0x9f95c20du, 0x8cc531f9u, 0x7eaeb2fau,
0x30e349b1u, 0xc288cab2u, 0xd1d83946u, 0x23b3ba45u, 0xf779deaeu, 0x05125dadu, 0x1642ae59u, 0xe4292d5au,
0xba3a117eu, 0x4851927du, 0x5b016189u, 0xa96ae28au, 0x7da08661u, 0x8fcb0562u, 0x9c9bf696u, 0x6ef07595u,
0x417b1dbcu, 0xb3109ebfu, 0xa0406d4bu, 0x522bee48u, 0x86e18aa3u, 0x748a09a0u, 0x67dafa54u, 0x95b17957u,
0xcba24573u, 0x39c9c670u, 0x2a993584u, 0xd8f2b687u, 0x0c38d26cu, 0xfe53516fu, 0xed03a29bu, 0x1f682198u,
0x5125dad3u, 0xa34e59d0u, 0xb01eaa24u, 0x42752927u, 0x96bf4dccu, 0x64d4cecfu, 0x77843d3bu, 0x85efbe38u,
0xdbfc821cu, 0x2997011fu, 0x3ac7f2ebu, 0xc8ac71e8u, 0x1c661503u, 0xee0d9600u, 0xfd5d65f4u, 0x0f36e6f7u,
0x61c69362u, 0x93ad1061u, 0x80fde395u, 0x72966096u, 0xa65c047du, 0x5437877eu, 0x4767748au, 0xb50cf789u,
0xeb1fcbadu, 0x197448aeu, 0x0a24bb5au, 0xf84f3859u, 0x2c855cb2u, 0xdeeedfb1u, 0xcdbe2c45u, 0x3fd5af46u,
0x7198540du, 0x83f3d70eu, 0x90a324fau, 0x62c8a7f9u, 0xb602c312u, 0x44694011u, 0x5739b3e5u, 0xa55230e6u,
0xfb410cc2u, 0x092a8fc1u, 0x1a7a7c35u, 0xe811ff36u, 0x3cdb9bddu, 0xceb018deu, 0xdde0eb2au, 0x2f8b6829u,
0x82f63b78u, 0x709db87bu, 0x63cd4b8fu, 0x91a6c88cu, 0x456cac67u, 0xb7072f64u, 0xa457dc90u, 0x563c5f93u,
0x082f63b7u, 0xfa44e0b4u, 0xe9141340u, 0x1b7f9043u, 0xcfb5f4a8u, 0x3dde77abu, 0x2e8e845fu, 0xdce5075cu,
0x92a8fc17u, 0x60c37f14u, 0x73938ce0u, 0x81f80fe3u, 0x55326b08u, 0xa759e80bu, 0xb4091bffu, 0x466298fcu,
0x1871a4d8u, 0xea1a27dbu, 0xf94ad42fu, 0x0b21572cu, 0xdfeb33c7u, 0x2d80b0c4u, 0x3ed04330u, 0xccbbc033u,
0xa24bb5a6u, 0x502036a5u, 0x4370c551u, 0xb11b4652u, 0x65d122b9u, 0x97baa1bau, 0x84ea524eu, 0x7681d14du,
0x2892ed69u, 0xdaf96e6au, 0xc9a99d9eu, 0x3bc21e9du, 0xef087a76u, 0x1d63f975u, 0x0e330a81u, 0xfc588982u,
0xb21572c9u, 0x407ef1cau, 0x532e023eu, 0xa145813du, 0x758fe5d6u, 0x87e466d5u, 0x94b49521u, 0x66df1622u,
0x38cc2a06u, 0xcaa7a905u, 0xd9f75af1u, 0x2b9cd9f2u, 0xff56bd19u, 0x0d3d3e1au, 0x1e6dcdeeu, 0xec064eedu,
0xc38d26c4u, 0x31e6a5c7u, 0x22b65633u, 0xd0ddd530u, 0x0417b1dbu, 0xf67c32d8u, 0xe52cc12cu, 0x1747422fu,
0x49547e0bu, 0xbb3ffd08u, 0xa86f0efcu, 0x5a048dffu, 0x8ecee914u, 0x7ca56a17u, 0x6ff599e3u, 0x9d9e1ae0u,
0xd3d3e1abu, 0x21b862a8u, 0x32e8915cu, 0xc083125fu, 0x144976b4u, 0xe622f5b7u, 0xf5720643u, 0x07198540u,
0x590ab964u, 0xab613a67u, 0xb831c993u, 0x4a5a4a90u, 0x9e902e7bu, 0x6cfbad78u, 0x7fab5e8cu, 0x8dc0dd8fu,
0xe330a81au, 0x115b2b19u, 0x020bd8edu, 0xf0605beeu, 0x24aa3f05u, 0xd6c1bc06u, 0xc5914ff2u, 0x37faccf1u,
0x69e9f0d5u, 0x9b8273d6u, 0x88d28022u, 0x7ab90321u, 0xae7367cau, 0x5c18e4c9u, 0x4f48173du, 0xbd23943eu,
0xf36e6f75u, 0x0105ec76u, 0x12551f82u, 0xe03e9c81u, 0x34f4f86au, 0xc69f7b69u, 0xd5cf889du, 0x27a40b9eu,
0x79b737bau, 0x8bdcb4b9u, 0x988c474du, 0x6ae7c44eu, 0xbe2da0a5u, 0x4c4623a6u, 0x5f16d052u, 0xad7d5351u,
};

uint32_t rbp_crc32c(const uint8_t *data, size_t len)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        c = crc32c_table[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* ---------------- COBS ---------------- */

size_t rbp_cobs_encode(const uint8_t *src, size_t len, uint8_t *dst)
{
    const uint8_t *end = src + len;
    uint8_t *dst_start = dst;
    uint8_t *codep = dst++; /* placeholder for first block code */
    uint8_t code = 1;

    while (src < end) {
        if (*src == 0) {
            *codep = code;
            code = 1;
            codep = dst++;
            src++;
        } else {
            *dst++ = *src++;
            code++;
            if (code == 0xFF) {
                *codep = code;
                code = 1;
                codep = dst++;
            }
        }
    }
    *codep = code;
    return (size_t)(dst - dst_start);
}

size_t rbp_cobs_decode(const uint8_t *src, size_t len, uint8_t *dst)
{
    const uint8_t *end = src + len;
    const uint8_t *dst_start = dst;

    while (src < end) {
        uint8_t code = *src++;
        if (code == 0) return 0; /* invalid inside record */
        size_t block = (size_t)(code - 1);
        if ((size_t)(end - src) < block) return 0;
        for (size_t i = 0; i < block; i++) *dst++ = *src++;
        /* a non-final block is followed by the zero that terminated it;
         * the last block carries no implicit zero (encoder drops the
         * wire delimiter). */
        if (code < 0xFF && src != end) *dst++ = 0;
    }
    return (size_t)(dst - dst_start);
}

/* ---------------- header ---------------- */

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void rbp_header_pack(const rbp_header_t *h, uint8_t out[RBP_HEADER_SIZE])
{
    out[0] = h->magic0; out[1] = h->magic1;
    out[2] = h->major; out[3] = h->minor;
    out[4] = h->kind; out[5] = h->flags;
    put16(out + 6, h->header_size);
    put32(out + 8, h->session_id);
    put32(out + 12, h->tx_seq);
    put32(out + 16, h->request_id);
    put16(out + 20, h->opcode);
    put16(out + 22, h->status);
    put16(out + 24, h->payload_size);
    put16(out + 26, h->reserved);
    put32(out + 28, h->connection_id);
}

bool rbp_header_parse(const uint8_t in[RBP_HEADER_SIZE], rbp_header_t *h)
{
    h->magic0 = in[0]; h->magic1 = in[1];
    h->major = in[2]; h->minor = in[3];
    h->kind = in[4]; h->flags = in[5];
    h->header_size = get16(in + 6);
    h->session_id = get32(in + 8);
    h->tx_seq = get32(in + 12);
    h->request_id = get32(in + 16);
    h->opcode = get16(in + 20);
    h->status = get16(in + 22);
    h->payload_size = get16(in + 24);
    h->reserved = get16(in + 26);
    h->connection_id = get32(in + 28);
    return h->magic0 == RBP_MAGIC0 && h->magic1 == RBP_MAGIC1 &&
           h->header_size == RBP_HEADER_SIZE && h->reserved == 0;
}

/* ---------------- frame builder ---------------- */

void rbp_txseq_init(rbp_txseq_t *s) { s->next_tx_seq = 1; }

void rbp_txframe_begin(rbp_txframe_t *f, const rbp_header_t *hdr)
{
    f->hdr = *hdr;
    f->payload_len = 0;
}

bool rbp_txframe_payload(rbp_txframe_t *f, const uint8_t *data, size_t len)
{
    if (f->payload_len > RBP_MAX_PAYLOAD || len > RBP_MAX_PAYLOAD - f->payload_len) return false;
    if (len) memcpy(f->payload + f->payload_len, data, len);
    f->payload_len += len;
    return true;
}

/* Shared encoder: complete-frame callers need no intermediate payload builder. */
static size_t encode_payload(rbp_header_t *h, rbp_txseq_t *seq,
                             const uint8_t *payload, size_t len, uint8_t *out)
{
    uint8_t header[RBP_HEADER_SIZE], tail[4];
    if (len > RBP_MAX_PAYLOAD || (len && !payload)) return 0;
    h->magic0 = RBP_MAGIC0;
    h->magic1 = RBP_MAGIC1;
    h->major = RBP_MAJOR;
    h->minor = RBP_MINOR;
    h->header_size = RBP_HEADER_SIZE;
    h->tx_seq = seq->next_tx_seq;
    h->reserved = 0;
    h->payload_size = (uint16_t)len;
    rbp_header_pack(h, header);
    /* Stream the three pieces through one COBS state. No full raw-frame
     * stack copy; CRC covers only header/payload, never its own trailer. */
    uint8_t *dst=out+1,*codep=out,code=1;
    uint32_t crc=0xffffffffu;
    for(unsigned part=0;part<3;part++) {
        if(part==2)put32(tail,crc^0xffffffffu);
        const uint8_t *p=part==0?header:part==1?payload:tail;
        size_t n=part==0?RBP_HEADER_SIZE:part==1?len:4;
        for(size_t i=0;i<n;i++) {
            uint8_t byte=p[i];
            if(part<2)crc=crc32c_table[(crc^byte)&0xffu]^(crc>>8);
            if(!byte){*codep=code;code=1;codep=dst++;}
            else {
                *dst++=byte;
                if(++code==0xff){*codep=code;code=1;codep=dst++;}
            }
        }
    }
    *codep=code;*dst++=0;seq->next_tx_seq++;
    return (size_t)(dst-out);
}

size_t rbp_frame_encode(const rbp_header_t *hdr, rbp_txseq_t *seq,
                        const uint8_t *payload, size_t payload_len, uint8_t *out)
{
    rbp_header_t h = *hdr;
    return encode_payload(&h, seq, payload, payload_len, out);
}

size_t rbp_txframe_commit(rbp_txframe_t *f, rbp_txseq_t *seq, uint8_t *out)
{
    return encode_payload(&f->hdr, seq, f->payload, f->payload_len, out);
}

/* ---------------- receiver ---------------- */

typedef enum {
    RXS_IDLE = 0,
    RXS_IN_FRAME = 1,
    RXS_DISCARD = 2, /* dropping until next delimiter (after overflow) */
} rx_state_t;

void rbp_rxparser_init(rbp_rxparser_t *p, uint32_t now_ms)
{
    p->state = RXS_IDLE;
    p->len = 0;
    p->frame_start_ms = now_ms;
}

static void rx_reset(rbp_rxparser_t *p, uint32_t now_ms)
{
    p->state = RXS_IDLE;
    p->len = 0;
    p->frame_start_ms = now_ms;
}

rbp_rx_event_t rbp_rxparser_feed(rbp_rxparser_t *p, const uint8_t *data,
                                 size_t len, uint32_t now_ms,
                                 rbp_rx_event_ctx_t *ctx)
{
    ctx->event = RBP_RX_NONE;
    ctx->resume = NULL;
    ctx->resume_len = 0;

    if(p->state==RXS_IN_FRAME && (uint32_t)(now_ms-p->frame_start_ms)>=1000u) {
        p->state=RXS_DISCARD;p->len=0;ctx->event=RBP_RX_TIMEOUT;
        ctx->resume=data;ctx->resume_len=len;return RBP_RX_TIMEOUT;
    }
    for (size_t i = 0; i < len; i++) {
        uint8_t b = data[i];

        if (p->state == RXS_DISCARD) {
            if (b == 0x00) rx_reset(p, now_ms);
            continue;
        }

        if (b == 0x00) {
            if (p->state == RXS_IN_FRAME && p->len > 0) {
                /* COBS decoding only writes behind its read cursor. Reuse
                 * the receive run rather than duplicate a 551-byte buffer. */
                uint8_t *raw=p->buf;
                size_t raw_len = rbp_cobs_decode(p->buf, p->len, raw);
                if (raw_len >= RBP_HEADER_SIZE + 4 && raw_len <= RBP_MAX_FRAME_RAW) {
                    size_t plen = raw_len - RBP_HEADER_SIZE - 4;
                    if (rbp_header_parse(raw, &ctx->header)) {
                        ctx->crc_ok = (rbp_crc32c(raw, RBP_HEADER_SIZE + plen) ==
                                       get32(raw + RBP_HEADER_SIZE + plen));
                        if (plen > RBP_MAX_PAYLOAD) plen = RBP_MAX_PAYLOAD;
                        ctx->payload=raw+RBP_HEADER_SIZE;
                        ctx->payload_len = (uint16_t)plen;
                        rx_reset(p, now_ms);
                        ctx->event = RBP_RX_FRAME;
                        ctx->resume = data + i + 1;
                        ctx->resume_len = len - i - 1;
                        return RBP_RX_FRAME;
                    }
                }
                rx_reset(p, now_ms);
                ctx->event = RBP_RX_GARBAGE;
                ctx->resume = data + i + 1;
                ctx->resume_len = len - i - 1;
                return RBP_RX_GARBAGE;
            }
            /* empty delimiter between frames */
            rx_reset(p, now_ms);
            continue;
        }

        if (p->state == RXS_IDLE) {
            p->state = RXS_IN_FRAME;
            p->len = 0;
            p->frame_start_ms = now_ms;
        }
        if (p->len >= RBP_COBS_RECV_MAX) {
            p->state = RXS_DISCARD;
            ctx->event = RBP_RX_OVERFLOW;
            ctx->resume = data + i + 1;
            ctx->resume_len = len - i - 1;
            return RBP_RX_OVERFLOW;
        }
        p->buf[p->len++] = b;
    }

    if (p->state == RXS_IN_FRAME &&
        (uint32_t)(now_ms - p->frame_start_ms) > 1000u) {
        p->state=RXS_DISCARD;p->len=0;
        ctx->event = RBP_RX_TIMEOUT;
        return RBP_RX_TIMEOUT;
    }
    return RBP_RX_NONE;
}

void rbp_rx_drain(rbp_rxparser_t *p, const uint8_t *data, size_t len,
                  uint32_t now_ms, rbp_rx_event_ctx_t *ctx,
                  rbp_rx_event_fn on_event, void *user)
{
    const uint8_t *cur = data;
    size_t left = len;
    while (left > 0) {
        rbp_rx_event_t ev = rbp_rxparser_feed(p, cur, left, now_ms, ctx);
        /* feed consumed everything up to the event; the remainder is
         * exactly ctx->resume_len bytes */
        cur += (left - ctx->resume_len);
        left = ctx->resume_len;
        if (on_event) on_event(ctx, user);
        if (ev == RBP_RX_NONE) break; /* nothing more will come out */
        /* force progress even if a future feed change misbehaves */
        if (ctx->resume_len >= (size_t)-1) break;
    }
}
