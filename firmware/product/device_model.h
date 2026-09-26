/* Logical device catalog and profile definitions. */
#ifndef RBP_DEVICE_MODEL_H
#define RBP_DEVICE_MODEL_H

#include "rbp/defs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t key_id;
    const char *name;
} rbp_key_def_t;

typedef struct {
    const char *model_id;      /* stable, e.g. "xiaomi.rc003" */
    const char *display_name;  /* readable, e.g. "Xiaomi Remote 2 Pro" */
    uint32_t catalog_revision;
    uint8_t key_count;
    const rbp_key_def_t *keys; /* key_count entries, slot = index */
} rbp_device_profile_t;

/* Voice is published only after a physical press/release is observed. */
extern const rbp_device_profile_t RBP_PROFILE_UNICOM;
extern const rbp_device_profile_t RBP_PROFILE_RC003;
extern const rbp_device_profile_t RBP_PROFILE_RC003_VOICE;

#ifdef __cplusplus
}
#endif
#endif /* RBP_DEVICE_MODEL_H */
