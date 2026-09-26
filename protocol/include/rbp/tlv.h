/* RBP/3.0 flat TLV codec.
 *
 *   tag:u8 | type:u8 | length:u16LE | value
 *
 * Rules (docs/wire-protocol.md section 3):
 *  - fixed-width scalars must use their exact type and length;
 *  - bool is 0 or 1; text is valid UTF-8 without embedded NUL (max 96);
 *  - bytes max 480; no nesting; no duplicate tags in one message;
 *  - unknown tags: length must not run past the buffer, duplicates rejected,
 *    unknown type values only tolerated inside unknown tags.
 */
#ifndef RBP_TLV_H
#define RBP_TLV_H

#include "rbp/defs.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RBP_TLV_MAX_TEXT 96u
#define RBP_TLV_MAX_BYTES 480u

typedef struct {
    const uint8_t *buf;
    size_t len;
    size_t pos;
    uint8_t seen_tags[32]; /* bitset for tags 0..255 */
    bool valid;
} rbp_tlv_reader_t;

typedef enum {
    RBP_TLV_OK = 0,
    RBP_TLV_ERR_MALFORMED = 1,   /* runs past buffer / bad header */
    RBP_TLV_ERR_DUPLICATE = 2,
    RBP_TLV_ERR_TYPE = 3,        /* wrong fixed type or length */
    RBP_TLV_ERR_VALUE = 4,       /* bool not 0/1, invalid UTF-8, over-long */
} rbp_tlv_err_t;

void rbp_tlv_reader_init(rbp_tlv_reader_t *r, const uint8_t *buf, size_t len);

/* Fetch next tag.  On success fills tag/type/val/val_len and returns
 * RBP_TLV_OK.  Does not validate scalar type/length pairs; use
 * rbp_tlv_expect_* helpers for typed access. */
rbp_tlv_err_t rbp_tlv_next(rbp_tlv_reader_t *r, uint8_t *tag, uint8_t *type,
                           const uint8_t **val, uint16_t *val_len);

/* Typed convenience: find a specific tag; *found set false if absent. */
rbp_tlv_err_t rbp_tlv_get_u8(rbp_tlv_reader_t *r, uint8_t tag, uint8_t *out, bool *found);
rbp_tlv_err_t rbp_tlv_get_u16(rbp_tlv_reader_t *r, uint8_t tag, uint16_t *out, bool *found);
rbp_tlv_err_t rbp_tlv_get_u32(rbp_tlv_reader_t *r, uint8_t tag, uint32_t *out, bool *found);
rbp_tlv_err_t rbp_tlv_get_u64(rbp_tlv_reader_t *r, uint8_t tag, uint64_t *out, bool *found);
rbp_tlv_err_t rbp_tlv_get_bool(rbp_tlv_reader_t *r, uint8_t tag, bool *out, bool *found);
rbp_tlv_err_t rbp_tlv_get_text(rbp_tlv_reader_t *r, uint8_t tag,
                               const uint8_t **out, uint16_t *out_len, bool *found);
rbp_tlv_err_t rbp_tlv_get_bytes(rbp_tlv_reader_t *r, uint8_t tag,
                                const uint8_t **out, uint16_t *out_len, bool *found);

/* Writer: append tags.  Tags must be appended in ascending tag order. */
typedef struct {
    uint8_t buf[RBP_MAX_PAYLOAD];
    uint16_t len;
    bool overflow;
    int last_tag;
} rbp_tlv_writer_t;

void rbp_tlv_writer_init(rbp_tlv_writer_t *w);
bool rbp_tlv_put_u8(rbp_tlv_writer_t *w, uint8_t tag, uint8_t v);
bool rbp_tlv_put_u16(rbp_tlv_writer_t *w, uint8_t tag, uint16_t v);
bool rbp_tlv_put_u32(rbp_tlv_writer_t *w, uint8_t tag, uint32_t v);
bool rbp_tlv_put_u64(rbp_tlv_writer_t *w, uint8_t tag, uint64_t v);
bool rbp_tlv_put_bool(rbp_tlv_writer_t *w, uint8_t tag, bool v);
bool rbp_tlv_put_text(rbp_tlv_writer_t *w, uint8_t tag, const char *s); /* NUL-terminated */
bool rbp_tlv_put_bytes(rbp_tlv_writer_t *w, uint8_t tag, const uint8_t *v, uint16_t len);

#ifdef __cplusplus
}
#endif
#endif /* RBP_TLV_H */
