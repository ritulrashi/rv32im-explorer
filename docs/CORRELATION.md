# RTL ↔ Model Correlation Log (Phase 5)

The RTL core (`rtl/rvx_core.sv`) and the C++ pipeline model running
`configs/baseline.cfg` are compared, per program, at three levels
(docs/phase5-design.md §1):
1. halt reason, retired count, total cycles, halt pc
2. retire trace, byte-identical
3. per-cycle stage occupancy, including bubble causes, byte-identical

Directed contract programs are also compared against expectations derived by
hand from the contract text, and `t26_diagram` against a hand-drawn per-cycle
diagram. **The contract is the referee:** when RTL and model disagree, the
side that contradicts `docs/phase2-pipeline-contract.md` is fixed, never the
other side to agree.

Commands and measured results: `scripts/phase5_exit.sh` →
[`results/phase5/exit_criterion.txt`](../results/phase5/exit_criterion.txt)
(per-program lines in `programs_release.txt` / `programs_asan.txt`).

## Deviations found

| # | Program / cycle | Symptom | Root cause | Side at fault (per contract) | Fix |
|---|---|---|---|---|---|
| — | — | **None.** The first full correlation run, before any fix, matched on every program at all three levels: 26 directed, 41 rv32ui, and 500 random rv32i programs, 256,595 cycles in total, on the scratch build. The official counts are in the results file. | — | — | — |

Zero deviations is a result that rule 6 says to treat as a suspected bug in
the *comparison*, so it was checked rather than reported as-is:

1. **The artefacts are real and non-trivial.** For a random program, both
   sides produce 545 retire-trace lines and 731 diagram lines, and a one-line
   edit to a diagram is detected by the comparison.
2. **Both sides were also checked independently of each other.** The RTL and
   the model each reproduce the hand-drawn 14-cycle diagram
   (`tests/rtl/t26_diagram.expected`) and all 26 hand-derived
   `# expect:` lines. So agreement does not come from a shared
   misunderstanding: both agree with the contract text.
3. **Mutation check:** 8 deliberate RTL bugs, each of which must be caught by
   the correlation; results in `results/phase5/mutation_check.txt`. All 8 are
   caught. Two of them show why the comparison is built the way it is:
   - **V6** (a load-use bubble labelled "mispredict") changes neither cycle
     counts nor the retire trace. Only level 3, the per-cycle diagram with
     bubble causes, catches it.
   - **V7** (misaligned jump-target check removed) is caught by exactly one
     program, the directed `t24_misaligned_jump`. The riscv-tests and the
     random programs never jump to a misaligned target, so hand-written
     directed tests are not optional.

Why a first-time match is plausible here, and not luck: the RTL was written
directly against the same written contract, in the same structure (current/
next registers, the same control priority). The contract had already been
tightened four times while building the model (P2 §13, P3 §9), so ambiguities
that would have produced deviations had largely been removed before the RTL
existed. Phase 2's hand-derived tests did the job a first correlation pass
usually does.

## Known gaps in what correlation checks

- **Memory contents after the run are not compared.** A bug that only
  changes memory without affecting any later retired instruction (for
  example, a store that should have been suppressed in the halting cycle,
  P2 §7) would not be caught by these three levels.
- **M extension:** the RTL has no MUL/DIV (as specified), so P2 §6.4 is
  checked only by the model's own hand-derived tests, never against RTL.
