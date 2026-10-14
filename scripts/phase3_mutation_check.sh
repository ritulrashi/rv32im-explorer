#!/usr/bin/env bash
# Do the Phase 3 tests actually catch Phase 3 bugs? (rule 6)
#
# Each mutant injects one timing bug. Most are invisible to lockstep (timing
# never changes values), so the directed tests with hand-computed cycle
# counts are what must catch them. For each mutant we report the failing
# unit tests and lockstep on random seeds 1-50 under configs/small_caches.cfg.
#
#   R1  an in-flight I-cache fill is cancelled by a redirect   (pipeline.cpp)
#   R2  operands are not re-latched when EX is held            (pipeline.cpp)
#   R3  the MUL/DIV counter freezes during a D-miss stall      (pipeline.cpp)
#   R4  cache evicts the MOST recently used way                (cache.cpp)
#   R5  gshare trains the counter re-indexed at EX             (pipeline.cpp)
#   R6  ID-redirect bubbles are charged to `base`              (pipeline.cpp)
#   R7  DRAM charges row-miss latency on a row hit             (dram.cpp)
#
# Usage: scripts/phase3_mutation_check.sh
set -euo pipefail
cd "$(dirname "$0")/.."
prov=$(scripts/env_report.sh)   # provenance first (docs/BUGS.md #2)
out=results/phase3/mutation_check.txt
report=$(mktemp)
src=$(mktemp -d)
trap 'rm -rf "$src" "$report"' EXIT
git ls-files -z --recurse-submodules | tar --null -T - -cf - | tar -xf - -C "$src"

mutate() {  # $1 = mutant id, $2 = tree
  local p=$2/src/pipeline/pipeline.cpp f
  case $1 in
    R1) f=$p; sed -i 's|^      n_pending = target;  // fill in progress: follow the redirect when it ends$|      n_pc = target; n_fetched = false; n_if_wait = 0; n_pending.reset();  // MUTANT R1|' "$f" ;;
    R2) f=$p; sed -i 's|^      h.rs1_val = ex_a;$|      // MUTANT R2: operands not re-latched|; s|^      h.rs2_val = ex_b;$||' "$f" ;;
    R3) f=$p; sed -i 's|^      if (done < latency) {$|      if (c.mem_stall \&\& done < latency) { c.ex_busy = true; n_md_cycles = md_cycles_; }  // MUTANT R3\n      else if (done < latency) {|' "$f" ;;
    R4) f=$2/src/cache/cache.cpp; sed -i 's|      if (ways\[w\].last_use < victim->last_use) victim = \&ways\[w\];|      if (ways[w].last_use > victim->last_use) victim = \&ways[w];  // MUTANT R4|' "$f" ;;
    R5) f=$p; sed -i 's|^      dir_.train(id_ex_.bp_index, tr.taken);$|      dir_.train(dir_.index(id_ex_.pc), tr.taken);  // MUTANT R5|' "$f" ;;
    R6) f=$p; sed -i '/^      case BubbleCause::IdRedirect:$/{n;s|^        cat = CpiCat::Branch;$|        cat = CpiCat::Base;  // MUTANT R6|}' "$f" ;;
    R7) f=$2/src/cache/dram.cpp; sed -i 's|^    latency = cfg_.row_hit_cycles;$|    latency = cfg_.row_miss_cycles;  // MUTANT R7|' "$f" ;;
  esac
  if ! grep -q "MUTANT $1" "$f"; then
    echo "mutation $1 did not apply; update the pattern in $0" >&2
    exit 1
  fi
}

{
  echo "# Phase 3 mutation check: do the tests catch injected timing bugs?"
  echo "command: scripts/phase3_mutation_check.sh"
  echo
  echo "## Provenance (of the unmutated tree)"
  echo "$prov"
  for m in R1 R2 R3 R4 R5 R6 R7; do
    work=$(mktemp -d)
    cp -r "$src/." "$work"
    mutate "$m" "$work"
    (cd "$work" && cmake --preset release >/dev/null && cmake --build --preset release >/dev/null)
    echo
    echo "## Mutant $m"
    grep -rn "MUTANT $m" "$work/src" | sed "s#$work/##"
    "$work/build/release/tests/rvx_unit_tests" > "$work/unit.log" 2>&1 || true
    failed=$(grep -E '^\[  FAILED  \] [A-Za-z0-9_]+\.' "$work/unit.log" | grep -v ' ms)' | sed 's/^\[  FAILED  \] //' || true)
    echo "unit tests failing: $(echo -n "$failed" | grep -c . || true)"
    echo "$failed" | sed '/^$/d; s/^/    /'
    rnd=$(bash scripts/random_lockstep.sh "$work/build/release" 1 50 "$work/rnd" -- --config configs/small_caches.cfg 2>/dev/null | tail -1 || true)
    echo "lockstep, small_caches, seeds 1-50: $rnd"
    rm -rf "$work"
  done
} > "$report"
mkdir -p "$(dirname "$out")"
mv "$report" "$out"
cat "$out"
