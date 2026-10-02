/* Scripted 32B8 GATT layout: no address, bond keys or recorded audio. The
 * map declares input F1; the actual database exposes input 1/3 and feature
 * notifications 5/6/7/8. Drive every discovery step through production code. */
static const struct {uint16_t value,cccd,ref;uint8_t id,type,properties;} db[]={
    {52,53,54,1,1,26},{56,0,57,2,2,14},{59,60,61,3,1,26},
    {63,0,64,4,3,10},{66,67,68,5,3,26},{70,71,72,6,3,26},
    {74,75,76,7,3,26},{78,79,80,8,3,26},{82,0,83,9,2,10},
    {85,86,87,10,3,26},{89,0,90,224,3,10},{92,0,93,225,3,10},
    {95,0,96,226,3,10},{98,0,99,227,3,10},{101,0,102,228,3,10},
    {104,0,105,229,3,10},{107,0,108,230,3,10},{110,0,111,231,3,10},
    {113,0,114,232,3,10},{116,0,117,233,3,10},{119,0,120,234,3,10},
    {122,0,123,235,3,10},{125,0,126,236,3,10},{128,0,129,237,3,10},
    {131,132,133,238,3,26},
};
enum {SIM_NONE,SIM_MTU,SIM_SERVICE,SIM_CHARS,SIM_DESCS,SIM_READ,SIM_WRITE};
static struct {unsigned op;uint16_t x,y,z;} pending_op;
static uint16_t omit_cccd,fail_cccd;
static unsigned mode_commands,mic_commands;
static int sim_queue(unsigned op,uint16_t x,uint16_t y,uint16_t z) {
    assert(pending_op.op==SIM_NONE);pending_op.op=op;
    pending_op.x=x;pending_op.y=y;pending_op.z=z;return 0;
}
static int sim_mtu(void *u,uint16_t m) {(void)u;return sim_queue(SIM_MTU,m,0,0);}
static int sim_service(void *u,uint16_t id) {(void)u;return sim_queue(SIM_SERVICE,id,0,0);}
static int sim_chars(void *u,uint16_t s,uint16_t e,uint16_t id) {(void)u;return sim_queue(SIM_CHARS,s,e,id);}
static int sim_descs(void *u,uint16_t s,uint16_t e) {(void)u;return sim_queue(SIM_DESCS,s,e,0);}
static int sim_read(void *u,uint16_t h) {(void)u;return sim_queue(SIM_READ,h,0,0);}
static int sim_write(void *u,uint16_t h,const uint8_t *v,uint16_t n) {
    (void)u;
    if(h==63) {assert(n==20 && v[3]==0x80 && v[4]<=1);mic_commands++;}
    else {assert(n==2 && !v[1] && v[0]==(h==4?2:1));}
    return sim_queue(SIM_WRITE,h,0,0);
}
static int sim_command(void *u,uint16_t h,const uint8_t *v,uint16_t n) {
    (void)u;assert(h==43 && n==1 && v[0]==1);mode_commands++;return 0;
}
static const rbp_gatt_client_t sim_gatt={.exchange_mtu=sim_mtu,
    .disc_service_by_uuid16=sim_service,.read_chars_by_uuid16=sim_chars,
    .disc_char_descs=sim_descs,.read_value=sim_read,.write_value=sim_write,
    .write_command=sim_command};
static void sim_event(rc003_adapter_t *a,rbp_gatt_evt_type_t type) {
    rbp_gatt_evt_t e={.type=type};rc003_adapter_on_gatt(a,&e);
}
static void sim_char(rc003_adapter_t *a,uint16_t h,uint8_t props) {
    uint8_t value[]={props,(uint8_t)h,(uint8_t)(h>>8)};
    rbp_gatt_evt_t e={.type=RBP_GATT_EVT_CHARS_FOUND,.value=value,.len=3};
    rc003_adapter_on_gatt(a,&e);
}
static void sim_descriptor(rc003_adapter_t *a,uint16_t h,uint16_t uuid) {
    if(h==omit_cccd && uuid==0x2902)return;
    rbp_gatt_evt_t e={.type=RBP_GATT_EVT_DESC_FOUND,.desc_handle=h,.uuid16=uuid};
    rc003_adapter_on_gatt(a,&e);
}
static void sim_respond(rc003_adapter_t *a) {
    unsigned op=pending_op.op;uint16_t x=pending_op.x,y=pending_op.y,z=pending_op.z;
    pending_op.op=SIM_NONE;a->now_ms+=5;
    switch(op) {
    case SIM_MTU: {
        rbp_gatt_evt_t e={.type=RBP_GATT_EVT_MTU_UPDATED,.mtu=247};rc003_adapter_on_gatt(a,&e);break;
    }
    case SIM_SERVICE: {
        assert(x==0x1801 || x==0x1812 || x==0x180f);
        rbp_gatt_evt_t e={.type=RBP_GATT_EVT_SERVICE_FOUND,
            .svc_start=x==0x1801?1:x==0x1812?37:14,
            .svc_end=x==0x1801?4:x==0x1812?133:17};
        rc003_adapter_on_gatt(a,&e);sim_event(a,RBP_GATT_EVT_PROC_DONE);break;
    }
    case SIM_CHARS:
        switch(z) {
        case 0x2a05:sim_char(a,3,32);break;
        case 0x2a4a:sim_char(a,39,2);break;
        case 0x2a4e:sim_char(a,43,6);break;
        case 0x2a4b:sim_char(a,45,2);break;
        case 0x2a19:sim_char(a,16,18);break;
        case 0x2a4d:
            for(unsigned i=0;i<sizeof db/sizeof db[0];i++)sim_char(a,db[i].value,db[i].properties);
            break;
        default:assert(false);
        }
        sim_event(a,RBP_GATT_EVT_PROC_DONE);break;
    case SIM_DESCS:
        for(unsigned h=x;h<=y;h++) {
            if(h==4 || h==17)sim_descriptor(a,h,0x2902);
            for(unsigned i=0;i<sizeof db/sizeof db[0];i++) {
                if(h==db[i].value-1)sim_descriptor(a,h,0x2803);
                if(h==db[i].cccd)sim_descriptor(a,h,0x2902);
                if(h==db[i].ref)sim_descriptor(a,h,0x2908);
            }
        }
        sim_event(a,RBP_GATT_EVT_PROC_DONE);break;
    case SIM_READ:
        if(x==43)reply(a,NULL,0);
        else if(x==45)reply(a,map,sizeof map);
        else if(x==39) {uint8_t info[]={0x11,1,0,3};reply(a,info,4);}
        else if(x==16) {uint8_t battery=73;reply(a,&battery,1);}
        else {
            bool found=false;
            for(unsigned i=0;i<sizeof db/sizeof db[0];i++)if(x==db[i].ref) {
                uint8_t ref[]={db[i].id,db[i].type};reply(a,ref,2);found=true;break;
            }
            assert(found);
        }
        break;
    case SIM_WRITE:
        if(x==fail_cccd) {
            rbp_gatt_evt_t e={.type=RBP_GATT_EVT_PROC_ERROR,.status=3};rc003_adapter_on_gatt(a,&e);
        } else sim_event(a,RBP_GATT_EVT_WRITE_DONE);
        break;
    default:assert(false);
    }
}
static void sim_drain(rc003_adapter_t *a) {
    unsigned count=0;
    while(pending_op.op && a->state!=ST_FAILED) {assert(++count<200);sim_respond(a);}
}
static void sim_start(rc003_adapter_t *a,bool learned) {
    failures=0;mode_commands=mic_commands=0;pending_op.op=SIM_NONE;
    voice_wanted=false;memset(voice_events,0,sizeof voice_events);
    rc003_adapter_init(a,&sim_gatt,NULL,NULL);
    a->variant.family=3;a->variant.map_crc=UINT32_C(0x2c56d124);
    if(learned) {
        a->variant.raw_count=2;
        a->variant.raw_keys[0].report=1;a->variant.raw_keys[0].usage=0x3e;a->variant.raw_keys[0].key=RBP_KEY_VOICE;
        a->variant.raw_keys[1].report=3;a->variant.raw_keys[1].usage=0xe9;a->variant.raw_keys[1].key=RBP_KEY_VOLUME_UP;
    }
    rc003_adapter_start_bound(a,100,7);sim_drain(a);
}
static void test_legacy_discovery(void) {
    rc003_adapter_t a;sim_start(&a,false);
    assert(!failures && a.state==ST_DONE && a.ready && a.voice_ready && mode_commands==1);
    assert(a.char_count==25 && a.legacy.control==63 && a.cache_valid);
    unsigned count=0;
    for(unsigned i=0;i<a.char_count;i++) {
        unsigned id=a.chars[i].report_id;
        assert(a.chars[i].subscribed==(id==1 || id==3 || (id>=5 && id<=8)));
        count+=a.chars[i].subscribed;
    }
    assert(count==6);
    sim_start(&a,true);assert(!failures && a.voice_ready);
    voice_wanted=true;
    uint8_t key[8]={0,0,0x3e};
    rbp_gatt_evt_t e={.type=RBP_GATT_EVT_NOTIFY,.handle=52,.value=key,.len=8};
    rc003_adapter_on_gatt(&a,&e);rc003_adapter_tick(&a,a.now_ms+5);sim_drain(&a);
    assert(mic_commands==1 && voice_events[RBP_VOICE_EVT_SOURCE_BEGIN]==1);
    /* Synthetic framing only; no claim to test the physical mSBC decoder. */
    uint8_t frame[60]={1,8,0xad};e.handle=70;e.value=frame;e.len=60;
    rc003_adapter_on_gatt(&a,&e);assert(voice_events[RBP_VOICE_EVT_ENCODED]==1);
    memset(key,0,sizeof key);e.handle=52;e.value=key;e.len=8;
    rc003_adapter_on_gatt(&a,&e);rc003_adapter_tick(&a,a.now_ms+5);sim_drain(&a);
    rc003_adapter_tick(&a,a.now_ms+100);
    assert(mic_commands==2 && voice_events[RBP_VOICE_EVT_END]==1);
    uint8_t cache[2048];size_t n=rc003_adapter_cache_export(&a,cache,sizeof cache);assert(n);
    rc003_adapter_detach(&a);assert(rc003_adapter_cache_import(&a,7,cache,n));
    rc003_adapter_start_bound(&a,a.now_ms+100,7);sim_drain(&a);
    assert(!failures && a.voice_ready && mode_commands==2);
    const uint16_t required_cccd[]={53,67,71,75,79};
    for(unsigned i=0;i<sizeof required_cccd/sizeof required_cccd[0];i++) {
        omit_cccd=required_cccd[i];sim_start(&a,false);assert(failures && !a.voice_ready);
    }
    omit_cccd=60;sim_start(&a,true);assert(failures && !a.voice_ready);
    omit_cccd=0;fail_cccd=71;sim_start(&a,false);assert(failures && !a.voice_ready);fail_cccd=0;
    puts("Legacy discovery/CCCD/voice framing/cache regression tests passed.");
}
