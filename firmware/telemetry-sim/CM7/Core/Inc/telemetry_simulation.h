/**
  ******************************************************************************
  * @file    telemetry_sim.h
  * @brief   VCU telemetry simulator for the NUCLEO-H755ZI-Q.
  *
  * Stands in for the VCU so the radio board's RS-485 receiver, the priority
  * scheduler and the whole air path can be proven before the VCU firmware
  * exists.
  *
  * Emits exactly the frame format in VCU_RS485_TELEMETRY_SPEC.md, carrying
  * real CANLOG records built from the team's own DBC message IDs. Same output
  * as rs485_sim.py, but self-contained -- no PC needed on the car side.
  *
  * WIRING (see the test procedure for the full picture)
  *   Nucleo USART2 TX  ->  radio board, RS-485 transceiver U110 pin 1 (R)
  *   Nucleo GND        ->  radio board GND
  *
  * and build the radio board with RS485_BENCH_TEST set to 1, which holds
  * RE_DE high so the transceiver's receiver output goes high-impedance and
  * does not fight the Nucleo. That drives the radio board's USART pin directly
  * at TTL level: no transceiver, no differential pair, no current through the
  * 120 ohm termination that a GPIO cannot supply.
  *
  * Everything downstream of the transceiver is exercised: USART, DMA, framing,
  * CRC, record batching, telemetry priority over voice, the air link, the
  * relay, USB, and the PC decoder. Only the transceiver itself is untested,
  * and that can wait for the real VCU.
  ******************************************************************************
  */

#ifndef TELEMETRY_SIM_H
#define TELEMETRY_SIM_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include <stdint.h>
#include <stdbool.h>

/* CAN records per second. 200 is a plausible steady rate for a car with four
   busy buses; the radio link can carry far more. */
#ifndef TELEMSIM_RECORDS_PER_SEC
#define TELEMSIM_RECORDS_PER_SEC    200U
#endif

/* Partial batch flush, matching the spec. */
#ifndef TELEMSIM_FLUSH_MS
#define TELEMSIM_FLUSH_MS           20U
#endif

/* Air payload limit, so a frame never exceeds what one radio packet holds. */
#define TELEMSIM_MAX_PAYLOAD        251U

/**
  * @brief Reset the generator. Call once before the main loop.
  */
void TelemetrySim_Init(void);

/**
  * @brief Generate and send whatever is due. Call continuously from the main
  *        loop; it paces itself off HAL_GetTick().
  */
void TelemetrySim_Poll(void);

/**
  * @brief Burst mode: alternates near-idle with 5x rate every second.
  *
  * Steady traffic will not find batching or queue-depth bugs -- those only
  * show up when the rate changes abruptly. Toggled by the blue B1 button.
  */
void TelemetrySim_SetBurst(bool enable);
bool TelemetrySim_GetBurst(void);

/* Counters, for the debugger or a status LED. */
uint32_t TelemetrySim_GetRecords(void);
uint32_t TelemetrySim_GetFrames(void);
uint32_t TelemetrySim_GetTxErrors(void);

#ifdef __cplusplus
}
#endif

#endif /* TELEMETRY_SIM_H */
