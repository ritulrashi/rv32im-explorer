# Bug Log

Every bug found and fixed, with how it was detected.

| # | Phase | Component | Symptom | Root cause | How detected | Fix commit |
|---|---|---|---|---|---|---|
| 1 | 1 | `tests/riscv-tests-env/riscv_test.h` (our custom test env) | `rv32ui-fence_i` halted with `illegal-instruction` at pc `0x80000178` after 56 retired instructions | Our `RVTEST_FAIL` macro added a spin loop labelled `2:`. `fence_i.S` uses the forward reference `2f` to mean its own `2:` label in `.data`; the assembler bound it to our earlier label in `.text.init` instead. The self-modifying stores therefore overwrote our spin loop, and the jump to "2f" landed on a corrupted word. | First full `ctest --preset asan` run (48/49 riscv-tests passing). The retire trace showed the stores at seq 49/51 targeting `0x80000178` (`.text.init`) instead of `0x800020xx` (`.data`). | see git log: "Phase 1: golden RV32IM ISS" |

## Notes on bug #1

- Fix: spin loops in `RVTEST_PASS`/`RVTEST_FAIL` now use `j .` (no label). The env now defines no numeric labels beyond upstream's single `1:` in `RVTEST_FAIL`. A comment in the header warns about this.
- Cross-check made during the investigation: in the broken binary, the store at `0x800000c4` traced as encoding `0x0ae29c23`. `riscv64-unknown-elf-objdump` (an independent decoder) disassembles it as `sh a4,184(t0)`, even though the source says `sh a0, 2f, t0`. So the ISS executed exactly what was in the binary. The register field was altered when the PC-relative low-12 offset was resolved against the wrong, in-section label (GNU as/ld 2.42-1ubuntu1+6). This is no longer reachable after the fix, and was not investigated further inside binutils.
- Lesson for later phases: when a test fails, check the test harness first. The model was right; the environment was wrong.
