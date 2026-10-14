#!/usr/bin/env bash
# Phase 5 exit criterion (docs/phase5-design.md §5): the RTL core and the C++
# model (configs/baseline.cfg) agree at all three levels - halt/retired/cycles,
# retire trace, per-cycle stage diagram - on every directed contract program
# (also checked against hand-derived expectations), every rv32ui riscv-test,
# and rvgen --isa rv32i seeds 1-500 (Release) / 1-100 (ASan+UBSan), and the
# RTL is lint-clean under verilator -Wall.
#
# Usage: scripts/phase5_exit.sh
set -euo pipefail
cd "$(dirname "$0")/.."
prov=$(scripts/env_report.sh)   # provenance first (docs/BUGS.md #2)
out=results/phase5
report=$(mktemp)
logs=build/logs
trap 'rm -f "$report"' EXIT
mkdir -p "$logs"

{
  echo "# Phase 5 exit criterion: RTL vs C++ model correlation"
  echo "command: scripts/phase5_exit.sh"
  echo
  echo "## Provenance"
  echo "$prov"
  echo
  echo "## Lint"
  if verilator --lint-only -Wall rtl/rvx_pkg.sv rtl/rvx_core.sv --top-module rvx_core > "$logs/p5-lint.log" 2>&1; then
    echo "verilator --lint-only -Wall rtl/rvx_pkg.sv rtl/rvx_core.sv: clean (0 warnings)"
  else
    echo "verilator lint: FAILED"
    cat "$logs/p5-lint.log"
  fi
  for spec in "release 500" "asan 100"; do
    set -- $spec
    preset=$1
    last=$2
    b=build/$preset
    rm -rf "$b"
    cmake --preset "$preset" > "$logs/p5-$preset-configure.log" 2>&1
    cmake --build --preset "$preset" > "$logs/p5-$preset-build.log" 2>&1
    echo
    echo "## Preset: $preset (rvgen --isa rv32i seeds 1-$last)"
    ctest --preset "$preset" -L "rtl|determinism" > "$logs/p5-$preset-ctest.log" 2>&1 || true
    grep -E 'tests passed|Failed|\*\*\*' "$logs/p5-$preset-ctest.log" | sed 's/^/ctest -L "rtl|determinism": /'
    start=$(date +%s)
    bash scripts/correlate.sh "$b" "$logs/p5-$preset-corr" 1 "$last" 2>&1 | grep -E '^(correlate|  )' || true
    echo "correlation wall time: $(( $(date +%s) - start )) s"
    cp "$logs/p5-$preset-corr/programs.txt" "$report.$preset.programs"
  done
} > "$report"

mkdir -p "$out"
mv "$report" "$out/exit_criterion.txt"
mv "$report.release.programs" "$out/programs_release.txt"
mv "$report.asan.programs" "$out/programs_asan.txt"
cat "$out/exit_criterion.txt"
