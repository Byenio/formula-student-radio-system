/**
  ******************************************************************************
  * @file    e28_radio.c
  * @brief   Radio configuration and packet transport. See e28_radio.h.
  ******************************************************************************
  */

#include "e28_radio.h"
#include "e28_port.h"
#include <string.h>

#if E28_USE_RTOS
#include "cmsis_os2.h"
#endif
#include "link_proto.h"

/* Only the car board has audio hardware. Guarding the include (not just the
   code) keeps the base station from needing audio.c at all. */
#if E28_ROLE_TRANSMITTER
#include "audio.h"
#include "ptt.h"
#else
/* The base station relays to the PC instead of having audio hardware. */
#include "usb_link.h"
#endif

/* ---- Configuration choices ------------------------------------------------
   GFSK at 1 Mb/s with 1.2 MHz bandwidth. Chosen over FLRC for the first link
   because it has the fewest parameters that can be wrong -- when a packet
   fails to arrive, the suspect list should be short. FLRC buys several dB of
   sensitivity and is the right long-term answer once this is proven; the
   switch is a packet type plus new modulation and packet params.

   At 1 Mb/s a 10 ms voice frame occupies roughly 760 us of air time, about
   7.6% duty cycle, leaving the channel overwhelmingly free for telemetry. */
#define RADIO_BITRATE_BW            E28_GFSK_BR_1_000_BW_1_2
#define RADIO_MOD_INDEX             E28_GFSK_MOD_IND_1_00
#define RADIO_MOD_SHAPING           E28_MOD_SHAPING_BT_0_5

#define RADIO_PREAMBLE              E28_PREAMBLE_32_BITS
#define RADIO_SYNCWORD_LEN          E28_SYNCWORD_LEN_5_BYTE
#define RADIO_SYNCWORD_MATCH        E28_RX_MATCH_SYNCWORD_1
#define RADIO_HEADER_TYPE           E28_PACKET_VARIABLE_LENGTH
#define RADIO_CRC_LEN               E28_CRC_2_BYTES
#define RADIO_WHITENING             E28_WHITENING_ON

#define RADIO_DEFAULT_CHANNEL       2U     /* 2452 MHz, between Wi-Fi 6 and 11 */

/* Bench testing: two boards on one desk, each with 27 dB of PA gain. At full
   power the receiver front end saturates and performance gets *worse*, which
   is a genuinely confusing failure. Raise this only for range testing. */
#define RADIO_TX_POWER_DBM          E28_TX_POWER_BENCH_DBM

/* While transmitting, pause and listen every this many frames. Zero disables
   it, which is the default: a transmitting station is deaf, so this only
   earns its 2% of uplink if something actually needs to reach the car
   mid-transmission. With mic state shown on the steering wheel there is
   nothing to send, so it stays off. Set to 50 to re-enable, and the control
   handling below is already in place. */
#ifndef RADIO_LISTEN_EVERY_FRAMES
#define RADIO_LISTEN_EVERY_FRAMES   0U
#endif

/* How long that listening pause lasts. Long enough for a packet already in
   flight to land, short enough to be inaudible. */
#ifndef RADIO_LISTEN_WINDOW_MS
#define RADIO_LISTEN_WINDOW_MS      12U
#endif

/* Five bytes, chosen for a balanced run of transitions -- a sync word that is
   mostly zeros or mostly ones correlates poorly and false-triggers on noise. */
static const uint8_t radio_syncword[5] = { 0x5AU, 0xC3U, 0x96U, 0x69U, 0xA5U };

/* ---- State ---------------------------------------------------------------- */

static uint32_t tx_count;
static uint32_t rx_count;
static uint32_t crc_error_count;
static volatile int8_t  last_rssi;
static volatile uint16_t last_irq;

static uint8_t rx_buffer[E28_MAX_PAYLOAD];
static volatile uint8_t rx_len;
static volatile bool rx_pending;

/* Bring-up visibility. dio1_count is the important one: it separates "the
   interrupt never fired" from "it fired but no packet came out", which are
   completely different faults and otherwise look identical. */
static volatile uint32_t dio1_count;
static volatile bool     config_ok;
static volatile bool     rx_armed;

/* Echo-test statistics. seq_gaps is the useful one: every gap in the
   sequence numbers is a packet that did not survive the round trip, so this
   is a direct packet-loss count for voice traffic. */
static volatile uint32_t echo_rx_count;
static volatile uint32_t seq_gaps;
static volatile uint32_t tx_timeouts;
static volatile uint32_t echo_timeouts;

#if !E28_ROLE_TRANSMITTER
static volatile uint32_t usb_forwarded;   /*!< radio -> PC                  */
static volatile uint32_t air_from_usb;    /*!< PC -> radio                  */
static volatile uint32_t usb_backpressure;/*!< PC not reading fast enough   */
#endif

/* ---- SPI helper -----------------------------------------------------------
   Buffer reads and writes bypass e28.c's command helpers because they need a
   held chip select across a header plus a bulk payload. They must still use
   TransmitReceive rather than Transmit: HAL_SPI_Transmit leaves up to three
   stale bytes in the RX FIFO, which then appear at the *start* of the next
   read -- silently corrupting every received packet. Same trap as e28.c. */

/* Longest buffer transaction: the 3-byte ReadBuffer header plus a full
   payload. Sizing these by the payload alone would silently reject a
   maximum-length packet. */
#define RADIO_SPI_MAX_FRAME   (3U + E28_MAX_PAYLOAD)

static uint8_t  radio_tx_frame[RADIO_SPI_MAX_FRAME];
static uint8_t  radio_rx_frame[RADIO_SPI_MAX_FRAME];

static bool radio_spi(const uint8_t *tx, uint8_t *rx, uint16_t len)
{
  extern SPI_HandleTypeDef E28_SPI_HANDLE;

  static uint8_t dump[RADIO_SPI_MAX_FRAME];
  static const uint8_t zeros[RADIO_SPI_MAX_FRAME] = { 0 };

  if (len > RADIO_SPI_MAX_FRAME) { return false; }

  const uint8_t *ptx = (tx != NULL) ? tx : zeros;
  uint8_t       *prx = (rx != NULL) ? rx : dump;

  return (HAL_SPI_TransmitReceive(&E28_SPI_HANDLE, (uint8_t *)ptx, prx,
                                  len, 200U) == HAL_OK);
}

/* ---- Helpers -------------------------------------------------------------- */

/**
  * @brief Steer the module's PA/LNA switch.
  *
  * The E28 wraps the SX1281 in an external amplifier; TX_EN and RX_EN select
  * which path is live. Both low is the safe idle state -- leaving TX_EN
  * asserted with no transmission wastes current and, worse, leaving both
  * asserted at once is a good way to damage the front end.
  */
static void rf_switch_idle(void)
{
  HAL_GPIO_WritePin(E28_TX_EN_GPIO_Port, E28_TX_EN_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(E28_RX_EN_GPIO_Port, E28_RX_EN_Pin, GPIO_PIN_RESET);
}

static void rf_switch_tx(void)
{
  HAL_GPIO_WritePin(E28_RX_EN_GPIO_Port, E28_RX_EN_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(E28_TX_EN_GPIO_Port, E28_TX_EN_Pin, GPIO_PIN_SET);
}

static void rf_switch_rx(void)
{
  HAL_GPIO_WritePin(E28_TX_EN_GPIO_Port, E28_TX_EN_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(E28_RX_EN_GPIO_Port, E28_RX_EN_Pin, GPIO_PIN_SET);
}

static uint32_t channel_to_hz(uint8_t channel)
{
  switch (channel)
  {
    case 0U:  return E28_CHANNEL_0_HZ;
    case 1U:  return E28_CHANNEL_1_HZ;
    case 2U:  return E28_CHANNEL_2_HZ;
    default:  return E28_CHANNEL_3_HZ;
  }
}

static bool radio_set_frequency(uint32_t hz)
{
  /* The chip wants frequency as a 24-bit multiple of its 52 MHz reference
     divided by 2^18 (about 198.36 Hz per step). Done in 64-bit integer maths
     rather than the reference driver's double, so there is no float
     dependency and no rounding surprise. */
  uint32_t reg = (uint32_t)(((uint64_t)hz << 18) / 52000000ULL);

  uint8_t params[3];
  params[0] = (uint8_t)((reg >> 16) & 0xFFU);
  params[1] = (uint8_t)((reg >> 8) & 0xFFU);
  params[2] = (uint8_t)(reg & 0xFFU);

  return E28_WriteCommand(E28_CMD_SET_RFFREQUENCY, params, 3U);
}

static bool radio_clear_irq(uint16_t mask)
{
  uint8_t params[2];
  params[0] = (uint8_t)((mask >> 8) & 0xFFU);
  params[1] = (uint8_t)(mask & 0xFFU);
  return E28_WriteCommand(E28_CMD_CLR_IRQSTATUS, params, 2U);
}

static uint16_t radio_get_irq(void)
{
  uint8_t result[2] = { 0U, 0U };

  if (!E28_ReadCommand(E28_CMD_GET_IRQSTATUS, result, 2U))
  {
    return 0U;
  }
  return (uint16_t)(((uint16_t)result[0] << 8) | result[1]);
}

/* ---- Configuration -------------------------------------------------------- */

bool E28_Radio_Config(void)
{
  uint8_t params[8];

  rf_switch_idle();

  /* Standby first: most configuration commands are only accepted here. */
  params[0] = E28_STDBY_RC;
  if (!E28_WriteCommand(E28_CMD_SET_STANDBY, params, 1U)) { return false; }

  /* LDO rather than DC-DC. The DC-DC needs an external inductor that the
     module may or may not populate; LDO always works, at the cost of roughly
     double the current. Revisit if power budget ever matters. */
  params[0] = E28_REG_MODE_LDO;
  if (!E28_WriteCommand(E28_CMD_SET_REGULATORMODE, params, 1U)) { return false; }

  /* Packet type MUST come first: it decides how the chip interprets every
     modulation and packet parameter byte that follows, and it also remaps the
     sync word registers. Setting it later silently invalidates everything. */
  params[0] = E28_PACKET_TYPE_GFSK;
  if (!E28_WriteCommand(E28_CMD_SET_PACKETTYPE, params, 1U)) { return false; }

  if (!radio_set_frequency(channel_to_hz(RADIO_DEFAULT_CHANNEL))) { return false; }

  /* Both buffers start at offset 0. The 256-byte FIFO is shared between Tx
     and Rx, but never used for both at once on a half-duplex link. */
  params[0] = 0x00U;   /* Tx base */
  params[1] = 0x00U;   /* Rx base */
  if (!E28_WriteCommand(E28_CMD_SET_BUFFERBASEADDR, params, 2U)) { return false; }

  params[0] = RADIO_BITRATE_BW;
  params[1] = RADIO_MOD_INDEX;
  params[2] = RADIO_MOD_SHAPING;
  if (!E28_WriteCommand(E28_CMD_SET_MODULATIONPARAMS, params, 3U)) { return false; }

  params[0] = RADIO_PREAMBLE;
  params[1] = RADIO_SYNCWORD_LEN;
  params[2] = RADIO_SYNCWORD_MATCH;
  params[3] = RADIO_HEADER_TYPE;
  params[4] = E28_MAX_PAYLOAD;    /* ignored in variable-length mode */
  params[5] = RADIO_CRC_LEN;
  params[6] = RADIO_WHITENING;
  if (!E28_WriteCommand(E28_CMD_SET_PACKETPARAMS, params, 7U)) { return false; }

  /* Written after SetPacketType, because the sync word register block moves
     depending on packet type. Note the self-test scribbles a test pattern
     over this same register -- this call is what puts it right. */
  if (!E28_WriteRegister(E28_REG_SYNCWORD1, radio_syncword, sizeof(radio_syncword)))
  {
    return false;
  }

  if (!E28_SetTxPower(RADIO_TX_POWER_DBM, E28_RAMP_20_US)) { return false; }

  /* Enable the interrupts we care about globally, but route only the ones
     that need a fast response to DIO1. CRC errors and timeouts are worth
     knowing about, so they come through too -- a link that is receiving but
     failing CRC looks identical to a dead link unless you count them. */
  {
    const uint16_t irq_mask  = E28_IRQ_TX_DONE | E28_IRQ_RX_DONE |
                               E28_IRQ_CRC_ERROR | E28_IRQ_RX_TX_TIMEOUT;
    const uint16_t dio1_mask = irq_mask;

    params[0] = (uint8_t)((irq_mask >> 8) & 0xFFU);
    params[1] = (uint8_t)(irq_mask & 0xFFU);
    params[2] = (uint8_t)((dio1_mask >> 8) & 0xFFU);
    params[3] = (uint8_t)(dio1_mask & 0xFFU);
    params[4] = 0x00U;  params[5] = 0x00U;   /* DIO2 unused */
    params[6] = 0x00U;  params[7] = 0x00U;   /* DIO3 unused */

    if (!E28_WriteCommand(E28_CMD_SET_DIOIRQPARAMS, params, 8U)) { return false; }
  }

  (void)radio_clear_irq(E28_IRQ_ALL);

  return E28_WaitReady(100U);
}

bool E28_Radio_SetChannel(uint8_t channel)
{
  return radio_set_frequency(channel_to_hz(channel % E28_CHANNEL_COUNT));
}

/* ---- Transmit ------------------------------------------------------------- */

bool E28_Radio_Send(const uint8_t *data, uint8_t len)
{
  if (data == NULL || len == 0U) { return false; }

  (void)radio_clear_irq(E28_IRQ_ALL);

  if (!E28_WaitReady(100U)) { return false; }

  /* Opcode, offset and payload assembled into ONE frame and sent in ONE
     transfer. Splitting this across two HAL calls with CS held low makes the
     peripheral restart mid-transaction, which emits a stray clock edge -- the
     far end then receives every byte shifted left by one bit. */
  radio_tx_frame[0] = E28_CMD_WRITE_BUFFER;
  radio_tx_frame[1] = 0x00U;               /* offset into the radio FIFO */
  memcpy(&radio_tx_frame[2], data, len);

  HAL_GPIO_WritePin(E28_CS_GPIO_Port, E28_CS_Pin, GPIO_PIN_RESET);
  {
    bool ok = radio_spi(radio_tx_frame, NULL, (uint16_t)(2U + len));

    HAL_GPIO_WritePin(E28_CS_GPIO_Port, E28_CS_Pin, GPIO_PIN_SET);
    if (!ok) { return false; }
  }

  /* Variable-length mode still needs the length declared in packet params. */
  {
    uint8_t pp[7];
    pp[0] = RADIO_PREAMBLE;
    pp[1] = RADIO_SYNCWORD_LEN;
    pp[2] = RADIO_SYNCWORD_MATCH;
    pp[3] = RADIO_HEADER_TYPE;
    pp[4] = len;
    pp[5] = RADIO_CRC_LEN;
    pp[6] = RADIO_WHITENING;
    if (!E28_WriteCommand(E28_CMD_SET_PACKETPARAMS, pp, 7U)) { return false; }
  }

  rf_switch_tx();

  /* Single-shot: period count 0 means no timeout, and the radio drops back to
     standby once the packet is out. */
  {
    uint8_t tx[3];
    tx[0] = E28_TICK_1000_US;
    tx[1] = 0x00U;
    tx[2] = 0x00U;
    if (!E28_WriteCommand(E28_CMD_SET_TX, tx, 3U))
    {
      rf_switch_idle();
      return false;
    }
  }

  tx_count++;
  return true;
}

/* ---- Receive -------------------------------------------------------------- */

bool E28_Radio_StartRx(void)
{
  uint8_t rx[3];

  (void)radio_clear_irq(E28_IRQ_ALL);

  rf_switch_rx();

  /* 0xFFFF means continuous: stay in Rx even after a packet arrives, so we
     never miss the next one while servicing the last. */
  rx[0] = E28_TICK_1000_US;
  rx[1] = 0xFFU;
  rx[2] = 0xFFU;

  if (!E28_WriteCommand(E28_CMD_SET_RX, rx, 3U))
  {
    rf_switch_idle();
    return false;
  }
  return true;
}

/**
  * @brief Pull a completed packet out of the radio FIFO.
  */
static void radio_read_packet(void)
{
  uint8_t status[2] = { 0U, 0U };
  uint8_t len;
  uint8_t offset;

  if (!E28_ReadCommand(E28_CMD_GET_RXBUFFERSTATUS, status, 2U)) { return; }

  len    = status[0];
  offset = status[1];

  if (len == 0U || len > E28_MAX_PAYLOAD) { return; }

  /* ReadBuffer: opcode, offset, one NOP, then the data -- again as a single
     frame in a single transfer, for the same reason as the write path. */
  {
    if (!E28_WaitReady(100U)) { return; }

    memset(radio_tx_frame, 0, (size_t)(3U + len));
    radio_tx_frame[0] = E28_CMD_READ_BUFFER;
    radio_tx_frame[1] = offset;
    radio_tx_frame[2] = 0x00U;             /* NOP before data clocks out */

    HAL_GPIO_WritePin(E28_CS_GPIO_Port, E28_CS_Pin, GPIO_PIN_RESET);

    bool ok = radio_spi(radio_tx_frame, radio_rx_frame, (uint16_t)(3U + len));

    HAL_GPIO_WritePin(E28_CS_GPIO_Port, E28_CS_Pin, GPIO_PIN_SET);

    if (ok)
    {
      memcpy(rx_buffer, &radio_rx_frame[3], len);
      rx_len     = len;
      rx_pending = true;
      rx_count++;
    }
  }

  /* GFSK reports sync RSSI in byte 1, not byte 0 -- byte 0 is where LoRa puts
     its packet RSSI. Encoding is -value/2 dBm either way. Diagnostic only, so
     a wrong index here costs nothing but a misleading number. */
  {
    uint8_t pkt_status[5] = { 0U };
    if (E28_ReadCommand(E28_CMD_GET_PACKETSTATUS, pkt_status, 5U))
    {
      last_rssi = (int8_t)-((int16_t)pkt_status[1] / 2);
    }
  }
}

bool E28_Radio_GetRxPacket(uint8_t *buf, uint8_t *len, uint8_t max_len)
{
  if (!rx_pending || buf == NULL || len == NULL) { return false; }

  uint8_t n = (rx_len < max_len) ? rx_len : max_len;

  for (uint8_t i = 0U; i < n; i++)
  {
    buf[i] = rx_buffer[i];
  }

  *len       = n;
  rx_pending = false;
  return true;
}

/* ---- Counters ------------------------------------------------------------- */

uint32_t E28_Radio_GetTxCount(void)        { return tx_count; }
uint32_t E28_Radio_GetRxCount(void)        { return rx_count; }
uint32_t E28_Radio_GetCrcErrorCount(void)  { return crc_error_count; }
int8_t   E28_Radio_GetLastRssi(void)       { return last_rssi; }
uint16_t E28_Radio_GetLastIrq(void)        { return last_irq; }

/* ---- DIO1 interrupt ------------------------------------------------------- */

void E28_Dio1Callback(void)
{
  dio1_count++;
  E28_Port_SignalDio1();
}

uint32_t E28_Radio_GetDio1Count(void)  { return dio1_count; }
uint32_t E28_Radio_GetEchoRxCount(void){ return echo_rx_count; }
uint32_t E28_Radio_GetSeqGaps(void)    { return seq_gaps; }
bool     E28_Radio_IsConfigured(void)  { return config_ok; }
bool     E28_Radio_IsRxArmed(void)     { return rx_armed; }

/* ---- Radio driving -------------------------------------------------------

   Two shapes, because the two boards have different constraints.

   Car board (RTOS): a blocking loop paced by the audio task. Capture ->
   encode -> transmit -> listen for the echo -> decode -> playback. Blocking is
   fine because FreeRTOS keeps the audio task running underneath it.

   Base station (bare metal): a non-blocking state machine polled from the main
   loop, because there is no scheduler to run USB while we wait. Receive ->
   retransmit. Blocking here would stall the USB stack.
   -------------------------------------------------------------------------- */

#if E28_USE_RTOS

/* Blocking send: hand the frame over and wait for TxDone. */
static bool radio_send_blocking(const uint8_t *packet, uint8_t len,
                                uint32_t timeout_ms)
{
  /* Clear here rather than inside the wait: we are about to start a
     transmission and want its TxDone, not a stale edge from before. */
  E28_Port_ClearDio1();

  if (!E28_Radio_Send(packet, len))
  {
    return false;
  }

  if (!E28_Port_WaitDio1(timeout_ms))
  {
    tx_timeouts++;
    rf_switch_idle();
    return false;
  }

  last_irq = radio_get_irq();
  (void)radio_clear_irq(E28_IRQ_ALL);
  rf_switch_idle();

  return ((last_irq & E28_IRQ_TX_DONE) != 0U);
}

#if E28_ROLE_TRANSMITTER
/**
  * @brief Deal with one packet the car board received.
  *
  * Audio goes to the speaker; control messages act immediately. Keeping this
  * in one place means the transmit-pause window and the idle listening path
  * behave identically -- an override must work the same either way.
  */
static void handle_rx_packet(uint8_t *buf, uint8_t len)
{
  if ((len < LINK_HEADER_LEN) ||
      (LINK_GET_VERSION(buf[0]) != LINK_PROTO_VERSION))
  {
    return;
  }

  uint8_t type    = LINK_GET_TYPE(buf[0]);
  uint8_t payload_len = buf[2];

  if (type == LINK_PKT_AUDIO)
  {
    (void)Audio_PutDecodedFrame(&buf[LINK_HEADER_LEN]);
    echo_rx_count++;
  }
  else if ((type == LINK_PKT_CONTROL) && (payload_len >= 2U))
  {
    if (buf[LINK_HEADER_LEN] == (uint8_t)LINK_CTRL_PTT_STATE)
    {
      /* The pit wall can key or unkey the driver's mic. This is the whole
         reason for the transmit-pause window above. */
      Ptt_SetRemote(buf[LINK_HEADER_LEN + 1U] != 0U);
    }
  }
}

/**
  * @brief Listen briefly, acting on anything that arrives.
  */
#if RADIO_LISTEN_EVERY_FRAMES > 0U
static void radio_listen_window(uint32_t window_ms)
{
  uint8_t buf[E28_MAX_PAYLOAD];
  uint8_t len = 0U;

  rf_switch_idle();
  rx_armed = E28_Radio_StartRx();

  if (E28_Port_WaitDio1(window_ms))
  {
    last_irq = radio_get_irq();

    if (last_irq & E28_IRQ_RX_DONE)
    {
      radio_read_packet();

      if (E28_Radio_GetRxPacket(buf, &len, sizeof(buf)))
      {
        handle_rx_packet(buf, len);
      }
    }
    if (last_irq & E28_IRQ_CRC_ERROR)
    {
      crc_error_count++;
    }

    (void)radio_clear_irq(E28_IRQ_ALL);
  }

  rf_switch_idle();
}
#endif
#endif

void E28_Radio_Loop(void)
{
  E28_Port_Bind();

  if (!E28_Radio_Config())
  {
    config_ok = false;
    for (;;) { E28_Port_Delay(1000); }
  }
  config_ok = true;

#if E28_ROLE_TRANSMITTER
  {
    uint8_t packet[LINK_HEADER_LEN + AUDIO_ENCODED_LEN];
    uint8_t rx_packet[E28_MAX_PAYLOAD];
    uint8_t rx_packet_len = 0U;
    uint8_t discard[AUDIO_ENCODED_LEN];
    uint8_t seq = 0U;
    uint8_t expected_seq = 0U;
    bool    have_expected = false;
    bool    listening = false;

    for (;;)
    {
      if (Ptt_IsTransmitting())
      {
        /* ---- Mic open: talk, do not listen ----
           A half-duplex radio on one antenna cannot do both, and trying to
           interleave them cost roughly a third of all packets in testing.
           Keying makes that contention disappear instead. */
        if (listening)
        {
          rf_switch_idle();
          listening = false;
        }

        if (!Audio_GetEncodedFrame(&packet[LINK_HEADER_LEN], 20U))
        {
          continue;   /* no frame ready yet; the DMA paces us */
        }

        packet[0] = LINK_VER_TYPE(LINK_PROTO_VERSION, LINK_PKT_AUDIO);
        packet[1] = seq;
        packet[2] = AUDIO_ENCODED_LEN;
        packet[3] = Ptt_IsOpen() ? LINK_FLAG_PTT_ACTIVE : 0x00U;

        (void)radio_send_blocking(packet, sizeof(packet), 50U);
        seq++;

#if RADIO_LISTEN_EVERY_FRAMES > 0U
        /* Periodically give up one frame to listen, so a control message can
           always get through even mid-transmission. */
        if ((seq % RADIO_LISTEN_EVERY_FRAMES) == 0U)
        {
          radio_listen_window(RADIO_LISTEN_WINDOW_MS);
          listening = false;      /* the window left the radio idle */
        }
#endif
      }
      else
      {
        /* ---- Mic closed: listen ----
           Captured audio is discarded rather than queued. Sending it later
           would play stale speech at the far end, and letting the queue back
           up would just make the first frame after keying be 20 ms old. */
        while (Audio_GetEncodedFrame(discard, 0U))
        {
          /* drop */
        }

        if (!listening)
        {
          rx_armed  = E28_Radio_StartRx();
          listening = true;
        }

        /* Short wait so a key press is acted on within one frame period.
           Loops rather than handling a single packet: with no clear-on-entry
           the flag may already be set for a packet that landed while we were
           busy, and a burst must not be truncated. */
        while (E28_Port_WaitDio1(20U))
        {
          last_irq = radio_get_irq();

          if (last_irq & E28_IRQ_RX_DONE)
          {
            radio_read_packet();

            if (E28_Radio_GetRxPacket(rx_packet, &rx_packet_len, sizeof(rx_packet)))
            {
              if ((rx_packet_len >= LINK_HEADER_LEN) &&
                  (LINK_GET_TYPE(rx_packet[0]) == LINK_PKT_AUDIO))
              {
                if (have_expected && (rx_packet[1] != expected_seq))
                {
                  seq_gaps++;
                }
                expected_seq  = (uint8_t)(rx_packet[1] + 1U);
                have_expected = true;
              }

              handle_rx_packet(rx_packet, rx_packet_len);
            }
          }
          if (last_irq & E28_IRQ_CRC_ERROR)
          {
            crc_error_count++;
          }

          (void)radio_clear_irq(E28_IRQ_ALL);

          /* Leave promptly if the driver keyed the mic mid-burst. */
          if (Ptt_IsTransmitting())
          {
            break;
          }
        }
      }
    }
  }
#else
  /* An RTOS build in the receive role is not a configuration we use; the base
     station is bare metal. Park rather than pretend. */
  for (;;) { E28_Port_Delay(1000); }
#endif
}

#else   /* ---------------- bare metal relay ---------------- */

/* Set to 1 to make the base station bounce packets straight back over the
   air, ignoring USB. Useful for isolating a radio problem from a PC problem:
   with this on, the car board hears itself with the PC out of the picture. */
#ifndef RELAY_ECHO_TO_AIR
#define RELAY_ECHO_TO_AIR       0
#endif

/* Status report to the PC, so the app can show link health even when nobody
   is talking. Also gives the USB path something to carry during bring-up. */
#define RELAY_KEEPALIVE_MS      500U

typedef enum
{
  RELAY_LISTENING = 0,   /*!< armed for receive                     */
  RELAY_SENDING          /*!< transmitting, waiting for TxDone      */
} RelayState_t;

static RelayState_t relay_state;
static uint32_t     relay_last_activity;
static uint32_t     relay_last_keepalive;
static uint8_t      relay_packet[E28_MAX_PAYLOAD];
static uint8_t      relay_packet_len;

bool E28_Radio_RelayInit(void)
{
  E28_Port_Bind();

  config_ok = E28_Radio_Config();
  if (!config_ok)
  {
    return false;
  }

  rx_armed             = E28_Radio_StartRx();
  relay_state          = RELAY_LISTENING;
  relay_last_activity  = E28_Port_Now();
  relay_last_keepalive = E28_Port_Now();

  return rx_armed;
}

/**
  * @brief Tell the PC how the radio link is doing.
  *
  * Sent on a timer rather than on demand so the app can distinguish "the link
  * is quiet" from "the relay has died" -- those look identical if the only
  * traffic is voice.
  */
static void relay_send_keepalive(void)
{
  uint8_t pkt[LINK_HEADER_LEN + 8U];

  pkt[0] = LINK_VER_TYPE(LINK_PROTO_VERSION, LINK_PKT_CONTROL);
  pkt[1] = 0U;                       /* control messages are not sequenced */
  pkt[2] = 8U;
  pkt[3] = 0U;

  pkt[LINK_HEADER_LEN + 0U] = (uint8_t)LINK_CTRL_LINK_STATS;
  pkt[LINK_HEADER_LEN + 1U] = (uint8_t)last_rssi;
  pkt[LINK_HEADER_LEN + 2U] = (uint8_t)(rx_count >> 8);
  pkt[LINK_HEADER_LEN + 3U] = (uint8_t)(rx_count);
  pkt[LINK_HEADER_LEN + 4U] = (uint8_t)(crc_error_count >> 8);
  pkt[LINK_HEADER_LEN + 5U] = (uint8_t)(crc_error_count);
  pkt[LINK_HEADER_LEN + 6U] = (uint8_t)(usb_backpressure);
  pkt[LINK_HEADER_LEN + 7U] = rx_armed ? 1U : 0U;

  (void)UsbLink_SendPacket(pkt, (uint8_t)sizeof(pkt));
}

void E28_Radio_RelayPoll(void)
{
  if (!config_ok)
  {
    return;
  }

  /* ---- Radio events ---- */
  if (E28_Port_Dio1Pending())
  {
    last_irq = radio_get_irq();
    (void)radio_clear_irq(E28_IRQ_ALL);
    relay_last_activity = E28_Port_Now();

    switch (relay_state)
    {
      case RELAY_LISTENING:
        if (last_irq & E28_IRQ_RX_DONE)
        {
          radio_read_packet();

          if (E28_Radio_GetRxPacket(relay_packet, &relay_packet_len,
                                    sizeof(relay_packet)) &&
              (relay_packet_len > 0U))
          {
            echo_rx_count++;

            /* Forward to the PC verbatim. The relay never inspects a payload
               -- its job is to move packets, not understand them. */
            if (UsbLink_SendPacket(relay_packet, relay_packet_len))
            {
              usb_forwarded++;
            }
            else
            {
              usb_backpressure++;
            }

#if RELAY_ECHO_TO_AIR
            if (E28_Radio_Send(relay_packet, relay_packet_len))
            {
              relay_state = RELAY_SENDING;
              break;
            }
#endif
          }

          rx_armed = E28_Radio_StartRx();
        }
        else
        {
          if (last_irq & E28_IRQ_CRC_ERROR)
          {
            crc_error_count++;
          }
          rx_armed = E28_Radio_StartRx();
        }
        break;

      case RELAY_SENDING:
      default:
        /* TxDone, or a timeout the radio reported -- either way, go back to
           listening. Dropping one packet beats getting stuck. */
        rf_switch_idle();
        rx_armed    = E28_Radio_StartRx();
        relay_state = RELAY_LISTENING;
        break;
    }

    return;   /* one event per poll keeps USB serviced between them */
  }

  /* ---- Downlink: anything the PC sent goes out over the air ---- */
  if (relay_state == RELAY_LISTENING)
  {
    uint8_t  down[E28_MAX_PAYLOAD];
    uint8_t  down_len = 0U;

    if (UsbLink_GetPacket(down, &down_len, sizeof(down)) && (down_len > 0U))
    {
      if (E28_Radio_Send(down, down_len))
      {
        air_from_usb++;
        relay_state         = RELAY_SENDING;
        relay_last_activity = E28_Port_Now();
      }
      else
      {
        rx_armed = E28_Radio_StartRx();
      }
      return;
    }
  }

  /* ---- Housekeeping ---- */
  if ((E28_Port_Now() - relay_last_keepalive) >= RELAY_KEEPALIVE_MS)
  {
    relay_last_keepalive = E28_Port_Now();
    relay_send_keepalive();
  }

  if ((E28_Port_Now() - relay_last_activity) > 2000U)
  {
    /* Nothing for two seconds. Read the radio's own flags in case an interrupt
       was never delivered, then re-arm. A bare-metal state machine has no
       supervisor, so it has to check on itself. */
    last_irq = radio_get_irq();
    (void)radio_clear_irq(E28_IRQ_ALL);

    rf_switch_idle();
    rx_armed            = E28_Radio_StartRx();
    relay_state         = RELAY_LISTENING;
    relay_last_activity = E28_Port_Now();
  }
}

uint32_t E28_Radio_GetUsbForwarded(void)   { return usb_forwarded; }
uint32_t E28_Radio_GetAirFromUsb(void)     { return air_from_usb; }
uint32_t E28_Radio_GetUsbBackpressure(void){ return usb_backpressure; }

#endif  /* E28_USE_RTOS */
