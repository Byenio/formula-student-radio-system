/**
  ******************************************************************************
  * @file    ptt.h
  * @brief   Microphone keying for the driver's radio.
  *
  * TOGGLE, NOT HOLD
  * ----------------
  * Press once to open the mic, press again to close it. A driver mid-corner
  * has neither the hand nor the attention to hold a button, so hold-to-talk is
  * the wrong ergonomics here even though the industry calls it PTT. The name
  * is kept because everyone knows what it means.
  *
  * HANGOVER
  * --------
  * Transmission continues for a short while after the mic is closed. Two
  * reasons: it stops the last syllable being clipped, and it stops a
  * double-press thrashing the radio between transmit and receive. Every real
  * radio does this.
  *
  * WHY KEYING MATTERS SO MUCH HERE
  * -------------------------------
  * The link is half-duplex on one antenna, so a board that is transmitting is
  * deaf. Measured: with both ends transmitting continuously, roughly a third
  * of packets were lost purely to turnaround. Keying means only one end talks
  * at a time, which removes that contention rather than working around it.
  ******************************************************************************
  */

#ifndef PTT_H
#define PTT_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include <stdint.h>
#include <stdbool.h>

/* How long transmission continues after the mic is toggled closed. */
#ifndef PTT_HANGOVER_MS
#define PTT_HANGOVER_MS         200U
#endif

/* A press must be seen this many polls running before it counts. At the
   20 ms poll rate that is 40 ms of contact settling -- comfortably longer
   than a tactile switch bounces, and far shorter than a human press. */
#ifndef PTT_DEBOUNCE_SAMPLES
#define PTT_DEBOUNCE_SAMPLES    2U
#endif

/**
  * @brief Reset keying state. Mic starts closed.
  */
void Ptt_Init(void);

/**
  * @brief Sample the button and age the hangover timer. Call every ~20 ms.
  */
void Ptt_Poll(void);

/**
  * @brief Should the radio be transmitting voice right now?
  *        True while the mic is open AND during the hangover afterwards.
  *        This is what the radio layer keys off.
  */
bool Ptt_IsTransmitting(void);

/**
  * @brief Is the mic open? Excludes hangover, so this is what a status LED
  *        or the pit-wall UI should show.
  */
bool Ptt_IsOpen(void);

/**
  * @brief Open or close the mic from somewhere other than the button --
  *        the CAN packet from the steering wheel, or the PC operator.
  *        Takes effect immediately, same as a press.
  */
void Ptt_SetRemote(bool open);

/**
  * @brief How many times the mic has been keyed. Handy for confirming the
  *        button and debounce are behaving without a scope.
  */
uint32_t Ptt_GetToggleCount(void);

#ifdef __cplusplus
}
#endif

#endif /* PTT_H */
