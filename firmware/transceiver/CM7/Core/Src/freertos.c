/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : freertos.c
  * Description        : Code for freertos applications
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "radio_protocol.h"
#include "adpcm.h"
#include "semphr.h"
#include "stream_buffer.h"
#include "queue.h"
#include <string.h>
#include <usb_device.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN Variables */
extern StreamBufferHandle_t xAudioInputStreamBuffer;
extern QueueHandle_t xRadioTxQueue;
extern SemaphoreHandle_t xUartTxSemaphore;

extern UART_HandleTypeDef huart6;

adpcm_state_t encoder_state = {0, 0};

#define FRAME_SAMPLES 320
#define FRAME_BYTES   (FRAME_SAMPLES * sizeof(int16_t))
/* USER CODE END Variables */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */

/* USER CODE END FunctionPrototypes */

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */

/* ============================================================ */
/* AUDIO ENCODER (Priority: Normal)                             */
/* Reads Raw PCM from USB Stream -> Encodes -> Pushes to Queue  */
/* ============================================================ */
void StartAudioTask(void* argument)
{
  MX_USB_DEVICE_Init();

  int16_t pcm_input_buffer[FRAME_SAMPLES];
  size_t bytes_received;
  RadioPacket_t audio_packet;

  audio_packet.start_byte = PACKET_START_BYTE;
  audio_packet.type = PACKET_TYPE_AUDIO;

  for (;;)
  {
    bytes_received = xStreamBufferReceive(
      xAudioInputStreamBuffer,
      (void*)pcm_input_buffer,
      FRAME_BYTES,
      portMAX_DELAY
    );

    if (bytes_received == FRAME_BYTES)
    {
      int encoded_idx = 0;

      for (int i = 0; i < FRAME_SAMPLES; i += 2)
      {
        uint8_t high = adpcm_encode_sample(pcm_input_buffer[i], &encoder_state);
        uint8_t low = 0;

        if (i + 1 < FRAME_SAMPLES)
        {
          low = adpcm_encode_sample(pcm_input_buffer[i + 1], &encoder_state);
        }

        audio_packet.payload[encoded_idx++] = (high << 4) | (low & 0x0F);
      }

      audio_packet.length = encoded_idx;

      xQueueSend(xRadioTxQueue, &audio_packet, pdMS_TO_TICKS(5));
    }
  }
}

/* ============================================================ */
/* TELEMETRY GENERATOR (Priority: High)                         */
/* Simulates CAN data -> Pushes to Queue                        */
/* ============================================================ */
void StartTelemetryTask(void* argument)
{
  RadioPacket_t telemetry_packet;
  telemetry_packet.start_byte = PACKET_START_BYTE;
  telemetry_packet.type = PACKET_TYPE_TELEMETRY;

  uint8_t dummy_soc = 95;
  uint8_t dummy_temp = 65;

  for (;;)
  {
    dummy_soc--;

    if (dummy_soc < 10) dummy_soc = 95;

    telemetry_packet.payload[0] = dummy_soc;
    telemetry_packet.payload[1] = dummy_temp;
    telemetry_packet.length = 2;

    xQueueSend(xRadioTxQueue, &telemetry_packet, 0);

    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

/* ============================================================ */
/* RADIO GATEKEEPER (Priority: Normal/High)                     */
/* Takes Packets from Queue -> Sends via UART DMA               */
/* ============================================================ */
void StartRadioTxTask(void* argument)
{
  RadioPacket_t tx_packet;

  static __attribute__((section(".dma_buffer"))) __attribute__((aligned(32))) uint8_t dma_buffer[sizeof(RadioPacket_t)];

  for (;;)
  {
    if (xQueueReceive(xRadioTxQueue, &tx_packet, portMAX_DELAY) == pdTRUE)
    {
      if (xSemaphoreTake(xUartTxSemaphore, portMAX_DELAY) == pdTRUE)
      {
        dma_buffer[0] = tx_packet.start_byte;
        dma_buffer[1] = tx_packet.type;
        dma_buffer[2] = tx_packet.length;

        memcpy(&dma_buffer[3], tx_packet.payload, tx_packet.length);

        uint16_t total_len = 3 + tx_packet.length;

        if (HAL_UART_Transmit_DMA(&huart6, dma_buffer, total_len) != HAL_OK)
        {
          xSemaphoreGive(xUartTxSemaphore);
        }
      }
    }
  }
}

/* USER CODE END Application */

