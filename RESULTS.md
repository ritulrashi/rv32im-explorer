# RESULTS: Workloads on the RV32IM Microarchitecture Explorer (Phase 4)

**Every number in this document is copied from a generated file** in
[`results/phase4/`](results/phase4/):
- [`tables.md`](results/phase4/tables.md): all cycle tables, CPI stacks,
  instruction mix, comparisons, and hot-loop listings
- [`investigations.md`](results/phase4/investigations.md): the four
  experiments in §5, with every simulator command line
- [`raw.csv`](results/phase4/raw.csv): one row per (workload, config); the JSON
  for each run is under `stats/`

`scripts/check_results_numbers.py` checks that every multi-digit or decimal
number below appears verbatim in those files. Small integers are parameters
(N=64, T=8, "2 cycles"), not measurements.

| | |
|---|---|
| Measured on | commit `053945e`, clean tree. GitHub Codespace, AMD EPYC 7763, 4 vCPU, Ubuntu 24.04.3; clang 18.1.3, riscv64-unknown-elf-gcc 13.2.0 (full provenance in [`run_summary.txt`](results/phase4/run_summary.txt)) |
| Reproduce | `scripts/phase4_run.sh` (about 108 s on that machine), then `scripts/check_results_numbers.py RESULTS.md results/phase4/*.md results/phase4/raw.csv` |
| Validity | 150 of 150 runs (30 workloads × 5 configs): each workload passed its own result check (checksum against an independent Python reference) **and** agreed with the golden ISS in lockstep. 5 of 5 determinism re-runs were byte-identical. |
| Scope | All figures are for the **region of interest** (the kernel only; setup and result checking excluded), on a model, not on hardware. See [LIMITATIONS](docs/LIMITATIONS.md). |

## 1. Findings in one screen

1. **Tiling GEMM made it slower in 8 of the 10 (size, config) points**
   (assembly kernels). On ideal memory it was 15–17% slower (hypothesis
   confirmed). More surprising, it was **about 25% slower on the small cache
   at N=64** (naive/tiled cycle ratio 0.80), the one place it was supposed to
   help most. The cause is **set conflicts**: 4 KiB-aligned matrices in a
   2-way cache. Padding each row by 4 words makes the tiled code **3.74×
   faster**, and then tiling beats naive by 3.32× (§5.1, §5.2).
2. **Making that small cache fully associative did not help the naive
   GEMM; it hurt.** Misses rose from 272,257 to 331,777. The naive kernel's
   problem is capacity: it cycles through more lines than fit, and true LRU
   evicts every line just before its reuse. Associativity can't fix that;
   tiling (plus padding) does (§5.1).
3. **A bimodal predictor was slower than static not-taken** on the tiled +
   unrolled GEMM (1.004× the baseline cycles). Its 2-iteration inner loop
   produces an alternating taken/not-taken branch, which a 2-bit counter gets
   wrong every time. Gshare learns the pattern: 626 mispredicts against
   8,794 (§5.3).
4. **Unrolling ×4 is the most reliable win**: 1.62–1.65× on ideal memory.
   But it does **not** hide MUL latency on this core. The multiplier is
   non-pipelined and blocks EX, so each MUL still costs 2 extra cycles. The
   gain comes from removing branches, load-use stalls and loop overhead (§4).
5. **clang -O3 matched or beat my hand-written assembly.**
   - Dot product: identical to the hand-written loop (both are the 7-instruction loop in `tables.md`).
   - Tiled GEMM-64: **1.94× faster** than my hand-tiled kernel. It fully
     unrolls the 8-iteration tile loop and strength-reduces the addressing.
   - GEMM-32 at -O3: it fully unrolls the 32-iteration k loop, runs out of
     registers, and spills pointers to the stack, costing extra loads and
     load-use stalls (§6).
6. **CPI is not performance.** clang -O0 has about the *same* CPI as -O3
   (1.57 vs 1.55 for GEMM-32 on baseline) but takes **6.17× more cycles**,
   because it executes 28.83 instructions per multiply-accumulate instead of
   4.74 (§6).

## 2. What was measured

| Kernel | Variants | Sizes |
|---|---|---|
| dot product (int32) | asm: `naive`, `unroll4` (4 accumulators). C: same two, clang -O0 / -O3 | N = 1024 (8 KiB) |
| GEMM (int32, row-major) | asm: `naive` (i-j-k), `unroll4`, `tiled` (8×8 blocks), `tiled_unroll4`. C: `naive`, `tiled`, `unroll4` at -O0 / -O3; `naive_pad`, `tiled_pad` at -O3 (rows padded by 4 words) | N = 32 (12 KiB), N = 64 (48 KiB) |

There is deliberately no tiled dot product: a dot product has no data reuse
for tiling to exploit ([design note §1.1](docs/phase4-design.md)).

| Config | Front end | Memory |
|---|---|---|
| `baseline` | static not-taken, no BTB (the Phase 2 machine) | ideal (1 cycle) |
| `bimodal_btb` | bimodal 512 + 64-entry BTB | ideal |
| `gshare_btb` | gshare 1024 / 10-bit history + 64-entry BTB | ideal |
| `small_caches` | bimodal 512 + 64-entry BTB | 1 KiB 2-way L1 I and D, 16 B lines, DRAM |
| `big` | gshare 4096 / 12-bit history + 256-entry BTB | 16 KiB 4-way L1 I and D, 32 B lines, DRAM |

**Reading caution:** `small_caches` and `big` differ from `baseline` in
**both** predictor and memory. For example, `asm-gemm_naive-n32` is faster on
`big` (353,879 cycles) than on `baseline` (406,627). That isn't because
caches beat ideal memory; it's `big`'s predictor. Use the two ideal-memory
configs (`bimodal_btb`, `gshare_btb`) to separate the two effects.

The CPI stack charges every cycle to whatever occupies the WB stage
([Phase 3 design §5](docs/phase3-design.md)): `base` is 1 per retired
instruction, and the other categories are the bubbles each hazard put there.
In the tables, each component is in cycles per retired instruction, so the
components sum to the CPI.

## 3. Hypotheses, written before measuring, and what happened

| # | Hypothesis ([design note §4](docs/phase4-design.md)) | Outcome |
|---|---|---|
| 1 | dot naive on baseline: load-use + MUL latency + a 2-cycle branch per iteration | **Confirmed exactly** (§4) |
| 2 | dot unroll4 removes branch cost and hides load-use **and MUL** latency | **Partly true.** Branch cost and load-use stalls are removed, but MUL latency is not hidden (§4) |
| 3 | Tiled GEMM is slower than naive with ideal memory | **Confirmed**: ratio 0.87 (N=32), 0.85 (N=64) |
| 4 | Tiled beats naive on `small_caches` at both N, and on `big` only at N=64 | **Partly refuted.** `big`: as predicted (0.85 at N=32, 1.17 at N=64). `small_caches`: 1.26 at N=32, but **0.80 at N=64**, slower (§5.1) |
| 5 | 4 KiB alignment may cause set conflicts that tiling alone doesn't fix | **Confirmed, and it is the cause of #4's failure** (§5.1, §5.2) |
| 6 | -O0: many loads/stores per MAC and a high load-use share; -O3 close to the hand-written loop | **Confirmed**, with one surprise: at N=32, -O3 unrolls fully and spills registers (§6) |

(Ratios are cycles(naive) / cycles(variant), so values above 1 mean the
variant is faster. They come from the "Key comparisons" table in
`tables.md`.)

## 4. Dot product: where every cycle goes

`asm-dot_naive-n1024` on `baseline`: CPI 1.714, with stack base 1.000,
load_use 0.143, branch 0.285, muldiv 0.286. Its loop is 7 instructions per
MAC, so per iteration that is:
- 7 base cycles
- 1 load-use stall (`lw t1` immediately followed by `mul t0, t0, t1`)
- 2 cycles for the taken back-edge, mispredicted by static not-taken
- 2 cycles of MUL latency (MUL occupies EX for 3 cycles)

That gives 12 cycles per MAC (12,290 cycles for 1,024 MACs), as predicted.

`asm-dot_unroll4-n1024` (19 instructions per 4 MACs): load_use **0.000**,
branch 0.105, muldiv **0.421**. Per iteration:
- 19 base cycles, plus 2 for the single branch, plus 8 for MUL (4 × 2)
- 29 cycles per 4 MACs, i.e. 7,426 cycles in total: **1.65× faster** on baseline

The load-use stall is gone, because all eight loads are issued before the
first multiply. The MUL cost per MAC is unchanged, which is why muldiv's
*share* rose from 0.286 to 0.421. Four independent accumulators would let a
pipelined multiplier overlap the multiplies. This core's multiplier is
non-pipelined and blocks EX ([contract §6.4](docs/phase2-pipeline-contract.md)),
so independence buys nothing for MUL. That's the part of hypothesis 2 that
was wrong.

**With caches, the speedup shrinks:** 1.18 on `small_caches`, 1.25 on
`big`. Both versions take the same compulsory D-misses, so the fixed
memory time dilutes the core-side gain. CPI moves the "wrong" way here:
unroll4's CPI on `small_caches` is 3.80 against naive's 3.04, yet it is the
faster kernel. Fewer instructions share the same miss cycles.

## 5. GEMM

### 5.1 Tiling, and why it failed on the small cache at N=64

On ideal memory, tiling is pure overhead. `asm-gemm_tiled` executes 9.32
instructions per MAC against naive's 7.41. Its 7-instruction inner loop
accounts for only 75.1% of retired instructions: every 8-MAC tile row
recomputes three addresses. Hence 0.87 (N=32) and 0.85 (N=64) on baseline.

With a cache, the comparison depends on whether the reuse survives:

| asm GEMM | D-misses naive | D-misses tiled | cycles ratio naive/tiled |
|---|---:|---:|---:|
| N=32, `small_caches` | 35,233 | 17,537 | 1.26 |
| N=32, `big` (fits) | 384 | 384 | 0.85 |
| N=64, `small_caches` | 272,257 | 315,393 | **0.80** |
| N=64, `big` | 27,273 | 5,519 | 1.17 |

The N=64, `small_caches` row is the anomaly: tiling *increased* misses. Here's
the geometry:
- **Set index repeats every 512 bytes.** The D-cache is 1 KiB, 2-way, with
  16 B lines, which gives 32 sets.
- **B's tile rows collide.** At N=64 a matrix row is 256 B, so B rows k,
  k+2, k+4 and k+6 map to the same sets. An 8-row B tile needs 4 lines per
  set, and the set has only 2 ways.
- **A and C pile onto the same sets.** All three matrices are 4 KiB-aligned
  (on purpose, as a worst case), so the A and C tiles land there too.
- **Result:** the tile's working set (three 256 B tiles, which would fit in
  1 KiB by capacity) evicts itself.

**Experiment E1** ([`investigations.md`](results/phase4/investigations.md)):
the same 1 KiB capacity, made fully associative (`--set dcache.assoc=64`):

| workload | D-misses 2-way | D-misses fully assoc. |
|---|---:|---:|
| `asm-gemm_tiled-n64` | 315,393 | 17,409 |
| `asm-gemm_naive-n64` | 272,257 | 331,777 |

- **Tiled:** misses drop 18-fold once conflicts are impossible, so tiling's
  problem was conflicts.
- **Naive:** misses *rise*. For each (i, j) the naive kernel walks a 64-line
  column of B plus 16 lines of A's row: 80 distinct lines through a 64-line
  cache, in the same order every time. Under true LRU, a cyclic working set
  just larger than the cache misses on every access. The 2-way cache
  accidentally keeps some lines. So the naive kernel's problem is capacity,
  and only restructuring the loop fixes it.

### 5.2 The fix: pad the rows

**Experiment E2:** the same C kernels (clang -O3), with each row stored with
4 extra words (stride 68 words = 272 B), so consecutive rows walk through
different sets:

| C -O3, N=64, `small_caches` | D-misses | ROI cycles |
|---|---:|---:|
| `gemm_naive` | 272,257 | 9,626,448 |
| `gemm_naive_pad` | 224,891 | 9,313,488 |
| `gemm_tiled` | 316,619 | 10,508,244 |
| `gemm_tiled_pad` | 25,079 | 2,808,524 |

- **Padding helps tiling far more than naive:** padded tiled is **3.74×**
  faster than unpadded tiled.
- **With padding, tiling wins:** padded tiled is **3.32×** faster than
  padded naive.

So the textbook result does hold, but only once the data layout cooperates
with the cache's indexing.

**Padding has a small cost when there's no conflict:** 0.99 (1% slower) on
`baseline` and `big`. It adds a few instructions (1,330,672 vs 1,327,071
retired) and 6% more footprint.

### 5.3 Branch prediction: bimodal loses to static not-taken

| `asm-gemm_tiled_unroll4-n32` | cond mispredicts | branch cycles | ROI cycles |
|---|---:|---:|---:|
| `baseline` (static not-taken) | 8,191 | 16,382 | 313,599 |
| `bimodal_btb` | 8,794 | 17,588 | 314,805 |
| `gshare_btb` | 626 | 1,253 | 298,470 |

- **Bimodal is wrong every time on the inner branch.** With T = 8 and the k
  loop unrolled ×4, the innermost loop runs exactly 2 iterations, so its
  back-edge goes taken, not-taken, taken, not-taken, … A 2-bit counter
  starting at weakly-not-taken flips between 1 and 2 and is wrong every time
  (the same pattern is the `Bimodal.AlternatingPatternDefeatsIt` unit test:
  20 wrong out of 20).
- **Static not-taken loses only half.** It is wrong only on the taken half,
  so bimodal ends up slightly *worse* (1.004× baseline cycles, at both N=32
  and N=64).
- **Gshare learns it.** It indexes with the global history, which
  distinguishes the two outcomes: 626 mispredicts.

Elsewhere, prediction helps as expected: about 16% fewer cycles on
`asm-gemm_naive-n32` (0.844 of baseline) and `asm-dot_naive-n1024` (0.834).

### 5.4 Unrolling GEMM

`asm-gemm_unroll4` cuts instructions per MAC from 7.41 to 5.16 (N=32) and
removes the load-use stall (load_use 0.000). That gives **1.62×** (N=32) and
**1.64×** (N=64) on baseline. Like dot, its MUL cost per MAC doesn't shrink.

On the small cache the gain falls to 1.12 / 1.11. The kernel is dominated by
B's column misses, which unrolling doesn't change (35,233 misses for both
naive and unroll4 at N=32).

## 6. What the compiler did (clang 18.1.3, -O0 vs -O3)

Instruction-mix numbers are from the "Dynamic instruction mix" table.
Listings are from the "Hot code" section of `tables.md`: every pc retired at
least half as often as the hottest one, with counts, disassembled from the
measured ELF.

**-O0 keeps every variable in the stack frame.** GEMM-32's hot code is 28
instructions per MAC:
- `lw` of `i`, `j`, `k` and the pointers from `-12(s0)` … `-36(s0)`
- the accumulator is loaded and stored every iteration (`lw a0,-32(s0)` …
  `sw a0,-32(s0)`)
- 3 unconditional `j` jumps per iteration, each a taken jump that static
  not-taken resolves in EX for 2 cycles

The result is 28.83 instructions, 10.22 loads and 2.13 stores per MAC, and
the highest load-use share of any variant (0.211 CPI).
- **Yet its CPI on baseline is 1.57, essentially the same as -O3's 1.55.**
- **It takes 6.17× more cycles** (1,484,747 against 240,527).

CPI says how well the pipeline digests the instruction stream, not how much
work the stream is.

**-O3 depends on the problem size, because N is a compile-time constant:**

**At N=64:** -O3 naive is the same 7-instruction loop as the hand-written
assembly (`lw, lw, mul, add, addi, addi, bne`), 7.14 vs 7.20 instructions per
MAC.

**At N=32:** clang fully unrolls the 32-iteration k loop into a
150-instruction block per (i, j), at 4.74 instructions per MAC.
- **It runs out of registers.** It keeps pointers to A's row elements in
  stack slots and reloads them: `lw s1,72(sp); lw s1,0(s1)`, a dependent pair.
- **That costs extra loads and stalls:** 2.50 loads per MAC instead of 2.00,
  and a load-use share of 0.112 that the hand-written unrolled kernel doesn't
  have.
- **Net effect:** it's still the fewest instructions per MAC, and 1.69×
  faster than the hand-written naive kernel on baseline.

**`gemm_naive` and `gemm_unroll4` compile to the same code at N=32**
(experiment E4).
- **What the diff shows:** the two `gemm` functions have the same 242
  instructions, and the only differences are the operand order of
  commutative adds (`add s1,s2,s1` vs `add s1,s1,s2`, 21 lines).
- **That's why their statistics are identical in every counter:** forwarding
  treats both source operands the same way.
- **At N=64 the code differs**, and the hand-unrolled source is faster
  (4.89 vs 7.14 instructions per MAC).

**Tiled C at -O3 beats my hand-tiled assembly by 1.94× at N=64** on
baseline. clang fully unrolls the 8-iteration tile loop (37 instructions for
8 MACs, with B addressed at constant offsets `-1024(t6)` … `768(t6)`) and
keeps the row pointers in registers, while my assembly recomputes addresses
for every (i, j). C -O3 tiled is also 1.64× faster than C -O3 naive on
ideal memory. Unrolling, not tiling, is the reason: it executes 5.06 rather
than 7.14 instructions per MAC.

## 7. Caveats specific to these numbers

- **Small error in event counts at the ROI edges.** The begin-marker store
  retires in WB while the first kernel load is already in MEM, so that load's
  cache access is counted before the ROI starts, while its stall cycles count
  inside. Example: `asm-dot_naive-n1024` on `big` shows 255 D-misses where 256
  lines are touched.
  - The end-marker store is the other side of the same effect: it misses on
    `small_caches` (its line was evicted) but hits on `big`. That's why
    `small_caches` shows exactly 512.
  - The error is at most a few events per run. Cycle counts and CPI stacks
    are exact.
- **The main tables mix predictor and memory differences** (§2). Use the
  ideal-memory configs to separate them.
- **These are two integer kernels on a scalar in-order model with blocking
  caches and no prefetcher.** A pipelined multiplier, a prefetcher, or
  non-blocking caches would change several conclusions. Unrolling with
  multiple accumulators, in particular, would start hiding MUL latency.
