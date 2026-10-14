# Phase 2 Design Note: 5-Stage Pipeline Contract

Status: **approved 2026-10-13 (including §11 sharing); implemented. Clarifications added during implementation are listed in §13.**

This document is the **contract** for the cycle-accurate pipeline model
(`src/pipeline`), and later for the SystemVerilog RTL (`rtl/`, Phase 5). Both
must implement exactly what is written here, so that their cycle counts can be
compared exactly. If the model and this document disagree, one of them is a
bug.

## 1. Scope

- Classic in-order 5-stage pipeline: **IF → ID → EX → MEM → WB**, one
  instruction per stage, at most one retirement per cycle.
- Full forwarding into EX, a 1-cycle load-use stall, branch/jump resolution in
  EX with a 2-instruction flush, and multi-cycle non-pipelined MUL and DIV in
  EX.
- **Memory is ideal in Phase 2:** instruction fetch and data access each
  complete in the cycle they are issued. Caches and DRAM come in Phase 3,
  behind the same stage interfaces.
- **Branch prediction in Phase 2 is static not-taken:** IF always fetches
  `pc + 4`. Phase 3 replaces the next-PC logic in IF without changing EX's
  resolution timing.

## 2. Clocking model

The model is written the way RTL is: **every pipeline register is a flop**.
Each cycle:

1. Compute all control signals (stall, flush, redirect, forwarding selects)
   from the **current** contents of the pipeline registers only.
2. Compute every stage's **next** pipeline-register value from current state
   plus those control signals.
3. Commit all next values at once (the clock edge).

No stage ever sees another stage's next value inside the same cycle. This
makes the model independent of the order stages are evaluated in, and maps
one-to-one onto RTL `always_ff` blocks.

**Cycle numbering.** Cycle 1 is the first cycle, in which IF fetches the entry
pc. The run ends in the cycle where the halting instruction is in WB. The
reported cycle count is that cycle's number. So a straight-line program of N
instructions with no hazards takes **N + 4 cycles**.

## 3. State owned by each stage

| Stage | Owns (written only by this stage) | Reads |
|---|---|---|
| IF | `PC` register | instruction memory; redirect and stall signals |
| ID | — (combinational decode and register read) | `IF/ID`, register file |
| EX | `md_cycles` (cycles the current MUL/DIV has spent in EX), `md_a`, `md_b` (operands captured on its first EX cycle) | `ID/EX`, forwarding from `EX/MEM` and `MEM/WB` |
| MEM | — | `EX/MEM`, data memory (loads read, stores write, in this stage) |
| WB | register file `x1..x31` (write port); retire counter | `MEM/WB` |

Each pipeline register is written by the stage on its **left** and read by the
stage on its **right**.

### 3.0 Memory timing

A store in MEM writes memory **at the end of the cycle** (the clock edge).
Any read in that same cycle, whether a load in MEM or the fetch in IF, sees
the old contents. This matches a synchronous-write RAM in RTL.

### 3.1 Register file timing

There is one write port, used by WB, and two read ports, used by ID. **A write
in WB is visible to a read in ID in the same cycle** (internal write-through).
In RTL this is "write first half, read second half", or an explicit bypass
mux. Either way, there is never a WB→ID hazard.

## 4. Pipeline registers (what is latched between stages)

Every pipeline register has a `valid` bit. An invalid entry is a **bubble**.
Every bubble also carries a `bubble_cause` tag saying why it exists. Phase 3
uses that tag to attribute every cycle to exactly one cause in the CPI stack.

| Field | IF/ID | ID/EX | EX/MEM | MEM/WB | Notes |
|---|:-:|:-:|:-:|:-:|---|
| `valid`, `bubble_cause` | ✓ | ✓ | ✓ | ✓ | |
| `pc` | ✓ | ✓ | ✓ | ✓ | |
| `insn` (raw word) | ✓ | ✓ | ✓ | ✓ | needed for the retire record |
| decoded `op, rd, rs1, rs2, imm` | | ✓ | ✓ (op, rd) | ✓ (rd) | decoded once, in ID |
| `rs1_val`, `rs2_val` as read in ID | | ✓ | | | may be stale; EX forwarding corrects them |
| `result` (ALU / link / MUL-DIV value) | | | ✓ | ✓ | |
| `store_data` (forwarded rs2) | | | ✓ | | |
| `mem_addr`, `mem_size`, `mem_kind` | | | ✓ | ✓ | |
| `load_raw` (bytes read, before extension) | | | | ✓ | trace field `data` |
| `exc` (fault kind, or none) | | ✓ | ✓ | ✓ | see §6.6 |

## 5. Stage behaviour (no hazards)

- **IF:** fetch the 32-bit word at `PC`; `IF/ID ← {pc, insn}`;
  `PC ← PC + 4`.
- **ID:** decode with the shared `decode()`; read `rs1` and `rs2` from the
  register file. An illegal encoding sets `exc = IllegalInsn` (§6.6).
- **EX:** select operands through the forwarding muxes (§6.1), then compute
  the ALU result, branch condition and target, `JAL`/`JALR` target and link
  value, load/store address, and misalignment checks. MUL/DIV start here
  (§6.4).
- **MEM:** a load reads memory, a store writes memory, anything else passes
  through.
- **WB:** if `rd != 0` and the op writes rd, write the register file. Emit the
  `RetireRecord` (this is the **retirement point**). If the instruction is the
  `tohost` store, or carries `exc`, the run halts this cycle (§6.6).

## 6. Hazards: exact timing

Notation: columns are cycles. `--` means flushed (turned into a bubble).
`stl` means held in the same stage because of a stall.

### 6.1 Forwarding (no stall)

Forwarding goes into **EX operand selection only**. For each source operand
`rs` (only if the instruction's format reads it, and `rs != 0`):

1. If `EX/MEM.valid` and `EX/MEM` writes `rd == rs`, take `EX/MEM.result`.
2. Otherwise, if `MEM/WB.valid` and `MEM/WB` writes `rd == rs`, take the
   `MEM/WB` value: load data after extension, or `result`.
3. Otherwise, take the value latched in `ID/EX`.

Priority 1 beats 2 because `EX/MEM` holds the younger producer.
`EX/MEM` never holds a load whose consumer is in EX: the load-use stall (§6.2)
makes that impossible, and the model **asserts** it.

```
cycle:          1    2    3    4    5    6
add x1,x2,x3    IF   ID   EX   MEM  WB
sub x4,x1,x5         IF   ID   EX   MEM  WB      x1 from EX/MEM in cycle 4
or  x6,x1,x7              IF   ID   EX   MEM     x1 from MEM/WB in cycle 5
and x8,x1,x9                   IF   ID   EX      x1 from register file (write-through) in cycle 6
```

Store data (`rs2` of `SB/SH/SW`) is forwarded the same way in EX and latched
in `EX/MEM.store_data`. There is **no MEM→MEM forwarding**. See §6.2 for what
that costs.

### 6.2 Load-use stall (1 cycle)

**Condition (evaluated in ID):** `ID/EX` holds a valid, **non-faulting**
load with `rd != 0`, and the instruction in ID reads that register as `rs1`
or `rs2` according to its format. "Non-faulting" includes the misaligned
check that EX does in the same cycle. A faulting load takes the fault path
(§6.6) instead, which keeps the control signals mutually exclusive (§6.7).

**Action:** hold `PC` and `IF/ID`; put a bubble (`bubble_cause = LoadUse`)
into `ID/EX`.

```
cycle:          1    2    3    4    5    6    7
lw  x1,0(x2)    IF   ID   EX   MEM  WB
add x3,x1,x4         IF   ID   stl  EX   MEM  WB    x1 from MEM/WB in cycle 5
next                      IF   stl  ID   EX   MEM ...
```
Cost: +1 cycle per occurrence.
- **Applies to store data too:** `lw x1; sw x1,0(x2)` stalls 1 cycle. MEM→MEM
  forwarding would remove this stall; it is left out to keep the RTL simple,
  and listed in LIMITATIONS.
- **Does not apply to `x0`:** `lw x0, …` followed by a use of `x0` does not
  stall.
- **Format-aware:** only registers the format actually reads count. For
  example, `lui` after `lw x5` never stalls, even if `lui`'s raw rs1 bits
  happen to equal 5. The shared decode forces unused fields to 0, which
  gives this for free.

### 6.3 Branches and jumps: resolved in EX, 2-cycle penalty when taken

All of `BEQ/BNE/BLT/BGE/BLTU/BGEU/JAL/JALR` compute their condition and
target in EX, using forwarded operands.
- **Not taken:** nothing happens; the fall-through instructions already
  fetched are correct.
- **Taken (and every `JAL`/`JALR`):** at the end of that EX cycle,
  `PC ← target`, `IF/ID ← bubble`, `ID/EX ← bubble`, both with
  `bubble_cause = Redirect`.

```
cycle:          1    2    3    4    5    6    7    8
beq (taken)     IF   ID   EX   MEM  WB
pc+4                 IF   ID   --
pc+8                      IF   --
target                         IF   ID   EX   MEM  WB
```
Cost: +2 cycles for every taken branch and every jump. Not-taken branches cost
nothing.

### 6.4 MUL / DIV: multi-cycle, non-pipelined, in EX

Configurable latencies `mul_latency` (default **3**) and `div_latency`
(default **33**). These defaults are modeling parameters: 33 represents a
radix-2 iterative divider (1 setup cycle + 32 iterations). They are not claims
about any particular chip. The latency is **fixed**: no early-out for small
operands or divide-by-zero. That keeps it deterministic and easy to match in
RTL. A latency of L means the instruction occupies EX for exactly L cycles.
- **Cycle 1 in EX:** the operands go through the forwarding muxes as usual
  and are captured into `md_a` and `md_b`. Later cycles use the captured
  values. Without this, a producer could retire while the MUL/DIV is still
  busy, and the stale value read in ID would be used.
- **Cycles 1 … L−1 in EX:** `ex_busy`. `PC`, `IF/ID` and `ID/EX` hold;
  `EX/MEM ← bubble` (`bubble_cause = MulDiv`). MEM and WB keep draining older
  instructions.
- **Cycle L:** the result goes to `EX/MEM` like any ALU result and can be
  forwarded from there to the next instruction.
- `MULH`, `MULHSU` and `MULHU` use `mul_latency`. `REM` and `REMU` use
  `div_latency`.

```
cycle:          1    2    3    4    5    6    7    8      (mul_latency = 3)
mul x1,x2,x3    IF   ID   EX   EX   EX   MEM  WB
add x4,x1,x5         IF   ID   stl  stl  EX   MEM  WB     x1 from EX/MEM in cycle 6
```
Cost: +(L − 1) cycles per MUL/DIV, whether or not anything depends on the
result.

### 6.5 FENCE.I

Treated as a **redirect to `pc + 4`**, resolved in EX like a taken jump
(+2 cycles). An older store is in MEM, or already done, by the end of that
EX cycle, and the refetch happens in the following cycle. So the refetch sees
the store. `FENCE` is a no-op: memory is in-order and there is only one hart.

**Stores to code without `FENCE.I`:** the pipeline may execute the stale
instruction already fetched, while the ISS always fetches fresh. The spec
permits both, but lockstep would report a mismatch. Self-modifying code
without `FENCE.I` is therefore **out of scope**. The random generator never
produces it, and it is listed in LIMITATIONS.

### 6.6 Faults (illegal instruction, misaligned access or target, ECALL, EBREAK)

The ISS halts without retiring the faulting instruction. The pipeline must
match that, and must also make sure **wrong-path faults never count** (for
example, an illegal word fetched after a taken branch).
- Faults are **recorded** where they are detected and **carried** in `exc`:
  - illegal / ECALL / EBREAK: in ID
  - misaligned load/store address: in EX
  - misaligned branch or jump target: in EX, and the redirect is suppressed
- When an instruction with `exc` is in **EX**, at the end of that cycle the
  pipeline flushes `IF/ID` and `ID/EX` and **freezes fetch**: IF supplies
  bubbles from then on. So no younger instruction ever reaches MEM, and no
  wrong-path or post-fault store can write memory.
- A faulting instruction **does not access memory** in MEM.
- When it reaches **WB**, the run halts with that fault reason. It does
  **not** retire and emits no retire record.

### 6.7 Control-signal priority

Each cycle, at most one of these is active. They are mutually exclusive
because each depends on what is in EX, and EX holds one instruction:
`redirect` (EX holds a taken branch, jump or FENCE.I), `fault_flush` (EX
holds an `exc` instruction), `ex_busy` (EX holds an unfinished MUL/DIV), and
`load_use` (EX holds a load and ID depends on it). The model **asserts** this
every cycle. The RTL can then use a simple priority mux without having to
argue that the order is irrelevant.

## 7. Termination

Same rule as the ISS: the run ends when the `tohost` store **retires in WB**.
Its memory write happened in MEM, in the previous cycle. A fault (§6.6), or
`--max-cycles` (deterministic, counted in simulated cycles), also ends the
run. There is no wall-clock limit.

## 8. Lockstep checker

`rvsim-lockstep PROGRAM` loads the program once into **two separate memory
instances** and runs the pipeline. Each time the pipeline retires an
instruction, the checker steps the ISS once and compares the two
`RetireRecord`s **as structs, field by field**. At the end it also compares
halt reason, halt pc and total retired count.

On the **first** divergence it stops and prints:
- the cycle and sequence number
- a field-by-field table with mismatched fields marked, for example:
  ```
  DIVERGENCE at seq 1834, cycle 2291
  field     iss          pipeline
  pc        80000a14     80000a14
  insn      02c5d533     02c5d533   (divu)
  rd        x10          x10
  val       0000000f     00000010   <-- MISMATCH
  ```
- the previous 8 retired records, which are identical in both models by
  construction
- a snapshot of the pipeline: what each stage and pipeline register holds,
  plus that cycle's control signals

Exit code 0 means no divergence. Any divergence gives a non-zero exit code.

## 9. Random program generator (`rvgen`)

- **Seeded and deterministic across platforms.** It uses its own
  `splitmix64`/`xoshiro256**` PRNG and its own range reduction, **not**
  `std::mt19937` with `std::uniform_int_distribution`. The standard does not
  pin down the distribution algorithms, so the same seed gives different
  programs under libstdc++ and libc++. That would break rule 5.
- **Emits assembly (`.S`), assembled by GNU as.** That gives an encoder
  independent of our decoder, and failing programs stay human-readable.
- **Content:**
  - random register initialization
  - a 4 KiB data region with random contents, through a reserved base
    register (`x31`)
  - ALU R/I ops and M ops
  - loads/stores with random aligned offsets inside the region
  - forward branches and jumps (`JAL`, and `auipc`+`JALR`) only
  - bounded backward loops on a reserved counter register (`x30`)
- **Guaranteed to terminate.** Forward-only control flow, plus loops with
  fixed trip counts. The program ends with a pass store to `tohost`.
- **Hazard density knob:** with probability *p*, each source register is
  picked from the last few destination registers. That forces forwarding,
  load-use and MUL→use cases. Profiles vary *p* by seed.
- **Exit criterion run:** the number of programs and the seed range are
  chosen before running and reported exactly. Plan: seeds 1–1000.

## 10. Tests written before the pipeline

Directed timing tests. Each one has the expected cycle count derived by hand
from the diagrams above, and the derivation is written in the test.

| Test | Expected |
|---|---|
| N independent ALU ops | N + 4 |
| Back-to-back dependent ALU chain (EX/MEM and MEM/WB forwarding) | N + 4 |
| Load → dependent use | +1 |
| Load → use two instructions later | +0 |
| Load → store using the loaded value as data | +1 |
| `lw x0` → use of x0 | +0 |
| `lw x5` → `lui x?` (format reads no rs) | +0 |
| Taken branch / not-taken branch | +2 / +0 |
| JAL, JALR | +2 each |
| JALR whose rs1 comes from the immediately preceding load | +1 (load-use) +2 (jump) |
| MUL, DIV with L = 3 / 33 | +2 / +32 |
| MUL → dependent use | +(L−1) only |
| FENCE.I | +2 |
| Illegal word on the wrong path after a taken branch | no halt, normal completion |
| Faulting load followed by a store | the store never writes memory |

There is also a per-cycle stage-occupancy test: for a 10-instruction program
mixing the hazards above, the model's pipeline diagram (pc in each stage, each
cycle) must equal a diagram drawn by hand. That same diagram output will be
compared cycle by cycle against the RTL in Phase 5. That is a much stronger
check than comparing total cycle counts.

**Exit criterion:** zero lockstep mismatches on all 49 riscv-tests plus every
random program (count and seeds reported). All directed timing tests pass
under ASan and Release.

## 11. What is shared between the ISS and the pipeline (rule 4)

| Shared | Why it's acceptable | Common-mode risk and mitigation |
|---|---|---|
| `src/common/decode` | Explicitly allowed by rule 4 | Hand-encoded decode tests; riscv-tests assembled by GNU as |
| `src/common/retire.h` and `trace` | They are the comparison contract, not semantics. Lockstep compares structs, so the formatter is outside the comparison path | Exact-string trace tests |
| **ELF loader and backing memory store (proposed)** | The program image is an *input* to both models, not model behaviour. Each model gets its **own** memory instance. All load/store semantics (sizing, sign extension, alignment, byte lanes) are implemented separately in each model | A loader bug would give both models the same wrong program, but the ISS passing all 49 riscv-tests shows the loader produces a correct image. Memory has its own unit tests |

**Not shared:** the ALU, branch conditions, address generation, load
extension, store masking, MUL/DIV and their corner cases, next-PC logic, and
halt detection. The pipeline gets a fresh implementation of each, in
`src/pipeline/exec.*`.

## 12. Tradeoffs worth defending

| Decision | Alternative | Why |
|---|---|---|
| Resolve branches in EX (2-cycle penalty) | Resolve in ID (1 cycle) | ID resolution needs a comparator and forwarding into ID, which lengthens the critical path. EX resolution is the textbook baseline the Phase 3 predictors improve on |
| No MEM→MEM store-data forwarding | Add it | Simpler RTL. The cost (+1 on load→store-data) is measurable and goes in LIMITATIONS |
| Fixed-latency, non-pipelined MUL/DIV | Pipelined MUL, early-out DIV | Deterministic and easy to match in RTL. Latency is a config knob, so its effect on CPI can still be studied |
| Faults act in EX, retire nothing | Precise traps | There is no trap architecture (Phase 1 decision). This is the minimum that matches the ISS exactly and never lets a wrong-path or post-fault store write memory |
| Model written as flops (current/next latches) | Evaluate stages in reverse order, updating in place | Order-independent and maps 1:1 to RTL. Reverse-order evaluation works, but hides the clock edge, and RTL correlation is the whole point |
| Pipeline diagram as a test oracle | Only total cycle counts | Totals can match while internal timing differs, with errors cancelling out. Per-cycle occupancy cannot hide that |

## 13. Clarifications added during implementation

These were found while deriving the hand-drawn diagram test or the hazard
cases. None of them changes a cycle count that §6 already stated; each pins
down behaviour §6 left implicit.

1. **Memory write timing (§3.0).** A store's write happens at the clock edge,
   so a same-cycle fetch or load sees the old data. Needed to define exactly
   when `FENCE.I` makes a store visible.
2. **MUL/DIV operand capture (§3, §6.4).** Operands are captured into
   `md_a`/`md_b` on the first EX cycle. Without this, a producer that retires
   during a long DIV would leave the stale value read in ID. The
   `MulOperandsAreCapturedOnFirstExCycle` test fails if capture is removed.
3. **Faulting loads never cause a load-use stall (§6.2).** Without this, a
   misaligned load followed by a consumer would raise both `fault_flush` and
   `load_use` in one cycle, violating §6.7.
4. **The halting cycle commits only WB (§7).** When the tohost store retires,
   a younger store that is in MEM in the same cycle must not write memory.
   The ISS never executes it.
