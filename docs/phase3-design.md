# Phase 3 Design Note: Microarchitecture Knobs and the CPI Stack

Status: **approved 2026-10-13; implemented. Clarifications from implementation in §9.**

Phase 3 adds branch prediction (static not-taken, bimodal, gshare, optional
BTB), L1 instruction and data caches, a banked DRAM latency model, a
runtime configuration file, and cycle attribution (a CPI stack). This note
extends the Phase 2 contract; section numbers like "P2 §6.3" refer to
`docs/phase2-pipeline-contract.md`.

## 0. Three principles

1. **The baseline config reproduces Phase 2 exactly.** `configs/baseline.cfg`
   (static not-taken, no BTB, caches off) must give the same cycle count as
   Phase 2 on every program. Phase 2's results file records the cycle counts
   for all 49 riscv-tests, and those become a regression oracle. The Phase 5
   RTL implements this baseline only.
2. **Timing-only structures.** Caches, predictors, the BTB and DRAM hold no
   data. They decide *how many cycles* things take. Values always come from
   the backing store, exactly as in Phase 2. So no knob can change an
   architectural result, and lockstep must still show zero mismatches under
   every config. That makes lockstep a strong check of the new
   redirect/stall logic. (The trade-off is listed in §8.)
3. **Every cycle has exactly one cause, by construction** (§5).

## 1. Configuration file

A plain `key = value` text format with `#` comments. There are no
dependencies, it is easy for the Phase 6 Python sweeps to generate, and it
diffs well in git.

```
# configs/baseline.cfg: reproduces Phase 2
core.mul_latency     = 3
core.div_latency     = 33
bpred.type           = static_nt    # static_nt | bimodal | gshare
bpred.entries        = 512          # 2-bit counters (power of 2)
bpred.history_bits   = 8            # gshare only, <= log2(entries)
btb.entries          = 0            # 0 = no BTB (direct-mapped, full tags)
icache.enabled       = false
icache.size_bytes    = 4096
icache.assoc         = 2
icache.line_bytes    = 32
dcache.enabled       = false
dcache.size_bytes    = 4096
dcache.assoc         = 2
dcache.line_bytes    = 32
dram.banks           = 4
dram.row_bytes       = 2048
dram.row_hit_cycles  = 20           # open row matches
dram.row_closed_cycles = 30         # bank has no open row
dram.row_miss_cycles = 40           # different row open: precharge + activate + CAS
dram.bus_bytes_per_cycle = 8        # line transfer time = line_bytes / this
```

- **Unknown keys are an error**, not a warning, so a typo in a sweep can't
  silently fall back to a default.
- **Sizes are validated:** powers of two, and the associativity and line size
  must fit the cache.
- Tools take `--config FILE` plus repeatable `--set key=value` overrides.
  Every stats output starts with the **fully resolved config**, defaults
  included, so a result always states the exact machine that produced it.

The DRAM numbers are modelling parameters chosen to give a realistic
*ratio* of hit, closed and miss latency. They are not taken from a DDR
datasheet.

## 2. Branch prediction

### 2.1 Structures (`src/bpred`)

| Structure | State | Lookup | Update |
|---|---|---|---|
| `static_nt` | none | always not-taken | — |
| `bimodal` | `entries` × 2-bit counters, initialised to 01 (weakly not-taken) | index = `(pc >> 2) & (entries-1)` | conditional branches only, at resolution |
| `gshare` | the same counters, plus a global history register (GHR) of `history_bits` bits, initialised to 0 | index = `((pc >> 2) ^ GHR) & (entries-1)` | counters as bimodal; then `GHR = (GHR << 1 \| taken) & mask` |
| BTB | `entries` direct-mapped slots of {valid, full pc tag, target, kind ∈ {cond, jal, jalr}} | index = `(pc >> 2) & (entries-1)`, hit if valid and tag matches | allocated or overwritten when a branch is **taken** or a jump executes |

The **GHR is updated at resolution, not speculatively.** That is simpler,
deterministic, and exactly reproducible in RTL. The cost: up to two younger
branches already in flight see a history that lags. That is a real accuracy
loss compared with speculative history, and it is listed in LIMITATIONS.

### 2.2 Where predictions are used (pipeline timing)

Every instruction carries a **predicted next pc** (`pred_npc`) from IF
through ID to EX. The direction predictor and the BTB are read **in IF**.
Their results travel in `IF/ID`.

| Stage | Action |
|---|---|
| **IF** | If the BTB hits: for `jal`/`jalr` kinds, predict taken to the BTB target; for `cond`, use the direction predictor. A taken prediction means the next fetch is from the BTB target, at **0 bubbles**. Otherwise fetch `pc + 4`. |
| **ID** (only when no BTB redirect happened, and only in a cycle where the instruction advances to EX) | If it is a `JAL`, or a conditional branch predicted taken, **redirect fetch to `pc + imm`, costing 1 bubble.** If IF used a BTB target that doesn't match the decoded target, or the instruction isn't a branch at all, correct it, also for 1 bubble. `JALR` targets are unknown in ID, so without a BTB hit they resolve in EX. |
| **EX** | Resolve taken/not-taken and the target (P2 §6.3). If the taken bit differs from the prediction, or both say taken and the targets differ, that is a **mispredict** (see §9.1: not a next-pc comparison): flush `IF/ID` and `ID/EX` and redirect, costing 2 bubbles, exactly as in P2. Update the predictor and BTB in the cycle the branch leaves EX. |

**Why ID redirect exists:** the prompt makes the BTB optional. Without one, a
taken prediction cannot be used in IF, because the target isn't known yet.
Redirecting from ID is the textbook way to make bimodal and gshare
worthwhile with no BTB.

**Static not-taken with no BTB:** `pred_npc = pc + 4`, there are no ID
redirects, every taken branch and jump costs 2 cycles, and `FENCE.I` still
redirects in EX. **That is exactly Phase 2.**

**Visibility:** structures update at the clock edge. A lookup in the same
cycle as an update sees the old state, the same rule as memory (P2 §3.0).

### 2.3 Worked example (this becomes a directed test)

```
      li   x1, 10
L:    addi x1, x1, -1
      bnez x1, L        # taken 9 times, then not-taken once
```

| Config | Hand derivation of branch-cost cycles | Total |
|---|---|---|
| static_nt, no BTB | 9 taken × 2 (EX mispredict) | **18** |
| bimodal, no BTB | iter 1: counter 01 says NT, actually T: 2. Iters 2–9: counter ≥ 10 says T, ID redirect: 8 × 1. Iter 10: says T, actually NT: 2 | **12** |
| bimodal + 8-entry BTB | iter 1: BTB miss, NT: 2 (BTB allocated, counter → 10). Iters 2–9: BTB hit and T: 0. Iter 10: 2 | **4** |

Each update becomes visible in time for the next iteration's fetch. I checked
this against the cycle diagram: the branch from iteration k leaves EX in the
same cycle that iteration k+1's branch is fetched, so it is visible from
iteration k+2 at the latest. For this pattern that doesn't change any
prediction. The test file will carry the cycle-by-cycle derivation.

## 3. Caches (`src/cache`)

### 3.1 Structure

- A generic set-associative cache holding **tags only**: valid, dirty, tag,
  and an LRU stamp per line.
  - `sets = size / (assoc × line)`
  - `index = (addr / line) % sets`
  - `tag = addr / (line × sets)`
- **True LRU**, using a per-line "last used" counter: a cycle-independent
  64-bit access counter, so it is deterministic. On a miss, an invalid way is
  filled first, then the LRU way.
- **D-cache:** write-back, write-allocate. A store hit sets dirty. A store
  miss allocates the line, then sets dirty. Evicting a dirty line causes a
  write-back.
- **I-cache:** read-only and never dirty.
- **`FENCE.I` invalidates the whole I-cache**, which costs refetch misses.
  The D-cache needs no action: the caches are timing-only, so I and D are
  automatically coherent through the backing store.

### 3.2 Miss penalty

```
P = [dirty victim ? dram(write, victim_line) : 0]
  + dram(read, missing_line)
  + line_bytes / bus_bytes_per_cycle
```

Both DRAM accesses update the row-buffer state, write-back first. A hit
costs nothing extra: the 1-cycle hit is part of the IF or MEM stage, as in
Phase 2.

### 3.3 Pipeline timing

**I-cache miss** (detected in IF at cycle t, penalty P):
- During cycles t … t+P−1, IF supplies bubbles (`cause = ICache`).
- At t+P, the fetch completes.
- **Total cost: P.**

**D-cache miss** (detected in MEM at cycle t, penalty P):
- The instruction stays in MEM for cycles t … t+P−1.
- `MEM/WB` gets `DCache` bubbles. Every register upstream (`PC`, `IF/ID`,
  `ID/EX`, `EX/MEM`) holds.
- At t+P the access completes and the instruction moves to `MEM/WB`.
- **Total cost: P.**

**Interactions:**
- **Memory-side timers keep running while the pipeline is frozen.** An
  I-miss countdown continues during a D-miss, MUL/DIV, or load-use hold. A
  fetch that completes while IF can't hand it over waits in IF. A MUL/DIV
  likewise keeps counting during a D-miss stall and completes when EX can
  advance.
- **An outstanding I-cache fill cannot be cancelled.** A redirect that
  arrives during an I-miss is recorded, and fetch from the new target starts
  when the fill completes. Wrong-path I-misses therefore really cost cycles
  and pollute the cache. Real in-order cores without fill abort behave the
  same way.
- **No contention:** I and D misses are serviced independently. If both
  start in the same cycle, the D-miss is applied to DRAM state first, being
  the older instruction. That order is fixed, so results are deterministic.

### 3.4 Control priority (replaces P2 §6.7)

Each cycle, at most one event *takes effect*, in this order:

1. **`mem_stall`:** MEM is waiting on a D-miss. Everything upstream holds.
   EX's conditions are re-evaluated once EX can advance.
2. **The EX events from P2 §6.7**, still mutually exclusive among
   themselves: `redirect` (now meaning *mispredict*), `fault_flush`,
   `ex_busy`, `load_use`.
3. **`id_redirect`**, only if the instruction in ID advances this cycle.
4. **The IF I-miss wait.**

A lower-priority event is postponed, never lost. The model asserts this, for
example that a redirect is never applied twice for one branch.

## 4. DRAM (`src/cache/dram`)

- `banks` banks, each with one open-row register (open-page policy). At
  reset, every bank is closed.
- **Address mapping (row-interleaved):**
  - `bank = (addr / row_bytes) % banks`
  - `row = addr / (row_bytes × banks)`
- **Latency per access:** `row_hit_cycles` if the open row matches,
  `row_closed_cycles` if the bank has no open row, `row_miss_cycles`
  otherwise. Afterwards, the accessed row is open.
- Reads and writes cost the same.
- **Not modelled:** refresh, bank-level parallelism, queueing, read/write
  turnaround, timing constraints across banks. Listed in LIMITATIONS.

**Worked example (I-cache only, 16-byte lines, 8 B/cycle bus, cold start):**
32 straight-line instructions occupy 8 lines, all in one DRAM row.
- First line: row closed, 30 + 2 = **32**.
- Each of the next 7: row hit, 20 + 2 = **22**.
- I-cache cycles = 32 + 7 × 22 = **186**.

## 5. Cycle attribution (CPI stack)

**Retire-slot accounting.** Every cycle, WB holds exactly one thing:
- **A retiring instruction:** the cycle counts as `base`.
- **A bubble:** the cycle counts toward the bubble's cause. Bubbles carry
  their cause from the stage that created them (P2 §4).

| CPI stack category | Bubble causes counted there |
|---|---|
| `base` | retiring instructions; the 4 pipeline-fill cycles at reset; the halting cycle of a fault |
| `load_use` | `LoadUse` |
| `branch` | mispredict flushes from EX, ID-redirect bubbles (a correctly predicted taken branch that has no BTB hit), and `FENCE.I` flushes |
| `icache` | `ICache` (including wrong-path I-misses still in progress after a redirect) |
| `dcache` | `DCache` |
| `muldiv` | `MulDiv` |

- **The sum equals total cycles by construction:** one WB occupant per
  cycle, and one cause per occupant. It is also **asserted** at the end of
  every run, and checked in every test.
- `branch` is broken down further in the detailed stats (`mispredict`,
  `id_redirect`, `fence_i`).
- **Naming deviation from your spec:** you asked for a category called
  "branch mispredict". I call it `branch`, because a correctly predicted
  taken branch without a BTB still costs 1 ID-redirect bubble, and calling
  that a mispredict would be wrong.
- **Known bias of this method:** a flush replaces bubbles that are already
  in flight, so the stack shows which event *put a bubble in the retire
  slot*, not every hazard that was pending. For example, if an I-miss's
  bubbles get flushed by a mispredict, those cycles are counted as `branch`.
  This is the standard limitation of slot-based accounting, and it is in
  LIMITATIONS.

**Statistics output:** `rvsim-pipe --stats FILE` (or `--stats -`) writes
JSON. Its fields are:
- the resolved config
- cycles, retired, and the CPI stack in cycles
- the branch breakdown
- predictor counts: conditional branches, mispredicts, jumps, BTB hits
- cache counts: accesses, misses, write-backs
- DRAM counts: row hits, closed-row accesses, row misses

Ratios such as CPI, hit rate and accuracy are computed from those integer
counts. The integers are always printed too.

## 6. Tests (written before the features)

**Unit tests, all with hand-computed expected values:**
- **Cache:** 2 sets × 2 ways × 16-byte lines, driven by an address sequence,
  with a per-access expected outcome: hit or miss, which way was evicted (LRU
  order), and write-backs. Also checks dirty tracking, write-allocate, and
  that I-cache invalidation clears everything.
- **Bimodal:** counter transition table; saturation at 00 and 11; index
  aliasing between two pcs that share low bits.
- **Gshare:** index = pc XOR history for a known history; an alternating
  T/N/T/N branch spaced far enough apart that history lag can't matter.
  Bimodal mispredicts it on most iterations; gshare with history ≥ 1 learns
  it after a hand-counted warm-up.
- **BTB:** hit or miss; replacement on index collision; a full tag prevents
  aliasing.
- **DRAM:** an address sequence across banks and rows giving an exact list of
  hit, closed and miss latencies.
- **Config:** parsing, defaults, rejection of unknown keys and invalid sizes,
  `--set` overrides.

**Pipeline integration tests** (expected cycles *and* every CPI-stack
component, derived by hand):
- the loop in §2.3 under all three predictor setups
- the straight-line I-miss example in §4
- D-cache tests:
  - a strided load sweep: first touch misses, same line hits
  - a store miss that evicts a dirty line, so the cost includes a write-back
  - a D-miss while a MUL is busy (the timers overlap)
- an I-miss that is still in flight when a mispredict redirect arrives
- `FENCE.I` causing I-cache refetch misses

**Regressions:**
- **Phase 2 regression:** the baseline config reproduces the per-test cycle
  counts in `results/phase2/exit_criterion.txt`, and all 34 Phase 2 timing
  tests still pass.
- **Lockstep across configs:** zero mismatches on all 49 riscv-tests and on
  random seeds 1–200 under each of `baseline`, `bimodal_btb`, `gshare_btb`,
  `small_caches` and `big`, so any wrong-path or redirect bug would show up.
- **CPI sum:** in every run above, the CPI stack sums exactly to total
  cycles.

**Exit criterion:**
- All of the above pass under ASan and Release.
- The CPI-sum assertion holds in every test.
- The measured results (with provenance) go in `results/phase3/`.

## 7. Code layout

```
src/common/config.*       key=value parser: types, validation, unknown-key rejection
src/bpred/predictor.*     StaticNT, Bimodal, Gshare; Btb
src/cache/cache.*         tag-only set-associative cache with LRU
src/cache/dram.*          banked open-page DRAM latency model
src/cache/memsys.*        miss-penalty calculation (write-back + fill + transfer)
src/pipeline/*            IF/ID/EX/MEM changes per §2.2, §3.3, §3.4; WB accounting per §5
configs/*.cfg             baseline, bimodal_btb, gshare_btb, small_caches, big
```

None of this is shared with the ISS. The ISS stays untouched.

## 8. Tradeoffs worth defending

| Decision | Alternative | Why |
|---|---|---|
| Timing-only caches and predictors | Caches that hold real data | Cannot change architectural results, so lockstep stays a valid check under every config. Cost: a bug that corrupts data in a real cache cannot be modelled. Irrelevant for a performance model |
| Non-speculative GHR | Speculative GHR with repair | Deterministic and simple in RTL. The cost (lagged history) is real and documented |
| ID-stage redirect | Predict only with a BTB | Makes bimodal and gshare meaningful without a BTB, which your spec makes optional |
| Blocking misses, no DRAM contention | MSHRs, a bus model | An in-order core with blocking caches is the standard baseline. Contention is listed in LIMITATIONS |
| Retire-slot CPI accounting | Per-stage stall counters | The sum is exact by construction, and each slot has an unambiguous owner. Its bias is documented in §5 |
| Strict config parser | Accepting unknown keys | A typo can't silently produce a wrong sweep point |

## 9. Clarifications added during implementation

1. **Misprediction is decided by the taken bit, then the target, never by
   comparing next-pc values** (EX and ID use the same rule). A taken jump to
   `pc + 4` that was predicted not-taken is a misprediction, exactly as in
   Phase 2. Found by the Phase 2 regression oracle (`docs/BUGS.md` #4).
2. **`static_nt` has no ID-stage redirects at all.** That is the Phase 2 front
   end. Otherwise JAL would cost 1 cycle instead of 2 and the baseline would
   stop reproducing Phase 2. ID redirect is enabled for `bimodal` and
   `gshare`. With `static_nt` plus a BTB, ID still corrects BTB predictions
   (a stale target, or a hit on a non-branch).
3. **The predictor trains the counter it actually read.** The table index
   used in IF travels with the instruction to EX and is trained there. For
   gshare the history may have moved on in between (§2.1), and indexing
   again at EX would train the wrong counter.
4. **When EX is held** (by a D-miss stall or a busy MUL/DIV), ID/EX is
   reloaded with the operand values forwarded that cycle. A producer in
   MEM/WB can retire during the hold, so the next cycle could no longer
   forward it, and the value latched in ID would be stale.
   `P3.OperandHeldInExSurvivesProducerRetiringDuringDMiss` covers this.
5. **The prediction is read in the cycle the instruction is handed from IF to
   IF/ID**, not when its I-cache fill completes. Lookups have no side
   effects, so a fetch that is held simply reads the predictor again, seeing
   the most recent state.
