#!/usr/bin/env bash
# Phase 3 exit criterion (docs/phase3-design.md §6), under ASan/UBSan and Release:
#   * all tests pass, including the directed cache/DRAM/predictor/pipeline tests
#     with hand-computed expected values;
#   * the baseline config reproduces the Phase 2 cycle counts exactly;
#   * the CPI stack sums exactly to total cycles in every run checked;
#   * lockstep agrees under every config (riscv-tests + rvgen seeds 1-200).
#
# Usage: scripts/phase3_exit.sh
set -euo pipefail
cd "$(dirname "$0")/.."
prov=$(scripts/env_report.sh)   # provenance first (docs/BUGS.md #2)

CONFIGS="baseline bimodal_btb gshare_btb small_caches big"
FIRST_SEED=1
LAST_SEED=200
out=results/phase3/exit_criterion.txt
report=$(mktemp)
logs=build/logs
trap 'rm -f "$report"' EXIT
mkdir -p "$logs"

{
  echo "# Phase 3 exit criterion: predictors, caches, DRAM, CPI stack"
  echo "command: scripts/phase3_exit.sh"
  echo
  echo "## Provenance"
  echo "$prov"
  echo
  echo "## Configurations"
  for c in $CONFIGS; do
    echo "### configs/$c.cfg"
    grep -v '^#' "configs/$c.cfg" | sed '/^$/d; s/^/    /'
  done
  for preset in asan release; do
    b=build/$preset
    rm -rf "$b"
    cmake --preset "$preset" > "$logs/p3-$preset-configure.log" 2>&1
    cmake --build --preset "$preset" > "$logs/p3-$preset-build.log" 2>&1
    ctest --preset "$preset" -j"$(nproc)" > "$logs/p3-$preset-ctest.log" 2>&1 || true
    echo
    echo "## Preset: $preset"
    echo "### Full CTest suite"
    grep -E 'tests passed|^[a-z]+ +=' "$logs/p3-$preset-ctest.log" | sed -E 's/ +[0-9.]+ sec\*proc//'
    grep -A100 'The following tests FAILED' "$logs/p3-$preset-ctest.log" || echo "failed tests: none"
    echo "### Directed tests with hand-computed expected values (Phase 3 components)"
    "$b/tests/rvx_unit_tests" \
      --gtest_filter='KvFile.*:MachineConfig.*:StaticNT.*:Bimodal.*:Gshare.*:Btb.*:Cache.*:Dram.*:MemSystem.*:P3.*' \
      2>&1 | grep -E '^\[ +(OK|FAILED) +\]|^\[  (PASSED|FAILED)  \]' | sed -E 's/ \([0-9]+ ms\)//'
    echo "### Phase 2 regression (baseline config vs results/phase2)"
    bash scripts/phase2_regression.sh "$b" || true
    echo "### CPI stack sums to total cycles"
    python3 scripts/check_cpi_sum.py "$b" $(for c in $CONFIGS; do echo "configs/$c.cfg"; done) || true
    echo "### Lockstep under every config"
    for c in $CONFIGS; do
      ok=0
      n=0
      for elf in "$b"/tests/riscv-tests/*.elf; do
        n=$((n + 1))
        "$b/rvsim-lockstep" "$elf" --config "configs/$c.cfg" > /dev/null 2>&1 && ok=$((ok + 1))
      done
      echo "$c: riscv-tests $ok of $n agree"
      bash scripts/random_lockstep.sh "$b" "$FIRST_SEED" "$LAST_SEED" "$logs/p3-$preset-rnd-$c" \
        -- --config "configs/$c.cfg" 2>&1 | tail -3 | sed "s/^/$c: /" || true
    done
  done
} > "$report"

mkdir -p "$(dirname "$out")"
mv "$report" "$out"
cat "$out"
