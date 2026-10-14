#!/usr/bin/env bash
# The regression run by CI on every push (docs/phase6-design.md §3), with every
# step timed. The same script runs locally, so CI and local timings are of the
# same work.
#
#   scripts/phase6_regression.sh          push regression
#   scripts/phase6_regression.sh --full   nightly: everything under ASan, incl. workloads
#
# Push regression:
#   ASan+UBSan build : every CTest label except "workloads" (golden ISS tests,
#                      pipeline timing, lockstep, random seeds, CPI sum, Phase 2
#                      regression, config/predictor/cache tests, RTL lint and
#                      correlation smoke, determinism, sweep tooling)
#   Release build    : the "workloads" label (30 workloads: self-check + lockstep)
#                      + RTL correlation on rvgen --isa rv32i seeds 1-100
# Writes build/logs/regression_runtime.txt; exits non-zero on the first failure.
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-push}
logs=build/logs
mkdir -p "$logs"
timing="$logs/regression_runtime.txt"
jobs=$(nproc)
: > "$timing.steps"
total_start=$(date +%s)

step() {  # $1 = name, rest = command
  local name=$1
  shift
  local s rc=0
  s=$(date +%s)
  echo "::group::$name" 2>/dev/null || true
  "$@" > "$logs/step-$(echo "$name" | tr ' /' '__').log" 2>&1 || rc=$?
  echo "::endgroup::" 2>/dev/null || true
  local e=$(( $(date +%s) - s ))
  printf '| %s | %d s | %s |\n' "$name" "$e" "$([ $rc -eq 0 ] && echo pass || echo "FAIL (rc=$rc)")" >> "$timing.steps"
  if [ $rc -ne 0 ]; then
    tail -40 "$logs/step-$(echo "$name" | tr ' /' '__').log"
    finish
    exit $rc
  fi
}

finish() {
  {
    echo "# Regression runtime ($mode)"
    echo "command: scripts/phase6_regression.sh $( [ "$mode" = --full ] && echo --full )"
    echo "parallel jobs: $jobs"
    echo
    echo "## Provenance"
    bash scripts/env_report.sh
    echo
    echo "| step | wall time | result |"
    echo "|---|---:|---|"
    cat "$timing.steps"
    echo
    echo "total wall time: $(( $(date +%s) - total_start )) s"
  } > "$timing"
  cat "$timing"
}

if [ "$mode" = "--full" ]; then
  step "asan configure+build" sh -c "rm -rf build/asan && cmake --preset asan && cmake --build --preset asan -j $jobs"
  step "asan ctest (all labels)" ctest --preset asan -j "$jobs"
  step "asan RTL correlation, seeds 1-500" bash scripts/correlate.sh build/asan "$logs/corr-asan" 1 500
else
  step "asan configure+build" sh -c "rm -rf build/asan && cmake --preset asan && cmake --build --preset asan -j $jobs"
  step "asan ctest (all labels except workloads)" ctest --preset asan -j "$jobs" -LE workloads
  step "release configure+build" sh -c "rm -rf build/release && cmake --preset release && cmake --build --preset release -j $jobs"
  step "release ctest (workloads)" ctest --preset release -j "$jobs" -L workloads
  step "release RTL correlation, seeds 1-100" bash scripts/correlate.sh build/release "$logs/corr-release" 1 100
fi
finish
