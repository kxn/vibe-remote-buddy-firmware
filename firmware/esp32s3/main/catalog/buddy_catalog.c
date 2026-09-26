#include "buddy_catalog.h"
#include <string.h>
static uint32_t u32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}
static bool range(uint32_t at, uint32_t n, uint32_t end) {
  return at <= end && n <= end - at;
}
uint32_t buddy_catalog_hash(const void *bytes, size_t n) {
  const uint8_t *p = bytes;
  uint32_t crc = UINT32_MAX;
  while (n--) {
    crc ^= *p++;
    for (unsigned i = 0; i < 8; i++)
      crc = (crc >> 1) ^ ((crc & 1) ? 0x82f63b78u : 0);
  }
  return ~crc;
}
bool buddy_catalog_object(const buddy_catalog_t *c, uint32_t at, void *p,
                          size_t n) {
  return c && p && n <= UINT32_MAX && at >= c->objects &&
         range(at, (uint32_t)n, c->length) && c->read(c->context, at, p, n);
}
bool buddy_catalog_entry(const buddy_catalog_t *c, uint32_t index,
                         buddy_catalog_entry_t *e) {
  uint8_t p[16];
  if (!c || !e || index >= c->model_count ||
      !c->read(c->context, c->directory + index * 16, p, 16))
    return false;
  *e = (buddy_catalog_entry_t){u32(p), u32(p + 4), u32(p + 8), u32(p + 12)};
  return true;
}
bool buddy_catalog_lookup(const buddy_catalog_t *c, bool fp, uint32_t index,
                          buddy_catalog_lookup_t *e) {
  uint8_t p[12];
  if (!c || !e || index >= (fp ? c->fingerprint_count : c->hint_count) ||
      !c->read(c->context, (fp ? c->fingerprints : c->hints) + index * 12, p,
               12))
    return false;
  *e = (buddy_catalog_lookup_t){u32(p), u32(p + 4), u32(p + 8)};
  return true;
}
uint32_t buddy_catalog_lower_bound(const buddy_catalog_t *c, bool fp,
                                   uint32_t hash) {
  uint32_t lo = 0, hi = fp ? c->fingerprint_count : c->hint_count;
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2;
    buddy_catalog_lookup_t e;
    if (!buddy_catalog_lookup(c, fp, mid, &e))
      return UINT32_MAX;
    if (e.hash < hash)
      lo = mid + 1;
    else
      hi = mid;
  }
  return lo;
}
bool buddy_catalog_open(buddy_catalog_t *out, buddy_catalog_read_fn read,
                        void *ctx, uint32_t available) {
  uint8_t h[64];
  if (!out || !read || available < sizeof h || !read(ctx, 0, h, sizeof h) ||
      memcmp(h, "VRBC", 4) || (u32(h + 4) != 1 && u32(h + 4) != BUDDY_CATALOG_FORMAT) ||
      u32(h + 8) != 64)
    return false;
  buddy_catalog_t c = {.format = u32(h + 4), .read = read,
                       .context = ctx,
                       .length = u32(h + 12),
                       .generation = u32(h + 16),
                       .model_count = u32(h + 20),
                       .fingerprint_count = u32(h + 24),
                       .hint_count = u32(h + 28),
                       .directory = u32(h + 32),
                       .fingerprints = u32(h + 36),
                       .hints = u32(h + 40),
                       .objects = u32(h + 44)};
  if (c.length > available || c.length > BUDDY_CATALOG_MAX_BYTES ||
      c.length < 64 || !c.generation ||
      c.model_count > BUDDY_CATALOG_MAX_MODELS ||
      c.fingerprint_count > 8 * c.model_count ||
      c.hint_count > 64 * c.model_count)
    return false;
  for (unsigned i = 52; i < 64; i++)
    if (h[i])
      return false;
  if (c.directory != 64 || c.fingerprints != c.directory + c.model_count * 16 ||
      c.hints != c.fingerprints + c.fingerprint_count * 12 ||
      c.objects != c.hints + c.hint_count * 12 || c.objects > c.length)
    return false;
  /* Checksum covers the header as well; its own field is zeroed. */
  uint32_t crc = UINT32_MAX;
  uint8_t chunk[256];
  for (uint32_t at = 0; at < c.length;) {
    uint32_t n = c.length - at;
    if (n > sizeof chunk)
      n = sizeof chunk;
    if (!read(ctx, at, chunk, n))
      return false;
    for (unsigned j = 0; j < n; j++) {
      uint32_t pos = at + j;
      uint8_t b = pos >= 48 && pos < 52 ? 0 : chunk[j];
      crc ^= b;
      for (unsigned k = 0; k < 8; k++)
        crc = (crc >> 1) ^ ((crc & 1) ? 0x82f63b78u : 0);
    }
    at += n;
  }
  if (~crc != u32(h + 48))
    return false;
  uint32_t previous = 0;
  for (uint32_t i = 0; i < c.model_count; i++) {
    buddy_catalog_entry_t e;
    if (!buddy_catalog_entry(&c, i, &e) || !e.length || !e.revision ||
        e.offset < c.objects || !range(e.offset, e.length, c.length) ||
        (i && e.hash < previous))
      return false;
    previous = e.hash;
  }
  for (unsigned type = 0; type < 2; type++) {
    previous = 0;
    uint32_t count = type ? c.fingerprint_count : c.hint_count;
    for (uint32_t i = 0; i < count; i++) {
      buddy_catalog_lookup_t e;
      uint8_t size[4];
      if (!buddy_catalog_lookup(&c, type != 0, i, &e) ||
          e.model >= c.model_count || (i && e.hash < previous) ||
          !buddy_catalog_object(&c, e.offset, size, 4))
        return false;
      uint32_t length = u32(size);
      if (!length || !range(e.offset + 4, length, c.length))
        return false;
      previous = e.hash;
    }
  }
  *out = c;
  return true;
}

bool buddy_catalog_model_bytes(const buddy_catalog_t *c, const buddy_catalog_entry_t *e,
                               uint8_t *out, size_t capacity, size_t *length) {
  if (!c || !e || !out || !length || e->length > capacity || e->length < 14) return false;
  if (c->format == 1) {
    *length = e->length;
    return buddy_catalog_object(c, e->offset, out, e->length);
  }
  uint8_t header[62];
  if (!buddy_catalog_object(c, e->offset, header, 11) || header[0] != 3 || header[10] >= 48) return false;
  size_t n = 14u + header[10];
  if (e->length < n + 1 || n > capacity || !buddy_catalog_object(c, e->offset, header, n + 1)) return false;
  unsigned count = header[n];
  if (count != header[n-3] + header[n-2] + (header[n-1] != 0) || e->length != n+1+6u*count) return false;
  memcpy(out, header, n); out[0] = 2;
  for (unsigned i=0; i<count; i++) {
    uint8_t ref[6];
    if (!buddy_catalog_object(c, e->offset + (uint32_t)(14u+header[10]+1+6u*i), ref, sizeof ref)) return false;
    uint32_t size = ref[4] | ((uint32_t)ref[5]<<8);
    if (!size || size > capacity-n || !buddy_catalog_object(c, u32(ref), out+n, size)) return false;
    n += size;
  }
  *length=n; return true;
}
