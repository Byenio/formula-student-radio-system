/**
  ******************************************************************************
  * @file    safety.c
  * @brief   Last-resort fault handling. See safety.h for the overview.
  *
  * IDENTICAL ON BOTH BOARDS -- see the board section below for the only
  * places they differ.
  *
  * CPU fault handlers
  * ------------------
  * This file defines HardFault_Handler, MemManage_Handler, BusFault_Handler
  * and UsageFault_Handler. CubeMX also generates all four, in
  * stm32xxxx_it.c, as empty infinite loops. Those copies are renamed out of
  * the way by four #defines in that file's USER CODE Includes block, so the
  * vector table resolves to the versions here. The renamed copies are never
  * referenced and --gc-sections removes them.
  *
  * The rename keeps every edit inside a USER CODE block, so a CubeMX
  * regeneration preserves it. The CubeMX-native alternative -- unticking
  * "Generate IRQ handler" for each fault in the NVIC Code generation tab --
  * also works, but then the .ioc and the code have to be changed together,
  * and forgetting one gives a duplicate-symbol link error.
  *
  * The handlers must be naked. On exception entry the hardware pushes r0-r3,
  * r12, lr, pc and xPSR onto whichever stack was active, and LR holds an
  * EXC_RETURN value whose bit 2 says which one: 0 = MSP (handler mode, or a
  * bare-metal main loop), 1 = PSP (a FreeRTOS task). A normal C function
  * would push its own registers first and move MSP, after which the frame
  * can no longer be found reliably. A naked function runs no prologue, so
  * the first instructions see the stacks exactly as the hardware left them.
  ******************************************************************************
  */

#include "safety.h"
#include "main.h"

/* ==========================================================================
   Board configuration -- the only part that differs between the boards
   ==========================================================================

   SAFETY_HAS_RTOS     FreeRTOS present: name the faulting task.
   SAFETY_HAS_LED      an MCU-driven LED exists for the blink code.

   The slave has no LED the MCU can drive -- D103 is a power indicator wired
   straight to +5 V -- so a halted slave reports its fault differently: it
   disconnects from USB. The host sees the device vanish and stay gone, which
   radio_station.py shows as the port disappearing, rather than a device that
   is still enumerated but has silently stopped answering. */

#if defined(STM32G491xx)                      /* master: on the car         */
  #define SAFETY_HAS_RTOS   1
  #define SAFETY_HAS_LED    1
#elif defined(STM32U375xx)                    /* slave: pit-wall relay      */
  #define SAFETY_HAS_RTOS   0
  #define SAFETY_HAS_LED    0
#else
  #error "safety.c: unknown board -- add it to the board configuration block"
#endif

#if SAFETY_HAS_RTOS
#include "FreeRTOS.h"
#include "task.h"
#endif

/* Both linker scripts start RAM here. Used to sanity-check a stack pointer
   before dereferencing it: after a stack overflow, SP can point anywhere,
   and a second fault inside a fault handler locks the core up for good. */
#define SAFETY_RAM_START        0x20000000UL
extern uint32_t _estack;                      /* end of RAM, from the .ld   */

/* EXC_RETURN bit 2: the exception frame is on the PSP, i.e. a task faulted. */
#define SAFETY_EXC_RETURN_PSP   (1UL << 2)

/* ==========================================================================
   State
   ========================================================================== */

static safety_record_t record;

/* ==========================================================================
   Helpers
   ========================================================================== */

/**
  * @brief Copy a name into the record, truncating and always terminating.
  *
  * Hand-rolled rather than strncpy: this runs after the system has already
  * failed once, so calling into libc means trusting state that may be the
  * very thing that broke.
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
  * @brief Put the external interfaces into their harmless states.
  *
  * Writes GPIO and peripheral registers directly rather than going through
  * the drivers: the e28 driver has its own state machine and the HAL has
  * locks, and none of it can be trusted at this point.
  *
  * Safe even if called before MX_GPIO_Init (Error_Handler can fire from
  * SystemClock_Config): a write to a peripheral whose clock is still off is
  * simply ignored, and the pins are still at their reset state anyway.
  */
static void safety_quiesce_peripherals(void)
{
  /* Both boards: E28 front end off, so the PA cannot stay keyed. */
  HAL_GPIO_WritePin(E28_TX_EN_GPIO_Port, E28_TX_EN_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(E28_RX_EN_GPIO_Port, E28_RX_EN_Pin, GPIO_PIN_RESET);

#if defined(STM32G491xx)
  /* Master: RS-485 transceiver back to receive, releasing the VCU link. */
  HAL_GPIO_WritePin(RS485_RE_DE_GPIO_Port, RS485_RE_DE_Pin, GPIO_PIN_RESET);
#endif

#if defined(STM32U375xx)
  /* Slave: drop the D+ pull-up, which is a clean USB disconnect as far as
     the host is concerned. Guarded on the clock because, unlike GPIO, the
     USB block may not be enabled yet if this fires during early init. */
  if (__HAL_RCC_USB1_IS_CLK_ENABLED())
  {
    USB_DRD_FS->BCDR &= ~USB_BCDR_DPPU;
  }
#endif
}

#if (SAFETY_HALT_ON_FAULT != 0) && SAFETY_HAS_LED
/**
  * @brief Rough busy-wait in milliseconds.
  *
  * HAL_Delay is unusable: it waits on the tick interrupt, which is now
  * disabled. Approximate -- the divisor assumes a few cycles per iteration --
  * but a blink code only has to be countable by eye.
  */
static void safety_busy_wait_ms(uint32_t ms)
{
  volatile uint32_t iterations = (SystemCoreClock / 6000U) * ms;

  while (iterations > 0U)
  {
    iterations--;
  }
}

/** @brief Blink the fault code forever: N pulses, then a pause. */
static void safety_blink_forever(uint32_t code)
{
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
#endif

/* ==========================================================================
   Public API
   ========================================================================== */

void Safety_Fault(safety_fault_t fault, const char *name)
{
  __disable_irq();

  record.magic = SAFETY_MAGIC;
  record.fault = (uint32_t)fault;
  record.count++;
  safety_set_name(name);

  safety_quiesce_peripherals();

#if (SAFETY_HALT_ON_FAULT != 0)
  #if SAFETY_HAS_LED
  safety_blink_forever((uint32_t)fault);
  #endif
  /* No LED: halt quietly. The record is in RAM for the debugger, and on the
     slave the USB disconnect above is the visible symptom. */
#else
  NVIC_SystemReset();
#endif

  /* Neither branch returns, but a fault handler that falls through silently
     is the exact bug this module exists to remove. */
  for (;;)
  {
  }
}

void Safety_AssertFailed(const char *file, unsigned long line)
{
  const char *basename = file;

  /* Keep only the file name: the record has 16 bytes and a full path would
     fill them with directories identical for every assert. */
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

  record.line = (uint32_t)line;
  Safety_Fault(SAFETY_FAULT_ASSERT, basename);
}

void Safety_ErrorHandler(void *caller)
{
  /* The caller's return address, with the Thumb bit cleared so addr2line
     points at an instruction rather than one byte past it. */
  record.pc = (uint32_t)caller & ~1UL;
  Safety_Fault(SAFETY_FAULT_ERROR_HANDLER, "Error_Handler");
}

void Safety_CpuFault(const uint32_t *frame, uint32_t exc_return, uint32_t vector)
{
  static const char *const names[] =
  {
    "HardFault", "MemManage", "BusFault", "UsageFault"
  };
  const char *name = ((vector >= 3U) && (vector <= 6U)) ? names[vector - 3U] : "CpuFault";

  __disable_irq();

  /* Fault status registers first: they do not depend on the stack at all,
     so they are captured even when the frame turns out to be unusable. */
  record.exc_return = exc_return;
  record.vector     = vector;
  record.cfsr       = SCB->CFSR;
  record.hfsr       = SCB->HFSR;
  record.mmfar      = SCB->MMFAR;
  record.bfar       = SCB->BFAR;

  /* The basic frame is 8 words. Only read it if all 8 sit inside RAM and the
     pointer is aligned -- after a stack overflow the SP may be wild, and a
     fault here would lock the core up without anything latched. */
  const uint32_t addr = (uint32_t)frame;
  const uint32_t ram_end = (uint32_t)&_estack;

  if (((addr & 3UL) == 0UL) && (addr >= SAFETY_RAM_START) && ((addr + 32UL) <= ram_end))
  {
    record.lr   = frame[5];
    record.pc   = frame[6];
    record.xpsr = frame[7];
  }

#if SAFETY_HAS_RTOS
  /* Frame on the PSP means a task was running: name the task instead of the
     exception, which is far more useful. The exception type is still in
     record.vector. pcTaskGetName(NULL) only reads the current TCB -- no
     locks, no allocation -- so it is safe here. */
  if (((exc_return & SAFETY_EXC_RETURN_PSP) != 0UL) &&
      (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED))
  {
    name = pcTaskGetName(NULL);
  }
#endif

  Safety_Fault(SAFETY_FAULT_CPU, name);
}

const safety_record_t *Safety_GetRecord(void)
{
  return &record;
}

bool Safety_HasFaulted(void)
{
  return (record.magic == SAFETY_MAGIC);
}

/* ==========================================================================
   CPU fault handlers
   ==========================================================================
   Four naked trampolines: find the frame, then branch -- not call -- to the
   C half, so nothing is pushed on a stack that may already be broken.

     tst   lr, #4        EXC_RETURN bit 2: which stack holds the frame
     ite   eq
     mrseq r0, msp       0 -> main stack
     mrsne r0, psp       1 -> process stack (a FreeRTOS task)
     mov   r1, lr        arg 2: EXC_RETURN itself
     movs  r2, #N        arg 3: exception number
     b     Safety_CpuFault

   MemManage, BusFault and UsageFault only reach their own handler when
   enabled in SCB->SHCSR. Neither board enables them, so in practice every
   CPU fault escalates to HardFault -- record.cfsr still says which kind it
   really was, and record.hfsr has FORCED set to show the escalation. */

#define SAFETY_FAULT_TRAMPOLINE(handler, number)                \
  __attribute__((naked, used)) void handler(void)               \
  {                                                             \
    __asm volatile(                                             \
      "tst   lr, #4          \n"                                \
      "ite   eq              \n"                                \
      "mrseq r0, msp         \n"                                \
      "mrsne r0, psp         \n"                                \
      "mov   r1, lr          \n"                                \
      "movs  r2, #" #number "\n"                                \
      "b     Safety_CpuFault \n");                              \
  }

SAFETY_FAULT_TRAMPOLINE(HardFault_Handler,  3)
SAFETY_FAULT_TRAMPOLINE(MemManage_Handler,  4)
SAFETY_FAULT_TRAMPOLINE(BusFault_Handler,   5)
SAFETY_FAULT_TRAMPOLINE(UsageFault_Handler, 6)