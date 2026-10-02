#include "buddy_input.h"
#include <string.h>
#include "buddy_standard_bindings.inc"
#include "buddy_binding_rules.inc"
void buddy_map_default(buddy_map_t *m) {
  memset(m, 0, sizeof *m);
  m->version = BUDDY_MAP_VERSION;
  m->revision = 1;
  memcpy(m->key, buddy_standard_bindings, sizeof m->key);
}
bool buddy_binding_valid(unsigned key, buddy_binding_t b) {
  if (!key || key >= BUDDY_KEYS || b.kind >= sizeof buddy_binding_rules/sizeof buddy_binding_rules[0]) return false;
  const buddy_binding_rule_t *r=&buddy_binding_rules[b.kind];
  return r->voice==(key==RBP_KEY_VOICE) && (!r->zero_mod || !b.modifiers) &&
    ((b.value>=r->min && b.value<=r->max) || (r->mod_only && !b.value && b.modifiers));
}
void buddy_input_init(buddy_input_t *i) {
  memset(i, 0, sizeof *i);
  buddy_arbiter_init(&i->arbiter);
  for (unsigned s = 0; s < BUDDY_SLOTS; s++)
    buddy_map_default(&i->map[s]);
}
uint64_t buddy_input_keys(buddy_input_t *i, unsigned s, uint32_t g,
                          uint64_t bits) {
  if (s >= BUDDY_SLOTS || !g || g != i->arbiter.generation[s])
    return 0;
  uint64_t edge = bits & ~i->arbiter.keys[s], actions = 0;
  uint16_t media = 0;
  for (unsigned k = 1; k < BUDDY_KEYS; k++)
    if (bits & (UINT64_C(1) << k)) {
      buddy_binding_t b = i->map[s].key[k];
      if (b.kind == BM_TOGGLE_VOICE_MODE && (edge & (UINT64_C(1) << k)))
        i->voice_mode_toggled[s] = !i->voice_mode_toggled[s];
      if (b.kind == BM_CONSUMER &&
          (b.value < 3 || (i->arbiter.owner == BUDDY_NO_OWNER &&
                           !(i->arbiter.suppressed[s] & (UINT64_C(1) << k)))))
        media |= 1u << b.value;
      if (b.kind == BM_ACTION && (edge & (UINT64_C(1) << k)) && i->management &&
          i->arbiter.owner == BUDDY_NO_OWNER)
        actions |= UINT64_C(1) << k;
    }
  buddy_arbiter_keys(&i->arbiter, s, g, bits, media);
  return actions;
}
buddy_binding_t buddy_input_voice_binding(const buddy_input_t *i, unsigned s) {
  if (s >= BUDDY_SLOTS) return (buddy_binding_t){0};
  buddy_binding_t b = i->map[s].key[RBP_KEY_VOICE];
  if (!i->voice_mode_toggled[s]) return b;
  if (b.kind == buddy_voice_meeting.kind && b.modifiers == buddy_voice_meeting.modifiers && b.value == buddy_voice_meeting.value)
    return buddy_voice_doubao;
  return buddy_voice_meeting;
}
void buddy_input_keyboard(const buddy_input_t *i, uint8_t out[8]) {
  memset(out, 0, 8);
  uint8_t held[256] = {0};
  unsigned owner = i->arbiter.owner;
  if (owner != BUDDY_NO_OWNER) {
    if (i->arbiter.down[owner] && !i->arbiter.rejected[owner]) {
      buddy_binding_t b = buddy_input_voice_binding(i, owner);
      buddy_voice_keyboard(b, BUDDY_HOST_UNKNOWN, out);
    }
    return;
  }
  for (unsigned s = 0; s < BUDDY_SLOTS; s++) {
    uint64_t bits = i->arbiter.keys[s] & ~i->arbiter.suppressed[s];
    for (unsigned k = 1; k < BUDDY_KEYS; k++)
      if (bits & (UINT64_C(1) << k)) {
        buddy_binding_t b = i->map[s].key[k];
        if (b.kind != BM_KEYBOARD)
          continue;
        out[0] |= b.modifiers;
        if (b.value)
          held[b.value] = 1;
      }
  }
  unsigned n = 0;
  for (unsigned k = 4; k < 224; k++)
    if (held[k]) {
      if (n == 6) {
        memset(out + 2, 1, 6);
        return;
      }
      out[2 + n++] = k;
    }
}

void buddy_voice_keyboard(buddy_binding_t b, buddy_host_os_t host, uint8_t out[8]) {
  memset(out, 0, 8);
  if (b.kind == BM_VOICE_PRESET) {
    b = buddy_voice_shortcut(b);
    if (b.kind != BM_VOICE) return;
    if (host == BUDDY_HOST_MACOS) out[1] = 1; /* Consumer 0x029d: macOS Globe/Fn */
    else { out[0] = b.modifiers; out[2] = b.value; }
  } else if (b.kind == BM_VOICE) { out[0] = b.modifiers; out[2] = b.value; }
}
