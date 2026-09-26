#pragma once
#include "buddy_input.h"
#include "cJSON.h"
#define BUDDY_MODELS 16
#define BUDDY_MODEL_BYTES 6144
#define BUDDY_MODEL_ID 48
typedef struct {
  char name[48], prefix[48];
  int company;
} buddy_match_t;
typedef struct {
  uint8_t report;
  uint16_t usage;
  uint8_t key;
} buddy_raw_key_t;
typedef struct {
  char id[BUDDY_MODEL_ID];
  uint32_t revision, map_crc;
  uint8_t family, match_count, key_count, raw_count;
  buddy_match_t matches[8];
  uint8_t keys[63];
  char labels[BUDDY_KEYS][48];
  buddy_raw_key_t raw[64];
  buddy_map_t defaults;
} buddy_model_t;
bool buddy_model_parse(const cJSON *, buddy_model_t *);
bool buddy_model_equal(const buddy_model_t *, const buddy_model_t *);
int buddy_model_match(const buddy_model_t *, unsigned count, const char *name,
                      int company);
bool buddy_model_has_key(const buddy_model_t *, unsigned key);
extern const char *const buddy_builtin_models[2];

int buddy_model_resolve(const buddy_model_t *, unsigned count, uint32_t crc, int selected);
bool buddy_model_compatible(const buddy_model_t *a, const buddy_model_t *b);

bool buddy_model_valid(const buddy_model_t *);
rbp_device_profile_t buddy_model_profile(const buddy_model_t *,rbp_key_def_t *,char (*labels)[48],const char *id);
int buddy_model_match_hints(const buddy_model_t *,unsigned,const char *,int,bool);
