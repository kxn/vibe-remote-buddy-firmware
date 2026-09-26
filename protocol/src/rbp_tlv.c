#include "rbp/tlv.h"
#include <string.h>

/* UTF-8 validation (rejects overlong encodings, surrogates, >U+10FFFF). */
static bool utf8_valid(const uint8_t *s, size_t len)
{
    size_t i = 0;
    while (i < len) {
        uint8_t c = s[i];
        if (c == 0) return false; /* embedded NUL rejected */
        if (c < 0x80) { i++; continue; }
        uint32_t cp;
        int n;
        if ((c & 0xE0) == 0xC0 && c >= 0xC2) { cp = c & 0x1F; n = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 2; }
        else if ((c & 0xF8) == 0xF0 && c <= 0xF4) { cp = c & 0x07; n = 3; }
        else return false;
        if ((size_t)(len - i - 1) < (size_t)n) return false;
        for (int k = 1; k <= n; k++) {
            if ((s[i + k] & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (s[i + k] & 0x3F);
        }
        if (n == 2 && cp < 0x800) return false;
        if (n == 3 && (cp < 0x10000 || (cp >= 0xD800 && cp <= 0xDFFF))) return false;
        if (n == 3 && cp > 0x10FFFF) return false;
        i += (size_t)n + 1;
    }
    return true;
}

void rbp_tlv_reader_init(rbp_tlv_reader_t *r, const uint8_t *buf, size_t len)
{
    r->buf = buf;
    r->len = len;
    r->pos = 0;
    memset(r->seen_tags, 0, sizeof(r->seen_tags));
    r->valid = true;
}

rbp_tlv_err_t rbp_tlv_next(rbp_tlv_reader_t *r, uint8_t *tag, uint8_t *type,
                           const uint8_t **val, uint16_t *val_len)
{
    if (!r->valid) return RBP_TLV_ERR_MALFORMED;
    if (r->pos == r->len) return RBP_TLV_ERR_MALFORMED; /* caller stops on r->pos==len */
    if (r->len - r->pos < 4) { r->valid = false; return RBP_TLV_ERR_MALFORMED; }
    uint8_t t = r->buf[r->pos];
    uint8_t ty = r->buf[r->pos + 1];
    uint16_t l = (uint16_t)(r->buf[r->pos + 2] | (r->buf[r->pos + 3] << 8));
    if (r->len - r->pos - 4 < l) { r->valid = false; return RBP_TLV_ERR_MALFORMED; }
    if (r->seen_tags[t >> 3] & (1u << (t & 7))) { r->valid = false; return RBP_TLV_ERR_DUPLICATE; }
    r->seen_tags[t >> 3] |= (uint8_t)(1u << (t & 7));
    /* unknown types are only skipped, never returned to typed getters */
    r->pos += 4 + l;
    *tag = t;
    *type = ty;
    *val = r->buf + (r->pos - l);
    *val_len = l;
    return RBP_TLV_OK;
}

/* Restart iteration: typed getters scan independently. */
static rbp_tlv_err_t scan(rbp_tlv_reader_t *r, uint8_t want,
                          uint8_t *out_type, const uint8_t **out_val,
                          uint16_t *out_len, bool *found)
{
    rbp_tlv_reader_t tmp = *r;
    tmp.pos = 0;
    memset(tmp.seen_tags, 0, sizeof(tmp.seen_tags));
    *found = false;
    while (tmp.pos < tmp.len) {
        uint8_t tag, type;
        const uint8_t *val;
        uint16_t vlen;
        rbp_tlv_err_t e = rbp_tlv_next(&tmp, &tag, &type, &val, &vlen);
        if (e != RBP_TLV_OK) return e; /* malformed anywhere -> reject whole message */
        if (tag == want) {
            *found = true;
            *out_type = type;
            *out_val = val;
            *out_len = vlen;
        }
    }
    return RBP_TLV_OK;
}

static rbp_tlv_err_t get_fixed(rbp_tlv_reader_t *r, uint8_t tag, uint8_t want_type,
                               uint16_t want_len /* 0 = any */, const uint8_t **out,
                               uint16_t *out_len /* may be NULL */, bool *found)
{
    uint8_t type=0;
    uint16_t vlen=0;
    rbp_tlv_err_t e = scan(r, tag, &type, out, &vlen, found);
    if (e != RBP_TLV_OK) return e;
    if (*found) {
        if (type != want_type || (want_len != 0 && vlen != want_len))
            return RBP_TLV_ERR_TYPE;
        if (out_len) *out_len = vlen;
    }
    return RBP_TLV_OK;
}

rbp_tlv_err_t rbp_tlv_get_u8(rbp_tlv_reader_t *r, uint8_t tag, uint8_t *out, bool *found)
{
    const uint8_t *v;
    rbp_tlv_err_t e = get_fixed(r, tag, RBP_TLV_T_U8, 1, &v, NULL, found);
    if (e == RBP_TLV_OK && *found) *out = v[0];
    return e;
}

rbp_tlv_err_t rbp_tlv_get_u16(rbp_tlv_reader_t *r, uint8_t tag, uint16_t *out, bool *found)
{
    const uint8_t *v;
    rbp_tlv_err_t e = get_fixed(r, tag, RBP_TLV_T_U16, 2, &v, NULL, found);
    if (e == RBP_TLV_OK && *found) *out = (uint16_t)(v[0] | (v[1] << 8));
    return e;
}

rbp_tlv_err_t rbp_tlv_get_u32(rbp_tlv_reader_t *r, uint8_t tag, uint32_t *out, bool *found)
{
    const uint8_t *v;
    rbp_tlv_err_t e = get_fixed(r, tag, RBP_TLV_T_U32, 4, &v, NULL, found);
    if (e == RBP_TLV_OK && *found)
        *out = (uint32_t)v[0] | ((uint32_t)v[1] << 8) | ((uint32_t)v[2] << 16) | ((uint32_t)v[3] << 24);
    return e;
}

rbp_tlv_err_t rbp_tlv_get_u64(rbp_tlv_reader_t *r, uint8_t tag, uint64_t *out, bool *found)
{
    const uint8_t *v;
    rbp_tlv_err_t e = get_fixed(r, tag, RBP_TLV_T_U64, 8, &v, NULL, found);
    if (e == RBP_TLV_OK && *found) {
        uint64_t x = 0;
        for (int i = 7; i >= 0; i--) x = (x << 8) | v[i];
        *out = x;
    }
    return e;
}

rbp_tlv_err_t rbp_tlv_get_bool(rbp_tlv_reader_t *r, uint8_t tag, bool *out, bool *found)
{
    const uint8_t *v;
    rbp_tlv_err_t e = get_fixed(r, tag, RBP_TLV_T_BOOL, 1, &v, NULL, found);
    if (e == RBP_TLV_OK && *found) {
        if (v[0] > 1) return RBP_TLV_ERR_VALUE;
        *out = v[0] != 0;
    }
    return e;
}

rbp_tlv_err_t rbp_tlv_get_text(rbp_tlv_reader_t *r, uint8_t tag,
                               const uint8_t **out, uint16_t *out_len, bool *found)
{
    rbp_tlv_err_t e = get_fixed(r, tag, RBP_TLV_T_TEXT, 0, out, out_len, found);
    /* type checked; length free-form but bounded */
    if (e != RBP_TLV_OK) return e;
    if (*found) {
        uint16_t l = *out_len;
        if (l > RBP_TLV_MAX_TEXT) return RBP_TLV_ERR_VALUE;
        if (l > 0 && !utf8_valid(*out, l)) return RBP_TLV_ERR_VALUE;
    }
    return RBP_TLV_OK;
}

rbp_tlv_err_t rbp_tlv_get_bytes(rbp_tlv_reader_t *r, uint8_t tag,
                                const uint8_t **out, uint16_t *out_len, bool *found)
{
    rbp_tlv_err_t e = get_fixed(r, tag, RBP_TLV_T_BYTES, 0, out, out_len, found);
    if (e != RBP_TLV_OK) return e;
    if (*found && *out_len > RBP_TLV_MAX_BYTES) return RBP_TLV_ERR_VALUE;
    return RBP_TLV_OK;
}

/* ---------------- writer ---------------- */

void rbp_tlv_writer_init(rbp_tlv_writer_t *w)
{
    w->len = 0;
    w->overflow = false;
    w->last_tag = -1;
}

static bool w_put(rbp_tlv_writer_t *w, uint8_t tag, uint8_t type, const void *v, uint16_t l)
{
    if (w->last_tag >= (int)tag) w->overflow = true; /* enforce ascending order */
    w->last_tag = tag;
    if (w->len + 4u + l > RBP_MAX_PAYLOAD) { w->overflow = true; return false; }
    uint8_t *p = w->buf + w->len;
    p[0] = tag; p[1] = type;
    p[2] = (uint8_t)l; p[3] = (uint8_t)(l >> 8);
    memcpy(p + 4, v, l);
    w->len = (uint16_t)(w->len + 4 + l);
    return true;
}

bool rbp_tlv_put_u8(rbp_tlv_writer_t *w, uint8_t tag, uint8_t v) { return w_put(w, tag, RBP_TLV_T_U8, &v, 1); }
bool rbp_tlv_put_u16(rbp_tlv_writer_t *w, uint8_t tag, uint16_t v)
{
    uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    return w_put(w, tag, RBP_TLV_T_U16, b, 2);
}
bool rbp_tlv_put_u32(rbp_tlv_writer_t *w, uint8_t tag, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    return w_put(w, tag, RBP_TLV_T_U32, b, 4);
}
bool rbp_tlv_put_u64(rbp_tlv_writer_t *w, uint8_t tag, uint64_t v)
{
    uint8_t b[8];
    for (int i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (8 * i));
    return w_put(w, tag, RBP_TLV_T_U64, b, 8);
}
bool rbp_tlv_put_bool(rbp_tlv_writer_t *w, uint8_t tag, bool v)
{
    uint8_t b = v ? 1 : 0;
    return w_put(w, tag, RBP_TLV_T_BOOL, &b, 1);
}
bool rbp_tlv_put_text(rbp_tlv_writer_t *w, uint8_t tag, const char *s)
{
    size_t l = strlen(s);
    if (l > RBP_TLV_MAX_TEXT) { w->overflow = true; return false; }
    return w_put(w, tag, RBP_TLV_T_TEXT, s, (uint16_t)l);
}
bool rbp_tlv_put_bytes(rbp_tlv_writer_t *w, uint8_t tag, const uint8_t *v, uint16_t len)
{
    if (len > RBP_TLV_MAX_BYTES) { w->overflow = true; return false; }
    return w_put(w, tag, RBP_TLV_T_BYTES, v, len);
}
