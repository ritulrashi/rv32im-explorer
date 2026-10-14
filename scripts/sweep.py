#!/usr/bin/env python3
"""Parallel design-space sweeps over the pipeline model (docs/phase6-design.md §1).

    scripts/sweep.py SPEC.json [--build build/release] [--out results/phase6/sweeps] [--workers N]
    scripts/sweep.py --selftest

A spec names workloads and the config values to sweep:

    {
      "name": "dcache",
      "base_config": "configs/small_caches.cfg",
      "workloads": ["asm-gemm_naive-n64", "tests/riscv-tests/rv32ui-add"],
      "axes": {"dcache.size_bytes": [256, 1024], "dcache.assoc": [1, 2]}
    }

* "axes" is a Cartesian product; "groups": [{"axes": {...}}, ...] gives a
  concatenation of products (so e.g. static_nt is not swept over table sizes
  it does not have). The special axis "config" sets the base config file.
* A workload name is an ELF under BUILD/workloads/, or a path relative to
  BUILD (without .elf).
* Each point runs `rvsim-pipe ELF --config BASE --set k=v ... --stats -`.
  Points the config validator rejects are recorded as `skipped` with the
  message, never silently dropped. Any other failure (workload result check
  failed, simulator error, CPI stack not summing to cycles) aborts the sweep.
* Rows are written in spec order, independent of worker count and scheduling,
  so the CSV is byte-identical for any --workers value. Wall time and
  provenance go to a sidecar file, never into the CSV.
"""
import argparse
import concurrent.futures
import csv
import io
import itertools
import json
import os
import subprocess
import sys
import time

CATS = ["base", "load_use", "branch", "icache", "dcache", "muldiv"]
COUNTERS = (["cycles", "retired"] + ["cpi_" + c for c in CATS] +
            ["cond_branches", "cond_mispredicts", "jumps", "jump_mispredicts", "btb_hits",
             "loads", "stores", "icache_accesses", "icache_misses", "dcache_accesses", "dcache_misses",
             "dram_row_hits", "dram_row_closed", "dram_row_misses"])


def expand(spec):
    """Returns (axis_keys, points); a point is (workload, ((key, value), ...))."""
    groups = spec.get("groups") or [{"axes": spec.get("axes", {})}]
    keys = []
    for g in groups:
        for k in g["axes"]:
            if k not in keys:
                keys.append(k)
    points = []
    for w in spec["workloads"]:
        for g in groups:
            names = list(g["axes"])
            for combo in itertools.product(*[g["axes"][k] for k in names]):
                points.append((w, tuple(zip(names, combo))))
    return keys, points


def elf_path(build, workload):
    if "/" in workload:
        return os.path.join(build, workload + ".elf")
    return os.path.join(build, "workloads", workload + ".elf")


def command(build, base_config, workload, settings):
    cfg = base_config
    sets = []
    for k, v in settings:
        if k == "config":
            cfg = v
        else:
            sets += ["--set", f"{k}={v}"]
    return [os.path.join(build, "rvsim-pipe"), elf_path(build, workload), "--config", cfg] + sets + ["--stats", "-"]


def counters_from(stats):
    block = stats["roi"] if stats.get("roi") else stats
    ic, dc = block["icache"] or {}, block["dcache"] or {}
    row = {"cycles": block["cycles"], "retired": block["retired"]}
    for c in CATS:
        row["cpi_" + c] = block["cpi_stack_cycles"][c]
    be = block["branch_events"]
    for k in ("cond_branches", "cond_mispredicts", "jumps", "jump_mispredicts", "btb_hits"):
        row[k] = be[k]
    row["loads"], row["stores"] = block["memory_ops"]["loads"], block["memory_ops"]["stores"]
    row["icache_accesses"], row["icache_misses"] = ic.get("accesses", ""), ic.get("misses", "")
    row["dcache_accesses"], row["dcache_misses"] = dc.get("accesses", ""), dc.get("misses", "")
    for k in ("row_hits", "row_closed", "row_misses"):
        row["dram_" + k] = block["dram"][k]
    if sum(row["cpi_" + c] for c in CATS) != row["cycles"]:
        raise RuntimeError("CPI stack does not sum to cycles")
    return ("roi" if stats.get("roi") else "full"), row


def run_point(args):
    build, base_config, workload, settings = args
    cmd = command(build, base_config, workload, settings)
    # rvsim-pipe also exits 6 for an unloadable program; that must be an
    # error, never a "skipped" point.
    if not os.path.isfile(cmd[1]):
        raise RuntimeError(f"workload not found: {cmd[1]}")
    p = subprocess.run(cmd, capture_output=True, text=True, check=False)
    if p.returncode == 6 and not p.stdout.strip():
        # Usage/config error before simulating: the config validator rejected
        # this point (e.g. more ways than lines). Recorded, not dropped.
        return "skipped", None, None, p.stderr.strip().splitlines()[0] if p.stderr.strip() else "rc=6", cmd
    if p.returncode != 0:
        raise RuntimeError(f"run failed (rc={p.returncode}): {' '.join(cmd)}\n{p.stderr}")
    stats = json.loads(p.stdout)
    if stats["halt"] != "tohost":
        raise RuntimeError(f"workload did not end normally ({stats['halt']}): {' '.join(cmd)}")
    region, row = counters_from(stats)
    return "ok", region, row, "", cmd


def sweep(spec, build, workers):
    keys, points = expand(spec)
    base = spec.get("base_config", "configs/baseline.cfg")
    jobs = [(build, base, w, s) for w, s in points]
    with concurrent.futures.ProcessPoolExecutor(max_workers=workers) as ex:
        results = list(ex.map(run_point, jobs))  # map preserves input order
    out = io.StringIO()
    fields = ["workload"] + keys + ["status", "region"] + COUNTERS + ["note"]
    w = csv.DictWriter(out, fieldnames=fields, lineterminator="\n")
    w.writeheader()
    commands = []
    for (workload, settings), (status, region, row, note, cmd) in zip(points, results):
        r = {"workload": workload, "status": status, "region": region or "", "note": note}
        for k in keys:
            r[k] = ""
        for k, v in settings:
            r[k] = v
        r.update(row or {c: "" for c in COUNTERS})
        w.writerow(r)
        commands.append(" ".join(cmd))
    return out.getvalue(), commands, len(points), sum(1 for x in results if x[0] == "skipped")


def selftest():
    keys, pts = expand({"workloads": ["w"], "axes": {"a": [1, 2], "b": ["x", "y", "z"]}})
    assert keys == ["a", "b"]
    assert [s for _, s in pts] == [(("a", 1), ("b", "x")), (("a", 1), ("b", "y")), (("a", 1), ("b", "z")),
                                    (("a", 2), ("b", "x")), (("a", 2), ("b", "y")), (("a", 2), ("b", "z"))]
    keys, pts = expand({"workloads": ["w1", "w2"],
                        "groups": [{"axes": {"t": ["s"]}}, {"axes": {"t": ["g"], "h": [2, 4]}}]})
    assert keys == ["t", "h"] and len(pts) == 6
    assert pts[0] == ("w1", (("t", "s"),)) and pts[3] == ("w2", (("t", "s"),))
    cmd = command("B", "configs/x.cfg", "w", (("config", "configs/y.cfg"), ("dcache.assoc", 4)))
    assert cmd == ["B/rvsim-pipe", "B/workloads/w.elf", "--config", "configs/y.cfg",
                   "--set", "dcache.assoc=4", "--stats", "-"]
    assert elf_path("B", "tests/riscv-tests/rv32ui-add") == "B/tests/riscv-tests/rv32ui-add.elf"
    print("sweep selftest: ok")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("spec", nargs="?")
    ap.add_argument("--build", default="build/release")
    ap.add_argument("--out", default="results/phase6/sweeps")
    ap.add_argument("--workers", type=int, default=os.cpu_count() or 1)
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        selftest()
        return 0
    with open(a.spec, encoding="utf-8") as f:
        spec_text = f.read()
    spec = json.loads(spec_text)
    prov = subprocess.run(["bash", "scripts/env_report.sh"], capture_output=True, text=True, check=False).stdout
    start = time.monotonic()
    text, commands, n, skipped = sweep(spec, a.build, a.workers)
    elapsed = time.monotonic() - start
    os.makedirs(a.out, exist_ok=True)
    base = os.path.join(a.out, spec["name"])
    with open(base + ".csv", "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    with open(base + ".commands.txt", "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(commands) + "\n")
    with open(base + ".provenance.txt", "w", encoding="utf-8", newline="\n") as f:
        f.write(f"# Sweep {spec['name']}\ncommand: scripts/sweep.py {a.spec} --build {a.build} --out {a.out} "
                f"--workers {a.workers}\n\n## Provenance\n{prov}\n## Run\npoints: {n} (skipped by the config "
                f"validator: {skipped})\nworkers: {a.workers}\nwall time: {elapsed:.1f} s\n\n## Spec\n{spec_text}")
    print(f"sweep {spec['name']}: {n} points, {skipped} skipped, {a.workers} workers, {elapsed:.1f} s -> {base}.csv")
    return 0


if __name__ == "__main__":
    sys.exit(main())
