#include "buddy_update.h"
#include "buddy_management.h"
#include "rbp/frame.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_flash.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_secure_boot.h"
#include "esp_flash_encrypt.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "mbedtls/sha256.h"
#include <string.h>
#include <stdlib.h>
/* All update operations run in the BLE host task after links have quiesced. */
enum {IDLE,QUIESCING,COPYING,RECEIVING,STAGED,RESTARTING,FAILED};
static unsigned state,bank;
static uint32_t size,offset,copied,last,restart_at,flash_bytes;
static esp_err_t error;
static bool handle_open,confirmed,pending;
static bool storage_ready;
static esp_ota_handle_t handle;
static const esp_partition_t *running,*destination,*data_src,*data_dst;
static mbedtls_sha256_context sha;
static uint8_t expected_hash[32];
static char version[32];
static const char *data_label;
extern bool s3_usb_configured(void);
esp_err_t __wrap_nvs_open(const char *name,nvs_open_mode_t mode,nvs_handle_t *out) {
 return nvs_open_from_partition(data_label?data_label:"data0",name,mode,out);
}
static int nib(char c){return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:-1;}
static bool unhex(const char *s,uint8_t *p,size_t n){if(!s||strlen(s)!=2*n)return false;for(size_t i=0;i<n;i++){int a=nib(s[2*i]),b=nib(s[2*i+1]);if(a<0||b<0)return false;p[i]=(a<<4)|b;}return true;}
static void fail(esp_err_t rc){if(handle_open)esp_ota_abort(handle);handle_open=false;mbedtls_sha256_free(&sha);error=rc;state=FAILED;}
bool buddy_update_busy(void){return state>=QUIESCING&&state<=RESTARTING;}
bool buddy_update_confirmed(void){return confirmed&&storage_ready;}
bool buddy_update_storage_ready(void){
 nvs_handle_t meta;if(nvs_open_from_partition(data_label,"buddy-meta",NVS_READWRITE,&meta)!=ESP_OK)return false;
 esp_err_t rc=nvs_set_u32(meta,"schema",BUDDY_DATA_SCHEMA);if(!rc)rc=nvs_commit(meta);nvs_close(meta);
 storage_ready=rc==ESP_OK;return storage_ready;
}
void buddy_update_init(void) {
 storage_ready=false;
 running=esp_ota_get_running_partition();ESP_ERROR_CHECK(running?ESP_OK:ESP_FAIL);
 ESP_ERROR_CHECK((running->subtype==ESP_PARTITION_SUBTYPE_APP_OTA_0||running->subtype==ESP_PARTITION_SUBTYPE_APP_OTA_1)?ESP_OK:ESP_ERR_NOT_SUPPORTED);
 bank=running->subtype-ESP_PARTITION_SUBTYPE_APP_OTA_0;data_label=bank?"data1":"data0";
 ESP_ERROR_CHECK(nvs_flash_init_partition(data_label));
 nvs_handle_t meta;ESP_ERROR_CHECK(nvs_open_from_partition(data_label,"buddy-meta",NVS_READWRITE,&meta));
 uint32_t schema=0;esp_err_t rc=nvs_get_u32(meta,"schema",&schema);
 if(rc==ESP_ERR_NVS_NOT_FOUND){ESP_ERROR_CHECK(nvs_set_u32(meta,"schema",BUDDY_DATA_SCHEMA));ESP_ERROR_CHECK(nvs_commit(meta));}
 else {ESP_ERROR_CHECK(rc);ESP_ERROR_CHECK(schema>=1&&schema<=BUDDY_DATA_SCHEMA?ESP_OK:ESP_ERR_INVALID_VERSION);}
 nvs_close(meta);
 ESP_ERROR_CHECK(esp_flash_get_size(NULL,&flash_bytes));
 esp_ota_img_states_t image_state;
 pending=esp_ota_get_state_partition(running,&image_state)==ESP_OK&&image_state==ESP_OTA_IMG_PENDING_VERIFY;
 confirmed=!pending;mbedtls_sha256_init(&sha);
}
void buddy_update_info(cJSON *j) {
 cJSON_AddNumberToObject(j,"update_api",1);cJSON_AddStringToObject(j,"target",BUDDY_TARGET);
 cJSON_AddNumberToObject(j,"schema",BUDDY_DATA_SCHEMA);cJSON_AddNumberToObject(j,"bank",bank);
 cJSON_AddBoolToObject(j,"confirmed",confirmed);cJSON_AddNumberToObject(j,"flash_bytes",flash_bytes);
 cJSON_AddNumberToObject(j,"psram_bytes",esp_psram_get_size());
}
void buddy_update_tick(uint32_t now,bool managed,bool ble_ready,bool quiet) {
 if(pending&&!confirmed) {
  if(now>5000&&storage_ready&&ble_ready&&s3_usb_configured()) {
   esp_err_t rc=esp_ota_mark_app_valid_cancel_rollback();if(rc==ESP_OK){confirmed=true;pending=false;}else fail(rc);
  }
  if(now>20000&&!confirmed){esp_ota_mark_app_invalid_rollback_and_reboot();esp_restart();}
 }
 if(state==RESTARTING){if((int32_t)(now-restart_at)>=0)esp_restart();return;}
 /* The pump samples time before parsing commands. A command may stamp last
  * one tick later; unsigned subtraction would look like a 49-day timeout. */
 if(buddy_update_busy()&&(!managed||(int32_t)(now-last)>15000)){fail(ESP_ERR_TIMEOUT);return;}
 if(state==QUIESCING&&quiet){state=COPYING;copied=0;}
 if(state==COPYING) {
  uint8_t buffer[4096];
  esp_err_t rc=esp_partition_read(data_src,copied,buffer,sizeof buffer);
  if(!rc)rc=esp_partition_erase_range(data_dst,copied,sizeof buffer);
  if(!rc)rc=esp_partition_write(data_dst,copied,buffer,sizeof buffer);
  if(rc){fail(rc);return;}copied+=sizeof buffer;
  if(copied==data_src->size) {
   rc=esp_ota_begin(destination,OTA_WITH_SEQUENTIAL_WRITES,&handle);
   if(rc){fail(rc);return;}handle_open=true;
   mbedtls_sha256_init(&sha);rc=mbedtls_sha256_starts(&sha,0);
   if(rc){fail(rc);return;}state=RECEIVING;
  }
 }
}
uint16_t buddy_update_command(uint16_t op,const cJSON *q,cJSON *j,uint32_t now) {
 if(op==BUDDY_UPDATE_STATUS){buddy_update_info(j);cJSON_AddNumberToObject(j,"state",state);cJSON_AddNumberToObject(j,"offset",offset);cJSON_AddNumberToObject(j,"size",size);cJSON_AddNumberToObject(j,"error",error);cJSON_AddNumberToObject(j,"copied",copied);return 0;}
 if(op==BUDDY_UPDATE_ABORT){if(state==RESTARTING)return RBP_STATUS_BUSY;if(handle_open)esp_ota_abort(handle);handle_open=false;mbedtls_sha256_free(&sha);state=IDLE;error=0;return 0;}
 if(op==BUDDY_UPDATE_BEGIN) {
  if(buddy_update_busy()||!confirmed)return RBP_STATUS_BUSY;
  uint32_t n,min,max;uint8_t hash[32];
  const char *target=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(q,"target"));
  const char *v=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(q,"version"));
  const char *h=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(q,"sha256"));
  if(!target||strcmp(target,BUDDY_TARGET)||!v||!strlen(v)||strlen(v)>=sizeof version||!unhex(h,hash,32)||
     !buddy_u32(q,"size",&n)||!buddy_u32(q,"data_min",&min)||!buddy_u32(q,"data_max",&max)||
     min>BUDDY_DATA_SCHEMA||max<BUDDY_DATA_SCHEMA)return RBP_STATUS_INVALID_ARGUMENT;
  if((flash_bytes < (
#ifdef CONFIG_ESPTOOLPY_FLASHSIZE_4MB
4u
#else
8u
#endif
)*1024u*1024u)||esp_psram_get_size()<BUDDY_PSRAM_MIN||esp_secure_boot_enabled()||esp_flash_encryption_enabled())return RBP_STATUS_UNSUPPORTED;
  destination=esp_ota_get_next_update_partition(NULL);
  data_src=esp_partition_find_first(ESP_PARTITION_TYPE_DATA,ESP_PARTITION_SUBTYPE_DATA_NVS,data_label);
  data_dst=esp_partition_find_first(ESP_PARTITION_TYPE_DATA,ESP_PARTITION_SUBTYPE_DATA_NVS,bank?"data0":"data1");
  if(!destination||destination->address==running->address||!data_src||!data_dst||data_src->size!=data_dst->size||n<288||n>destination->size)return RBP_STATUS_INVALID_ARGUMENT;
  size=n;offset=0;copied=0;last=now;error=0;strcpy(version,v);memcpy(expected_hash,hash,32);state=QUIESCING;
  cJSON_AddNumberToObject(j,"chunk",192);return 0;
 }
 if(op==BUDDY_UPDATE_DATA) {
  if(state!=RECEIVING)return RBP_STATUS_BAD_STATE;
  uint32_t pos;const char *hex=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(q,"hex"));
  if(!buddy_u32(q,"offset",&pos)||!hex)return RBP_STATUS_INVALID_ARGUMENT;
  size_t n=strlen(hex)/2;uint8_t block[192],old[192];
  if(!n||n>sizeof block||!unhex(hex,block,n)||pos>size||n>size-pos)return RBP_STATUS_INVALID_ARGUMENT;
  if(pos<offset) {
   if(n>offset-pos||esp_partition_read(destination,pos,old,n)||memcmp(old,block,n))return RBP_STATUS_BAD_STATE;
  } else {
   if(pos!=offset)return RBP_STATUS_BAD_STATE;
   esp_err_t rc=esp_ota_write(handle,block,n);if(rc){fail(rc);cJSON_AddNumberToObject(j,"error",rc);return RBP_STATUS_DEVICE_ERROR;}
   rc=mbedtls_sha256_update(&sha,block,n);
   if(rc){fail(rc);cJSON_AddNumberToObject(j,"error",rc);return RBP_STATUS_DEVICE_ERROR;}offset+=n;
  }
  last=now;cJSON_AddNumberToObject(j,"offset",offset);return 0;
 }
 if(op==BUDDY_UPDATE_FINISH) {
  if(state!=RECEIVING||offset!=size)return RBP_STATUS_BAD_STATE;
  uint8_t hash[32];esp_err_t rc=mbedtls_sha256_finish(&sha,hash);mbedtls_sha256_free(&sha);
  if(!rc&&memcmp(hash,expected_hash,32))rc=ESP_ERR_INVALID_CRC;
  if(rc){fail(rc);cJSON_AddNumberToObject(j,"error",rc);return RBP_STATUS_DEVICE_ERROR;}
  rc=esp_ota_end(handle);handle_open=false;esp_app_desc_t desc;
  if(!rc)rc=esp_ota_get_partition_description(destination,&desc);
  if(!rc&&(strncmp(desc.project_name,BUDDY_PROJECT,sizeof desc.project_name)||strncmp(desc.version,version,sizeof desc.version)))rc=ESP_ERR_INVALID_VERSION;
  if(rc){fail(rc);cJSON_AddNumberToObject(j,"error",rc);return RBP_STATUS_DEVICE_ERROR;}
  state=STAGED;last=now;return 0;
 }
 if(op==BUDDY_UPDATE_ACTIVATE) {
  if(state!=STAGED)return RBP_STATUS_BAD_STATE;
  esp_err_t rc=esp_ota_set_boot_partition(destination);
  if(rc){fail(rc);cJSON_AddNumberToObject(j,"error",rc);return RBP_STATUS_DEVICE_ERROR;}
  state=RESTARTING;restart_at=now+750;return 0;
 }
 return RBP_STATUS_UNSUPPORTED;
}
