/* Generated from resources/voice-protocols.json; do not edit. */
typedef struct {uint8_t id,type,properties;} voice_report_rule_t;
static const voice_report_rule_t voice_reports_1[] = {
  {1,1,16},
};
static const voice_report_rule_t voice_reports_2[] = {
  {1,1,16},
  {3,1,16},
  {252,1,16},
  {251,2,8},
  {248,1,16},
  {250,2,8},
  {249,1,16},
};
static const voice_report_rule_t voice_reports_3[] = {
  {1,1,16},
  {4,3,8},
  {5,3,16},
  {6,3,16},
  {7,3,16},
  {8,3,16},
};
static unsigned voice_report_rules(unsigned family,const voice_report_rule_t **out) {
 switch(family){
 case 1:*out=voice_reports_1;return 1;
 case 2:*out=voice_reports_2;return 7;
 case 3:*out=voice_reports_3;return 6;
 default:*out=NULL;return 0;}}
static unsigned protocol_min_mtu(unsigned family){switch(family){
 case 1:return 123;
 case 2:return 23;
 case 3:return 63;
 default:return 0;}}

#define VOICE_ATVV_2_PROPERTIES 12

#define VOICE_ATVV_3_PROPERTIES 16

#define VOICE_ATVV_4_PROPERTIES 16

static const uint8_t k_atvv_svc_uuid[16] = {0x64,0xb6,0x17,0xf6,0x01,0xaf,0x7d,0xbc,0x05,0x4f,0x21,0x5a,0x01,0x00,0x5e,0xab};

static const uint8_t k_atvv_tx_uuid[16] = {0x64,0xb6,0x17,0xf6,0x01,0xaf,0x7d,0xbc,0x05,0x4f,0x21,0x5a,0x02,0x00,0x5e,0xab};

static const uint8_t k_atvv_audio_uuid[16] = {0x64,0xb6,0x17,0xf6,0x01,0xaf,0x7d,0xbc,0x05,0x4f,0x21,0x5a,0x03,0x00,0x5e,0xab};

static const uint8_t k_atvv_ctl_uuid[16] = {0x64,0xb6,0x17,0xf6,0x01,0xaf,0x7d,0xbc,0x05,0x4f,0x21,0x5a,0x04,0x00,0x5e,0xab};
