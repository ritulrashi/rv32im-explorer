#!/usr/bin/env bash
# Phase 2 exit criterion: zero lockstep mismatches between the golden ISS and
# the pipeline on every riscv-test plus random programs from seeds 1-1000, and
# every directed pipeline timing test passing, under both the ASan/UBSan and
# Release builds. Also checks that cycle counts are identical across two runs.
#
# Usage: scripts/phase2_exit.sh
set -euo pipefail
cd "$(dirname "$0")/.."
prov=$(scripts/env_report.sh)   # provenance first (docs/BUGS.md #2)

FIRST_SEED=1
LAST_SEED=1000
out=results/phase2/exit_criterion.txt
report=$(mktemp)
logs=build/logs
trap 'rm -f "$report"' EXIT
mkdir -p "$logs"

lockstep_riscv_tests() {  # $1 = build dir; prints one line per test, sorted
  for elf in "$1"/tests/riscv-tests/*.elf; do
    "$1/rvsim-lockstep" "$elf" 2>&1 | sed "s#$1/tests/riscv-tests/##" || true
  done | sort
}

{
  echo "# Phase 2 exit criterion: pipeline vs golden ISS in lockstep"
  echo "command: scripts/phase2_exit.sh"
  echo "pipeline config: defaults (mul_latency=3, div_latency=33)"
  echo
  echo "## Provenance"
  echo "$prov"
  for preset in asan release; do
    b=build/$preset
    rm -rf "$b"
    cmake --preset "$preset" > "$logs/p2-$preset-configure.log" 2>&1
    cmake --build --preset "$preset" > "$logs/p2-$preset-build.log" 2>&1
    ctest --preset "$preset" -j"$(nproc)" > "$logs/p2-$preset-ctest.log" 2>&1 || true
    echo
    echo "## Preset: $preset"
    echo "### Full CTest suite"
    grep -E 'tests passed|^(determinism|lockstep|pipeline|random|riscv-tests|unit) +=' "$logs/p2-$preset-ctest.log" \
      | sed -E 's/ +[0-9.]+ sec\*proc//'
    grep -A100 'The following tests FAILED' "$logs/p2-$preset-ctest.log" || echo "failed tests: none"
    echo "### Directed pipeline timing tests"
    "$b/tests/rvx_unit_tests" --gtest_filter='PipeTest.*' 2>&1 | grep -E '^\[  (PASSED|FAILED) +\]' || true
    echo "### Lockstep on riscv-tests (retired instructions and pipeline cycles per test)"
    lockstep_riscv_tests "$b" > "$logs/p2-$preset-ls1.txt"
    cat "$logs/p2-$preset-ls1.txt"
    ok=$(grep -c '^LOCKSTEP OK' "$logs/p2-$preset-ls1.txt" || true)
    n=$(ls "$b"/tests/riscv-tests/*.elf | wc -l)
    echo "riscv-tests in lockstep: $ok of $n agree"
    echo "### Determinism: second run of the same 49 programs"
    lockstep_riscv_tests "$b" > "$logs/p2-$preset-ls2.txt"
    if cmp -s "$logs/p2-$preset-ls1.txt" "$logs/p2-$preset-ls2.txt"; then
      echo "identical output (retired and cycle counts) across two runs: yes"
    else
      echo "identical output across two runs: NO"
      diff "$logs/p2-$preset-ls1.txt" "$logs/p2-$preset-ls2.txt" | head -20
    fi
    echo "### Random programs (rvgen seeds $FIRST_SEED-$LAST_SEED)"
    bash scripts/random_lockstep.sh "$b" "$FIRST_SEED" "$LAST_SEED" "$logs/p2-$preset-random" 2>&1 | tail -25 || true
  done
} > "$report"

mkdir -p "$(dirname "$out")"
mv "$report" "$out"
cat "$out"
