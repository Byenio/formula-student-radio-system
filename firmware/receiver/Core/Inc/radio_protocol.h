#ifndef RECEIVER_RADIO_PROTOCOL_H
#define RECEIVER_RADIO_PROTOCOL_H

#include <stdint.h>

#define PACKET_START_BYTE      0xAA
#define PACKET_TYPE_AUDIO      0x01
#define PACKET_TYPE_TELEMETRY  0x02
#define MAX_PAYLOAD_SIZE       160

typedef struct {
  uint8_t start_byte;
  uint8_t type;
  uint8_t length;
  uint8_t payload[MAX_PAYLOAD_SIZE];
} RadioPacket_t;

#endif //RECEIVER_RADIO_PROTOCOL_H