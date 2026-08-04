/**
  ******************************************************************************
  * @file    ptt.c
  * @brief   Toggle-to-talk keying. See ptt.h.
  ******************************************************************************
  */

#include "ptt.h"

static volatile bool     mic_open;
static volatile bool     hangover_active;
static volatile uint32_t hangover_started;
static volatile uint32_t toggle_count;

/* Debounce state. The button pulls PC13 to ground, and the pin has an
   internal pull-up, so pressed reads LOW. */
static uint8_t  stable_pressed;
static uint8_t  candidate_count;
static bool     last_stable;
static volatile bool can_authority;

void Ptt_Init(void)
{
  mic_open         = false;
  hangover_active  = false;
  hangover_started = 0U;
  stable_pressed   = 0U;
  candidate_count  = 0U;
  last_stable      = false;
  can_authority    = false;
}

void Ptt_SetCanAuthority(bool can_alive)
{
  can_authority = can_alive;
}

bool Ptt_IsButtonActive(void)
{
  return !can_authority;
}

static void ptt_toggle(void)
{
  if (mic_open)
  {
    /* Closing: start the hangover rather than dropping the carrier at once,
       so the tail of the last word still gets out. */
    mic_open         = false;
    hangover_active  = true;
    hangover_started = HAL_GetTick();
  }
  else
  {
    /* Opening cancels any hangover still running -- a quick double press
       should end up open, not stuck mid-release. */
    mic_open        = true;
    hangover_active = false;
  }

  toggle_count++;
}

void Ptt_Poll(void)
{
  bool raw_pressed =
      (HAL_GPIO_ReadPin(PTT_BTN_GPIO_Port, PTT_BTN_Pin) == GPIO_PIN_RESET);

  /* Debounce by agreement rather than by delay: a reading only becomes
     official once it has repeated. Nothing blocks, so this is safe to call
     from a task that has other work. */
  if (raw_pressed != (bool)stable_pressed)
  {
    candidate_count++;

    if (candidate_count >= PTT_DEBOUNCE_SAMPLES)
    {
      stable_pressed  = raw_pressed ? 1U : 0U;
      candidate_count = 0U;
    }
  }
  else
  {
    candidate_count = 0U;
  }

  /* Act on the press, not the release. Keying on release would feel laggy,
     and would also key on the way out of an accidental long hold. */
  bool now_stable = (stable_pressed != 0U);

  if (now_stable && !last_stable)
  {
    /* Button only keys when CAN is not in charge. Still tracked while locked
       out, so releasing CAN authority does not immediately fire a stale
       press. */
    if (!can_authority)
    {
      ptt_toggle();
    }
  }
  last_stable = now_stable;

  /* Age the hangover. */
  if (hangover_active &&
      ((HAL_GetTick() - hangover_started) >= PTT_HANGOVER_MS))
  {
    hangover_active = false;
  }
}

bool Ptt_IsTransmitting(void)
{
  return (mic_open || hangover_active);
}

bool Ptt_IsOpen(void)
{
  return mic_open;
}

void Ptt_SetRemote(bool open)
{
  if (open != mic_open)
  {
    ptt_toggle();
  }
}

uint32_t Ptt_GetToggleCount(void)
{
  return toggle_count;
}
