/**
  ******************************************************************************
  * @file    safety.c
  * @brief   Last-resort fault handling. See safety.h for why this exists.
  ******************************************************************************
  */

#include "safety.h"
#include "main.h"

/* ---------------------------------------------------------------------------
   State
   --------------------------------------------------------------------------- */

static safety_record_t record;

/* ---------------------------------------------------------------------------
   Helpers
   --------------------------------------------------------------------------- */

/**
  * @brief Copy a name into the record, truncating and always terminating.
  *
  * Hand-rolled rather than strncpy because this runs after the system has
  * already failed once: pulling in a libc routine here means trusting a stack
  * that may be the very thing that overflowed.
  */
static void safety_set_name(const char *src)
{
  uint32_t i = 0U;

  if (src != NULL)
  {
    while ((i < (SAFETY_NAME_LEN - 1U)) && (src[i] != '\0'))
    {
      record.name[i] = src[i];
      i++;
    }
  }

  while (i < SAFETY_NAME_LEN)
  {
    record.name[i] = '\0';
    i++;
  }
}

/**
  * @brief Rough busy-wait, in milliseconds.
  *
  * HAL_Delay is unusable here: it waits on the SysTick interrupt and we have
  * just disabled interrupts. The loop is approximate -- the divisor assumes a
  * handful of cycles per iteration and the compiler is free to disagree -- but
  * a blink code only has to be countable by eye, not accurate.
  */
static void safety_busy_wait_ms(uint32_t ms)
{
  volatile uint32_t iterations = (SystemCoreClock / 6000U) * ms;

  while (iterations > 0U)
  {
    iterations--;
  }
}

/**
  * @brief Blink the fault code on STATUS_LED2 forever: N pulses, then a pause.
  */
static void safety_blink_forever(uint32_t code)
{
  if (code == 0U)
  {
    code = 1U;
  }

  for (;;)
  {
    for (uint32_t pulse = 0U; pulse < code; pulse++)
    {
      HAL_GPIO_WritePin(STATUS_LED2_GPIO_Port, STATUS_LED2_Pin, GPIO_PIN_SET);
      safety_busy_wait_ms(200U);
      HAL_GPIO_WritePin(STATUS_LED2_GPIO_Port, STATUS_LED2_Pin, GPIO_PIN_RESET);
      safety_busy_wait_ms(200U);
    }

    safety_busy_wait_ms(1200U);
  }
}

/**
  * @brief Put the external interfaces into their harmless states.
  *
  * The radio first: both enable lines low leaves the E28 front end off, so a
  * dead board cannot sit on the channel with the PA keyed. Then RE_DE low,
  * which puts the RS-485 transceiver back in receive and releases the bus for
  * whoever else is on it.
  *
  * Written straight to the GPIO peripheral through the HAL rather than going
  * via the e28 driver: that driver has its own state machine and locks, and
  * neither can be trusted at this point.
  */
static void safety_quiesce_peripherals(void)
{
  HAL_GPIO_WritePin(E28_TX_EN_GPIO_Port, E28_TX_EN_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(E28_RX_EN_GPIO_Port, E28_RX_EN_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(RS485_RE_DE_GPIO_Port, RS485_RE_DE_Pin, GPIO_PIN_RESET);
}

/* ---------------------------------------------------------------------------
   Public API
   --------------------------------------------------------------------------- */

void Safety_Fault(safety_fault_t fault, const char *name)
{
  __disable_irq();

  record.magic = SAFETY_MAGIC;
  record.fault = (uint32_t)fault;
  record.count++;
  safety_set_name(name);

  safety_quiesce_peripherals();

#if (SAFETY_HALT_ON_FAULT != 0)
  safety_blink_forever((uint32_t)fault);
#else
  /* Interrupts stay disabled: the reset is immediate and there is nothing
     left worth servicing between here and it. */
  NVIC_SystemReset();
#endif

  /* Neither branch returns, but a fault handler that falls through silently
     would be the exact bug this module was written to remove. */
  for (;;)
  {
  }
}

void Safety_AssertFailed(const char *file, unsigned long line)
{
  const char *basename = file;

  /* Keep only the file name: the record has 16 bytes and a full CubeMX path
     would fill them with directories that are identical for every assert. */
  if (file != NULL)
  {
    for (const char *p = file; *p != '\0'; p++)
    {
      if ((*p == '/') || (*p == '\\'))
      {
        basename = p + 1;
      }
    }
  }

  record.line = line;
  Safety_Fault(SAFETY_FAULT_ASSERT, basename);
}

const safety_record_t *Safety_GetRecord(void)
{
  return &record;
}

bool Safety_HasFaulted(void)
{
  return (record.magic == SAFETY_MAGIC);
}
