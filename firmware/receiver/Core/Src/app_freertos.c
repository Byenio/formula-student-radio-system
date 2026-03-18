/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : app_freertos.c
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
#include "stream_buffer.h"
#include "queue.h"
#include "semphr.h"
#include <string.h>

extern UART_HandleTypeDef hlpuart1;
extern StreamBufferHandle_t xRadioRxStreamBuffer;
extern QueueHandle_t xPcTxQueue;
extern SemaphoreHandle_t xPcUartSemaphore;
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

/* USER CODE END Variables */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */

/* USER CODE END FunctionPrototypes */

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */


/* ============================================================ */
/* PACKET PARSER                                                */
/* Reads byte stream -> Finds Packets -> Queues for PC          */
/* ============================================================ */
void StartParserTask(void* argument)
{
  uint8_t rx_byte;
  uint8_t state = 0;  // 0=WaitStart, 1=Type, 2=Len, 3=Payload
  RadioPacket_t current_packet;
  uint8_t payload_index = 0;

  for (;;)
  {
    if (xStreamBufferReceive(xRadioRxStreamBuffer, &rx_byte, 1, portMAX_DELAY) == 1)
    {
      switch (state)
      {
        case 0:
          if (rx_byte == PACKET_START_BYTE)
          {
            current_packet.start_byte = rx_byte;
            state = 1;
          }
          break;

        case 1:
          current_packet.type = rx_byte;
          state = 2;
          break;

        case 2:
          current_packet.length = rx_byte;

          if (current_packet.length > MAX_PAYLOAD_SIZE)
          {
            state = 0;
          } else
          {
            payload_index = 0;
            state = 3;
          }
          break;

        case 3:
          current_packet.payload[payload_index++] = rx_byte;
          if (payload_index >= current_packet.length)
          {
            xQueueSend(xPcTxQueue, &current_packet, pdMS_TO_TICKS(10));
            state = 0;
          }
          break;
      }
    }
  }
}

/* ============================================================ */
/* PC GATEKEEPER                                                */
/* Serializes Packet -> Sends to PC via LPUART                  */
/* ============================================================ */
void StartPcTxTask(void* argument)
{
  RadioPacket_t tx_packet;
  static uint8_t pc_buffer[sizeof(RadioPacket_t) + 5];

  for (;;)
  {
    if (xQueueReceive(xPcTxQueue, &tx_packet, portMAX_DELAY) == pdTRUE)
    {
      if (xSemaphoreTake(xPcUartSemaphore, portMAX_DELAY) == pdTRUE)
      {
        pc_buffer[0] = tx_packet.start_byte;
        pc_buffer[1] = tx_packet.type;
        pc_buffer[2] = tx_packet.length;
        memcpy(&pc_buffer[3], tx_packet.payload, tx_packet.length);

        uint16_t len = 3 + tx_packet.length;

        HAL_UART_Transmit_DMA(&hlpuart1, pc_buffer, len);
      }
    }
  }
}
/* USER CODE END Application */
