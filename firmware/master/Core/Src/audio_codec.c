/**
  ******************************************************************************
  * @file    audio_codec.c
  * @brief   Mu-law companding and rate conversion. See audio_codec.h.
  ******************************************************************************
  */

#include "audio_codec.h"
#include <string.h>

/* The ADC gives unsigned 0..4095 centred on mid-scale, because the analogue
   front end biases the signal to VDD/2. Mu-law wants signed PCM centred on
   zero, so every sample is rebiased and scaled on the way in and back on the
   way out. Shifting by 4 maps 12-bit to the full 16-bit range mu-law expects,
   which keeps the companding curve operating where it was designed to. */
#define ADC_MIDSCALE        2048
#define ADC_TO_PCM_SHIFT    4

/* Standard G.711 constants. The bias is added before the exponent search so
   that small magnitudes still land in a sensible segment. */
#define MULAW_BIAS          0x84U
#define MULAW_CLIP          32635

/* ---- Decimation filter ---------------------------------------------------
   15-tap windowed-sinc (Hamming), 3.4 kHz cutoff at 16 kHz, in Q15. Taps sum
   to exactly 32768, so DC gain is unity and no level shift creeps in.

   Rejection where it matters: -35 dB at 5 kHz, -57 dB at 6 kHz, -63 dB at
   7 kHz. The 2-tap average this replaces managed -5, -8 and -14 dB. */
#define DECIM_TAPS          15U

/* ---- 300 Hz high-pass, Q14 --------------------------------------------
   Second-order Butterworth at the 8 kHz output rate, applied after
   decimation so it runs on 80 samples per frame rather than 160.
   Direct Form I with a 32-bit accumulator; Q14 rather than Q15 because a1
   exceeds 1.0 in magnitude and would not fit. */
#define HP_Q                14
#define HP_B0               13868
#define HP_B1              (-27737)
#define HP_B2               13868
#define HP_A1              (-27348)
#define HP_A2               11741

static const int16_t decim_taps[DECIM_TAPS] =
{
       9,    215,    202,   -922,  -1696,
    1955,   9667,  13908,   9667,   1955,
   -1696,   -922,    202,    215,      9
};

uint8_t AudioCodec_MuLawEncode(int16_t pcm)
{
  uint8_t sign = (pcm < 0) ? 0x80U : 0x00U;

  int32_t magnitude = (pcm < 0) ? -(int32_t)pcm : (int32_t)pcm;
  if (magnitude > MULAW_CLIP)
  {
    magnitude = MULAW_CLIP;
  }

  uint16_t biased = (uint16_t)(magnitude + (int32_t)MULAW_BIAS);

  /* Exponent is the index of the highest set bit, found by walking a mask
     down rather than with a 256-byte lookup table -- same result, less flash,
     and the loop runs at most eight times. */
  uint8_t  exponent = 7U;
  uint16_t mask     = 0x4000U;

  while (((biased & mask) == 0U) && (exponent > 0U))
  {
    exponent--;
    mask >>= 1;
  }

  uint8_t mantissa = (uint8_t)((biased >> (exponent + 3U)) & 0x0FU);

  /* The final complement is part of the standard: it makes silence encode to
     0xFF, so a dead or all-zero link sounds like loud noise rather than
     plausible quiet. That is a feature -- failures should be obvious. */
  return (uint8_t)(~(sign | (uint8_t)(exponent << 4) | mantissa));
}

int16_t AudioCodec_MuLawDecode(uint8_t ulaw)
{
  static const int16_t segment_base[8] =
  {
    0, 132, 396, 924, 1980, 4092, 8316, 16764
  };

  uint8_t u        = (uint8_t)~ulaw;
  uint8_t sign     = u & 0x80U;
  uint8_t exponent = (uint8_t)((u >> 4) & 0x07U);
  uint8_t mantissa = u & 0x0FU;

  int32_t sample = (int32_t)segment_base[exponent] +
                   ((int32_t)mantissa << (exponent + 3U));

  return (int16_t)(sign ? -sample : sample);
}

/* -------------------------------------------------------------------------- */

void AudioCodec_EncodeFrame(const uint16_t *adc_in, uint32_t n_in,
                            uint8_t *ulaw_out, uint32_t n_out)
{
  if (adc_in == NULL || ulaw_out == NULL || n_out == 0U || n_in < (n_out * 2U))
  {
    return;
  }

  /* Carried between frames so the filter sees a continuous stream rather than
     restarting at every 10 ms boundary -- restarting would put a small
     discontinuity into the audio 100 times a second. */
  static int16_t history[DECIM_TAPS - 1U];
  static int16_t work[(DECIM_TAPS - 1U) + AUDIO_CODEC_MAX_IN_SAMPLES];

  if (n_in > AUDIO_CODEC_MAX_IN_SAMPLES) { return; }

  /* Rebias to signed PCM once, up front, so the filter runs on centred data
     and the DC component cannot leak into the result. */
  memcpy(work, history, sizeof(history));

  for (uint32_t i = 0U; i < n_in; i++)
  {
    int32_t pcm = ((int32_t)adc_in[i] - ADC_MIDSCALE) << ADC_TO_PCM_SHIFT;

#if AUDIO_CODEC_MIC_GAIN_Q8 != 256
    /* Digital trim. Applied before the filter so that any clipping introduced
       here is at least band-limited afterwards rather than aliased. */
    pcm = (pcm * AUDIO_CODEC_MIC_GAIN_Q8) >> 8;

    if (pcm > 32767)  { pcm = 32767; }
    if (pcm < -32768) { pcm = -32768; }
#endif

    work[(DECIM_TAPS - 1U) + i] = (int16_t)pcm;
  }

  /* Keep the tail for the next frame's filter warm-up. */
  memcpy(history, &work[n_in], sizeof(history));

  for (uint32_t i = 0U; i < n_out; i++)
  {
    /* One output per two inputs: filter, then keep every second result. Only
       the samples that survive decimation are ever computed, so this costs
       15 multiply-accumulates per output, not per input. */
    int32_t acc = 0;

    for (uint32_t t = 0U; t < DECIM_TAPS; t++)
    {
      acc += (int32_t)decim_taps[t] * (int32_t)work[(2U * i) + t];
    }

    acc >>= 15;   /* undo the Q15 scaling */

    if (acc > 32767)  { acc = 32767; }
    if (acc < -32768) { acc = -32768; }

#if AUDIO_CODEC_HIGHPASS
    {
      /* State persists across frames, like the decimator's. Encoder-local, so
         it cannot be desynchronised by a lost packet. */
      static int32_t x1, x2, y1, y2;

      int32_t x0 = acc;
      int32_t y0 = ((int32_t)HP_B0 * x0
                  + (int32_t)HP_B1 * x1
                  + (int32_t)HP_B2 * x2
                  - (int32_t)HP_A1 * y1
                  - (int32_t)HP_A2 * y2) >> HP_Q;

      x2 = x1; x1 = x0;
      y2 = y1; y1 = y0;

      if (y0 > 32767)  { y0 = 32767; }
      if (y0 < -32768) { y0 = -32768; }
      acc = y0;
    }
#endif

    ulaw_out[i] = AudioCodec_MuLawEncode((int16_t)acc);
  }
}

void AudioCodec_DecodeFrame(const uint8_t *ulaw_in, uint32_t n_in,
                            uint16_t *dac_out, uint32_t n_out)
{
  if (ulaw_in == NULL || dac_out == NULL || n_in == 0U || n_out < (n_in * 2U))
  {
    return;
  }

  for (uint32_t i = 0U; i < n_in; i++)
  {
    int32_t pcm = AudioCodec_MuLawDecode(ulaw_in[i]);

    int32_t dac = (pcm >> ADC_TO_PCM_SHIFT) + ADC_MIDSCALE;

    if (dac > 4095) { dac = 4095; }
    if (dac < 0)    { dac = 0; }

    /* Zero-order hold: each 8 kHz sample is emitted twice at 16 kHz. The
       resulting staircase is smoothed by the analogue low-pass on the speaker
       output, so interpolating here would buy nothing audible. */
    dac_out[2U * i]        = (uint16_t)dac;
    dac_out[(2U * i) + 1U] = (uint16_t)dac;
  }
}

void AudioCodec_Silence(uint16_t *dac_out, uint32_t n_out)
{
  if (dac_out == NULL) { return; }

  for (uint32_t i = 0U; i < n_out; i++)
  {
    dac_out[i] = (uint16_t)ADC_MIDSCALE;
  }
}
