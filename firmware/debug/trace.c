#include "trace.h"
#include <string.h>
#ifdef __riscv
#include "CH58x_common.h"
static uint32_t lock(void){uint32_t s;SYS_DisableAllIrq(&s);__asm__ volatile("":::"memory");return s;}
static void unlock(uint32_t s){__asm__ volatile("":::"memory");SYS_RecoverIrq(s);}
#elif defined(RBP_TRACE_TEST)
extern uint32_t trace_test_lock(void);
extern void trace_test_unlock(uint32_t);
static uint32_t lock(void){return trace_test_lock();}
static void unlock(uint32_t s){trace_test_unlock(s);}
#else
static uint32_t lock(void){return 0;}
static void unlock(uint32_t s){(void)s;}
#endif
#define CAP 16u
static dt_record records[CAP];
static uint32_t sequence,dropped,highwater,clock_ms,modules=255;
static uint8_t head,count,level_limit=DT_DETAIL;
void dt_time(uint32_t ms){clock_ms=ms;}
void dt_config(uint32_t mask,uint8_t level){uint32_t s=lock();modules=mask;level_limit=level;unlock(s);}
void dt_log(uint8_t module,uint8_t level,uint16_t code,uint32_t a,uint32_t b,uint32_t c,uint32_t d){
    /* Filtered diagnostics must not repeatedly mask radio interrupts. */
    if(module>7 || !(modules&(1u<<module)) || level>level_limit)return;
    uint32_t s=lock();
    if(module>7 || !(modules&(1u<<module)) || level>level_limit){unlock(s);return;}
    uint32_t seq=++sequence;
    if(count==CAP){dropped++;unlock(s);return;}
    dt_record *r=&records[(head+count)%CAP];
    *r=(dt_record){seq,clock_ms,module,level,code,a,b,c,d};
    if(++count>highwater)highwater=count;
    unlock(s);
}
void dt_blob(uint8_t module,uint16_t code,const uint8_t *data,uint16_t len){
    /* Disabled raw tracing must not walk/copy the attribute at all. */
    if(module>7 || !(modules&(1u<<module)) || level_limit<DT_RAW)return;
    for(uint16_t offset=0;offset<len;offset+=12){
        uint32_t chunk[3]={0};uint16_t n=len-offset;if(n>12)n=12;
        memcpy(chunk,data+offset,n);
        dt_log(module,DT_RAW,code,((uint32_t)len<<16)|offset,chunk[0],chunk[1],chunk[2]);
    }
}
uint8_t dt_peek(dt_record *out,uint8_t cap){
    /* One task-context consumer owns peek/consume. Interrupt producers append
     * only; occupied slots stay immutable until that consumer consumes them.
     * Snapshot indexes under lock, then copy with radio interrupts enabled. */
    uint32_t s=lock();uint8_t n=count<cap?count:cap,first=head;
    unlock(s);
    for(uint8_t i=0;i<n;i++)out[i]=records[(first+i)%CAP];
    return n;
}
void dt_consume(uint8_t n){uint32_t s=lock();if(n>count)n=count;head=(head+n)%CAP;count-=n;unlock(s);}
void dt_stats(uint32_t *seq,uint32_t *drop,uint32_t *high){uint32_t s=lock();*seq=sequence;*drop=dropped;*high=highwater;unlock(s);}
