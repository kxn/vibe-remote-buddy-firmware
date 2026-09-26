#include "device_model.h"

/* Slot order is the catalog presentation order (slot 0..key_count-1). */
static const rbp_key_def_t rc003_keys[] = {
    { RBP_KEY_POWER, "Power" },
    { RBP_KEY_UP, "Up" },
    { RBP_KEY_DOWN, "Down" },
    { RBP_KEY_LEFT, "Left" },
    { RBP_KEY_RIGHT, "Right" },
    { RBP_KEY_OK, "OK" },
    { RBP_KEY_BACK, "Back" },
    { RBP_KEY_HOME, "Home" },
    { RBP_KEY_MENU, "Menu" },
    { RBP_KEY_TV, "TV" },
    { RBP_KEY_VOLUME_UP, "Volume Up" },
    { RBP_KEY_VOLUME_DOWN, "Volume Down" },
    { RBP_KEY_VOICE, "Voice" },
};

const rbp_device_profile_t RBP_PROFILE_RC003 = {
    "xiaomi.rc003",
    "Xiaomi Remote 2 Pro",
    1,
    sizeof(rc003_keys) / sizeof(rc003_keys[0]) - 1,
    rc003_keys,
};

const rbp_device_profile_t RBP_PROFILE_RC003_VOICE = {"xiaomi.rc003", "Xiaomi Remote 2 Pro", 2, sizeof(rc003_keys)/sizeof(rc003_keys[0]), rc003_keys};

static const rbp_key_def_t unicom_keys[] = {
    {RBP_KEY_POWER, "Power"},
    {RBP_KEY_MUTE, "Mute"},
    {RBP_KEY_SETTINGS, "Settings"},
    {RBP_KEY_UP, "Up"},
    {RBP_KEY_DOWN, "Down"},
    {RBP_KEY_LEFT, "Left"},
    {RBP_KEY_RIGHT, "Right"},
    {RBP_KEY_OK, "Ok"},
    {RBP_KEY_HOME, "Home"},
    {RBP_KEY_LOCAL, "Local"},
    {RBP_KEY_BACK, "Back"},
    {RBP_KEY_VOLUME_UP, "Volume Up"},
    {RBP_KEY_VOLUME_DOWN, "Volume Down"},
    {RBP_KEY_CHANNEL_UP, "Channel Up"},
    {RBP_KEY_CHANNEL_DOWN, "Channel Down"},
    {RBP_KEY_VOICE, "Voice"},
    {RBP_KEY_DIGIT_1, "Digit 1"},
    {RBP_KEY_DIGIT_2, "Digit 2"},
    {RBP_KEY_DIGIT_3, "Digit 3"},
    {RBP_KEY_DIGIT_4, "Digit 4"},
    {RBP_KEY_DIGIT_5, "Digit 5"},
    {RBP_KEY_DIGIT_6, "Digit 6"},
    {RBP_KEY_DIGIT_7, "Digit 7"},
    {RBP_KEY_DIGIT_8, "Digit 8"},
    {RBP_KEY_DIGIT_9, "Digit 9"},
    {RBP_KEY_STAR, "Star"},
    {RBP_KEY_DIGIT_0, "Digit 0"},
    {RBP_KEY_HASH, "Hash"},
};
const rbp_device_profile_t RBP_PROFILE_UNICOM = {"unicom.hid_ico.v1", "Unicom BLE Voice Remote", 1, sizeof(unicom_keys)/sizeof(unicom_keys[0]), unicom_keys};
