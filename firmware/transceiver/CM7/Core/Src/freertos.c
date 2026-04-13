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
#include "semphr.h"
#include "stream_buffer.h"
#include "queue.h"
#include <string.h>
#include <usb_device.h>
#include "opus.h"
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

#define FRAME_SAMPLES 320 // 20ms @ 16kHz
#define FRAME_BYTES   (FRAME_SAMPLES * sizeof(int16_t))
/* USER CODE END Variables */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */

/* USER CODE END FunctionPrototypes */

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */

void StartAudioTask(void* argument)
{
  MX_USB_DEVICE_Init();

  int16_t pcm_input_buffer[FRAME_SAMPLES];
  size_t bytes_received;
  RadioPacket_t audio_packet;

  audio_packet.start_byte = PACKET_START_BYTE;
  audio_packet.type = PACKET_TYPE_AUDIO;

  int enc_size = opus_encoder_get_size(1);
  OpusEncoder *encoder = (OpusEncoder*)pvPortMalloc(enc_size);

  if (encoder == NULL)
  {
    while(1) {
      HAL_GPIO_TogglePin(GPIOB, GPIO_PIN_14);
      vTaskDelay(pdMS_TO_TICKS(50));
    }
  }

  memset(encoder, 0, enc_size);

  int opus_err = opus_encoder_init(encoder, 16000, 1, OPUS_APPLICATION_VOIP);
  if (opus_err != OPUS_OK)
  {
    while(1) {
      HAL_GPIO_TogglePin(GPIOB, GPIO_PIN_14);
      vTaskDelay(pdMS_TO_TICKS(250));
    }
  }

  opus_encoder_ctl(encoder, OPUS_SET_BITRATE(24000));
  opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(0));

  for (;;)
  {
    size_t total_bytes_received = 0;
    uint8_t *buffer_ptr = (uint8_t*)pcm_input_buffer;
    uint8_t audio_seq = 0;

    while (total_bytes_received < FRAME_BYTES)
    {
      size_t bytes_received = xStreamBufferReceive(
        xAudioInputStreamBuffer,
        buffer_ptr + total_bytes_received,
        FRAME_BYTES - total_bytes_received,
        portMAX_DELAY
      );
      total_bytes_received += bytes_received;
    }

    if (BSP_PB_GetState(BUTTON_USER) == 1)
    {
      int encoded_bytes = opus_encode(
        encoder,
        pcm_input_buffer,
        FRAME_SAMPLES,
        audio_packet.payload,
        MAX_PAYLOAD_SIZE
      );

      if (encoded_bytes > 0)
      {
        audio_packet.length = (uint8_t)encoded_bytes;
        audio_packet.seq_num = audio_seq++;
        xQueueSend(xRadioTxQueue, &audio_packet, pdMS_TO_TICKS(5));
      } else if (encoded_bytes < 0)
      {
        HAL_GPIO_TogglePin(GPIOB, GPIO_PIN_14);
      }
    }
  }
}

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

    xQueueSendToFront(xRadioTxQueue, &telemetry_packet, pdMS_TO_TICKS(10));

    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

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
        dma_buffer[3] = tx_packet.seq_num;

        memcpy(&dma_buffer[4], tx_packet.payload, tx_packet.length);

        uint16_t total_len = 4 + tx_packet.length;

        SCB_CleanDCache_by_Addr((uint32_t*)dma_buffer, total_len + 32);

        if (HAL_UART_Transmit_DMA(&huart6, dma_buffer, total_len) != HAL_OK)
        {
          xSemaphoreGive(xUartTxSemaphore);
          HAL_GPIO_WritePin(GPIOB, GPIO_PIN_14, GPIO_PIN_SET);
        } else
        {
          HAL_GPIO_WritePin(GPIOB, GPIO_PIN_14, GPIO_PIN_RESET);
        }
      }
    }
  }
}

/* USER CODE END Application */

