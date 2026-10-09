/**
  ******************************************************************************
  * @file    e28_port.c
  * @brief   RTOS and bare-metal implementations of the ports. See e28_port.h.
  ******************************************************************************
  */

#include "e28_port.h"

#if E28_USE_RTOS

#include "cmsis_os2.h"

#define PORT_FLAG_DIO1      (1U << 0)

static osThreadId_t bound_thread;

void E28_Port_Bind(void)
{
  bound_thread = osThreadGetId();
}

void E28_Port_Delay(uint32_t ms)
{
  osDelay(ms);
}

uint32_t E28_Port_Now(void)
{
  return HAL_GetTick();
}

void E28_Port_SignalDio1(void)
{
  if (bound_thread != NULL)
  {
    /* osThreadFlagsSet is ISR-safe provided the interrupt priority is
       numerically >= configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY, which is
       why the EXTI line is configured at priority 5. */
    (void)osThreadFlagsSet(bound_thread, PORT_FLAG_DIO1);
  }
}

bool E28_Port_Dio1Pending(void)
{
  uint32_t flags = osThreadFlagsWait(PORT_FLAG_DIO1, osFlagsWaitAny, 0U);
  return ((flags & osFlagsError) == 0U);
}

void E28_Port_ClearDio1(void)
{
  (void)osThreadFlagsClear(PORT_FLAG_DIO1);
}

bool E28_Port_WaitDio1(uint32_t timeout_ms)
{
  /* Deliberately does NOT clear first. An edge that arrived while the caller
     was busy is a real event -- usually a packet already sitting in the radio
     FIFO -- and discarding it loses that packet. Callers that need a clean
     slate (i.e. before a transmit) call E28_Port_ClearDio1() themselves. */
  uint32_t flags = osThreadFlagsWait(PORT_FLAG_DIO1, osFlagsWaitAny, timeout_ms);
  return ((flags & osFlagsError) == 0U);
}

#else   /* ---------------- bare metal ---------------- */

/* Written from the EXTI handler, read from the main loop, so volatile is
   mandatory -- without it the compiler is entitled to cache the read. */
static volatile bool dio1_flag;

void E28_Port_Bind(void)
{
  dio1_flag = false;
}

void E28_Port_ClearDio1(void)
{
  dio1_flag = false;
}

void E28_Port_Delay(uint32_t ms)
{
  HAL_Delay(ms);
}

uint32_t E28_Port_Now(void)
{
  return HAL_GetTick();
}

void E28_Port_SignalDio1(void)
{
  dio1_flag = true;
}

bool E28_Port_Dio1Pending(void)
{
  if (dio1_flag)
  {
    dio1_flag = false;
    return true;
  }
  return false;
}

bool E28_Port_WaitDio1(uint32_t timeout_ms)
{
  /* Provided for completeness and used only during start-up, before USB is
     running. The relay itself must never call this: spinning here stalls the
     whole program, including the USB stack.

     As with the RTOS version, no clear on entry -- an already-pending edge is
     a real event. */
  uint32_t start = HAL_GetTick();

  while (!dio1_flag)
  {
    if ((HAL_GetTick() - start) > timeout_ms)
    {
      return false;
    }
  }

  dio1_flag = false;
  return true;
}

#endif  /* E28_USE_RTOS */
