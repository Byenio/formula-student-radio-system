/**
  ******************************************************************************
  * @file    rs485.c
  * @brief   Telemetry input from the VCU over RS-485. See rs485.h.
  ******************************************************************************
  */

#include "rs485.h"
#include "cmsis_os2.h"
#include <string.h>

extern UART_HandleTypeDef huart1;

/* ---- DMA reception ------------------------------------------------------- */

static uint8_t  dma_buf[RS485_DMA_BUF_LEN];
static uint32_t dma_read_pos;          /* how far the parser has consumed */

/* ---- Frame reassembly ---------------------------------------------------- */

static uint8_t  frame[RS485_MAX_FRAME];
static uint32_t frame_fill;

/* ---- Output queue -------------------------------------------------------- */

typedef struct {
    uint8_t len;
    uint8_t data[LINK_MAX_OTA_PAYLOAD];
} rs485_payload_t;

static osMessageQueueId_t payload_queue;
static osThreadId_t       rs485_task_handle;

/* ---- Counters ------------------------------------------------------------ */

static volatile uint32_t bytes_received;
static volatile uint32_t frames_good;
static volatile uint32_t frames_bad_crc;
static volatile uint32_t resyncs;
static volatile uint32_t queue_full;
static volatile uint32_t dma_errors;

#define RS485_FLAG_DATA     (1U << 0)

static const osThreadAttr_t rs485_task_attributes = {
  .name       = "rs485Task",
  .priority   = (osPriority_t) osPriorityNormal,   /* below audio */
  .stack_size = 512 * 4
};

static void RS485_Task(void *argument);

/* -------------------------------------------------------------------------- */

void RS485_Init(void)
{
  /* Receive only: RE low enables the receiver, DE low disables the driver.
     Both are the same pin here, so one write does it and it never changes. */
  HAL_GPIO_WritePin(RS485_RE_DE_GPIO_Port, RS485_RE_DE_Pin, GPIO_PIN_RESET);

  payload_queue = osMessageQueueNew(RS485_QUEUE_DEPTH,
                                    sizeof(rs485_payload_t), NULL);
  if (payload_queue == NULL)
  {
    Error_Handler();
  }

  rs485_task_handle = osThreadNew(RS485_Task, NULL, &rs485_task_attributes);
  if (rs485_task_handle == NULL)
  {
    Error_Handler();
  }
}

bool RS485_GetPayload(uint8_t *out, uint8_t *len, uint32_t timeout_ms)
{
  rs485_payload_t item;

  if (out == NULL || len == NULL || payload_queue == NULL)
  {
    return false;
  }

  if (osMessageQueueGet(payload_queue, &item, NULL, timeout_ms) != osOK)
  {
    return false;
  }

  memcpy(out, item.data, item.len);
  *len = item.len;
  return true;
}

/* ---- Frame parser --------------------------------------------------------
   Tolerant by design: the sync pattern can occur inside a payload by chance,
   so a frame that fails its CRC does not poison the stream -- we drop one byte
   and resume the hunt, recovering within a frame or two. Same approach as the
   USB link on the base station, for the same reason. */

static void feed(uint8_t b)
{
  if (frame_fill >= sizeof(frame))
  {
    frame_fill = 0U;              /* nothing valid found; start over */
  }

  frame[frame_fill++] = b;

  /* Discard anything before a plausible sync. */
  for (;;)
  {
    if (frame_fill >= 1U && frame[0] != 0xA5U)
    {
      memmove(frame, &frame[1], --frame_fill);
      resyncs++;
      continue;
    }
    if (frame_fill >= 2U && frame[1] != 0x5AU)
    {
      memmove(frame, &frame[1], --frame_fill);
      resyncs++;
      continue;
    }
    break;
  }

  if (frame_fill < 3U)
  {
    return;                       /* length byte not here yet */
  }

  uint8_t  payload_len = frame[2];
  uint32_t total       = 3U + (uint32_t)payload_len + 2U;

  if (payload_len == 0U || payload_len > LINK_MAX_OTA_PAYLOAD)
  {
    /* Impossible length, so the sync was a false positive. */
    memmove(frame, &frame[1], --frame_fill);
    resyncs++;
    return;
  }

  if (frame_fill < total)
  {
    return;                       /* still arriving */
  }

  uint16_t got  = (uint16_t)frame[3U + payload_len] |
                  ((uint16_t)frame[3U + payload_len + 1U] << 8);
  uint16_t want = Link_Crc16(&frame[2], 1U + (uint32_t)payload_len);

  if (got == want)
  {
    rs485_payload_t item;
    item.len = payload_len;
    memcpy(item.data, &frame[3], payload_len);

    /* Never block: this runs in the receive path, and stalling it would let
       the DMA ring overrun. Dropping the newest keeps the older ones in
       order, which matters because telemetry is timestamped. */
    if (osMessageQueuePut(payload_queue, &item, 0U, 0U) == osOK)
    {
      frames_good++;
    }
    else
    {
      queue_full++;
    }

    /* Consume the frame, keeping whatever followed it. */
    frame_fill -= total;
    if (frame_fill > 0U)
    {
      memmove(frame, &frame[total], frame_fill);
    }
  }
  else
  {
    frames_bad_crc++;
    memmove(frame, &frame[1], --frame_fill);
  }
}

/* ---- Task ---------------------------------------------------------------- */

static void RS485_Task(void *argument)
{
  (void)argument;

  dma_read_pos = 0U;
  frame_fill   = 0U;

  /* ReceiveToIdle gives us a callback whenever the line goes quiet, so a
     partial frame is handed over promptly instead of waiting for the buffer
     to fill. Circular mode means the DMA never needs restarting. */
  if (HAL_UARTEx_ReceiveToIdle_DMA(&huart1, dma_buf, RS485_DMA_BUF_LEN) != HAL_OK)
  {
    Error_Handler();
  }

  /* The half-transfer interrupt is not useful here and only adds callbacks. */
  __HAL_DMA_DISABLE_IT(huart1.hdmarx, DMA_IT_HT);

  for (;;)
  {
    (void)osThreadFlagsWait(RS485_FLAG_DATA, osFlagsWaitAny, 100U);

    /* Where has the DMA got to? Works the same whether we were woken by an
       idle line, a half/full transfer, or the timeout above -- so a missed
       callback costs latency, not data. */
    uint32_t write_pos = RS485_DMA_BUF_LEN
                       - __HAL_DMA_GET_COUNTER(huart1.hdmarx);

    while (dma_read_pos != write_pos)
    {
      feed(dma_buf[dma_read_pos]);
      bytes_received++;

      dma_read_pos++;
      if (dma_read_pos >= RS485_DMA_BUF_LEN)
      {
        dma_read_pos = 0U;
      }
    }
  }
}

/* ---- HAL callbacks (ISR context) ----------------------------------------- */

void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
  (void)Size;

  if (huart->Instance == USART1 && rs485_task_handle != NULL)
  {
    osThreadFlagsSet(rs485_task_handle, RS485_FLAG_DATA);
  }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART1)
  {
    dma_errors++;

    /* An overrun or framing error aborts the transfer, so it has to be
       restarted or the link goes permanently silent. Most likely cause during
       bring-up is a baud rate mismatch. */
    (void)HAL_UARTEx_ReceiveToIdle_DMA(huart, dma_buf, RS485_DMA_BUF_LEN);
    __HAL_DMA_DISABLE_IT(huart->hdmarx, DMA_IT_HT);
  }
}

/* ---- Counters ------------------------------------------------------------ */

uint32_t RS485_GetBytesReceived(void) { return bytes_received; }
uint32_t RS485_GetFramesGood(void)    { return frames_good; }
uint32_t RS485_GetFramesBadCrc(void)  { return frames_bad_crc; }
uint32_t RS485_GetResyncs(void)       { return resyncs; }
uint32_t RS485_GetQueueFull(void)     { return queue_full; }
uint32_t RS485_GetDmaErrors(void)     { return dma_errors; }
