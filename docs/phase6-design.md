# Phase 6 Design Note: Sweeps, Plots, CI

Status: **awaiting review**

Goal: turn the model into a tool. That means:
- sweep configs × workloads in parallel into CSV
- plot CPI stacks and predictor and cache sweeps from that CSV
- run the full regression on every push with GitHub Actions
- **report the total regression runtime, measured**

## 1. Sweep tool: `scripts/sweep.py` (Python standard library only)

**Input:** a JSON sweep spec, checked into `sweeps/`:

```json
{
  "name": "dcache_size_assoc",
  "base_config": "configs/small_caches.cfg",
  "workloads": ["asm-gemm_naive-n64", "asm-gemm_tiled-n64", "c-O3-gemm_tiled_pad-n64", "asm-dot_naive-n1024"],
  "axes": {
    "dcache.size_bytes": [256, 512, 1024, 2048, 4096, 8192, 16384],
    "dcache.assoc": [1, 2, 4, 8]
  }
}
```

- **Expansion:** every point of the Cartesian product of the axes, for every
  workload, becomes one `rvsim-pipe --config BASE --set k=v ... --stats`
  run.
  - Points the config validator rejects (e.g. more ways than lines) are
    **recorded as skipped**, with the validator's message, not silently
    dropped.
- **Parallelism:** a process pool with `os.cpu_count()` workers. Each run is
  an independent subprocess.
- **Results are deterministic**, so rows are sorted (workload, then axes in
  spec order), and the CSV is byte-identical whatever the worker count or
  scheduling. A test checks this.
- **Output:** `results/phase6/sweeps/<name>.csv`. One row per point: the
  workload, every swept key, then the ROI counters as in Phase 4 (cycles,
  retired, the six CPI-stack components, D/I misses, mispredicts, loads and
  stores). Integers only.
- **Provenance sidecar:** `<name>.provenance.txt` holds the `env_report`
  output, the spec file, the exact command line, the worker count and the
  wall time.
- **Run log:** every simulator command line goes into `<name>.commands.txt`,
  so any single point can be rerun by hand.

**Planned sweeps** (specs in `sweeps/`):

| Sweep | Axes | Workloads | Why |
|---|---|---|---|
| `cpi_stacks` | the 5 named configs | all 30 Phase 4 workloads | the CPI stack bars |
| `bpred` | type {static_nt, bimodal, gshare} × entries {16, 64, 256, 1024, 4096} × BTB {0, 64}; ideal memory | branchy subset (dot, GEMM naive/tiled/tiled_unroll4, C -O0/-O3) | predictor accuracy vs table size; is the bimodal pathology of RESULTS §5.3 size-independent? |
| `dcache` | size {256 … 16384} × assoc {1, 2, 4, 8}, line 16 | GEMM-64 naive / tiled / tiled_pad, dot | miss-rate curves; does associativity alone fix tiled GEMM's conflicts (RESULTS §5.1)? |

That's roughly 150 + 330 + 112 runs. Each takes well under a second on
Release, so I expect a few minutes in total. The actual time will be
measured and reported.

## 2. Plots: `scripts/plot.py` (matplotlib)

- **Input is only the sweep CSVs**, never the simulator directly.
- **Output** is `results/phase6/plots/*.png` plus a generated
  `results/phase6/PLOTS.md` listing every figure with its source CSV and
  command.
- **Planned figures:**
  1. **CPI stack bars:** one stacked bar per (workload, config) in the six
     categories, grouped by workload.
  2. **Predictor sweep:** mispredicts per 1,000 conditional branches vs table
     size, one line per predictor type, one panel per workload.
  3. **D-cache sweep:** D-misses per 1,000 accesses vs capacity, one line per
     associativity, one panel per workload, with the cycle count on a second
     panel row.
- **matplotlib comes from Ubuntu's package** (`python3-matplotlib`, pinned by
  the distro), not from pip. Its version goes into provenance.
- **Plots are not byte-compared.** Rendering can differ between machines.
  The CSV is the result; the PNG is a picture of it.

## 3. CI: `.github/workflows/regression.yml`

**Runner:** `ubuntu-24.04`, the same distro as the dev container. Packages
are installed by the same `.devcontainer/setup.sh`, so CI and development
use identical tool versions. Submodules are checked out recursively.

**Splitting the work.** The full suite under ASan takes about 10 minutes on
4 cores, mostly the 64×64 GEMM workloads at `-O0` (the slowest single test
is about 58 s). So:

| Trigger | Job | What runs |
|---|---|---|
| every push and PR | `regression` | **ASan build:** every CTest label *except* `workloads` (golden ISS tests, pipeline timing, lockstep, random seeds, CPI sum, Phase 2 regression, configs, RTL lint and correlation smoke, determinism). **Release build:** the `workloads` label (all 30 workloads pass + lockstep), plus `correlate.sh` on rvgen seeds 1–100 |
| nightly (cron) and manual | `full` | everything under ASan, including the workloads, plus the Phase 2/3/5 exit scripts |

So every push runs the complete regression the spec asks for: golden tests,
lockstep, random seeds, CPI-sum assertions, and RTL correlation. Each part
runs under at least one of the two builds, and the ASan-only coverage of the
heavy workloads is moved to the nightly job.

**Failure artefacts:** CTest logs, plus `correlate.sh`'s kept mismatch
artefacts, uploaded with `actions/upload-artifact`.

## 4. Measuring "total regression runtime"

Two separately labelled numbers, never mixed:
1. **GitHub Actions:** wall time of the `regression` job on the push of the
   Phase 6 commit, read from `gh run view --json` (job start and end), with
   the runner type recorded.
2. **Codespace:** `scripts/phase6_regression.sh` runs exactly the CI steps
   locally, with `date +%s` around each step. It writes a step-by-step timing
   table, with provenance, to `results/phase6/regression_runtime.txt`.

**A cost you should know about:** the repo is **private**.
- Private repos on GitHub Free get 2,000 Actions minutes a month on 2-vCPU
  runners.
- Public repos get unlimited minutes on 4-vCPU runners.

I estimate the push job at 15–25 minutes on a private runner (an estimate,
to be replaced by the measurement), so roughly 80–130 pushes a month. A
public repo would be faster and free. Also, the measured CI runtime depends
on which runner type you get, so I'll record it.

## 5. Tests written before the tools

- `sweep.py --selftest`:
  - spec expansion (counts, order, skipped invalid points), against a
    hand-written expected list for a 2 × 3 spec
  - CSV column set
- **Determinism:** a small sweep run with 1 worker and with 4 workers must
  give byte-identical CSVs (CTest).
- **CPI sum:** `sweep.py` itself rejects any row whose CPI stack doesn't sum
  to its cycles.
- **CI checks itself:** the workflow runs `sweep.py` on a tiny smoke spec, so
  the tooling is regression-tested too.

## 6. Exit criterion

- **The sweeps** run on a clean checkout, with their CSVs, provenance and
  plots committed.
- **CI is green** on the Phase 6 push.
- **Total regression runtime** is reported both ways (§4), measured.
