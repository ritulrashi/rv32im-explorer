# Limitations

What this project does **not** model or verify. Updated every phase.

## ISA coverage (Phase 1, golden ISS)
- **RV32IM, unprivileged only.** No CSRs (Zicsr), no traps or exceptions, no interrupts, no privilege modes, no compressed (C) instructions, no atomics (A), no floating point.
- **Exceptions halt the simulator** instead of trapping: illegal instruction, misaligned fetch/load/store, `ECALL`, `EBREAK`. Each has a distinct exit code. There is no architectural trap state to compare between models.
- **Misaligned data access halts.** The spec allows either hardware support or a trap; we do neither. `rv32ui-ma_data` is therefore excluded (it is listed by CMake at configure time and in `results/phase1/exit_criterion.txt`).
- **`FENCE` and `FENCE.I` are no-ops in the ISS.** The ISS re-reads memory on every fetch, so self-modifying code is always coherent there. That is *not* a statement about the pipeline (Phase 2).
- **Memory is a flat, sparse 4 GiB space with no MMIO** apart from the `tohost` store that ends a run. Every address is readable; unwritten memory reads as zero. There is no bus-error model.

## Verification
- **riscv-tests run under a custom minimal environment** (`tests/riscv-tests-env/riscv_test.h`), not upstream `env/p`. The test bodies and `test_macros.h` are unmodified upstream files from the pinned submodule. Upstream's machine-mode setup and trap-based pass/fail path are not exercised.
- **The riscv-tests are directed and fairly small.** Passing them shows each instruction works on the cases they cover. It is not exhaustive. Edge cases they miss are partly covered by our own unit tests (`tests/unit/iss_test.cpp`).
- **Decode is shared between the ISS and the pipeline model** (rule 4 allows exactly this). A decode bug would be common-mode and invisible to lockstep comparison. Mitigations: decode unit tests use hand-encoded words from the spec, and the test encoder is itself checked against hand-computed literals. The risk is reduced, not eliminated.
- **The test encoder (`tests/unit/encode.h`) and the decoder were written by the same author.** A shared misreading of the spec would pass both. The literal encodings in `Encoder.MatchesHandComputedLiterals`, and the riscv-tests (assembled by GNU as, an independent encoder), are the independent checks.
