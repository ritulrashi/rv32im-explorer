# Bug Log

Every bug found and fixed, with how it was detected.

| # | Phase | Component | Symptom | Root cause | How detected | Fix commit |
|---|---|---|---|---|---|---|
| 1 | 1 | `tests/riscv-tests-env/riscv_test.h` (our custom test env) | `rv32ui-fence_i` halted with `illegal-instruction` at pc `0x80000178` after 56 retired instructions | Our `RVTEST_FAIL` macro added a spin loop labelled `2:`. `fence_i.S` uses the forward reference `2f` to mean its own `2:` label in `.data`; the assembler bound it to our earlier label in `.text.init` instead. The self-modifying stores therefore overwrote our spin loop, and the jump to "2f" landed on a corrupted word. | First full `ctest --preset asan` run (48/49 riscv-tests passing). The retire trace showed the stores at seq 49/51 targeting `0x80000178` (`.text.init`) instead of `0x800020xx` (`.data`). | see git log: "Phase 1: golden RV32IM ISS" |

| 2 | 1 | `scripts/phase1_exit.sh`, `scripts/phase1_mutation_check.sh` (result provenance) | The first official run on a clean checkout of `cc3da27` reported `tree: DIRTY` | Both scripts piped their output through `tee results/phase1/...`. `tee` creates its output file as soon as the pipeline starts, so `env_report.sh`, running inside that same pipeline, saw an untracked `results/` directory. The measured code really was `cc3da27`, but the label was wrong. | Reading the generated results file before publishing it. The provenance block contradicted `git status`, which had been clean immediately before the run. | see git log: "Fix result provenance" |

| 3 | 2 | `tools/rvgen.cpp` (random program generator) | None observed. Caught before the code was first run. | Calls such as `line(fmt, kR[rng.below(10)], dst(), a, b)` drew several random numbers inside one function call's arguments. C++ does not specify the order in which arguments are evaluated, so another compiler (or GCC at another optimisation level) could draw them in a different order and produce a different program from the same seed. That breaks rule 5 (determinism) and makes a failing seed irreproducible on another machine. | Code review while writing `rvgen`, before the first run | see git log: "Phase 2" |

## Notes on bug #1

- Fix: spin loops in `RVTEST_PASS`/`RVTEST_FAIL` now use `j .` (no label). The env now defines no numeric labels beyond upstream's single `1:` in `RVTEST_FAIL`. A comment in the header warns about this.
- Cross-check made during the investigation: in the broken binary, the store at `0x800000c4` traced as encoding `0x0ae29c23`. `riscv64-unknown-elf-objdump` (an independent decoder) disassembles it as `sh a4,184(t0)`, even though the source says `sh a0, 2f, t0`. So the ISS executed exactly what was in the binary. The register field was altered when the PC-relative low-12 offset was resolved against the wrong, in-section label (GNU as/ld 2.42-1ubuntu1+6). This is no longer reachable after the fix, and was not investigated further inside binutils.
- Lesson for later phases: when a test fails, check the test harness first. The model was right; the environment was wrong.
