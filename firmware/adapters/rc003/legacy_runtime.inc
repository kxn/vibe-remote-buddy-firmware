/* Xiaomi HID/mSBC: measured VID2717/PID32BA. Addresses are discovered by Report Reference. */
static void legacy_stop(rc003_adapter_t *a) {
    if(!a->legacy.active || a->legacy.closing)return;
    a->legacy.closing=true;a->legacy.close_ms=a->now_ms;a->legacy.command=2;a->legacy.command_ms=a->now_ms;
}
static void legacy_end(rc003_adapter_t *a,uint8_t reason) {
    bool active=a->legacy.active;
    a->legacy.active=a->legacy.started=a->legacy.closing=false;
    a->legacy.command=0;
    if(active){rbp_voice_evt_t ev={0};ev.type=RBP_VOICE_EVT_END;ev.u.end.reason=reason;rbp_server_on_voice(a->server,&ev,a->now_ms);}
}
static void legacy_key(rc003_adapter_t *a,bool down) {
    if(a->legacy.down==down)return;
    a->legacy.down=down;
#ifdef RBP_STANDALONE
    standalone_peer_voice_key(a->server,down,a->now_ms);
#endif
    if(!down){legacy_stop(a);return;}
    if(!a->voice_ready || a->legacy.active || !rbp_server_voice_wanted(a->server))return;
    a->legacy.active=true;a->legacy.started=false;a->legacy.sequence_valid=false;
    a->legacy.closing=false;a->legacy.command=1;a->legacy.last_audio=a->now_ms;
    a->legacy.command_ms=a->now_ms;a->legacy.end_reason=RBP_END_NORMAL;
    a->legacy.capture_session=rbp_server_voice_capture_session(a->server);
    rbp_voice_evt_t ev={0};ev.type=RBP_VOICE_EVT_SOURCE_BEGIN;rbp_server_on_voice(a->server,&ev,a->now_ms);
}
static bool legacy_report_layout(rc003_adapter_t *a) {
    if(!protocol_reports(a,3))return false;
    for(unsigned i=0;i<a->char_count;i++)if(a->chars[i].report_id==4)a->legacy.control=a->chars[i].value_handle;
    return true;
}
static bool legacy_reports(rc003_adapter_t *a) {
    return legacy_report_layout(a) && a->negotiated_mtu>=protocol_min_mtu(3);
}
static void legacy_ready(rc003_adapter_t *a) {
    if(!legacy_reports(a)){fail_initialization(a,"Legacy HID voice reports/MTU");return;}
    static const rbp_codec_t codecs[]={{RBP_CODEC_MSBC,1}};
    static const rbp_audio_caps_t caps={codecs,57,1};
    complete_ready(a,true);a->voice_key_verified=true;
    rbp_server_set_profile(a->server,a->variant.profile?a->variant.profile:&RBP_PROFILE_RC003_VOICE);
    rbp_server_set_voice_caps(a->server,&caps);
    rbp_server_on_voice_state(a->server,RBP_VOICE_READY,RBP_VI_HTT,16000);
    rbp_server_on_link(a->server,RBP_LINK_READY,0,a->now_ms);
}
static void legacy_tick(rc003_adapter_t *a) {
    if(a->state!=ST_DONE)return;
    if(a->legacy.active && (!rbp_server_voice_wanted(a->server) ||
        rbp_server_voice_capture_session(a->server)!=a->legacy.capture_session))legacy_stop(a);
    if(a->legacy.in_flight) {
        if((int32_t)(a->now_ms-a->legacy.deadline)>=0){legacy_end(a,RBP_END_DEVICE_ERROR);fail_transport(a,"Legacy write timeout");}
        return;
    }
    if(a->legacy.active && !a->legacy.closing && a->now_ms-a->legacy.last_audio>=2000){a->legacy.end_reason=RBP_END_DEVICE_ERROR;rbp_fault_record(RBP_FAULT_ADAPTER,0xa2,2000,0);legacy_stop(a);}
    if(a->legacy.command) {
        if(a->now_ms-a->legacy.command_ms>=ATT_RESPONSE_TIMEOUT_MS){legacy_end(a,RBP_END_DEVICE_ERROR);fail_transport(a,"Legacy submit timeout");return;}
        uint8_t cmd=a->legacy.command;
        if(cmd==1 && a->legacy.closing)cmd=2;
        uint8_t value[20]={0};value[3]=0x80;value[4]=cmd==1;
        a->legacy.command=0;a->legacy.in_flight=cmd;a->legacy.deadline=a->now_ms+ATT_RESPONSE_TIMEOUT_MS;
        int rc=a->gatt->write_value(a->gatt_user,a->legacy.control,value,20);
        if(rc==RBP_GATT_RETRY){a->legacy.in_flight=0;a->legacy.command=cmd;}
        else if(rc){a->legacy.in_flight=0;legacy_end(a,RBP_END_DEVICE_ERROR);fail_transport(a,"Legacy write rejected");}
        return;
    }
    if(a->legacy.active && a->legacy.closing &&
       ((a->now_ms-a->legacy.last_audio>=60 && a->now_ms-a->legacy.close_ms>=60) || a->now_ms-a->legacy.close_ms>=500))
        legacy_end(a,a->legacy.end_reason);
}
static bool legacy_notify(rc003_adapter_t *a,const rbp_gatt_evt_t *e) {
    for(unsigned i=0;i<a->char_count;i++) {
        rc003_report_char_t *c=&a->chars[i];if(c->value_handle!=e->handle || !c->subscribed)continue;
        if(c->report_id==5) {
            if(e->len==20 && e->value[0]==0 && e->value[1]==0) {
                /* This independent release may arrive without HID key-up.
                 * Clear cached Voice as well, or another report can reopen it. */
                for(unsigned k=0;k<a->char_count;k++)
                    a->keys_by_report[k]&=~(UINT64_C(1)<<RBP_KEY_VOICE);
                recompute_keys(a);
            }
            return true; /* Status down duplicates F5; only F5 starts capture. */
        }
        if(c->report_id<6 || c->report_id>8)return false;
        if(!a->legacy.active || !rbp_server_voice_wanted(a->server) ||
           rbp_server_voice_capture_session(a->server)!=a->legacy.capture_session)return true;
        const uint8_t *p=e->value;int seq=-1;
        if(e->len==60 && p && p[0]==1 && p[2]==0xad && !p[3] && !p[4]) {
            const uint8_t h2[]={8,0x38,0xc8,0xf8};for(int k=0;k<4;k++)if(p[1]==h2[k])seq=k;
        }
        if(seq<0){rbp_fault_record(RBP_FAULT_ADAPTER,0xa0,e->len,e->handle);return true;}
        if(a->legacy.sequence_valid && seq==a->legacy.sequence){rbp_fault_record(RBP_FAULT_ADAPTER,0xa3,seq,e->handle);return true;}
        uint8_t lost=a->legacy.sequence_valid?(seq-a->legacy.sequence-1)&3:0;
        if(lost)rbp_fault_record(RBP_FAULT_ADAPTER,0xa1,lost,e->handle);
        a->legacy.sequence=seq;a->legacy.sequence_valid=true;a->legacy.last_audio=a->now_ms;
        if(!a->legacy.started) {
            rbp_voice_evt_t ev={0};ev.type=RBP_VOICE_EVT_START;
            ev.u.start=(rbp_audio_format_t){RBP_CODEC_MSBC,16000,57,1,0,1,NULL};
            a->legacy.started=true;rbp_server_on_voice(a->server,&ev,a->now_ms);
        }
        rbp_voice_evt_t ev={0};ev.type=RBP_VOICE_EVT_ENCODED;
        ev.u.encoded.data=p+2;ev.u.encoded.len=ev.u.encoded.unit_size=57;ev.u.encoded.samples=120;ev.u.encoded.erasures_before=lost;
        rbp_server_on_voice(a->server,&ev,a->now_ms);return true;
    }
    return false;
}
