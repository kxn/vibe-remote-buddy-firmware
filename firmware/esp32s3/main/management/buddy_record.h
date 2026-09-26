#pragma once
#include "buddy_input.h"
#include "buddy_models.h"
#include "host/ble_hs.h"
#include "cJSON.h"
#define RECORD_VERSION 2
/* Memory layout is private. Persistent representation is explicit JSON. */
typedef struct {
 uint32_t version, peer_id;
 bool cleanup_pending; /* Durable unbind tombstone; address retained until bond removal. */
 ble_addr_t address;
 char name[48];
 char model_id[48];
 uint8_t model;
 uint16_t cache_len;
 buddy_map_t map;
 /* Hardware/defaults are owned by this binding, never a library pointer. */
 buddy_model_t definition;
 uint64_t overrides;
 uint8_t cache[1200];
} record_t;
char *buddy_record_encode(const record_t *r);
bool buddy_record_decode(const char *text, size_t length, record_t *r);

/* Repair the precisely identified incomplete catalog-v2 Xiaomi snapshots. */
bool buddy_record_repair_catalog_voice(record_t *r);
