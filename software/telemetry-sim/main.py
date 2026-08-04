#!/usr/bin/env python3
"""
VCU telemetry simulator -- RS-485 side.

Stands in for the VCU so the radio board's receiver can be brought up and
proven before anyone writes the VCU firmware. Emits exactly the frame format
in VCU_RS485_TELEMETRY_SPEC.md, carrying real CANLOG records built from the
team's own DBC message IDs.

    python rs485_sim.py --port COM5 --fps 200

WIRING
------
The radio board expects differential RS-485 on RS485_A / RS485_B. Options,
easiest first:

  1. USB-RS485 adapter -> A/B. Nothing else needed.

  2. USB-UART adapter + any 3.3 V RS-485 transceiver (MAX3485, THVD2450...).
     Tie the transceiver's DE high and RE high (transmit only).

  3. Nucleo-H755 as the bridge. Run firmware that copies its ST-Link virtual
     COM port to a USART, then that USART through a transceiver. Same H7
     family as the VCU, so the USART setup transfers directly to the real
     thing later.

  4. No transceiver at all -- TTL straight into the MCU. Set RS485_RE_DE HIGH
     on the radio board (disables the receiver, so its R output goes
     high-impedance), then drive the R net directly from a 3.3 V TTL TX.
     Ugly, needs a wire onto a transceiver pin, but it proves the USART, the
     DMA and the parser without any extra hardware. Remember to put RE_DE back
     low afterwards.

Baud is 1 000 000 to match the spec. 8N1, no flow control.
"""

import argparse
import json
import os
import random
import struct
import sys
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("pyserial missing:  pip install pyserial")


# ----------------------------------------------------------------- protocol

SYNC0, SYNC1 = 0xA5, 0x5A
MAX_PAYLOAD = 251
CRC_POLY, CRC_INIT = 0x1021, 0xFFFF


def crc16(data: bytes) -> int:
    """CRC16-CCITT. Byte-identical to Link_Crc16() in the firmware."""
    crc = CRC_INIT
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ CRC_POLY) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def frame(payload: bytes) -> bytes:
    """Wrap a batch of records for the wire."""
    if not 1 <= len(payload) <= MAX_PAYLOAD:
        raise ValueError(f"payload must be 1..{MAX_PAYLOAD} bytes, got {len(payload)}")
    body = bytes([len(payload)]) + payload
    return bytes([SYNC0, SYNC1]) + body + struct.pack("<H", crc16(body))


# ------------------------------------------------------------------ CANLOG
# Mirrors CANLOG_Encode() in the VCU firmware:
#
#   [header] [data0..data(len-1)] [ID: 2 B std / 4 B ext] [time: 4 B ms]
#   header = dlccode(0-3) | ext(4) | channel(5-6) | dir(7)

DLC_TABLE = [0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64]


def dlc_to_code(n: int) -> int:
    for code, size in enumerate(DLC_TABLE):
        if size >= n:
            return code
    raise ValueError(f"no DLC code fits {n} bytes")


def encode_record(can_id: int, data: bytes, channel: int,
                  time_ms: int, ext: bool = False, tx: bool = False) -> bytes:
    """One CANLOG record. 7..73 bytes depending on payload and ID width."""
    code = dlc_to_code(len(data))
    padded = data + bytes(DLC_TABLE[code] - len(data))

    header = (code & 0x0F) | ((1 if ext else 0) << 4) \
           | ((channel & 0x03) << 5) | ((1 if tx else 0) << 7)

    out = bytes([header]) + padded
    out += struct.pack("<I", can_id) if ext else struct.pack("<H", can_id)
    out += struct.pack("<I", time_ms & 0xFFFFFFFF)
    return out


# ---------------------------------------------------------------- generator

FALLBACK_IDS = {
    1: [(0x101, 8), (0x102, 8), (0x103, 8)],
    2: [(0x100, 8), (0x201, 5), (0x050, 6)],
    3: [(0x100, 8), (0x201, 7), (0x202, 8)],
    4: [(0x400, 12), (0x401, 8), (0x402, 12)],
}


def load_message_pool(path):
    """Real IDs from the DBC export if present, plausible ones otherwise."""
    if path and os.path.exists(path):
        raw = json.load(open(path))
        pool = {}
        for ch, msgs in raw.items():
            pool[int(ch)] = [(m[0], m[2]) for m in msgs]
        return pool, True
    return FALLBACK_IDS, False


class Generator:
    """
    Produces records at a requested rate, spread across the four buses.

    Rates are deliberately uneven: on a real car the inverter and BMS chatter
    far faster than GPS, and a simulator that emits everything at one rate
    would not exercise the batching or the priority path realistically.
    """

    WEIGHTS = {1: 0.45, 2: 0.30, 3: 0.15, 4: 0.10}

    def __init__(self, pool):
        self.pool = pool
        self.t0 = time.time()
        self.counter = 0

    def time_ms(self):
        return int((time.time() - self.t0) * 1000.0) & 0xFFFFFFFF

    def one(self):
        r = random.random()
        acc = 0.0
        channel = 1
        for ch, w in self.WEIGHTS.items():
            acc += w
            if r <= acc:
                channel = ch
                break

        msgs = self.pool.get(channel) or self.pool[1]
        can_id, dlc = random.choice(msgs)

        # Counter in the first two bytes so a receiver can spot gaps; the rest
        # is noise, since nothing downstream decodes it during bring-up.
        self.counter = (self.counter + 1) & 0xFFFF
        data = struct.pack("<H", self.counter) + bytes(
            random.getrandbits(8) for _ in range(max(0, dlc - 2)))

        return encode_record(can_id, data[:dlc], channel, self.time_ms())


# --------------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(description="VCU RS-485 telemetry simulator")
    ap.add_argument("-p", "--port", help="serial port (lists them if omitted)")
    ap.add_argument("-b", "--baud", type=int, default=1000000,
                    help="baud rate (default 1000000, per spec)")
    ap.add_argument("--fps", type=int, default=200,
                    help="CAN records per second (default 200)")
    ap.add_argument("--flush-ms", type=int, default=20,
                    help="max time a partial batch waits (default 20, per spec)")
    ap.add_argument("--ids", default="can_ids.json",
                    help="DBC-derived ID pool (falls back to synthetic)")
    ap.add_argument("--burst", action="store_true",
                    help="alternate 1 s idle with 1 s at 5x rate, to exercise "
                         "batching and queue depth")
    ap.add_argument("--dry-run", action="store_true",
                    help="print frames instead of opening a port")
    args = ap.parse_args()

    pool, from_dbc = load_message_pool(args.ids)
    print(f"message pool: {'DBC' if from_dbc else 'synthetic'}, "
          f"{sum(len(v) for v in pool.values())} IDs across {len(pool)} buses")

    ser = None
    if not args.dry_run:
        if not args.port:
            print("Available ports:")
            for p in list_ports.comports():
                print(f"  {p.device}  {p.description}")
            sys.exit("\nspecify one with --port")
        ser = serial.Serial(args.port, args.baud, timeout=0.01)
        print(f"sending on {args.port} at {args.baud} baud")

    gen = Generator(pool)
    batch = bytearray()
    last_flush = time.time()
    next_record = time.time()
    last_report = time.time()

    stat_records = stat_frames = stat_bytes = 0
    burst_phase_start = time.time()
    bursting = False

    def send(buf):
        nonlocal stat_frames, stat_bytes
        wire = frame(bytes(buf))
        if ser is not None:
            ser.write(wire)
        else:
            print(f"  frame {len(wire):3d} B  payload {len(buf):3d} B  "
                  f"{wire[:12].hex(' ')}...")
        stat_frames += 1
        stat_bytes += len(wire)

    try:
        while True:
            now = time.time()

            if args.burst and (now - burst_phase_start) >= 1.0:
                bursting = not bursting
                burst_phase_start = now

            rate = args.fps * (5 if bursting else 1) if args.burst else args.fps
            if args.burst and not bursting:
                rate = 1          # near-idle, not fully silent

            interval = 1.0 / max(rate, 1)

            while now >= next_record:
                rec = gen.one()

                # A frame must contain only whole records -- never split one
                # across two frames, or the receiver desynchronises.
                if len(batch) + len(rec) > MAX_PAYLOAD:
                    send(batch)
                    batch = bytearray()
                    last_flush = now

                batch.extend(rec)
                stat_records += 1
                next_record += interval

                # Guard against falling far behind after a stall.
                if now - next_record > 0.5:
                    next_record = now

            if batch and (now - last_flush) >= args.flush_ms / 1000.0:
                send(batch)
                batch = bytearray()
                last_flush = now

            if now - last_report >= 1.0:
                el = now - last_report
                print(f"  {stat_records/el:6.0f} rec/s   {stat_frames/el:5.1f} frame/s"
                      f"   {stat_bytes/el/1024:6.1f} kB/s"
                      f"   {'BURST' if bursting else ''}")
                stat_records = stat_frames = stat_bytes = 0
                last_report = now

            time.sleep(0.0005)

    except KeyboardInterrupt:
        print("\nstopping")
    finally:
        if ser is not None:
            ser.close()


if __name__ == "__main__":
    main()