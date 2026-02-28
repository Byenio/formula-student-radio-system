#ifndef ADPCM_H
#define ADPCM_H

#include <stdint.h>

typedef struct {
  int16_t predicted_sample;
  int8_t step_index;
} adpcm_state_t;

uint8_t adpcm_encode_sample(int16_t sample, adpcm_state_t *state);
int16_t adpcm_decode_sample(uint8_t code, adpcm_state_t *state);

#endif