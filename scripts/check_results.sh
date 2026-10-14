#!/usr/bin/env bash
# Rule 1 guard for RESULTS.md: every measurement it quotes must appear in a
# generated results file (Phase 4 tables/investigations/raw CSV, Phase 6 sweep
# CSVs). Prints the checker's verdict; exit status 1 if any number is missing.
#
#   scripts/check_results.sh
set -euo pipefail
cd "$(dirname "$0")/.."
files=(results/phase4/tables.md results/phase4/investigations.md results/phase4/raw.csv results/phase4/run_summary.txt)
for f in results/phase6/sweeps/*.csv; do [ -f "$f" ] && files+=("$f"); done
python3 scripts/check_results_numbers.py RESULTS.md "${files[@]}"
