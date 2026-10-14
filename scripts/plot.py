#!/usr/bin/env python3
"""Plots from the Phase 6 sweep CSVs (docs/phase6-design.md §2). Reads only the
CSVs in results/phase6/sweeps/; never runs the simulator.

    scripts/plot.py [--sweeps results/phase6/sweeps] [--out results/phase6]

Writes plots/*.png and PLOTS.md (each figure with its source CSV, which is the
figure's table view).

Visual encoding (validated with the data-viz palette validator, see
docs/phase6-design.md §7): categorical slots in fixed order for nominal series
(CPI-stack causes, predictor types); a one-hue ordinal ramp for an ordered
quantity (associativity); one y-axis per panel, never two; hairline grid; a
2px surface gap between stacked segments; legends on every multi-series
figure. PNGs are light-mode only.
"""
import argparse
import csv
import os


import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
from matplotlib.patches import Patch  # noqa: E402

# Reference palette (dataviz skill, references/palette.md), light mode.
SURFACE = "#fcfcfb"
INK = "#0b0b0b"
INK2 = "#52514e"
MUTED = "#898781"
GRID = "#e1e0d9"
AXIS = "#c3c2b7"
CAT = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4", "#008300"]  # slots 1-6, fixed order
ORDINAL = ["#86b6ef", "#3987e5", "#1c5cab", "#0d366b"]                   # blue 250/400/550/700

CAUSES = ["base", "load_use", "branch", "icache", "dcache", "muldiv"]
MAIN_CONFIGS = [("configs/baseline.cfg", "baseline"), ("configs/small_caches.cfg", "small_caches"),
                ("configs/big.cfg", "big")]


def style():
    plt.rcParams.update({
        "figure.facecolor": SURFACE, "axes.facecolor": SURFACE, "savefig.facecolor": SURFACE,
        "font.family": "sans-serif", "font.size": 9, "text.color": INK, "axes.labelcolor": INK2,
        "axes.edgecolor": AXIS, "axes.linewidth": 0.8, "xtick.color": MUTED, "ytick.color": MUTED,
        "xtick.labelcolor": INK2, "ytick.labelcolor": INK2, "axes.titlecolor": INK, "axes.titlesize": 10,
        "axes.spines.top": False, "axes.spines.right": False, "axes.grid": False,
        "legend.frameon": False, "legend.fontsize": 8.5,
    })


def hairline_grid(ax, axis="y"):
    ax.grid(True, axis=axis, color=GRID, linewidth=0.8, linestyle="-")
    ax.set_axisbelow(True)


def read(path):
    with open(path, encoding="utf-8") as f:
        return [r for r in csv.DictReader(f) if r["status"] == "ok"]


def macs(workload):
    n = int(workload.rsplit("-n", 1)[1])
    return n if workload.split("-")[-2].startswith("dot") or "dot_" in workload else n ** 3


def short(workload):
    return workload.rsplit("-n", 1)[0]


# ---- Figure 1: CPI stacks as cycles per multiply-accumulate -----------------
def fig_cpi_stacks(rows, out):
    figs = []
    groups = [("dot", 1024), ("gemm", 32), ("gemm", 64)]
    by = {(r["workload"], r["config"]): r for r in rows}
    for kernel, n in groups:
        names = [w for w in dict.fromkeys(r["workload"] for r in rows)
                 if f"{kernel}_" in w and w.endswith(f"-n{n}")]
        fig, axes = plt.subplots(1, 3, figsize=(12, 0.42 * len(names) + 1.6), sharey=True, sharex=True)
        for ax, (cfg, label) in zip(axes, MAIN_CONFIGS):
            ys = list(range(len(names)))[::-1]
            for i, w in enumerate(names):
                r = by[(w, cfg)]
                m = macs(w)
                left = 0.0
                for k, cause in enumerate(CAUSES):
                    v = int(r["cpi_" + cause]) / m
                    if v > 0:
                        # Surface-colored edge = the 2px gap between stacked segments.
                        ax.barh(ys[i], v, left=left, height=0.62, color=CAT[k], edgecolor=SURFACE,
                                linewidth=1.5, label=cause if (i == 0 and ax is axes[0]) else None)
                    left += v
                ax.text(left, ys[i], f" {left:.1f}", va="center", ha="left", fontsize=7.5, color=INK2)
            ax.set_yticks(ys)
            ax.set_yticklabels([short(w) for w in names])
            ax.set_title(label, loc="left")
            ax.set_xlabel("cycles per multiply-accumulate")
            hairline_grid(ax, "x")
        # Legend built from all six causes, not from the drawn bars: a cause
        # that is zero in the first panel (icache/dcache on baseline) must
        # still be in the legend, or its color is unexplained elsewhere.
        handles = [Patch(facecolor=CAT[k], edgecolor=SURFACE, label=c) for k, c in enumerate(CAUSES)]
        fig.legend(handles=handles, loc="upper center", ncol=6, bbox_to_anchor=(0.5, 1.0))
        fig.suptitle(f"Where the cycles go: {kernel} N={n}, cycles per MAC by cause (ROI)",
                     x=0.01, ha="left", y=1.06, fontsize=11, color=INK)
        fig.tight_layout()
        name = f"cpi_stack_{kernel}_n{n}.png"
        fig.savefig(os.path.join(out, name), dpi=150, bbox_inches="tight")
        plt.close(fig)
        figs.append((name, f"CPI stack as cycles per MAC, {kernel} N={n}, configs baseline / small_caches / big",
                     "cpi_stacks.csv"))
    return figs


# ---- Figure 2: predictor accuracy vs table size --------------------------------
def fig_bpred(rows, out):
    rows = [r for r in rows if r["btb.entries"] == "0"]  # direction predictor alone
    workloads = list(dict.fromkeys(r["workload"] for r in rows))
    # Distinct marker per predictor (secondary encoding): series often coincide
    # exactly, and a line hidden under another must still be identifiable.
    markers = {"bimodal": "o", "gshare2": "s", "gshare4": "^"}
    series = [("static_nt", "", "static not-taken"), ("bimodal", "", "bimodal"),
              ("gshare", "2", "gshare, 2-bit history"), ("gshare", "4", "gshare, 4-bit history")]
    fig, axes = plt.subplots(2, 3, figsize=(12, 6.4), sharex=True)
    for ax, w in zip(axes.flat, workloads):
        for k, (t, h, label) in enumerate(series):
            pts = sorted((int(r["bpred.entries"]), 1000 * int(r["cond_mispredicts"]) / max(1, int(r["cond_branches"])))
                         for r in rows if r["workload"] == w and r["bpred.type"] == t
                         and (not h or r["bpred.history_bits"] == h) and r["bpred.entries"])
            if t == "static_nt":
                r0 = next(r for r in rows if r["workload"] == w and r["bpred.type"] == t)
                v = 1000 * int(r0["cond_mispredicts"]) / max(1, int(r0["cond_branches"]))
                ax.plot([16, 4096], [v, v], color=CAT[k], linewidth=2, label=label)
            else:
                xs, ys = zip(*pts)
                # Nested widths (wide underneath, drawn first) keep every series
                # visible where predictors give identical results.
                lw, ms = {"bimodal": (5, 8), "gshare2": (3, 6), "gshare4": (1.5, 4.5)}[t + h]
                ax.plot(xs, ys, color=CAT[k], linewidth=lw, marker=markers[t + h], markersize=ms,
                        markeredgecolor=SURFACE, markeredgewidth=1.0, label=label)
        ax.set_xscale("log", base=2)
        ax.set_xticks([16, 64, 256, 1024, 4096])
        ax.set_xticklabels(["16", "64", "256", "1K", "4K"])
        ax.set_ylim(bottom=0)
        ax.set_title(short(w), loc="left")
        hairline_grid(ax)
    for ax in axes[1]:
        ax.set_xlabel("predictor table entries (2-bit counters)")
    for ax in axes[:, 0]:
        ax.set_ylabel("mispredicts per 1,000 cond. branches")
    handles, labels = axes[0, 0].get_legend_handles_labels()
    fig.legend(handles, labels, loc="upper center", ncol=4, bbox_to_anchor=(0.5, 1.0))
    fig.suptitle("Direction-predictor accuracy vs table size (no BTB, ideal memory)",
                 x=0.01, ha="left", y=1.05, fontsize=11)
    fig.tight_layout()
    fig.savefig(os.path.join(out, "bpred_sweep.png"), dpi=150, bbox_inches="tight")
    plt.close(fig)
    return [("bpred_sweep.png", "Conditional mispredicts per 1,000 branches vs predictor table size", "bpred.csv")]


# ---- Figure 3: D-cache size x associativity ---------------------------------------
def fig_dcache(rows, out):
    workloads = list(dict.fromkeys(r["workload"] for r in rows))
    assocs = ["1", "2", "4", "8"]
    fig, axes = plt.subplots(2, len(workloads), figsize=(12, 6.2), sharex=True)
    for col, w in enumerate(workloads):
        for k, a in enumerate(assocs):
            pts = sorted((int(r["dcache.size_bytes"]), r) for r in rows if r["workload"] == w and r["dcache.assoc"] == a)
            xs = [p[0] for p in pts]
            miss = [1000 * int(p[1]["dcache_misses"]) / max(1, int(p[1]["dcache_accesses"])) for p in pts]
            cpm = [int(p[1]["cycles"]) / macs(w) for p in pts]
            for row, ys in ((0, miss), (1, cpm)):
                axes[row, col].plot(xs, ys, color=ORDINAL[k], linewidth=2, marker="o", markersize=5,
                                    markeredgecolor=SURFACE, markeredgewidth=1.2,
                                    label=f"{a}-way" if col == 0 and row == 0 else None)
        axes[0, col].set_title(short(w), loc="left")
        for row in (0, 1):
            ax = axes[row, col]
            ax.set_xscale("log", base=2)
            ax.set_xticks([256, 1024, 4096, 16384])
            ax.set_xticklabels(["256", "1K", "4K", "16K"])
            ax.set_ylim(bottom=0)
            hairline_grid(ax)
        axes[1, col].set_xlabel("D-cache capacity (bytes), 16 B lines")
    axes[0, 0].set_ylabel("D-misses per 1,000 accesses")
    axes[1, 0].set_ylabel("cycles per MAC")
    handles, labels = axes[0, 0].get_legend_handles_labels()
    fig.legend(handles, labels, loc="upper center", ncol=4, bbox_to_anchor=(0.5, 1.0))
    fig.suptitle("D-cache capacity x associativity (small_caches core otherwise), ROI",
                 x=0.01, ha="left", y=1.05, fontsize=11)
    fig.tight_layout()
    fig.savefig(os.path.join(out, "dcache_sweep.png"), dpi=150, bbox_inches="tight")
    plt.close(fig)
    return [("dcache_sweep.png", "D-cache miss rate and cycles per MAC vs capacity, one line per associativity",
             "dcache.csv")]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sweeps", default="results/phase6/sweeps")
    ap.add_argument("--out", default="results/phase6")
    a = ap.parse_args()
    style()
    plots = os.path.join(a.out, "plots")
    os.makedirs(plots, exist_ok=True)
    figs = []
    figs += fig_cpi_stacks(read(os.path.join(a.sweeps, "cpi_stacks.csv")), plots)
    figs += fig_bpred(read(os.path.join(a.sweeps, "bpred.csv")), plots)
    figs += fig_dcache(read(os.path.join(a.sweeps, "dcache.csv")), plots)
    lines = ["# Phase 6 plots (generated by scripts/plot.py; do not edit)", "",
             f"Generated from the sweep CSVs in `{a.sweeps}` with matplotlib {matplotlib.__version__}. "
             "Each figure's source CSV is its table view; the sweep's `.provenance.txt` and "
             "`.commands.txt` give the machine, the spec and every simulator command.", ""]
    for name, caption, src in figs:
        lines += [f"## {caption}", "", f"![{caption}](plots/{name})", "",
                  f"Source (table view): [`sweeps/{src}`](sweeps/{src})", ""]
    with open(os.path.join(a.out, "PLOTS.md"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))
    print(f"plots: {len(figs)} figures -> {plots}")


if __name__ == "__main__":
    main()
