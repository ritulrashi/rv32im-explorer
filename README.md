# RV32IM Microarchitecture Explorer

A cycle-accurate, parameterizable in-order RISC-V (RV32IM) pipeline model in C++20. It is verified against an independent golden functional simulator and correlated against SystemVerilog RTL.

**Rule for every number in this repo:** it comes from a command you can rerun, recorded next to the commit hash, machine (`lscpu`) and tool versions that produced it. Anything not measured says "not measured".

## Status

| Phase | Content | Status |
|---|---|---|
| 1 | Golden functional ISS + riscv-tests | **Done.** [results/phase1/](results/phase1/) |
| 2 | Cycle-accurate 5-stage pipeline + lockstep checker + random programs | **Done.** [results/phase2/](results/phase2/) |
| 3 | Branch predictors, caches, DRAM model, config files, CPI stack | **Done.** [results/phase3/](results/phase3/) |
| 4 | Assembly and clang-compiled workloads, compiler analysis, [RESULTS.md](RESULTS.md) | **Done.** [results/phase4/](results/phase4/) |
| 5 | SystemVerilog RV32I RTL + Verilator correlation ([CORRELATION.md](docs/CORRELATION.md)) | **Done.** [results/phase5/](results/phase5/) |
| 6 | Sweep automation, plots, CI | not started |

## Architecture

```
src/common/        Shared by both models (approved in docs/phase2-pipeline-contract.md §11):
  decode.*           bits -> {op, rd, rs1, rs2, imm}. The ONLY instruction logic shared.
  retire.h, trace.*  retire record, halt reasons, stable text trace (the comparison contract)
  memory.*           sparse little-endian backing store (each model gets its own instance)
  loader.*           hand-written ELF32 loader (PT_LOAD + symtab) and flat-binary loader
src/iss/           Golden ISS: one instruction per step(), no timing
src/pipeline/      Cycle-accurate IF/ID/EX/MEM/WB pipeline, written as flops (current/next latches)
  config.*           machine config: strict `key = value` parser, validation, describe()
  stats*             CPI stack (retire-slot accounting) and JSON stats
  exec.*             the pipeline's own ALU/branch/load/MUL/DIV semantics (independent of src/iss)
src/bpred/         static not-taken, bimodal, gshare direction predictors; BTB (timing only)
src/cache/         tag-only set-associative LRU caches, banked open-page DRAM model, miss penalties
configs/           baseline (= Phase 2), bimodal_btb, gshare_btb, small_caches, big
rtl/               SystemVerilog RV32I core implementing the Phase 2 contract (rvx_pkg.sv, rvx_core.sv)
tb/                Verilator testbench: rvsim-rtl (same trace/diagram formats as rvsim-pipe)
workloads/         Phase 4 kernels: asm/ (hand-written), c/ (clang -O0/-O3), common/ (runtime, ROI markers), gen_data.py
tools/
  rvsim-iss          run a program on the ISS
  rvsim-pipe         run a program on the pipeline (cycles, trace, per-cycle diagram)
  rvsim-lockstep     run both models side by side; stop at the first divergence
  rvgen              seeded, platform-deterministic random program generator (emits assembly)
tests/unit/        GoogleTest: decode, memory, loader, trace, ISS semantics, pipeline timing,
                   config, predictors, caches, DRAM, Phase 3 pipeline integration
tests/riscv-tests-env/  minimal test environment replacing upstream env/p
third_party/riscv-tests official riscv-tests, pinned git submodule
```

Design notes: [Phase 1](docs/phase1-design.md), [Phase 2 pipeline contract](docs/phase2-pipeline-contract.md) (implemented by both the C++ model and the RTL), [Phase 3 predictors/caches/CPI stack](docs/phase3-design.md), [Phase 4 workloads](docs/phase4-design.md), [Phase 5 RTL correlation](docs/phase5-design.md) and its [log](docs/CORRELATION.md). Known limits: [docs/LIMITATIONS.md](docs/LIMITATIONS.md). Bugs found and how they were detected: [docs/BUGS.md](docs/BUGS.md).

## Build

The reference environment is the repo's dev container (GitHub Codespaces, Ubuntu 24.04). `.devcontainer/setup.sh` installs the toolchain.

```bash
git clone --recursive https://github.com/ritulrashi/rv32im-explorer
cd rv32im-explorer
cmake --preset asan     && cmake --build --preset asan     # ASan+UBSan, for correctness runs
cmake --preset release  && cmake --build --preset release  # -O3, for benchmarks
```

## Run

```bash
ctest --preset asan                     # everything below
ctest --preset asan -L riscv-tests      # ISS on rv32ui/rv32um
ctest --preset asan -L pipeline         # pipeline on rv32ui/rv32um
ctest --preset asan -L lockstep         # ISS vs pipeline: riscv-tests, random programs, every config
ctest --preset asan -L regression       # baseline config reproduces the Phase 2 cycle counts

B=build/release
$B/rvsim-iss  $B/tests/riscv-tests/rv32ui-add.elf --trace -        # ISS retire trace
$B/rvsim-pipe $B/tests/riscv-tests/rv32ui-add.elf --diagram -      # per-cycle stage occupancy
$B/rvsim-lockstep $B/tests/riscv-tests/rv32um-div.elf              # lockstep one program
scripts/random_lockstep.sh $B 1 100                                # lockstep random seeds 1-100
$B/rvgen --seed 42 > prog.S                                        # one random program

# Phase 3: any machine config (+ overrides), with JSON stats: cycles, CPI stack,
# branch/predictor, cache and DRAM counters. Integers only; ratios are left to the reader.
$B/rvsim-pipe prog.elf --config configs/small_caches.cfg --set bpred.type=gshare --stats -
$B/rvsim-lockstep prog.elf --config configs/big.cfg                # lockstep must hold under every config

# Phase 5: the RTL core (baseline machine), same trace/diagram formats as rvsim-pipe
$B/rvsim-rtl $B/tests/rtl/t26_diagram.elf --diagram -
scripts/correlate.sh $B /tmp/corr 1 100       # RTL vs model: directed + rv32ui + rvgen --isa rv32i seeds 1-100
```

Config files are `key = value` text (see [configs/](configs/) and [docs/phase3-design.md §1](docs/phase3-design.md)). Unknown keys and impossible geometries are errors. The stats JSON always echoes the fully resolved config.

Exit codes for `rvsim-iss` and `rvsim-pipe`: 0 pass, 1 fail (tohost value ≠ 1), 2 illegal instruction, 3 misaligned access, 4 ecall/ebreak, 5 instruction/cycle budget exhausted, 6 usage/load error. `rvsim-lockstep`: 0 the models agree, 1 divergence (a report is printed), 2 did not finish, 6 usage/load error.

Trace format (one line per retired instruction):
```
<seq> pc=<hex8> insn=<hex8> rd=<xNN|-> val=<hex8|-> mem=<R|W|-> addr=<hex8|-> data=<hex8|-> sz=<1|2|4|->
```

## Reproduce the results

| Result | Command | Output |
|---|---|---|
| Phase 1 exit criterion (riscv-tests pass count, under ASan and Release) | `scripts/phase1_exit.sh` | `results/phase1/exit_criterion.txt` |
| Phase 1 harness sanity check (injected ISS bugs are detected) | `scripts/phase1_mutation_check.sh` | `results/phase1/mutation_check.txt` |
| Phase 2 exit criterion (lockstep on riscv-tests + seeds 1-1000, timing tests, determinism) | `scripts/phase2_exit.sh` | `results/phase2/exit_criterion.txt` |
| Phase 2 lockstep sanity check (injected pipeline bugs are detected) | `scripts/phase2_mutation_check.sh` | `results/phase2/mutation_check.txt` |
| Phase 3 exit criterion (directed tests, Phase 2 regression, CPI sum, lockstep under 5 configs) | `scripts/phase3_exit.sh` | `results/phase3/exit_criterion.txt` |
| Phase 3 sanity check (injected timing bugs are detected) | `scripts/phase3_mutation_check.sh` | `results/phase3/mutation_check.txt` |
| Phase 4: every workload × every config, tables, investigations, and the check that every number in [RESULTS.md](RESULTS.md) is generated | `scripts/phase4_run.sh` | `results/phase4/` (`tables.md`, `investigations.md`, `raw.csv`, `stats/`, `results_check.txt`) |
| Phase 5 exit criterion (RTL vs model at three levels on directed + rv32ui + random rv32i programs; lint) | `scripts/phase5_exit.sh` | `results/phase5/exit_criterion.txt`, `programs_*.txt` |
| Phase 5 sanity check (injected RTL bugs are caught by correlation) | `scripts/phase5_mutation_check.sh` | `results/phase5/mutation_check.txt` |

Every results file starts with the output of `scripts/env_report.sh`: commit, whether the tree was clean, submodule pin, `lscpu`, and tool versions. Run each script on a clean checkout. A script that runs after another has written a results file will correctly report the tree as dirty.
