/* Generated from resources/bindings.json; do not edit. */
typedef struct {bool voice,zero_mod,mod_only; uint16_t min,max;} buddy_binding_rule_t;
static const buddy_binding_rule_t buddy_binding_rules[] = {
  {0,1,0,0,0},
  {0,0,1,4,223},
  {0,1,0,0,7},
  {1,0,1,4,223},
  {0,1,0,1,65535},
  {1,1,0,1,2},
  {0,1,0,0,0},
};
_Static_assert(BM_DISABLED == 0, "binding ABI");
_Static_assert(BM_KEYBOARD == 1, "binding ABI");
_Static_assert(BM_CONSUMER == 2, "binding ABI");
_Static_assert(BM_VOICE == 3, "binding ABI");
_Static_assert(BM_ACTION == 4, "binding ABI");
_Static_assert(BM_VOICE_PRESET == 5, "binding ABI");
_Static_assert(BM_TOGGLE_VOICE_MODE == 6, "binding ABI");
static const buddy_binding_t buddy_voice_doubao = {5,0,1};
static const buddy_binding_t buddy_voice_wechat = {5,0,2};
static const buddy_binding_t buddy_voice_meeting = {3,0,44};
static buddy_binding_t buddy_voice_shortcut(buddy_binding_t b) {
 if(b.value==buddy_voice_doubao.value)return (buddy_binding_t){3,64,0};
 if(b.value==buddy_voice_wechat.value)return (buddy_binding_t){3,9,0};
 return (buddy_binding_t){0};}
