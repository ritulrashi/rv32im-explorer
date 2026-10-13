#!/usr/bin/env bash
# Phase 1 exit criterion: build from scratch with the ASan/UBSan preset and the
# Release preset, run the full test suite under each, and record the results
# with provenance in results/phase1/exit_criterion.txt.
#
# Usage: scripts/phase1_exit.sh
set -euo pipefail
cd "$(dirname "$0")/.."
out=results/phase1/exit_criterion.txt
mkdir -p results/phase1

run() {
  local preset=$1
  rm -rf "build/$preset"
  cmake --preset "$preset" > "build-$preset-configure.log" 2>&1
  cmake --build --preset "$preset" > "build-$preset-build.log" 2>&1
  # ctest exits non-zero on failure; capture the output either way.
  ctest --preset "$preset" -j"$(nproc)" > "build-$preset-ctest.log" 2>&1 || true
}

{
  echo "# Phase 1 exit criterion: golden ISS vs official riscv-tests"
  echo "command: scripts/phase1_exit.sh"
  echo
  echo "## Provenance"
  scripts/env_report.sh
  for preset in asan release; do
    run "$preset"
    echo
    echo "## Preset: $preset"
    grep -E '^-- riscv-tests' "build-$preset-configure.log" || true
    echo "### Per-test results (riscv-tests)"
    grep -E 'Test +#[0-9]+: rv32u[im]-' "build-$preset-ctest.log" \
      | sed -E 's/^ *[0-9]+\/[0-9]+ Test +#[0-9]+: //; s/ \.+ +/ /; s/ +[0-9.]+ sec$//' | sort
    echo "### Summary"
    grep -E 'tests passed|Label Time Summary|^(determinism|riscv-tests|rv32ui|rv32um|unit) +=' \
      "build-$preset-ctest.log"
    grep -A100 'The following tests FAILED' "build-$preset-ctest.log" || echo "failed tests: none"
  done
} | tee "$out"
rm -f build-*-configure.log build-*-build.log build-*-ctest.log
