/* Shared GATT discovery, separate measured-profile runtime. */
enum { UNICOM_MEDIA_AUDIO_TIMEOUT=1, UNICOM_MEDIA_FC_GAP, UNICOM_MEDIA_OVERRUN };
static void unicom_stop(rc003_adapter_t *a);
static void unicom_end(rc003_adapter_t *a,uint8_t reason) {
    bool active=a->unicom.announced;
    a->unicom.active=false;a->unicom.started=false;a->unicom.closing=false;
    a->unicom.part=0;a->unicom.previous_valid=false;
    a->unicom.announced=false;a->unicom.pre_head=a->unicom.pre_count=0;
    if(active){rbp_voice_evt_t ev={0};ev.type=RBP_VOICE_EVT_END;ev.u.end.reason=reason;
        rbp_server_on_voice(a->server,&ev,a->now_ms);}
}
static void unicom_bad(rc003_adapter_t *a,const char *why) {
    unicom_end(a,RBP_END_SOURCE_DATA_LOST);
    fail_initialization(a,why); /* close link; never try another vendor command */
}
static void unicom_transport_fault(rc003_adapter_t *a,const char *why) {
    unicom_end(a,RBP_END_SOURCE_DATA_LOST);
    fail_transport(a,why); /* a failed GATT transport requires link recovery */
}
static void unicom_media_fault(rc003_adapter_t *a,uint32_t cause) {
    DT(DT_HID,DT_ERROR,24,cause,a->unicom.sequence,a->unicom.part,a->now_ms-a->unicom.last_audio_ms);
    rbp_fault_record(RBP_FAULT_ADAPTER,0x94,cause,a->unicom.sequence);
    if(a->unicom.announced) {
        a->unicom.announced=false;a->unicom.started=false;
        rbp_voice_evt_t ev={0};ev.type=RBP_VOICE_EVT_END;
        ev.u.end.reason=RBP_END_SOURCE_DATA_LOST;
        rbp_server_on_voice(a->server,&ev,a->now_ms);
    }
    a->unicom.pre_head=a->unicom.pre_count=0;
    if(a->unicom.active)unicom_stop(a);
}
static bool unicom_reports(rc003_adapter_t *a) {
    if(!protocol_reports(a,2))return false;
    for(unsigned i=0;i<a->char_count;i++) {
        rc003_report_char_t *c=&a->chars[i];
        if(c->report_id==0xfb)a->unicom.fb=c->value_handle;
        if(c->report_id==0xfc)a->unicom.fc=c->value_handle;
        if(c->report_id==0xf8)a->unicom.f8=c->value_handle;
    }
    return true;
}
static void unicom_ready(rc003_adapter_t *a) {
    static const rbp_codec_t codecs[]={{RBP_CODEC_IFLYTEK_ICO,1}};
    static const rbp_audio_caps_t caps={codecs,40,1};
    /* FD-less variants have no handshake channel by design; F8/FB/FC carry
     * the stream and were validated at discovery. */
    complete_ready(a,a->unicom.hello || !a->unicom.fd);
    rbp_server_set_profile(a->server,&RBP_PROFILE_UNICOM);
    rbp_server_set_voice_caps(a->server,&caps);
    rbp_server_on_voice_state(a->server,a->voice_ready?RBP_VOICE_READY:RBP_VOICE_UNSUPPORTED,
        a->voice_ready?RBP_VI_HTT:RBP_VI_UNAVAILABLE,a->voice_ready?16000:0);
    rbp_server_on_link(a->server,RBP_LINK_READY,0,a->now_ms);
}
static void unicom_stop(rc003_adapter_t *a) {
    if(!a->unicom.active || a->unicom.closing)return;
    a->unicom.closing=true;a->unicom.close_ms=a->now_ms;
    if(!a->unicom.announced)a->unicom.pre_head=a->unicom.pre_count=0;
    a->unicom.command=2; /* queued FB=00; cannot overlap an accepted write */
}
static void unicom_start_pending(rc003_adapter_t *a) {
    if(!a->unicom.pending_voice || !a->unicom.down || !a->ready || !a->voice_ready ||
       a->unicom.active)return;
    uint32_t owner=rbp_server_voice_capture_session(a->server);
    if(!owner || owner!=a->unicom.capture_session){a->unicom.pending_voice=false;return;}
    a->unicom.pending_voice=false;
    DT(DT_HID,DT_INFO,21,a->state,a->unicom.down,1,a->now_ms);
    a->unicom.active=true;a->unicom.started=false;a->unicom.closing=false;
    a->unicom.part=0;a->unicom.previous_valid=false;a->unicom.command=1;
    a->unicom.last_audio_ms=a->unicom.capture_ms=a->now_ms;
    a->unicom.pre_head=a->unicom.pre_count=0;
    a->unicom.announced=rbp_server_voice_wanted(a->server);
    if(a->unicom.announced){rbp_voice_evt_t ev={0};ev.type=RBP_VOICE_EVT_SOURCE_BEGIN;
        rbp_server_on_voice(a->server,&ev,a->now_ms);}

}
static bool unicom_emit(rc003_adapter_t *a,const uint8_t *p,uint8_t lost) {
    if(!rbp_server_voice_wanted(a->server) ||
       rbp_server_voice_capture_session(a->server)!=a->unicom.capture_session)return false;
    if(!a->unicom.announced) {
        if(a->unicom.closing)return false;
        rbp_voice_evt_t ev={0};ev.type=RBP_VOICE_EVT_SOURCE_BEGIN;
        a->unicom.announced=true;rbp_server_on_voice(a->server,&ev,a->now_ms);
    }
    if(!a->unicom.started) {
        rbp_voice_evt_t ev={0};ev.type=RBP_VOICE_EVT_START;
        ev.u.start=(rbp_audio_format_t){RBP_CODEC_IFLYTEK_ICO,16000,40,1,0,1,NULL};
        a->unicom.started=true;rbp_server_on_voice(a->server,&ev,a->now_ms);
    }
    rbp_voice_evt_t ev={0};ev.type=RBP_VOICE_EVT_ENCODED;
    ev.u.encoded.erasures_before=lost;ev.u.encoded.data=p;ev.u.encoded.len=40;ev.u.encoded.unit_size=40;ev.u.encoded.samples=320;
    rbp_server_on_voice(a->server,&ev,a->now_ms);return true;
}
static void unicom_preroll_cancel(rc003_adapter_t *a,uint8_t reason) {
    DT(DT_HID,DT_INFO,23,reason,a->unicom.pre_count,a->now_ms-a->unicom.capture_ms,0);
    if(reason)rbp_fault_record(RBP_FAULT_ADAPTER,0x90,reason,a->unicom.pre_count);
    a->unicom.pre_count=0;unicom_stop(a);
}
static void unicom_tick(rc003_adapter_t *a) {
    if(a->state==ST_UNICOM_WAIT) {
        if(a->unicom.hello || (int32_t)(a->now_ms-a->deadline_ms)>=0)unicom_ready(a);
        return;
    }
    if(a->state!=ST_DONE || !a->voice_ready)return;
    unicom_start_pending(a);
    if(a->unicom.active) {
        if(rbp_server_voice_capture_session(a->server)!=a->unicom.capture_session ||
           (a->unicom.announced && !rbp_server_voice_wanted(a->server)))unicom_preroll_cancel(a,0);
        if(!a->unicom.announced && !a->unicom.closing && rbp_server_voice_wanted(a->server)) {
            a->unicom.announced=true;rbp_voice_evt_t ev={0};ev.type=RBP_VOICE_EVT_SOURCE_BEGIN;
            rbp_server_on_voice(a->server,&ev,a->now_ms);
        }
        if(!a->unicom.announced && !a->unicom.closing &&
           a->now_ms-a->unicom.capture_ms>=250)unicom_preroll_cancel(a,RBP_END_CONSUMER_DISABLED);
        /* Drain at most one unit per 5ms turn; never burst eight into USB's
         * small media queue. Live FC appends behind retained units. */
        if(a->unicom.pre_count && unicom_emit(a,a->unicom.preroll+40*a->unicom.pre_head,a->unicom.pre_loss[a->unicom.pre_head])) {
            a->unicom.pre_head=(a->unicom.pre_head+1)%8;a->unicom.pre_count--;
        }
    }
    if(a->unicom.closing && a->unicom.command && !a->unicom.in_flight && a->now_ms-a->unicom.close_ms>=2000) {
        unicom_transport_fault(a,"Unicom stop submission timeout");return;
    }
    if(a->unicom.in_flight) {
        if((int32_t)(a->now_ms-a->unicom.command_deadline)>=0)
            unicom_transport_fault(a,"Unicom command timeout");
        return;
    }
    if(a->unicom.active && a->unicom.closing && !a->unicom.command &&
       ((a->now_ms-a->unicom.last_audio_ms>=300 && a->now_ms-a->unicom.close_ms>=300) ||
        a->now_ms-a->unicom.close_ms>=1000)) {
        /* Physical release already ended capture. An incomplete final group
         * must not discard the verified PCM tail or become a transport fault. */
        if(a->unicom.part)rbp_fault_record(RBP_FAULT_ADAPTER,0x93,a->unicom.sequence,a->unicom.part);
        unicom_end(a,a->unicom.started?RBP_END_NORMAL:RBP_END_DEVICE_ERROR);return;
    }
    if(a->unicom.active && !a->unicom.closing) {
        if(a->now_ms-a->unicom.last_audio_ms>=1000){unicom_media_fault(a,UNICOM_MEDIA_AUDIO_TIMEOUT);return;}
        if(a->unicom.started && (int32_t)(a->now_ms-a->unicom.refresh_ms)>=0)
            a->unicom.command=1;
    }
    if(a->unicom.command) {
        uint8_t cmd=a->unicom.command,value=cmd==1?1:0;
        /* Install state before submission: simulated transports may complete inline. */
        a->unicom.in_flight=cmd;a->unicom.command=0;
        a->unicom.command_deadline=a->now_ms+ATT_RESPONSE_TIMEOUT_MS;
        int rc=a->gatt->write_value(a->gatt_user,a->unicom.fb,&value,1);
        if(rc==RBP_GATT_RETRY){a->unicom.in_flight=0;a->unicom.command=cmd;}
        else if(rc<0){a->unicom.in_flight=0;unicom_transport_fault(a,"Unicom command rejected");}
        else if(cmd==1)a->unicom.refresh_ms=a->now_ms+5000;
    }
}
static void unicom_keys(rc003_adapter_t *a,unsigned index,const uint8_t *p,unsigned n) {
    uint8_t id=a->chars[index].report_id;
    if((id==1 && (n!=8 || p[0] || p[1])) || (id==3 && n!=2)) {
        unicom_bad(a,"Unicom key layout");return;
    }
    uint64_t custom;bool handled;
    if(!variant_report(a,id,p,n,&custom,&handled)){unicom_bad(a,"variant key layout");return;}
    if(handled) {
        uint64_t bits=0;
        for(unsigned k=1;k<64;k++)if(custom&(UINT64_C(1)<<k)){unsigned slot=key_slot(a,k);if(slot<64)bits|=UINT64_C(1)<<slot;}
        a->keys_by_report[index]=bits;bits=0;
        for(unsigned k=0;k<a->char_count;k++)bits|=a->keys_by_report[k];
        if(bits!=a->pressed_bits){a->pressed_bits=bits;key_state_send(a,bits);}return;
    }
    for(unsigned k=id==1?2:0;k<(id==1?8:1);k++) {
        unsigned usage=id==1?p[k]:(p[0]|((uint16_t)p[1]<<8));
        bool known=usage==0;
        for(unsigned j=0;j<sizeof(unicom_keys_map)/sizeof(unicom_keys_map[0]);j++)
            if(unicom_keys_map[j].report==id && unicom_keys_map[j].usage==usage)known=true;
        if(!known){unicom_bad(a,"Unicom unknown key report");return;}
    }
    uint64_t bits=0;
    for(unsigned j=0;j<sizeof(unicom_keys_map)/sizeof(unicom_keys_map[0]);j++) {
        if(unicom_keys_map[j].report!=id)continue;
        bool found=false;
        if(id==1){for(unsigned k=2;k<8;k++)if(p[k]==unicom_keys_map[j].usage)found=true;}
        else found=(p[0]|((uint16_t)p[1]<<8))==unicom_keys_map[j].usage;
        if(found){unsigned slot=key_slot(a,unicom_keys_map[j].key);if(slot<64)bits|=UINT64_C(1)<<slot;}
    }
    a->keys_by_report[index]=bits;
    bits=0;for(unsigned i=0;i<a->char_count;i++)bits|=a->keys_by_report[i];
    if(bits!=a->pressed_bits){a->pressed_bits=bits;key_state_send(a,bits);}
}
static void unicom_notify(rc003_adapter_t *a,const rbp_gatt_evt_t *e) {
    if(!e->value){unicom_bad(a,"Unicom empty notification");return;}
    if(e->handle==a->unicom.fd) {
        static const uint8_t hello[]={0x10,1,0x4b,0x2a,3,2,4,0,2,3};
        if(e->len!=sizeof hello || memcmp(e->value,hello,sizeof hello)) {
            unicom_bad(a,"Unicom initialization mismatch");return;
        }
        bool first=!a->unicom.hello;a->unicom.hello=true;
        if(first && (a->state==ST_UNICOM_WAIT || a->state==ST_DONE))unicom_ready(a);
        return; /* CCCD ACK remains owned by discovery */
    }
    if(e->handle==a->unicom.f8) {
        bool valid=e->len==20 && e->value[0]==0x82 && e->value[1]==3 && e->value[2]<=1;
        for(unsigned i=3;valid && i<20;i++)if(e->value[i])valid=false;
        if(!valid){unicom_bad(a,"Unicom F8 mismatch");return;}
        bool down=e->value[2]!=0,edge=down&&!a->unicom.down;a->unicom.down=down;
#ifdef RBP_STANDALONE
        standalone_peer_voice_key(a->server,down,a->now_ms);
#endif
        DT(DT_HID,DT_INFO,20,down,a->state,rbp_server_voice_wanted(a->server),rbp_server_voice_capture_session(a->server));
        if(!down){a->unicom.pending_voice=false;unicom_stop(a);return;}
        /* Preserve a real down edge received while restoring subscriptions.
         * This includes READY before host VOICE_ENABLE; the product policy
         * distinguishes pending negotiation from explicit disable per session.
         * Release/detach cancels it; never infer a press from link wakeup. */
        if(edge && !a->unicom.active) {
            a->unicom.capture_session=rbp_server_voice_capture_session(a->server);
            a->unicom.pending_voice=a->unicom.capture_session!=0;
        }
        unicom_start_pending(a);
        return;
    }
    if(!a->ready)return;
    if(e->handle==a->unicom.fc) {
        if(!a->unicom.active)return;
        if(a->unicom.closing && !a->unicom.announced) {
            if(e->len==20)a->unicom.last_audio_ms=a->now_ms;
            return; /* Media already failed; drain timing only, never fault repeatedly. */
        }
        const uint8_t *p=e->value;
        if(e->len!=20){unicom_bad(a,"Unicom FC length");return;}
        uint16_t seq=p[0]|((uint16_t)p[1]<<8),part=p[2]|((uint16_t)p[3]<<8);
        if(part>2){unicom_bad(a,"Unicom FC part");return;}
        /* Anchor on the first observed group even if part 0 is missing.
         * Modulo sequence arithmetic: old packets never advance decoder/timers.
         * Bound one recovery to the shared ICO concealment limit. */
        if(!a->unicom.previous_valid) {
            a->unicom.previous=(uint16_t)(seq-1);a->unicom.previous_valid=true;
        }
        uint16_t delta=(uint16_t)(seq-a->unicom.previous-1);
        if(delta>=0x8000u)return;
        if(delta>UNICOM_ICO_MAX_CONCEALED_UNITS) {
            rbp_fault_record(RBP_FAULT_ADAPTER,0x91,
                (uint32_t)seq|((uint32_t)part<<16),
                (uint32_t)(uint16_t)(a->unicom.previous+1)|((uint32_t)a->unicom.part<<16));
            unicom_media_fault(a,UNICOM_MEDIA_FC_GAP);return;
        }
        if(a->unicom.part && seq!=a->unicom.sequence) {
            if((uint16_t)(seq-a->unicom.sequence)>=0x8000u)return;
            a->unicom.part=0;
        }
        if(a->unicom.part && part<a->unicom.part)return; /* duplicate */
        if(part!=a->unicom.part) {a->unicom.part=0;return;}
        if(!part)a->unicom.sequence=seq;
        memcpy(a->unicom.body+16*part,p+4,16);a->unicom.part++;
        a->unicom.last_audio_ms=a->now_ms;
        if(part!=2)return;
        p=a->unicom.body;
        /* FD-equipped units echo the group sequence plus the part-1 tail as
         * an integrity trailer. The measured FD-less XFRCB22 generation puts
         * a per-start frame counter and zeros there instead (probe trace
         * 2026-09-21), so the echo is enforced only when the handshake
         * channel exists; decode and PLC stay the content gate for the rest. */
        if(a->unicom.fd && ((p[42]|((uint16_t)p[43]<<8))!=seq || memcmp(p+44,p+28,4))) {
            a->unicom.part=0;return; /* Drop corrupt content, conceal at the next valid group. */
        }
        uint8_t lost=(uint8_t)delta;
        if(lost)rbp_fault_record(RBP_FAULT_ADAPTER,0x92,lost,seq);
        a->unicom.part=0;a->unicom.previous=seq;a->unicom.previous_valid=true;
        if(!a->unicom.announced && a->unicom.closing)return;
        if(rbp_server_voice_capture_session(a->server)!=a->unicom.capture_session) {
            unicom_preroll_cancel(a,0);return;
        }
        if(!a->unicom.pre_count && unicom_emit(a,p,lost))return;
        if(a->unicom.announced && !rbp_server_voice_wanted(a->server)){unicom_stop(a);return;}
        if(a->unicom.pre_count==8){
            if(a->unicom.announced)unicom_media_fault(a,UNICOM_MEDIA_OVERRUN);
            else unicom_preroll_cancel(a,RBP_END_BUFFER_OVERRUN);
            return;
        }
        a->unicom.pre_loss[(a->unicom.pre_head+a->unicom.pre_count)%8]=lost;
        memcpy(a->unicom.preroll+40*((a->unicom.pre_head+a->unicom.pre_count)%8),p,40);
        a->unicom.pre_count++;
        DT(DT_HID,DT_DETAIL,22,a->unicom.pre_count,seq,a->unicom.announced,0);
        return;
    }
    for(unsigned i=0;i<a->char_count;i++)if(a->chars[i].value_handle==e->handle) {
        unsigned id=a->chars[i].report_id;
        if(id==1 || id==3)unicom_keys(a,i,e->value,e->len);
        else if((id==0xf9 && e->len!=1) || (id==4 && e->len!=20))unicom_bad(a,"Unicom extension length");
        return;
    }
    if(e->handle==a->bat_value_handle && e->len==1 && e->value[0]<=100)
        rbp_server_on_battery(a->server,e->value[0],0);
}
