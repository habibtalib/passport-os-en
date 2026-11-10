// main/adpcm.h -- IMA ADPCM 4:1 codec (compression for the BLE audio channel).
//
// Why ADPCM and not Opus: the real BLE bottleneck is the mbuf pool (4x256B) plus "first fragment
// fails -> whole frame discarded", not air bandwidth (see task design.md section 3). With the bottleneck fixed, 8KB/s needs only
// about 0.7 packets per connection event; Opus's extra compression is of no use, yet it needs 38.5KB of static state +
// 16KB of stack -- exactly the heap needed to fix the bottleneck. ADPCM state is 4 bytes, no heap, no alloca.
//
// Each block decodes independently: the block header carries the predictor (the block's first sample, raw) and the adaptive index,
// the decoder depends only on the header, not on the previous block -- losing a block costs only that 100ms and does not pollute later ones.
// The encoder's index carries across blocks (for sound quality only: avoids re-climbing from step_table[0] each block),
// but since it is snapshotted into the header, independence is unaffected; the predictor is reset to the first sample's raw value each block,
// which also removes cross-block error accumulation.
//
// Nibble order: low nibble first (same as WAV IMA ADPCM), so the Mac-side Python decoder lines up.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ADPCM_HEADER_BYTES   4      // [int16 LE predictor][uint8 index][uint8 rsv=0]
#define ADPCM_BLOCK_SAMPLES  1600   // 100ms @ 16kHz(= AUDIO_FRAME_BYTES / 2)
// The first sample goes in the header raw, the remaining samples-1 take 4 bits each (for an odd count the last byte's high nibble is 0)
#define ADPCM_BLOCK_BYTES    (ADPCM_HEADER_BYTES + (ADPCM_BLOCK_SAMPLES / 2))   // 804

// Encoder adaptive state (4 bytes, fine on the stack; decoding needs no cross-block state)
typedef struct {
    int16_t predictor;   // last reconstructed sample value
    uint8_t index;       // step_table index (0..88)
} adpcm_state_t;

// Reset to the initial adaptive state. Call at session start (do not carry over the previous session's silence adaptation).
void adpcm_state_reset(adpcm_state_t *st);

// Encode one block. samples must be >=1; returns 0 if out_cap is too small or arguments are invalid (never writes out of bounds).
// Returns bytes written to out = ADPCM_HEADER_BYTES + samples/2 (exact when samples is even).
// On return st carries the adaptive index at the end of this block, for the next block to continue.
size_t adpcm_encode_block(adpcm_state_t *st, const int16_t *pcm, size_t samples,
                          uint8_t *out, size_t out_cap);

// Decode one block. samples = the block's original sample count (fixed by the protocol: ADPCM_BLOCK_SAMPLES).
// Returns 0 if in_len / pcm_cap are too small or arguments are invalid. Returns the number of samples written to pcm.
// Needs no external state: predictor/index all come from the block header -- each block is self-contained.
size_t adpcm_decode_block(const uint8_t *in, size_t in_len, size_t samples,
                          int16_t *pcm, size_t pcm_cap);

#ifdef __cplusplus
}
#endif
