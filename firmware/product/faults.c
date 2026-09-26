#include "faults.h"
#include <string.h>
#if defined(__riscv)
extern void SYS_DisableAllIrq(uint32_t *);
extern void SYS_RecoverIrq(uint32_t);
#else
static void SYS_DisableAllIrq(uint32_t *s){*s=0;}
static void SYS_RecoverIrq(uint32_t s){(void)s;}
#endif
static rbp_fault_t records[RBP_FAULT_CAPACITY];
static uint32_t serial,evictions;
static volatile uint32_t clock_ms;
void rbp_fault_set_time(uint32_t board_ms){clock_ms=board_ms;}
static uint8_t used,next;
void rbp_fault_record(uint16_t domain,uint16_t stage,uint32_t code,uint32_t context) {
    uint32_t irq;SYS_DisableAllIrq(&irq);
    rbp_fault_t *r=&records[(next+RBP_FAULT_CAPACITY-1)%RBP_FAULT_CAPACITY];
    if(++serial==0)++serial;
    if(used && r->domain==domain && r->stage==stage && r->code==code && r->context==context) {
        if(r->count!=UINT32_MAX)r->count++;
        r->sequence=serial;r->board_ms=clock_ms;
    } else {
        if(used==RBP_FAULT_CAPACITY && evictions!=UINT32_MAX)evictions++;
        records[next]=(rbp_fault_t){serial,domain,stage,code,context,1,clock_ms};
        next=(next+1)%RBP_FAULT_CAPACITY;if(used<RBP_FAULT_CAPACITY)used++;
    }
    SYS_RecoverIrq(irq);
}
uint8_t rbp_fault_snapshot(rbp_fault_t out[RBP_FAULT_CAPACITY],uint32_t *sequence,uint32_t *evicted) {
    uint32_t irq;SYS_DisableAllIrq(&irq);
    uint8_t count=used;
    for(uint8_t i=0;i<count;i++)out[i]=records[(next+RBP_FAULT_CAPACITY-count+i)%RBP_FAULT_CAPACITY];
    *sequence=serial;if(evicted)*evicted=evictions;SYS_RecoverIrq(irq);return count;
}
