/**
  ******************************************************************************
  * @file    e28.h
  * @brief   Low-level driver for the Ebyte E28-2G4M27SX (Semtech SX1280).
  *
  * Bus: SPI2 (PB13 SCK / PB14 MISO / PB15 MOSI), software chip select on PB12.
  * Control: NRESET PB11, BUSY PA5, RX_EN PB10, TX_EN PB9, DIO1 PB5 (EXTI).
  *
  * THE BUSY RULE
  * -------------
  * The SX1280 raises BUSY while it is digesting a command. Asserting CS while
  * BUSY is high corrupts the transaction silently -- no error, just wrong
  * behaviour later. Every transaction here therefore waits for BUSY low before
  * pulling CS down, and again after releasing it. This is the single most
  * common source of "my SX1280 doesn't work" reports.
  *
  * Opcodes below are taken from Semtech's reference driver, not from memory.
  ******************************************************************************
  */

#ifndef E28_H
#define E28_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include <stdint.h>
#include <stdbool.h>

/* ---- Command opcodes (Semtech RadioCommands_t) --------------------------- */
#define E28_CMD_GET_STATUS          0xC0U
#define E28_CMD_WRITE_REGISTER      0x18U
#define E28_CMD_READ_REGISTER       0x19U
#define E28_CMD_WRITE_BUFFER        0x1AU
#define E28_CMD_READ_BUFFER         0x1BU
#define E28_CMD_SET_SLEEP           0x84U
#define E28_CMD_SET_STANDBY         0x80U
#define E28_CMD_SET_FS              0xC1U
#define E28_CMD_SET_TX              0x83U
#define E28_CMD_SET_RX              0x82U
#define E28_CMD_SET_PACKETTYPE      0x8AU
#define E28_CMD_GET_PACKETTYPE      0x03U
#define E28_CMD_SET_RFFREQUENCY     0x86U
#define E28_CMD_SET_TXPARAMS        0x8EU
#define E28_CMD_SET_BUFFERBASEADDR  0x8FU
#define E28_CMD_SET_MODULATIONPARAMS 0x8BU
#define E28_CMD_SET_PACKETPARAMS    0x8CU
#define E28_CMD_GET_RXBUFFERSTATUS  0x17U
#define E28_CMD_GET_PACKETSTATUS    0x1DU
#define E28_CMD_SET_DIOIRQPARAMS    0x8DU
#define E28_CMD_GET_IRQSTATUS       0x15U
#define E28_CMD_CLR_IRQSTATUS       0x97U
#define E28_CMD_SET_REGULATORMODE   0x96U

/* ---- Board binding -------------------------------------------------------
   Pin macros (E28_CS_Pin, E28_BUSY_Pin, E28_NRESET_Pin, E28_TX_EN_Pin,
   E28_RX_EN_Pin, E28_DIO1_Pin) all come from main.h, generated from the
   CubeMX user labels. Give both boards identical labels and this driver
   compiles unchanged on each -- only the SPI instance differs (SPI2 on the
   car board, SPI1 on the base station). */
#ifndef E28_SPI_HANDLE
#define E28_SPI_HANDLE              hspi2
#endif

/* Standby oscillator selection (argument to SET_STANDBY) */
#define E28_STDBY_RC                0x00U
#define E28_STDBY_XOSC              0x01U

/* ---- Transmit power ------------------------------------------------------
   SetTxParams encodes power as (dBm + 18), valid over -18..+13 dBm. Note this
   is the SX1281 die's own output: the E28 module's PA adds roughly 27 dB on
   top, so 0 dBm here means about 27 dBm at the antenna. Ebyte recommends
   exactly that -- pushing the die higher mostly just burns current. */
#define E28_TX_POWER_MIN_DBM        (-18)
#define E28_TX_POWER_MAX_DBM        (13)
#define E28_TX_POWER_NOMINAL_DBM    (0)     /* ~27 dBm out of the module   */
#define E28_TX_POWER_BENCH_DBM      (-18)   /* two boards on one desk      */

/* PA ramp times (second SetTxParams byte). Slower ramps mean less spectral
   splatter; 20 us is the conservative default. */
#define E28_RAMP_02_US              0x00U
#define E28_RAMP_04_US              0x20U
#define E28_RAMP_06_US              0x40U
#define E28_RAMP_08_US              0x60U
#define E28_RAMP_10_US              0x80U
#define E28_RAMP_12_US              0xA0U
#define E28_RAMP_16_US              0xC0U
#define E28_RAMP_20_US              0xE0U

/* A safely writable/readable register: SyncWord 1 base address.
   Used as a scratch location by the self-test. */
#define E28_REG_SYNCWORD1_BASE      0x09CEU

/* ---- Self-test result ---------------------------------------------------- */
typedef enum
{
  E28_TEST_NOT_RUN = 0,
  E28_TEST_OK,                 /*!< Chip answered and register R/W matched   */
  E28_TEST_ERR_BUSY_STUCK,     /*!< BUSY never went low -> no power / no clk */
  E28_TEST_ERR_NO_RESPONSE,    /*!< Status was 0x00 or 0xFF -> MISO dead     */
  E28_TEST_ERR_SPI_FAULT,      /*!< HAL SPI call itself failed               */
  E28_TEST_ERR_REG_MISMATCH,   /*!< Chip talks, but data came back wrong     */
  E28_TEST_RUNNING,            /*!< Task entered, test in progress           */
  E28_TEST_ERR_NO_TASK         /*!< osThreadNew() failed -- FreeRTOS heap    */
} E28_TestResult_t;

/* ---- Public API ---------------------------------------------------------- */

/**
  * @brief Create the radio thread. Call alongside the other osThreadNew()
  *        calls, after osKernelInitialize().
  */
void E28_Init(void);

/**
  * @brief Result of the power-on self-test. Watch this in the debugger.
  */
E28_TestResult_t E28_GetTestResult(void);

/**
  * @brief How far the self-test got. Watch alongside the result to see
  *        exactly which step hung if it never completes. See e28.c.
  */
uint8_t E28_GetTestStep(void);

/**
  * @brief The three GetStatus bytes packed into one word (byte0 in bits
  *        7:0). Easier to read in a debugger than the raw array.
  */
uint32_t E28_GetLastStatusWord(void);

/**
  * @brief Set the die's transmit power and PA ramp time.
  * @param dbm       -18..+13; clamped if out of range. Remember the module's
  *                  PA adds ~27 dB on top of this.
  * @param ramp_time One of the E28_RAMP_* values.
  */
bool E28_SetTxPower(int8_t dbm, uint8_t ramp_time);

/**
  * @brief Called from the DIO1 EXTI interrupt. Weak no-op here; the radio
  *        layer overrides it. Keeps e28.c free of any dependency on the
  *        layer above it.
  */
void E28_Dio1Callback(void);

/**
  * @brief Entry point for the radio layer, invoked by E28_Task once the
  *        self-test passes. Weak no-op here; never returns when overridden.
  */
void E28_Radio_Loop(void);

/* Low-level primitives, exposed so the protocol layer can build on them. */
bool E28_WaitReady(uint32_t timeout_ms);
void E28_Reset(void);
bool E28_WriteCommand(uint8_t opcode, const uint8_t *params, uint16_t len);
bool E28_ReadCommand(uint8_t opcode, uint8_t *result, uint16_t len);
bool E28_WriteRegister(uint16_t addr, const uint8_t *data, uint16_t len);
bool E28_ReadRegister(uint16_t addr, uint8_t *data, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* E28_H */
