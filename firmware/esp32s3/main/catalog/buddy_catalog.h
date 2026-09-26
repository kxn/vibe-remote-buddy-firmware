#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* Offsets are relative to the image. All persisted integers are LE. */
#define BUDDY_CATALOG_FORMAT 2
#define BUDDY_CATALOG_HEADER 64
#define BUDDY_CATALOG_MAX_MODELS 4096
#define BUDDY_CATALOG_MAX_BYTES (1536u * 1024u)
#define BUDDY_CATALOG_DIRECTORY_BYTES 16
#define BUDDY_CATALOG_LOOKUP_BYTES 12
typedef bool (*buddy_catalog_read_fn)(void *, uint32_t, void *, size_t);
typedef struct {
  buddy_catalog_read_fn read;
  void *context;
  uint32_t format, length, generation, model_count, fingerprint_count, hint_count;
  uint32_t directory, fingerprints, hints, objects;
} buddy_catalog_t;
typedef struct { uint32_t hash, offset, length, revision; } buddy_catalog_entry_t;
typedef struct { uint32_t hash, model, offset; } buddy_catalog_lookup_t;
_Static_assert(sizeof(buddy_catalog_entry_t)==16,"catalog directory budget");
_Static_assert(sizeof(buddy_catalog_lookup_t)==12,"catalog lookup budget");
/* Open validates the complete image before any object becomes visible. */
bool buddy_catalog_open(buddy_catalog_t *,buddy_catalog_read_fn,void *,uint32_t available);
bool buddy_catalog_entry(const buddy_catalog_t *,uint32_t,buddy_catalog_entry_t *);
bool buddy_catalog_lookup(const buddy_catalog_t *,bool fingerprint,uint32_t,buddy_catalog_lookup_t *);
bool buddy_catalog_object(const buddy_catalog_t *,uint32_t,void *,size_t);
/* Lower-bound search over the sorted digest index; collisions remain a range.
 * Callers MUST compare the actual ID/fingerprint/hint before accepting a hit. */
uint32_t buddy_catalog_lower_bound(const buddy_catalog_t *,bool fingerprint,uint32_t hash);
uint32_t buddy_catalog_hash(const void *,size_t);

/* Reconstruct a bounded snapshot from shared format-2 sections. */
bool buddy_catalog_model_bytes(const buddy_catalog_t *, const buddy_catalog_entry_t *, uint8_t *, size_t, size_t *);
