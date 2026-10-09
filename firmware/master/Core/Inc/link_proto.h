/**
  ******************************************************************************
  * @file    link_proto.h
  * @brief   Wire format for the driver radio link.
  *
  * Three hops, two framings:
  *
  *   car board  --(2.4 GHz SX1281)-->  base station  --(USB CDC)-->  PC app
  *   car board  <--(2.4 GHz SX1281)--  base station  <--(USB CDC)--  PC app
  *
  * The over-the-air header is identical in both directions and is what the
  * base station relays verbatim -- it never parses a payload, it only re-frames
  * between radio and USB. All encoding, decoding and audio device handling
  * lives in the PC application.
  *
  * This file is the single source of truth. Keep the Python side in sync with
  * it; if the two ever disagree, this one wins.
  ******************************************************************************
  */

#ifndef LINK_PROTO_H
#define LINK_PROTO_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* Bump on any incompatible change. The receiver drops mismatched versions
   rather than guessing, so a half-updated pair of boards fails loudly. */
#define LINK_PROTO_VERSION          1U

/* ==========================================================================
   Over-the-air packet
   ==========================================================================

   +--------+--------+--------+--------+---------------------------+
   | ver/typ|  seq   |  len   | flags  |        payload            |
   +--------+--------+--------+--------+---------------------------+
      byte0    byte1    byte2    byte3       byte4 .. byte4+len-1

   byte0: version in bits 7:4, packet type in bits 3:0
   byte1: sequence number, wraps at 256, counted PER TYPE so a receiver can
          spot a missing audio frame without telemetry traffic confusing it
   byte2: payload length in bytes, not counting this header
   byte3: flags, see LINK_FLAG_*

   No software checksum: the SX1281 appends a hardware CRC and discards
   corrupt frames before they ever reach us. Adding another would be wasted
   airtime. The USB hop does carry its own CRC, because a USB CDC byte stream
   has no framing guarantees of its own.
   ========================================================================== */

#define LINK_HEADER_LEN             4U

/* SX1281 caps a payload at 255 bytes; the header eats four of them. */
#define LINK_MAX_OTA_PAYLOAD        251U

#define LINK_VER_TYPE(ver, type)    ((uint8_t)(((ver) << 4) | ((type) & 0x0FU)))
#define LINK_GET_VERSION(b)         ((uint8_t)((b) >> 4))
#define LINK_GET_TYPE(b)            ((uint8_t)((b) & 0x0FU))

typedef enum
{
  LINK_PKT_AUDIO            = 0x1U,  /*!< one 10 ms voice frame            */
  LINK_PKT_TELEM_CRITICAL   = 0x2U,  /*!< SoC, cell temps -- pre-empts all */
  LINK_PKT_TELEM_NORMAL     = 0x3U,  /*!< scheduled telemetry cadence      */
  LINK_PKT_TELEM_BULK       = 0x4U,  /*!< logs, diagnostics; first dropped */
  LINK_PKT_CONTROL          = 0x5U   /*!< PTT state, channel negotiation,
                                          keepalive, link statistics       */
} link_pkt_type_t;

/* Priority order used by the transmit scheduler. Lower value wins. Kept here
   rather than in the scheduler so both ends agree on what "important" means. */
#define LINK_PRIO_TELEM_CRITICAL    0U
#define LINK_PRIO_CONTROL           1U
#define LINK_PRIO_TELEM_NORMAL      2U
#define LINK_PRIO_AUDIO             3U
#define LINK_PRIO_TELEM_BULK        4U

/* ---- Flags (byte 3) ------------------------------------------------------ */
#define LINK_FLAG_PTT_ACTIVE        (1U << 0) /*!< driver is holding PTT     */
#define LINK_FLAG_MORE_QUEUED       (1U << 1) /*!< sender has more waiting;
                                                   lets the other end hold
                                                   off transmitting          */
#define LINK_FLAG_TELEM_OVERFLOW    (1U << 2) /*!< a telemetry queue dropped
                                                   something -- surfaces
                                                   congestion to the pit wall */

/* ==========================================================================
   Audio format
   ==========================================================================

   Capture runs at 16 kHz (see audio.h) but transmission is decimated to
   8 kHz. That is not a compromise: the mic path is already low-passed at
   about 4.8 kHz by R118/C121 on the board, so there is nothing above 4 kHz
   worth sending. Decimating halves airtime for no audible loss.

   Samples are then mu-law companded from 12-bit linear to 8 bits. Mu-law is
   the telephony standard for exactly this job -- it spends its resolution
   where the ear is sensitive, and both encode and decode are table lookups.

   One frame therefore costs 80 bytes, plus 4 header = 84 bytes on air.
   ========================================================================== */

#define LINK_AUDIO_RATE_HZ          8000U
#define LINK_AUDIO_FRAME_MS         10U
#define LINK_AUDIO_SAMPLES_PER_FRAME 80U   /* 8 kHz * 10 ms                  */
#define LINK_AUDIO_PAYLOAD_LEN      LINK_AUDIO_SAMPLES_PER_FRAME  /* 1 B/sample */

/* Decimation factor from the capture rate down to the on-air rate. */
#define LINK_AUDIO_DECIMATION       2U     /* 16 kHz capture -> 8 kHz air    */

/* ==========================================================================
   USB CDC framing (base station <-> PC)
   ==========================================================================

   A CDC byte stream has no message boundaries, so the radio packet is wrapped:

   +------+------+----------------------+--------------+------+------+
   | 0xAA | 0x55 |  OTA header (4 B)    |   payload    | CRC lo| CRC hi|
   +------+------+----------------------+--------------+------+------+

   CRC16-CCITT (poly 0x1021, init 0xFFFF) over the header and payload, not the
   sync bytes. Sync plus an explicit length plus a CRC means a receiver that
   joins mid-stream, or hits a false sync inside a payload, resynchronises
   within a frame or two instead of desynchronising permanently.

   Python side: struct.unpack('<BBBB', ...) after locating AA 55.
   ========================================================================== */

#define LINK_USB_SYNC0              0xAAU
#define LINK_USB_SYNC1              0x55U
#define LINK_USB_OVERHEAD           (2U + LINK_HEADER_LEN + 2U)
#define LINK_USB_MAX_FRAME          (LINK_USB_OVERHEAD + LINK_MAX_OTA_PAYLOAD)

#define LINK_CRC16_POLY             0x1021U
#define LINK_CRC16_INIT             0xFFFFU

/* ==========================================================================
   Control payloads (LINK_PKT_CONTROL)
   ==========================================================================
   First payload byte is a sub-type, so control messages can be added without
   burning packet-type values. */

typedef enum
{
  LINK_CTRL_KEEPALIVE       = 0x01U, /*!< empty; proves the link is alive   */
  LINK_CTRL_PTT_STATE       = 0x02U, /*!< 1 byte: 0 released, 1 held        */
  LINK_CTRL_LINK_STATS      = 0x03U, /*!< RSSI, SNR, dropped-frame counters */
  LINK_CTRL_CHANNEL_REQUEST = 0x04U, /*!< channel negotiation, reserved for
                                          the future two-antenna setup      */
  LINK_CTRL_CHANNEL_GRANT   = 0x05U
} link_ctrl_subtype_t;

/* ==========================================================================
   Helpers
   ========================================================================== */

/**
  * @brief CRC16-CCITT over a buffer. Used for the USB hop only.
  */
static inline uint16_t Link_Crc16(const uint8_t *data, uint32_t len)
{
  uint16_t crc = LINK_CRC16_INIT;

  for (uint32_t i = 0U; i < len; i++)
  {
    crc ^= (uint16_t)data[i] << 8;

    for (uint8_t bit = 0U; bit < 8U; bit++)
    {
      crc = (crc & 0x8000U) ? (uint16_t)((crc << 1) ^ LINK_CRC16_POLY)
                            : (uint16_t)(crc << 1);
    }
  }

  return crc;
}

#ifdef __cplusplus
}
#endif

#endif /* LINK_PROTO_H */
