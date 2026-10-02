#pragma once
#include "cJSON.h"

void buddy_power_init(void);
/* Host-task only. Samples the chip sensor on demand, never in a periodic task. */
void buddy_power_stats(cJSON *out);
