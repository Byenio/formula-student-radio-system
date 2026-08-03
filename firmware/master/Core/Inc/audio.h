/**
  ******************************************************************************
  * @file    audio.h
  * @brief   Audio capture/playback pipeline for the driver radio board.
  *
  * Sample flow:
  *   TIM6 (16 kHz) --trigger--> ADC1 --DMA--> adc_buffer  (mic in)
  *   TIM6 (16 kHz) --trigger--> DAC1 <--DMA-- dac_buffer  (headset out)
  *
  * Both converters share one timer trigger, so capture and playback stay
  * sample-locked with no drift between them.
  *
  * Each DMA buffer holds two frames. The DMA runs continuously around the
  * buffer while the CPU works on whichever half it is not currently touching
  * (classic ping-pong), so there is no tearing and no need to stop the stream.
  *
  * DECOUPLING FROM THE RADIO
  * -------------------------
  * The audio task runs at high priority against a hard 10 ms deadline. The
  * radio task blocks on BUSY and on TxDone, for milliseconds at a time. They
  * therefore never call each other: encoded frames go into a queue that the
  * radio drains, and received frames come back through a second queue. A slow
  * radio transaction can then never stall the DMA pipeline -- it just costs a
  * dropped frame, which is exactly the failure mode voice tolerates best.
  *
  * These same queues are where telemetry will plug in later.
  ******************************************************************************
  */

#ifndef AUDIO_H
#define AUDIO_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include "link_proto.h"
#include <stdint.h>
#include <stdbool.h>

/* Sample rate is set by TIM6 (PSC/ARR), not by this constant -- this is here
   so application code can reason about timing. Keep the two in sync. */
#define AUDIO_SAMPLE_RATE_HZ   16000U

/* One frame = 10 ms of audio. This is the unit that becomes one radio
   payload, so it is worth keeping it a round number of ms. */
#define AUDIO_FRAME_SAMPLES    160U

/* DMA buffers hold two frames: one being filled/drained by DMA, one being
   worked on by the CPU. */
#define AUDIO_DMA_BUFFER_LEN   (AUDIO_FRAME_SAMPLES * 2U)

/* ---- Audio path selector -------------------------------------------------
   Lets the codec be judged independently of the radio, which is otherwise
   impossible -- a click from a dropped packet and distortion from a clipping
   preamp both just sound "bad".

     0 = radio echo   : capture -> encode -> radio -> echo -> decode -> play
     1 = codec loop   : capture -> encode -> decode -> play   (no radio)
     2 = raw passthru : capture -> play                       (no codec)

   Compare 2 against 1 to hear exactly what the codec costs, then 1 against 0
   to hear exactly what the link costs. Whichever step introduces the problem
   is where to look. */
#define AUDIO_PATH_RADIO_ECHO   0
#define AUDIO_PATH_CODEC_LOOP   1
#define AUDIO_PATH_RAW_PASSTHRU 2

#ifndef AUDIO_PATH
#define AUDIO_PATH              AUDIO_PATH_RADIO_ECHO
#endif

/* Encoded frame size, after decimation to 8 kHz and mu-law companding. */
#define AUDIO_ENCODED_LEN      LINK_AUDIO_PAYLOAD_LEN     /* 80 bytes */

/* Queue depths, in frames. Deliberately shallow: every queued frame is 10 ms
   of latency, and for live voice latency matters more than smoothing. Two on
   the way out is enough to ride over one slow radio transaction; four on the
   way in absorbs the jitter of a half-duplex turnaround. */
#define AUDIO_TX_QUEUE_FRAMES  2U
#define AUDIO_RX_QUEUE_FRAMES  4U

/**
  * @brief Create the audio thread and its queues. Call after
  *        osKernelInitialize(), alongside the other osThreadNew() calls.
  */
void Audio_Init(void);

/**
  * @brief Take the next encoded mic frame, for the radio to transmit.
  * @param out        buffer of at least AUDIO_ENCODED_LEN bytes
  * @param timeout_ms 0 to poll, or a wait in milliseconds
  * @retval true if a frame was returned
  */
bool Audio_GetEncodedFrame(uint8_t *out, uint32_t timeout_ms);

/**
  * @brief Hand a received frame to the playback path.
  * @retval false if the queue was full, i.e. playback is falling behind
  */
bool Audio_PutDecodedFrame(const uint8_t *in);

/**
  * @brief Most recent mic frame peak-to-peak amplitude, in ADC counts.
  *        Useful for VOX/squelch decisions and for a signal-present LED.
  */
uint16_t Audio_GetLastFramePeakToPeak(void);

/* Bring-up counters, all watchable in Live Expressions. */
uint32_t Audio_GetFramesCaptured(void);
uint32_t Audio_GetFramesPlayed(void);
uint32_t Audio_GetTxDropped(void);   /*!< radio not draining fast enough    */
uint32_t Audio_GetRxUnderruns(void); /*!< nothing to play; silence emitted  */

/**
  * @brief Highest peak-to-peak seen since the last call, then reset.
  *        A value pinned near 4095 means the mic preamp is clipping, which is
  *        analogue distortion no codec setting will fix.
  */
uint16_t Audio_GetPeakHoldAndReset(void);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_H */
