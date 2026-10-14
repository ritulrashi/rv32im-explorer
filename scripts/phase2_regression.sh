#!/usr/bin/env bash
# Phase 2 regression oracle (docs/phase3-design.md §0.1): under
# configs/baseline.cfg, every riscv-test must retire the same number of
# instructions in the same number of cycles as recorded in
# results/phase2/exit_criterion.txt.
#
#   scripts/phase2_regression.sh BUILD_DIR
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
build=$(cd "$1" && pwd)
expected=$(grep '^LOCKSTEP OK' "$here/results/phase2/exit_criterion.txt" | sort -u)
actual=$(for elf in "$build"/tests/riscv-tests/*.elf; do
  "$build/rvsim-lockstep" "$elf" --config "$here/configs/baseline.cfg" | sed "s#$build/tests/riscv-tests/##"
done | sort)
n=$(echo "$expected" | wc -l)
if [ "$expected" == "$actual" ]; then
  echo "phase2-regression: baseline matches the Phase 2 record for all $n riscv-tests"
else
  echo "phase2-regression: MISMATCH against results/phase2/exit_criterion.txt"
  diff <(echo "$expected") <(echo "$actual") || true
  exit 1
fi
