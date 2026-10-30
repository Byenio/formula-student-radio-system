#!/usr/bin/env python3
"""
plot_matrix.py -- figures and tables for the baseline matrix.

    python plot_matrix.py runs/                          one matrix
    python plot_matrix.py runs/ runs_after/              before vs after
    python plot_matrix.py runs/ runs_after/ --labels "18d2a8a" "fix"
    python plot_matrix.py runs/ --out figures/

Writes to --out (default: figures/):
    delivery.png/.svg   packets delivered per direction, per scenario (%)
    cpu_master.png/.svg master CPU load per scenario, split by task
    s6_timeline.png/.svg  scenario 6 per-second rates, both directions
    tables.md           the same numbers as Markdown tables, plus timing

Needs matplotlib (uv add matplotlib). Numbers come from matrix.py's
summarise(), so the figures and the CSVs can never disagree.

Method notes, worth repeating in the thesis:
  * Master and slave of one scenario may be recorded one after the other,
    not at the same time. Delivery is therefore the ratio of steady-state
    MEDIAN rates (received/s on one board over sent/s on the other), not a
    packet-by-packet match.
  * The slave has no per-type receive counter: "car -> pit" counts every
    packet, telemetry and voice together.
"""
import argparse
import csv
import glob
import os
import re
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt                        # noqa: E402

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import matrix                                          # noqa: E402

# --- palette: validated categorical slots, light mode (print) -------------
SURFACE = "#fcfcfb"
INK = "#0b0b0b"
INK2 = "#52514e"
MUTED = "#898781"
GRID = "#e1e0d9"
AXIS = "#c3c2b7"
SERIES = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100"]   # blue, orange, aqua, yellow
OTHER = "#c3c2b7"                                          # "other" = neutral

SCEN = {
    "s1": "S1 idle",
    "s2": "S2 driver PTT",
    "s3": "S3 pit PTT",
    "s4": "S4 telemetry",
    "s5": "S5 telemetry + driver PTT",
    "s6": "S6 telemetry + pit PTT",
}
TASKS = ["e28Task", "audioTask", "rs485Task", "stats"]
TASK_LABEL = {"e28Task": "radio", "audioTask": "audio",
              "rs485Task": "RS-485", "stats": "stats"}

plt.rcParams.update({
    "font.size": 9, "axes.edgecolor": AXIS, "axes.labelcolor": INK2,
    "xtick.color": MUTED, "ytick.color": INK2, "axes.facecolor": SURFACE,
    "figure.facecolor": SURFACE, "savefig.facecolor": SURFACE,
    "axes.spines.top": False, "axes.spines.right": False,
    "svg.fonttype": "none",
})


# --- data ------------------------------------------------------------------
def load(root):
    rates, perf, tasks = matrix.summarise(root)
    r = {}
    for x in rates:
        r[(x["scenario"], x["board"], x["field"])] = x
    return dict(root=root, rates=r, perf=perf, tasks=tasks)


def med(d, scen, board, field):
    x = d["rates"].get((scen, board, field))
    return float(x["median"]) if x else 0.0


def delivery(d):
    """[(scenario, direction, sent/s, received/s, pct)] for every flow."""
    out = []
    for s in SCEN:
        up_sent = med(d, s, "master", "radio.tx")
        up_recv = med(d, s, "slave", "radio.rx")
        dn_sent = med(d, s, "slave", "radio.tx")
        dn_recv = med(d, s, "master", "radio.rx")
        if up_sent >= 1:
            out.append((s, "car → pit", up_sent, up_recv,
                        min(100.0, 100.0 * up_recv / up_sent)))
        if dn_sent >= 1:
            out.append((s, "pit → car", dn_sent, dn_recv,
                        min(100.0, 100.0 * dn_recv / dn_sent)))
    return out


def cpu(d):
    """{scenario: {task: cpu%, 'other': cpu%, 'total': cpu%}} for the master."""
    out = {}
    for t in d["tasks"]:
        o = out.setdefault(t["scenario"], {})
        o[t["task"]] = float(t["cpu_pct_mean"])
    for s, o in out.items():
        total = 100.0 - o.get("IDLE", 100.0)
        known = sum(o.get(k, 0.0) for k in TASKS)
        o["other"] = max(0.0, total - known)
        o["total"] = total
    return out


def per_second(root, board, scen, field):
    runs = [p for p in glob.glob(os.path.join(root, f"*_{board}_{scen}_*"))]
    if not runs:
        return [], []
    rows = matrix.load_samples(sorted(runs)[-1])
    t0 = int(rows[0]["t_ms"])
    xs, ys = [], []
    for t, dlt, _ in matrix.per_second(rows, t0):
        xs.append(t)
        ys.append(dlt.get(field, 0.0))
    return xs, ys


# --- drawing helpers -------------------------------------------------------
def style_axis(ax, xgrid=True):
    ax.grid(axis="x" if xgrid else "y", color=GRID, linewidth=0.6)
    ax.set_axisbelow(True)
    ax.tick_params(length=0)


def save(fig, out, name):
    for ext in ("png", "svg"):
        fig.savefig(os.path.join(out, f"{name}.{ext}"), dpi=300,
                    bbox_inches="tight")
    plt.close(fig)


# --- figures ---------------------------------------------------------------
def fig_delivery(sets, labels, out):
    flows = []                                  # union, in scenario order
    for d in sets:
        for s, direction, *_ in delivery(d):
            if (s, direction) not in flows:
                flows.append((s, direction))
    flows.sort(key=lambda f: (list(SCEN).index(f[0]), f[1]))
    vals = [{(s, dr): p for s, dr, _, _, p in delivery(d)} for d in sets]

    n = len(sets)
    bar = 0.34 if n > 1 else 0.5
    fig, ax = plt.subplots(figsize=(6.3, 0.42 * len(flows) * max(n, 1.4) + 0.9))
    for i, (v, lab) in enumerate(zip(vals, labels)):
        ys = [k * n * 0.5 + i * bar for k in range(len(flows))]
        xs = [v.get(f, 0.0) for f in flows]
        ax.barh(ys, xs, height=bar * 0.9, color=SERIES[i], label=lab,
                edgecolor=SURFACE, linewidth=1)
        for y, x in zip(ys, xs):
            ax.text(x + 1, y, f"{x:.0f} %", va="center", fontsize=8, color=INK)
    centres = [k * n * 0.5 + (n - 1) * bar / 2 for k in range(len(flows))]
    ax.set_yticks(centres, [f"{SCEN[s]}\n{dr}" for s, dr in flows])
    ax.invert_yaxis()
    ax.set_xlim(0, 112)
    ax.set_xticks([0, 25, 50, 75, 100], ["0", "25", "50", "75", "100 %"])
    ax.set_title("Packets delivered, by direction", loc="left",
                 color=INK, fontsize=10)
    style_axis(ax)
    if n > 1:
        ax.legend(frameon=False, loc="lower right", fontsize=8,
                  labelcolor=INK2)
    save(fig, out, "delivery")


def fig_cpu(sets, labels, out):
    data = [cpu(d) for d in sets]
    rows = []
    for s in SCEN:
        for d, lab in zip(data, labels):
            if s in d:
                rows.append((s, lab, d[s]))
    if not rows:
        return
    fig, ax = plt.subplots(figsize=(6.3, 0.32 * len(rows) + 1.1))
    ys = list(range(len(rows)))
    left = [0.0] * len(rows)
    keys = TASKS + ["other"]
    colours = SERIES + [OTHER]
    for k, c in zip(keys, colours):
        w = [r[2].get(k, 0.0) for r in rows]
        ax.barh(ys, w, left=left, height=0.62, color=c,
                label=TASK_LABEL.get(k, k), edgecolor=SURFACE, linewidth=1)
        left = [a + b for a, b in zip(left, w)]
    for y, r in zip(ys, rows):
        ax.text(r[2]["total"] + 0.3, y, f"{r[2]['total']:.1f} %", va="center",
                fontsize=8, color=INK)
    names = [SCEN[s] + (f"  ({lab})" if len(sets) > 1 else "")
             for s, lab, _ in rows]
    ax.set_yticks(ys, names)
    ax.invert_yaxis()
    ax.set_xlim(0, max(left) * 1.18 + 1)
    ax.set_xlabel("CPU time, % of 170 MHz (steady state, mean)")
    ax.set_title("Master CPU load by task", loc="left", color=INK, fontsize=10)
    style_axis(ax)
    ax.legend(frameon=False, ncol=5, loc="upper left",
              bbox_to_anchor=(0, -0.18 if len(rows) < 8 else -0.1),
              fontsize=8, labelcolor=INK2)
    save(fig, out, "cpu_master")


def fig_timeline(sets, labels, out):
    panels = [("slave", "radio.rx", "Car → pit: packets received per second "
                                    "(slave; master sends 50 telemetry/s)"),
              ("master", "radio.rx", "Pit → car: packets received per second "
                                     "(master; slave sends 100 voice/s)")]
    fig, axes = plt.subplots(2, 1, figsize=(6.3, 4.2), sharex=True)
    for ax, (board, field, title) in zip(axes, panels):
        for i, (d, lab) in enumerate(zip(sets, labels)):
            xs, ys = per_second(d["root"], board, "s6", field)
            ax.plot(xs, ys, color=SERIES[i], linewidth=2, label=lab)
            if xs:
                ax.text(xs[-1] + 2, ys[-1], lab, color=INK2, fontsize=8,
                        va="center")
        ax.set_title(title, loc="left", color=INK, fontsize=9)
        ax.set_ylim(bottom=0)
        style_axis(ax, xgrid=False)
    axes[-1].set_xlabel("time since start of run, s  (PTT keyed at ~10 s)")
    if len(sets) > 1:
        axes[0].legend(frameon=False, fontsize=8, labelcolor=INK2,
                       loc="lower right")
    fig.tight_layout()
    save(fig, out, "s6_timeline")


# --- tables ----------------------------------------------------------------
def md_table(head, rows):
    w = [max(len(str(h)), *(len(str(r[i])) for r in rows))
         for i, h in enumerate(head)]
    line = lambda c: "| " + " | ".join(str(x).ljust(w[i])
                                       for i, x in enumerate(c)) + " |"
    sep = "|" + "|".join("-" * (x + 2) for x in w) + "|"
    return "\n".join([line(head), sep] + [line(r) for r in rows])


def write_tables(sets, labels, out):
    parts = ["# Baseline matrix -- tables\n",
             "Steady state: from %g s after the start of each run to %g s "
             "before its end. Rates are medians of per-second values." %
             (matrix.STEADY_START, matrix.TAIL), ""]

    for d, lab in zip(sets, labels):
        parts.append(f"## Delivery -- {lab}\n")
        rows = [(SCEN[s], dr, f"{snt:.0f}", f"{rcv:.0f}", f"{p:.1f} %")
                for s, dr, snt, rcv, p in delivery(d)]
        parts.append(md_table(["scenario", "direction", "sent/s",
                               "received/s", "delivered"], rows))
        parts.append("")

        parts.append(f"## Master CPU -- {lab}\n")
        c = cpu(d)
        rows = []
        for s in SCEN:
            if s in c:
                o = c[s]
                rows.append([SCEN[s]] + [f"{o.get(k, 0):.2f}" for k in TASKS]
                            + [f"{o['other']:.2f}", f"{o['total']:.2f}"])
        parts.append(md_table(["scenario"] + [TASK_LABEL[k] for k in TASKS]
                              + ["other", "total %"], rows))
        parts.append("")

        parts.append(f"## Timed sections -- {lab}\n")
        parts.append("Mean and worst-case elapsed time per execution, in µs. "
                     "Elapsed includes any interrupts and higher-priority "
                     "tasks that ran in between.\n")
        rows = []
        for p in sorted(d["perf"], key=lambda p: (p["board"], p["section"],
                                                  p["scenario"])):
            rows.append((p["board"], p["section"], SCEN[p["scenario"]],
                         p["count_per_s"], p["mean_us"], p["max_us"],
                         p["util_pct"]))
        parts.append(md_table(["board", "section", "scenario", "runs/s",
                               "mean µs", "max µs", "util %"], rows))
        parts.append("")

        parts.append(f"## Errors and drops -- {lab}\n")
        fields = [("master", "radio.crc_err"), ("slave", "radio.crc_err"),
                  ("master", "rs485.bad_crc"), ("master", "rs485.noise"),
                  ("master", "rs485.framing"), ("master", "rs485.queue_full"),
                  ("slave", "relay.backpressure"), ("slave", "usb.tx_ring_full"),
                  ("master", "statlink.dropped"), ("slave", "statlink.dropped")]
        rows = []
        for b, f in fields:
            vals = [d["rates"].get((s, b, f)) for s in SCEN]
            rows.append([f"{b} {f}"] + [f"{float(v['median']):.1f}" if v else "-"
                                        for v in vals])
        parts.append(md_table(["counter (median /s)"] + list(SCEN), rows))
        parts.append("")

    with open(os.path.join(out, "tables.md"), "w") as fh:
        fh.write("\n".join(parts))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("roots", nargs="+", help="one or two matrix run folders")
    ap.add_argument("--labels", nargs="+", help="names for the run folders")
    ap.add_argument("--out", default="figures")
    a = ap.parse_args()
    if len(a.roots) > len(SERIES):
        sys.exit("at most four run folders")
    labels = a.labels or [os.path.basename(os.path.normpath(r))
                          for r in a.roots]
    if len(labels) != len(a.roots):
        sys.exit("--labels needs one name per run folder")

    os.makedirs(a.out, exist_ok=True)
    sets = [load(r) for r in a.roots]
    fig_delivery(sets, labels, a.out)
    fig_cpu(sets, labels, a.out)
    fig_timeline(sets, labels, a.out)
    write_tables(sets, labels, a.out)
    print(f"wrote delivery, cpu_master, s6_timeline (.png/.svg) and "
          f"tables.md to {a.out}/")


if __name__ == "__main__":
    main()