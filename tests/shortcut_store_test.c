#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "buddy_shortcut_store.h"
static int flash[3] = {-1, -1, -1}, staged[3] = {-1, -1, -1};
static unsigned writes, commits;
static esp_err_t set_error, commit_error;
static unsigned index_of(const char *key) {
    if (!strcmp(key, "voice-win")) return 0;
    if (!strcmp(key, "voice-mac")) return 1;
    assert(!strcmp(key, "voice-linux")); return 2;
}
esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *out) {
    assert(handle == 1); int value = flash[index_of(key)];
    if (value < 0) return ESP_ERR_NVS_NOT_FOUND;
    *out = (uint8_t)value; return ESP_OK;
}
esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value) {
    assert(handle == 1); writes++;
    if (set_error) return set_error;
    staged[index_of(key)] = value; return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle) {
    assert(handle == 1); commits++;
    if (commit_error) return commit_error;
    for (unsigned i = 0; i < 3; i++) if (staged[i] >= 0) { flash[i] = staged[i]; staged[i] = -1; }
    return ESP_OK;
}
int main(void) {
    buddy_shortcut_store_t store;
    buddy_shortcuts_t loaded;
    assert(!buddy_shortcut_store_load(&store, 1, &loaded));
    assert(!loaded.value[0] && !loaded.value[1] && !loaded.value[2] && !writes);
    assert(!buddy_shortcut_store_save(&store, 0, BUDDY_SHORTCUT_RCTRL));
    assert(!buddy_shortcut_store_save(&store, 1, BUDDY_SHORTCUT_FN));
    assert(!buddy_shortcut_store_save(&store, 2, BUDDY_SHORTCUT_RSHIFT));
    assert(writes == 3 && commits == 3);
    assert(!buddy_shortcut_store_save(&store, 0, BUDDY_SHORTCUT_RCTRL) && writes == 3);
    assert(buddy_shortcut_store_save(&store, 0, BUDDY_SHORTCUT_FN) == ESP_ERR_INVALID_ARG);
    assert(buddy_shortcut_store_save(&store, 2, BUDDY_SHORTCUT_FN) == ESP_ERR_INVALID_ARG);
    assert(buddy_shortcut_store_save(&store, 3, 1) == ESP_ERR_INVALID_ARG && writes == 3);
    assert(!buddy_shortcut_store_load(&store, 1, &loaded));
    assert(loaded.value[0] == 2 && loaded.value[1] == 4 && loaded.value[2] == 3);
    set_error = 0x1111;
    assert(buddy_shortcut_store_save(&store, 0, BUDDY_SHORTCUT_RALT) == set_error);
    assert(store.saved.value[0] == 2 && flash[0] == 2);
    set_error = 0; commit_error = 0x1112;
    assert(buddy_shortcut_store_save(&store, 0, BUDDY_SHORTCUT_RALT) == commit_error);
    assert(store.saved.value[0] == 2 && flash[0] == 2);
    commit_error = 0;
    assert(!buddy_shortcut_store_save(&store, 0, BUDDY_SHORTCUT_RALT));
    assert(!buddy_shortcut_store_load(&store, 1, &loaded));
    assert(loaded.value[0] == 1 && loaded.value[1] == 4 && loaded.value[2] == 3);
    flash[0] = BUDDY_SHORTCUT_FN; /* Invalid Windows record cannot enable Globe. */
    assert(buddy_shortcut_store_load(&store, 1, &loaded) == ESP_ERR_INVALID_ARG);
    assert(loaded.value[0] == 0 && loaded.value[1] == 4 && loaded.value[2] == 3);
    puts("Per-platform persistence, reload, write deduplication and failure retry passed.");
    return 0;
}
