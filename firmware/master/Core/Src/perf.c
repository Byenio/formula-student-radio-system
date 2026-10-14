/**
  ******************************************************************************
  * @file    perf.c
  * @brief   DWT-based section timing. See perf.h.
  *
  * IDENTICAL ON BOTH BOARDS.
  ******************************************************************************
  */

#include "perf.h"

#if FW_STATS_ENABLE

#if defined(FW_HOST_TEST)
  uint32_t perf_host_cycles;
  #define PERF_IRQ_SAVE()        0U
  #define PERF_IRQ_RESTORE(s)    ((void)(s))
#else
  #include "main.h"              /* CMSIS core: DWT, CoreDebug, PRIMASK     */
  #define PERF_IRQ_SAVE()        perf_irq_save()
  #define PERF_IRQ_RESTORE(s)    __set_PRIMASK(s)

  static inline uint32_t perf_irq_save(void)
  {
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    return primask;
  }
#endif

/* Must stay in step with perf_id_t. Dotted names group naturally in the PC
   tool's output; keep them short -- they travel in every schema frame. */
static const char *const perf_names[PERF_COUNT] =
{
  [PERF_AUDIO_FRAME]   = "audio.frame",
  [PERF_AUDIO_ENCODE]  = "audio.encode",
  [PERF_AUDIO_DECODE]  = "audio.decode",
  [PERF_AUDIO_WAKE]    = "audio.wake",
  [PERF_RADIO_AIRTIME] = "radio.airtime",
  [PERF_RS485_DRAIN]   = "rs485.drain",
  [PERF_RADIO_SEND]    = "radio.send",
  [PERF_STATS]         = "stats.self",
  [PERF_LOOP]          = "loop",
  [PERF_USBX]          = "usbx",
  [PERF_USBLINK]       = "usblink",
  [PERF_RELAY]         = "relay",
};

static perf_window_t windows[PERF_COUNT];
static uint32_t      overhead;
static bool          supported;
static bool          initialised;

static void perf_reset_windows(void)
{
  for (uint32_t i = 0U; i < (uint32_t)PERF_COUNT; i++)
  {
    windows[i].count = 0U;
    windows[i].min   = UINT32_MAX;
    windows[i].max   = 0U;
    windows[i].sum   = 0U;
  }
}

void Perf_Init(void)
{
  if (initialised)
  {
    return;
  }
  initialised = true;

#if defined(FW_HOST_TEST)
  supported = true;
#else
  /* TRCENA powers the DWT block. Set by the debugger too when SWO is in use;
     setting it again is harmless. The counter is not zeroed, because a
     debugger may already be using it and nothing here needs an absolute
     value -- only differences. */
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;

  /* The cycle counter is optional on ARMv8-M. NOCYCCNT says this core was
     built without one; then every section reads 0 and the schema says so,
     rather than reporting zeros as if they were measurements. */
  supported = ((DWT->CTRL & DWT_CTRL_NOCYCCNT_Msk) == 0U);
  if (supported)
  {
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
  }
#endif

  perf_reset_windows();

  /* Calibrate: the cost of an empty BEGIN/END pair, minimum of a few runs
     with interrupts off so nothing else is caught in the measurement. It is
     the floor every measurement sits on, and is reported rather than
     subtracted. */
  {
    uint32_t best = UINT32_MAX;
    uint32_t s = PERF_IRQ_SAVE();

    for (uint32_t i = 0U; i < 16U; i++)
    {
      const uint32_t t0 = Perf_Now();
      const uint32_t t1 = Perf_Now();
      const uint32_t d  = t1 - t0;
      if (d < best) { best = d; }
    }

    PERF_IRQ_RESTORE(s);
    overhead = supported ? best : 0U;
  }
}

void Perf_Record(perf_id_t id, uint32_t cycles)
{
  if ((uint32_t)id >= (uint32_t)PERF_COUNT)
  {
    return;
  }

  perf_window_t *w = &windows[id];

  w->count++;
  w->sum += cycles;
  if (cycles > w->max) { w->max = cycles; }
  if (cycles < w->min) { w->min = cycles; }
}

void Perf_Snapshot(perf_window_t out[PERF_COUNT])
{
  /* Interrupts off for the copy and reset only: twelve 16-byte structs,
     on the order of a hundred cycles. Long enough to be atomic, short enough
     not to show up in any other section's numbers. */
  uint32_t s = PERF_IRQ_SAVE();

  for (uint32_t i = 0U; i < (uint32_t)PERF_COUNT; i++)
  {
    out[i] = windows[i];
  }
  perf_reset_windows();

  PERF_IRQ_RESTORE(s);
}

const char *Perf_Name(perf_id_t id)
{
  return ((uint32_t)id < (uint32_t)PERF_COUNT) ? perf_names[id] : "?";
}

uint32_t Perf_GetOverhead(void) { return overhead; }
bool     Perf_IsSupported(void) { return supported; }

#endif /* FW_STATS_ENABLE */
