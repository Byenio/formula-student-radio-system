/**
  ******************************************************************************
  * @file    perf.h
  * @brief   Cycle-accurate timing of code sections, using the DWT cycle counter.
  *
  * IDENTICAL ON BOTH BOARDS, like safety.c/.h.
  *
  * Usage -- the two macros must sit in the same C block:
  *
  *     PERF_BEGIN(PERF_AUDIO_ENCODE);
  *     AudioCodec_EncodeFrame(...);
  *     PERF_END(PERF_AUDIO_ENCODE);
  *
  * Every section accumulates count / min / max / sum of elapsed CPU cycles
  * over a reporting window (STATS_SAMPLE_MS). stats.c collects and resets the
  * windows and ships them to the PC, where cycles become microseconds.
  *
  * What is measured is ELAPSED cycles between the two markers, not cycles
  * spent by this code alone: an interrupt or a higher-priority task that runs
  * in between is included. For sections in the highest-priority task that is
  * only ISR time, which is the honest number anyway -- it is what the deadline
  * sees. For a section that waits (PERF_RADIO_AIRTIME) it is wall-clock time
  * by design.
  *
  * Rules:
  *   - one section is updated from one context only (one task, or one ISR),
  *     never both: the update is not atomic, which keeps it ~15 cycles;
  *   - sections must be shorter than 2^32 cycles (25 s at 170 MHz).
  *
  * The raw figures include the cost of the markers themselves. That cost is
  * measured once at start-up (Perf_GetOverhead) and reported in the schema
  * frame, so analysis subtracts it -- the firmware never adjusts raw data.
  *
  * With FW_STATS_ENABLE = 0 every macro expands to nothing and perf.c
  * compiles to an empty object: the race build carries no instrumentation.
  ******************************************************************************
  */

#ifndef PERF_H
#define PERF_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

#ifndef FW_STATS_ENABLE
#define FW_STATS_ENABLE 0
#endif

/**
  * @brief  Every timed section on either board. One shared list, so ids and
  *         names are identical everywhere; a section a board never enters
  *         simply reports count = 0.
  *
  *         Adding one: append before PERF_COUNT and add its name to
  *         perf_names[] in perf.c. The PC tool learns names from the schema,
  *         so nothing on the PC side needs changing.
  */
typedef enum
{
  /* master */
  PERF_AUDIO_FRAME = 0,   /*!< whole 10 ms frame: capture + playback path      */
  PERF_AUDIO_ENCODE,      /*!< FIR decimation + mu-law encode                  */
  PERF_AUDIO_DECODE,      /*!< mu-law decode + upsampling                      */
  PERF_AUDIO_WAKE,        /*!< ADC DMA interrupt -> audio task running         */
  PERF_RADIO_AIRTIME,     /*!< SetTx accepted -> TxDone seen (wall clock)      */
  PERF_RS485_DRAIN,       /*!< one pass over newly arrived RS-485 bytes        */
  /* both */
  PERF_RADIO_SEND,        /*!< E28_Radio_Send: SPI upload + TX command         */
  PERF_STATS,             /*!< the stats layer's own cost, per report          */
  /* slave */
  PERF_LOOP,              /*!< one pass of the bare-metal main loop            */
  PERF_USBX,              /*!< ux_system_tasks_run()                           */
  PERF_USBLINK,           /*!< UsbLink_Poll()                                  */
  PERF_RELAY,             /*!< E28_Radio_RelayPoll()                           */

  PERF_COUNT
} perf_id_t;

/** @brief  One section's accumulated window. */
typedef struct
{
  uint32_t count;         /*!< times the section ran                          */
  uint32_t min;           /*!< shortest, cycles (UINT32_MAX if count == 0)    */
  uint32_t max;           /*!< longest, cycles                                */
  uint32_t sum;           /*!< total, cycles                                  */
} perf_window_t;

#if FW_STATS_ENABLE

/* The DWT cycle counter. Architecturally fixed at this address on every
   ARMv7-M and ARMv8-M core, so no CMSIS header is needed here -- which lets
   FreeRTOSConfig.h use the same definition as the run-time-stats clock. */
#define PERF_CYCCNT  (*(volatile uint32_t *)0xE0001004UL)

#if defined(FW_HOST_TEST)
extern uint32_t perf_host_cycles;               /* advanced by the test     */
static inline uint32_t Perf_Now(void) { return perf_host_cycles; }
#else
static inline uint32_t Perf_Now(void) { return PERF_CYCCNT; }
#endif

/**
  * @brief  Start the cycle counter and calibrate the marker overhead.
  *         Idempotent. Called by Stats_Init, and on the master also by
  *         FreeRTOS before the scheduler starts (it is the run-time clock).
  */
void Perf_Init(void);

/** @brief  Add one measurement to a section's window. */
void Perf_Record(perf_id_t id, uint32_t cycles);

/**
  * @brief  Copy every window out and reset it, atomically with respect to
  *         interrupts, so a snapshot is never half of one window and half of
  *         the next.
  */
void Perf_Snapshot(perf_window_t out[PERF_COUNT]);

/** @brief  Section name, for the schema frame. */
const char *Perf_Name(perf_id_t id);

/** @brief  Cycles one BEGIN/END pair adds to every measurement. */
uint32_t Perf_GetOverhead(void);

/** @brief  False if this core was built without a cycle counter. */
bool Perf_IsSupported(void);

#define PERF_BEGIN(id)   const uint32_t perf_t0_##id = Perf_Now()
#define PERF_END(id)     Perf_Record((id), Perf_Now() - perf_t0_##id)

#else  /* FW_STATS_ENABLE == 0 */

#define PERF_BEGIN(id)   ((void)0)
#define PERF_END(id)     ((void)0)

#endif

#ifdef __cplusplus
}
#endif

#endif /* PERF_H */