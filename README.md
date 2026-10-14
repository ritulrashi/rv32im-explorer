# RV32IM Microarchitecture Explorer

A cycle-accurate, parameterizable in-order RISC-V (RV32IM) pipeline model in C++20. It is verified against an independent golden functional simulator and correlated against SystemVerilog RTL.

**Rule for every number in this repo:** it comes from a command you can rerun, recorded next to the commit hash, machine (`lscpu`) and tool versions that produced it. Anything not measured says "not measured".

## Status

| Phase | Content | Status |
|---|---|---|
| 1 | Golden functional ISS + riscv-tests | **Done.** [results/phase1/](results/phase1/) |
| 2 | Cycle-accurate 5-stage pipeline + lockstep checker + random programs | **Done.** [results/phase2/](results/phase2/) |
| 3 | Branch predictors, caches, DRAM model, CPI stack | not started |
| 4 | Assembly / compiled workloads, RESULTS.md | not started |
| 5 | SystemVerilog RTL correlation | not started |
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
  exec.*             the pipeline's own ALU/branch/load/MUL/DIV semantics (independent of src/iss)
tools/
  rvsim-iss          run a program on the ISS
  rvsim-pipe         run a program on the pipeline (cycles, trace, per-cycle diagram)
  rvsim-lockstep     run both models side by side; stop at the first divergence
  rvgen              seeded, platform-deterministic random program generator (emits assembly)
tests/unit/        GoogleTest: decode, memory, loader, trace, ISS semantics, pipeline timing
tests/riscv-tests-env/  minimal test environment replacing upstream env/p
third_party/riscv-tests official riscv-tests, pinned git submodule
```

Design notes: [Phase 1](docs/phase1-design.md) and [Phase 2 pipeline contract](docs/phase2-pipeline-contract.md) (the RTL will implement the same contract). Known limits: [docs/LIMITATIONS.md](docs/LIMITATIONS.md). Bugs found and how they were detected: [docs/BUGS.md](docs/BUGS.md).

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
ctest --preset asan -L lockstep         # ISS vs pipeline, riscv-tests + 50 random programs

B=build/release
$B/rvsim-iss  $B/tests/riscv-tests/rv32ui-add.elf --trace -        # ISS retire trace
$B/rvsim-pipe $B/tests/riscv-tests/rv32ui-add.elf --diagram -      # per-cycle stage occupancy
$B/rvsim-lockstep $B/tests/riscv-tests/rv32um-div.elf              # lockstep one program
scripts/random_lockstep.sh $B 1 100                                # lockstep random seeds 1-100
$B/rvgen --seed 42 > prog.S                                        # one random program
```

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

Every results file starts with the output of `scripts/env_report.sh`: commit, whether the tree was clean, submodule pin, `lscpu`, and tool versions. Run each script on a clean checkout. A script that runs after another has written a results file will correctly report the tree as dirty.
