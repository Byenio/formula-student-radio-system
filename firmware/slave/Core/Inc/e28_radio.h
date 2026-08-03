/**
  ******************************************************************************
  * @file    e28_radio.h
  * @brief   Radio configuration and packet transport for the SX1281.
  *
  * Sits on top of e28.c, which owns the SPI transport and the BUSY handshake.
  * This layer owns the modem: packet type, frequency, modulation, sync word,
  * IRQ routing, and the transmit/receive calls.
  *
  * All constants below come from Semtech's reference driver enums, not from
  * memory -- the SX1280 parameter bytes are non-obvious (bit-rate and
  * bandwidth share one token, most fields are pre-shifted) and a single wrong
  * value produces a link that never connects with no diagnostic.
  ******************************************************************************
  */

#ifndef E28_RADIO_H
#define E28_RADIO_H

#ifdef __cplusplus
extern "C" {
#endif

#include "e28.h"
#include "e28_port.h"
#include <stdint.h>
#include <stdbool.h>

/* ---- Role -----------------------------------------------------------------
   Define E28_ROLE_TRANSMITTER to 1 in the car board's main.h. The base
   station leaves it at 0 and listens. Only affects the bring-up ping test;
   the real system transmits and receives on both ends. */
#ifndef E28_ROLE_TRANSMITTER
#define E28_ROLE_TRANSMITTER        0
#endif

/* ---- Packet types --------------------------------------------------------- */
#define E28_PACKET_TYPE_GFSK        0x00U
#define E28_PACKET_TYPE_LORA        0x01U
#define E28_PACKET_TYPE_RANGING     0x02U
#define E28_PACKET_TYPE_FLRC        0x03U
#define E28_PACKET_TYPE_BLE         0x04U

/* ---- GFSK modulation ------------------------------------------------------
   Bit rate and bandwidth are a single token -- they are not independent. */
#define E28_GFSK_BR_2_000_BW_2_4    0x04U
#define E28_GFSK_BR_1_600_BW_2_4    0x28U
#define E28_GFSK_BR_1_000_BW_2_4    0x4CU
#define E28_GFSK_BR_1_000_BW_1_2    0x45U
#define E28_GFSK_BR_0_800_BW_2_4    0x70U
#define E28_GFSK_BR_0_500_BW_1_2    0x8DU
#define E28_GFSK_BR_0_250_BW_0_6    0xCEU
#define E28_GFSK_BR_0_125_BW_0_3    0xEFU

#define E28_GFSK_MOD_IND_0_50       0x01U
#define E28_GFSK_MOD_IND_1_00       0x03U

#define E28_MOD_SHAPING_BT_OFF      0x00U
#define E28_MOD_SHAPING_BT_1_0      0x10U
#define E28_MOD_SHAPING_BT_0_5      0x20U

/* ---- Packet parameters ---------------------------------------------------- */
#define E28_PREAMBLE_16_BITS        0x30U
#define E28_PREAMBLE_32_BITS        0x70U

#define E28_SYNCWORD_LEN_4_BYTE     0x06U
#define E28_SYNCWORD_LEN_5_BYTE     0x08U

#define E28_RX_MATCH_SYNCWORD_OFF   0x00U
#define E28_RX_MATCH_SYNCWORD_1     0x10U

#define E28_PACKET_FIXED_LENGTH     0x00U
#define E28_PACKET_VARIABLE_LENGTH  0x20U

#define E28_CRC_OFF                 0x00U
#define E28_CRC_1_BYTE              0x10U
#define E28_CRC_2_BYTES             0x20U
#define E28_CRC_3_BYTES             0x30U

#define E28_WHITENING_ON            0x00U
#define E28_WHITENING_OFF           0x08U

/* ---- IRQ flags ------------------------------------------------------------ */
#define E28_IRQ_NONE                0x0000U
#define E28_IRQ_TX_DONE             0x0001U
#define E28_IRQ_RX_DONE             0x0002U
#define E28_IRQ_SYNCWORD_VALID      0x0004U
#define E28_IRQ_SYNCWORD_ERROR      0x0008U
#define E28_IRQ_CRC_ERROR           0x0040U
#define E28_IRQ_RX_TX_TIMEOUT       0x4000U
#define E28_IRQ_PREAMBLE_DETECTED   0x8000U
#define E28_IRQ_ALL                 0xFFFFU

/* ---- Timeout tick sizes --------------------------------------------------- */
#define E28_TICK_0015_US            0x00U
#define E28_TICK_0062_US            0x01U
#define E28_TICK_1000_US            0x02U
#define E28_TICK_4000_US            0x03U

/* ---- Regulator ------------------------------------------------------------ */
#define E28_REG_MODE_LDO            0x00U
#define E28_REG_MODE_DCDC           0x01U

/* ---- Registers ------------------------------------------------------------ */
#define E28_REG_SYNCWORD1           0x09CEU
#define E28_REG_FIRMWARE_VERSION    0x0153U

/* ---- Channel plan ---------------------------------------------------------
   2.4 GHz ISM shared with Wi-Fi, whose 20 MHz channels sit at 2412, 2437 and
   2462 MHz. These four land in the gaps between them, which matters at a
   competition venue where every team and spectator has a hotspot. */
#define E28_CHANNEL_COUNT           4U
#define E28_CHANNEL_0_HZ            2403000000UL
#define E28_CHANNEL_1_HZ            2427000000UL
#define E28_CHANNEL_2_HZ            2452000000UL
#define E28_CHANNEL_3_HZ            2478000000UL

/* Largest payload the SX1281 will carry in one packet. */
#define E28_MAX_PAYLOAD             255U

/* ---- API ------------------------------------------------------------------ */

/**
  * @brief Configure the modem. Call once, after the self-test passes.
  *        Leaves the radio in standby.
  */
bool E28_Radio_Config(void);

/**
  * @brief Retune. Radio must be in standby; returns it to standby.
  */
bool E28_Radio_SetChannel(uint8_t channel);

/**
  * @brief Load a payload and transmit it. Returns once the packet is queued;
  *        completion arrives as a DIO1 interrupt.
  */
bool E28_Radio_Send(const uint8_t *data, uint8_t len);

/**
  * @brief Put the radio into continuous receive.
  */
bool E28_Radio_StartRx(void);

/**
  * @brief Fetch the most recently received packet, if any.
  * @retval true if a packet was copied out.
  */
bool E28_Radio_GetRxPacket(uint8_t *buf, uint8_t *len, uint8_t max_len);

/* ---- Bring-up counters, all watchable in the debugger --------------------- */
uint32_t E28_Radio_GetTxCount(void);
uint32_t E28_Radio_GetRxCount(void);
uint32_t E28_Radio_GetCrcErrorCount(void);
int8_t   E28_Radio_GetLastRssi(void);      /*!< dBm, negative              */
uint16_t E28_Radio_GetLastIrq(void);
uint32_t E28_Radio_GetDio1Count(void); /*!< DIO1 interrupts seen         */
bool     E28_Radio_IsConfigured(void); /*!< E28_Radio_Config() succeeded */
bool     E28_Radio_IsRxArmed(void);    /*!< SetRx accepted               */
uint32_t E28_Radio_GetEchoRxCount(void);/*!< good echoes / relays    */
uint32_t E28_Radio_GetSeqGaps(void);   /*!< round-trip packet losses     */
#if E28_ROLE_TRANSMITTER
uint32_t E28_Radio_GetTelemSent(void);   /*!< telemetry packets to air     */
uint32_t E28_Radio_GetAudioSent(void);   /*!< voice packets to air         */
uint32_t E28_Radio_GetAudioYielded(void);/*!< voice dropped for telemetry  */
#endif

#if E28_USE_RTOS
/**
  * @brief Blocking radio loop for the RTOS build. Called by E28_Task once the
  *        self-test passes; never returns.
  */
void E28_Radio_Loop(void);
#else
/**
  * @brief Configure the radio and arm receive. Bare-metal build.
  * @retval false if configuration failed; poll() then does nothing.
  */
bool E28_Radio_RelayInit(void);

/**
  * @brief One step of the relay state machine. Call repeatedly from the main
  *        loop; always returns promptly so USB keeps getting serviced.
  */
void E28_Radio_RelayPoll(void);

/* Relay statistics, bare-metal build only. */
uint32_t E28_Radio_GetUsbForwarded(void);    /*!< radio -> PC              */
uint32_t E28_Radio_GetAirFromUsb(void);      /*!< PC -> radio              */
uint32_t E28_Radio_GetUsbBackpressure(void); /*!< PC not reading fast enough */
#endif

#ifdef __cplusplus
}
#endif

#endif /* E28_RADIO_H */
