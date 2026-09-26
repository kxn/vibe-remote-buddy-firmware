#pragma once
#include "buddy_models.h"
#include <stddef.h>
/* Portable little-endian runtime object. No ABI layout or pointers on Flash. */
#define BUDDY_MODEL_WIRE_VERSION 2
#define BUDDY_MODEL_WIRE_MAX 6144
size_t buddy_model_pack(const buddy_model_t *model, uint8_t *bytes, size_t capacity);
bool buddy_model_unpack(const uint8_t *bytes, size_t length, buddy_model_t *model);
