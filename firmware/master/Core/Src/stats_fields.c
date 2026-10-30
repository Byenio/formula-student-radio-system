/**
  ******************************************************************************
  * @file    stats_fields.c   (MASTER)
  * @brief   The counters the master reports. Board-specific by design; the
  *          slave has its own file of the same name.
  *
  * One list, STATS_FIELDS, drives both the descriptions sent in the schema
  * frame and the function that reads the values -- a counter cannot be named
  * without being read, or read without being named.
  *
  * Adding a counter: one line. X(kind, "group.name", expression)
  *   kind        U32 for counts, I32 for anything that can go negative
  *   name        dotted, <= 31 characters; groups by prefix on the PC
  *   expression  anything cheap that returns an integer. Evaluated every
  *               STATS_SAMPLE_MS from the stats task, so it must not block
  *               and must not have side effects -- e.g. never
  *               Audio_GetPeakHoldAndReset(), which would steal the reset
  *               from whoever relies on it.
  *
  * Counters are cumulative since boot. Rates are the PC's job.
  ******************************************************************************
  */

#include "stats.h"

#if FW_STATS_ENABLE

#include "e28_radio.h"
#include "rs485.h"
#include "audio.h"
#include "can_bus.h"
#include "ptt.h"
#include "statlink.h"

#define STATS_FIELDS(X)                                                        \
  /* radio link */                                                             \
  X(U32, "radio.tx",              E28_Radio_GetTxCount())                      \
  X(U32, "radio.rx",              E28_Radio_GetRxCount())                      \
  X(U32, "radio.crc_err",         E28_Radio_GetCrcErrorCount())                \
  X(I32, "radio.rssi",            E28_Radio_GetLastRssi())                     \
  X(U32, "radio.dio1",            E28_Radio_GetDio1Count())                    \
  X(U32, "radio.seq_gaps",        E28_Radio_GetSeqGaps())                      \
  X(U32, "radio.tx_timeouts",     E28_Radio_GetTxTimeouts())                   \
  X(U32, "radio.tx_wrong_irq",    E28_Radio_GetTxWrongIrq())                   \
  X(U32, "radio.configured",      E28_Radio_IsConfigured())                    \
  X(U32, "radio.rx_armed",        E28_Radio_IsRxArmed())                       \
  /* scheduler: what went on air, and what voice gave way to telemetry */      \
  X(U32, "sched.telem_sent",      E28_Radio_GetTelemSent())                    \
  X(U32, "sched.telem_aggr",      E28_Radio_GetTelemAggregated())              \
  X(U32, "sched.audio_sent",      E28_Radio_GetAudioSent())                    \
  X(U32, "sched.audio_yielded",   E28_Radio_GetAudioYielded())                 \
  /* RS-485 telemetry from the VCU */                                          \
  X(U32, "rs485.bytes",           RS485_GetBytesReceived())                    \
  X(U32, "rs485.frames",          RS485_GetFramesGood())                       \
  X(U32, "rs485.bad_crc",         RS485_GetFramesBadCrc())                     \
  X(U32, "rs485.resyncs",         RS485_GetResyncs())                          \
  X(U32, "rs485.queue_full",      RS485_GetQueueFull())                        \
  X(U32, "rs485.dma_err",         RS485_GetDmaErrors())                        \
  X(U32, "rs485.recoveries",      RS485_GetRecoveries())                       \
  X(U32, "rs485.last_err",        RS485_GetLastErrorCode())                    \
  X(U32, "rs485.noise",           RS485_GetNoiseErrors())                      \
  X(U32, "rs485.framing",         RS485_GetFramingErrors())                    \
  X(U32, "rs485.overrun",         RS485_GetOverrunErrors())                    \
  /* audio */                                                                  \
  X(U32, "audio.captured",        Audio_GetFramesCaptured())                   \
  X(U32, "audio.played",          Audio_GetFramesPlayed())                     \
  X(U32, "audio.tx_dropped",      Audio_GetTxDropped())                        \
  X(U32, "audio.rx_underrun",     Audio_GetRxUnderruns())                      \
  X(U32, "audio.pkpk",            Audio_GetLastFramePeakToPeak())              \
  /* CAN: RADIO_CONTROL from the steering wheel */                             \
  X(U32, "can.rx",                CanBus_GetRxCount())                         \
  X(U32, "can.stale",             CanBus_GetStaleCount())                      \
  X(U32, "can.link_drops",        CanBus_GetLinkDrops())                       \
  X(U32, "can.errors",            CanBus_GetErrorCount())                      \
  X(U32, "can.counter",           CanBus_GetLastCounter())                     \
  X(U32, "can.alive",             CanBus_IsLinkAlive())                        \
  X(U32, "can.ptt_req",           CanBus_GetPttRequest())                      \
  /* PTT */                                                                    \
  X(U32, "ptt.toggles",           Ptt_GetToggleCount())                        \
  X(U32, "ptt.open",              Ptt_IsOpen())                                \
  X(U32, "ptt.transmitting",      Ptt_IsTransmitting())                        \
  X(U32, "ptt.button",            Ptt_IsButtonActive())                        \
  /* the stats link itself */                                                  \
  X(U32, "statlink.bytes",        StatLink_GetBytesSent())                     \
  X(U32, "statlink.dropped",      StatLink_GetDropped())

#define X_DESC(kind, name, expr)  { STATS_##kind, name },
#define X_READ(kind, name, expr)  out[i++] = (uint32_t)(int32_t)(expr);

const stats_field_t stats_fields[] = { STATS_FIELDS(X_DESC) };
const uint16_t      stats_field_count =
    (uint16_t)(sizeof(stats_fields) / sizeof(stats_fields[0]));

_Static_assert(sizeof(stats_fields) / sizeof(stats_fields[0]) <= STATS_MAX_FIELDS,
               "more counters than STATS_MAX_FIELDS");

void stats_fields_read(uint32_t *out)
{
  uint32_t i = 0U;
  STATS_FIELDS(X_READ)
  (void)i;
}

#endif /* FW_STATS_ENABLE */