# Phase 1 Design Note — Golden Functional Simulator (ISS)

Status: **approved 2026-10-13; implemented**

## Purpose

The ISS is the *reference* that the Phase 2 pipeline is checked against in
lockstep. Every choice below favours obvious correctness and readability over
speed. It has no notion of cycles.

## Scope

- RV32I base + M extension, unprivileged only.
- No CSRs, no traps, no privilege modes, no interrupts.
- `FENCE` and `FENCE.I` are architectural no-ops in the ISS (there is no cache
  or prefetch state to synchronise). They *will* matter in Phase 2.
- `ECALL` / `EBREAK` halt the simulation (reported as such; not used by tests).

## Components and owned state

| Component | Location | Owns | Interface |
|---|---|---|---|
| Decode table | `src/common/decode.{h,cpp}` | nothing (pure) | `DecodedInsn decode(uint32_t raw)` → op enum, rd, rs1, rs2, sign-extended imm, format |
| Memory | `src/iss/memory.{h,cpp}` | sparse map of 4 KiB pages; never-written bytes read as 0 | `uint32_t load(addr, size)`, `void store(addr, size, val)`, little-endian |
| Hart / ISS | `src/iss/iss.{h,cpp}` | `x[32]`, `pc`, retired-instruction count, halt reason | `RetireRecord step()`, `RunResult run(max_insns)` |
| Loader | `src/iss/loader.{h,cpp}` | nothing | `LoadResult loadElf(path, Memory&)` (entry, `tohost` symbol); `loadFlat(path, base, Memory&)` |
| Trace writer | `src/iss/trace.{h,cpp}` | output stream | `void write(const RetireRecord&)` |
| CLI | `tools/iss_main.cpp` | nothing | `rvsim-iss prog.elf [--flat --base ADDR --entry ADDR] [--trace FILE] [--max-insns N]` |

### Independence boundary (rule 4)

`src/common/decode` is the **only** code shared with the Phase 2 pipeline. It
maps bits → fields. All *semantics* (ALU ops, branch conditions, load/store
sizing and sign extension, MUL/DIV edge cases, next-PC computation) live in
`src/iss` and will be written again, separately, in `src/pipeline`.

**Known risk:** a decode bug is common-mode — both models would agree on the
wrong answer, so lockstep cannot catch it. Mitigations:
1. Decode has its own GoogleTest suite using instruction words **hand-encoded
   from the ISA spec tables**, not produced by the toolchain we are testing
   against.
2. The riscv-tests check architectural results end-to-end, so a decode error
   shows up as an ISS test failure.

This risk will be listed in `LIMITATIONS.md`.

## Retire trace format

One line per retired instruction, fixed field order, `-` for absent fields:

```
<seq> pc=<hex8> insn=<hex8> rd=<xNN|-> val=<hex8|-> mem=<R|W|-> addr=<hex8|-> data=<hex8|-> sz=<1|2|4|->
```

Example:
```
41 pc=80000104 insn=00c5a023 rd=- val=- mem=W addr=80002000 data=0000002a sz=4
42 pc=80000108 insn=0005a603 rd=x12 val=0000002a mem=R addr=80002000 data=0000002a sz=4
```

Rules, chosen so two correct models can never disagree on formatting:
- Writes to `x0` are logged as `rd=- val=-` (they have no architectural effect).
- Branches/stores have `rd=-`.
- Load `data` is the raw value read, zero-extended to 32 bits, *before*
  sign extension; `val` is what lands in `rd` (after extension).
- Store `data` is the value written, masked to `sz` bytes.
- `seq` starts at 0.
- Lowercase hex, no `0x` prefix, always 8 digits.

## Termination

- **Pass/fail:** a store to the `tohost` symbol's address. `1` = pass;
  odd value `(n<<1)|1` = failure of test case `n`. Simulation stops on the
  retiring store.
- **Errors** (non-zero exit, distinct codes): illegal instruction, misaligned
  load/store/fetch, `ECALL`/`EBREAK`, `--max-insns` reached.
- No wall-clock timeouts anywhere (determinism, rule 5). Memory is
  zero-initialised; there is no randomness in the ISS.

## riscv-tests strategy — **Decision: custom minimal test environment**

The upstream `p` environment sets up machine mode via CSRs (`mtvec`, `mepc`,
`satp`, PMP, …) and exits via `ecall`/`mret`. Supporting it would force CSRs and
trap handling into the Phase 2 pipeline and Phase 5 RTL — complexity that has
nothing to do with the performance-modeling story.

So:
- `riscv-tests` is a git submodule **pinned to a specific commit** (recorded in
  the README); test bodies (`isa/rv32ui/*.S`, `isa/rv32um/*.S`) are used
  **unmodified**.
- `tests/riscv-tests-env/riscv_test.h` + `link.ld` replace upstream's `env/p`:
  `RVTEST_CODE_BEGIN` just falls into the test, `RVTEST_PASS`/`RVTEST_FAIL`
  store to `tohost` and spin. ~40 lines, fully reviewable.
- Interview phrasing: *"official test bodies, minimal custom environment —
  here is the 40-line diff from upstream's."*

Alternative rejected: implementing enough M-mode to run `env/p` unmodified.

## Misaligned access — **Decision: halt with error**

The spec permits either hardware support or a trap. We have no traps, and
supporting misaligned access in the pipeline/RTL means multi-access
loads/stores. So misaligned data access halts the ISS with a distinct error.

Consequence: `rv32ui/ma_data` is **excluded** (it tests misaligned access).
Exclusions are listed explicitly in CMake and reported alongside the pass
count — never silently dropped.

`rv32ui/fence_i` (self-modifying code) is **kept**: trivial in the ISS, and an
important correctness case for the Phase 2 pipeline.

## Build & test

- CMake ≥ 3.24, C++20, `CMakePresets.json`:
  - `release` — `-O3`, for benchmarks
  - `asan` — `-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all`,
    for correctness runs
- `-Wall -Wextra -Werror` on project code.
- GoogleTest via `FetchContent`, pinned to a release tag.
- Unit tests (written before the code they test):
  - decode: every RV32IM opcode, hand-encoded, incl. immediate sign extension for I/S/B/U/J
  - memory: endianness, cross-page access, untouched = 0
  - ISS semantics: `DIV`/`REM` by zero, `INT_MIN / -1`, `MULH`/`MULHSU`/`MULHU` signs,
    shift-amount masking, `x0` hardwired, `JALR` clears bit 0, `LB`/`LH` sign extension
  - loader: ELF segments land correctly, BSS zero-filled, bad ELF rejected
  - trace: exact golden-string formatting
- Integration: one CTest per riscv-test ELF, built by CMake with
  `riscv64-unknown-elf-gcc -march=rv32im -mabi=ilp32 -nostdlib -nostartfiles`.
- **Exit criterion:** every non-excluded rv32ui + rv32um test passes under the
  `asan` preset; report the exact count, command, commit hash, `lscpu`, and
  compiler versions.

## Deliberate tradeoffs

| Choice | Alternative | Why |
|---|---|---|
| `switch` interpreter over decoded op | threaded code / JIT | speed is irrelevant for a golden model; clarity is everything |
| Sparse paged memory | flat array | load ELFs at any address without huge allocations |
| Hand-written ELF32 parser (PT_LOAD + symtab) | libelf / ELFIO | no dependency; ~100 lines I can explain line by line |
| Decode to an enum, then execute | execute directly on raw bits | the decode table is the thing Phase 2 is allowed to share |

## Deviations from this note during implementation

- CLI: no `--entry` flag (a flat binary's entry is `--base`). Added `--tohost ADDR`
  so flat binaries can still signal pass/fail.
- Cross-compile flags are `-march=rv32im_zifencei` (GCC 13 needs Zifencei for
  `fence.i`). **Zicsr is deliberately omitted** so any CSR instruction fails
  to assemble rather than silently depending on unmodelled behaviour.
- Warnings are stricter than stated: `-Wall -Wextra -Wpedantic -Wshadow -Werror`.
- Tests were written before the implementation, but were not run red against
  stubs first. The harness's ability to fail was checked separately by
  mutation (see `results/phase1/mutation_check.txt`).
- Added a `trace-determinism` CTest: two traced runs must be byte-identical.

## Environment

GitHub Codespace, Ubuntu 24.04 dev container (`.devcontainer/`), packages
installed by `postCreateCommand` so the environment is reproducible.
All measurements are taken there and logged with `lscpu` and tool versions.
