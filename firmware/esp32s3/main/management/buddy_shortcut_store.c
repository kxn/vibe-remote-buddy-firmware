#include "buddy_shortcut_store.h"
#include <string.h>

static const char *const keys[BUDDY_SHORTCUT_PLATFORMS] = {
    "voice-win", "voice-mac", "voice-linux"
};

esp_err_t buddy_shortcut_store_load(buddy_shortcut_store_t *s, nvs_handle_t handle,
                                    buddy_shortcuts_t *out) {
    memset(s, 0, sizeof *s);
    s->handle = handle;
    for (unsigned i = 0; i < BUDDY_SHORTCUT_PLATFORMS; i++) {
        uint8_t value = 0;
        esp_err_t rc = nvs_get_u8(handle, keys[i], &value);
        if (rc == ESP_ERR_NVS_NOT_FOUND) continue;
        if (rc == ESP_OK && !buddy_shortcut_valid(buddy_shortcut_host(i), value))
            rc = ESP_ERR_INVALID_ARG;
        if (rc == ESP_OK) s->saved.value[i] = value;
        else s->error = rc; /* Ignore only the invalid platform; keep other settings. */
    }
    *out = s->saved;
    return s->error;
}

esp_err_t buddy_shortcut_store_save(buddy_shortcut_store_t *s, unsigned platform,
                                    uint8_t value) {
    if (platform >= BUDDY_SHORTCUT_PLATFORMS ||
        !buddy_shortcut_valid(buddy_shortcut_host(platform), value))
        return ESP_ERR_INVALID_ARG;
    if (s->error == ESP_OK && s->saved.value[platform] == value) return ESP_OK;
    esp_err_t rc = nvs_set_u8(s->handle, keys[platform], value);
    if (rc == ESP_OK) rc = nvs_commit(s->handle);
    s->error = rc;
    if (rc == ESP_OK) { s->saved.value[platform] = value; s->commits++; }
    return rc;
}
