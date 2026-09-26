#ifndef RBP_DEBUG_TRACE_H
#define RBP_DEBUG_TRACE_H
#include <stdint.h>
#include <stddef.h>
enum {DT_SYS=0,DT_USB,DT_SESSION,DT_BLE,DT_GATT,DT_HID,DT_ATVV,DT_STORE};
enum {DT_ERROR=1,DT_INFO=2,DT_DETAIL=3,DT_RAW=4};
/* All record fields are little endian on the CH582F. Wire layout is 28 B. */
typedef struct {uint32_t sequence,ms;uint8_t module,level;uint16_t code;uint32_t a,b,c,d;} dt_record;
#ifdef RBP_DEBUG
void dt_time(uint32_t ms);
void dt_config(uint32_t mask,uint8_t level);
void dt_log(uint8_t module,uint8_t level,uint16_t code,uint32_t a,uint32_t b,uint32_t c,uint32_t d);
void dt_blob(uint8_t module,uint16_t code,const uint8_t *data,uint16_t len);
uint8_t dt_peek(dt_record *records,uint8_t count);
void dt_consume(uint8_t count);
void dt_stats(uint32_t *sequence,uint32_t *dropped,uint32_t *highwater);
#define DT(...) dt_log(__VA_ARGS__)
#define DT_BLOB(...) dt_blob(__VA_ARGS__)
#else
#define DT(...) ((void)0)
#define DT_BLOB(...) ((void)0)
#endif
#endif
