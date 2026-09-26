#include "buddy_model_wire.h"
#include <stdlib.h>
#include <string.h>
typedef struct {
  uint8_t *p;
  size_t left;
  bool ok;
} writer;
typedef struct {
  const uint8_t *p;
  size_t left;
  bool ok;
} reader;
static void put(writer *w, uint32_t n, unsigned width) {
  if (w->left < width) {
    w->ok = false;
    return;
  }
  for (unsigned i = 0; i < width; i++)
    *w->p++ = (uint8_t)(n >> (8 * i));
  w->left -= width;
}
static uint32_t get(reader *r, unsigned width) {
  if (r->left < width) {
    r->ok = false;
    return 0;
  }
  uint32_t v = 0;
  for (unsigned i = 0; i < width; i++)
    v |= (uint32_t)*r->p++ << (8 * i);
  r->left -= width;
  return v;
}
static void string_put(writer *w, const char *s, size_t cap) {
  const char *end = memchr(s, 0, cap);
  if (!end) {
    w->ok = false;
    return;
  }
  size_t n = (size_t)(end - s);
  put(w, (uint32_t)n, 1);
  for (size_t i = 0; i < n; i++)
    put(w, (uint8_t)s[i], 1);
}
static void string_get(reader *r, char *s, size_t cap) {
  unsigned n = get(r, 1);
  if (n >= cap || n > r->left) {
    r->ok = false;
    return;
  }
  memcpy(s, r->p, n);
  s[n] = 0;
  r->p += n;
  r->left -= n;
  if (memchr(s, 0, n))
    r->ok = false;
}
size_t buddy_model_pack(const buddy_model_t *m, uint8_t *p, size_t n) {
  if (!m || !p || m->key_count > 63 || m->raw_count > 64 || m->match_count > 8)
    return 0;
  writer w = {p, n, true};
  put(&w, BUDDY_MODEL_WIRE_VERSION, 1);
  put(&w, m->family, 1);
  put(&w, m->revision, 4);
  put(&w, m->map_crc, 4);
  string_put(&w, m->id, sizeof m->id);
  put(&w, m->match_count, 1);
  put(&w, m->key_count, 1);
  put(&w, m->raw_count, 1);
  for (unsigned i = 0; i < m->match_count; i++) {
    string_put(&w, m->matches[i].name, 48);
    string_put(&w, m->matches[i].prefix, 48);
    put(&w, (uint32_t)m->matches[i].company, 4);
  }
  for (unsigned i = 0; i < m->key_count; i++) {
    unsigned k = m->keys[i];
    if (!k || k >= BUDDY_KEYS)
      return 0;
    buddy_binding_t b = m->defaults.key[k];
    put(&w, k, 1);
    put(&w, b.kind, 1);
    put(&w, b.modifiers, 1);
    put(&w, b.value, 2);
    string_put(&w, m->labels[k], 48);
  }
  for (unsigned i = 0; i < m->raw_count; i++) {
    put(&w, m->raw[i].report, 1);
    put(&w, m->raw[i].usage, 2);
    put(&w, m->raw[i].key, 1);
  }
  return w.ok ? n - w.left : 0;
}
static bool unpack_into(const uint8_t *p, size_t n, buddy_model_t *out) {
  if (!p || !out || n > BUDDY_MODEL_WIRE_MAX)
    return false;
  reader r = {p, n, true};
  buddy_model_t *m = out;
  unsigned version = get(&r, 1);
  if (version != 1 && version != BUDDY_MODEL_WIRE_VERSION)
    return false;
  m->family = get(&r, 1);
  m->revision = get(&r, 4);
  m->map_crc = get(&r, 4);
  string_get(&r, m->id, sizeof m->id);
  m->match_count = get(&r, 1);
  m->key_count = get(&r, 1);
  m->raw_count = get(&r, 1);
  if (!m->id[0] || !m->revision || m->family < 1 || m->family > 3 ||
      !m->match_count || m->match_count > 8 || !m->key_count ||
      m->key_count > 63 || m->raw_count > 64)
    return false;
  for (unsigned i = 0; i < m->match_count; i++) {
    buddy_match_t *b = &m->matches[i];
    string_get(&r, b->name, 48);
    string_get(&r, b->prefix, 48);
    uint32_t c = get(&r, 4);
    if (c > 65535 && c != UINT32_MAX)
      return false;
    b->company = c == UINT32_MAX ? -1 : (int)c;
  }
  m->defaults.version = BUDDY_MAP_VERSION;
  m->defaults.revision = 1;
  for (unsigned i = 0; i < m->key_count; i++) {
    unsigned k = get(&r, 1);
    buddy_binding_t b = {0};
    b.kind = get(&r, 1);
    b.modifiers = get(&r, 1);
    b.value = get(&r, 2);
    if(!k || k>=BUDDY_KEYS)return false;
    m->keys[i] = k;
    m->defaults.key[k] = b;
    if (version >= 2)
      string_get(&r, m->labels[k], 48);
  }
  for (unsigned i = 0; i < m->raw_count; i++) {
    buddy_raw_key_t *b = &m->raw[i];
    b->report = get(&r, 1);
    b->usage = get(&r, 2);
    b->key = get(&r, 1);

  }
  if (!r.ok || r.left)
    return false;

  return buddy_model_valid(m);
}

bool buddy_model_unpack(const uint8_t *p, size_t n, buddy_model_t *out) {
  if (!out)
    return false;
  buddy_model_t *value = calloc(1, sizeof *value);
  if (!value)
    return false;
  bool ok = unpack_into(p, n, value);
  if (ok)
    *out = *value;
  free(value);
  return ok;
}
