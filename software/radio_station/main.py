#!/usr/bin/env python3
"""
Formula Student driver's radio -- base station application.

Talks to the relay board over its CDC virtual COM port. Mirrors the wire
format defined in link_proto.h; if the two ever disagree, that header wins.

    car board  --2.4 GHz-->  relay  --USB CDC-->  this script
    car board  <--2.4 GHz--  relay  <--USB CDC--  this script

Modes:

  --echo      bounce every audio packet straight back to the car. The driver
              hears their own voice having made the full round trip, which
              proves the entire chain end to end with no audio hardware on
              this side. Start here.

  --play      decode audio to the default output device as well.

  --loopback  additionally capture from the default input device and send it
              to the car, i.e. actual two-way voice.

Requires pyserial. --play/--loopback additionally need sounddevice and numpy.
Note that Python's audioop module was removed in 3.13, so mu-law is
implemented here directly -- which is also why the codec was chosen to be
simple enough that doing so is a non-event.
"""

import argparse
import collections
import datetime
import math
import os
import struct
import sys
import threading
import time
import wave

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("pyserial missing:  pip install pyserial")


# ---------------------------------------------------------------- link_proto
# Keep in step with link_proto.h.

PROTO_VERSION = 1

SYNC0, SYNC1 = 0xAA, 0x55
HEADER_LEN = 4
MAX_OTA_PAYLOAD = 251

PKT_AUDIO = 0x1
PKT_TELEM_CRITICAL = 0x2
PKT_TELEM_NORMAL = 0x3
PKT_TELEM_BULK = 0x4
PKT_CONTROL = 0x5

PKT_NAMES = {
    PKT_AUDIO: "audio",
    PKT_TELEM_CRITICAL: "telem-crit",
    PKT_TELEM_NORMAL: "telem",
    PKT_TELEM_BULK: "telem-bulk",
    PKT_CONTROL: "control",
}

CTRL_KEEPALIVE = 0x01
CTRL_PTT_STATE = 0x02
CTRL_LINK_STATS = 0x03

FLAG_PTT_ACTIVE = 1 << 0

AUDIO_RATE = 8000
AUDIO_SAMPLES_PER_FRAME = 80

CRC_POLY = 0x1021
CRC_INIT = 0xFFFF


def crc16(data: bytes) -> int:
    """CRC16-CCITT, matching Link_Crc16() byte for byte."""
    crc = CRC_INIT
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ CRC_POLY) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def frame(body: bytes) -> bytes:
    """Wrap an over-the-air packet for the USB hop."""
    c = crc16(body)
    return bytes([SYNC0, SYNC1]) + body + struct.pack("<H", c)


def make_header(pkt_type: int, seq: int, payload_len: int, flags: int = 0) -> bytes:
    return bytes([
        ((PROTO_VERSION & 0x0F) << 4) | (pkt_type & 0x0F),
        seq & 0xFF,
        payload_len & 0xFF,
        flags & 0xFF,
    ])


# ------------------------------------------------------------------- mu-law
# G.711, mirroring audio_codec.c. Stateless, so a lost packet costs one frame
# and nothing more -- the reason ADPCM was rejected for this link.

_SEG_BASE = (0, 132, 396, 924, 1980, 4092, 8316, 16764)


def _mulaw_decode_one(u: int) -> int:
    u = ~u & 0xFF
    sign = u & 0x80
    exponent = (u >> 4) & 0x07
    mantissa = u & 0x0F
    sample = _SEG_BASE[exponent] + (mantissa << (exponent + 3))
    return -sample if sign else sample


def _mulaw_encode_one(pcm: int) -> int:
    BIAS, CLIP = 0x84, 32635
    sign = 0x80 if pcm < 0 else 0x00
    mag = min(abs(pcm), CLIP)
    biased = mag + BIAS
    exponent = 7
    mask = 0x4000
    while not (biased & mask) and exponent > 0:
        exponent -= 1
        mask >>= 1
    mantissa = (biased >> (exponent + 3)) & 0x0F
    return (~(sign | (exponent << 4) | mantissa)) & 0xFF


DECODE_TABLE = [_mulaw_decode_one(i) for i in range(256)]
ENCODE_TABLE = None  # built lazily; only needed for --loopback


def mulaw_to_pcm(data: bytes):
    return [DECODE_TABLE[b] for b in data]


def pcm_to_mulaw(samples) -> bytes:
    global ENCODE_TABLE
    if ENCODE_TABLE is None:
        # 16-bit domain quantised to 14 bits: mu-law discards the low bits
        # anyway, so this table is exact and 16384 entries instead of 65536.
        ENCODE_TABLE = [_mulaw_encode_one(v << 2) for v in range(-8192, 8192)]
    out = bytearray()
    for s in samples:
        v = max(-32768, min(32767, int(s))) >> 2
        out.append(ENCODE_TABLE[v + 8192])
    return bytes(out)


# ------------------------------------------------------------- telemetry

# Mirrors CANLOG_Encode() in the VCU firmware:
#   [header][data 0..n][ID: 2 B std / 4 B ext][time: 4 B ms]
#   header = dlccode(0-3) | ext(4) | channel(5-6) | dir(7)

DLC_TABLE = (0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64)


class CanRecord:
    __slots__ = ("channel", "can_id", "data", "ext", "tx", "time_ms")

    def __init__(self, channel, can_id, data, ext, tx, time_ms):
        self.channel = channel
        self.can_id = can_id
        self.data = data
        self.ext = ext
        self.tx = tx
        self.time_ms = time_ms


def split_records(payload: bytes):
    """
    Split a telemetry payload back into individual CAN records.

    The radio board forwards these untouched -- it never parses them -- so this
    is the first place the structure is interpreted. A malformed record aborts
    the rest of the payload rather than guessing: the batch came through one
    CRC check, so if it does not decode cleanly something is wrong upstream and
    silently salvaging half of it would hide that.
    """
    out = []
    i = 0
    n = len(payload)

    while i < n:
        header = payload[i]
        code = header & 0x0F
        ext = bool(header & 0x10)
        channel = (header >> 5) & 0x03
        tx = bool(header & 0x80)

        dlc = DLC_TABLE[code]
        id_len = 4 if ext else 2
        total = 1 + dlc + id_len + 4

        if i + total > n:
            break                       # truncated tail; ignore quietly

        data = payload[i + 1: i + 1 + dlc]
        off = i + 1 + dlc
        can_id = int.from_bytes(payload[off: off + id_len], "little")
        time_ms = int.from_bytes(payload[off + id_len: off + id_len + 4], "little")

        out.append(CanRecord(channel, can_id, data, ext, tx, time_ms))
        i += total

    return out


class CanRepublisher:
    """
    Re-emits decoded records onto a python-can bus so canapka sees them as an
    ordinary CAN channel.

    The transport matches canapka's own: udp_multicast, ONE port with FOUR
    multicast groups, one per CAN bus. That is the opposite of the more obvious
    one-group-many-ports arrangement, and getting it wrong means canapka simply
    never sees anything -- no error, just silence. The constants below are
    copied from canapka's sim/run_sender.py and must stay in step with it.

    Keeping the two applications separate rather than merging this into canapka
    means a crash in one cannot take the other down, which matters on a pit
    wall.
    """

    # Must match GROUPS/PORT in canapka's sim/run_sender.py and ui/standalone.py
    GROUPS = ("225.0.0.11", "225.0.0.12", "225.0.0.13", "225.0.0.14")
    PORT = 43113

    def __init__(self):
        self.buses = {}
        self.available = False
        self.error = None
        try:
            import can
            self._can = can
            self.available = True
        except ImportError:
            self.error = "python-can not installed"

    def _bus(self, channel):
        if channel not in self.buses:
            self.buses[channel] = self._can.Bus(
                interface="udp_multicast",
                channel=self.GROUPS[channel % len(self.GROUPS)],
                port=self.PORT,
                receive_own_messages=False)
        return self.buses[channel]

    def publish(self, rec: CanRecord):
        if not self.available:
            return False
        try:
            msg = self._can.Message(arbitration_id=rec.can_id,
                                    is_extended_id=rec.ext,
                                    data=rec.data,
                                    is_rx=not rec.tx)
            self._bus(rec.channel).send(msg, timeout=0)
            return True
        except Exception as exc:
            if self.error != str(exc):
                # Print each distinct failure once. Repeating it 200 times a
                # second would bury everything else.
                print(f"CAN republish failed: {exc!r}")
            self.error = str(exc)
            return False

    def close(self):
        for b in self.buses.values():
            try:
                b.shutdown()
            except Exception:
                pass
        self.buses.clear()


# ------------------------------------------------------------------- parser

class FrameParser:
    """
    Byte-stream reassembler. Tolerant by design: the sync pattern can occur
    inside a payload, so a frame failing its CRC does not poison the stream --
    we drop one byte and resume the search, recovering within a frame or two.
    """

    def __init__(self):
        self.buf = bytearray()
        self.crc_errors = 0
        self.resyncs = 0

    def feed(self, data: bytes):
        self.buf.extend(data)
        out = []

        while True:
            # Discard anything before a plausible sync.
            start = self.buf.find(bytes([SYNC0, SYNC1]))
            if start < 0:
                # Keep at most one byte, in case a sync straddles two reads.
                if len(self.buf) > 1:
                    del self.buf[:-1]
                break
            if start > 0:
                del self.buf[:start]
                self.resyncs += 1

            if len(self.buf) < 2 + HEADER_LEN:
                break

            payload_len = self.buf[2 + 2]
            total = 2 + HEADER_LEN + payload_len + 2

            if payload_len > MAX_OTA_PAYLOAD:
                del self.buf[:1]          # impossible length: false sync
                self.resyncs += 1
                continue

            if len(self.buf) < total:
                break                     # still arriving

            body_len = HEADER_LEN + payload_len
            body = bytes(self.buf[2:2 + body_len])
            got = struct.unpack_from("<H", self.buf, 2 + body_len)[0]

            if got == crc16(body):
                out.append(body)
                del self.buf[:total]
            else:
                self.crc_errors += 1
                del self.buf[:1]
                self.resyncs += 1

        return out


def parse_packet(body: bytes):
    ver = body[0] >> 4
    ptype = body[0] & 0x0F
    seq = body[1]
    length = body[2]
    flags = body[3]
    payload = body[HEADER_LEN:HEADER_LEN + length]
    return ver, ptype, seq, flags, payload



# ------------------------------------------------------------------- player

class Player:
    """
    Jitter-buffered playback.

    Writing decoded frames straight to the sound card does not work: packets
    arrive in USB-timed bursts, so the device runs dry between them and every
    gap becomes a click. At 100 frames a second those clicks fuse into a
    steady buzz -- which is exactly what a naive implementation sounds like.

    So the audio callback pulls from a queue instead, and we let a little
    latency accumulate first. The queue is also capped: if the PC ever falls
    behind we drop the oldest audio rather than let delay grow without bound,
    because for live voice, late is the same as lost.
    """

    def __init__(self, sd, np, rate, device=None,
                 prebuffer_frames=6, max_frames=25):
        self._np = np
        self._buf = collections.deque()
        self._lock = threading.Lock()
        self._priming = True
        self._prebuffer = prebuffer_frames
        self._max = max_frames
        self.underruns = 0
        self.dropped = 0
        self.stream = sd.OutputStream(
            samplerate=rate, channels=1, dtype="int16", device=device,
            blocksize=AUDIO_SAMPLES_PER_FRAME * 2, callback=self._callback)

    def _callback(self, outdata, frames, time_info, status):
        out = []
        with self._lock:
            if self._priming:
                if len(self._buf) >= self._prebuffer:
                    self._priming = False
            if not self._priming:
                need = frames
                while need > 0 and self._buf:
                    chunk = self._buf[0]
                    take = min(need, len(chunk))
                    out.extend(chunk[:take])
                    if take == len(chunk):
                        self._buf.popleft()
                    else:
                        self._buf[0] = chunk[take:]
                    need -= take
                if need > 0:
                    # Ran dry. Silence is the least objectionable filler, and
                    # we go back to priming so one hiccup does not become a
                    # permanent stutter.
                    out.extend([0] * need)
                    self.underruns += 1
                    self._priming = True
            else:
                out = [0] * frames

        outdata[:] = self._np.array(out, dtype="int16").reshape(-1, 1)

    def push(self, samples):
        with self._lock:
            self._buf.append(list(samples))
            while len(self._buf) > self._max:
                self._buf.popleft()
                self.dropped += 1

    def start(self):
        self.stream.start()

    def stop(self):
        self.stream.stop()
        self.stream.close()


# ------------------------------------------------------------- recorder

class ConversationRecorder:
    """
    Records both sides of the conversation to one stereo WAV:
    left = driver, right = engineer.

    WHY A WALL CLOCK
    ----------------
    The two directions arrive independently -- driver audio only when the
    driver talks, engineer audio only when the operator does. Writing each
    channel as its frames turn up would let them drift apart, and a recording
    where the two halves no longer line up is useless for reviewing who said
    what. So the file advances with real time: one 10 ms slot per 10 ms
    elapsed, filled with silence when a side is quiet. The silences are part of
    the record -- a gap before an answer is information.

    Stereo rather than a mono mix because separation cannot be recovered
    afterwards, and a mix can be made from this at any time.
    """

    SLOT_SEC = 0.010

    # Most a side may run ahead of the timeline before the oldest is dropped.
    # Both ends nominally produce exactly 100 frames a second, but the car
    # board's crystal and the PC's are independent, so over a long session they
    # drift. Without a cap that drift becomes ever-growing lag in one channel
    # and the two sides stop lining up -- which is the one thing this recording
    # exists to preserve. Half a second is far more than any real burst.
    MAX_QUEUED_FRAMES = 50

    def __init__(self, directory="recordings"):
        os.makedirs(directory, exist_ok=True)
        stamp = datetime.datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
        self.path = os.path.join(directory, f"radio_{stamp}.wav")

        self.wav = wave.open(self.path, "wb")
        self.wav.setnchannels(2)
        self.wav.setsampwidth(2)
        self.wav.setframerate(AUDIO_RATE)

        self._driver = collections.deque()
        self._engineer = collections.deque()
        self._silence = [0] * AUDIO_SAMPLES_PER_FRAME
        self._start = time.time()
        self._slots = 0

        self.driver_frames = 0
        self.engineer_frames = 0
        self.dropped = 0

    def _push(self, queue, pcm):
        queue.append(list(pcm))
        while len(queue) > self.MAX_QUEUED_FRAMES:
            queue.popleft()
            self.dropped += 1

    def push_driver(self, pcm):
        self._push(self._driver, pcm)
        self.driver_frames += 1

    def push_engineer(self, pcm):
        self._push(self._engineer, pcm)
        self.engineer_frames += 1

    def tick(self):
        """Bring the file up to the current wall-clock time."""
        due = int((time.time() - self._start) / self.SLOT_SEC)

        # Cap the catch-up so a stalled process cannot suddenly write minutes
        # of silence in one pass and block the loop.
        if due - self._slots > 200:
            self._slots = due - 200

        while self._slots < due:
            left = self._driver.popleft() if self._driver else self._silence
            right = self._engineer.popleft() if self._engineer else self._silence

            n = AUDIO_SAMPLES_PER_FRAME
            inter = [0] * (n * 2)
            inter[0::2] = left[:n] + [0] * max(0, n - len(left))
            inter[1::2] = right[:n] + [0] * max(0, n - len(right))

            self.wav.writeframes(struct.pack(f"<{len(inter)}h", *inter))
            self._slots += 1

    @property
    def duration(self):
        return self._slots * self.SLOT_SEC

    def close(self):
        try:
            self.tick()
        except Exception:
            pass
        self.wav.close()
        return self.path


# -------------------------------------------------------------- devices

class AudioEngine:
    """
    Owns the sound devices and can swap them while running.

    Being able to change device without restarting matters in practice: the
    operator will often want the mic routed through noise-suppression software
    (SteelSeries Sonar, VoiceMeeter, NVIDIA Broadcast), which appears as a
    virtual device, and a grandstand is not the place to be editing command
    lines. Picking the wrong one and correcting it should cost one click.
    """

    def __init__(self, station):
        import numpy as np
        import sounddevice as sd
        self.sd, self.np = sd, np
        self.station = station
        self.input_device = None
        self.output_device = None

    # -- enumeration ---------------------------------------------------

    def devices(self, kind):
        """(index, label) for every device with channels of the given kind."""
        key = "max_input_channels" if kind == "input" else "max_output_channels"
        out = []
        try:
            hostapis = self.sd.query_hostapis()
            for i, d in enumerate(self.sd.query_devices()):
                if d[key] > 0:
                    api = hostapis[d["hostapi"]]["name"]
                    out.append((i, f"{d['name']}  [{api}]"))
        except Exception:
            pass
        return out

    def default(self, kind):
        try:
            idx = self.sd.default.device[0 if kind == "input" else 1]
            return idx if idx is not None and idx >= 0 else None
        except Exception:
            return None

    # -- open / reopen -------------------------------------------------

    def open_output(self, device=None):
        self.close_output()
        self.output_device = device
        player = Player(self.sd, self.np, AUDIO_RATE, device=device)
        player.start()
        self.station.play = player
        return player

    def open_input(self, device=None):
        self.close_input()
        self.input_device = device
        stream = self.sd.InputStream(samplerate=AUDIO_RATE, channels=1,
                                     dtype="int16", device=device,
                                     blocksize=AUDIO_SAMPLES_PER_FRAME)
        stream.start()
        self.station.capture = stream
        return stream

    def close_output(self):
        if self.station.play is not None:
            try:
                self.station.play.stop()
            except Exception:
                pass
            self.station.play = None

    def close_input(self):
        if self.station.capture is not None:
            try:
                self.station.capture.stop()
                self.station.capture.close()
            except Exception:
                pass
            self.station.capture = None

    def close(self):
        self.close_input()
        self.close_output()


# --------------------------------------------------------------- station

class Station:
    """
    One iteration of work, callable from either a console loop or a GUI timer.

    Deliberately not threaded: the serial read, the frame parser and the audio
    queues all get touched from here, and a single caller means none of that
    needs locking. Tkinter's after() drives it just as well as a while loop.
    """

    def __init__(self, ser, play=None, capture=None, recorder=None,
                 echo=False, hexdump=False, conversation=None):
        self.ser = ser
        self.play = play
        self.capture = capture
        self.recorder = recorder
        self.echo = echo
        self.hexdump = hexdump
        self.conversation = conversation

        self.parser = FrameParser()
        self.counts = collections.Counter()
        self.tx_seq = 0
        self.last_audio_seq = None
        self.seq_gaps = 0
        self.link_stats = None
        self.mic_open = False
        self.driver_talking = False
        self._last_audio_time = 0.0

        # Applied to captured audio before companding. Windows input levels
        # vary enormously between devices and driver settings, so rather than
        # asking the operator to hunt through Sound Control Panel, the level is
        # trimmed here where it can be seen and adjusted live.
        self.mic_gain = 1.0
        self.mic_peak = 0        # peak of the last captured block, for the meter
        self.rx_peak = 0         # peak of the last received frame

        self.republisher = None
        self.telem_records = 0        # total CAN records decoded
        self.telem_published = 0      # successfully put on the CAN bus
        self.telem_bad = 0            # payloads that did not decode
        self.telem_per_bus = [0, 0, 0, 0]
        self.telem_last_time = 0.0
        self.telem_active = False

        self.write_failures = 0
        self.last_rx_time = time.time()

    # -- outbound ---------------------------------------------------------

    def send_packet(self, ptype, payload, flags=0):
        hdr = make_header(ptype, self.tx_seq, len(payload), flags)
        try:
            self.ser.write(frame(hdr + payload))
        except Exception:
            # Timed out or the port went away. Dropping the packet is correct:
            # it is live audio, worthless by the time a retry would land, and
            # the link watchdog below handles a genuinely dead device.
            self.write_failures += 1
            return
        self.tx_seq = (self.tx_seq + 1) & 0xFF

    # -- one pass ---------------------------------------------------------

    def poll(self):
        data = self.ser.read(4096)
        if data:
            self.last_rx_time = time.time()
            for body in self.parser.feed(data):
                self._handle(body)

        if self.capture is not None:
            avail = self.capture.read_available
            while avail >= AUDIO_SAMPLES_PER_FRAME:
                block, _ = self.capture.read(AUDIO_SAMPLES_PER_FRAME)
                avail -= AUDIO_SAMPLES_PER_FRAME

                # Drain even while closed. Letting the device back up means the
                # first frame after keying is already stale.
                samples = block[:, 0]
                peak = int(max(abs(int(v)) for v in samples)) if len(samples) else 0
                self.mic_peak = peak

                if not self.mic_open:
                    continue

                if self.mic_gain != 1.0:
                    samples = [max(-32768, min(32767, int(v * self.mic_gain)))
                               for v in samples]

                mu = pcm_to_mulaw(samples)
                self.send_packet(PKT_AUDIO, mu, FLAG_PTT_ACTIVE)

                if self.conversation is not None:
                    # Recorded post-gain but pre-compand: this is the best
                    # version of the operator's audio that exists anywhere.
                    self.conversation.push_engineer(samples)

        # "Driver talking" is inferred from traffic rather than announced, so
        # it needs to time out -- otherwise it would latch on forever after the
        # last packet.
        if self.driver_talking and (time.time() - self._last_audio_time) > 0.3:
            self.driver_talking = False

        # Same treatment for telemetry: inferred from traffic, so it has to
        # time out or it would latch on after the car is switched off.
        if self.telem_active and (time.time() - self.telem_last_time) > 2.0:
            self.telem_active = False

        if self.conversation is not None:
            self.conversation.tick()

    def _handle(self, body):
        ver, ptype, seq, flags, payload = parse_packet(body)

        if ver != PROTO_VERSION:
            self.counts["version-mismatch"] += 1
            return

        self.counts[PKT_NAMES.get(ptype, f"type-{ptype}")] += 1

        if self.hexdump:
            print(f"  {PKT_NAMES.get(ptype, ptype)} seq={seq} "
                  f"flags={flags:02X} {payload[:12].hex(' ')}")

        if ptype == PKT_AUDIO:
            self.driver_talking = True
            self._last_audio_time = time.time()

            if self.last_audio_seq is not None:
                expected = (self.last_audio_seq + 1) & 0xFF
                if seq != expected:
                    self.seq_gaps += (seq - expected) & 0xFF
            self.last_audio_seq = seq

            if self.echo:
                self.send_packet(PKT_AUDIO, payload, flags)

            if self.play is not None or self.recorder is not None:
                pcm = mulaw_to_pcm(payload)
                self.rx_peak = max(abs(v) for v in pcm) if pcm else 0

                # Half duplex: a radio that is transmitting cannot receive, so
                # playing the fragments that squeeze through while our mic is
                # open just sounds like a fault. Silence is the honest output.
                # The packet is still counted and still recorded.
                if self.play is not None and not self.mic_open:
                    self.play.push(pcm)

                if self.recorder is not None:
                    self.recorder.writeframes(
                        struct.pack(f"<{len(pcm)}h", *pcm))

            if self.conversation is not None:
                self.conversation.push_driver(mulaw_to_pcm(payload))

        elif ptype in (PKT_TELEM_CRITICAL, PKT_TELEM_NORMAL, PKT_TELEM_BULK):
            self.telem_active = True
            self.telem_last_time = time.time()

            records = split_records(payload)
            if not records and payload:
                self.telem_bad += 1

            for rec in records:
                self.telem_records += 1
                if rec.channel < 4:
                    self.telem_per_bus[rec.channel] += 1
                if self.republisher is not None and self.republisher.publish(rec):
                    self.telem_published += 1

        elif ptype == PKT_CONTROL and payload:
            if payload[0] == CTRL_LINK_STATS and len(payload) >= 8:
                rssi = struct.unpack("b", payload[1:2])[0]
                rx = (payload[2] << 8) | payload[3]
                crce = (payload[4] << 8) | payload[5]
                self.link_stats = (rssi, rx, crce, payload[6], payload[7])

    # -- reporting --------------------------------------------------------

    def take_stats(self, elapsed):
        rates = {k: v / elapsed for k, v in self.counts.items()}
        self.counts.clear()
        return rates


# ------------------------------------------------------------------- gui

def run_gui(app):
    """
    Operator console.

    Runs with no command-line flags: everything the operator needs is in the
    window, so the app can be launched from a shortcut and left running beside
    canapka for a whole session. It also survives the radio being unplugged --
    the serial port is reopened automatically -- because on a pit wall nobody
    should have to notice a USB glitch and restart software.
    """
    import tkinter as tk
    from tkinter import ttk

    root = tk.Tk()
    root.title("Driver's Radio -- Base Station")
    root.geometry("660x760")
    root.configure(bg="#1e1e1e")

    FG, DIM, BG, PANEL = "#e0e0e0", "#808080", "#1e1e1e", "#262626"

    def label(parent, text, size=10, fg=FG, bg=BG, **kw):
        return tk.Label(parent, text=text, bg=bg, fg=fg,
                        font=("Consolas", size), **kw)

    def section(title):
        f = tk.Frame(root, bg=PANEL, padx=10, pady=8)
        f.pack(fill="x", padx=10, pady=4)
        label(f, title, 9, DIM, PANEL).pack(anchor="w")
        return f

    # ================= connection =================
    conn = section("LINK")
    conn_lbl = label(conn, "searching for radio...", 11, "#ffb000", PANEL)
    conn_lbl.pack(anchor="w", pady=(2, 0))

    # ================= voice =================
    voice = section("VOICE")

    mic_btn = tk.Button(voice, text="MIC CLOSED", font=("Consolas", 15, "bold"),
                        width=20, height=2, bg="#3a3a3a", fg=FG,
                        activebackground="#4a4a4a", relief="flat")
    mic_btn.pack(pady=(4, 2))
    label(voice, "click, or press SPACE", 8, DIM, PANEL).pack()

    driver_lbl = label(voice, "DRIVER: silent", 12, DIM, PANEL)
    driver_lbl.pack(pady=(8, 4))

    meters = tk.Frame(voice, bg=PANEL)
    meters.pack()
    label(meters, "your mic ", 9, DIM, PANEL).grid(row=0, column=0, sticky="e")
    mic_meter = tk.Canvas(meters, width=240, height=11, bg="#151515",
                          highlightthickness=0)
    mic_meter.grid(row=0, column=1, pady=1)
    label(meters, "driver   ", 9, DIM, PANEL).grid(row=1, column=0, sticky="e")
    rx_meter = tk.Canvas(meters, width=240, height=11, bg="#151515",
                         highlightthickness=0)
    rx_meter.grid(row=1, column=1, pady=1)

    gainf = tk.Frame(voice, bg=PANEL)
    gainf.pack(pady=(6, 0))
    label(gainf, "mic gain", 9, DIM, PANEL).pack(side="left", padx=(0, 6))
    gain_val = label(gainf, "1.0x", 9, FG, PANEL)

    def on_gain(v):
        app.station.mic_gain = float(v)
        gain_val.config(text=f"{float(v):.1f}x")

    gain = tk.Scale(gainf, from_=1.0, to=20.0, resolution=0.5,
                    orient="horizontal", length=190, showvalue=False,
                    command=on_gain, bg=PANEL, fg=FG, troughcolor="#151515",
                    highlightthickness=0, sliderrelief="flat")
    gain.set(1.0)
    gain.pack(side="left")
    gain_val.pack(side="left", padx=(6, 0))

    # ================= telemetry =================
    telem = section("TELEMETRY")
    telem_lbl = label(telem, "no telemetry", 11, DIM, PANEL)
    telem_lbl.pack(anchor="w", pady=(2, 0))
    bus_lbl = label(telem, "", 9, DIM, PANEL, justify="left")
    bus_lbl.pack(anchor="w")
    pub_lbl = label(telem, "", 9, DIM, PANEL, justify="left")
    pub_lbl.pack(anchor="w", pady=(2, 0))

    # ================= devices =================
    devs = section("AUDIO DEVICES")
    devgrid = tk.Frame(devs, bg=PANEL)
    devgrid.pack(anchor="w", pady=(2, 0))

    def add_selector(row, caption, kind, opener):
        label(devgrid, caption, 9, DIM, PANEL).grid(row=row, column=0,
                                                    sticky="e", padx=(0, 6))
        entries = app.engine.devices(kind) if app.engine else []
        names = ["(system default)"] + [n for _, n in entries]
        box = ttk.Combobox(devgrid, values=names, width=50, state="readonly")
        box.current(0)
        box.grid(row=row, column=1, pady=2)

        def on_pick(_e):
            sel = box.current()
            device = None if sel == 0 else entries[sel - 1][0]
            try:
                opener(device)
                warn_lbl.config(text="")
            except Exception as exc:
                warn_lbl.config(text=f"could not open device: {exc}")

        box.bind("<<ComboboxSelected>>", on_pick)

    # ================= status =================
    stat = section("STATUS")
    stats_lbl = label(stat, "waiting...", 9, DIM, PANEL, justify="left")
    stats_lbl.pack(anchor="w")
    radio_lbl = label(stat, "", 9, DIM, PANEL, justify="left")
    radio_lbl.pack(anchor="w")
    rec_lbl = label(stat, "", 9, DIM, PANEL)
    rec_lbl.pack(anchor="w", pady=(4, 0))
    warn_lbl = label(stat, "", 9, "#ffb000", PANEL)
    warn_lbl.pack(anchor="w", pady=(2, 0))

    if app.engine is not None:
        add_selector(0, "output", "output", app.engine.open_output)
        add_selector(1, "input ", "input", app.engine.open_input)

    # ================= behaviour =================
    def toggle(_event=None):
        st = app.station
        if st.capture is None:
            warn_lbl.config(text="no microphone open")
            return
        st.mic_open = not st.mic_open
        mic_btn.config(text="MIC OPEN" if st.mic_open else "MIC CLOSED",
                       bg="#c03030" if st.mic_open else "#3a3a3a")

    mic_btn.config(command=toggle)
    root.bind("<space>", toggle)
    root.bind("t", toggle)
    root.focus_force()

    def draw_meter(canvas, peak):
        canvas.delete("all")
        if peak > 0:
            db = 20 * math.log10(max(peak, 1) / 32768.0)
            frac = max(0.0, min(1.0, (db + 60.0) / 60.0))
        else:
            frac = 0.0
        w = int(240 * frac)
        colour = "#40c040" if frac < 0.8 else ("#ffb000" if frac < 0.95 else "#ff3030")
        if w > 0:
            canvas.create_rectangle(0, 0, w, 11, fill=colour, width=0)

    state = {"last": time.time()}

    def tick():
        app.poll()
        st = app.station

        now = time.time()
        elapsed = now - state["last"]
        if elapsed >= 0.5:
            state["last"] = now
            rates = st.take_stats(elapsed) if st else {}

            # -- connection --
            if app.connected:
                conn_lbl.config(text=f"connected  {app.port}", fg="#40c040")
            else:
                conn_lbl.config(
                    text=f"radio not found -- retrying ({app.retries})",
                    fg="#ffb000")

            # -- voice --
            driver_lbl.config(
                text="DRIVER: TALKING" if st.driver_talking else "DRIVER: silent",
                fg="#40c040" if st.driver_talking else DIM)

            draw_meter(mic_meter, st.mic_peak)
            draw_meter(rx_meter, st.rx_peak)
            st.mic_peak = 0
            st.rx_peak = 0

            # -- telemetry --
            trate = rates.get("telem", 0.0) + rates.get("telem-crit", 0.0) \
                  + rates.get("telem-bulk", 0.0)
            if st.telem_active:
                rec_rate = st.telem_records - state.get("prev_rec", 0)
                telem_lbl.config(
                    text=f"ACTIVE   {trate:.0f} packet/s   "
                         f"{rec_rate / elapsed:.0f} CAN frame/s",
                    fg="#40c040")
            else:
                telem_lbl.config(text="no telemetry", fg=DIM)
            state["prev_rec"] = st.telem_records

            bus_lbl.config(text="   ".join(
                f"CAN{i+1} {v}" for i, v in enumerate(st.telem_per_bus)))

            if st.republisher is None or not st.republisher.available:
                why = st.republisher.error if st.republisher else "disabled"
                pub_lbl.config(text=f"CAN republish off ({why})", fg=DIM)
            elif st.telem_published == 0 and st.republisher.error:
                # Importing python-can succeeded but publishing is failing.
                # Show the reason: the usual cause is the udp_multicast backend
                # refusing to open a socket, and hiding that behind a bare "0"
                # makes it look like nothing is being attempted.
                pub_lbl.config(text=f"republish FAILING: {st.republisher.error}",
                               fg="#ff6060")
            else:
                pub_lbl.config(
                    text=f"republished {st.telem_published} to "
                         f"{CanRepublisher.GROUPS[0]}-"
                         f"{CanRepublisher.GROUPS[-1].rsplit('.', 1)[-1]}"
                         f":{CanRepublisher.PORT}",
                    fg=DIM)

            # -- status --
            line = (f"audio {rates.get('audio', 0):5.0f}/s   "
                    f"gaps {st.seq_gaps}   usb-crc {st.parser.crc_errors}   "
                    f"resync {st.parser.resyncs}")
            if st.play is not None:
                line += f"   under {st.play.underruns}"
            stats_lbl.config(text=line)

            if st.link_stats:
                rssi, rx, crce, backp, armed = st.link_stats
                radio_lbl.config(text=f"radio   rssi {rssi} dBm   rx {rx}   "
                                      f"crc {crce}   backp {backp}   armed {armed}")

            if st.conversation is not None:
                mins, secs = divmod(int(st.conversation.duration), 60)
                rec_lbl.config(
                    text=f"REC  {os.path.basename(st.conversation.path)}   "
                         f"{mins}:{secs:02d}", fg="#c04040")

            if st.mic_open and st.driver_talking:
                warn_lbl.config(text="BOTH TRANSMITTING -- driver muted, packets lost")
            elif st.telem_bad:
                warn_lbl.config(text=f"{st.telem_bad} telemetry payloads failed to decode")
            else:
                warn_lbl.config(text="")

        root.after(5, tick)

    def on_close():
        app.shutdown()
        root.destroy()

    root.protocol("WM_DELETE_WINDOW", on_close)
    root.after(5, tick)
    root.mainloop()


# ------------------------------------------------------------------- app

class App:
    """
    Owns everything and keeps it alive.

    The serial port is opened lazily and reopened on failure, so the app can be
    started before the radio is plugged in and survives it being unplugged.
    That is deliberate: this is meant to be launched once and left running for
    a whole session alongside canapka.
    """

    RETRY_SEC = 2.0

    def __init__(self, record_dir="recordings"):
        self.ser = None
        self.port = None
        self.connected = False
        self.retries = 0
        self._last_try = 0.0

        self.conversation = ConversationRecorder(record_dir)
        self.station = Station(None, recorder=None,
                               conversation=self.conversation)
        self.station.republisher = CanRepublisher()

        self.engine = None
        try:
            self.engine = AudioEngine(self.station)
            self.engine.open_output(None)
            self.engine.open_input(None)
        except Exception:
            # No sound hardware, or no sounddevice: telemetry still works, and
            # that is the half that must not depend on audio being available.
            self.engine = None

    def _try_connect(self):
        now = time.time()
        if now - self._last_try < self.RETRY_SEC:
            return
        self._last_try = now

        port = find_port(None)
        if not port:
            self.retries += 1
            return

        try:
            # write_timeout matters as much as timeout: without it, a device
            # that enumerates but stops draining its endpoint blocks the write
            # forever, and since everything runs on one thread that freezes
            # the whole window until the cable is physically pulled.
            self.ser = serial.Serial(port, 115200,
                                     timeout=0.005, write_timeout=0.05)
            self.station.ser = self.ser
            self.port = port
            self.connected = True
            self.retries = 0
            print(f"connected: {port}")
        except Exception:
            self.retries += 1
            self.ser = None
            self.station.ser = None

    def poll(self):
        if not self.connected:
            self._try_connect()
            if self.conversation is not None:
                self.conversation.tick()   # keep the timeline honest
            return

        # The relay sends a keepalive twice a second unconditionally, so
        # silence for several seconds means the device is gone or wedged even
        # if the OS still shows the port. Forcing a reconnect recovers from an
        # RF-induced USB stall without touching the cable.
        if (time.time() - self.station.last_rx_time) > 5.0:
            print("no data for 5 s -- reopening port")
            self.connected = False
            try:
                self.ser.close()
            except Exception:
                pass
            self.ser = None
            self.station.ser = None
            self.station.last_rx_time = time.time()
            return

        try:
            self.station.poll()
        except Exception as exc:
            print(f"link lost: {exc}")
            self.connected = False
            try:
                self.ser.close()
            except Exception:
                pass
            self.ser = None
            self.station.ser = None

    def shutdown(self):
        if self.conversation is not None:
            path = self.conversation.close()
            mins, secs = divmod(int(self.conversation.duration), 60)
            print(f"saved {path}  ({mins}:{secs:02d})")
        if self.station.republisher is not None:
            self.station.republisher.close()
        if self.engine is not None:
            self.engine.close()
        if self.ser is not None:
            try:
                self.ser.close()
            except Exception:
                pass


# ---------------------------------------------------------------------- main

def find_port(explicit):
    if explicit:
        return explicit
    for p in list_ports.comports():
        # VID 0x0483 is ST; PID 0x5711 is set in ux_device_descriptors.h
        if p.vid == 0x0483 and p.pid in (0x5711, 0x5710):
            return p.device
    return None


def main():
    print("Driver's Radio -- base station")
    print("  recordings/  conversation WAVs")
    print("  telemetry is republished on udp_multicast for canapka")
    print()

    app = App()
    try:
        run_gui(app)
    except KeyboardInterrupt:
        app.shutdown()


if __name__ == "__main__":
    main()