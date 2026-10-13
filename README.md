# RV32IM Microarchitecture Explorer

A cycle-accurate, parameterizable in-order RISC-V (RV32IM) pipeline model in C++20. It is verified against an independent golden functional simulator and correlated against SystemVerilog RTL.

**Rule for every number in this repo:** it comes from a command you can rerun, recorded next to the commit hash, machine (`lscpu`) and tool versions that produced it. Anything not measured says "not measured".

## Status

| Phase | Content | Status |
|---|---|---|
| 1 | Golden functional ISS + riscv-tests | **Done.** See [results/phase1/exit_criterion.txt](results/phase1/exit_criterion.txt) |
| 2 | Cycle-accurate 5-stage pipeline + lockstep checker | not started |
| 3 | Branch predictors, caches, DRAM model, CPI stack | not started |
| 4 | Assembly / compiled workloads, RESULTS.md | not started |
| 5 | SystemVerilog RTL correlation | not started |
| 6 | Sweep automation, plots, CI | not started |

## Architecture (so far)

```
src/common/decode.{h,cpp}   bits -> {op, rd, rs1, rs2, imm}. The ONLY code shared by ISS and pipeline.
src/iss/memory.*            sparse little-endian memory, 4 KiB pages, unwritten = 0
src/iss/iss.*               golden ISS: one instruction per step(), no timing
src/iss/loader.*            hand-written ELF32 loader (PT_LOAD + symtab) and flat-binary loader
src/iss/retire.h, trace.*   retire record + stable text trace (the lockstep contract)
tools/iss_main.cpp          rvsim-iss command-line driver
tests/unit/                 GoogleTest: decode, memory, ISS semantics, loader, trace
tests/riscv-tests-env/      minimal test environment replacing upstream env/p
third_party/riscv-tests     official riscv-tests, pinned git submodule
```

Design notes: [docs/phase1-design.md](docs/phase1-design.md). Known limits: [docs/LIMITATIONS.md](docs/LIMITATIONS.md). Bugs found and how they were detected: [docs/BUGS.md](docs/BUGS.md).

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
ctest --preset asan                     # unit tests + rv32ui/rv32um riscv-tests + determinism check
ctest --preset asan -L riscv-tests      # just the riscv-tests

# Run one program, writing the retire trace to stdout:
build/release/rvsim-iss build/release/tests/riscv-tests/rv32ui-add.elf --trace -
```

`rvsim-iss` exit codes: 0 pass, 1 fail (tohost value ≠ 1), 2 illegal instruction, 3 misaligned access, 4 ecall/ebreak, 5 instruction budget exhausted, 6 usage/load error.

Trace format (one line per retired instruction):
```
<seq> pc=<hex8> insn=<hex8> rd=<xNN|-> val=<hex8|-> mem=<R|W|-> addr=<hex8|-> data=<hex8|-> sz=<1|2|4|->
```

## Reproduce the results

| Result | Command | Output |
|---|---|---|
| Phase 1 exit criterion (riscv-tests pass count, under ASan and Release) | `scripts/phase1_exit.sh` | `results/phase1/exit_criterion.txt` |
| Phase 1 harness sanity check (injected ISS bugs are detected) | `scripts/phase1_mutation_check.sh` | `results/phase1/mutation_check.txt` |

Every results file starts with the output of `scripts/env_report.sh`: commit, whether the tree was clean, submodule pin, `lscpu`, and tool versions.
