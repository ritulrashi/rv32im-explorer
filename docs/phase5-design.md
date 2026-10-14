# Phase 5 Design Note: RTL Correlation

Status: **awaiting review**

Goal: a SystemVerilog 5-stage RV32I core that implements the **Phase 2
contract** (`docs/phase2-pipeline-contract.md`, "P2 §n"), a Verilator
testbench, and a correlation flow comparing the RTL against the C++ model
running `configs/baseline.cfg`. Exit: they match exactly, or every deviation
is root-caused in `docs/CORRELATION.md` and fixed in whichever side the
contract says is wrong. **The contract is the referee, not the model.** If
the model disagrees with the contract, the model gets fixed and the fix is
logged.

## 1. What is compared, and how strictly

For every program, three levels, each strictly stronger than the one before:

| Level | RTL output | C++ side | Must match |
|---|---|---|---|
| 1. Total cycles | cycle count when the halting instruction is in WB | `rvsim-pipe --config configs/baseline.cfg` | exactly |
| 2. Retire trace | one line per retired instruction (P1 trace format) | the same model run (and therefore the ISS, via lockstep) | byte-identical |
| 3. **Stage occupancy, every cycle** | pc held by IF, ID, EX, MEM and WB, or the **bubble cause** | `rvsim-pipe --diagram` | byte-identical, cycle by cycle |

Level 3 is the point of the exercise. Total cycles can match while internal
timing differs, with errors cancelling. A per-cycle, per-stage comparison,
including *why* each bubble exists (fill, load-use, redirect, fence.i, fault
flush), cannot hide that.

The diagram format becomes a stable contract artefact:

```
<cycle> <IF> <ID> <EX> <MEM> <WB>
```

Each field is an 8-hex-digit pc, or `-<cause>` for a bubble (`-fill`,
`-load-use`, `-mispredict`, `-fence.i`, `-fault-flush`, `-frozen` for a
frozen IF). Today's `--diagram` output truncates cause names; it will move to
this format. No Phase 2–4 result depends on the diagram format.

## 2. Scope

- **ISA: RV32I + FENCE/FENCE.I, as the spec asks.** M-extension encodings are
  **illegal** in the RTL (it halts with `illegal-instruction`, P2 §6.6). So
  the correlation programs must be RV32I-only, and the model must run them
  with M never executed. Nothing in the contract's M timing (P2 §6.4) is
  exercised; that's listed in LIMITATIONS.
- **No caches or predictors:** the baseline machine, with ideal 1-cycle
  memory, static not-taken, and branches/jumps resolved in EX.
- **Faults are implemented** (illegal, ecall/ebreak, misaligned
  load/store/target): fault flush, frozen fetch, halt at WB without retiring
  (P2 §6.6). They're cheap to add, and the contract is incomplete without
  them.

## 3. RTL structure (`rtl/`)

The decoder, ALU and branch comparator are written from scratch in
SystemVerilog. Nothing is shared with the C++ models, so this is a third
independent implementation of the ISA semantics.

| File | Contents |
|---|---|
| `rvx_pkg.sv` | opcode/funct constants, bubble-cause and fault enums, pipeline-register structs (`if_id_t`, `id_ex_t`, `ex_mem_t`, `mem_wb_t`), the same fields as P2 §4 |
| `rvx_decode.sv` | combinational RV32I decoder: fields, immediate, legality, "reads rs1/rs2", "writes rd" |
| `rvx_alu.sv` | ALU, branch comparator |
| `rvx_regfile.sv` | 31×32 registers; one write port, two read ports with **write-through** (P2 §3.1) |
| `rvx_core.sv` | the five stages, forwarding muxes (P2 §6.1), hazard/control unit with the P2 §6.7 priority, pipeline registers as `always_ff`, next-state logic as `always_comb` |

**Style:** exactly the model's style (P2 §2). All control signals and
next-state values are computed combinationally from the current registers.
Every register updates on one clock edge. Synchronous, active-high reset puts
every pipeline register into a bubble with cause `fill`, and PC at the entry
address (an input).

**Debug and trace ports** (outputs of `rvx_core`):
- **Retire port:** `valid`, pc, insn, rd, value, memory kind/addr/data/size.
- **Halt port:** `halt`, reason, halt pc, tohost value.
- **Stage-view port:** a pc and a valid/cause per stage, for level 3.

These outputs are combinational from registers. Nothing feeds back into the
core.

## 4. Memory interface and the testbench (`tb/`, C++ with Verilator)

The contract (P2 §3.0) needs **combinational reads in the same cycle**: IF
fetches the word at PC, and MEM reads at the EX/MEM address. It also needs a
**write at the clock edge**. The obvious Verilator approach, a DPI-C read
function called from `always_comb`, has a subtle trap. Verilator only
re-evaluates a combinational block when its *inputs* change. A store
followed by a load from the same address in the next cycle leaves the
address unchanged, so the read would return the stale value.

**The design avoids this:** the testbench supplies read data on input ports.
- Both read addresses come straight from **flops**: the PC register, and the
  EX/MEM address.
- **Each cycle:**
  1. evaluate (the clock is low; addresses are settled)
  2. read the backing memory at both addresses in C++ and drive
     `imem_rdata` / `dmem_rdata`
  3. evaluate again (all combinational logic settles)
  4. sample the trace ports, and the write request (`dmem_we`, address, data,
     size)
  5. apply the write to memory, then raise the clock (the edge)
- There's no combinational loop, because read addresses don't depend on read
  data.

**Testbench reuse:** the testbench reuses the project's ELF loader and
sparse memory (`src/common`). They're harness, not semantics, as with
lockstep.

**`rvsim-rtl PROGRAM [--trace FILE] [--diagram FILE] [--max-cycles N]`:**
exit codes and stderr summary as `rvsim-pipe`.

## 5. Correlation flow (`scripts/correlate.sh`)

For each program:
1. run `rvsim-rtl` and `rvsim-pipe --config configs/baseline.cfg`, both with
   `--trace` and `--diagram`
2. compare cycles, then trace, then diagram
3. on the **first** difference, print it: the cycle, the stage, and both
   values with three lines of context
4. keep going over the remaining programs, then summarise

**Program set:**
- **The 41 rv32ui riscv-tests** (`ma_data` excluded, as everywhere). rv32um
  needs M, so it's out of scope.
- **Directed contract programs** (`tests/rtl/*.S`): one per hazard case of
  P2 §6, plus the faults of §6.6. These are the hazard programs from
  `pipeline_timing_test.cpp`, rewritten as assembly. The 14-cycle hand-drawn
  diagram (P2 §10) gets a checked-in expected diagram file, so **the RTL is
  also compared against the hand derivation, not only against the model.**
- **Random RV32I programs:** `rvgen --isa rv32i`, seeds 1–500. This is a new
  flag that excludes MUL/DIV. The default (rv32im) output must stay
  byte-identical for every seed: the flag only changes the draws when it's
  set, which keeps Phase 2 results reproducible. A test checks this.

**Exit criterion:**
- every program matches at all three levels, *or* each deviation has an
  entry in `docs/CORRELATION.md` with root cause, which side was wrong per
  the contract, the fix, and the commit
- `verilator --lint-only -Wall` is clean
- report the number of programs, seeds, and total cycles compared,
  measured, with provenance

## 6. Process when they disagree

1. Find the first differing cycle and stage from the level-3 diff.
2. Read the contract section for that hazard. Decide which side does what
   the text says. If the text is ambiguous, that's a contract bug: clarify
   the contract first (and ask you if it changes any cycle count already
   reported).
3. Fix that side only. Never adjust the other side to agree.
4. Log it in `docs/CORRELATION.md`: program, cycle, symptom, root cause, the
   side at fault, and the fix. Also log it in `docs/BUGS.md`, cross-linked.
5. Re-run the **whole** set, and the model's full Phase 2/3 suites if the
   model changed.

## 7. Tests written before the RTL

- `tests/rtl/*.S`, each with its expected cycle count in a header comment,
  taken from P2's formulas (the same numbers as `pipeline_timing_test.cpp`).
  The script checks them against the RTL as well as the model.
- The hand-drawn 14-cycle diagram as an expected-output file.
- `rvgen --isa rv32i` determinism, and default-output-unchanged tests.

## 8. Risks I'm flagging now

- **Verilator version.** Ubuntu 24.04 ships 5.020. I'll record it in
  provenance. If the CMake `verilate()` integration isn't usable, I'll fall
  back to `verilator --cc --exe --build` from a custom command.
- **The RTL is simulated, not synthesised.** No timing closure, area or FPGA
  results. "Correlation" here means cycle-behaviour equivalence with the
  model, and that's what will be claimed.
- **Same author for model and RTL.** If I misread the contract the same way
  twice, the two would agree on a wrong behaviour. The hand-derived
  expectations (P2 §10 diagram, directed cycle counts) are the guard against
  that. They come from the contract text, not from either implementation.
