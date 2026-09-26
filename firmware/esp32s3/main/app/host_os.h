#pragma once
#include <stdint.h>
#include <stdbool.h>
typedef enum { BUDDY_HOST_UNKNOWN, BUDDY_HOST_WINDOWS, BUDDY_HOST_MACOS, BUDDY_HOST_OTHER } buddy_host_os_t;
typedef struct { uint16_t strings, short2, short4, full255; uint32_t last; buddy_host_os_t result; bool frozen; } buddy_host_probe_t;
void buddy_host_reset(buddy_host_probe_t *p);
void buddy_host_setup(buddy_host_probe_t *p, uint8_t type, uint8_t request, uint16_t value, uint16_t length, uint32_t now);
buddy_host_os_t buddy_host_classify(buddy_host_probe_t *p, uint32_t now, bool configured);
/* Atomic snapshot, updated only by the USB task. */
buddy_host_os_t s3_host_os(void);
