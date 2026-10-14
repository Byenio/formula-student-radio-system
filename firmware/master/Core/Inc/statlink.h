/**
  ******************************************************************************
  * @file    statlink.h
  * @brief   Out-of-band statistics link: framed binary over a debug UART.
  *
  * IDENTICAL ON BOTH BOARDS.
  *
  * Why a separate UART at all: the radio link is the thing being measured.
  * Shipping statistics over the air would consume the airtime under test and
  * change the result. Both boards route a spare UART to the STDC14 debug
  * header (J102 pin 14 = target TX, pin 13 = target RX), which is exactly
  * where an STLINK-V3 exposes its virtual COM port -- so the statistics reach
  * the PC over the same cable as SWD, with no extra hardware, and debugging
  * and logging run at the same time.
  *
  *   board    USART    TX pin   RX pin (unused)
  *   master   USART2   PA2      PA3
  *   slave    USART1   PA9      PA10
  *
  * This module owns its USART completely, at register level. It deliberately
  * does not use the HAL UART driver: HAL's completion and error callbacks are
  * global functions shared by every UART, and the master's rs485.c already
  * implements them for USART1. Keeping out of that path means a stats-link
  * error can never be mistaken for an RS-485 one, and vice versa.
  *
  * CubeMX: leave the master's USART2 UNCONFIGURED. The slave's USART1 is
  * configured in its .ioc but unused by anything else; StatLink_Init simply
  * reprograms it. On either board, do NOT enable that USART's interrupt in
  * CubeMX -- the IRQ handler lives here and a generated one would collide.
  *
  * Wire format, little-endian throughout:
  *
  *   +------+------+------+--------+-----------------+--------+
  *   | 0xA5 | 0x5A | type | len16  | payload (len B) | crc16  |
  *   +------+------+------+--------+-----------------+--------+
  *
  *   crc16: CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) over type, len and
  *   payload -- the same CRC as the USB hop in link_proto.h. Sync bytes differ
  *   from the USB framing (AA 55) so a mixed-up capture is caught at once.
  ******************************************************************************
  */

#ifndef STATLINK_H
#define STATLINK_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include "perf.h"           /* FW_STATS_ENABLE default */

#define STATLINK_SYNC0          0xA5U
#define STATLINK_SYNC1          0x5AU
#define STATLINK_OVERHEAD       7U       /* sync 2 + type 1 + len 2 + crc 2 */

/* 1 Mbaud divides exactly from both boards' USART clocks (170 MHz and
   96 MHz) and from the STLINK-V3's, so neither end has any baud error. Also
   the rate the VCU RS-485 spec uses, so one terminal setting fits both. */
#ifndef STATLINK_BAUD
#define STATLINK_BAUD           1000000U
#endif

/* Power of two. Must hold the largest single frame -- the schema, ~1 kB --
   plus whatever is still draining from the previous report. */
#define STATLINK_RING_SIZE      2048U

#if FW_STATS_ENABLE

/** @brief  Claim and configure the USART and its TX pin. Call once. */
void StatLink_Init(void);

/**
  * @brief  Queue one complete frame. All or nothing: if the ring cannot take
  *         the whole frame it is dropped and counted, never truncated and
  *         never waited for. Statistics must not be able to stall the system
  *         they describe.
  * @note   SINGLE PRODUCER. Call from one context only -- the stats task on
  *         the master, the main loop on the slave.
  * @retval true if queued
  */
bool StatLink_Send(uint8_t type, const uint8_t *payload, uint16_t len);

/** @brief  CRC-16/CCITT-FALSE, the frame CRC. Seed with 0xFFFF; chainable. */
uint16_t StatLink_Crc16(uint16_t crc, const uint8_t *data, uint32_t len);

uint32_t StatLink_GetBytesSent(void);    /*!< bytes handed to the UART      */
uint32_t StatLink_GetDropped(void);      /*!< frames refused for lack of room*/

#if defined(FW_HOST_TEST)
/** @brief  Host test only: take what would have gone out of the UART. */
uint32_t StatLink_HostDrain(uint8_t *out, uint32_t max);
#endif

#endif /* FW_STATS_ENABLE */

#ifdef __cplusplus
}
#endif

#endif /* STATLINK_H */