/**
  ******************************************************************************
  * @file    stats.h
  * @brief   Statistics layer: counters, section timings and RTOS state,
  *          reported periodically over statlink.
  *
  * IDENTICAL ON BOTH BOARDS. What differs per board is the list of counters,
  * which lives in that board's stats_fields.c.
  *
  * Self-describing on purpose. The firmware never assumes the PC knows its
  * layout: a SCHEMA frame names every counter and timed section, and every
  * data frame carries the schema's 16-bit fingerprint. A logger can attach
  * mid-run (the schema repeats every STATS_SCHEMA_MS), and a log recorded
  * today stays readable after counters are added tomorrow, because each log
  * contains its own description. Adding a counter is one line in
  * stats_fields.c and nothing on the PC side.
  *
  * Frames (statlink type byte), all fields little-endian. Every payload
  * starts with the same 8 bytes:
  *
  *   u32 seq       one counter across all frame types: a gap = frames lost
  *   u32 t_ms      HAL tick at the time of the report
  *
  * 0x01 SCHEMA
  *   u8  proto     STATS_PROTO_VERSION
  *   u8  board     0 master, 1 slave, 255 host test
  *   u16 schema_id CRC16 over field kinds+names and section names
  *   u32 core_hz   SystemCoreClock: converts cycles to time
  *   u16 sample_ms SAMPLE/PERF period
  *   u16 tasks_ms  TASKS period, 0 on a board without an RTOS
  *   u32 perf_overhead   cycles one BEGIN/END pair adds (subtract it)
  *   u8  perf_ok   0 if the core has no cycle counter
  *   u32 baud      statlink baud rate
  *   str build     git hash, dirty flag, build type, compiler
  *   u16 n_fields, then n_fields x { u8 kind (0 u32, 1 i32), str name }
  *   u8  n_perf,   then n_perf   x { str name }
  *   (str = u8 length + bytes, no terminator)
  *
  * 0x02 SAMPLE   u16 schema_id, u32 value[n_fields]
  *               Counters are cumulative since boot; the PC differentiates.
  *
  * 0x03 PERF     u16 schema_id, u32 window_cycles,
  *               n_perf x { u32 count, u32 min, u32 max, u32 sum } (cycles)
  *               Windowed: reset after every report.
  *
  * 0x04 TASKS    (RTOS boards only)
  *               u32 total_delta  run-time clock ticks this window (cycles)
  *               u32 heap_free, u32 heap_min_ever  (bytes)
  *               u8 n, then n x { u8 number, str name, u8 priority,
  *                                u8 state, u16 stack_free_words,
  *                                u32 run_delta }
  *               CPU load = 1 - idle.run_delta / total_delta.
  ******************************************************************************
  */

#ifndef STATS_H
#define STATS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include "perf.h"            /* FW_STATS_ENABLE default */

#define STATS_PROTO_VERSION     1U

#define STATS_FRAME_SCHEMA      0x01U
#define STATS_FRAME_SAMPLE      0x02U
#define STATS_FRAME_PERF        0x03U
#define STATS_FRAME_TASKS       0x04U

#ifndef STATS_SAMPLE_MS
#define STATS_SAMPLE_MS         100U     /*!< counters + timing windows    */
#endif
#ifndef STATS_TASKS_MS
#define STATS_TASKS_MS          1000U    /*!< per-task CPU and stack       */
#endif
#ifndef STATS_SCHEMA_MS
#define STATS_SCHEMA_MS         5000U    /*!< repeat, for late-attaching PCs*/
#endif

#define STATS_MAX_FIELDS        64U
#define STATS_MAX_TASKS         12U

/** @brief  How the PC should read a counter's 32 bits. */
typedef enum
{
  STATS_U32 = 0U,
  STATS_I32 = 1U
} stats_kind_t;

typedef struct
{
  uint8_t     kind;          /*!< stats_kind_t                             */
  const char *name;          /*!< dotted, e.g. "radio.tx"; <= 31 chars      */
} stats_field_t;

/* ---- Provided by each board's stats_fields.c ----------------------------- */
extern const stats_field_t stats_fields[];
extern const uint16_t      stats_field_count;
/** @brief  Read every counter, in stats_fields[] order. */
void stats_fields_read(uint32_t *out);

#if FW_STATS_ENABLE

/**
  * @brief  Start the cycle counter and the statistics link, send the first
  *         schema. On the master, also create the stats task: call from the
  *         RTOS_THREADS block, before osKernelStart(). On the slave, call
  *         once after HAL init, then Stats_Poll() from the main loop.
  */
void Stats_Init(void);

/** @brief  Slave main loop hook. Cheap when nothing is due. */
void Stats_Poll(void);

#else

static inline void Stats_Init(void) { }
static inline void Stats_Poll(void) { }

#endif

#ifdef __cplusplus
}
#endif

#endif /* STATS_H */