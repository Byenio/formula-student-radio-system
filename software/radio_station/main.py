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

    # -- outbound ---------------------------------------------------------

    def send_packet(self, ptype, payload, flags=0):
        hdr = make_header(ptype, self.tx_seq, len(payload), flags)
        self.ser.write(frame(hdr + payload))
        self.tx_seq = (self.tx_seq + 1) & 0xFF

    # -- one pass ---------------------------------------------------------

    def poll(self):
        data = self.ser.read(4096)
        if data:
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

def run_gui(station, engine, has_mic):
    """
    Minimal operator console.

    Exists because PyCharm's Run window is a pipe, not a terminal -- neither
    msvcrt nor termios sees a keystroke there, so console key handling silently
    does nothing. A window also gives the operator something glanceable, which
    matters more on a pit wall than in a shell.
    """
    import tkinter as tk
    from tkinter import ttk

    root = tk.Tk()
    root.title("Driver's Radio -- Base Station")
    root.geometry("620x520")
    root.configure(bg="#1e1e1e")

    FG, DIM, BG = "#e0e0e0", "#808080", "#1e1e1e"

    def label(parent, text, size=10, fg=FG, **kw):
        return tk.Label(parent, text=text, bg=BG, fg=fg,
                        font=("Consolas", size), **kw)

    # -- mic button ----------------------------------------------------
    mic_btn = tk.Button(root, text="MIC CLOSED", font=("Consolas", 16, "bold"),
                        width=18, height=2,
                        bg="#3a3a3a", fg=FG, activebackground="#4a4a4a",
                        relief="flat", state="normal" if has_mic else "disabled")
    mic_btn.pack(pady=(16, 4))

    hint = label(root, "click, or press SPACE" if has_mic
                 else "run with --talk to enable your microphone",
                 9, DIM)
    hint.pack()

    # -- driver indicator ----------------------------------------------
    driver_lbl = label(root, "DRIVER: silent", 13)
    driver_lbl.pack(pady=(14, 2))

    # Declared early because the device selectors report failures through it.
    warn_lbl = label(root, "", 10, "#ffb000")

    # -- device selection ------------------------------------------------
    if engine is not None:
        dev_frame = tk.Frame(root, bg=BG)
        dev_frame.pack(pady=(10, 0))

        def add_selector(row, caption, kind, opener, enabled):
            label(dev_frame, caption, 9, DIM).grid(row=row, column=0,
                                                   sticky="e", padx=(0, 6))
            entries = engine.devices(kind)
            names = ["(system default)"] + [n for _, n in entries]
            box = ttk.Combobox(dev_frame, values=names, width=46,
                               state="readonly" if enabled else "disabled")
            box.current(0)
            box.grid(row=row, column=1, pady=2)

            def on_pick(_e):
                sel = box.current()
                device = None if sel == 0 else entries[sel - 1][0]
                try:
                    opener(device)
                    warn_lbl.config(text="")
                except Exception as exc:
                    # A device can refuse the rate or be exclusively held by
                    # another app; say so rather than dying silently.
                    warn_lbl.config(text=f"could not open device: {exc}")

            box.bind("<<ComboboxSelected>>", on_pick)
            return box

    # -- level meters and gain ------------------------------------------
    meter_frame = tk.Frame(root, bg=BG)
    meter_frame.pack(pady=(8, 0))

    label(meter_frame, "your mic ", 9, DIM).grid(row=0, column=0, sticky="e")
    mic_meter = tk.Canvas(meter_frame, width=220, height=12,
                          bg="#2a2a2a", highlightthickness=0)
    mic_meter.grid(row=0, column=1, pady=1)

    label(meter_frame, "driver  ", 9, DIM).grid(row=1, column=0, sticky="e")
    rx_meter = tk.Canvas(meter_frame, width=220, height=12,
                         bg="#2a2a2a", highlightthickness=0)
    rx_meter.grid(row=1, column=1, pady=1)

    if has_mic:
        gain_frame = tk.Frame(root, bg=BG)
        gain_frame.pack(pady=(6, 0))
        label(gain_frame, "mic gain", 9, DIM).pack(side="left", padx=(0, 6))

        gain_val = label(gain_frame, "1.0x", 9, FG)

        def on_gain(v):
            station.mic_gain = float(v)
            gain_val.config(text=f"{float(v):.1f}x")

        gain = tk.Scale(gain_frame, from_=1.0, to=20.0, resolution=0.5,
                        orient="horizontal", length=200, showvalue=False,
                        command=on_gain, bg=BG, fg=FG, troughcolor="#2a2a2a",
                        highlightthickness=0, sliderrelief="flat")
        gain.set(1.0)
        gain.pack(side="left")
        gain_val.pack(side="left", padx=(6, 0))

    def draw_meter(canvas, peak):
        """Log scale: a linear bar spends most of its length on loud signals,
        which is the opposite of where the interesting detail is."""
        canvas.delete("all")
        if peak > 0:
            db = 20 * math.log10(max(peak, 1) / 32768.0)
            frac = max(0.0, min(1.0, (db + 60.0) / 60.0))
        else:
            frac = 0.0
        w = int(220 * frac)
        # green up to -12 dBFS, amber to -3, red above: clipping is the thing
        # to avoid, so it has to be visible before it happens.
        colour = "#40c040" if frac < 0.8 else ("#ffb000" if frac < 0.95 else "#ff3030")
        if w > 0:
            canvas.create_rectangle(0, 0, w, 12, fill=colour, width=0)

    # -- stats ---------------------------------------------------------
    stats_lbl = label(root, "waiting for packets...", 10, DIM, justify="left")
    stats_lbl.pack(pady=(12, 2))

    radio_lbl = label(root, "", 10, DIM, justify="left")
    radio_lbl.pack()

    rec_lbl = label(root, "", 9, DIM)
    rec_lbl.pack(pady=(6, 0))

    warn_lbl.pack(pady=(10, 0))

    def toggle(_event=None):
        if not has_mic:
            return
        station.mic_open = not station.mic_open
        if station.mic_open:
            mic_btn.config(text="MIC OPEN", bg="#c03030")
        else:
            mic_btn.config(text="MIC CLOSED", bg="#3a3a3a")

    mic_btn.config(command=toggle)
    root.bind("<space>", toggle)
    root.bind("t", toggle)
    root.bind("<Escape>", lambda e: root.destroy())
    root.focus_force()

    if engine is not None:
        add_selector(0, "output", "output", engine.open_output, True)
        add_selector(1, "input ", "input", engine.open_input, has_mic)

    state = {"last": time.time()}

    def tick():
        try:
            station.poll()
        except Exception as exc:              # serial unplugged, etc.
            warn_lbl.config(text=f"link error: {exc}")
            root.after(500, tick)
            return

        now = time.time()
        elapsed = now - state["last"]
        if elapsed >= 0.5:
            state["last"] = now
            rates = station.take_stats(elapsed)

            audio = rates.get("audio", 0.0)
            ctrl = rates.get("control", 0.0)

            driver_lbl.config(
                text="DRIVER: TALKING" if station.driver_talking else "DRIVER: silent",
                fg="#40c040" if station.driver_talking else DIM)

            line = (f"audio {audio:5.0f}/s   control {ctrl:4.1f}/s   "
                    f"gaps {station.seq_gaps}   "
                    f"usb-crc {station.parser.crc_errors}")
            if station.play is not None:
                line += f"   under {station.play.underruns}"
            stats_lbl.config(text=line)

            if station.conversation is not None:
                mins, secs = divmod(int(station.conversation.duration), 60)
                rec_lbl.config(
                    text=f"REC  {os.path.basename(station.conversation.path)}"
                         f"   {mins}:{secs:02d}",
                    fg="#c04040")

            draw_meter(mic_meter, station.mic_peak)
            draw_meter(rx_meter, station.rx_peak)
            station.mic_peak = 0
            station.rx_peak = 0

            if station.link_stats:
                rssi, rx, crce, backp, armed = station.link_stats
                radio_lbl.config(
                    text=f"radio   rssi {rssi} dBm   rx {rx}   "
                         f"crc {crce}   backp {backp}   armed {armed}")

            # Both stations transmitting at once is the one case that
            # measurably degrades the link, so say so plainly.
            if station.mic_open and station.driver_talking:
                warn_lbl.config(
                    text="BOTH TRANSMITTING -- driver muted, packets lost")
            elif not station.link_stats:
                warn_lbl.config(text="")
            else:
                warn_lbl.config(text="")

        root.after(5, tick)

    root.after(5, tick)
    root.mainloop()


# ---------------------------------------------------------------------- main


def find_port(explicit):
    if explicit:
        return explicit
    for p in list_ports.comports():
        # VID 0x0483 is ST; PID 0x5711 is what we set in ux_device_descriptors.h
        if p.vid == 0x0483 and p.pid in (0x5711, 0x5710):
            return p.device
    return None


def main():
    ap = argparse.ArgumentParser(description="Driver's radio base station")
    ap.add_argument("-p", "--port", help="serial port (auto-detected if omitted)")
    ap.add_argument("--echo", action="store_true",
                    help="bounce audio packets back to the car")
    ap.add_argument("--play", action="store_true",
                    help="play received audio locally")
    ap.add_argument("--talk", "--loopback", action="store_true", dest="talk",
                    help="enable your microphone")
    ap.add_argument("--record", metavar="FILE.wav",
                    help="write received audio only, to a named WAV "
                         "(the two-sided recording below is separate)")
    ap.add_argument("--record-dir", default="recordings", metavar="DIR",
                    help="where conversation recordings go (default: "
                         "./recordings)")
    ap.add_argument("--no-record", action="store_true",
                    help="do not record the conversation")
    ap.add_argument("--list-devices", action="store_true",
                    help="print available audio devices and exit")
    ap.add_argument("--input-device", metavar="N_OR_NAME",
                    help="capture device index, or part of its name")
    ap.add_argument("--output-device", metavar="N_OR_NAME",
                    help="playback device index, or part of its name")
    ap.add_argument("--mic-gain", type=float, default=1.0, metavar="X",
                    help="multiply captured mic audio (console mode; the GUI "
                         "has a live slider)")
    ap.add_argument("--console", action="store_true",
                    help="text output instead of the window")
    ap.add_argument("--hexdump", action="store_true",
                    help="print the first bytes of every packet")
    args = ap.parse_args()

    if args.list_devices:
        try:
            import sounddevice as sd
        except ImportError:
            sys.exit("needs:  pip install sounddevice numpy")
        apis = sd.query_hostapis()
        print(f"{'idx':>4}  {'in':>3} {'out':>3}  device")
        for i, d in enumerate(sd.query_devices()):
            print(f"{i:>4}  {d['max_input_channels']:>3} "
                  f"{d['max_output_channels']:>3}  {d['name']}  "
                  f"[{apis[d['hostapi']]['name']}]")
        return

    port = find_port(args.port)
    if not port:
        print("No relay found. Available ports:")
        for p in list_ports.comports():
            desc = f"  {p.device}  {p.description}"
            if p.vid:
                desc += f"  VID:PID={p.vid:04X}:{p.pid:04X}"
            print(desc)
        sys.exit(1)

    # Baud rate is meaningless over CDC -- the host sets it, the device ignores
    # it. Left at a conventional value so nothing downstream complains.
    ser = serial.Serial(port, 115200, timeout=0.005)
    print(f"connected: {port}")

    def resolve_device(spec, kind):
        """Accept an index or a substring, so the operator can pass a readable
        name instead of a number that shifts when devices are plugged in."""
        if spec is None:
            return None
        try:
            return int(spec)
        except ValueError:
            pass
        import sounddevice as sd
        key = "max_input_channels" if kind == "input" else "max_output_channels"
        needle = spec.lower()
        for i, d in enumerate(sd.query_devices()):
            if d[key] > 0 and needle in d["name"].lower():
                return i
        sys.exit(f"no {kind} device matching {spec!r}; try --list-devices")

    play = capture = recorder = engine = None

    if args.record:
        recorder = wave.open(args.record, "wb")
        recorder.setnchannels(1)
        recorder.setsampwidth(2)
        recorder.setframerate(AUDIO_RATE)
        print(f"recording to {args.record}")

    conversation = None
    if not args.no_record:
        # On by default: an unrecorded session cannot be recovered, and the
        # cost is about 115 MB an hour on a laptop that has plenty.
        conversation = ConversationRecorder(args.record_dir)
        print(f"recording conversation to {conversation.path}")

    station = Station(ser, play=None, capture=None, recorder=recorder,
                      echo=args.echo, hexdump=args.hexdump,
                      conversation=conversation)
    station.mic_gain = args.mic_gain

    if args.play or args.talk:
        try:
            engine = AudioEngine(station)
        except ImportError:
            sys.exit("audio needs:  pip install sounddevice numpy")

        if args.play:
            engine.open_output(resolve_device(args.output_device, "output"))
        if args.talk:
            engine.open_input(resolve_device(args.input_device, "input"))

    try:
        if args.console:
            last = time.time()
            while True:
                station.poll()
                now = time.time()
                elapsed = now - last
                if elapsed >= 1.0:
                    last = now
                    rates = station.take_stats(elapsed)
                    parts = [f"{k}={v:.0f}/s" for k, v in sorted(rates.items())]
                    line = "  ".join(parts) if parts else "no packets"
                    line += f"  gaps={station.seq_gaps}"
                    line += f"  crc_err={station.parser.crc_errors}"
                    line += f"  resync={station.parser.resyncs}"
                    if station.play is not None:
                        line += (f"  under={station.play.underruns}"
                                 f" late={station.play.dropped}")
                    if station.link_stats:
                        rssi, rx, crce, backp, armed = station.link_stats
                        line += (f"  | radio rssi={rssi}dBm rx={rx} crc={crce}"
                                 f" backp={backp} armed={armed}")
                    print(line)
        else:
            run_gui(station, engine, has_mic=args.talk)
    except KeyboardInterrupt:
        print("\nstopping")
    finally:
        if conversation is not None:
            path = conversation.close()
            mins, secs = divmod(int(conversation.duration), 60)
            print(f"saved {path}  ({mins}:{secs:02d}, "
                  f"driver {conversation.driver_frames} frames, "
                  f"engineer {conversation.engineer_frames})")
        if engine is not None:
            engine.close()
        if recorder is not None:
            recorder.close()
            print(f"wrote {args.record}")
        ser.close()


if __name__ == "__main__":
    main()