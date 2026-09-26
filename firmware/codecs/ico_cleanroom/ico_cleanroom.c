/*
 * Independent decoder written from the approved profile and normative
 * G.722.1 equations. The only package-derived inputs are numeric arrays in
 * itu_g7221_numeric_tables.h; synthesis kernels are generated from section
 * 4.7's equations by generate_math_tables.py.
 */
#include "ico_cleanroom.h"

#include <limits.h>
#include <string.h>

#include "ico_dct_iv_q15.h"
#include <ico_huffman_lookup.h>
#include <itu_g7221_numeric_tables.h>

typedef struct {
    uint16_t words[20];
    unsigned bit;
} bit_reader;

static const uint8_t ico_permutation[20] = {
    0, 1, 18, 8, 9, 5, 2, 17, 11, 16,
    10, 3, 12, 7, 14, 15, 4, 13, 6, 19
};

static const uint8_t *const code_bits[7] = {
    itu_mlt_sqvh_bitcount_category_0, itu_mlt_sqvh_bitcount_category_1,
    itu_mlt_sqvh_bitcount_category_2, itu_mlt_sqvh_bitcount_category_3,
    itu_mlt_sqvh_bitcount_category_4, itu_mlt_sqvh_bitcount_category_5,
    itu_mlt_sqvh_bitcount_category_6
};

static const uint16_t *const code_words[7] = {
    itu_mlt_sqvh_code_category_0, itu_mlt_sqvh_code_category_1,
    itu_mlt_sqvh_code_category_2, itu_mlt_sqvh_code_category_3,
    itu_mlt_sqvh_code_category_4, itu_mlt_sqvh_code_category_5,
    itu_mlt_sqvh_code_category_6
};

static const uint16_t code_count[7] = {196, 100, 49, 625, 256, 243, 32};
static const uint16_t noise_q15[3] = {5793, 8192, 23170};
/* Restore the ICO profile's seven-index envelope adjustment in Q10. */
#define ICO_RMS_RESTORE_Q10 11585

static int peek_bits(const bit_reader *br, unsigned count, unsigned *value)
{
    unsigned word, offset, shift, v;
    uint32_t pair;
    if (count > 16 || br->bit + count > 320) return 0;
    if (count == 0) { *value = 0; return 1; }
    word = br->bit >> 4;
    offset = br->bit & 15u;
    pair = (uint32_t)br->words[word] << 16;
    if (offset + count > 16u) pair |= br->words[word + 1u];
    shift = 32u - offset - count;
    v = (pair >> shift) & ((1u << count) - 1u);
    *value = v;
    return 1;
}

static int read_bits(bit_reader *br, unsigned count, unsigned *value)
{
    if (!peek_bits(br, count, value)) return 0;
    br->bit += count;
    return 1;
}

static int decode_symbol(bit_reader *br, const uint16_t *codes,
                         const uint8_t *lengths, unsigned count,
                         unsigned max_length, const ico_vq_lookup_table *lookup,
                         unsigned *symbol)
{
    unsigned code = 0;
    unsigned length;
    if (lookup) {
        unsigned prefix;
        if (peek_bits(br, 10, &prefix)) {
            uint16_t entry = lookup->short_code[prefix];
            if (entry != UINT16_MAX) {
                unsigned used = entry >> 10;
                *symbol = entry & 0x03ffu;
                br->bit += used;
                return 1;
            }
            {
                unsigned start = lookup->bucket_offsets[prefix];
                unsigned end = lookup->bucket_offsets[prefix + 1u];
                br->bit += 10u;
                code = prefix;
                for (length = 11; length <= max_length; ++length) {
                    unsigned bit;
                    unsigned i;
                    if (!read_bits(br, 1, &bit)) return 0;
                    code = (code << 1) | bit;
                    for (i = start; i < end; ++i) {
                        unsigned candidate = lookup->bucket_symbols[i];
                        if (lengths[candidate] == length && codes[candidate] == code) {
                            *symbol = candidate;
                            return 1;
                        }
                    }
                }
                return -1;
            }
        }
    }
    for (length = 1; length <= max_length; ++length) {
        unsigned bit;
        unsigned i;
        if (!read_bits(br, 1, &bit)) return 0;
        code = (code << 1) | bit;
        for (i = 0; i < count; ++i) {
            if (lengths[i] == length && codes[i] == code) {
                *symbol = i;
                return 1;
            }
        }
    }
    return -1;
}

static unsigned expected_bits(const uint8_t category[14])
{
    unsigned total = 0;
    unsigned r;
    for (r = 0; r < 14; ++r) total += itu_expected_bits_table[category[r]];
    return total;
}

static int trunc_div2(int value)
{
    return value / 2;
}

static int32_t reconstructed_standard_deviation(int rms)
{
    unsigned index = (unsigned)(rms + 33);
    if (index < 54u)
        return (int32_t)itu_int_region_standard_deviation_table[index];

    /* The transmitted envelope may extend seven half-step indices beyond
     * the direct table. Restore sqrt(2^7) from the adjusted in-range entry. */
    return (int32_t)(((int64_t)itu_int_region_standard_deviation_table[rms + 26]
        * ICO_RMS_RESTORE_Q10 + 512) >> 10);
}

static void allocate_categories(unsigned available, const int rms[14],
                                unsigned selected, uint8_t result[14])
{
    int offset = -32;
    int delta = 32;
    uint8_t high[14], low[14], balances[32];
    unsigned high_bits, low_bits;
    int high_pointer = 16, low_pointer = 16;
    unsigned r, n;

    while (delta > 0) {
        uint8_t trial[14];
        unsigned bits;
        for (r = 0; r < 14; ++r) {
            int cat = trunc_div2(offset + delta - rms[r]);
            if (cat < 0) cat = 0;
            if (cat > 7) cat = 7;
            trial[r] = (uint8_t)cat;
        }
        bits = expected_bits(trial);
        if ((int)bits >= (int)available - 32) offset += delta;
        delta >>= 1;
    }

    for (r = 0; r < 14; ++r) {
        int cat = trunc_div2(offset - rms[r]);
        if (cat < 0) cat = 0;
        if (cat > 7) cat = 7;
        high[r] = low[r] = (uint8_t)cat;
    }
    high_bits = low_bits = expected_bits(high);

    for (n = 1; n < 16; ++n) {
        if (high_bits + low_bits <= 2u * available) {
            int chosen = -1;
            int best = INT_MAX;
            for (r = 0; r < 14; ++r) {
                int score;
                if (high[r] == 0) continue;
                score = offset - rms[r] - 2 * (int)high[r];
                if (score < best) { best = score; chosen = (int)r; }
            }
            if (chosen < 0) break;
            --high_pointer;
            balances[high_pointer] = (uint8_t)chosen;
            high_bits -= itu_expected_bits_table[high[chosen]];
            --high[chosen];
            high_bits += itu_expected_bits_table[high[chosen]];
        } else {
            int chosen = -1;
            int best = INT_MIN;
            for (r = 0; r < 14; ++r) {
                int score;
                if (low[r] == 7) continue;
                score = offset - rms[r] - 2 * (int)low[r];
                if (score >= best) { best = score; chosen = (int)r; }
            }
            if (chosen < 0) break;
            balances[low_pointer++] = (uint8_t)chosen;
            low_bits -= itu_expected_bits_table[low[chosen]];
            ++low[chosen];
            low_bits += itu_expected_bits_table[low[chosen]];
        }
    }

    memcpy(result, high, 14);
    for (n = 1; n <= selected; ++n) {
        ++result[balances[high_pointer + (int)n - 1]];
    }
}

static uint16_t next_random_word(ico_cleanroom_decoder *d)
{
    uint16_t next = (uint16_t)(d->random_seed[0] + d->random_seed[3]);
    uint16_t old0 = d->random_seed[0];
    uint16_t old1 = d->random_seed[1];
    uint16_t old2 = d->random_seed[2];
    if (next & 0x8000u) ++next;
    d->random_seed[0] = next;
    d->random_seed[1] = old0;
    d->random_seed[2] = old1;
    d->random_seed[3] = old2;
    return next;
}

static int random_sign(ico_cleanroom_decoder *d)
{
    int sign;
    if (d->random_bits_left == 0) {
        d->random_word = next_random_word(d);
        d->random_bits_left = 16;
    }
    sign = (d->random_word & 1u) ? 1 : -1;
    d->random_word >>= 1;
    --d->random_bits_left;
    return sign;
}

static void begin_random_segment(ico_cleanroom_decoder *d)
{
    d->random_word = next_random_word(d);
    d->random_bits_left = 16;
}

static void discard_random_segment(ico_cleanroom_decoder *d)
{
    d->random_word = 0;
    d->random_bits_left = 0;
}

static int64_t shift_q15(int64_t value)
{
    if (value >= 0) return value / 32768;
    return -((-value + 32767) / 32768);
}

static int64_t rounded_shift(int64_t value, unsigned bits)
{
    uint64_t magnitude;
    uint64_t half;
    if (bits == 0) return value;
    magnitude = value < 0 ? (uint64_t)(-(value + 1)) + 1u : (uint64_t)value;
    half = UINT64_C(1) << (bits - 1u);
    magnitude = (magnitude + half) >> bits;
    return value < 0 ? -(int64_t)magnitude : (int64_t)magnitude;
}

typedef struct { int32_t re, im; } ico_complex_i32;

static int32_t q15_product_32(int32_t value, int16_t coefficient)
{
    int64_t result = rounded_shift((int64_t)value * coefficient, 15);
    if (result > INT32_MAX) return INT32_MAX;
    if (result < INT32_MIN) return INT32_MIN;
    return (int32_t)result;
}

static ico_complex_i32 complex_product_q15(ico_complex_i32 value,
                                            ico_complex_q15 coefficient)
{
    ico_complex_i32 result;
    int64_t re = (int64_t)value.re * coefficient.re
               - (int64_t)value.im * coefficient.im;
    int64_t im = (int64_t)value.re * coefficient.im
               + (int64_t)value.im * coefficient.re;
    re = rounded_shift(re, 15);
    im = rounded_shift(im, 15);
    if (re > INT32_MAX) re = INT32_MAX;
    if (re < INT32_MIN) re = INT32_MIN;
    if (im > INT32_MAX) im = INT32_MAX;
    if (im < INT32_MIN) im = INT32_MIN;
    result.re = (int32_t)re;
    result.im = (int32_t)im;
    return result;
}

static unsigned reverse_7(unsigned value)
{
    return ((value & 0x01u) << 6) | ((value & 0x02u) << 4)
         | ((value & 0x04u) << 2) | (value & 0x08u)
         | ((value & 0x10u) >> 2) | ((value & 0x20u) >> 4)
         | ((value & 0x40u) >> 6);
}

/* In-place DIF FFT for one decimated 128-point subsequence. */
static void fft_128_dif(ico_complex_i32 values[128])
{
    unsigned span;
    for (span = 128; span >= 2; span >>= 1) {
        unsigned base;
        for (base = 0; base < 128u; base += span) {
            unsigned j;
            for (j = 0; j < span / 2u; ++j) {
                ico_complex_i32 a = values[base + j];
                ico_complex_i32 b = values[base + j + span / 2u];
                int64_t sum_re = (int64_t)a.re + b.re;
                int64_t sum_im = (int64_t)a.im + b.im;
                int64_t diff_re = (int64_t)a.re - b.re;
                int64_t diff_im = (int64_t)a.im - b.im;
                unsigned twiddle = (1280u / span) * j;
                ico_complex_i32 difference;
                ico_complex_i32 rotated;
                values[base + j].re = (int32_t)sum_re;
                values[base + j].im = (int32_t)sum_im;
                difference.re = (int32_t)diff_re;
                difference.im = (int32_t)diff_im;
                if (twiddle == 0u) {
                    rotated = difference;
                } else if (twiddle == 320u) {
                    rotated.re = -difference.im;
                    rotated.im = difference.re;
                } else if (twiddle == 160u) {
                    rotated.re = q15_product_32(
                        difference.re - difference.im, 23170);
                    rotated.im = q15_product_32(
                        difference.re + difference.im, 23170);
                } else if (twiddle == 480u) {
                    rotated.re = -q15_product_32(
                        difference.re + difference.im, 23170);
                    rotated.im = q15_product_32(
                        difference.re - difference.im, 23170);
                } else {
                    rotated = complex_product_q15(
                        difference, ico_dct_iv_fft_twiddle[twiddle]);
                }
                values[base + j + span / 2u] = rotated;
            }
        }
    }
}

/* Evaluate the 320-point DCT-IV from its definition using a zero-padded
 * 640-point complex FFT. The factorization and constants are generated from
 * the normative cosine equation; no decoder implementation is imported. */
static void dct_iv_fast(int32_t work[320])
{
    ico_complex_i32 fft[128];
    int64_t combined_q15[320];
    int64_t maximum = 0;
    unsigned input_shift = 0;
    unsigned n, l;

    for (n = 0; n < 280; ++n) {
        int64_t value = work[n];
        if (value < 0) value = -value;
        if (value > maximum) maximum = value;
    }
    while ((maximum >> input_shift) > 10000) ++input_shift;
    memset(combined_q15, 0, sizeof(combined_q15));
    /* The mixed-radix factorization uses five 128-point FFTs. Process each
     * sequence in a compact work area and accumulate its output immediately. */
    for (l = 0; l < 5; ++l) {
        memset(fft, 0, sizeof(fft));
        for (n = 0; n < 64; ++n) {
            unsigned source = 5u * n + l;
            int64_t value = rounded_shift(work[source], input_shift);
            const ico_complex_q15 phase = ico_dct_iv_fft_twiddle[source];
            fft[n].re = q15_product_32((int32_t)value, phase.re);
            fft[n].im = q15_product_32((int32_t)value, phase.im);
        }
        fft_128_dif(fft);
        for (n = 0; n < 320; ++n) {
            unsigned reversed = reverse_7(n % 128u);
            const ico_complex_i32 x = fft[reversed];
            const ico_complex_q15 w = ico_dct_iv_final_twiddle[5u * n + l];
            combined_q15[n] += (int64_t)x.re * w.re - (int64_t)x.im * w.im;
        }
    }

    /* The seven radix-2 stages divided the transform by 128. Restore that
     * scale together with sqrt(2/320), then restore the input block shift. */
    for (n = 0; n < 320; ++n) {
        {
            int64_t scaled = rounded_shift(combined_q15[n] * 2590, 30);
            int64_t restored = scaled * (INT64_C(1) << input_shift);
            if (restored > INT32_MAX) restored = INT32_MAX;
            if (restored < INT32_MIN) restored = INT32_MIN;
            work[n] = (int32_t)restored;
        }
    }
}

static void store_previous_spectrum(ico_cleanroom_decoder *d,
                                    const int32_t spectrum[280])
{
    unsigned region;
    for (region = 0; region < 14; ++region) {
        unsigned i;
        int64_t maximum = 0;
        unsigned shift = 0;
        unsigned base = region * 20u;
        for (i = 0; i < 20; ++i) {
            int64_t value = spectrum[base + i];
            if (value < 0) value = -value;
            if (value > maximum) maximum = value;
        }
        while ((maximum >> shift) > INT16_MAX) ++shift;
        d->spectrum_shift[region] = (uint8_t)shift;
        for (i = 0; i < 20; ++i) {
            int64_t value = rounded_shift(spectrum[base + i], shift);
            if (value > INT16_MAX) value = INT16_MAX;
            if (value < INT16_MIN) value = INT16_MIN;
            d->previous_spectrum[base + i] = (int16_t)value;
        }
    }
}

static void load_previous_spectrum(const ico_cleanroom_decoder *d,
                                   int32_t spectrum[320])
{
    unsigned i;
    memset(spectrum, 0, 320u * sizeof(spectrum[0]));
    for (i = 0; i < 280; ++i) {
        unsigned shift = d->spectrum_shift[i / 20u];
        spectrum[i] = (int32_t)((int64_t)d->previous_spectrum[i]
                                * (INT64_C(1) << shift));
    }
}

static void store_overlap(ico_cleanroom_decoder *d, const int32_t overlap[160])
{
    int64_t maximum = 0;
    unsigned shift = 0;
    unsigned i;
    for (i = 0; i < 160; ++i) {
        int64_t value = overlap[i];
        if (value < 0) value = -value;
        if (value > maximum) maximum = value;
    }
    while ((maximum >> shift) > INT16_MAX) ++shift;
    d->overlap_shift = (uint8_t)shift;
    for (i = 0; i < 160; ++i) {
        int64_t value = rounded_shift(overlap[i], shift);
        if (value > INT16_MAX) value = INT16_MAX;
        if (value < INT16_MIN) value = INT16_MIN;
        d->overlap[i] = (int16_t)value;
    }
}

static void synthesize(ico_cleanroom_decoder *d, int32_t work[320],
                       int16_t pcm[320])
{
    unsigned n;
    dct_iv_fast(work);

    for (n = 0; n < 160; ++n) {
        int64_t old_n = (int64_t)d->overlap[n] * (INT64_C(1) << d->overlap_shift);
        int64_t old_mirror = (int64_t)d->overlap[159u - n]
                           * (INT64_C(1) << d->overlap_shift);
        int64_t a = (int64_t)ico_dct_iv_q15_window[n] * work[159u - n]
                  + (int64_t)ico_dct_iv_q15_window[319u - n] * old_n;
        int64_t b = (int64_t)ico_dct_iv_q15_window[160u + n] * work[n]
                  - (int64_t)ico_dct_iv_q15_window[159u - n] * old_mirror;
        int64_t ya = shift_q15(a);
        int64_t yb = shift_q15(b);
        if (ya > INT16_MAX) ya = INT16_MAX;
        if (ya < INT16_MIN) ya = INT16_MIN;
        if (yb > INT16_MAX) yb = INT16_MAX;
        if (yb < INT16_MIN) yb = INT16_MIN;
        pcm[n] = (int16_t)((uint16_t)(int16_t)ya & 0xfffcu);
        pcm[n + 160u] = (int16_t)((uint16_t)(int16_t)yb & 0xfffcu);
    }
    store_overlap(d, work + 160);
}

static void conceal_spectrum(ico_cleanroom_decoder *d, int32_t spectrum[320])
{
    if (!d->previous_was_erasure)
        load_previous_spectrum(d, spectrum);
    else
        memset(spectrum, 0, 320u * sizeof(spectrum[0]));
    memset(d->previous_spectrum, 0, sizeof(d->previous_spectrum));
    memset(d->spectrum_shift, 0, sizeof(d->spectrum_shift));
    d->previous_was_erasure = 1;
}

void ico_cleanroom_restart(ico_cleanroom_decoder *decoder)
{
    if (!decoder) return;
    memset(decoder, 0, sizeof(*decoder));
    decoder->random_seed[0] = decoder->random_seed[1] = 1;
    decoder->random_seed[2] = decoder->random_seed[3] = 1;
}

size_t ico_cleanroom_conceal(ico_cleanroom_decoder *decoder,
                             int16_t pcm[ICO_CLEANROOM_SAMPLES])
{
    int32_t spectrum[320];
    if (!decoder || !pcm) return 0;
    conceal_spectrum(decoder, spectrum);
    synthesize(decoder, spectrum, pcm);
    return ICO_CLEANROOM_SAMPLES;
}

size_t ico_cleanroom_decode(ico_cleanroom_decoder *decoder,
                            const uint8_t frame[ICO_CLEANROOM_FRAME_BYTES],
                            size_t frame_bytes,
                            int16_t pcm[ICO_CLEANROOM_SAMPLES])
{
    bit_reader br;
    int rms[14];
    uint8_t categories[14];
    int32_t spectrum[320];
    unsigned raw, control, r, j, vector, available;
    int malformed = 0;
    int underflow = 0;

    if (!decoder || !frame || !pcm || frame_bytes != ICO_CLEANROOM_FRAME_BYTES) return 0;
    memset(&br, 0, sizeof(br));
    for (r = 0; r < 20; ++r) {
        unsigned src = ico_permutation[r];
        uint16_t word = (uint16_t)(frame[2u * src] | ((uint16_t)frame[2u * src + 1u] << 8));
        br.words[r] = (uint16_t)(word ^ 0x0416u);
    }
    memset(spectrum, 0, sizeof(spectrum));

    if (!read_bits(&br, 5, &raw) || raw == 0 || raw > 31) malformed = 1;
    if (!malformed) {
        rms[0] = (int)raw - 7;
        for (r = 1; r < 14; ++r) {
            unsigned symbol;
            int found = decode_symbol(&br,
                itu_differential_region_power_codes + r * 24u,
                itu_differential_region_power_bits + r * 24u,
                24, 16, NULL, &symbol);
            if (found <= 0) { malformed = 1; break; }
            rms[r] = rms[r - 1] + (int)symbol - 12;
        }
    }
    if (!malformed) {
        if (rms[0] < -6 || rms[0] > 24) malformed = 1;
        for (r = 1; r < 14; ++r)
            if (rms[r] < -15 || rms[r] > 24) malformed = 1;
    }
    if (!malformed && !read_bits(&br, 4, &control)) malformed = 1;
    if (malformed) return ico_cleanroom_conceal(decoder, pcm);

    available = 320u - br.bit;
    allocate_categories(available, rms, control, categories);
    memset(spectrum, 0, sizeof(spectrum));

    for (r = 0; r < 14; ++r) {
        unsigned category = categories[r];
        unsigned dim, vectors, base = r * 20u;
        uint8_t quant_zero[20];
        memset(quant_zero, 1, sizeof(quant_zero));
        if (category < 7) {
            dim = itu_vector_dimension[category];
            vectors = itu_number_of_vectors[category];
            for (vector = 0; vector < vectors; ++vector) {
                unsigned index = 0;
                uint8_t magnitudes[5];
                int found = decode_symbol(&br, code_words[category], code_bits[category],
                                          code_count[category], 16,
                                          &ico_vq_lookup[category], &index);
                if (found == 0) {
                    unsigned k;
                    for (k = base; k < base + 20u; ++k) spectrum[k] = 0;
                    underflow = 1;
                    category = 7;
                    break;
                }
                if (found < 0) { malformed = 1; break; }
                for (j = 0; j < dim; ++j) {
                    unsigned vmax = (unsigned)itu_max_bin[category] + 1u;
                    unsigned q;
                    /* The radix power is deliberately formed iteratively. */
                    unsigned radix_power = 1;
                    for (q = 0; q < j; ++q) radix_power *= vmax;
                    magnitudes[j] = (uint8_t)((index / radix_power) % vmax);
                }
                /* Signs follow increasing spectral-coefficient order. */
                for (j = 0; j < dim; ++j) {
                    unsigned digit = dim - j - 1u;
                    unsigned magnitude = magnitudes[digit];
                    unsigned position = base + (vector + 1u) * dim - digit - 1u;
                    if (magnitude != 0) {
                        unsigned sign;
                        int64_t product;
                        if (!read_bits(&br, 1, &sign)) {
                            unsigned k;
                            for (k = base; k < base + 20u; ++k) spectrum[k] = 0;
                            underflow = 1;
                            category = 7;
                            break;
                        }
                        /* The centroid table is Q12. Keep high-energy
                         * coefficients in 32 bits until the inverse MLT. */
                        product = (int64_t)reconstructed_standard_deviation(rms[r])
                                * itu_mlt_quant_centroid[category * 16u + magnitude];
                        spectrum[position] = (int32_t)(product >> 12);
                        if (!sign) spectrum[position] = -spectrum[position];
                        quant_zero[position - base] = (uint8_t)(spectrum[position] == 0);
                    }
                    if (category == 7) break;
                }
                if (malformed) break;
                if (category == 7) break;
            }
        }
        if (malformed) break;

        if (category == 5 || category == 6 || category == 7) {
            unsigned segment;
            int32_t amplitude = (int32_t)(((int64_t)noise_q15[category - 5u]
                * reconstructed_standard_deviation(rms[r])) >> 15);
            for (segment = 0; segment < 2; ++segment) {
                unsigned i;
                begin_random_segment(decoder);
                for (i = base + segment * 10u; i < base + (segment + 1u) * 10u; ++i) {
                    /* Categories 5/6 consume a sign bit only for a zero
                     * reconstructed coefficient. Category 7 fills all bins. */
                    if (category == 7 || quant_zero[i - base])
                        spectrum[i] = amplitude * random_sign(decoder);
                }
                discard_random_segment(decoder);
            }
        }
        if (underflow && r + 1u < 14u) {
            unsigned k;
            category = 7;
            for (k = r + 1u; k < 14u; ++k) categories[k] = 7;
        }
    }

    if (!malformed) {
        while (br.bit < 320u) {
            unsigned tail;
            if (!read_bits(&br, 1, &tail) || tail == 0) { malformed = 1; break; }
        }
    }
    if (malformed) {
        return ico_cleanroom_conceal(decoder, pcm);
    }

    store_previous_spectrum(decoder, spectrum);
    decoder->previous_was_erasure = 0;
    synthesize(decoder, spectrum, pcm);
    return ICO_CLEANROOM_SAMPLES;
}
