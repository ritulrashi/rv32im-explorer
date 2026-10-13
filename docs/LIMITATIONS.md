# Limitations

What this project does **not** model or verify. Updated every phase.

## Phase 1 (golden ISS)
- No privileged architecture: no CSRs, traps, interrupts, or privilege modes.
- Misaligned data access halts the simulator rather than being emulated or trapped; `rv32ui/ma_data` is therefore excluded.
- riscv-tests run under a minimal custom environment (`tests/riscv-tests-env/`), not upstream `env/p`. Test bodies are unmodified.
- Decode tables are shared between the ISS and the pipeline model, so a decode bug would be common-mode and invisible to lockstep. Mitigated by hand-encoded decode unit tests, but not eliminated.
- `FENCE`/`FENCE.I` are no-ops in the ISS.
