#pragma once
#include "cJSON.h"
#include <stdint.h>
#include <stdbool.h>
#define BUDDY_UPDATE_STATUS 0x420
#define BUDDY_UPDATE_BEGIN 0x421
#define BUDDY_UPDATE_DATA 0x422
#define BUDDY_UPDATE_FINISH 0x423
#define BUDDY_UPDATE_ACTIVATE 0x424
#define BUDDY_UPDATE_ABORT 0x425
#define BUDDY_DATA_SCHEMA 3
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif
#if defined(CONFIG_ESPTOOLPY_FLASHSIZE_4MB)
#define BUDDY_TARGET "s3-q2-f4-ab2"
#define BUDDY_PROJECT "buddy_s3_q2_f4_ab2"
#define BUDDY_PSRAM_MIN (2u * 1024u * 1024u)
#elif defined(CONFIG_SPIRAM_MODE_QUAD)
#define BUDDY_TARGET "s3-q2-ab1"
#define BUDDY_PROJECT "buddy_s3_q2_ab1"
#define BUDDY_PSRAM_MIN (2u * 1024u * 1024u)
#else
#define BUDDY_TARGET "s3-o8-ab1"
#define BUDDY_PROJECT "buddy_s3_o8_ab1"
#define BUDDY_PSRAM_MIN (8u * 1024u * 1024u)
#endif
void buddy_update_init(void);
bool buddy_update_busy(void);
bool buddy_update_confirmed(void);
bool buddy_update_storage_ready(void);
void buddy_update_tick(uint32_t now,bool managed,bool ble_ready,bool quiet);
uint16_t buddy_update_command(uint16_t op,const cJSON *q,cJSON *j,uint32_t now);
void buddy_update_info(cJSON *j);
