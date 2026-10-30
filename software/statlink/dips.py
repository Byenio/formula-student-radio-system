#!/usr/bin/env python3
"""
dips.py -- line up the slave's radio counters with its main-loop timing,
one row per second, so a reception dip can be classified from one log.

    python dips.py runs/<slave run>          (default: newest slave run)

Columns per second:
    dio1      edges on the radio's DIO1 pin  (the radio said "something happened")
    rx        packets read out of the radio  (the firmware got one)
    crc       packets the radio received with a bad CRC (heard, but damaged)
    gaps      sequence-number gaps           (packets the master sent but we missed)
    loops     main-loop passes               (how often the relay got to look)
    loop_max  longest pass, us
    relay_max / usbx_max   longest pass of those two stages, us

How to read a dip (rx low):
    dio1 low too, loops normal  -> the radio itself heard nothing: RF / power
    dio1 high, crc high         -> radio heard it, too damaged to use: RF
    dio1 high, rx and crc low   -> radio heard it, firmware lost it
    loops collapse / max huge   -> the loop was stuck somewhere: firmware
"""
import csv
import glob
import os
import sys
from collections import defaultdict


def newest_slave_run() -> str:
    runs = sorted(d for d in glob.glob("runs/*") if "slave" in d)
    if not runs:
        sys.exit("no runs/*slave* directory here; pass the run directory")
    return runs[-1]


def main() -> None:
    run = sys.argv[1] if len(sys.argv) > 1 else newest_slave_run()
    samples = sorted(glob.glob(os.path.join(run, "samples_*.csv")))
    if not samples:
        sys.exit(f"no samples_*.csv in {run}")

    rows = list(csv.DictReader(open(samples[-1])))
    rows.sort(key=lambda r: int(r["t_ms"]))

    # perf.csv: one row per section per 100 ms window, keyed by t_ms
    perf = defaultdict(dict)
    pf = os.path.join(run, "perf.csv")
    if os.path.exists(pf):
        for p in csv.DictReader(open(pf)):
            perf[int(p["t_ms"])][p["section"]] = p

    def counter(a, b, key):
        return int(b[key]) - int(a[key]) if key in b else -1

    print(f"run: {run}")
    print(f"{'t[s]':>7} {'dio1':>5} {'rx':>5} {'crc':>5} {'gaps':>5} {'loops':>7} "
          f"{'loop_max':>9} {'relay_max':>10} {'usbx_max':>9}  armed")

    # 10 samples = 1 s at the 100 ms report period
    for a, b in zip(rows[::10], rows[10::10]):
        t0, t1 = int(a["t_ms"]), int(b["t_ms"])
        loops = 0
        mx = {"loop": 0.0, "relay": 0.0, "usbx": 0.0}
        for t, secs in perf.items():
            if t0 < t <= t1:
                if "loop" in secs:
                    loops += int(secs["loop"]["count"])
                for s in mx:
                    if s in secs:
                        mx[s] = max(mx[s], float(secs[s]["max_us"]))

        rx = counter(a, b, "radio.rx")
        flag = "   <-- dip" if 0 <= rx < 20 else ""
        print(f"{t1/1000:7.1f} {counter(a, b, 'radio.dio1'):5d} {rx:5d} "
              f"{counter(a, b, 'radio.crc_err'):5d} {counter(a, b, 'radio.seq_gaps'):5d} {loops:7d} "
              f"{mx['loop']:9.0f} {mx['relay']:10.0f} {mx['usbx']:9.0f}  "
              f"{b.get('radio.rx_armed', '?'):>5}{flag}")


if __name__ == "__main__":
    main()