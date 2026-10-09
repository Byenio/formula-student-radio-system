/**
  ******************************************************************************
  * @file    can_bus.c
  * @brief   CAN interface on the car radio board. See can_bus.h.
  ******************************************************************************
  */

#include "can_bus.h"
#include "ptt.h"
#include "cmsis_os2.h"

extern FDCAN_HandleTypeDef hfdcan1;

/* ---- State --------------------------------------------------------------- */

static volatile bool     link_alive;
static volatile bool     ptt_request;
static volatile uint8_t  last_counter;
static volatile bool     have_counter;
static volatile uint32_t last_fresh_ms;

static volatile uint32_t rx_count;
static volatile uint32_t stale_count;
static volatile uint32_t link_drops;
static volatile uint32_t error_count;

/* -------------------------------------------------------------------------- */

void CanBus_Init(void)
{
  FDCAN_FilterTypeDef filter;

  /* Wake the TCAN3414. Both pins are active high, so driving them low puts the
     transceiver in normal mode; they idle high on their pull-ups otherwise,
     which would leave the bus silent and look exactly like a wiring fault. */
  HAL_GPIO_WritePin(CAN_SHDN_GPIO_Port, CAN_SHDN_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(CAN_STB_GPIO_Port,  CAN_STB_Pin,  GPIO_PIN_RESET);

  /* Accept only RADIO_CONTROL into FIFO0. Filtering in hardware rather than
     in software matters here: the car's buses carry hundreds of messages a
     second, and waking the CPU for every one of them to discard it would be
     pure waste. */
  filter.IdType       = FDCAN_STANDARD_ID;
  filter.FilterIndex  = 0U;
  filter.FilterType   = FDCAN_FILTER_DUAL;
  filter.FilterConfig = FDCAN_FILTER_TO_RXFIFO0;
  filter.FilterID1    = CAN_ID_RADIO_CONTROL;
  filter.FilterID2    = CAN_ID_RADIO_CONTROL;

  if (HAL_FDCAN_ConfigFilter(&hfdcan1, &filter) != HAL_OK)
  {
    Error_Handler();
  }

  /* Everything that does not match is rejected outright. */
  if (HAL_FDCAN_ConfigGlobalFilter(&hfdcan1,
                                   FDCAN_REJECT, FDCAN_REJECT,
                                   FDCAN_REJECT_REMOTE,
                                   FDCAN_REJECT_REMOTE) != HAL_OK)
  {
    Error_Handler();
  }

  if (HAL_FDCAN_Start(&hfdcan1) != HAL_OK)
  {
    Error_Handler();
  }

  if (HAL_FDCAN_ActivateNotification(&hfdcan1,
                                     FDCAN_IT_RX_FIFO0_NEW_MESSAGE,
                                     0U) != HAL_OK)
  {
    Error_Handler();
  }

  link_alive    = false;
  ptt_request   = false;
  have_counter  = false;
  last_fresh_ms = HAL_GetTick();
}

/* -------------------------------------------------------------------------- */

void CanBus_Poll(void)
{
  if (link_alive && ((HAL_GetTick() - last_fresh_ms) > CAN_LIVENESS_TIMEOUT_MS))
  {
    link_alive = false;
    link_drops++;

    /* Fail safe: close the mic. A VCU that has stopped talking cannot tell us
       to stop transmitting, so leaving the mic where it was would jam the
       uplink indefinitely. */
    Ptt_SetRemote(false);
  }
}

bool CanBus_IsLinkAlive(void)   { return link_alive; }
bool CanBus_GetPttRequest(void) { return ptt_request; }

uint32_t CanBus_GetRxCount(void)     { return rx_count; }
uint32_t CanBus_GetStaleCount(void)  { return stale_count; }
uint32_t CanBus_GetLinkDrops(void)   { return link_drops; }
uint32_t CanBus_GetErrorCount(void)  { return error_count; }
uint8_t  CanBus_GetLastCounter(void) { return last_counter; }

/* ---- Interrupt callbacks -------------------------------------------------- */

void HAL_FDCAN_RxFifo0Callback(FDCAN_HandleTypeDef *hfdcan, uint32_t RxFifo0ITs)
{
  FDCAN_RxHeaderTypeDef header;
  uint8_t data[8];

  if ((RxFifo0ITs & FDCAN_IT_RX_FIFO0_NEW_MESSAGE) == 0U)
  {
    return;
  }

  /* Drain the FIFO: several messages can queue while we were busy, and
     leaving any behind would delay the next notification. */
  while (HAL_FDCAN_GetRxFifoFillLevel(hfdcan, FDCAN_RX_FIFO0) > 0U)
  {
    if (HAL_FDCAN_GetRxMessage(hfdcan, FDCAN_RX_FIFO0, &header, data) != HAL_OK)
    {
      error_count++;
      return;
    }

    if (header.Identifier != CAN_ID_RADIO_CONTROL)
    {
      continue;                       /* filter should prevent this */
    }

    rx_count++;

    bool    ptt     = (data[0] & 0x01U) != 0U;
    uint8_t counter = (uint8_t)((data[0] >> 1) & 0x7FU);

    /* A repeated counter means the VCU has not produced a new message -- this
       is a stale copy, or the bus is echoing. Do not treat it as proof of
       life, or a stuck transmitter would hold the mic open. */
    if (have_counter && (counter == last_counter))
    {
      stale_count++;
      continue;
    }

    last_counter  = counter;
    have_counter  = true;
    last_fresh_ms = HAL_GetTick();
    link_alive    = true;
    ptt_request   = ptt;

    /* The CAN request is absolute, unlike the board button which toggles.
       Ptt_SetRemote only acts on a change, so repeating the same state every
       100 ms costs nothing. */
    Ptt_SetRemote(ptt);
  }
}

void HAL_FDCAN_ErrorCallback(FDCAN_HandleTypeDef *hfdcan)
{
  (void)hfdcan;
  error_count++;
}

void HAL_FDCAN_ErrorStatusCallback(FDCAN_HandleTypeDef *hfdcan, uint32_t ErrorStatusITs)
{
  (void)hfdcan;
  (void)ErrorStatusITs;
  error_count++;
}
