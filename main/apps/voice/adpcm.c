// main/adpcm.c -- IMA ADPCM 4:1 codec implementation (design and bit order: see adpcm.h).
// Pure functions, no global state, no dynamic allocation: round-trip and independence cases run directly in host tests.
#include "adpcm.h"

// Standard IMA ADPCM table (must match WAV / libsndfile / the Mac-side companion/adpcm.py)
static const int16_t k_step[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,
    19,    21,    23,    25,    28,    31,    34,    37,    41,    45,
    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,
    337,   371,   408,   449,   494,   544,   598,   658,   724,   796,
    876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,
    5894,  6484,  7132,  7845,  8630,  9493,  10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
};
static const int8_t k_index_delta[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8,
};

#define INDEX_MAX 88

static inline int clamp_index(int idx) {
    if (idx < 0) return 0;
    if (idx > INDEX_MAX) return INDEX_MAX;
    return idx;
}

// int16 saturation: ADPCM reconstructed values may overflow and must be clamped (otherwise they wrap into a phase-inverted pop)
static inline int16_t clamp_s16(int32_t v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

void adpcm_state_reset(adpcm_state_t *st) {
    if (!st) return;
    st->predictor = 0;
    st->index = 0;
}

// Single sample -> 4-bit code, advancing predictor/index in place (encode and decode must use the same reconstruction formula,
// otherwise encoder and decoder predictors diverge -> accumulating distortion).
static uint8_t encode_sample(adpcm_state_t *st, int16_t sample) {
    int step = k_step[st->index];
    int32_t diff = (int32_t)sample - (int32_t)st->predictor;
    uint8_t code = 0;
    if (diff < 0) {
        code = 8;          // sign bit
        diff = -diff;
    }
    int32_t tmp = step;
    if (diff >= tmp) { code |= 4; diff -= tmp; }
    tmp >>= 1;
    if (diff >= tmp) { code |= 2; diff -= tmp; }
    tmp >>= 1;
    if (diff >= tmp) { code |= 1; }

    // Reconstruction mirrors the decoder step by step
    int32_t diffq = step >> 3;
    if (code & 4) diffq += step;
    if (code & 2) diffq += step >> 1;
    if (code & 1) diffq += step >> 2;
    int32_t pred = (int32_t)st->predictor + ((code & 8) ? -diffq : diffq);
    st->predictor = clamp_s16(pred);
    st->index = (uint8_t)clamp_index((int)st->index + k_index_delta[code]);
    return code;
}

static int16_t decode_sample(adpcm_state_t *st, uint8_t code) {
    int step = k_step[st->index];
    int32_t diffq = step >> 3;
    if (code & 4) diffq += step;
    if (code & 2) diffq += step >> 1;
    if (code & 1) diffq += step >> 2;
    int32_t pred = (int32_t)st->predictor + ((code & 8) ? -diffq : diffq);
    st->predictor = clamp_s16(pred);
    st->index = (uint8_t)clamp_index((int)st->index + k_index_delta[code]);
    return st->predictor;
}

size_t adpcm_encode_block(adpcm_state_t *st, const int16_t *pcm, size_t samples,
                          uint8_t *out, size_t out_cap) {
    if (!st || !pcm || !out || samples == 0) return 0;
    const size_t need = ADPCM_HEADER_BYTES + samples / 2;
    if (out_cap < need) return 0;

    // Block header: first sample raw + current adaptive index. The predictor is reset to the raw value each block --
    // (1) the decoder is self-contained without the previous block (2) cross-block error does not accumulate.
    st->predictor = pcm[0];
    out[0] = (uint8_t)((uint16_t)pcm[0] & 0xFF);
    out[1] = (uint8_t)(((uint16_t)pcm[0] >> 8) & 0xFF);
    out[2] = st->index;
    out[3] = 0;   // reserved: set to 0, the decoder ignores it

    size_t w = ADPCM_HEADER_BYTES;
    // The first sample is already in the header; the data area starts at the 2nd sample; low nibble first.
    // With an odd number of samples to encode, the last byte's high nibble stays 0 (the decoder counts by samples and never reads it).
    for (size_t i = 1; i < samples; i++) {
        uint8_t code = encode_sample(st, pcm[i]);
        if ((i & 1) == 1) {
            out[w] = (uint8_t)(code & 0x0F);        // low nibble: written first, placeholder
            if (i + 1 == samples) w++;              // tail: high nibble stays 0
        } else {
            out[w] = (uint8_t)(out[w] | (uint8_t)(code << 4));
            w++;
        }
    }
    return need;
}

size_t adpcm_decode_block(const uint8_t *in, size_t in_len, size_t samples,
                          int16_t *pcm, size_t pcm_cap) {
    if (!in || !pcm || samples == 0) return 0;
    const size_t need = ADPCM_HEADER_BYTES + samples / 2;
    if (in_len < need || pcm_cap < samples) return 0;

    adpcm_state_t st;
    st.predictor = (int16_t)(uint16_t)((uint16_t)in[0] | ((uint16_t)in[1] << 8));
    st.index = (uint8_t)clamp_index(in[2]);   // clamp an out-of-range header index, never read past the table
    pcm[0] = st.predictor;

    size_t r = ADPCM_HEADER_BYTES;
    for (size_t i = 1; i < samples; i++) {
        uint8_t code;
        if ((i & 1) == 1) {
            code = (uint8_t)(in[r] & 0x0F);
        } else {
            code = (uint8_t)((in[r] >> 4) & 0x0F);
            r++;
        }
        pcm[i] = decode_sample(&st, code);
    }
    return samples;
}
