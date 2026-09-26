#include "ima_decoder.h"

static const int16_t k_step_table[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

static const int8_t k_index_table[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8,
};

void ima_state_reset(ima_state_t *st, int16_t predictor, uint8_t step_index)
{
    if (step_index > 88) step_index = 88;
    st->predictor = predictor;
    st->step_index = step_index;
}

int16_t ima_decode_nibble(ima_state_t *st, uint8_t nibble)
{
    int32_t step = k_step_table[st->step_index];
    int32_t diff = step >> 3;
    if (nibble & 4) diff += step;
    if (nibble & 2) diff += step >> 1;
    if (nibble & 1) diff += step >> 2;
    int32_t pred = st->predictor;
    pred += (nibble & 8) ? -diff : diff;
    if (pred > 32767) pred = 32767;
    if (pred < -32768) pred = -32768;
    st->predictor = (int16_t)pred;
    int idx = st->step_index + k_index_table[nibble & 7];
    if (idx < 0) idx = 0;
    if (idx > 88) idx = 88;
    st->step_index = (uint8_t)idx;
    return st->predictor;
}

size_t ima_decode(const ima_state_t *st_in, const uint8_t *data, size_t len,
                  int16_t *out, size_t out_max, ima_state_t *st_out)
{
    ima_state_t local;
    if (st_in) {
        local = *st_in;
    } else {
        ima_state_reset(&local, 0, 0);
    }
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        if (n + 2 > out_max) break;
        out[n++] = ima_decode_nibble(&local, (uint8_t)(data[i] >> 4));
        out[n++] = ima_decode_nibble(&local, (uint8_t)(data[i] & 0x0F));
    }
    if (st_out) *st_out = local;
    return n;
}
