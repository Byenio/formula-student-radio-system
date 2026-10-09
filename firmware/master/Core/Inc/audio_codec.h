/**
  ******************************************************************************
  * @file    audio_codec.h
  * @brief   Voice codec for the radio link: mu-law companding and 2:1 rate
  *          conversion.
  *
  * WHY MU-LAW AND NOT ADPCM
  * ------------------------
  * ADPCM would halve the payload again, but it is differential: the decoder
  * carries a predictor and step index derived from every previous sample.
  * Lose one packet over the air and the decoder diverges from the encoder,
  * garbling audio until they happen to resynchronise. On a lossy half-duplex
  * link that turns a 10 ms dropout into seconds of noise.
  *
  * Mu-law is stateless. Every sample decodes independently, so a lost packet
  * costs exactly one 10 ms gap and the next frame is perfect. Airtime is not
  * the constraint here anyway -- voice occupies under 8% of the channel.
  *
  * RATE CONVERSION
  * ---------------
  * Capture is 16 kHz, air is 8 kHz. Decimation averages sample pairs, which
  * is a 2-tap boxcar with a null at 8 kHz; adequate because the mic path is
  * already low-passed near 4.8 kHz by R118/C121.
  *
  * Decimation runs a 15-tap windowed-sinc low-pass first. A plain 2-tap
  * average is only 3 dB down at 4 kHz, so anything between 4 and 8 kHz folds
  * straight back into the voice band -- and clipping harmonics live exactly
  * there. This filter is 35 to 60 dB down across that range instead.
  *
  * The filter carries 14 samples of state between frames. That does NOT
  * reintroduce the error propagation that rules out ADPCM: the state lives
  * only in the encoder and never crosses the air, so a lost packet cannot
  * desynchronise anything.
  *
  * Interpolation is zero-order hold, deliberately: R123/C129 on the speaker
  * output form a ~3.4 kHz low-pass that does the reconstruction filtering in
  * analogue, so there is nothing to gain from interpolating in software.
  ******************************************************************************
  */

#ifndef AUDIO_CODEC_H
#define AUDIO_CODEC_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

/**
  * @brief Companding, one sample at a time. Exposed mainly so the round trip
  *        can be unit-tested without any hardware.
  */
/* Largest capture frame the encoder will be handed, so its scratch buffer can
   be sized at compile time rather than allocated on the audio task's stack. */
#define AUDIO_CODEC_MAX_IN_SAMPLES  160U

/* ---- Microphone gain trim, Q8 (256 = unity) -------------------------------
   Applied digitally after the ADC. This exists so the analogue gain can be
   set LOW -- low enough that a shouting driver never clips the preamp -- and
   the level then brought back up here, where it costs a recompile instead of
   a soldering iron.

   Order matters: analogue clipping destroys information permanently, digital
   gain only trades resolution. Losing two bits of a 12-bit ADC still leaves
   about 60 dB, and mu-law's own ~38 dB is the real limit anyway, so the trade
   is heavily in favour of turning the analogue gain down.

   With R121 changed to 4k7 (analogue gain ~11), start at 1024 (4x) and adjust
   until peak_hold sits around 3000 with a loud voice. */
#ifndef AUDIO_CODEC_MIC_GAIN_Q8
#define AUDIO_CODEC_MIC_GAIN_Q8     256
#endif

/* ---- High-pass at 300 Hz --------------------------------------------------
   Set to 0 to bypass, for A/B comparison.

   The board's own transmitter couples into the analogue front end, raising the
   noise floor about 12 dB while keyed. It shows up as 100 Hz and its harmonics
   -- one current pulse per audio frame.

   This removes the fundamental (-19 dB) and most of the second harmonic
   (-8 dB) at no cost to speech, which is why the telephone band starts at
   300 Hz in the first place. It deliberately does NOT try to notch the
   harmonics above that: they sit inside the voice band, and a comb filter
   spaced every 100 Hz would gut a male voice along with the noise. Those have
   to be fixed at source, with lower analogue gain and better decoupling. */
#ifndef AUDIO_CODEC_HIGHPASS
#define AUDIO_CODEC_HIGHPASS        1
#endif

uint8_t AudioCodec_MuLawEncode(int16_t pcm);
int16_t AudioCodec_MuLawDecode(uint8_t ulaw);

/**
  * @brief Encode one captured frame for transmission.
  *
  * @param adc_in   16 kHz unsigned 12-bit samples, biased around mid-scale
  * @param n_in     number of input samples (must be 2x n_out)
  * @param ulaw_out one mu-law byte per output sample
  * @param n_out    number of output samples
  */
void AudioCodec_EncodeFrame(const uint16_t *adc_in, uint32_t n_in,
                            uint8_t *ulaw_out, uint32_t n_out);

/**
  * @brief Decode one received frame for playback.
  *
  * @param ulaw_in mu-law bytes as they arrived
  * @param n_in    number of input samples
  * @param dac_out 16 kHz unsigned 12-bit samples ready for the DAC
  * @param n_out   number of output samples (must be 2x n_in)
  */
void AudioCodec_DecodeFrame(const uint8_t *ulaw_in, uint32_t n_in,
                            uint16_t *dac_out, uint32_t n_out);

/**
  * @brief Fill a DAC frame with silence, i.e. the mid-scale bias the analogue
  *        stage idles at. Used when no packet arrived in time.
  */
void AudioCodec_Silence(uint16_t *dac_out, uint32_t n_out);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_CODEC_H */
