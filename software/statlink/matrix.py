#!/usr/bin/env python3
"""
matrix.py -- summarise the 6-scenario x 2-board baseline matrix.

    python matrix.py runs/            (directory holding the run folders)

For every run it splits the log into two windows, by time since the first
sample:
    pre     0 .. PRE_END s        before the scenario's action (PTT) started
    steady  STEADY_START .. end-TAIL s   the scenario itself

and reports, for the steady window:
    rates   per-second counter rates: median, 5th percentile, minimum
    perf    timed sections: mean of window means, max of window maxes (us),
            mean utilisation (%)
    tasks   (master) mean CPU % per task, min stack headroom, min heap

Writes matrix_rates.csv, matrix_perf.csv, matrix_tasks.csv next to the runs
and prints a compact summary. Pure standard library.
"""
import csv
import glob
import json
import os
import re
import statistics as st
import sys
from collections import defaultdict

PRE_END = 8.0
STEADY_START = 20.0
TAIL = 5.0

RUN_RE = re.compile(r"\d{8}-\d{6}_(master|slave)_(s\d)_(\w+)$")


def pct(vals, p):
    if not vals:
        return float("nan")
    v = sorted(vals)
    k = (len(v) - 1) * p / 100.0
    lo, hi = int(k), min(int(k) + 1, len(v) - 1)
    return v[lo] + (v[hi] - v[lo]) * (k - lo)


def load_samples(run):
    f = sorted(glob.glob(os.path.join(run, "samples_*.csv")))[-1]
    rows = list(csv.DictReader(open(f)))
    rows.sort(key=lambda r: int(r["t_ms"]))
    return rows


def per_second(rows, t_first):
    """Cumulative counters -> list of (t_rel_end, {field: delta per 1 s})."""
    out = []
    keys = [k for k in rows[0] if "." in k]
    for a, b in zip(rows[::10], rows[10::10]):
        dt = (int(b["t_ms"]) - int(a["t_ms"])) / 1000.0
        if dt <= 0:
            continue
        d = {k: (int(b[k]) - int(a[k])) / dt for k in keys}
        out.append(((int(b["t_ms"]) - t_first) / 1000.0, d, b))
    return out


def window(series, lo, hi):
    return [s for s in series if lo <= s[0] <= hi]


def summarise(root):
    """Return (rate_rows, perf_rows, task_rows) for every matrix run in root.
    Also used by plot_matrix.py."""
    runs = sorted(d for d in glob.glob(os.path.join(root, "*"))
                  if RUN_RE.search(os.path.basename(d)))
    if not runs:
        raise SystemExit(f"no matrix runs under {root}")

    rate_rows, perf_rows, task_rows = [], [], []

    for run in runs:
        board, scen, name = RUN_RE.search(os.path.basename(run)).groups()
        meta = json.load(open(os.path.join(run, "meta.json")))
        rows = load_samples(run)
        t_first = int(rows[0]["t_ms"])
        t_last = (int(rows[-1]["t_ms"]) - t_first) / 1000.0
        ser = per_second(rows, t_first)
        pre = window(ser, 0, PRE_END)
        steady = window(ser, STEADY_START, t_last - TAIL)

        for k in ser[0][1]:
            vals = [s[1][k] for s in steady]
            pvals = [s[1][k] for s in pre]
            last = steady[-1][2][k] if steady else ""
            rate_rows.append(dict(
                scenario=scen, name=name, board=board, field=k,
                pre_median=round(st.median(pvals), 2) if pvals else "",
                median=round(st.median(vals), 2), p5=round(pct(vals, 5), 2),
                min=round(min(vals), 2), max=round(max(vals), 2),
                last_value=last, seconds=len(vals)))

        # perf: rows keyed by t_ms
        pf = os.path.join(run, "perf.csv")
        sec = defaultdict(list)
        for p in csv.DictReader(open(pf)):
            t = (int(p["t_ms"]) - t_first) / 1000.0
            if STEADY_START <= t <= t_last - TAIL:
                sec[p["section"]].append(p)
        for s, ps in sec.items():
            cnt = sum(int(p["count"]) for p in ps)
            sm = sum(int(p["sum_cycles"]) for p in ps)
            core = meta["schemas"][0]["core_hz"]
            perf_rows.append(dict(
                scenario=scen, name=name, board=board, section=s,
                count_per_s=round(cnt / max(len(ps) * 0.1, 1e-9), 1),
                mean_us=round(sm / cnt * 1e6 / core, 3) if cnt else "",
                p99_window_max_us=round(pct([float(p["max_us"]) for p in ps], 99), 2),
                max_us=round(max(float(p["max_us"]) for p in ps), 2),
                util_pct=round(st.mean(float(p["util_pct"]) for p in ps), 3)))

        tf = os.path.join(run, "tasks.csv")
        if os.path.exists(tf):
            tk = defaultdict(list)
            for p in csv.DictReader(open(tf)):
                t = (int(p["t_ms"]) - t_first) / 1000.0
                if STEADY_START <= t <= t_last - TAIL:
                    tk[p["task"]].append(p)
            for task, ps in tk.items():
                task_rows.append(dict(
                    scenario=scen, name=name, task=task,
                    cpu_pct_mean=round(st.mean(float(p["cpu_pct"]) for p in ps), 3),
                    cpu_pct_max=round(max(float(p["cpu_pct"]) for p in ps), 3),
                    stack_free_min_words=min(int(p["stack_free_words"]) for p in ps),
                    heap_min_ever=min(int(p["heap_min_ever"]) for p in ps)))

    return rate_rows, perf_rows, task_rows


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "runs"
    rate_rows, perf_rows, task_rows = summarise(root)

    for fname, rows in (("matrix_rates.csv", rate_rows),
                        ("matrix_perf.csv", perf_rows),
                        ("matrix_tasks.csv", task_rows)):
        if rows:
            with open(os.path.join(root, fname), "w", newline="") as fh:
                w = csv.DictWriter(fh, fieldnames=list(rows[0]))
                w.writeheader()
                w.writerows(rows)
    print(f"wrote matrix_rates.csv ({len(rate_rows)}), matrix_perf.csv "
          f"({len(perf_rows)}), matrix_tasks.csv ({len(task_rows)}) in {root}")


if __name__ == "__main__":
    main()