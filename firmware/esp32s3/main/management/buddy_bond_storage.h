#pragma once
#include "buddy_record.h"
#include "host/ble_store.h"
#define BUDDY_BINDING_BLOB_MAX (16384u + 146u)
typedef struct {
  bool our_present, peer_present;
  struct ble_store_value_sec our, peer;
} buddy_bond_t;
/* The only persistent binding unit. Never expose these bytes in management
 * snapshots, catalog resources or diagnostics: they contain pairing keys. */
uint8_t *buddy_binding_blob_encode(const record_t *, const buddy_bond_t *, size_t *);
bool buddy_binding_blob_decode(const uint8_t *, size_t, record_t *, buddy_bond_t *, bool *legacy);
bool buddy_bond_capture(const ble_addr_t *, buddy_bond_t *);
int buddy_bond_restore(const buddy_bond_t *);
