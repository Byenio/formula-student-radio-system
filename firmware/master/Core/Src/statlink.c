/**
  ******************************************************************************
  * @file    statlink.c
  * @brief   Framed statistics over a debug UART. See statlink.h.
  *
  * IDENTICAL ON BOTH BOARDS. The only per-board code is the hardware block
  * immediately below.
  *
  * Transmit path
  * -------------
  * Frames go into a byte ring. The USART interrupt drains it into the
  * hardware TX FIFO (8 deep on both chips) and switches itself off when the
  * ring is empty. With the FIFO, one interrupt moves up to 8 bytes, so a
  * 200-byte frame costs ~25 short interrupts at 1 Mbaud -- well under 0.1 %
  * of either CPU, and that cost is itself visible in the stats.self section.
  *
  * The interrupt runs at the lowest NVIC priority on both boards, so it can
  * never delay the audio DMA, the radio DIO1 line, RS-485 or USB.
  *
  * Why the ring needs no lock: exactly one producer (the stats context) and
  * one consumer (the ISR). The producer writes bytes, then publishes them by
  * moving head; the consumer only ever moves tail. Each index has a single
  * writer, so no update can be lost. The one shared control bit is the TX
  * interrupt enable: the producer sets it AFTER publishing, the ISR clears it
  * only when it finds the ring empty. Because the ISR preempts the producer
  * and never the reverse, the worst interleaving is an ISR that runs once,
  * finds nothing, and switches itself off again -- never data stranded in
  * the ring with the interrupt disabled.
  ******************************************************************************
  */

#include "statlink.h"

#if FW_STATS_ENABLE

/* ==========================================================================
   Board hardware
   ========================================================================== */

#if defined(FW_HOST_TEST)

  #define STATLINK_DMB()         ((void)0)

#elif defined(STM32G491xx)          /* master */

  #include "main.h"
  #define STATLINK_USART         USART2
  #define STATLINK_IRQn          USART2_IRQn
  #define STATLINK_IRQHandler    USART2_IRQHandler
  #define STATLINK_PERIPHCLK     RCC_PERIPHCLK_USART2
  #define STATLINK_CLK_ENABLE()  __HAL_RCC_USART2_CLK_ENABLE()
  #define STATLINK_TX_PORT       GPIOA
  #define STATLINK_TX_PIN        GPIO_PIN_2
  #define STATLINK_TX_AF         GPIO_AF7_USART2
  #define STATLINK_GPIO_ENABLE() __HAL_RCC_GPIOA_CLK_ENABLE()
  #define STATLINK_DMB()         __DMB()

#elif defined(STM32U375xx)          /* slave */

  #include "main.h"
  #define STATLINK_USART         USART1
  #define STATLINK_IRQn          USART1_IRQn
  #define STATLINK_IRQHandler    USART1_IRQHandler
  #define STATLINK_PERIPHCLK     RCC_PERIPHCLK_USART1
  #define STATLINK_CLK_ENABLE()  __HAL_RCC_USART1_CLK_ENABLE()
  #define STATLINK_TX_PORT       GPIOA
  #define STATLINK_TX_PIN        GPIO_PIN_9
  #define STATLINK_TX_AF         GPIO_AF7_USART1
  #define STATLINK_GPIO_ENABLE() __HAL_RCC_GPIOA_CLK_ENABLE()
  #define STATLINK_DMB()         __DMB()

#else
  #error "statlink.c: unknown board -- add its USART to the hardware block"
#endif

/* ==========================================================================
   State
   ========================================================================== */

#define RING_MASK  (STATLINK_RING_SIZE - 1U)

#if (STATLINK_RING_SIZE & RING_MASK) != 0U
#error "STATLINK_RING_SIZE must be a power of two"
#endif

static uint8_t           ring[STATLINK_RING_SIZE];
static volatile uint32_t head;            /* written by producer only       */
static volatile uint32_t tail;            /* written by the ISR only        */
static volatile uint32_t bytes_sent;
static uint32_t          frames_dropped;

/* ==========================================================================
   CRC-16/CCITT-FALSE, byte at a time
   ==========================================================================
   Bitwise rather than table-driven: ~40 cycles a byte, so a 200-byte frame
   costs ~8000 cycles (47 us at 170 MHz) every 100 ms. A 512-byte table would
   buy back about 40 us per report, not worth the flash. */

static uint16_t crc16_update(uint16_t crc, uint8_t byte)
{
  crc ^= (uint16_t)((uint16_t)byte << 8);
  for (uint32_t bit = 0U; bit < 8U; bit++)
  {
    crc = ((crc & 0x8000U) != 0U) ? (uint16_t)((crc << 1) ^ 0x1021U)
                                  : (uint16_t)(crc << 1);
  }
  return crc;
}

/* ==========================================================================
   Hardware
   ========================================================================== */

#if !defined(FW_HOST_TEST)

static void statlink_kick(void)
{
  /* Producer side of the hand-off: set the enable after head has moved. */
  STATLINK_USART->CR1 |= USART_CR1_TXEIE_TXFNFIE;
}

void StatLink_Init(void)
{
  GPIO_InitTypeDef gpio = {0};

  STATLINK_GPIO_ENABLE();
  STATLINK_CLK_ENABLE();

  /* TX only. The RX pin is left as CubeMX set it: nothing is received yet,
     and an unused input left floating on a debug header is just a noise
     antenna. A later command channel (start/stop a test run from the PC)
     would claim it here. */
  gpio.Pin       = STATLINK_TX_PIN;
  gpio.Mode      = GPIO_MODE_AF_PP;
  gpio.Pull      = GPIO_NOPULL;
  gpio.Speed     = GPIO_SPEED_FREQ_MEDIUM;
  gpio.Alternate = STATLINK_TX_AF;
  HAL_GPIO_Init(STATLINK_TX_PORT, &gpio);

  /* Configure with the USART disabled: FIFOEN and BRR are only writable
     while UE = 0. On the slave this also overrides whatever MX_USART1 set. */
  STATLINK_USART->CR1   = 0U;
  STATLINK_USART->CR2   = 0U;              /* 1 stop bit                   */
  STATLINK_USART->CR3   = 0U;              /* no flow control              */
  STATLINK_USART->PRESC = 0U;              /* kernel clock / 1             */

  {
    const uint32_t clk = HAL_RCCEx_GetPeriphCLKFreq(STATLINK_PERIPHCLK);
    /* 16x oversampling: BRR = clk / baud, rounded to nearest. */
    STATLINK_USART->BRR = (clk + (STATLINK_BAUD / 2U)) / STATLINK_BAUD;
  }

  STATLINK_USART->CR1 = USART_CR1_FIFOEN | USART_CR1_TE;   /* 8N1, TX only */
  STATLINK_USART->CR1 |= USART_CR1_UE;

  head = 0U;
  tail = 0U;

  NVIC_SetPriority(STATLINK_IRQn, (1UL << __NVIC_PRIO_BITS) - 1UL);
  NVIC_ClearPendingIRQ(STATLINK_IRQn);
  NVIC_EnableIRQ(STATLINK_IRQn);
}

/**
  * @brief  Drain the ring into the TX FIFO. Lowest NVIC priority.
  */
void STATLINK_IRQHandler(void)
{
  uint32_t t = tail;
  const uint32_t h = head;

  while ((t != h) && ((STATLINK_USART->ISR & USART_ISR_TXE_TXFNF) != 0U))
  {
    STATLINK_USART->TDR = ring[t & RING_MASK];
    t++;
    bytes_sent++;
  }

  tail = t;

  if (t == h)
  {
    /* Consumer side of the hand-off: only ever switched off when empty. */
    STATLINK_USART->CR1 &= ~USART_CR1_TXEIE_TXFNFIE;
  }
}

#else  /* FW_HOST_TEST */

static void statlink_kick(void) { }

void StatLink_Init(void)
{
  head = 0U;
  tail = 0U;
}

uint32_t StatLink_HostDrain(uint8_t *out, uint32_t max)
{
  uint32_t n = 0U;

  while ((tail != head) && (n < max))
  {
    out[n++] = ring[tail & RING_MASK];
    tail++;
    bytes_sent++;
  }
  return n;
}

#endif

/* ==========================================================================
   Producer
   ========================================================================== */

bool StatLink_Send(uint8_t type, const uint8_t *payload, uint16_t len)
{
  const uint32_t need = (uint32_t)STATLINK_OVERHEAD + len;
  uint32_t       h    = head;

  /* Free space: one slot is kept empty so head == tail always means empty. */
  if ((STATLINK_RING_SIZE - 1U - (h - tail)) < need)
  {
    frames_dropped++;
    return false;
  }

  uint16_t crc = 0xFFFFU;

  ring[h++ & RING_MASK] = STATLINK_SYNC0;
  ring[h++ & RING_MASK] = STATLINK_SYNC1;

  ring[h++ & RING_MASK] = type;                   crc = crc16_update(crc, type);
  ring[h++ & RING_MASK] = (uint8_t)(len & 0xFFU); crc = crc16_update(crc, (uint8_t)(len & 0xFFU));
  ring[h++ & RING_MASK] = (uint8_t)(len >> 8);    crc = crc16_update(crc, (uint8_t)(len >> 8));

  for (uint32_t i = 0U; i < len; i++)
  {
    ring[h++ & RING_MASK] = payload[i];
    crc = crc16_update(crc, payload[i]);
  }

  ring[h++ & RING_MASK] = (uint8_t)(crc & 0xFFU);
  ring[h++ & RING_MASK] = (uint8_t)(crc >> 8);

  /* Bytes must be visible before the index that publishes them. */
  STATLINK_DMB();
  head = h;

  statlink_kick();
  return true;
}

uint16_t StatLink_Crc16(uint16_t crc, const uint8_t *data, uint32_t len)
{
  for (uint32_t i = 0U; i < len; i++)
  {
    crc = crc16_update(crc, data[i]);
  }
  return crc;
}

uint32_t StatLink_GetBytesSent(void) { return bytes_sent; }
uint32_t StatLink_GetDropped(void)   { return frames_dropped; }

#endif /* FW_STATS_ENABLE */