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

## Pipeline model (Phase 2)
- **Ideal memory:** every fetch and data access completes in one cycle. Caches and DRAM arrive in Phase 3.
- **Static not-taken prediction only:** every taken branch and every jump costs 2 cycles. Predictors arrive in Phase 3.
- **No MEM→MEM store-data forwarding:** `lw x1; sw x1, …` stalls 1 cycle. A design with that path would not stall.
- **MUL/DIV are non-pipelined and fixed-latency** (defaults 3 and 33, configurable). There is no early-out and no overlap with a following independent MUL. Real implementations vary widely; these defaults are parameters, not claims about a specific chip.
- **Self-modifying code without `FENCE.I` is out of scope.** The pipeline may run a stale instruction it already fetched, while the ISS always fetches fresh. Both are legal per the spec, but lockstep would report a mismatch. The random generator never produces such code.
- **Faults are not precise traps.** A faulting instruction ends the run when it reaches WB. Nothing younger has changed state by then, but there is no trap handler, `mepc` or `mcause`.
- **What lockstep covers:** architectural results only (the retire stream and the halt state). It does **not** check that the cycle counts are right. Cycle timing is checked by the directed timing tests, which use hand-derived expected values, and by the hand-drawn per-cycle diagram test. In Phase 5 it will also be checked against RTL.
- **Random programs are limited in shape:** forward branches, plus bounded loops on a reserved register (x30) with a reserved data base (x31). They exercise hazards and corner-case values well, but are not representative workloads, and never contain recursion, calls/returns, or backward branches other than loop back-edges.
