#include "hci_probe.h"
#include "esp_bt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include <string.h>

#define CAPACITY 1024u
static portMUX_TYPE guard = portMUX_INITIALIZER_UNLOCKED;
static hci_probe_entry_t entries[CAPACITY];
static uint32_t count, acl_count, continuation_count, malformed_count;
static bool frozen;
static esp_vhci_host_callback_t downstream, wrapped;
static uint16_t le16(const uint8_t *p) { return p[0] | ((uint16_t)p[1]<<8); }
static void record(uint8_t layer, uint16_t conn, uint16_t attr, const uint8_t *p, unsigned n) {
  uint32_t hash=2166136261u;
  for(unsigned i=0;i<n;i++)hash=(hash^p[i])*16777619u;
  uint32_t now = (uint32_t)esp_timer_get_time();
  portENTER_CRITICAL(&guard);
  if (!frozen) {
    hci_probe_entry_t *e = &entries[count % CAPACITY];
    *e = (hci_probe_entry_t){.ordinal=count+1,.time_us=now,
        .connection=conn,.attribute=attr,.sequence=le16(p),.part=le16(p+2),.layer=layer,.length=n,.hash=hash};
    count++;
  }
  portEXIT_CRITICAL(&guard);
}
static void observe(const uint8_t *p, unsigned n) {
  if (!n || p[0]!=2) return;
  portENTER_CRITICAL(&guard);
  if (!frozen) {
    acl_count++;
    if(n<5 || le16(p+3)!=n-5) malformed_count++;
    else if(((le16(p+1)>>12)&3)==1) continuation_count++;
  }
  portEXIT_CRITICAL(&guard);
  /* Match all complete ATT notifications, including 120-byte ATVV audio.
   * Fragmented ACL coverage remains explicitly counted, never inferred. */
  if(n<16 || le16(p+3)!=n-5 || ((le16(p+1)>>12)&3)==1 ||
     le16(p+5)!=n-9 || le16(p+7)!=4 || p[9]!=0x1b) return;
  record(0,le16(p+1)&0xfff,le16(p+10),p+12,n-12);
}
static int receive(uint8_t *p, uint16_t n) {
  observe(p,n);
  return downstream.notify_host_recv(p,n);
}
extern esp_err_t __real_esp_vhci_host_register_callback(const esp_vhci_host_callback_t *cb);
esp_err_t __wrap_esp_vhci_host_register_callback(const esp_vhci_host_callback_t *cb) {
  if (!cb || !cb->notify_host_recv)
    return __real_esp_vhci_host_register_callback(cb);
  downstream=*cb;wrapped=*cb;wrapped.notify_host_recv=receive;
  return __real_esp_vhci_host_register_callback(&wrapped);
}
void hci_probe_app(uint16_t c,uint16_t a,const uint8_t *p,unsigned n) {
  if(p && n>=4 && n<=512)record(1,c,a,p,n);
}
void hci_probe_reset(void) {
  portENTER_CRITICAL(&guard);
  count=acl_count=continuation_count=malformed_count=0;frozen=false;
  portEXIT_CRITICAL(&guard);
}
void hci_probe_freeze(void) {
  portENTER_CRITICAL(&guard);frozen=true;portEXIT_CRITICAL(&guard);
}
void hci_probe_status(uint32_t out[5]) {
  portENTER_CRITICAL(&guard);
  out[0]=count;out[1]=frozen;out[2]=acl_count;
  out[3]=continuation_count;out[4]=malformed_count;
  portEXIT_CRITICAL(&guard);
}
bool hci_probe_get(unsigned offset,hci_probe_entry_t *e) {
  portENTER_CRITICAL(&guard);
  bool ok=frozen && offset<count && offset<CAPACITY;
  if(ok)*e=entries[(count-1-offset)%CAPACITY];
  portEXIT_CRITICAL(&guard);return ok;
}
