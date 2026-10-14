#!/usr/bin/env bash
# Phase 6 exit criterion (docs/phase6-design.md §6):
#   1. the push regression, timed step by step on this machine;
#   2. the three sweeps (CPI stacks, predictors, D-cache) into CSV + provenance;
#   3. plots from those CSVs.
# The GitHub Actions timing is recorded separately by scripts/ci_runtime.sh.
#
# Everything is written to a temporary directory OUTSIDE the repository and
# moved into results/phase6 only at the end, so the provenance each step
# records (tree: clean/DIRTY) is not polluted by earlier steps' outputs
# (docs/BUGS.md #2 and #7).
#
# Usage: scripts/phase6_exit.sh
set -euo pipefail
cd "$(dirname "$0")/.."
out=results/phase6
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
bash scripts/phase6_regression.sh
cp build/logs/regression_runtime.txt "$tmp/regression_runtime_codespace.txt"
for s in cpi_stacks bpred dcache; do
  python3 scripts/sweep.py "sweeps/$s.json" --build build/release --out "$tmp/sweeps"
done
python3 scripts/plot.py --sweeps "$tmp/sweeps" --out "$tmp"
# PLOTS.md links are relative to results/phase6, so the source paths it
# prints refer to the final location.
sed -i "s#$tmp/sweeps#results/phase6/sweeps#g" "$tmp/PLOTS.md"
rm -rf "$out"
mkdir -p "$(dirname "$out")"
cp -r "$tmp" "$out"
echo "phase6: results in $out"
