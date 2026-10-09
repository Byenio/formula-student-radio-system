/**
  ******************************************************************************
  * @file    bb_diff.h
  * @brief   Bit-banged differential transmitter for the bench link.
  *
  * WHY NOT JUST USE THE UART
  * -------------------------
  * Driving one line of an RS-485 pair from a GPIO and grounding the other only
  * produces a real differential signal for one of the two logic levels. The
  * other lands at zero volts difference, where the receiver falls back on its
  * bus-failsafe threshold -- a decision made at the threshold rather than
  * safely away from it. Noise makes it chatter, and since that is half of
  * every byte, the result is a framing error on almost every one. Measured:
  * 1984 errors in 2119 bytes.
  *
  * Reversing the wires does not help: in that orientation both levels sit in
  * failsafe and the receiver output never moves at all.
  *
  * So both lines are driven, in antiphase, from two GPIOs. Every bit is then a
  * solid +/-2.5 V differential and the failsafe threshold is never involved.
  *
  * POLARITY
  * --------
  *   line A (J101 pin 1)  <-  bit
  *   line B (J101 pin 2)  <-  !bit
  *
  * so A-B is positive for a mark and negative for a space, which is exactly
  * what a real RS-485 driver produces. The receiver output is therefore a
  * NORMAL UART signal, and the master board no longer needs RX inversion --
  * one less difference between the bench and the car.
  *
  * WIRING
  *   Nucleo PD4  ->  J101 pin 1 (RS485_A)
  *   Nucleo PD5  ->  J101 pin 2 (RS485_B)
  *   Nucleo GND  ->  master GND
  ******************************************************************************
  */

#ifndef BB_DIFF_H
#define BB_DIFF_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include <stdint.h>

/* Must match the master board's USART1 setting. */
#ifndef BB_DIFF_BAUD
#define BB_DIFF_BAUD            115200U
#endif

/* Line A: mark high. Line B: its complement. */
#define BB_DIFF_A_PORT          GPIOD
#define BB_DIFF_A_PIN           GPIO_PIN_4
#define BB_DIFF_B_PORT          GPIOD
#define BB_DIFF_B_PIN           GPIO_PIN_5

/**
  * @brief Configure both pins and start the cycle counter used for timing.
  *        Call before the main loop, instead of using USART2.
  */
void BbDiff_Init(void);

/**
  * @brief Send a buffer, 8N1, LSB first. Blocking.
  *
  * Interrupts are masked for the duration of each byte -- about 87 us at
  * 115200 -- so that a SysTick or other interrupt cannot stretch a bit and
  * corrupt the framing. A byte is short enough that the tick is delayed rather
  * than lost, so HAL_GetTick() stays accurate.
  */
void BbDiff_Send(const uint8_t *data, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* BB_DIFF_H */
