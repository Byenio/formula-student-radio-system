/**
  ******************************************************************************
  * @file    safety.h
  * @brief   Last-resort fault handling for the car board.
  *
  * FreeRTOS is configured to detect two classes of fatal error --
  * configCHECK_FOR_STACK_OVERFLOW is 2 and configUSE_MALLOC_FAILED_HOOK is 1 --
  * but detection is only half of it. Until this module existed both hooks had
  * empty bodies, so the kernel spotted the fault and then returned into
  * undefined behaviour. This makes the response explicit:
  *
  *   1. stop the radio transmitting and release the RS-485 driver,
  *   2. latch what happened so it can be read back over SWD or the stats link,
  *   3. either halt and blink the fault code, or reset.
  *
  * Step 1 matters most. A board that has lost its stack but is still keying a
  * 27 dBm PA is worse than a board that is simply dead: it jams the channel for
  * every other device on it, including the one the driver is talking on.
  *
  * The choice in step 3 is deliberately a build option, because the bench and
  * the car want opposite things. On the bench a halt preserves the evidence and
  * the blink code tells you what happened without attaching a debugger. In the
  * car nobody is looking at the LED, and a running radio beats a stopped one,
  * so a reset is correct -- and once the IWDG lands it becomes the same path.
  ******************************************************************************
  */

#ifndef SAFETY_H
#define SAFETY_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

/* Long enough for configMAX_TASK_NAME_LEN and for a short source file name. */
#define SAFETY_NAME_LEN         16U

/* Marks the record as written by us rather than being uninitialised RAM. */
#define SAFETY_MAGIC            0x5AFE0001UL

/**
  * @brief  What to do once the fault has been latched.
  *
  * 1 = halt, blinking the fault code on STATUS_LED2. Bench default: the state
  *     of every buffer and counter is preserved for inspection.
  * 0 = NVIC_SystemReset(). Correct for the car.
  *
  * Override from the build system (-DSAFETY_HALT_ON_FAULT=0) rather than
  * editing this file, so the race build differs from the bench build by a flag
  * and not by a commit.
  */
#ifndef SAFETY_HALT_ON_FAULT
#define SAFETY_HALT_ON_FAULT    1
#endif

/** Blink code = number of pulses on STATUS_LED2 between pauses. */
typedef enum
{
  SAFETY_FAULT_NONE           = 0U,
  SAFETY_FAULT_STACK_OVERFLOW = 1U,  /*!< a task overran its stack           */
  SAFETY_FAULT_MALLOC_FAILED  = 2U,  /*!< pvPortMalloc returned NULL         */
  SAFETY_FAULT_ASSERT         = 3U   /*!< configASSERT tripped               */
} safety_fault_t;

/**
  * @brief Latched description of the fault.
  *
  * Plain .bss for now, so it survives inspection but not a reset. Moving it to
  * a .noinit section is what makes it survive the reset path, and that is worth
  * doing at the same time as the IWDG -- not before, since nothing resets yet.
  */
typedef struct
{
  uint32_t magic;                   /*!< SAFETY_MAGIC once written           */
  uint32_t fault;                   /*!< safety_fault_t                      */
  uint32_t count;                   /*!< faults seen; >1 means it recurred   */
  uint32_t line;                    /*!< source line, asserts only           */
  char     name[SAFETY_NAME_LEN];   /*!< task name, or file name for asserts */
} safety_record_t;

/**
  * @brief  Latch a fatal fault and stop the board doing harm. Does not return
  *         when SAFETY_HALT_ON_FAULT is 1.
  * @param  fault      what went wrong
  * @param  name       task name, or NULL
  * @note   Safe to call from an ISR or with the scheduler stopped: it disables
  *         interrupts and never touches the RTOS or the SysTick-based delays.
  */
void Safety_Fault(safety_fault_t fault, const char *name);

/**
  * @brief  configASSERT target. Records the site, then calls Safety_Fault.
  * @note   Declared again in FreeRTOSConfig.h so that header stays standalone.
  */
void Safety_AssertFailed(const char *file, unsigned long line);

/** @brief  The latched record. Never NULL; check magic before trusting it. */
const safety_record_t *Safety_GetRecord(void);

/** @brief  True once a fault has been latched. */
bool Safety_HasFaulted(void);

#ifdef __cplusplus
}
#endif

#endif /* SAFETY_H */