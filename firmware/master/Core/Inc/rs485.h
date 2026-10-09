/**
  ******************************************************************************
  * @file    rs485.h
  * @brief   Telemetry input from the VCU over RS-485.
  *
  * Link: USART1 at 1 Mbaud, 8N1, half-duplex transceiver (THVD2450DR, U110).
  * The VCU pushes; this board only listens, so RE_DE is held low permanently --
  * receiver enabled, driver disabled. There is no turnaround to get wrong.
  *
  * Frame format (see VCU_RS485_TELEMETRY_SPEC.md, the shared definition):
  *
  *   +------+------+-----+---------------------+--------+--------+
  *   | 0xA5 | 0x5A | LEN |  PAYLOAD (1..251 B) | CRC lo | CRC hi |
  *   +------+------+-----+---------------------+--------+--------+
  *
  *   CRC16-CCITT over LEN and PAYLOAD, not the sync bytes.
  *
  * The payload is one or more CANLOG records, already batched by the VCU. This
  * board does NOT parse them: it forwards the payload verbatim as a single
  * telemetry packet over the air, and the PC splits it. Keeping the radio a
  * dumb pipe means the VCU can change which frames it sends, or the record
  * format itself, without this firmware caring.
  *
  * Reception is DMA + idle-line: HAL_UARTEx_ReceiveToIdle_DMA delivers whatever
  * has arrived whenever the line goes quiet, so a partial frame is handed over
  * promptly rather than waiting for a fixed-size buffer to fill.
  ******************************************************************************
  */

#ifndef RS485_H
#define RS485_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include "link_proto.h"
#include <stdint.h>
#include <stdbool.h>

/* ---- Bench test mode -----------------------------------------------------
   Set to 1 when the telemetry source is a bare 3.3 V UART wired into the
   differential pair at J101, rather than a real RS-485 driver:

       J101 pin 1 (RS485_A)  ->  source GND
       J101 pin 2 (RS485_B)  ->  source TX
       GND                   ->  source GND

   The transceiver stays fully enabled -- RE_DE low, exactly as in the car --
   because it is doing the receiving. What changes is polarity. With A grounded
   and B driven:

       TX high  ->  A-B is negative      ->  receiver output LOW
       TX low   ->  A and B both at 0 V  ->  bus failsafe, output HIGH

   so the MCU sees an inverted UART. The fix is one setting: RX pin inversion,
   applied in USER CODE BEGIN USART1_Init 2 under this same flag. A real
   transceiver on the far end delivers the correct polarity, so this must be 0
   in the car or nothing will decode.

   Note this loads the source pin with the board's 120 ohm termination, drawing
   around 20 mA. Fine for a bench fixture, not a design to ship. */
#ifndef RS485_BENCH_TEST
#define RS485_BENCH_TEST        0
#endif

/* Largest frame on the wire: sync(2) + len(1) + payload(251) + crc(2). */
#define RS485_MAX_FRAME         (3U + LINK_MAX_OTA_PAYLOAD + 2U)

/* DMA ring. Several frames deep so a busy burst cannot outrun the task. */
#define RS485_DMA_BUF_LEN       1024U

/* Completed payloads waiting for the radio. Each becomes one air packet. */
#define RS485_QUEUE_DEPTH       6U

/**
  * @brief Start receiving. Call after the scheduler is running, alongside the
  *        other init calls.
  */
void RS485_Init(void);

/**
  * @brief Collect the next validated telemetry payload.
  * @param out        buffer of at least LINK_MAX_OTA_PAYLOAD bytes
  * @param len        payload length in bytes
  * @param timeout_ms 0 to poll
  * @retval true if a payload was returned
  */
bool RS485_GetPayload(uint8_t *out, uint8_t *len, uint32_t timeout_ms);

/* ---- Bring-up counters, all watchable in Live Expressions ---------------- */

/**
  * @brief Length of the next queued payload, or 0 if none.
  *
  * Lets the radio layer merge several payloads into one air packet without
  * dequeuing one it cannot use -- there is no way to put an item back.
  */
uint8_t RS485_PeekLen(void);

uint32_t RS485_GetBytesReceived(void);  /*!< raw bytes off the wire          */
uint32_t RS485_GetFramesGood(void);     /*!< frames that passed CRC          */
uint32_t RS485_GetFramesBadCrc(void);   /*!< wrong baud, noise, or a false
                                             sync inside a payload           */
uint32_t RS485_GetResyncs(void);        /*!< bytes discarded hunting sync    */
uint32_t RS485_GetQueueFull(void);      /*!< radio not draining fast enough  */
uint32_t RS485_GetDmaErrors(void);      /*!< overrun/framing/noise from HAL  */
uint32_t RS485_GetRecoveries(void);     /*!< successful RX restarts          */
uint32_t RS485_GetLastErrorCode(void);  /*!< HAL_UART_ERROR_* bitmask        */

#ifdef __cplusplus
}
#endif

#endif /* RS485_H */
