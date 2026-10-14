#!/usr/bin/env python3
"""
statlink -- decoder and logger for the radio boards' statistics link.

Reads the framed binary stream both boards emit on their debug UART (STDC14
pins 13/14, i.e. the STLINK-V3 virtual COM port), decodes it using the schema
the board sends about itself, and records a measurement run.

    python statlink.py /dev/ttyACM0 --tag baseline_mulaw      # log a run
    python statlink.py --replay runs/<run>/raw.bin             # re-decode

Every run gets its own directory:

    runs/20261010-215400_master_baseline_mulaw/
        raw.bin       every byte received, untouched. THE record. Everything
                      else is derived from it and can be regenerated with
                      --replay, so a decoder bug never costs a measurement.
        meta.json     run id, tag, host, port, start/end times, and every
                      schema the board sent (firmware hash, clock, field list)
        samples.csv   counters, one row per report (cumulative since boot)
        perf.csv      timed sections, one row per section per window
        tasks.csv     per-task CPU and stack, one row per task per report
        link.csv      decoder health: CRC errors, bytes skipped, lost frames

Wire format and frame layouts are defined in the firmware's statlink.h and
stats.h. If this file and those ever disagree, the firmware headers win.

Only dependency for live capture: pyserial. Replay needs nothing.
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import socket
import struct
import sys
import time
from collections import deque
from dataclasses import dataclass, field
from datetime import datetime
from typing import Iterator, Optional

# ---------------------------------------------------------------------------
# Wire format (statlink.h)
# ---------------------------------------------------------------------------

SYNC = b"\xA5\x5A"
OVERHEAD = 7                 # sync 2 + type 1 + len 2 + crc 2
MAX_PAYLOAD = 4096           # anything larger is a false sync

FRAME_SCHEMA, FRAME_SAMPLE, FRAME_PERF, FRAME_TASKS = 0x01, 0x02, 0x03, 0x04
FRAME_NAMES = {FRAME_SCHEMA: "SCHEMA", FRAME_SAMPLE: "SAMPLE",
               FRAME_PERF: "PERF", FRAME_TASKS: "TASKS"}

BOARD_NAMES = {0: "master", 1: "slave", 255: "hosttest"}

# FreeRTOS eTaskState
TASK_STATES = {0: "running", 1: "ready", 2: "blocked", 3: "suspended",
               4: "deleted", 5: "invalid"}


def crc16_ccitt_false(data: bytes, crc: int = 0xFFFF) -> int:
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else (crc << 1)
            crc &= 0xFFFF
    return crc


class FrameDecoder:
    """Turns a byte stream into (type, payload) frames.

    Resynchronises after corruption: a bad CRC discards only the first sync
    byte and searches again, so a frame that merely *contains* A5 5A in its
    payload cannot swallow the real frame after it.
    """

    def __init__(self) -> None:
        self.buf = bytearray()
        self.crc_errors = 0
        self.bytes_skipped = 0
        self.frames = 0

    def feed(self, data: bytes) -> Iterator[tuple[int, bytes]]:
        self.buf += data
        while True:
            i = self.buf.find(SYNC)
            if i < 0:
                # keep a trailing A5: it may be the first half of a sync
                keep = 1 if self.buf[-1:] == SYNC[:1] else 0
                self.bytes_skipped += len(self.buf) - keep
                del self.buf[:len(self.buf) - keep]
                return
            if i > 0:
                self.bytes_skipped += i
                del self.buf[:i]
            if len(self.buf) < 5:
                return
            ftype = self.buf[2]
            length = self.buf[3] | (self.buf[4] << 8)
            if length > MAX_PAYLOAD:
                self.crc_errors += 1
                self.bytes_skipped += 1
                del self.buf[:1]
                continue
            total = OVERHEAD + length
            if len(self.buf) < total:
                return
            body = bytes(self.buf[2:5 + length])
            got = self.buf[5 + length] | (self.buf[6 + length] << 8)
            if crc16_ccitt_false(body) != got:
                self.crc_errors += 1
                self.bytes_skipped += 1
                del self.buf[:1]
                continue
            payload = bytes(self.buf[5:5 + length])
            del self.buf[:total]
            self.frames += 1
            yield ftype, payload


# ---------------------------------------------------------------------------
# Payload decoding (stats.h)
# ---------------------------------------------------------------------------

class Reader:
    def __init__(self, data: bytes) -> None:
        self.d, self.o = data, 0

    def u8(self) -> int:
        v = self.d[self.o]; self.o += 1; return v

    def u16(self) -> int:
        v, = struct.unpack_from("<H", self.d, self.o); self.o += 2; return v

    def u32(self) -> int:
        v, = struct.unpack_from("<I", self.d, self.o); self.o += 4; return v

    def str(self) -> str:
        n = self.u8()
        s = self.d[self.o:self.o + n].decode("ascii", "replace")
        self.o += n
        return s


@dataclass
class Schema:
    proto: int
    board: int
    schema_id: int
    core_hz: int
    sample_ms: int
    tasks_ms: int
    perf_overhead: int
    perf_ok: bool
    baud: int
    build: str
    fields: list[tuple[int, str]]      # (kind, name), kind 0=u32 1=i32
    perf: list[str]

    @property
    def board_name(self) -> str:
        return BOARD_NAMES.get(self.board, f"board{self.board}")

    def as_json(self) -> dict:
        return {
            "proto": self.proto, "board": self.board_name,
            "schema_id": f"0x{self.schema_id:04X}", "core_hz": self.core_hz,
            "sample_ms": self.sample_ms, "tasks_ms": self.tasks_ms,
            "perf_overhead_cycles": self.perf_overhead,
            "perf_supported": self.perf_ok, "baud": self.baud,
            "build": self.build,
            "fields": [{"name": n, "kind": "i32" if k else "u32"}
                       for k, n in self.fields],
            "perf_sections": self.perf,
        }


def parse_schema(r: Reader) -> Schema:
    proto, board = r.u8(), r.u8()
    sid, core = r.u16(), r.u32()
    sample_ms, tasks_ms = r.u16(), r.u16()
    overhead, ok, baud = r.u32(), r.u8(), r.u32()
    build = r.str()
    fields = [(r.u8(), r.str()) for _ in range(r.u16())]
    perf = [r.str() for _ in range(r.u8())]
    return Schema(proto, board, sid, core, sample_ms, tasks_ms, overhead,
                  bool(ok), baud, build, fields, perf)


# ---------------------------------------------------------------------------
# Run recorder
# ---------------------------------------------------------------------------

@dataclass
class LinkHealth:
    lost_frames: int = 0             # gaps in the firmware's frame sequence
    no_schema: int = 0               # data frames whose schema never came
    late_decoded: int = 0            # held until their schema arrived
    schema_mismatch: int = 0         # data frame for a schema we do not have
    reboots: int = 0                 # sequence restarted near zero


class Run:
    """Decodes frames into CSV files. Owns no I/O source -- feed() it bytes."""

    def __init__(self, out_dir: Optional[str], tag: str, quiet: bool,
                 save_raw: bool = True) -> None:
        self.out_dir = out_dir
        self.tag = tag
        self.quiet = quiet
        self.dec = FrameDecoder()
        self.health = LinkHealth()
        self.schemas: dict[int, Schema] = {}
        self.current: Optional[Schema] = None
        self.last_seq: Optional[int] = None
        self.files: dict[str, tuple] = {}
        self.raw = None
        self.meta: dict = {}
        self.last_print = 0.0
        self.latest: dict = {}
        # Data frames whose schema has not arrived yet -- at attach time, or
        # because the schema frame itself was corrupted. Held and decoded
        # when it arrives, so one lost schema costs nothing. Bounded: ~100 s
        # of reports at 10 Hz before the oldest are given up.
        self.pending: deque = deque(maxlen=2000)

        if out_dir:
            os.makedirs(out_dir, exist_ok=True)
            if save_raw:
                self.raw = open(os.path.join(out_dir, "raw.bin"), "wb")

    # -- csv helpers ------------------------------------------------------
    def _writer(self, name: str, header: list[str]):
        if not self.out_dir:
            return None
        if name not in self.files:
            f = open(os.path.join(self.out_dir, name), "w", newline="")
            w = csv.writer(f)
            w.writerow(header)
            self.files[name] = (f, w)
        return self.files[name][1]

    # -- input ------------------------------------------------------------
    def feed(self, data: bytes, host_t: float) -> None:
        if self.raw:
            self.raw.write(data)
        for ftype, payload in self.dec.feed(data):
            try:
                self._frame(ftype, payload, host_t)
            except (struct.error, IndexError):
                # CRC was good but the layout was not what we expected:
                # count it rather than crash a measurement run.
                self.health.schema_mismatch += 1
        if not self.quiet and host_t - self.last_print >= 1.0:
            self.last_print = host_t
            self._print_status()

    def _frame(self, ftype: int, payload: bytes, host_t: float) -> None:
        r = Reader(payload)
        seq, t_ms = r.u32(), r.u32()

        if self.last_seq is not None:
            gap = (seq - self.last_seq - 1) & 0xFFFFFFFF
            if seq < 16 and self.last_seq > seq:
                self.health.reboots += 1          # board restarted
            elif gap and gap < 0x80000000:
                self.health.lost_frames += gap
        self.last_seq = seq

        lw = self._writer("link.csv", ["host_time", "seq", "t_ms", "crc_errors",
                                       "bytes_skipped", "lost_frames", "reboots"])
        if ftype == FRAME_SCHEMA:
            s = parse_schema(r)
            if s.schema_id not in self.schemas and not self.quiet:
                print(f"[schema 0x{s.schema_id:04X}] {s.board_name}  {s.build}  "
                      f"{s.core_hz/1e6:.0f} MHz  {len(s.fields)} counters  "
                      f"{len(s.perf)} sections  overhead {s.perf_overhead} cyc",
                      file=sys.stderr)
            self.schemas[s.schema_id] = s
            self.current = s
            self._drain_pending(s.schema_id)
            if lw:
                lw.writerow([f"{host_t:.3f}", seq, t_ms, self.dec.crc_errors,
                             self.dec.bytes_skipped, self.health.lost_frames,
                             self.health.reboots])
            return

        if ftype == FRAME_TASKS:
            self._tasks(r, seq, t_ms, host_t)
            return

        sid = r.u16()
        s = self.schemas.get(sid)
        if s is None:
            if len(self.pending) == self.pending.maxlen:
                self.health.no_schema += 1        # oldest is about to go
            self.pending.append((ftype, payload, host_t, sid))
            return
        self._data(ftype, r, s, sid, seq, t_ms, host_t)

    def _drain_pending(self, sid: int) -> None:
        keep = deque(maxlen=self.pending.maxlen)
        for ftype, payload, host_t, psid in self.pending:
            if psid != sid:
                keep.append((ftype, payload, host_t, psid))
                continue
            r = Reader(payload)
            seq, t_ms = r.u32(), r.u32()
            r.u16()
            try:
                self._data(ftype, r, self.schemas[sid], sid, seq, t_ms, host_t)
                self.health.late_decoded += 1
            except (struct.error, IndexError):
                self.health.schema_mismatch += 1
        self.pending = keep

    def _data(self, ftype: int, r: Reader, s: Schema, sid: int,
              seq: int, t_ms: int, host_t: float) -> None:

        if ftype == FRAME_SAMPLE:
            vals = []
            for kind, _ in s.fields:
                v = r.u32()
                vals.append(v - (1 << 32) if kind and v & 0x80000000 else v)
            w = self._writer(f"samples_{sid:04X}.csv",
                             ["host_time", "seq", "t_ms"] + [n for _, n in s.fields])
            if w:
                w.writerow([f"{host_t:.3f}", seq, t_ms] + vals)
            self.latest["sample"] = dict(zip((n for _, n in s.fields), vals))

        elif ftype == FRAME_PERF:
            window = r.u32()
            us = 1e6 / s.core_hz
            w = self._writer("perf.csv",
                             ["host_time", "seq", "t_ms", "window_cycles", "section",
                              "count", "min_cycles", "max_cycles", "sum_cycles",
                              "mean_us", "max_us", "util_pct"])
            rows = {}
            for name in s.perf:
                count, mn, mx, sm = r.u32(), r.u32(), r.u32(), r.u32()
                if count == 0:
                    continue          # section not entered this window
                mean_us = sm / count * us
                util = 100.0 * sm / window if window else 0.0
                rows[name] = (count, mean_us, mx * us, util)
                if w:
                    w.writerow([f"{host_t:.3f}", seq, t_ms, window, name, count,
                                mn, mx, sm, f"{mean_us:.3f}", f"{mx*us:.3f}",
                                f"{util:.3f}"])
            self.latest["perf"] = rows

    def _tasks(self, r: Reader, seq: int, t_ms: int, host_t: float) -> None:
        total, heap_free, heap_min = r.u32(), r.u32(), r.u32()
        w = self._writer("tasks.csv",
                         ["host_time", "seq", "t_ms", "task", "number", "priority",
                          "state", "stack_free_words", "run_cycles", "cpu_pct",
                          "heap_free", "heap_min_ever"])
        cpu = {}
        for _ in range(r.u8()):
            num, name, prio, state = r.u8(), r.str(), r.u8(), r.u8()
            stack, run = r.u16(), r.u32()
            pct = 100.0 * run / total if total else 0.0
            cpu[name] = (pct, stack)
            if w:
                w.writerow([f"{host_t:.3f}", seq, t_ms, name, num, prio,
                            TASK_STATES.get(state, state), stack, run,
                            f"{pct:.3f}", heap_free, heap_min])
        self.latest["tasks"] = (cpu, heap_free, heap_min)

    # -- console ----------------------------------------------------------
    def _print_status(self) -> None:
        d, h = self.dec, self.health
        line = (f"frames {d.frames}  crc_err {d.crc_errors}  "
                f"lost {h.lost_frames}  skipped {d.bytes_skipped}B")
        tasks = self.latest.get("tasks")
        if tasks:
            cpu, heap_free, _ = tasks
            idle = cpu.get("IDLE", (None,))[0]
            if idle is not None:
                line += f"  | cpu {100 - idle:5.1f}%  heap {heap_free}B"
        perf = self.latest.get("perf", {})
        for name in ("audio.frame", "loop"):
            if name in perf:
                c, mean_us, max_us, util = perf[name]
                line += f"  | {name} {mean_us:.0f}/{max_us:.0f}us"
        print(line, file=sys.stderr)

    # -- finish -----------------------------------------------------------
    def close(self, extra_meta: dict) -> dict:
        summary = {
            "frames": self.dec.frames,
            "crc_errors": self.dec.crc_errors,
            "bytes_skipped": self.dec.bytes_skipped,
            "lost_frames": self.health.lost_frames,
            "late_decoded": self.health.late_decoded,
            "no_schema": self.health.no_schema + len(self.pending),
            "schema_mismatch": self.health.schema_mismatch,
            "reboots": self.health.reboots,
        }
        if self.out_dir:
            meta = dict(extra_meta)
            meta["decoder"] = summary
            meta["schemas"] = [s.as_json() for s in self.schemas.values()]
            with open(os.path.join(self.out_dir, "meta.json"), "w") as f:
                json.dump(meta, f, indent=2)
            for f, _ in self.files.values():
                f.close()
            if self.raw:
                self.raw.close()
        return summary


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def run_dir(root: str, board: str, tag: str) -> str:
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    name = f"{stamp}_{board}" + (f"_{tag}" if tag else "")
    return os.path.join(root, name)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port", nargs="?", help="serial port, e.g. /dev/ttyACM0")
    ap.add_argument("--baud", type=int, default=1_000_000)
    ap.add_argument("--tag", default="", help="short run label, e.g. baseline_mulaw")
    ap.add_argument("--board", default="board",
                    help="label for the run directory (the schema says which "
                         "board it really was; this is only the folder name)")
    ap.add_argument("--out", default="runs", help="root directory for runs")
    ap.add_argument("--duration", type=float, default=0,
                    help="stop after this many seconds (0 = until Ctrl+C)")
    ap.add_argument("--replay", metavar="RAW",
                    help="decode a raw.bin instead of a serial port; writes "
                         "the CSVs next to it unless --out is given")
    ap.add_argument("--no-save", action="store_true", help="decode and print only")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    meta = {"tag": args.tag, "host": socket.gethostname(),
            "started": datetime.now().isoformat(timespec="seconds"),
            "argv": sys.argv}

    if args.replay:
        out = None if args.no_save else (
            args.out if args.out != "runs" else
            os.path.join(os.path.dirname(os.path.abspath(args.replay)), "replay"))
        # save_raw=False: a replay must never write a raw.bin of its own,
        # which could later be mistaken for (or overwrite) an original.
        run = Run(out, args.tag, args.quiet, save_raw=False)
        with open(args.replay, "rb") as f:
            data = f.read()
        meta["source"] = os.path.abspath(args.replay)
        for i in range(0, len(data), 4096):
            run.feed(data[i:i + 4096], host_t=0.0)
        summary = run.close(meta)
        print(json.dumps(summary), file=sys.stderr)
        return 0

    if not args.port:
        ap.error("give a serial port, or --replay RAW")

    try:
        import serial  # type: ignore
    except ImportError:
        print("pyserial is required for live capture: pip install pyserial",
              file=sys.stderr)
        return 1

    out = None if args.no_save else run_dir(args.out, args.board, args.tag)
    run = Run(out, args.tag, args.quiet)
    meta.update({"port": args.port, "baud": args.baud})
    if out:
        print(f"recording to {out}", file=sys.stderr)

    t_end = time.monotonic() + args.duration if args.duration else None
    with serial.Serial(args.port, args.baud, timeout=0.1) as ser:
        ser.reset_input_buffer()
        try:
            while t_end is None or time.monotonic() < t_end:
                data = ser.read(4096)
                if data:
                    run.feed(data, time.time())
        except KeyboardInterrupt:
            pass

    meta["ended"] = datetime.now().isoformat(timespec="seconds")
    summary = run.close(meta)
    print(json.dumps(summary), file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())