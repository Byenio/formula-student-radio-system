/**
  ******************************************************************************
  * @file    usb_link.h
  * @brief   link_proto framing over USBX CDC ACM, standalone mode.
  *
  * Sits between the radio relay and the PC. Radio packets go out wrapped in
  * the USB framing from link_proto.h; frames arriving from the PC are
  * unwrapped and handed back for transmission.
  *
  * WHY THE FRAMING EXISTS
  * ----------------------
  * CDC ACM is a byte stream with no message boundaries. Sixty-four byte USB
  * packets do not line up with our variable-length radio packets, so a reader
  * that assumes otherwise will silently split and merge them. Hence sync
  * bytes, an explicit length and a CRC: a receiver that joins mid-stream, or
  * hits a false sync inside a payload, resynchronises within a frame or two.
  *
  * WHY EVERYTHING IS POLLED
  * ------------------------
  * USBX standalone has no scheduler. Its read and write calls return a state
  * rather than blocking, and a transfer only completes if you keep calling
  * until it reports UX_STATE_NEXT -- which is why single-shot writes appear to
  * fail. UsbLink_Poll() drives that from the main loop and must be called
  * often, alongside ux_system_tasks_run().
  ******************************************************************************
  */

#ifndef USB_LINK_H
#define USB_LINK_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include "link_proto.h"
#include <stdint.h>
#include <stdbool.h>

/* Outbound byte ring.
   Must hold a comfortable burst of the LARGEST packets, not the average. A
   packet is refused whole if it does not fit, so a ring that is merely
   adequate on average starves big packets while small ones still squeeze in --
   with voice (88 B) and telemetry (up to 259 B) sharing the link, that meant
   telemetry stopping dead the moment the driver keyed the mic, while audio
   carried on. 4 kB is about 300 ms of the worst-case combined rate. */
#define USB_LINK_TX_RING_LEN    4096U

/* Inbound reassembly buffer: one maximum USB frame. */
#define USB_LINK_RX_BUF_LEN     LINK_USB_MAX_FRAME

/* Completed inbound packets waiting to be collected.
   A single slot is not enough: one UsbLink_Poll() reads 64 bytes and can
   finish several frames, while the caller collects at most one per pass. With
   one slot the later frames silently overwrite the earlier ones, which sounds
   exactly like half the audio going missing. */
#define USB_LINK_RX_QUEUE_LEN   8U

/**
  * @brief Reset the link state. Call once before the main loop.
  */
void UsbLink_Init(void);

/**
  * @brief Service USB reads and writes. Call every pass of the main loop.
  */
void UsbLink_Poll(void);

/**
  * @brief Frame a radio packet and queue it for the PC.
  * @param ota  the over-the-air packet, header and payload together
  * @retval false if the ring is full, i.e. the host is not reading
  */
bool UsbLink_SendPacket(const uint8_t *ota, uint8_t len);

/**
  * @brief Collect a complete, CRC-checked packet from the PC.
  * @retval true if one was returned
  */
bool UsbLink_GetPacket(uint8_t *ota, uint8_t *len, uint8_t max_len);

/**
  * @brief True once the host has configured the interface.
  */
bool UsbLink_IsConnected(void);

/* Called from the CDC ACM activate/deactivate callbacks in
   ux_device_cdc_acm.c, which is where USBX hands over the class instance. */
void UsbLink_OnActivate(void *cdc_acm_instance);
void UsbLink_OnDeactivate(void);

/* Bring-up counters. */
uint32_t UsbLink_GetTxPackets(void);
uint32_t UsbLink_GetRxPackets(void);
uint32_t UsbLink_GetTxRingFull(void);  /*!< host not draining              */
uint32_t UsbLink_GetRxCrcErrors(void); /*!< framing lost, or a false sync  */
uint32_t UsbLink_GetTxAbandoned(void);  /*!< host never drained a write     */
uint32_t UsbLink_GetRxOverflow(void);   /*!< inbound queue full; packet lost */

#ifdef __cplusplus
}
#endif

#endif /* USB_LINK_H */
