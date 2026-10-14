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

## Predictors, caches, DRAM and the CPI stack (Phase 3)
- **Timing-only structures:** caches, predictors and the BTB hold no data, so values always come from the backing store. That guarantees no configuration can change results, but it also means corruption bugs a real cache could have (stale data, a lost write-back) cannot be modelled.
- **Non-speculative global history:** gshare's history register updates when a branch resolves in EX. Up to two younger branches in flight predict with lagged history. Real designs update speculatively and repair on a mispredict. `P3.GshareTrainsTheCounterItPredictedWith` shows the effect.
- **No return-address stack:** without a BTB hit, `JALR` returns always resolve in EX (2 cycles).
- **BTB:** direct-mapped, with full-pc tags (no aliasing, but unrealistically large tags). Allocated only on taken branches and jumps.
- **Blocking caches, no MSHRs:** a miss blocks IF or MEM until the fill completes. In-flight I-cache fills cannot be cancelled, so wrong-path fetch misses cost real time. A core that aborts fills would recover faster.
- **No bus or DRAM contention:** I and D misses are serviced independently and may overlap in time. When both start in the same cycle, the D access is applied to DRAM state first. Real memory systems queue, arbitrate, and suffer read/write turnaround.
- **DRAM model:** open-page policy, per-bank open-row state, fixed hit/closed/miss latencies, row-interleaved mapping. No refresh, bank-level parallelism, tFAW/tRRD limits, or queueing. The latency defaults are modelling parameters, not taken from a datasheet.
- **`FENCE.I` invalidates the whole I-cache.** Real implementations may invalidate selectively.
- **Retire-slot CPI accounting:** each cycle is charged to whatever occupies WB. When a flush replaces bubbles that were already in flight (for example, an I-miss bubble replaced by a mispredict bubble), those cycles are charged to the flushing event. The stack answers "what put a bubble in the retire slot", not "every hazard that was pending". Pipeline-fill cycles count as `base`.
- **The `branch` category includes more than mispredictions:** it also covers ID-redirect bubbles (correctly predicted taken branches without a BTB hit) and `FENCE.I` refetches. The stats JSON breaks these out separately.

## Workloads and RESULTS.md (Phase 4)
- **Two small integer kernels (dot product, GEMM) on a scalar in-order core.** They are not a benchmark suite. Conclusions are about these loops on this model, not about RISC-V cores in general.
- **One data seed and fixed sizes** (dot N=1024; GEMM N=32 and 64; tile T=8). Data values do not affect timing (no data-dependent latency in the model), but sizes and alignment do. The effect of array alignment is shown explicitly (padding experiment), not swept.
- **Arrays are 4 KiB-aligned by construction**, which is the worst case for set conflicts. A real allocator or linker could place them differently.
- **Cold caches at the start of the region of interest.** The ROI-marker store allocates one D-cache line (on its own line, so it evicts no workload data), and the marker store that ends the ROI is counted inside it.
- **C kernels are compiled freestanding with `-fno-builtin`**, so clang cannot call `memset`/`memcpy`. One clang version (recorded in provenance); other versions or GCC may generate different code.
- **No timing noise:** the model is deterministic, so each result is one run. Error bars would be meaningless here. They would matter on hardware.
- **ROI edges: event counts can be off by a few.** The ROI snapshot is taken when a marker store retires in WB. Younger instructions already in flight may have made their cache accesses in that same cycle, so those events land on the other side of the boundary from the stall cycles they cause. Example: 255 instead of 256 D-misses for the dot product on `big` (RESULTS.md §7). Cycle counts and CPI stacks are exact.

## RTL and correlation (Phase 5)
- **RV32I only:** the RTL has no MUL/DIV (as specified), so the contract's MUL/DIV timing (P2 §6.4), the rv32um tests and the Phase 4 workloads are not correlated against RTL.
- **Baseline machine only:** no caches, predictors or DRAM in RTL. Phase 3 timing is checked only by the model's own hand-derived tests.
- **Simulated with Verilator 5.020, never synthesised:** no timing closure, area, power or FPGA results. "Correlation" means cycle-for-cycle behavioural equivalence with the model.
- **Memory contents are not compared:** the three levels are summary, retire trace and per-cycle stage occupancy. A memory-only side effect with no later architectural consequence would go unnoticed (see docs/CORRELATION.md).
- **The testbench supplies memory read data** computed from flops (docs/phase5-design.md §4). It is not a model of a real SRAM interface.
- **Same author for the model and the RTL.** A shared misreading of the contract is guarded against by expectations derived by hand from the contract text, not eliminated.
