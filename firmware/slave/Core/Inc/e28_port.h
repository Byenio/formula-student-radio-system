/**
  ******************************************************************************
  * @file    e28_port.h
  * @brief   The three OS services the radio driver needs, abstracted.
  *
  * The car board runs FreeRTOS. The base station cannot: USBX on STM32U3 is
  * only offered with FreeRTOS disabled, and ThreadX ships no CMSIS-RTOS2
  * adapter -- so the relay is bare-metal with USBX in standalone mode.
  *
  * Rather than fork the driver, everything OS-dependent funnels through here.
  * It comes to three things: delay, wait for the DIO1 interrupt, and signal
  * that interrupt from an ISR.
  *
  * ONE IMPORTANT DIFFERENCE
  * ------------------------
  * Under an RTOS, E28_Port_WaitDio1() blocks the calling task and other tasks
  * keep running. Bare-metal it spins, and nothing else runs at all -- which
  * would starve the USB stack. So the bare-metal relay never calls it; it uses
  * E28_Port_Dio1Pending() and a state machine instead, and the main loop stays
  * free to service USB. Blocking waits are for the RTOS side only.
  ******************************************************************************
  */

#ifndef E28_PORT_H
#define E28_PORT_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include <stdint.h>
#include <stdbool.h>

/* 1 = CMSIS-RTOS2 available (car board). 0 = bare metal (base station).
   Set E28_USE_RTOS to 0 in the base station's main.h. */
#ifndef E28_USE_RTOS
#define E28_USE_RTOS        1
#endif

/**
  * @brief Record who should be woken by DIO1. Call once from whichever context
  *        will consume the interrupt, before arming the radio.
  */
void E28_Port_Bind(void);

/**
  * @brief Millisecond delay. Yields under an RTOS, busy-waits without one.
  */
void E28_Port_Delay(uint32_t ms);

/**
  * @brief Milliseconds since boot.
  */
uint32_t E28_Port_Now(void);

/**
  * @brief Flag a DIO1 edge. ISR context.
  */
void E28_Port_SignalDio1(void);

/**
  * @brief Non-blocking test-and-clear. Safe in both builds, and the only way
  *        the bare-metal relay looks for interrupts.
  */
bool E28_Port_Dio1Pending(void);

/**
  * @brief Discard any pending DIO1 notification.
  *
  * Call immediately before starting a transaction whose completion you intend
  * to wait for, so a leftover edge from the previous one cannot satisfy the
  * wait early. Do NOT call it before waiting for a receive: a packet that
  * arrived while you were busy has already set the flag, and clearing it
  * throws that packet away.
  */
void E28_Port_ClearDio1(void);

/**
  * @brief Block until DIO1 fires or the timeout expires.
  * @note  RTOS builds only -- see the header comment.
  * @retval true if the interrupt arrived
  */
bool E28_Port_WaitDio1(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* E28_PORT_H */
