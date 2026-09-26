/* Composite CDC, HID and UAC device. All TinyUSB application calls share
 * the USB task. S3_USB_QUALIFY substitutes silence and release-only HID. */
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "soc/rtc_cntl_reg.h"
#include "esp_private/usb_phy.h"
#include "tinyusb.h"
#include "device/dcd.h"
#include "host_os.h"
#ifndef S3_USB_QUALIFY
#include "s3_runtime.h"
#include "standalone.h"
#endif

static portMUX_TYPE host_guard = portMUX_INITIALIZER_UNLOCKED;
static buddy_host_probe_t host_probe;
static _Atomic unsigned host_snapshot;
buddy_host_os_t s3_host_os(void) { return atomic_load(&host_snapshot); }
void __real_dcd_event_handler(dcd_event_t const *event, bool in_isr);
void __wrap_dcd_event_handler(dcd_event_t const *event, bool in_isr) {
    if (event->event_id == DCD_EVENT_BUS_RESET || event->event_id == DCD_EVENT_UNPLUGGED || event->event_id == DCD_EVENT_SETUP_RECEIVED) {
        if(in_isr)portENTER_CRITICAL_ISR(&host_guard);else portENTER_CRITICAL(&host_guard);
        if(event->event_id != DCD_EVENT_SETUP_RECEIVED) { buddy_host_reset(&host_probe); atomic_store(&host_snapshot,BUDDY_HOST_UNKNOWN); }
        else { tusb_control_request_t const *r=&event->setup_received;
            buddy_host_setup(&host_probe,r->bmRequestType,r->bRequest,r->wValue,r->wLength,(uint32_t)(esp_timer_get_time()/1000)); }
        if(in_isr)portEXIT_CRITICAL_ISR(&host_guard);else portEXIT_CRITICAL(&host_guard);
    }
    __real_dcd_event_handler(event,in_isr);
}
static _Atomic uint32_t submitted,completed;
static uint32_t audio_bytes;
#ifdef S3_USB_QUALIFY
static uint32_t audio_requests[24],request_count;
#endif
static uint32_t hid_sent_ms,hid_max_delay;
static _Atomic uint32_t hid_done_ms;
static _Atomic bool in_flight,usb_ready_snapshot;
bool s3_usb_configured(void) { return atomic_load(&usb_ready_snapshot); }
static uint32_t reboot_at;
static uint8_t sent_report[8],acked_report[8],sent_id;
static uint16_t sent_consumer,acked_consumer;
static bool recording;
static uint8_t mute[2];
static int16_t volume[2];
static const uint8_t reports[]={/* 8-byte keyboard: modifier, Fn bit + padding, six usages. */
 0x05,0x01,0x09,0x06,0xa1,0x01,0x85,0x01,
 0x05,0x07,0x19,0xe0,0x29,0xe7,0x15,0,0x25,1,0x75,1,0x95,8,0x81,2,
 0x05,0xff,0x09,0x03,0x95,1,0x81,2,0x75,7,0x95,1,0x81,3,
 0x05,0x07,0x19,0,0x29,0xdf,0x15,0,0x26,0xdf,0,0x75,8,0x95,6,0x81,0,
 0x05,0x08,0x19,1,0x29,5,0x15,0,0x25,1,0x75,1,0x95,5,0x91,2,
 0x75,3,0x95,1,0x91,3,0xc0,0x05,0x0c,0x09,0x01,0xa1,0x01,0x85,0x02,
 0x15,0x00,0x25,0x01,0x75,0x01,0x95,0x08,
 0x09,0xe9,0x09,0xea,0x09,0xe2,0x09,0xcd,0x09,0xb5,0x09,0xb6,0x09,0xb7,0x0a,0x23,0x02,0x81,0x02,
 0x75,0x08,0x95,0x01,0x81,0x03,0xc0};
enum {CDC=0,CDC_DATA,HID,AUDIO,AUDIO_STREAM,ITFS};
#define CONFIG_LEN (TUD_CONFIG_DESC_LEN+TUD_CDC_DESC_LEN+TUD_HID_DESC_LEN+TUD_AUDIO20_MIC_ONE_CH_DESC_LEN)
static const uint8_t config[]={
    TUD_CONFIG_DESCRIPTOR(1,ITFS,0,CONFIG_LEN,0,100),
    TUD_CDC_DESCRIPTOR(CDC,4,0x81,8,0x02,0x82,64),
    TUD_HID_DESCRIPTOR(HID,5,HID_ITF_PROTOCOL_NONE,sizeof reports,0x83,16,1),
    TUD_AUDIO20_MIC_ONE_CH_DESCRIPTOR(AUDIO,6,2,16,0x84,34)
};
static const tusb_desc_device_t device={
    .bLength=sizeof(tusb_desc_device_t),.bDescriptorType=TUSB_DESC_DEVICE,.bcdUSB=0x0200,
    .bDeviceClass=TUSB_CLASS_MISC,.bDeviceSubClass=MISC_SUBCLASS_COMMON,.bDeviceProtocol=MISC_PROTOCOL_IAD,
    .bMaxPacketSize0=64,.idVendor=0xcafe,.idProduct=0x4016,.bcdDevice=0x0400,
    .iManufacturer=1,.iProduct=2,.iSerialNumber=3,.bNumConfigurations=1
};
static char serial[17];
static const char *strings[]={"\x09\x04","Remote Bridge","Remote USB S3",serial,"Management","Keyboard","Remote microphone"};
const uint8_t *tud_hid_descriptor_report_cb(uint8_t instance) {(void)instance;return reports;}
uint16_t tud_hid_get_report_cb(uint8_t instance,uint8_t id,hid_report_type_t type,uint8_t *buf,uint16_t len) {
    (void)instance;if(type!=HID_REPORT_TYPE_INPUT)return 0;
    if(id==1){if(len>8)len=8;memcpy(buf,acked_report,len);return len;}
    if(id==2){if(len>2)len=2;memcpy(buf,&acked_consumer,len);return len;}
    return 0;
}
void tud_hid_set_report_cb(uint8_t instance,uint8_t id,hid_report_type_t type,const uint8_t *buf,uint16_t len) {
    (void)instance;(void)id;(void)type;(void)buf;(void)len;
}
void tud_hid_report_complete_cb(uint8_t instance,const uint8_t *report,uint16_t len) {
    (void)instance;(void)report;(void)len;completed++;in_flight=false;
    hid_done_ms=esp_timer_get_time()/1000;
    if(hid_done_ms-hid_sent_ms>hid_max_delay)hid_max_delay=hid_done_ms-hid_sent_ms;
    if(sent_id==1)memcpy(acked_report,sent_report,8);else if(sent_id==2)acked_consumer=sent_consumer;
}
bool tud_audio_set_itf_cb(uint8_t port,const tusb_control_request_t *r) {
    (void)port;if(tu_u16_low(r->wIndex)==AUDIO_STREAM)recording=r->wValue!=0;return true;
}
bool tud_audio_set_itf_close_ep_cb(uint8_t port,const tusb_control_request_t *r) {
    (void)port;(void)r;recording=false;return true;
}
bool tud_audio_get_req_entity_cb(uint8_t port,const tusb_control_request_t *r) {
#ifdef S3_USB_QUALIFY
    if(request_count<24)audio_requests[request_count++]=(uint32_t)r->wIndex<<16|r->wValue|r->bRequest;
#endif
    uint8_t entity=tu_u16_high(r->wIndex),sel=tu_u16_high(r->wValue),ch=tu_u16_low(r->wValue);
    if(tu_u16_low(r->wIndex)!=AUDIO)return false;
    if(entity==4&&sel==AUDIO20_CS_CTRL_SAM_FREQ) {
        if(r->bRequest==AUDIO20_CS_REQ_CUR) {uint32_t rate=16000;return tud_audio_buffer_and_schedule_control_xfer(port,r,&rate,sizeof rate);}
        if(r->bRequest==AUDIO20_CS_REQ_RANGE) {
            const uint8_t range[]={1,0,0x80,0x3e,0,0,0x80,0x3e,0,0,0,0,0,0};
            return tud_audio_buffer_and_schedule_control_xfer(port,r,(void*)range,sizeof range);
        }
    }
    if(entity==4&&sel==AUDIO20_CS_CTRL_CLK_VALID&&r->bRequest==AUDIO20_CS_REQ_CUR) {
        uint8_t valid=1;return tud_audio_buffer_and_schedule_control_xfer(port,r,&valid,1);
    }
    if(entity==1&&sel==AUDIO20_TE_CTRL_CONNECTOR&&r->bRequest==AUDIO20_CS_REQ_CUR) {
        const uint8_t cluster[]={1,0,0,0,0,0};return tud_audio_buffer_and_schedule_control_xfer(port,r,(void*)cluster,sizeof cluster);
    }
    if(entity==2&&ch<2) {
        if(sel==AUDIO20_FU_CTRL_MUTE&&r->bRequest==AUDIO20_CS_REQ_CUR)
            return tud_audio_buffer_and_schedule_control_xfer(port,r,&mute[ch],1);
        if(sel==AUDIO20_FU_CTRL_VOLUME) {
            if(r->bRequest==AUDIO20_CS_REQ_CUR)return tud_audio_buffer_and_schedule_control_xfer(port,r,&volume[ch],2);
            if(r->bRequest==AUDIO20_CS_REQ_RANGE) {
                const uint8_t range[]={1,0,0,0,0,0,0,1}; /* fixed 0 dB, 1 dB resolution */
                return tud_audio_buffer_and_schedule_control_xfer(port,r,(void*)range,sizeof range);
            }
        }
    }
    return false;
}
bool tud_audio_set_req_entity_cb(uint8_t port,const tusb_control_request_t *r,uint8_t *buf) {
    (void)port;uint8_t ch=tu_u16_low(r->wValue),sel=tu_u16_high(r->wValue);
    if(tu_u16_low(r->wIndex)!=AUDIO||tu_u16_high(r->wIndex)!=2||ch>=2||r->bRequest!=AUDIO20_CS_REQ_CUR)return false;
    if(sel==AUDIO20_FU_CTRL_MUTE&&r->wLength==1){mute[ch]=buf[0]!=0;return true;}
    if(sel==AUDIO20_FU_CTRL_VOLUME&&r->wLength==2&&buf[0]==0&&buf[1]==0){volume[ch]=0;return true;}
    return false;
}
void tud_cdc_line_coding_cb(uint8_t instance,const cdc_line_coding_t *coding) {
    (void)instance;if(coding->bit_rate==1200)reboot_at=(uint32_t)(esp_timer_get_time()/1000)+100;
}
static void enter_bootloader(void) {
    tud_disconnect();vTaskDelay(pdMS_TO_TICKS(100));
    ESP_ERROR_CHECK(tinyusb_driver_uninstall());
    /* RTC USB mux survives software reset: restore ROM Serial/JTAG PHY. */
    usb_phy_handle_t rom_phy;
    const usb_phy_config_t rom_cfg={.controller=USB_PHY_CTRL_SERIAL_JTAG,.target=USB_PHY_TARGET_INT};
    ESP_ERROR_CHECK(usb_new_phy(&rom_cfg,&rom_phy));
    REG_WRITE(RTC_CNTL_OPTION1_REG,RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    esp_restart();
}
static void usb_task(void *arg) {
    (void)arg;
#ifdef S3_USB_QUALIFY
    uint32_t last=0;
#else
    uint32_t reconnect_at=0,epoch=0;bool was_ready=false,was_cdc=false;
#endif
    for(;;) {
        tud_task_ext(1,false);
        uint32_t now=(uint32_t)(esp_timer_get_time()/1000);
        if(reboot_at&&(int32_t)(now-reboot_at)>=0)enter_bootloader();
        portENTER_CRITICAL(&host_guard);
        atomic_store(&host_snapshot,buddy_host_classify(&host_probe,now,tud_mounted()));
        portEXIT_CRITICAL(&host_guard);
        usb_ready_snapshot=tud_mounted()&&!tud_suspended();
#ifndef S3_USB_QUALIFY
        bool ready=tud_mounted()&&!tud_suspended();
        s3_usb_state(ready,recording&&ready);
        if(ready&&!was_ready){memset(acked_report,0xff,8);acked_consumer=0xffff;in_flight=false;}
        was_ready=ready;
        uint32_t current_epoch=s3_audio_epoch();
        if(epoch!=current_epoch){epoch=current_epoch;tud_audio_clear_ep_in_ff();}
        if(in_flight&&now-hid_sent_ms>500) {
            s3_hid_fault();s3_usb_state(false,false);tud_disconnect();
            recording=false;in_flight=false;reconnect_at=now+100;
        }
        if(reconnect_at&&(int32_t)(now-reconnect_at)>=0){reconnect_at=0;tud_connect();}
        if(!reconnect_at&&ready&&!in_flight&&tud_hid_ready()) {
            uint8_t desired[8];s3_hid_desired(desired);uint16_t media=s3_consumer_desired();
            if(memcmp(desired,acked_report,8)) {
                memcpy(sent_report,desired,8);sent_id=1;
                if(tud_hid_report(1,sent_report,8)){in_flight=true;submitted++;hid_sent_ms=now;}
            } else if(media!=acked_consumer) {
                sent_consumer=media;sent_id=2;
                if(tud_hid_report(2,&sent_consumer,2)){in_flight=true;submitted++;hid_sent_ms=now;}
            }
        }
        bool cdc=tud_cdc_connected();s3_cdc_state(cdc);
        if(was_cdc&&!cdc)tud_cdc_write_clear();
        was_cdc=cdc;
        if(cdc) {
            uint8_t buf[512];unsigned room=tud_cdc_write_available();if(room>sizeof buf)room=sizeof buf;
            size_t n=s3_cdc_output(buf,room);if(n){tud_cdc_write(buf,n);tud_cdc_write_flush();}
            if(tud_cdc_available()){n=tud_cdc_read(buf,sizeof buf);if(s3_cdc_receive(buf,n)!=n)s3_hid_fault();}
        }
#endif
        if(recording) {
            /* TinyUSB audio FIFO permits overwrite. Bound writes explicitly;
             * only dequeue real PCM when the USB FIFO has room. */
            unsigned fill=tu_fifo_count(tud_audio_get_ep_in_ff());
            if(fill<256) {
                unsigned samples=(256-fill)/2;int16_t data[128]={0};
#ifndef S3_USB_QUALIFY
                s3_pcm_read(data,samples);
                if(mute[0]||mute[1])memset(data,0,samples*2);
#endif
                audio_bytes+=tud_audio_write(data,samples*2);
            }
        }
#ifdef S3_USB_QUALIFY
        if(now-last>=1000) {
            last=now;
            if(tud_hid_ready()&&tud_hid_keyboard_report(1,0,NULL)){submitted++;hid_sent_ms=now;sent_id=1;memset(sent_report,0,8);}
            if(tud_cdc_connected()) {
                char line[256];int n=snprintf(line,sizeof line,"{\"firmware\":\"s3-usb-qualification-2\",\"ms\":%lu,\"hid_sent\":%lu,\"hid_done\":%lu,\"audio_open\":%d,\"audio_bytes\":%lu,\"psram\":%u,\"heap\":%u,\"hid_max_ms\":%lu}\r\n",
                    (unsigned long)now,(unsigned long)submitted,(unsigned long)completed,recording,(unsigned long)audio_bytes,
                    (unsigned)esp_psram_get_size(),(unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),(unsigned long)hid_max_delay);
                tud_cdc_write(line,n);tud_cdc_write_flush();
                for(unsigned i=0;i<request_count;i++) {
                    n=snprintf(line,sizeof line,"audio_request[%u]=%08lx\r\n",i,(unsigned long)audio_requests[i]);
                    tud_cdc_write(line,n);
                }
                tud_cdc_write_flush();
            }
        }
        static char command[32];static unsigned command_len;
        while(tud_cdc_available()) {
            char c=(char)tud_cdc_read_char();
            if(c=='\n') {
                command[command_len]=0;
                if(!strcmp(command,"bootloader")) {
                    enter_bootloader();
                }
                command_len=0;
            } else if(c!='\r') {
                if(command_len<sizeof command-1)command[command_len++]=c;
                else command_len=0;
            }
        }
#endif
        vTaskDelay(1);
    }
}
#ifndef S3_USB_QUALIFY
void standalone_usb_stats(uint8_t out[STANDALONE_USB_STATS_BYTES]) {
    /* Atomic diagnostic snapshots; never call the USB stack on BLE task. */
    uint32_t words[]={1,submitted,submitted,completed,0,0,0,0,hid_done_ms,
        (usb_ready_snapshot?1u:0u)|(in_flight?4u:0u),0,0,0,0,0,0};
    for(unsigned i=0;i<16;i++)for(unsigned j=0;j<4;j++)out[4*i+j]=words[i]>>(8*j);
}
#endif
void usb_device_start(void) {
    uint8_t mac[6];ESP_ERROR_CHECK(esp_read_mac(mac,ESP_MAC_WIFI_STA));
    snprintf(serial,sizeof serial,"%02X%02X%02X%02X%02X%02X",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    tinyusb_config_t cfg={.device_descriptor=&device,.string_descriptor=strings,.string_descriptor_count=7,.configuration_descriptor=config};
    ESP_ERROR_CHECK(tinyusb_driver_install(&cfg));
    configASSERT(xTaskCreatePinnedToCore(usb_task,"usb",6144,NULL,5,NULL,1)==pdPASS);
}
