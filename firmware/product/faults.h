#ifndef RBP_FAULTS_H
#define RBP_FAULTS_H
#include <stdint.h>
enum {RBP_FAULT_SDK=1,RBP_FAULT_GAP,RBP_FAULT_GATT_API,RBP_FAULT_GATT_EVENT,
      RBP_FAULT_ATT,RBP_FAULT_ATVV,RBP_FAULT_STORAGE,RBP_FAULT_ADAPTER,
      RBP_FAULT_STANDALONE};
/* Diagnostic metadata only; never keys, payloads or memory contents. */
typedef struct {uint32_t sequence;uint16_t domain,stage;uint32_t code,context,count,board_ms;} rbp_fault_t;
#define RBP_FAULT_CAPACITY 4
void rbp_fault_set_time(uint32_t board_ms);
void rbp_fault_record(uint16_t domain,uint16_t stage,uint32_t code,uint32_t context);
uint8_t rbp_fault_snapshot(rbp_fault_t out[RBP_FAULT_CAPACITY],uint32_t *sequence,uint32_t *evicted);
#endif
