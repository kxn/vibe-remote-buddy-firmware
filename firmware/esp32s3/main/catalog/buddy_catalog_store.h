#pragma once
#include "buddy_catalog.h"
#include "cJSON.h"
void buddy_catalog_store_init(void);
const buddy_catalog_t *buddy_catalog_active(void);
bool buddy_catalog_store_busy(void);
void buddy_catalog_store_tick(uint32_t now, bool managed);
void buddy_catalog_store_info(cJSON *);
uint16_t buddy_catalog_store_command(uint16_t,const cJSON *,cJSON *,uint32_t now,bool quiet);
#define BUDDY_DB_STATUS 0x460
#define BUDDY_DB_BEGIN 0x461
#define BUDDY_DB_DATA 0x462
#define BUDDY_DB_COMMIT 0x463
#define BUDDY_DB_ABORT 0x464
#define BUDDY_DB_ADOPT_DEFAULTS 0x465
