/**
  ******************************************************************************
  * @file    can_bus.h
  * @brief   CAN interface on the car radio board.
  *
  * The board is the `RADIO` node in the team's DBC. It sits on the car's CAN3
  * bus, though this board has only one CAN interface -- "CAN3" is the bus name
  * on the car, not a peripheral number here.
  *
  * RECEIVE (implemented)
  *   RADIO_CONTROL   ID 0x480 (1152), 1 byte, VCU -> RADIO, every 100 ms
  *     bit 0     RADIO_PTT       mic open request
  *     bits 1-7  RADIO_COUNTER   0..127, increments every message
  *
  * TRANSMIT (defined in the DBC, deliberately not implemented yet)
  *   RADIO_STATISTICS  ID 0x490 (1168), 4 B, every 100 ms
  *   RADIO_VERSION     ID 0x503 (1283), 6 B, every 1000 ms
  *
  * WHY THE COUNTER MATTERS
  * -----------------------
  * RADIO_COUNTER is a liveness counter, not decoration. If the VCU stops
  * transmitting while the PTT bit happens to be set -- a crash, a broken wire,
  * the connector falling out -- the last received bit would leave the mic open
  * forever, jamming the uplink with nobody able to intervene. The steering
  * wheel indicator cannot warn about that either, because the VCU driving it
  * is the thing that died.
  *
  * So a message only counts as fresh if the counter has advanced. If it stops
  * advancing, the link is treated as dead and the mic closes.
  ******************************************************************************
  */

#ifndef CAN_BUS_H
#define CAN_BUS_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include <stdint.h>
#include <stdbool.h>

/* ---- Message IDs, from the team DBC (can3.dbc) --------------------------- */
#define CAN_ID_RADIO_CONTROL        0x480U   /* 1152 */
#define CAN_ID_RADIO_STATISTICS     0x490U   /* 1168, not sent yet */
#define CAN_ID_RADIO_VERSION        0x503U   /* 1283, not sent yet */

/* How long without an advancing counter before the link counts as dead.
   Three missed messages at the DBC's 100 ms cycle time -- long enough to ride
   out a single dropped frame, short enough that a stuck mic self-clears
   quickly. */
#ifndef CAN_LIVENESS_TIMEOUT_MS
#define CAN_LIVENESS_TIMEOUT_MS     350U
#endif

/**
  * @brief Bring up the transceiver and the CAN peripheral, and start receiving.
  *        Call alongside the other init functions.
  */
void CanBus_Init(void);

/**
  * @brief Age the liveness timer. Call every ~20 ms from a task.
  */
void CanBus_Poll(void);

/**
  * @brief Is the VCU currently talking to us?
  *        False means no RADIO_CONTROL with an advancing counter recently.
  */
bool CanBus_IsLinkAlive(void);

/**
  * @brief Last received PTT request. Only meaningful while the link is alive.
  */
bool CanBus_GetPttRequest(void);

/* ---- Bring-up counters --------------------------------------------------- */

uint32_t CanBus_GetRxCount(void);       /*!< RADIO_CONTROL messages received */
uint32_t CanBus_GetStaleCount(void);    /*!< counter did not advance         */
uint32_t CanBus_GetLinkDrops(void);     /*!< alive -> dead transitions       */
uint32_t CanBus_GetErrorCount(void);    /*!< HAL/peripheral errors           */
uint8_t  CanBus_GetLastCounter(void);   /*!< last RADIO_COUNTER seen         */

#ifdef __cplusplus
}
#endif

#endif /* CAN_BUS_H */
