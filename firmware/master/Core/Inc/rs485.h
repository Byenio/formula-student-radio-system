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
   Set to 1 when the telemetry source is the Nucleo telemetry simulator
   rather than the real VCU. The simulator bit-bangs both lines of the pair
   in antiphase (firmware/telemetry-sim, bb_diff.c):

       Nucleo PD4  ->  J101 pin 1 (RS485_A)
       Nucleo PD5  ->  J101 pin 2 (RS485_B)
       Nucleo GND  ->  master GND

   That produces the same polarity as a real RS-485 driver, so the transceiver
   works exactly as in the car -- RE_DE low, no RX inversion. What the flag
   changes, in USER CODE BEGIN USART1_Init 2 of main.c:

     - baud drops to RS485_BENCH_BAUD, because the bit-banged source cannot
       reach the real link's 1 Mbaud;
     - overrun detection is off and DMA keeps running through a receive
       error, so a glitch on the bench wiring cannot stop the link.

   Must be 0 in the car, or the master listens at the wrong baud.

   History, because it explains the wiring: the first bench setup drove only
   B from a plain UART, with A grounded. Half of every bit then sat at zero
   differential and relied on the transceiver's bus-failsafe threshold, which
   chattered under noise; it needed RX inversion and still failed. Driving
   both lines removed both problems.

   The Nucleo pins drive the board's 120 ohm termination directly, sourcing
   around 15-20 mA each. Fine for a bench fixture, not a design to ship. */
#ifndef RS485_BENCH_TEST
#define RS485_BENCH_TEST        0
#endif

/* Bench link baud. The real VCU link is 1 Mbaud (CubeMX's setting for
   USART1); the bench fixture's bit-banged transmitter is not fast enough for
   that, so with RS485_BENCH_TEST = 1 the master listens at this rate instead.
   Must equal BB_DIFF_BAUD in the telemetry sim's bb_diff.h.

   Bench measurements are therefore taken at a slower wire rate than the car:
   a 45-byte telemetry frame spends ~3.9 ms on the wire instead of ~0.45 ms.
   Throughput is unaffected (the link is far from full at either rate), but
   any end-to-end telemetry latency measured on the bench includes the extra
   ~3.5 ms per frame, and must be reported with that caveat. */
#ifndef RS485_BENCH_BAUD
#define RS485_BENCH_BAUD        115200U
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
/* Line errors. Counted only -- reception continues through them, and the
   frame CRC decides whether data is good. Lower bounds: polled, so two of
   the same flag between polls count as one. */
uint32_t RS485_GetNoiseErrors(void);    /*!< start/data bit samples disagreed */
uint32_t RS485_GetFramingErrors(void);  /*!< stop bit not where expected      */
uint32_t RS485_GetOverrunErrors(void);  /*!< byte lost: RDR not read in time  */

#ifdef __cplusplus
}
#endif

#endif /* RS485_H */