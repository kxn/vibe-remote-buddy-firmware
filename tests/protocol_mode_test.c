/* Exercise the production state machine with scripted ATT replies. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include "../firmware/adapters/rc003/rc003_adapter.c"

static unsigned failures, writes, reads, searches;
static int write_result;
static uint16_t searched_uuid;
static bool voice_wanted;
static unsigned voice_events[6];
static int read_value(void *u,uint16_t h) {(void)u;(void)h;reads++;return 0;}
static int write_command(void *u,uint16_t h,const uint8_t *v,uint16_t n) {
    (void)u;assert(h==43 && n==1 && v[0]==1);writes++;return write_result;
}
static int search(void *u,uint16_t start,uint16_t end,uint16_t uuid) {
    (void)u;(void)start;(void)end;searches++;searched_uuid=uuid;return 0;
}
static const rbp_gatt_client_t gatt={
    .read_value=read_value,.write_command=write_command,.read_chars_by_uuid16=search};

void rbp_server_set_voice_caps(rbp_server_t *s,const rbp_audio_caps_t *c) {(void)s;(void)c;}
void rbp_server_set_profile(rbp_server_t *s,const rbp_device_profile_t *p) {(void)s;(void)p;}
void rbp_server_adapter_failed(rbp_server_t *s,const char *r) {(void)s;(void)r;failures++;}
bool rbp_server_voice_wanted(const rbp_server_t *s) {(void)s;return voice_wanted;}
uint32_t rbp_server_voice_capture_session(const rbp_server_t *s) {(void)s;return 1;}
void rbp_server_on_voice(rbp_server_t *s,const rbp_voice_evt_t *e,uint32_t t) {(void)s;(void)t;voice_events[e->type]++;}
void rbp_server_on_voice_state(rbp_server_t *s,uint8_t v,uint8_t i,uint32_t r) {(void)s;(void)v;(void)i;(void)r;}
void rbp_server_on_link(rbp_server_t *s,rbp_link_state_t v,uint32_t c,uint32_t t) {(void)s;(void)v;(void)c;(void)t;}
void rbp_server_on_battery(rbp_server_t *s,uint8_t l,uint8_t c) {(void)s;(void)l;(void)c;}
void rbp_server_on_keys(rbp_server_t *s,const rbp_keys_report_t *r) {(void)s;(void)r;}

/* Public 147-byte legacy model fingerprint; no per-device identifiers. */
static const char map_hex[]=
    "05010906a1018501050719e029e71500250175019508810295017508810195057501050819012905910295017503910195067508152825fe0507192829fe8100050c0901a10185f1150025017501951809b509b609b709cd09e209e509e709e909ea0a52010a53010a54010a55010a83010a8a010a92010a94010a21020a23020a24020a25020a26020a27020a2a028102c0c0";
static uint8_t map[147];
static void setup(rc003_adapter_t *a) {
    failures=writes=reads=searches=0;write_result=0;searched_uuid=0;
    rc003_adapter_init(a,&gatt,NULL,NULL);
    a->protocol_mode_handle=43;a->state=ST_READ_PROTOCOL_MODE;
    a->variant.family=3;a->variant.map_crc=UINT32_C(0x2c56d124);
}
static void reply(rc003_adapter_t *a,const uint8_t *v,uint16_t n) {
    rbp_gatt_evt_t e={.type=RBP_GATT_EVT_READ_RSP,.value=v,.len=n,.proc_complete=true};
    rc003_adapter_on_gatt(a,&e);
}
static void empty_mode(rc003_adapter_t *a) {
    reply(a,NULL,0);
    assert(writes==1 && reads==1 && a->state==ST_VERIFY_PROTOCOL_MODE);
    reply(a,NULL,0);
    assert(a->protocol_mode_unverified && !a->ready && !a->voice_ready);
}
static void read_map(rc003_adapter_t *a) {
    uint8_t declaration[]={2,45,0};
    rbp_gatt_evt_t e={.type=RBP_GATT_EVT_CHARS_FOUND,.value=declaration,.len=3};
    rc003_adapter_on_gatt(a,&e);
    e.type=RBP_GATT_EVT_PROC_DONE;rc003_adapter_on_gatt(a,&e);
    assert(a->state==ST_READ_MAP);
    reply(a,map,sizeof map);
}
static bool resolve_legacy(rc003_adapter_t *a,uint32_t crc) {
    assert(crc==UINT32_C(0x2c56d124));
    a->variant.family=3;a->variant.map_crc=crc;return true;
}
#include "legacy_discovery_test.inc"
int main(void) {
    for(unsigned i=0;i<sizeof map;i++) {
        unsigned v;assert(sscanf(map_hex+2*i,"%2x",&v)==1);map[i]=(uint8_t)v;
    }
    assert(rbp_crc32c(map,sizeof map)==UINT32_C(0x2c56d124));
    rc003_adapter_t a;uint8_t report=1,boot=0,bad=2,extra[]={1,0};
    setup(&a);reply(&a,&report,1);
    assert(!writes && !failures && a.state==ST_FIND_MAP_CHAR);
    setup(&a);reply(&a,&boot,1);reply(&a,&report,1);
    assert(writes==1 && !failures && !a.protocol_mode_unverified && a.state==ST_FIND_MAP_CHAR);
    setup(&a);reply(&a,NULL,0);reply(&a,&report,1);
    assert(writes==1 && !failures && !a.protocol_mode_unverified && a.protocol_mode_value==1);
    setup(&a);reply(&a,&bad,1);assert(failures==1 && !writes);
    setup(&a);reply(&a,extra,2);assert(failures==1 && !writes);
    setup(&a);reply(&a,NULL,1);assert(failures==1 && !writes);
    setup(&a);reply(&a,&boot,1);reply(&a,&boot,1);assert(failures==1);

    setup(&a);empty_mode(&a);read_map(&a);
    assert(!failures && a.state==ST_ENUM_REPORT_CHARS && a.legacy.selected);
    assert(searched_uuid==RBP_UUID16_HID_REPORT && a.protocol_mode_length==0);
    setup(&a);a.variant.family=0;a.variant.map_crc=0;a.variant.resolve=resolve_legacy;
    empty_mode(&a);read_map(&a);assert(!failures && a.legacy.selected);
    setup(&a);a.variant.family=1;empty_mode(&a);read_map(&a);assert(failures==1);
    setup(&a);a.variant.family=0;a.variant.map_crc=0;empty_mode(&a);read_map(&a);assert(failures==1);
    setup(&a);empty_mode(&a);map[10]^=1;read_map(&a);map[10]^=1;assert(failures==1);

    setup(&a);a.restoring=true;a.legacy.selected=true;a.bat_value_handle=16;
    a.map_len=sizeof map;memcpy(a.map_buf,map,sizeof map);
    empty_mode(&a);assert(!failures && a.state==ST_READ_BATTERY);
    setup(&a);a.restoring=true;a.variant.family=1;empty_mode(&a);assert(failures==1);
    setup(&a);write_result=-1;reply(&a,NULL,0);assert(failures==1 && !reads);
    setup(&a);write_result=RBP_GATT_RETRY;reply(&a,NULL,0);
    assert(!failures && a.state==ST_SET_PROTOCOL_MODE && !reads);
    write_result=0;begin_next(&a,100);reply(&a,&report,1);
    assert(!failures && writes==2 && reads==1 && a.state==ST_FIND_MAP_CHAR);
    test_legacy_discovery();
    puts("Protocol Mode regression tests passed (normal, legacy, rejection, restore, retry).");
    return 0;
}
