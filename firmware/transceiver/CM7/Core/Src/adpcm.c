#include <stdint.h>

// IMA ADPCM Step Table
const int16_t step_table[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
    19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

const int8_t index_table[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8
};

typedef struct {
    int16_t predicted_sample;
    int8_t step_index;
} adpcm_state_t;

// Encode a single 16-bit sample to 4-bit ADPCM
uint8_t adpcm_encode_sample(int16_t sample, adpcm_state_t *state) {
    int32_t diff = sample - state->predicted_sample;
    int32_t step = step_table[state->step_index];
    uint8_t code = 0;

    if (diff < 0) {
        code = 8;
        diff = -diff;
    }
    if (diff >= step) {
        code |= 4;
        diff -= step;
    }
    step >>= 1;
    if (diff >= step) {
        code |= 2;
        diff -= step;
    }
    step >>= 1;
    if (diff >= step) {
        code |= 1;
    }

    // Update state
    int32_t diffq = (step_table[state->step_index] * (code & 7)) / 4 + (step_table[state->step_index] / 8);
    if (code & 8) diffq = -diffq;

    state->predicted_sample += diffq;
    
    // Clamp sample
    if (state->predicted_sample > 32767) state->predicted_sample = 32767;
    else if (state->predicted_sample < -32768) state->predicted_sample = -32768;

    state->step_index += index_table[code];
    
    // Clamp step index
    if (state->step_index < 0) state->step_index = 0;
    else if (state->step_index > 88) state->step_index = 88;

    return code;
}

// Decode a single 4-bit nibble to 16-bit PCM
int16_t adpcm_decode_sample(uint8_t code, adpcm_state_t *state) {
    int32_t step = step_table[state->step_index];
    int32_t diffq = step >> 3;

    if (code & 4) diffq += step;
    if (code & 2) diffq += step >> 1;
    if (code & 1) diffq += step >> 2;

    if (code & 8) state->predicted_sample -= diffq;
    else state->predicted_sample += diffq;

    // Clamp
    if (state->predicted_sample > 32767) state->predicted_sample = 32767;
    else if (state->predicted_sample < -32768) state->predicted_sample = -32768;

    state->step_index += index_table[code];
    
    if (state->step_index < 0) state->step_index = 0;
    else if (state->step_index > 88) state->step_index = 88;

    return state->predicted_sample;
}