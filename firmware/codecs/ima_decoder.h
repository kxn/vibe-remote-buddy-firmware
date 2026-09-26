/* IMA ADPCM decoder (ATVV 1.0: 4:1, high nibble first, 16-bit output).
 * Portable C99. */
#ifndef IMA_DECODER_H
#define IMA_DECODER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int16_t predictor;
    uint8_t step_index; /* 0..88 */
} ima_state_t;

/* Decode nibble stream (high nibble first) into samples.
 * Returns number of samples written (2 per byte max).
 * If state is NULL an internal default state (0/0) is used; ATVV streams
 * must be initialized from AUDIO_SYNC instead of guessing. */
size_t ima_decode(const ima_state_t *st_in, const uint8_t *data, size_t len,
                  int16_t *out, size_t out_max, ima_state_t *st_out);

/* Result of decoding an explicit nibble (for tests). */
int16_t ima_decode_nibble(ima_state_t *st, uint8_t nibble);

void ima_state_reset(ima_state_t *st, int16_t predictor, uint8_t step_index);

#ifdef __cplusplus
}
#endif
#endif
