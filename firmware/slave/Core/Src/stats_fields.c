/**
  ******************************************************************************
  * @file    stats_fields.c   (SLAVE)
  * @brief   The counters the slave reports. Board-specific by design; the
  *          master has its own file of the same name.
  *
  * One list, STATS_FIELDS, drives both the descriptions sent in the schema
  * frame and the function that reads the values -- a counter cannot be named
  * without being read, or read without being named.
  *
  * Adding a counter: one line. X(kind, "group.name", expression)
  *   kind        U32 for counts, I32 for anything that can go negative
  *   name        dotted, <= 31 characters; groups by prefix on the PC
  *   expression  cheap, non-blocking, no side effects. Evaluated from the
  *               main loop every STATS_SAMPLE_MS.
  *
  * Counters are cumulative since boot. Rates are the PC's job.
  ******************************************************************************
  */

#include "stats.h"

#if FW_STATS_ENABLE

#include "e28_radio.h"
#include "usb_link.h"
#include "statlink.h"

#define STATS_FIELDS(X)                                                        \
  /* radio link */                                                             \
  X(U32, "radio.tx",              E28_Radio_GetTxCount())                      \
  X(U32, "radio.rx",              E28_Radio_GetRxCount())                      \
  X(U32, "radio.crc_err",         E28_Radio_GetCrcErrorCount())                \
  X(I32, "radio.rssi",            E28_Radio_GetLastRssi())                     \
  X(U32, "radio.dio1",            E28_Radio_GetDio1Count())                    \
  X(U32, "radio.relayed",         E28_Radio_GetEchoRxCount())                  \
  X(U32, "radio.seq_gaps",        E28_Radio_GetSeqGaps())                      \
  X(U32, "radio.tx_timeouts",     E28_Radio_GetTxTimeouts())                   \
  X(U32, "radio.tx_wrong_irq",    E28_Radio_GetTxWrongIrq())                   \
  X(U32, "radio.configured",      E28_Radio_IsConfigured())                    \
  X(U32, "radio.rx_armed",        E28_Radio_IsRxArmed())                       \
  /* relay between air and USB */                                              \
  X(U32, "relay.to_usb",          E28_Radio_GetUsbForwarded())                 \
  X(U32, "relay.from_usb",        E28_Radio_GetAirFromUsb())                   \
  X(U32, "relay.backpressure",    E28_Radio_GetUsbBackpressure())              \
  /* USB CDC link to the PC */                                                 \
  X(U32, "usb.connected",         UsbLink_IsConnected())                       \
  X(U32, "usb.tx_packets",        UsbLink_GetTxPackets())                      \
  X(U32, "usb.rx_packets",        UsbLink_GetRxPackets())                      \
  X(U32, "usb.tx_ring_full",      UsbLink_GetTxRingFull())                     \
  X(U32, "usb.rx_crc_err",        UsbLink_GetRxCrcErrors())                    \
  X(U32, "usb.tx_abandoned",      UsbLink_GetTxAbandoned())                    \
  X(U32, "usb.rx_overflow",       UsbLink_GetRxOverflow())                     \
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