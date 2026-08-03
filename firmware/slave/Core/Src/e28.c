/**
  ******************************************************************************
  * @file    e28.c
  * @brief   Low-level driver for the E28-2G4M27SX (SX1280). See e28.h.
  ******************************************************************************
  */

#include "e28.h"
#include "e28_port.h"
#include <string.h>

#if E28_USE_RTOS
#include "cmsis_os2.h"
#endif

/* Bound to a specific SPI instance by E28_SPI_HANDLE in e28.h, so the same
   driver serves both the car board (SPI2) and the base station (SPI1). */
extern SPI_HandleTypeDef E28_SPI_HANDLE;

#define E28_SPI_TIMEOUT_MS   100U
#define E28_BUSY_TIMEOUT_MS  100U

/* Longest payload a caller may hand to the command/register helpers. */
#define E28_SPI_MAX_PAYLOAD  64U

/* Worst case on the wire: the 4-byte ReadRegister header plus that payload.
   Kept separate from the payload limit because every frame here is header +
   data assembled into ONE buffer, and sizing the buffers by the payload alone
   silently rejects a full-length transfer. */
#define E28_SPI_MAX_FRAME    (4U + E28_SPI_MAX_PAYLOAD)


#if E28_USE_RTOS
static osThreadId_t e28TaskHandle;
#endif
static volatile E28_TestResult_t test_result = E28_TEST_NOT_RUN;
static volatile uint8_t test_step = 0U;   /* see E28_SelfTest() for meanings */
static uint8_t last_status[3];

/* Same three bytes packed into one word: byte0 in bits 7:0, byte1 in 15:8,
   byte2 in 23:16. Watching an array in the debugger shows its address rather
   than its contents, so this is simply easier to read. */
static volatile uint32_t last_status_word;


#if E28_USE_RTOS
static const osThreadAttr_t e28Task_attributes = {
  .name       = "e28Task",
  .priority   = (osPriority_t) osPriorityNormal,  /* below audio (High) */
  .stack_size = 512 * 4
};

static void E28_Task(void *argument);
#endif

static E28_TestResult_t E28_SelfTest(void);

/* ---- Chip select -------------------------------------------------------- */

static inline void cs_assert(void)
{
  HAL_GPIO_WritePin(E28_CS_GPIO_Port, E28_CS_Pin, GPIO_PIN_RESET);
}

static inline void cs_release(void)
{
  HAL_GPIO_WritePin(E28_CS_GPIO_Port, E28_CS_Pin, GPIO_PIN_SET);
}

/* ---- SPI transfer helper -------------------------------------------------
   Everything goes through TransmitReceive, even writes where the returned
   bytes are discarded.

   The reason: HAL_SPI_Transmit does not drain the RX FIFO as it goes -- it
   just clears the overrun flag at the end, which pops a single entry. On a
   32-bit FIFO an N-byte write can leave up to three stale bytes behind, and
   those surface as corruption at the *start* of the next HAL_SPI_Receive.
   That would have quietly broken the register read-back test. TransmitReceive
   reads one byte for every byte it writes, so the FIFO never accumulates.

   Equally important: exactly ONE call per chip-select window. The HAL treats
   each transfer as a complete transaction -- it programs a size, starts it,
   waits for end-of-transfer, then disables the peripheral. Restarting it while
   CS is still asserted can emit stray clock edges and lose byte alignment,
   which the radio sees as a corrupted command. So callers assemble header and
   payload into one buffer and issue a single transfer. */

static bool spi_xfer(const uint8_t *tx, uint8_t *rx, uint16_t len)
{
  static uint8_t dump[E28_SPI_MAX_FRAME];
  static const uint8_t zeros[E28_SPI_MAX_FRAME] = { 0 };

  if (len > E28_SPI_MAX_FRAME)
  {
    return false;
  }

  const uint8_t *ptx = (tx != NULL) ? tx : zeros;
  uint8_t       *prx = (rx != NULL) ? rx : dump;

  return (HAL_SPI_TransmitReceive(&E28_SPI_HANDLE, (uint8_t *)ptx, prx, len,
                                  E28_SPI_TIMEOUT_MS) == HAL_OK);
}

/* ---- BUSY handshake ------------------------------------------------------ */

/**
  * @brief Block until the radio lowers BUSY.
  *
  * Spin-waits rather than osDelay()-ing: BUSY is typically released in tens of
  * microseconds, and a 1 ms scheduler tick would dominate every transaction.
  * Acceptable because this task sits below audio in priority and the wait is
  * bounded. If a later profiling pass shows this hurting, the fix is to make
  * BUSY an EXTI source and block on a thread flag instead.
  */
bool E28_WaitReady(uint32_t timeout_ms)
{
  uint32_t start = HAL_GetTick();

  while (HAL_GPIO_ReadPin(E28_BUSY_GPIO_Port, E28_BUSY_Pin) == GPIO_PIN_SET)
  {
    if ((HAL_GetTick() - start) > timeout_ms)
    {
      return false;
    }
  }
  return true;
}

/* ---- Reset --------------------------------------------------------------- */

void E28_Reset(void)
{
  /* Park the front-end switch: neither TX nor RX path enabled. Note the board
     has 10k pull-ups (R130/R131) on these lines, so they sit high until the
     GPIO driver takes over -- worth driving low explicitly and early. */
  HAL_GPIO_WritePin(E28_TX_EN_GPIO_Port, E28_TX_EN_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(E28_RX_EN_GPIO_Port, E28_RX_EN_Pin, GPIO_PIN_RESET);

  cs_release();

  E28_Port_Delay(10);
  HAL_GPIO_WritePin(E28_NRESET_GPIO_Port, E28_NRESET_Pin, GPIO_PIN_RESET);
  E28_Port_Delay(20);
  HAL_GPIO_WritePin(E28_NRESET_GPIO_Port, E28_NRESET_Pin, GPIO_PIN_SET);
  E28_Port_Delay(20);
}

/* ---- SPI transactions ---------------------------------------------------- */

bool E28_WriteCommand(uint8_t opcode, const uint8_t *params, uint16_t len)
{
  uint8_t buf[1U + E28_SPI_MAX_PAYLOAD];

  if (len > E28_SPI_MAX_PAYLOAD) { return false; }
  if (!E28_WaitReady(E28_BUSY_TIMEOUT_MS)) { return false; }

  buf[0] = opcode;
  if (params != NULL && len > 0U)
  {
    memcpy(&buf[1], params, len);
  }

  cs_assert();
  bool ok = spi_xfer(buf, NULL, 1U + len);
  cs_release();

  /* Deliberately not waiting for BUSY here: the caller's next transaction
     does that, and some commands (SetTx/SetRx) hold BUSY for a long time. */
  return ok;
}

bool E28_ReadCommand(uint8_t opcode, uint8_t *result, uint16_t len)
{
  if (len > E28_SPI_MAX_PAYLOAD) { return false; }
  if (!E28_WaitReady(E28_BUSY_TIMEOUT_MS)) { return false; }

  if (opcode == E28_CMD_GET_STATUS)
  {
    /* GetStatus is the odd one out: no NOP byte before the answer. */
    uint8_t tx[3] = { E28_CMD_GET_STATUS, 0x00U, 0x00U };
    uint8_t rx[3] = { 0U, 0U, 0U };

    cs_assert();
    bool ok = spi_xfer(tx, rx, 3U);
    cs_release();

    last_status[0] = rx[0];
    last_status[1] = rx[1];
    last_status[2] = rx[2];

    last_status_word = (uint32_t)rx[0]
                     | ((uint32_t)rx[1] << 8)
                     | ((uint32_t)rx[2] << 16);

    if (ok && len > 0U && result != NULL)
    {
      result[0] = rx[0];
    }
    return ok;
  }
  else
  {
    uint8_t tx_buf[2U + E28_SPI_MAX_PAYLOAD];
    uint8_t rx_buf[2U + E28_SPI_MAX_PAYLOAD];

    memset(tx_buf, 0, sizeof(tx_buf));
    tx_buf[0] = opcode;
    tx_buf[1] = 0x00U;                    /* NOP before the data */

    cs_assert();
    bool ok = spi_xfer(tx_buf, rx_buf, 2U + len);
    cs_release();

    if (ok && result != NULL && len > 0U)
    {
      memcpy(result, &rx_buf[2], len);
    }
    return ok;
  }
}

bool E28_WriteRegister(uint16_t addr, const uint8_t *data, uint16_t len)
{
  uint8_t buf[3U + E28_SPI_MAX_PAYLOAD];

  if (len > E28_SPI_MAX_PAYLOAD) { return false; }
  if (!E28_WaitReady(E28_BUSY_TIMEOUT_MS)) { return false; }

  buf[0] = E28_CMD_WRITE_REGISTER;
  buf[1] = (uint8_t)((addr >> 8) & 0xFFU);
  buf[2] = (uint8_t)(addr & 0xFFU);

  if (data != NULL && len > 0U)
  {
    memcpy(&buf[3], data, len);
  }

  cs_assert();
  bool ok = spi_xfer(buf, NULL, 3U + len);
  cs_release();

  return ok;
}

bool E28_ReadRegister(uint16_t addr, uint8_t *data, uint16_t len)
{
  uint8_t tx_buf[4U + E28_SPI_MAX_PAYLOAD];
  uint8_t rx_buf[4U + E28_SPI_MAX_PAYLOAD];

  if (len > E28_SPI_MAX_PAYLOAD) { return false; }
  if (!E28_WaitReady(E28_BUSY_TIMEOUT_MS)) { return false; }

  memset(tx_buf, 0, sizeof(tx_buf));
  tx_buf[0] = E28_CMD_READ_REGISTER;
  tx_buf[1] = (uint8_t)((addr >> 8) & 0xFFU);
  tx_buf[2] = (uint8_t)(addr & 0xFFU);
  tx_buf[3] = 0x00U;                      /* NOP before the data */

  cs_assert();
  bool ok = spi_xfer(tx_buf, rx_buf, 4U + len);
  cs_release();

  if (ok && data != NULL && len > 0U)
  {
    memcpy(data, &rx_buf[4], len);
  }

  return ok;
}

/* ---- Self-test ----------------------------------------------------------- */

/**
  * @brief Prove the SPI link and the chip are both alive.
  *
  * Deliberately layered so a failure narrows the cause:
  *   BUSY never drops      -> module unpowered, or NRESET/BUSY miswired
  *   status 0x00 / 0xFF    -> MISO stuck; check MISO, CS polarity, SPI mode
  *   register mismatch     -> link works one way but data is corrupt;
  *                            usually SPI clock too fast or wrong CPOL/CPHA
  */
static E28_TestResult_t E28_SelfTest(void)
{
  uint8_t status = 0U;
  const uint8_t pattern[5]  = { 0xA5U, 0x5AU, 0xC3U, 0x3CU, 0x96U };
  uint8_t readback[5] = { 0U, 0U, 0U, 0U, 0U };

  /* test_step is bumped as we go so a hang is locatable in the debugger:
       1 = entered, about to reset      4 = GetStatus returned
       2 = reset done, waiting on BUSY  5 = standby command sent
       3 = BUSY low, sending GetStatus  6 = register written
                                        7 = register read back */
  test_step = 1U;
  E28_Reset();

  test_step = 2U;
  if (!E28_WaitReady(E28_BUSY_TIMEOUT_MS))
  {
    return E28_TEST_ERR_BUSY_STUCK;
  }

  test_step = 3U;
  if (!E28_ReadCommand(E28_CMD_GET_STATUS, &status, 1U))
  {
    return E28_TEST_ERR_SPI_FAULT;
  }

  test_step = 4U;

  /* On this board the status arrives in byte 1, not byte 0: byte 0 reads 0xFF
     because the line is still idling high while the opcode goes out. Semtech's
     reference driver assumes byte 0, which is why scanning all three positions
     matters here. A healthy post-reset answer is 0x4x -- bits 7:5 are the
     circuit mode, and 0b010 is STDBY_RC. */
  status = 0x00U;
  for (uint32_t i = 0U; i < 3U; i++)
  {
    if (last_status[i] != 0x00U && last_status[i] != 0xFFU)
    {
      status = last_status[i];
      break;
    }
  }

  if (status == 0x00U)
  {
    return E28_TEST_ERR_NO_RESPONSE;
  }

  /* Put the radio somewhere predictable before touching registers. */
  const uint8_t standby_rc = E28_STDBY_RC;
  if (!E28_WriteCommand(E28_CMD_SET_STANDBY, &standby_rc, 1U))
  {
    return E28_TEST_ERR_SPI_FAULT;
  }

  test_step = 5U;
  if (!E28_WaitReady(E28_BUSY_TIMEOUT_MS))
  {
    return E28_TEST_ERR_BUSY_STUCK;
  }

  /* Round-trip a known pattern. This checks MOSI and MISO independently of
     each other -- GetStatus alone only proves the chip can talk, not listen. */
  if (!E28_WriteRegister(E28_REG_SYNCWORD1_BASE, pattern, sizeof(pattern)))
  {
    return E28_TEST_ERR_SPI_FAULT;
  }

  test_step = 6U;
  if (!E28_ReadRegister(E28_REG_SYNCWORD1_BASE, readback, sizeof(readback)))
  {
    return E28_TEST_ERR_SPI_FAULT;
  }

  test_step = 7U;

  for (uint32_t i = 0U; i < sizeof(pattern); i++)
  {
    if (readback[i] != pattern[i])
    {
      return E28_TEST_ERR_REG_MISMATCH;
    }
  }

  return E28_TEST_OK;
}

/* ---- Task ---------------------------------------------------------------- */

#if E28_USE_RTOS

static void E28_Task(void *argument)
{
  (void)argument;

  E28_Port_Bind();

  /* Set before doing anything else: if the debugger shows RUNNING the task is
     alive and the test hung; if it still shows NOT_RUN the task never ran. */
  test_result = E28_TEST_RUNNING;
  test_result = E28_SelfTest();

  if (test_result == E28_TEST_OK)
  {
    /* Hand over to the radio layer, which does not return. Deliberately gated
       on the self-test: configuring a radio we cannot reliably talk to would
       just produce confusing downstream failures. */
    E28_Radio_Loop();
  }

  for (;;)
  {
    E28_Port_Delay(1000);
  }
}

void E28_Init(void)
{
  e28TaskHandle = osThreadNew(E28_Task, NULL, &e28Task_attributes);

  /* Most likely cause of a NULL handle is configTOTAL_HEAP_SIZE being too
     small for one more task stack. Silent failure here would look exactly
     like "the test never ran", so make it visible. */
  if (e28TaskHandle == NULL)
  {
    test_result = E28_TEST_ERR_NO_TASK;
  }
}

#else   /* bare metal */

void E28_Init(void)
{
  /* No task to create. The self-test runs here and now: it takes about 50 ms,
     all of it before the main loop starts, so blocking is harmless. The caller
     then polls the relay from the main loop. */
  E28_Port_Bind();

  test_result = E28_TEST_RUNNING;
  test_result = E28_SelfTest();
}

#endif  /* E28_USE_RTOS */

E28_TestResult_t E28_GetTestResult(void)
{
  return test_result;
}

uint8_t E28_GetTestStep(void)
{
  return test_step;
}

uint32_t E28_GetLastStatusWord(void)
{
  return last_status_word;
}

bool E28_SetTxPower(int8_t dbm, uint8_t ramp_time)
{
  uint8_t params[2];

  if (dbm < E28_TX_POWER_MIN_DBM) { dbm = E28_TX_POWER_MIN_DBM; }
  if (dbm > E28_TX_POWER_MAX_DBM) { dbm = E28_TX_POWER_MAX_DBM; }

  params[0] = (uint8_t)(dbm + 18);   /* datasheet encoding: -18 dBm -> 0 */
  params[1] = ramp_time;

  return E28_WriteCommand(E28_CMD_SET_TXPARAMS, params, 2U);
}

/* ---- DIO1 interrupt ------------------------------------------------------ */

/**
  * @brief EXTI callback. DIO1 is the radio's IRQ line (TxDone, RxDone, etc).
  *
  * ALL THREE of these are defined on purpose. The HAL changed this API between
  * families: the STM32G4 dispatches to a single HAL_GPIO_EXTI_Callback, while
  * the newer STM32U3/U5/H5 HAL splits it into Rising and Falling variants and
  * has no combined callback at all.
  *
  * Defining only the G4 name on a U3 compiles cleanly and silently never
  * fires -- the interrupt reaches HAL_GPIO_EXTI_IRQHandler, which dispatches
  * to a weak HAL stub. Defining all three means whichever one the HAL
  * actually calls gets through, and the unused ones cost a few bytes of
  * flash. Worth it to keep one driver building on both boards.
  */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
  if (GPIO_Pin == E28_DIO1_Pin)
  {
    E28_Dio1Callback();
  }
}

void HAL_GPIO_EXTI_Rising_Callback(uint16_t GPIO_Pin)
{
  if (GPIO_Pin == E28_DIO1_Pin)
  {
    E28_Dio1Callback();
  }
}

void HAL_GPIO_EXTI_Falling_Callback(uint16_t GPIO_Pin)
{
  (void)GPIO_Pin;   /* DIO1 is configured rising-edge only */
}

/* Weak defaults. Linking e28_radio.c replaces both. Keeping them here means
   e28.c builds and self-tests on its own, with or without the radio layer. */

__attribute__((weak)) void E28_Dio1Callback(void)
{
}

__attribute__((weak)) void E28_Radio_Loop(void)
{
}
