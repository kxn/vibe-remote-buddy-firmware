/* Bounded HID Report Map parser + input report decoder for HOGP hosts.
 *
 * Supports the descriptor structures accepted by this implementation:
 * short items only, input reports identified by Report ID, usage page of the
 * first input item, bit/byte field sizes.  Long items or absurd sizes are
 * rejected explicitly (design constraint, not a spec limit).
 */
#ifndef RBP_REPORT_MAP_H
#define RBP_REPORT_MAP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HOGP_MAX_INPUT_REPORTS 8
#define HOGP_MAX_MAP_SIZE      512

typedef struct {
    uint8_t report_id;    /* 0 = no report id used */
    uint16_t usage_page;  /* usage page in effect at the first INPUT item */
    uint16_t bits;        /* total input bits for this report id */
    uint16_t bytes;       /* (bits+7)/8 */
} hogp_input_report_t;

#define HOGP_MAX_FIELDS 64
typedef struct {
    uint16_t offset, page, usage_min, usage_max, logical_min;
    uint8_t report_id, size, count;
    bool variable;
} hogp_field_t;
typedef struct {
    hogp_field_t fields[HOGP_MAX_FIELDS];
    uint8_t field_count;
    hogp_input_report_t inputs[HOGP_MAX_INPUT_REPORTS];
    uint8_t input_count;
    bool uses_report_ids;
} hogp_report_map_t;

/* Parse a Report Map descriptor.  Returns false on malformed/unsupported. */
bool hogp_report_map_parse(const uint8_t *data, size_t len, hogp_report_map_t *out);

/* GATT Report values exclude the Report ID byte; id comes from 0x2908.
 * Output bitmap is by stable key_id, not catalog slot. */
bool hogp_decode_report(const hogp_report_map_t *map, uint8_t id, const uint8_t *data, size_t len, uint64_t *keys);
uint16_t hogp_usage_to_key(uint16_t usage_page, uint16_t usage);
/* Explicit Boot layout/profile only; never used as a generic length fallback. */
bool hogp_decode_boot_keyboard(const uint8_t *data,size_t len,uint64_t *keys);

#ifdef __cplusplus
}
#endif
#endif
