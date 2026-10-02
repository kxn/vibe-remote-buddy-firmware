#pragma once
#include "buddy_shortcuts.h"
#include "nvs.h"

typedef struct {
    nvs_handle_t handle;
    buddy_shortcuts_t saved;
    esp_err_t error;
    uint32_t commits;
} buddy_shortcut_store_t;
/* Caller opens the active A/B data bank and serializes all calls on BLE task. */
esp_err_t buddy_shortcut_store_load(buddy_shortcut_store_t *store, nvs_handle_t handle,
                                    buddy_shortcuts_t *out);
esp_err_t buddy_shortcut_store_save(buddy_shortcut_store_t *store, unsigned platform,
                                    uint8_t value);
