/**
  ******************************************************************************
  * @file    usb_link.c
  * @brief   link_proto framing over USBX CDC ACM. See usb_link.h.
  ******************************************************************************
  */

#include "usb_link.h"
#include "ux_api.h"
#include "ux_device_class_cdc_acm.h"
#include <string.h>

/* USB endpoints on this device are 64 bytes, so transfers are chunked to
   match. Asking for more per call gains nothing and complicates the state. */
#define USB_CHUNK           64U

/* How long a single chunk may stay in flight before we give up on it.
   ux_device_class_cdc_acm_write_run() only reports completion once the host
   actually drains the endpoint -- so if no application has the port open, it
   never completes. Left unbounded, our poll loop re-enters it forever and the
   CDC class never gets to service a control request, which makes the host's
   attempt to configure the port fail. Abandoning the chunk keeps the class
   free to answer control traffic. */
#define USB_TX_TIMEOUT_MS   50U

/* Chunks pushed per poll. One 64-byte chunk per call could not keep up with
   voice and telemetry together (~150 packets/s, ~13 kB/s), so the ring filled
   and the larger packets were the ones refused. The USB stack is polled from
   the same loop, so this is bounded rather than a drain-everything loop. */
#define USB_TX_CHUNKS_PER_POLL  8U

static UX_SLAVE_CLASS_CDC_ACM *cdc_acm;      /* set by the activate callback */

/* ---- Outbound ------------------------------------------------------------ */

static uint8_t  tx_ring[USB_LINK_TX_RING_LEN];
static volatile uint32_t tx_head;            /* next write position          */
static volatile uint32_t tx_tail;            /* next byte to send            */

static uint8_t  tx_chunk[USB_CHUNK];
static uint32_t tx_chunk_len;
static bool     tx_in_flight;
static uint32_t tx_started_ms;

/* ---- Inbound ------------------------------------------------------------- */

static uint8_t  rx_chunk[USB_CHUNK];
static uint8_t  rx_frame[USB_LINK_RX_BUF_LEN];
static uint32_t rx_fill;

/* Ring of completed packets. Written by the parser, drained by the relay. */
static uint8_t  rx_queue[USB_LINK_RX_QUEUE_LEN][USB_LINK_RX_BUF_LEN];
static uint8_t  rx_queue_len[USB_LINK_RX_QUEUE_LEN];
static uint32_t rx_queue_head;
static uint32_t rx_queue_tail;

/* ---- Counters ------------------------------------------------------------ */

static volatile uint32_t tx_packets;
static volatile uint32_t rx_packets;
static volatile uint32_t tx_ring_full;
static volatile uint32_t rx_crc_errors;
static volatile uint32_t tx_abandoned;   /*!< chunks dropped on timeout    */
static volatile uint32_t rx_overflow;    /*!< inbound queue was full        */

/* -------------------------------------------------------------------------- */

void UsbLink_OnActivate(void *cdc_acm_instance)
{
  cdc_acm = (UX_SLAVE_CLASS_CDC_ACM *)cdc_acm_instance;
}

void UsbLink_OnDeactivate(void)
{
  cdc_acm      = UX_NULL;
  tx_in_flight = false;

  /* Drop anything queued. On reconnect the host has no idea what came before,
     so sending it a half-finished frame would just cost it a resync. */
  tx_head = 0U;
  tx_tail = 0U;
  rx_fill = 0U;

  rx_queue_head = 0U;
  rx_queue_tail = 0U;
}

bool UsbLink_IsConnected(void)
{
  return (cdc_acm != UX_NULL);
}

void UsbLink_Init(void)
{
  cdc_acm        = UX_NULL;
  tx_head        = 0U;
  tx_tail        = 0U;
  tx_chunk_len   = 0U;
  tx_in_flight   = false;
  tx_started_ms  = 0U;
  rx_fill        = 0U;
  rx_queue_head  = 0U;
  rx_queue_tail  = 0U;
}

/* ---- Ring helpers -------------------------------------------------------- */

static uint32_t tx_ring_used(void)
{
  return (tx_head - tx_tail) & (USB_LINK_TX_RING_LEN - 1U);
}

static uint32_t tx_ring_free(void)
{
  /* One byte held back so full and empty stay distinguishable. */
  return (USB_LINK_TX_RING_LEN - 1U) - tx_ring_used();
}

static void tx_ring_push(uint8_t b)
{
  tx_ring[tx_head] = b;
  tx_head = (tx_head + 1U) & (USB_LINK_TX_RING_LEN - 1U);
}

/* -------------------------------------------------------------------------- */

bool UsbLink_SendPacket(const uint8_t *ota, uint8_t len)
{
  if (ota == NULL || len == 0U || len > LINK_MAX_OTA_PAYLOAD + LINK_HEADER_LEN)
  {
    return false;
  }

  uint32_t needed = 2U + (uint32_t)len + 2U;   /* sync + body + crc */

  if (tx_ring_free() < needed)
  {
    tx_ring_full++;
    return false;
  }

  uint16_t crc = Link_Crc16(ota, len);

  tx_ring_push(LINK_USB_SYNC0);
  tx_ring_push(LINK_USB_SYNC1);

  for (uint8_t i = 0U; i < len; i++)
  {
    tx_ring_push(ota[i]);
  }

  tx_ring_push((uint8_t)(crc & 0xFFU));
  tx_ring_push((uint8_t)(crc >> 8));

  tx_packets++;
  return true;
}

bool UsbLink_GetPacket(uint8_t *ota, uint8_t *len, uint8_t max_len)
{
  if ((rx_queue_head == rx_queue_tail) || ota == NULL || len == NULL)
  {
    return false;
  }

  uint8_t n = rx_queue_len[rx_queue_tail];
  if (n > max_len) { n = max_len; }

  memcpy(ota, rx_queue[rx_queue_tail], n);
  *len = n;

  rx_queue_tail = (rx_queue_tail + 1U) % USB_LINK_RX_QUEUE_LEN;
  return true;
}

/* ---- Frame parser -------------------------------------------------------- */

/**
  * @brief Feed one received byte into the reassembler.
  *
  * Deliberately tolerant: the sync pattern can appear inside a payload by
  * chance, so a frame that fails its CRC does not poison the stream. The
  * buffer is shifted by one byte and the search resumes, which costs a little
  * work but recovers within a frame or two instead of desynchronising for good.
  */
static void rx_feed(uint8_t b)
{
  if (rx_fill >= sizeof(rx_frame))
  {
    rx_fill = 0U;          /* nothing valid found; start over */
  }

  rx_frame[rx_fill++] = b;

  for (;;)
  {
    /* Discard anything before a plausible sync. */
    if (rx_fill >= 1U && rx_frame[0] != LINK_USB_SYNC0)
    {
      memmove(rx_frame, &rx_frame[1], --rx_fill);
      continue;
    }
    if (rx_fill >= 2U && rx_frame[1] != LINK_USB_SYNC1)
    {
      memmove(rx_frame, &rx_frame[1], --rx_fill);
      continue;
    }
    break;
  }

  /* Need sync plus a header before the length is even readable. */
  if (rx_fill < (2U + LINK_HEADER_LEN))
  {
    return;
  }

  uint8_t  payload_len = rx_frame[2U + 2U];        /* header byte 2 = length */
  uint32_t total       = 2U + LINK_HEADER_LEN + payload_len + 2U;

  if (payload_len > LINK_MAX_OTA_PAYLOAD || total > sizeof(rx_frame))
  {
    /* Length is impossible, so the sync was a false positive. */
    memmove(rx_frame, &rx_frame[1], --rx_fill);
    return;
  }

  if (rx_fill < total)
  {
    return;                                        /* still arriving */
  }

  uint32_t body_len = LINK_HEADER_LEN + payload_len;
  uint16_t got      = (uint16_t)rx_frame[2U + body_len] |
                      ((uint16_t)rx_frame[2U + body_len + 1U] << 8);
  uint16_t want     = Link_Crc16(&rx_frame[2], body_len);

  if (got == want)
  {
    uint32_t next = (rx_queue_head + 1U) % USB_LINK_RX_QUEUE_LEN;

    if (next != rx_queue_tail)
    {
      memcpy(rx_queue[rx_queue_head], &rx_frame[2], body_len);
      rx_queue_len[rx_queue_head] = (uint8_t)body_len;
      rx_queue_head = next;
      rx_packets++;
    }
    else
    {
      /* Relay is not keeping up. Dropping the newest keeps the older ones in
         order, which matters for audio -- out-of-order voice is worse than
         missing voice. */
      rx_overflow++;
    }

    /* Consume the frame, keeping whatever followed it. */
    rx_fill -= total;
    if (rx_fill > 0U)
    {
      memmove(rx_frame, &rx_frame[total], rx_fill);
    }
  }
  else
  {
    rx_crc_errors++;
    memmove(rx_frame, &rx_frame[1], --rx_fill);
  }
}

/* ---- Poll ---------------------------------------------------------------- */

void UsbLink_Poll(void)
{
  ULONG actual_length = 0U;

  if (cdc_acm == UX_NULL)
  {
    return;
  }

  /* ---- Inbound ---- */
  if (ux_device_class_cdc_acm_read_run(cdc_acm, rx_chunk, USB_CHUNK,
                                       &actual_length) == UX_STATE_NEXT)
  {
    for (ULONG i = 0U; i < actual_length; i++)
    {
      rx_feed(rx_chunk[i]);
    }
  }

  /* ---- Outbound ----
     A chunk is latched, then write_run is called on every poll until it
     reports UX_STATE_NEXT. That repetition is the part that trips people up:
     the call does not finish the transfer by itself, and abandoning it after
     one attempt looks exactly like a broken write. */
  for (uint32_t pass = 0U; pass < USB_TX_CHUNKS_PER_POLL; pass++)
  {
    if (!tx_in_flight)
    {
      uint32_t used = tx_ring_used();

      if (used == 0U)
      {
        break;                      /* nothing queued */
      }

      tx_chunk_len = (used < USB_CHUNK) ? used : USB_CHUNK;

      for (uint32_t i = 0U; i < tx_chunk_len; i++)
      {
        tx_chunk[i] = tx_ring[(tx_tail + i) & (USB_LINK_TX_RING_LEN - 1U)];
      }

      tx_in_flight  = true;
      tx_started_ms = HAL_GetTick();
    }

    actual_length = 0U;

    if (ux_device_class_cdc_acm_write_run(cdc_acm, tx_chunk, tx_chunk_len,
                                          &actual_length) == UX_STATE_NEXT)
    {
      tx_tail      = (tx_tail + tx_chunk_len) & (USB_LINK_TX_RING_LEN - 1U);
      tx_in_flight = false;
    }
    else
    {
      if ((HAL_GetTick() - tx_started_ms) > USB_TX_TIMEOUT_MS)
      {
        /* Nobody is reading. Drop everything queued rather than retrying
           forever: stale audio is worthless, and holding the class in a
           transmit state stops it answering control requests. */
        tx_tail      = tx_head;
        tx_in_flight = false;
        tx_abandoned++;
      }

      break;    /* still busy -- give the stack a turn before trying again */
    }
  }
}

/* ---- Counters ------------------------------------------------------------ */

uint32_t UsbLink_GetTxPackets(void)   { return tx_packets; }
uint32_t UsbLink_GetRxPackets(void)   { return rx_packets; }
uint32_t UsbLink_GetTxRingFull(void)  { return tx_ring_full; }
uint32_t UsbLink_GetRxCrcErrors(void) { return rx_crc_errors; }
uint32_t UsbLink_GetTxAbandoned(void) { return tx_abandoned; }
uint32_t UsbLink_GetRxOverflow(void)  { return rx_overflow; }
