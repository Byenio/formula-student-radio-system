/**
  ******************************************************************************
  * @file    audio.c
  * @brief   Audio capture/playback pipeline. See audio.h for the data flow.
  ******************************************************************************
  */

#include "audio.h"
#include "audio_codec.h"
#include "cmsis_os2.h"
#include <string.h>

/* Handles owned by CubeMX in main.c */
extern ADC_HandleTypeDef hadc1;
extern DAC_HandleTypeDef hdac1;
extern TIM_HandleTypeDef htim6;

/* ---- DMA buffers ---------------------------------------------------------
   DMA writes to these behind the compiler's back, so they must not be
   optimised into registers. */
static uint16_t adc_buffer[AUDIO_DMA_BUFFER_LEN];
static uint16_t dac_buffer[AUDIO_DMA_BUFFER_LEN];

/* Which half of adc_buffer just became ready. Written from ISR context,
   read from the task after a thread flag wakes it. */
#define AUDIO_FLAG_FIRST_HALF   (1U << 0)
#define AUDIO_FLAG_SECOND_HALF  (1U << 1)

static osThreadId_t       audioTaskHandle;
static osMessageQueueId_t audioTxQueue;   /* encoded mic  -> radio    */
static osMessageQueueId_t audioRxQueue;   /* radio        -> speaker  */

static volatile uint16_t last_frame_pk_pk;
static volatile uint32_t frames_captured;
static volatile uint32_t frames_played;
static volatile uint32_t tx_dropped;
static volatile uint32_t rx_underruns;
static volatile uint16_t peak_hold;

static const osThreadAttr_t audioTask_attributes = {
  .name       = "audioTask",
  .priority   = (osPriority_t) osPriorityHigh,   /* audio must not be starved */
  .stack_size = 512 * 4
};

static void Audio_Task(void *argument);
static void Audio_ProcessFrame(const uint16_t *mic_in, uint16_t *spk_out);

/* -------------------------------------------------------------------------- */

void Audio_Init(void)
{
  audioTxQueue = osMessageQueueNew(AUDIO_TX_QUEUE_FRAMES, AUDIO_ENCODED_LEN, NULL);
  audioRxQueue = osMessageQueueNew(AUDIO_RX_QUEUE_FRAMES, AUDIO_ENCODED_LEN, NULL);

  if (audioTxQueue == NULL || audioRxQueue == NULL)
  {
    Error_Handler();     /* out of FreeRTOS heap -- fail loudly, not silently */
  }

  audioTaskHandle = osThreadNew(Audio_Task, NULL, &audioTask_attributes);

  if (audioTaskHandle == NULL)
  {
    Error_Handler();
  }
}

bool Audio_GetEncodedFrame(uint8_t *out, uint32_t timeout_ms)
{
  if (out == NULL || audioTxQueue == NULL) { return false; }

  return (osMessageQueueGet(audioTxQueue, out, NULL, timeout_ms) == osOK);
}

bool Audio_PutDecodedFrame(const uint8_t *in)
{
  if (in == NULL || audioRxQueue == NULL) { return false; }

  /* Never block: this is called from the radio task, and stalling it to wait
     on playback would back up the whole receive path. Dropping the oldest
     audio is always better than delaying the newest. */
  return (osMessageQueuePut(audioRxQueue, in, 0U, 0U) == osOK);
}

uint16_t Audio_GetLastFramePeakToPeak(void) { return last_frame_pk_pk; }
uint32_t Audio_GetFramesCaptured(void)      { return frames_captured; }
uint32_t Audio_GetFramesPlayed(void)        { return frames_played; }
uint32_t Audio_GetTxDropped(void)           { return tx_dropped; }
uint32_t Audio_GetRxUnderruns(void)         { return rx_underruns; }

uint16_t Audio_GetPeakHoldAndReset(void)
{
  uint16_t v = peak_hold;
  peak_hold = 0U;
  return v;
}

/* -------------------------------------------------------------------------- */

static void Audio_Task(void *argument)
{
  (void)argument;

  /* Silence = mid-scale, matching the ~1.65 V bias the analogue stage sits at.
     Starting from zeros would slam the output to 0 V and thump the headset. */
  AudioCodec_Silence(dac_buffer, AUDIO_DMA_BUFFER_LEN);

  /* Order matters: arm both converters before starting the timer that
     triggers them, so the first trigger edge is not missed. */
  if (HAL_DAC_Start_DMA(&hdac1, DAC_CHANNEL_1,
                        (uint32_t *)dac_buffer, AUDIO_DMA_BUFFER_LEN,
                        DAC_ALIGN_12B_R) != HAL_OK)
  {
    Error_Handler();
  }

  if (HAL_ADC_Start_DMA(&hadc1, (uint32_t *)adc_buffer,
                        AUDIO_DMA_BUFFER_LEN) != HAL_OK)
  {
    Error_Handler();
  }

  if (HAL_TIM_Base_Start(&htim6) != HAL_OK)
  {
    Error_Handler();
  }

  for (;;)
  {
    uint32_t flags = osThreadFlagsWait(AUDIO_FLAG_FIRST_HALF | AUDIO_FLAG_SECOND_HALF,
                                       osFlagsWaitAny, osWaitForever);

    if (flags & AUDIO_FLAG_FIRST_HALF)
    {
      Audio_ProcessFrame(&adc_buffer[0], &dac_buffer[0]);
    }
    if (flags & AUDIO_FLAG_SECOND_HALF)
    {
      Audio_ProcessFrame(&adc_buffer[AUDIO_FRAME_SAMPLES],
                         &dac_buffer[AUDIO_FRAME_SAMPLES]);
    }
  }
}

/**
  * @brief Handle one 10 ms frame: encode the mic half, fill the speaker half.
  *
  * Both directions are handled here because both are tied to the same DMA
  * half-complete event, and doing them together keeps capture and playback
  * on exactly the same cadence.
  */
static void Audio_ProcessFrame(const uint16_t *mic_in, uint16_t *spk_out)
{
  uint8_t  encoded[AUDIO_ENCODED_LEN];
  uint16_t min = 4095U;
  uint16_t max = 0U;

  /* ---- Capture path: mic -> 8 kHz mu-law -> queue -> radio ---- */

  for (uint32_t i = 0U; i < AUDIO_FRAME_SAMPLES; i++)
  {
    uint16_t s = mic_in[i];
    if (s < min) { min = s; }
    if (s > max) { max = s; }
  }
  last_frame_pk_pk = (uint16_t)(max - min);
  if (last_frame_pk_pk > peak_hold) { peak_hold = last_frame_pk_pk; }

  frames_captured++;

#if AUDIO_PATH == AUDIO_PATH_RAW_PASSTHRU

  /* No codec at all -- the original 16 kHz loopback, kept as a reference. If
     this sounds clean and the codec path does not, the codec is at fault. */
  for (uint32_t i = 0U; i < AUDIO_FRAME_SAMPLES; i++)
  {
    spk_out[i] = mic_in[i];
  }
  frames_played++;
  (void)encoded;

#elif AUDIO_PATH == AUDIO_PATH_CODEC_LOOP

  /* Straight through the codec and back, bypassing the queues and the radio.
     Isolates decimation and companding from anything the link does. */
  AudioCodec_EncodeFrame(mic_in, AUDIO_FRAME_SAMPLES, encoded, AUDIO_ENCODED_LEN);
  AudioCodec_DecodeFrame(encoded, AUDIO_ENCODED_LEN, spk_out, AUDIO_FRAME_SAMPLES);
  frames_played++;

#else   /* AUDIO_PATH_RADIO_ECHO */

  AudioCodec_EncodeFrame(mic_in, AUDIO_FRAME_SAMPLES,
                         encoded, AUDIO_ENCODED_LEN);

  /* Timeout zero. If the radio has not drained the queue we drop this frame
     rather than miss the DMA deadline -- one lost 10 ms of speech is far
     less damaging than a stalled audio pipeline. */
  if (osMessageQueuePut(audioTxQueue, encoded, 0U, 0U) != osOK)
  {
    tx_dropped++;
  }

  /* ---- Playback path: radio -> queue -> 16 kHz -> speaker ---- */

  uint8_t received[AUDIO_ENCODED_LEN];

  if (osMessageQueueGet(audioRxQueue, received, NULL, 0U) == osOK)
  {
    AudioCodec_DecodeFrame(received, AUDIO_ENCODED_LEN,
                           spk_out, AUDIO_FRAME_SAMPLES);
    frames_played++;
  }
  else
  {
    /* Nothing arrived in time. Emitting mid-scale is the least objectionable
       option: it is what the analogue stage idles at, so no step, no click. */
    AudioCodec_Silence(spk_out, AUDIO_FRAME_SAMPLES);
    rx_underruns++;
  }

#endif
}

/* ---- DMA completion callbacks (ISR context) ------------------------------
   These are weak symbols in the HAL; defining them here overrides them.
   Keep them short: just wake the task, do the work there. */

void HAL_ADC_ConvHalfCpltCallback(ADC_HandleTypeDef *hadc)
{
  if (hadc->Instance == ADC1 && audioTaskHandle != NULL)
  {
    osThreadFlagsSet(audioTaskHandle, AUDIO_FLAG_FIRST_HALF);
  }
}

void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
  if (hadc->Instance == ADC1 && audioTaskHandle != NULL)
  {
    osThreadFlagsSet(audioTaskHandle, AUDIO_FLAG_SECOND_HALF);
  }
}
