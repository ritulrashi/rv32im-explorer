#!/usr/bin/env bash
# Phase 6 exit criterion (docs/phase6-design.md §6):
#   1. the push regression, timed step by step on this machine;
#   2. the three sweeps (CPI stacks, predictors, D-cache) into CSV + provenance;
#   3. plots from those CSVs.
# The GitHub Actions timing is recorded separately by scripts/ci_runtime.sh.
#
# Usage: scripts/phase6_exit.sh
set -euo pipefail
cd "$(dirname "$0")/.."
out=results/phase6
rm -rf "$out"
bash scripts/phase6_regression.sh
mkdir -p "$out"
cp build/logs/regression_runtime.txt "$out/regression_runtime_codespace.txt"
for s in cpi_stacks bpred dcache; do
  python3 scripts/sweep.py "sweeps/$s.json" --build build/release --out "$out/sweeps"
done
python3 scripts/plot.py --sweeps "$out/sweeps" --out "$out"
echo "phase6: results in $out"
