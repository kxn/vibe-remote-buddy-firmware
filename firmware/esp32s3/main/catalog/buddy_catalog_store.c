#include "buddy_catalog_store.h"
#include "buddy_catalog_query.h"
#include "buddy_management.h"
#include "buddy_model_wire.h"
#include "buddy_update.h"
#include "esp_flash.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "nvs.h"
#include <stdlib.h>
#include <string.h>
static const esp_partition_t *banks[2];
static buddy_catalog_t catalogs[2];
static int active = -1;
static nvs_handle_t meta;
static bool available;
static uint32_t capacity;
static struct {
  uint32_t token, length, used, last;
  int bank;
} transfer;
static bool read_flash(void *ctx, uint32_t offset, void *p, size_t n) {
  return esp_partition_read(ctx, offset, p, n) == ESP_OK;
}
const buddy_catalog_t *buddy_catalog_active(void) {
  return active < 0 ? NULL : &catalogs[active];
}
bool buddy_catalog_store_busy(void) { return transfer.token != 0; }
void buddy_catalog_store_tick(uint32_t now, bool managed) {
  if (transfer.token && (!managed || (int32_t)(now - transfer.last) > 15000))
    transfer.token = 0;
}
void buddy_catalog_store_init(void) {
  uint32_t flash = 0;
  available = false;
  active = -1;
  transfer.token = 0;
  if (esp_flash_get_size(NULL, &flash) != ESP_OK) return;
  capacity = 0;
  for (unsigned i = 0; i < 2; i++) {
    const char *name = i ? "catalog1" : "catalog0";
    banks[i] = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, name);
#ifndef CONFIG_ESPTOOLPY_FLASHSIZE_4MB
    /* Only the original ab1 target may recover its pre-catalog partition table. */
    if (!banks[i] && flash >= 8u*1024u*1024u &&
        esp_partition_register_external(NULL, 0x460000+i*BUDDY_CATALOG_MAX_BYTES,
          BUDDY_CATALOG_MAX_BYTES, name, ESP_PARTITION_TYPE_DATA, 0x40+i, &banks[i]) != ESP_OK) return;
#endif
#ifdef CONFIG_ESPTOOLPY_FLASHSIZE_4MB
    const uint32_t expected_offset=0x2e0000+i*0x90000, expected_size=0x90000;
#else
    const uint32_t expected_offset=0x460000+i*BUDDY_CATALOG_MAX_BYTES, expected_size=BUDDY_CATALOG_MAX_BYTES;
#endif
    if (!banks[i] || banks[i]->address!=expected_offset || banks[i]->size!=expected_size ||
        banks[i]->address>flash || banks[i]->size>flash-banks[i]->address) return;
    capacity=banks[i]->size;
  }
  if (nvs_open("catalog", NVS_READWRITE, &meta) != ESP_OK)
    return;
  available = true;
  uint32_t selected;
  /* Only a committed selector is authoritative. An uploaded, validated but
   * uncommitted inactive image must not become active after a power failure. */
  if (nvs_get_u32(meta, "active", &selected) == ESP_OK && selected < 2 &&
      buddy_catalog_open(&catalogs[selected], read_flash,
                         (void *)banks[selected], banks[selected]->size))
    active = (int)selected;

}
void buddy_catalog_store_info(cJSON *j) {
  cJSON_AddNumberToObject(j, "catalog_api", 2);
  cJSON_AddBoolToObject(j, "catalog_available", available);
  cJSON_AddNumberToObject(j, "catalog_format", BUDDY_CATALOG_FORMAT);
  cJSON_AddNumberToObject(j, "catalog_max_count", BUDDY_CATALOG_MAX_MODELS);
  cJSON_AddNumberToObject(j, "catalog_max_bytes", capacity);
  cJSON_AddNumberToObject(j, "catalog_count",
                          active < 0 ? 0 : catalogs[active].model_count);
  cJSON_AddNumberToObject(j, "catalog_bytes",
                          active < 0 ? 0 : catalogs[active].length);
  cJSON_AddNumberToObject(j, "catalog_generation",
                          active < 0 ? 0 : catalogs[active].generation);
  cJSON_AddNumberToObject(j, "offset", transfer.used);
  cJSON_AddBoolToObject(j, "uploading", transfer.token != 0);
}
static int nibble(char c) {
  return c >= '0' && c <= '9'   ? c - '0'
         : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                : -1;
}
uint16_t buddy_catalog_store_command(uint16_t op, const cJSON *q, cJSON *j,
                                     uint32_t now, bool quiet) {
  if (op == BUDDY_DB_STATUS) {
    buddy_catalog_store_info(j);
    return 0;
  }
  if (!available)
    return RBP_STATUS_UNSUPPORTED;
  if (op == BUDDY_DB_BEGIN) {
    uint32_t length;
    if (!buddy_u32(q, "length", &length) || length < 64 ||
        length > capacity)
      return RBP_STATUS_INVALID_ARGUMENT;
    if (!quiet || !buddy_update_confirmed() || buddy_update_busy() ||
        transfer.token)
      return RBP_STATUS_BUSY;
    unsigned target = active == 0 ? 1 : 0;
    /* Flash erases are block-sized; no other bank is touched. */
    if (esp_partition_erase_range(banks[target], 0,
                                  (length + 4095u) & ~4095u) != ESP_OK)
      return RBP_STATUS_STORAGE_FAILED;
    transfer.token = esp_random() | 1;
    transfer.length = length;
    transfer.used = 0;
    transfer.last = now;
    transfer.bank = target;
    cJSON_AddNumberToObject(j, "token", transfer.token);
    return 0;
  }
  uint32_t token;
  if (!buddy_u32(q, "token", &token) || !transfer.token ||
      token != transfer.token || (int32_t)(now - transfer.last) > 15000)
    return RBP_STATUS_BAD_STATE;
  transfer.last = now;
  if (op == BUDDY_DB_ABORT) {
    transfer.token = 0;
    return 0;
  }
  if (!quiet || buddy_update_busy() || !buddy_update_confirmed())
    return RBP_STATUS_BUSY;
  if (op == BUDDY_DB_DATA) {
    const char *hex =
        cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(q, "hex"));
    uint32_t at;
    if (!buddy_u32(q, "offset", &at) || !hex || !strlen(hex) ||
        strlen(hex) > 384 || strlen(hex) % 2)
      return RBP_STATUS_INVALID_ARGUMENT;
    size_t n = strlen(hex) / 2;
    uint8_t bytes[192], previous[192];
    if (at > transfer.length || n > transfer.length - at)
      return RBP_STATUS_INVALID_ARGUMENT;
    for (unsigned i = 0; i < n; i++) {
      int a = nibble(hex[2 * i]), b = nibble(hex[2 * i + 1]);
      if (a < 0 || b < 0)
        return RBP_STATUS_INVALID_ARGUMENT;
      bytes[i] = (a << 4) | b;
    }
    if (at < transfer.used) {
      if (n > transfer.used - at ||
          esp_partition_read(banks[transfer.bank], at, previous, n) != ESP_OK ||
          memcmp(previous, bytes, n))
        return RBP_STATUS_BAD_STATE;
    } else {
      if (at != transfer.used)
        return RBP_STATUS_BAD_STATE;
      if (esp_partition_write(banks[transfer.bank], at, bytes, n) != ESP_OK)
        return RBP_STATUS_STORAGE_FAILED;
      transfer.used += (uint32_t)n;
    }
    cJSON_AddNumberToObject(j, "offset", transfer.used);
    return 0;
  }
  if (op != BUDDY_DB_COMMIT)
    return RBP_STATUS_UNSUPPORTED;
  if (transfer.used != transfer.length)
    return RBP_STATUS_BAD_STATE;
  buddy_catalog_t next;
  if (!buddy_catalog_open(&next, read_flash, (void *)banks[transfer.bank],
                          transfer.length) ||
      next.length != transfer.length)
    return RBP_STATUS_INVALID_ARGUMENT;
  if (active >= 0 && next.generation <= catalogs[active].generation)
    return RBP_STATUS_VERSION_MISMATCH;
  /* Reject invalid runtime definitions before publishing the selector. */
  uint8_t *bytes = malloc(BUDDY_MODEL_WIRE_MAX);
  buddy_model_t *model = malloc(sizeof *model);
  if (!bytes || !model) {
    free(bytes);
    free(model);
    return RBP_STATUS_RESOURCE_LIMIT;
  }
  bool valid = buddy_catalog_validate_lookups(&next);
  for (uint32_t i = 0; i < next.model_count && valid; i++) {
    buddy_catalog_entry_t e;
    size_t length;
    valid = buddy_catalog_entry(&next, i, &e) &&
            e.length <= BUDDY_MODEL_WIRE_MAX &&
            buddy_catalog_model_bytes(&next, &e, bytes, BUDDY_MODEL_WIRE_MAX, &length) &&
            buddy_model_unpack(bytes, length, model) &&
            e.hash == buddy_catalog_hash(model->id, strlen(model->id)) &&
            e.revision == model->revision;
  }
  free(bytes);
  free(model);
  if (!valid)
    return RBP_STATUS_INVALID_ARGUMENT;
  if (nvs_set_u32(meta, "active", transfer.bank) != ESP_OK ||
      nvs_commit(meta) != ESP_OK)
    return RBP_STATUS_STORAGE_FAILED;
  catalogs[transfer.bank] = next;
  active = transfer.bank;
  transfer.token = 0;
  buddy_catalog_store_info(j);
  return 0;
}
