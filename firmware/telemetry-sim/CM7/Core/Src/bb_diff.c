/**
  ******************************************************************************
  * @file    bb_diff.c
  * @brief   Bit-banged differential transmitter. See bb_diff.h.
  ******************************************************************************
  */

#include "bb_diff.h"

/* Cycles per bit, computed at init from the real core clock rather than
   assumed -- this project runs on HSI at 64 MHz, not the 480 MHz the board is
   capable of, and hard-coding either would silently break the other. */
static uint32_t cycles_per_bit;

/* -------------------------------------------------------------------------- */

static inline void bb_set_bit(uint32_t bit)
{
  /* Written as two BSRR accesses rather than HAL_GPIO_WritePin so the two
     lines change within a couple of cycles of each other. A large skew would
     briefly put both lines at the same level, which is the failsafe state this
     whole approach exists to avoid. */
  if (bit)
  {
    BB_DIFF_A_PORT->BSRR = BB_DIFF_A_PIN;                  /* A high */
    BB_DIFF_B_PORT->BSRR = (uint32_t)BB_DIFF_B_PIN << 16;  /* B low  */
  }
  else
  {
    BB_DIFF_A_PORT->BSRR = (uint32_t)BB_DIFF_A_PIN << 16;  /* A low  */
    BB_DIFF_B_PORT->BSRR = BB_DIFF_B_PIN;                  /* B high */
  }
}

void BbDiff_Init(void)
{
  GPIO_InitTypeDef g = {0};

  __HAL_RCC_GPIOD_CLK_ENABLE();

  g.Pin   = BB_DIFF_A_PIN;
  g.Mode  = GPIO_MODE_OUTPUT_PP;
  g.Pull  = GPIO_NOPULL;
  /* Fast rather than the maximum: the lines drive a 120 ohm termination, and
     the fastest slew setting buys nothing but overshoot on wires this short. */
  g.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(BB_DIFF_A_PORT, &g);

  g.Pin = BB_DIFF_B_PIN;
  HAL_GPIO_Init(BB_DIFF_B_PORT, &g);

  /* Idle is a mark. */
  bb_set_bit(1U);

  /* DWT cycle counter, used as the bit clock. More precise than a delay loop
     and immune to compiler optimisation changing the loop's length. */
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0U;
  DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;

  cycles_per_bit = SystemCoreClock / BB_DIFF_BAUD;
}

void BbDiff_Send(const uint8_t *data, uint16_t len)
{
  if (data == NULL || len == 0U || cycles_per_bit == 0U)
  {
    return;
  }

  for (uint16_t i = 0U; i < len; i++)
  {
    uint8_t  byte = data[i];
    uint32_t next;

    /* One byte at a time. Masking interrupts for a whole frame would lose
       SysTick ticks; masking for 10 bits only delays one, so HAL_GetTick()
       stays correct and the record pacing does not drift. */
    __disable_irq();

    next = DWT->CYCCNT;

    /* Start bit. */
    bb_set_bit(0U);
    next += cycles_per_bit;
    while ((int32_t)(DWT->CYCCNT - next) < 0) { }

    /* Eight data bits, LSB first. */
    for (uint8_t b = 0U; b < 8U; b++)
    {
      bb_set_bit((byte >> b) & 1U);
      next += cycles_per_bit;
      while ((int32_t)(DWT->CYCCNT - next) < 0) { }
    }

    /* Stop bit, and back to idle. */
    bb_set_bit(1U);
    next += cycles_per_bit;
    while ((int32_t)(DWT->CYCCNT - next) < 0) { }

    __enable_irq();
  }
}
