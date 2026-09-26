/* Independent ICO / G.722.1 compatibility decoder. */
#ifndef ICO_CLEANROOM_H
#define ICO_CLEANROOM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ICO_CLEANROOM_FRAME_BYTES 40u
#define ICO_CLEANROOM_SAMPLES 320u

typedef struct {
    int16_t overlap[160];
    int16_t previous_spectrum[280];
    uint8_t spectrum_shift[14];
    uint8_t overlap_shift;
    uint16_t random_seed[4];
    uint16_t random_word;
    uint8_t random_bits_left;
    uint8_t previous_was_erasure;
} ico_cleanroom_decoder;

void ico_cleanroom_restart(ico_cleanroom_decoder *decoder);

/* Returns 320 on success, 0 for an invalid call. Invalid calls preserve state. */
size_t ico_cleanroom_decode(ico_cleanroom_decoder *decoder,
                            const uint8_t frame[ICO_CLEANROOM_FRAME_BYTES],
                            size_t frame_bytes,
                            int16_t pcm[ICO_CLEANROOM_SAMPLES]);

/* Synthesize one externally signalled missing frame. */
size_t ico_cleanroom_conceal(ico_cleanroom_decoder *decoder,
                             int16_t pcm[ICO_CLEANROOM_SAMPLES]);

#ifdef __cplusplus
}
#endif

#endif
