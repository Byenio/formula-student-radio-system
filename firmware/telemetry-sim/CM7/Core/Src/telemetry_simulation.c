/**
  ******************************************************************************
  * @file    telemetry_sim.c
  * @brief   VCU telemetry simulator for the Nucleo. See telemetry_sim.h.
  ******************************************************************************
  */

#include "telemetry_simulation.h"
#include <string.h>

/* Set to 1 to send over the bit-banged differential pair instead of USART2.
   Needed for the bench link: a single-ended GPIO into one line of an RS-485
   pair leaves half of every byte sitting on the receiver's failsafe threshold,
   which produces a framing error on almost every byte. See bb_diff.h. */
#ifndef TELEMSIM_USE_BITBANG
#define TELEMSIM_USE_BITBANG    1
#endif

#if TELEMSIM_USE_BITBANG
#include "bb_diff.h"
#endif

extern UART_HandleTypeDef huart2;

/* ---- Frame format -------------------------------------------------------- */

#define SYNC0                   0xA5U
#define SYNC1                   0x5AU
#define CRC_POLY                0x1021U
#define CRC_INIT                0xFFFFU

/* ---- CANLOG encoding -----------------------------------------------------
   Mirrors CANLOG_Encode() in the VCU firmware:

     [header][data 0..n][ID: 2 B std / 4 B ext][time: 4 B ms]
     header = dlccode(0-3) | ext(4) | channel(5-6) | dir(7)                  */

static const uint8_t dlc_table[16] =
{
  0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 12U, 16U, 20U, 24U, 32U, 48U, 64U
};

/* ---- Message pool --------------------------------------------------------
   Real IDs from the team's DBC files, so the traffic is representative rather
   than invented. Rates are deliberately uneven -- the inverter and BMS chatter
   far faster than GPS on a real car, and a simulator that emits everything at
   one rate would not exercise batching realistically. */

typedef struct
{
  uint16_t id;
  uint8_t  dlc;
} sim_msg_t;

static const sim_msg_t msgs_can1[] = {          /* inverter, VCU dynamics */
  {0x101U, 8U}, {0x102U, 8U}, {0x103U, 8U}, {0x104U, 8U},
  {0x152U, 7U}, {0x153U, 8U}, {0x154U, 6U},
};
static const sim_msg_t msgs_can2[] = {          /* HV BMS, charger */
  {0x050U, 6U}, {0x051U, 5U}, {0x100U, 8U}, {0x201U, 5U}, {0x202U, 8U},
};
static const sim_msg_t msgs_can3[] = {          /* LV BMS, PDU, radio */
  {0x100U, 8U}, {0x201U, 7U}, {0x202U, 8U}, {0x203U, 6U}, {0x480U, 1U},
};
static const sim_msg_t msgs_can4[] = {          /* GPS, aux sensors */
  {0x400U, 8U}, {0x401U, 8U}, {0x402U, 8U}, {0x403U, 8U},
};

/* Cumulative weights x100: CAN1 45%, CAN2 30%, CAN3 15%, CAN4 10%. */
static const uint8_t bus_cum[4] = { 45U, 75U, 90U, 100U };

/* ---- State --------------------------------------------------------------- */

static uint8_t  batch[TELEMSIM_MAX_PAYLOAD];
static uint16_t batch_len;

static uint8_t  wire[3U + TELEMSIM_MAX_PAYLOAD + 2U];

static uint32_t last_flush_ms;
static uint32_t next_record_ms;
static uint32_t start_ms;
static uint32_t rng_state = 0x12345678U;

static uint16_t seq_counter;
static bool     burst_enabled;
static bool     burst_high;
static uint32_t burst_phase_ms;

/* Deliberately NOT static: CubeIDE's Live Expressions frequently cannot
   resolve a file-scope static from another translation unit's context, and
   silently shows 0 rather than an error -- which is indistinguishable from
   code that never ran. Globals are unambiguous. */
volatile uint32_t stat_records;
volatile uint32_t stat_frames;
volatile uint32_t stat_tx_errors;

/* Increments every call, regardless of anything else. If this is moving, the
   main loop is running and HAL_GetTick() is being read; if it is stuck at 0,
   execution never reaches the loop. That distinction is the whole diagnosis. */
volatile uint32_t sim_poll_count;
volatile uint32_t sim_last_tick;

/* -------------------------------------------------------------------------- */

/* xorshift32. Not cryptographic, but repeatable and costs nothing -- the point
   is varied payload bytes, not randomness. */
static uint32_t rnd(void)
{
  rng_state ^= rng_state << 13;
  rng_state ^= rng_state >> 17;
  rng_state ^= rng_state << 5;
  return rng_state;
}

static uint16_t crc16(const uint8_t *data, uint32_t len)
{
  uint16_t crc = CRC_INIT;

  for (uint32_t i = 0U; i < len; i++)
  {
    crc ^= (uint16_t)data[i] << 8;

    for (uint8_t b = 0U; b < 8U; b++)
    {
      crc = (crc & 0x8000U) ? (uint16_t)((crc << 1) ^ CRC_POLY)
                            : (uint16_t)(crc << 1);
    }
  }
  return crc;
}

static uint8_t dlc_to_code(uint8_t n)
{
  for (uint8_t code = 0U; code < 16U; code++)
  {
    if (dlc_table[code] >= n)
    {
      return code;
    }
  }
  return 8U;
}

/**
  * @brief Build one CANLOG record into buf. Returns its length.
  */
static uint16_t make_record(uint8_t *buf)
{
  uint8_t pick = (uint8_t)(rnd() % 100U);
  uint8_t channel = 0U;

  while ((channel < 3U) && (pick >= bus_cum[channel]))
  {
    channel++;
  }

  const sim_msg_t *pool;
  uint8_t pool_len;

  switch (channel)
  {
    case 0U:  pool = msgs_can1; pool_len = sizeof(msgs_can1) / sizeof(msgs_can1[0]); break;
    case 1U:  pool = msgs_can2; pool_len = sizeof(msgs_can2) / sizeof(msgs_can2[0]); break;
    case 2U:  pool = msgs_can3; pool_len = sizeof(msgs_can3) / sizeof(msgs_can3[0]); break;
    default:  pool = msgs_can4; pool_len = sizeof(msgs_can4) / sizeof(msgs_can4[0]); break;
  }

  const sim_msg_t *m = &pool[rnd() % pool_len];

  uint8_t code   = dlc_to_code(m->dlc);
  uint8_t padded = dlc_table[code];

  uint16_t n = 0U;

  /* dir bit stays 0: everything here is "received by the VCU". */
  buf[n++] = (uint8_t)((code & 0x0FU) | ((channel & 0x03U) << 5));

  /* First two bytes carry a sequence counter so the far end can spot gaps;
     the rest is filler, since nothing downstream decodes it during bring-up. */
  seq_counter++;
  for (uint8_t i = 0U; i < padded; i++)
  {
    if (i == 0U)      { buf[n++] = (uint8_t)(seq_counter & 0xFFU); }
    else if (i == 1U) { buf[n++] = (uint8_t)(seq_counter >> 8); }
    else              { buf[n++] = (uint8_t)(rnd() & 0xFFU); }
  }

  /* Standard 11-bit ID, little-endian. */
  buf[n++] = (uint8_t)(m->id & 0xFFU);
  buf[n++] = (uint8_t)((m->id >> 8) & 0xFFU);

  uint32_t t = HAL_GetTick() - start_ms;
  buf[n++] = (uint8_t)(t & 0xFFU);
  buf[n++] = (uint8_t)((t >> 8) & 0xFFU);
  buf[n++] = (uint8_t)((t >> 16) & 0xFFU);
  buf[n++] = (uint8_t)((t >> 24) & 0xFFU);

  return n;
}

static void send_batch(void)
{
  if (batch_len == 0U)
  {
    return;
  }

  wire[0] = SYNC0;
  wire[1] = SYNC1;
  wire[2] = (uint8_t)batch_len;
  memcpy(&wire[3], batch, batch_len);

  /* CRC covers the length byte and the payload, not the sync bytes. */
  uint16_t crc = crc16(&wire[2], 1U + batch_len);
  wire[3U + batch_len]      = (uint8_t)(crc & 0xFFU);
  wire[3U + batch_len + 1U] = (uint8_t)(crc >> 8);

  uint16_t total = (uint16_t)(3U + batch_len + 2U);

  /* Blocking is fine here: even at 115200 a 256-byte frame is 22 ms, and at
     200 records/s this fires about 50 times a second with short frames. A test
     fixture does not need DMA, and blocking keeps the failure modes obvious. */
#if TELEMSIM_USE_BITBANG
  BbDiff_Send(wire, total);
  stat_frames++;
#else
  if (HAL_UART_Transmit(&huart2, wire, total, 50U) != HAL_OK)
  {
    stat_tx_errors++;
  }
  else
  {
    stat_frames++;
  }
#endif

  batch_len = 0U;
}

/* -------------------------------------------------------------------------- */

void TelemetrySim_Init(void)
{
#if TELEMSIM_USE_BITBANG
  BbDiff_Init();
#endif

  batch_len      = 0U;
  start_ms       = HAL_GetTick();
  last_flush_ms  = start_ms;
  next_record_ms = start_ms;
  burst_phase_ms = start_ms;
  burst_enabled  = false;
  burst_high     = false;
}

void TelemetrySim_SetBurst(bool enable)
{
  burst_enabled  = enable;
  burst_high     = false;
  burst_phase_ms = HAL_GetTick();
}

bool TelemetrySim_GetBurst(void) { return burst_enabled; }

void TelemetrySim_Poll(void)
{
  uint32_t now = HAL_GetTick();

  sim_poll_count++;
  sim_last_tick = now;

  if (burst_enabled && ((now - burst_phase_ms) >= 1000U))
  {
    burst_high     = !burst_high;
    burst_phase_ms = now;
  }

  /* Interval between records, in milliseconds. Kept in integer maths -- at
     200/s that is one every 5 ms, and the tick is the limit of our resolution
     anyway. */
  uint32_t rate = TELEMSIM_RECORDS_PER_SEC;
  if (burst_enabled)
  {
    rate = burst_high ? (TELEMSIM_RECORDS_PER_SEC * 5U) : 5U;
  }

  uint32_t interval_us = 1000000U / (rate ? rate : 1U);

  /* Emit however many records are due since the last pass. Working in
     microseconds internally keeps rates above 1000/s from rounding to zero. */
  static uint32_t carry_us;
  static uint32_t last_now;

  if (last_now == 0U)
  {
    last_now = now;
  }

  uint32_t elapsed_us = (now - last_now) * 1000U;
  last_now = now;
  carry_us += elapsed_us;

  /* Cap the catch-up so a long stall cannot produce a flood. */
  if (carry_us > 200000U)
  {
    carry_us = 200000U;
  }

  while (carry_us >= interval_us)
  {
    uint8_t  rec[80];
    uint16_t rec_len = make_record(rec);

    /* A frame must contain only whole records -- never split one across two
       frames, or the receiver desynchronises. */
    if ((uint32_t)batch_len + rec_len > TELEMSIM_MAX_PAYLOAD)
    {
      send_batch();
      last_flush_ms = now;
    }

    memcpy(&batch[batch_len], rec, rec_len);
    batch_len = (uint16_t)(batch_len + rec_len);
    stat_records++;

    carry_us -= interval_us;
  }

  if ((batch_len > 0U) && ((now - last_flush_ms) >= TELEMSIM_FLUSH_MS))
  {
    send_batch();
    last_flush_ms = now;
  }

  (void)next_record_ms;
}

uint32_t TelemetrySim_GetRecords(void)  { return stat_records; }
uint32_t TelemetrySim_GetFrames(void)   { return stat_frames; }
uint32_t TelemetrySim_GetTxErrors(void) { return stat_tx_errors; }
