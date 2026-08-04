/**
  ******************************************************************************
  * @file    rs485.c
  * @brief   Telemetry input from the VCU over RS-485. See rs485.h.
  ******************************************************************************
  */

#include "rs485.h"
#include "cmsis_os2.h"
#include "e28_port.h"
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

/* Single-producer/single-consumer ring rather than a CMSIS queue.
   The consumer must know how large the next payload is BEFORE taking it, so it
   can decide whether to merge it into an air packet it is already building. A
   CMSIS queue cannot be peeked, and dequeuing an item that then does not fit
   would mean discarding telemetry that arrived perfectly well.

   Written only by the RS-485 task, read only by the radio task, so volatile
   indices suffice: each side owns one index and neither writes the other's. */
static rs485_payload_t   payloads[RS485_QUEUE_DEPTH];
static volatile uint32_t pl_head;      /* producer advances */
static volatile uint32_t pl_tail;      /* consumer advances */

static osThreadId_t      rs485_task_handle;

/* ---- Counters ------------------------------------------------------------ */

static volatile uint32_t bytes_received;
static volatile uint32_t frames_good;
static volatile uint32_t frames_bad_crc;
static volatile uint32_t resyncs;
static volatile uint32_t queue_full;
static volatile uint32_t dma_errors;
static volatile uint32_t recoveries;
static volatile uint32_t last_error_code;   /*!< huart1.ErrorCode at the fault */
static volatile bool     needs_recovery;

#define RS485_FLAG_DATA     (1U << 0)

/**
  * @brief Tear the receiver down and start it again from a known state.
  *
  * A framing or overrun error leaves the HAL in HAL_UART_STATE_ERROR, and any
  * attempt to restart reception from that state returns HAL_BUSY. Restarting
  * from inside the error callback therefore fails silently and the link dies
  * permanently after the first glitch -- which is exactly what one stray byte
  * at startup produces.
  *
  * So recovery happens here, in task context, and aborts first. The abort is
  * the part that clears the error state; without it the restart is a no-op.
  */
static void rs485_restart_rx(void)
{
  (void)HAL_UART_AbortReceive(&huart1);

  /* Clear anything latched. ORE in particular stays set and immediately
     re-triggers if it is not explicitly cleared. */
  __HAL_UART_CLEAR_FLAG(&huart1, UART_CLEAR_OREF | UART_CLEAR_NEF |
                                 UART_CLEAR_FEF  | UART_CLEAR_PEF);

  huart1.ErrorCode = HAL_UART_ERROR_NONE;

  /* Discard whatever was mid-flight: after an error the buffer contents and
     our read position are both meaningless, and carrying them forward would
     just produce phantom resyncs. */
  dma_read_pos = 0U;
  frame_fill   = 0U;

  if (HAL_UARTEx_ReceiveToIdle_DMA(&huart1, dma_buf, RS485_DMA_BUF_LEN) == HAL_OK)
  {
    __HAL_DMA_DISABLE_IT(huart1.hdmarx, DMA_IT_HT);
    recoveries++;
  }
}

static const osThreadAttr_t rs485_task_attributes = {
  .name       = "rs485Task",
  /* Above the radio task, below audio.
     At equal priority the radio task starves this one whenever the driver
     keys the mic: it then runs flat out sending a voice frame every 10 ms,
     and RS-485 bytes pile up in the DMA ring undecoded. Telemetry is supposed
     to outrank voice, so the task that receives it must outrank the task that
     sends voice. It only ever runs in short bursts -- parse a frame, queue it,
     block again -- so it cannot starve anything itself. */
  .priority   = (osPriority_t) osPriorityAboveNormal,
  .stack_size = 512 * 4
};

static void RS485_Task(void *argument);

/* -------------------------------------------------------------------------- */

void RS485_Init(void)
{
  /* Receive only: RE low enables the receiver, DE low disables the driver.
     Both are the same pin here, so one write does it and it never changes.

     The same in bench mode: the transceiver is still doing the receiving, and
     only the signal polarity differs (see RS485_BENCH_TEST in rs485.h). */
  HAL_GPIO_WritePin(RS485_RE_DE_GPIO_Port, RS485_RE_DE_Pin, GPIO_PIN_RESET);

  pl_head = 0U;
  pl_tail = 0U;

  rs485_task_handle = osThreadNew(RS485_Task, NULL, &rs485_task_attributes);
  if (rs485_task_handle == NULL)
  {
    Error_Handler();
  }
}

bool RS485_GetPayload(uint8_t *out, uint8_t *len, uint32_t timeout_ms)
{
  if (out == NULL || len == NULL)
  {
    return false;
  }

  /* Poll rather than block. The radio task has other work to check between
     payloads, so it never wants to sleep here; a short spin covers the case
     where a frame is a millisecond away. */
  uint32_t start = E28_Port_Now();

  while (pl_head == pl_tail)
  {
    if ((E28_Port_Now() - start) >= timeout_ms)
    {
      return false;
    }
    osThreadYield();
  }

  const rs485_payload_t *item = &payloads[pl_tail];

  memcpy(out, item->data, item->len);
  *len = item->len;

  pl_tail = (pl_tail + 1U) % RS485_QUEUE_DEPTH;
  return true;
}

uint8_t RS485_PeekLen(void)
{
  if (pl_head == pl_tail)
  {
    return 0U;
  }
  return payloads[pl_tail].len;
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
    uint32_t next = (pl_head + 1U) % RS485_QUEUE_DEPTH;

    /* Never block: this runs in the receive path, and stalling it would let
       the DMA ring overrun. Dropping the newest keeps the older ones in
       order, which matters because telemetry is timestamped. */
    if (next != pl_tail)
    {
      payloads[pl_head].len = payload_len;
      memcpy(payloads[pl_head].data, &frame[3], payload_len);
      pl_head = next;
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

    if (needs_recovery)
    {
      needs_recovery = false;
      rs485_restart_rx();
      continue;
    }

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
    last_error_code = huart->ErrorCode;

    /* Record and hand off. Restarting here cannot work: the HAL is still in
       its error state and would reject the call. The task does it properly. */
    needs_recovery = true;

    if (rs485_task_handle != NULL)
    {
      osThreadFlagsSet(rs485_task_handle, RS485_FLAG_DATA);
    }
  }
}

/* ---- Counters ------------------------------------------------------------ */

uint32_t RS485_GetBytesReceived(void) { return bytes_received; }
uint32_t RS485_GetFramesGood(void)    { return frames_good; }
uint32_t RS485_GetFramesBadCrc(void)  { return frames_bad_crc; }
uint32_t RS485_GetResyncs(void)       { return resyncs; }
uint32_t RS485_GetQueueFull(void)     { return queue_full; }
uint32_t RS485_GetDmaErrors(void)     { return dma_errors; }
uint32_t RS485_GetRecoveries(void)    { return recoveries; }
uint32_t RS485_GetLastErrorCode(void) { return last_error_code; }
