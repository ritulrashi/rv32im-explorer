#!/usr/bin/env python3
"""Turns Phase 4 stats into raw.csv and tables.md. Generated output only:
never edit tables.md by hand.

    scripts/phase4_tables.py RESULTS_DIR WORKLOAD_BUILD_DIR

Every number is either an integer counter from the simulator's JSON (region of
interest only) or a ratio of two such integers, printed with a fixed number of
decimals. The hot-loop listings come from the per-pc retirement profiles and
the objdump disassembly of the exact ELFs that were measured.
"""
import csv
import glob
import json
import os
import re
import sys

MAIN_CONFIGS = ["baseline", "small_caches", "big"]
ALL_CONFIGS = ["baseline", "bimodal_btb", "gshare_btb", "small_caches", "big"]
CATS = ["base", "load_use", "branch", "icache", "dcache", "muldiv"]
NAME_RE = re.compile(r"^(asm|c-O0|c-O3)-(\w+?)-n(\d+)$")


def parse_name(name):
    m = NAME_RE.match(name)
    if not m:
        raise ValueError(name)
    src, variant, n = m.group(1), m.group(2), int(m.group(3))
    kernel = variant.split("_")[0]
    macs = n if kernel == "dot" else n ** 3
    return src, variant, kernel, n, macs


def load_rows(results):
    rows = []
    for path in sorted(glob.glob(os.path.join(results, "stats", "*", "*.json"))):
        cfg = os.path.basename(os.path.dirname(path))
        name = os.path.splitext(os.path.basename(path))[0]
        with open(path, encoding="utf-8") as f:
            st = json.load(f)
        roi = st["roi"]
        if roi is None:
            raise SystemExit(f"{path}: no region of interest")
        src, variant, kernel, n, macs = parse_name(name)
        r = {"workload": name, "source": src, "variant": variant, "kernel": kernel, "n": n,
             "config": cfg, "macs": macs, "halt": st["halt"],
             "cycles": roi["cycles"], "retired": roi["retired"],
             "loads": roi["memory_ops"]["loads"], "stores": roi["memory_ops"]["stores"],
             "cond_branches": roi["branch_events"]["cond_branches"],
             "cond_mispredicts": roi["branch_events"]["cond_mispredicts"],
             "dcache_misses": (roi["dcache"] or {}).get("misses", ""),
             "dcache_accesses": (roi["dcache"] or {}).get("accesses", ""),
             "icache_misses": (roi["icache"] or {}).get("misses", ""),
             "dram_row_hits": roi["dram"]["row_hits"], "dram_row_closed": roi["dram"]["row_closed"],
             "dram_row_misses": roi["dram"]["row_misses"]}
        for c in CATS:
            r["cpi_" + c] = roi["cpi_stack_cycles"][c]
        if sum(r["cpi_" + c] for c in CATS) != r["cycles"]:
            raise SystemExit(f"{path}: ROI CPI stack does not sum to ROI cycles")
        rows.append(r)
    return rows


def fmt(x, d=3):
    return f"{x:.{d}f}"


def order_key(r):
    src_order = {"asm": 0, "c-O0": 1, "c-O3": 2}
    return (r["kernel"], r["n"], src_order[r["source"]], r["variant"])


def table_cycles(rows, kernel, n, out):
    sel = [r for r in rows if r["kernel"] == kernel and r["n"] == n]
    names = sorted({r["workload"] for r in sel}, key=lambda w: order_key(next(r for r in sel if r["workload"] == w)))
    by = {(r["workload"], r["config"]): r for r in sel}
    out.append(f"### {kernel} N={n}: ROI cycles (CPI) per config\n")
    out.append("| workload | retired | instr/MAC | " + " | ".join(ALL_CONFIGS) + " |")
    out.append("|---|---:|---:|" + "---:|" * len(ALL_CONFIGS))
    for w in names:
        r0 = by[(w, "baseline")]
        cells = []
        for c in ALL_CONFIGS:
            r = by[(w, c)]
            cells.append(f"{r['cycles']:,} ({fmt(r['cycles'] / r['retired'], 2)})")
        out.append(f"| `{w}` | {r0['retired']:,} | {fmt(r0['retired'] / r0['macs'], 2)} | " + " | ".join(cells) + " |")
    out.append("")


def table_stack(rows, kernel, n, out):
    sel = [r for r in rows if r["kernel"] == kernel and r["n"] == n and r["config"] in MAIN_CONFIGS]
    sel.sort(key=lambda r: (order_key(r), MAIN_CONFIGS.index(r["config"])))
    out.append(f"### {kernel} N={n}: CPI stack (cycles per retired instruction, by cause)\n")
    out.append("| workload | config | CPI | " + " | ".join(CATS) + " | D-miss | cond mispred |")
    out.append("|---|---|---:|" + "---:|" * len(CATS) + "---:|---:|")
    for r in sel:
        parts = [fmt(r["cpi_" + c] / r["retired"]) for c in CATS]
        out.append(f"| `{r['workload']}` | {r['config']} | {fmt(r['cycles'] / r['retired'])} | " + " | ".join(parts)
                   + f" | {format(r['dcache_misses'], ',') if r['dcache_misses'] != '' else '-'} | {r['cond_mispredicts']:,} |")
    out.append("")


def table_per_mac(rows, out):
    base = sorted([r for r in rows if r["config"] == "baseline"], key=order_key)
    out.append("### Dynamic instruction mix per multiply-accumulate (config-independent)\n")
    out.append("| workload | MACs | instr/MAC | loads/MAC | stores/MAC | cond branches/MAC |")
    out.append("|---|---:|---:|---:|---:|---:|")
    for r in base:
        m = r["macs"]
        out.append(f"| `{r['workload']}` | {m:,} | {fmt(r['retired'] / m, 2)} | {fmt(r['loads'] / m, 2)} | "
                   f"{fmt(r['stores'] / m, 2)} | {fmt(r['cond_branches'] / m, 3)} |")
    out.append("")


# Pairs compared in RESULTS.md: (label, workload A, workload B). The ratio is
# cycles(A) / cycles(B): > 1 means B is faster than A.
COMPARISONS = [
    ("dot: unroll4 vs naive (asm)", "asm-dot_naive-n1024", "asm-dot_unroll4-n1024"),
    ("gemm-32: tiled vs naive (asm)", "asm-gemm_naive-n32", "asm-gemm_tiled-n32"),
    ("gemm-64: tiled vs naive (asm)", "asm-gemm_naive-n64", "asm-gemm_tiled-n64"),
    ("gemm-32: unroll4 vs naive (asm)", "asm-gemm_naive-n32", "asm-gemm_unroll4-n32"),
    ("gemm-64: unroll4 vs naive (asm)", "asm-gemm_naive-n64", "asm-gemm_unroll4-n64"),
    ("gemm-64: tiled_unroll4 vs naive (asm)", "asm-gemm_naive-n64", "asm-gemm_tiled_unroll4-n64"),
    ("dot: -O3 vs -O0 (C naive)", "c-O0-dot_naive-n1024", "c-O3-dot_naive-n1024"),
    ("gemm-32: -O3 vs -O0 (C naive)", "c-O0-gemm_naive-n32", "c-O3-gemm_naive-n32"),
    ("gemm-64: -O3 vs -O0 (C naive)", "c-O0-gemm_naive-n64", "c-O3-gemm_naive-n64"),
    ("gemm-32: C -O3 naive vs asm naive", "asm-gemm_naive-n32", "c-O3-gemm_naive-n32"),
    ("gemm-64: C -O3 tiled vs asm tiled", "asm-gemm_tiled-n64", "c-O3-gemm_tiled-n64"),
    ("gemm-64: C -O3 tiled vs C -O3 naive", "c-O3-gemm_naive-n64", "c-O3-gemm_tiled-n64"),
    ("gemm-64: padded tiled vs tiled (C -O3)", "c-O3-gemm_tiled-n64", "c-O3-gemm_tiled_pad-n64"),
    ("gemm-64: padded tiled vs padded naive (C -O3)", "c-O3-gemm_naive_pad-n64", "c-O3-gemm_tiled_pad-n64"),
    ("gemm-32: padded naive vs naive (C -O3)", "c-O3-gemm_naive-n32", "c-O3-gemm_naive_pad-n32"),
]


def table_comparisons(rows, out):
    by = {(r["workload"], r["config"]): r for r in rows}
    out.append("### Key comparisons: cycles(A) / cycles(B), so > 1.00 means B is faster\n")
    out.append("| comparison | " + " | ".join(ALL_CONFIGS) + " |")
    out.append("|---|" + "---:|" * len(ALL_CONFIGS))
    for label, a, b in COMPARISONS:
        cells = [fmt(by[(a, c)]["cycles"] / by[(b, c)]["cycles"], 2) for c in ALL_CONFIGS]
        out.append(f"| {label} | " + " | ".join(cells) + " |")
    out.append("")
    out.append("### Predictor effect: cycles(config) / cycles(baseline), so > 1.00 means the config is slower\n")
    out.append("| workload | bimodal_btb | gshare_btb |")
    out.append("|---|---:|---:|")
    for w in ["asm-gemm_tiled_unroll4-n32", "asm-gemm_tiled_unroll4-n64", "asm-gemm_naive-n32",
              "asm-dot_naive-n1024", "c-O3-gemm_naive-n32"]:
        b0 = by[(w, "baseline")]["cycles"]
        out.append(f"| `{w}` | {fmt(by[(w, 'bimodal_btb')]['cycles'] / b0, 3)} | "
                   f"{fmt(by[(w, 'gshare_btb')]['cycles'] / b0, 3)} |")
    out.append("")


def disasm_map(dump_path):
    m = {}
    with open(dump_path, encoding="utf-8") as f:
        for line in f:
            mm = re.match(r"^\s*([0-9a-f]+):\s+[0-9a-f]+\s+(.*)$", line.rstrip())
            if mm:
                m[int(mm.group(1), 16)] = re.sub(r"\s+", " ", mm.group(2)).strip()
    return m


def hot_loops(results, wbuild, out):
    out.append("### Hot code of each workload\n")
    out.append("Every pc retired at least half as often as the hottest pc inside the ROI (from "
               "`--pc-profile`), with its count, disassembled from the measured ELF's objdump. "
               "(Half, not equal: at -O0 the loop-condition check runs once more per loop than the body.)\n")
    for prof in sorted(glob.glob(os.path.join(results, "profiles", "*.txt"))):
        name = os.path.splitext(os.path.basename(prof))[0]
        counts = {}
        with open(prof, encoding="utf-8") as f:
            for line in f:
                pc, n = line.split()
                counts[int(pc, 16)] = int(n)
        top = max(counts.values())
        hot = sorted(pc for pc, n in counts.items() if 2 * n >= top)
        dis = disasm_map(os.path.join(wbuild, name + ".elf.dump"))
        total = sum(counts.values())
        hot_total = sum(counts[pc] for pc in hot)
        out.append(f"**`{name}`**: {len(hot)} instructions; they account for "
                   f"{fmt(100 * hot_total / total, 1)}% of the ROI's retired instructions\n")
        out.append("```")
        for pc in hot:
            out.append(f"{pc:08x}  {counts[pc]:>9,}  {dis.get(pc, '?')}")
        out.append("```\n")


def main():
    results, wbuild = sys.argv[1], sys.argv[2]
    rows = load_rows(results)
    rows.sort(key=lambda r: (order_key(r), ALL_CONFIGS.index(r["config"])))
    with open(os.path.join(results, "raw.csv"), "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()), lineterminator="\n")
        w.writeheader()
        w.writerows(rows)

    out = ["# Phase 4 tables (generated by scripts/phase4_tables.py; do not edit)", ""]
    with open(os.path.join(results, "run_summary.txt"), encoding="utf-8") as f:
        out += ["```", f.read().rstrip(), "```", ""]
    out.append("All numbers are for the region of interest only (the kernel). CPI = ROI cycles / ROI "
               "retired instructions. Every CPI stack below sums exactly to its CPI (checked by this script).\n")
    out.append("## Cycles across all five configs\n")
    for kernel, n in [("dot", 1024), ("gemm", 32), ("gemm", 64)]:
        table_cycles(rows, kernel, n, out)
    out.append("## CPI stacks (main configs)\n")
    for kernel, n in [("dot", 1024), ("gemm", 32), ("gemm", 64)]:
        table_stack(rows, kernel, n, out)
    out.append("## Comparisons\n")
    table_comparisons(rows, out)
    out.append("## Instruction mix\n")
    table_per_mac(rows, out)
    out.append("## Hot loops\n")
    hot_loops(results, wbuild, out)
    with open(os.path.join(results, "tables.md"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out) + "\n")
    print(f"phase4 tables: {len(rows)} rows")


if __name__ == "__main__":
    main()
