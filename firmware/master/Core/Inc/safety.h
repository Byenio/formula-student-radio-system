/**
  ******************************************************************************
  * @file    safety.h
  * @brief   Last-resort fault handling, shared by the master and slave boards.
  *
  * IDENTICAL ON BOTH BOARDS. Board differences live behind #if on the chip
  * define in safety.c, so this file and safety.c can be copied between
  * firmware/master and firmware/slave unchanged -- `diff` between the two
  * copies must come back empty.
  *
  * Every way the firmware can die ends up in Safety_Fault():
  *
  *   source                        caught by                 boards
  *   ---------------------------   -----------------------   ---------
  *   task stack overflow           FreeRTOS hook             master
  *   heap exhausted                FreeRTOS hook             master
  *   configASSERT                  FreeRTOSConfig.h          master
  *   HardFault, MemManage,         naked handlers in         both
  *     BusFault, UsageFault          safety.c
  *   HAL init failure              Error_Handler()           both
  *
  * Before this module, every one of those either hung silently or, for the
  * RTOS hooks, returned into undefined behaviour -- in each case with the
  * radio front end left in whatever state it happened to be in. Safety_Fault
  * makes the response explicit:
  *
  *   1. stop the radio transmitting, and release any shared bus,
  *   2. latch what happened, for SWD now and the stats link later,
  *   3. halt, or reset.
  *
  * Step 1 matters most. A board that has lost control but is still keying a
  * 27 dBm PA jams the channel for every device on it, including the one the
  * driver is talking on.
  *
  * Step 3 is a build option because the bench and the car want opposite
  * things. On the bench a halt preserves the evidence. In the car nobody is
  * watching, and a running radio beats a stopped one, so reset is correct --
  * and once the IWDG lands it becomes the same path.
  ******************************************************************************
  */

#ifndef SAFETY_H
#define SAFETY_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

/* Long enough for configMAX_TASK_NAME_LEN and a short source file name. */
#define SAFETY_NAME_LEN         16U

/* Marks the record as written by us rather than being uninitialised RAM. */
#define SAFETY_MAGIC            0x5AFE0002UL

/**
  * @brief  What to do once the fault has been latched.
  *
  * 1 = halt. Bench default: every buffer and counter is preserved for
  *     inspection over SWD. The master also blinks the fault code.
  * 0 = NVIC_SystemReset(). Correct for the car.
  *
  * Override from the build (-DSAFETY_HALT_ON_FAULT=0) rather than editing
  * this file, so the race build differs from the bench build by a flag and
  * not by a commit.
  */
#ifndef SAFETY_HALT_ON_FAULT
#define SAFETY_HALT_ON_FAULT    1
#endif

/**
  * @brief  Fault classes. On the master the value is also the blink code:
  *         that many pulses on STATUS_LED2, then a pause.
  */
typedef enum
{
  SAFETY_FAULT_NONE           = 0U,
  SAFETY_FAULT_STACK_OVERFLOW = 1U,  /*!< a task overran its stack          */
  SAFETY_FAULT_MALLOC_FAILED  = 2U,  /*!< pvPortMalloc returned NULL        */
  SAFETY_FAULT_ASSERT         = 3U,  /*!< configASSERT tripped              */
  SAFETY_FAULT_CPU            = 4U,  /*!< Hard/MemManage/Bus/UsageFault     */
  SAFETY_FAULT_ERROR_HANDLER  = 5U   /*!< CubeMX Error_Handler() was called */
} safety_fault_t;

/**
  * @brief  Latched description of the fault.
  *
  * Plain .bss, so it survives inspection but not a reset. Moving it to a
  * .noinit section is what makes it survive the reset path; that belongs with
  * the IWDG work, since nothing resets yet.
  *
  * Reading it after a fault: in the debugger, add `*Safety_GetRecord()` or
  * the static `record` in safety.c to the watches. For a CPU fault, feed
  * `pc` to addr2line to get the faulting line:
  *
  *     arm-none-eabi-addr2line -e build/RelWithDebInfo/master.elf -f -C 0x0800xxxx
  */
typedef struct
{
  uint32_t magic;                   /*!< SAFETY_MAGIC once written          */
  uint32_t fault;                   /*!< safety_fault_t                     */
  uint32_t count;                   /*!< faults seen; >1 means it recurred  */
  uint32_t line;                    /*!< source line, asserts only          */
  char     name[SAFETY_NAME_LEN];   /*!< task, file or exception name       */

  /* Filled for CPU faults. pc is also set for Error_Handler (the caller's
     return address) and is zero otherwise. */
  uint32_t pc;                      /*!< faulting instruction               */
  uint32_t lr;                      /*!< link register at the fault         */
  uint32_t xpsr;                    /*!< program status at the fault        */
  uint32_t exc_return;              /*!< EXC_RETURN: which stack, FPU state */
  uint32_t vector;                  /*!< 3 Hard, 4 MemManage, 5 Bus, 6 Usage*/
  uint32_t cfsr;                    /*!< SCB->CFSR: what kind of fault      */
  uint32_t hfsr;                    /*!< SCB->HFSR: was it escalated        */
  uint32_t mmfar;                   /*!< SCB->MMFAR: address, if MMARVALID  */
  uint32_t bfar;                    /*!< SCB->BFAR:  address, if BFARVALID  */
} safety_record_t;

/**
  * @brief  Latch a fatal fault and stop the board doing harm. Does not return.
  * @param  fault  what went wrong
  * @param  name   task name, file name, or NULL
  * @note   Safe from an ISR or with the scheduler stopped: disables
  *         interrupts first and never touches the RTOS or SysTick delays.
  */
void Safety_Fault(safety_fault_t fault, const char *name) __attribute__((noreturn));

/**
  * @brief  configASSERT target. Records the site, then calls Safety_Fault.
  * @note   Declared again in FreeRTOSConfig.h so that header stays
  *         standalone. Unused on the slave, which has no RTOS.
  */
void Safety_AssertFailed(const char *file, unsigned long line) __attribute__((noreturn));

/**
  * @brief  Error_Handler target.
  * @param  caller  return address of Error_Handler, i.e. the call site.
  *                 Pass __builtin_return_address(0) from inside
  *                 Error_Handler; addr2line turns it into a file and line.
  */
void Safety_ErrorHandler(void *caller) __attribute__((noreturn));

/**
  * @brief  C half of the CPU fault handlers. Called only from the naked
  *         handlers in safety.c; not for use from anywhere else.
  * @param  frame       exception stack frame (r0-r3, r12, lr, pc, xPSR)
  * @param  exc_return  EXC_RETURN value from LR on exception entry
  * @param  vector      exception number, 3..6
  */
void Safety_CpuFault(const uint32_t *frame, uint32_t exc_return, uint32_t vector)
     __attribute__((noreturn));

/** @brief  The latched record. Never NULL; check magic before trusting it. */
const safety_record_t *Safety_GetRecord(void);

/** @brief  True once a fault has been latched. */
bool Safety_HasFaulted(void);

#ifdef __cplusplus
}
#endif

#endif /* SAFETY_H */