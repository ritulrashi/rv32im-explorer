#!/usr/bin/env bash
# Does lockstep actually catch pipeline bugs? (rule 6)
#
# For each mutant: copy the tracked tree to a temp dir, inject one bug into
# the PIPELINE model only, build, and run lockstep on every riscv-test and on
# random seeds 1-200. Record how many programs detect the mutant.
#
#   Q1  MULHSU loses its sign correction            (exec.cpp)
#   Q2  forwarding from EX/MEM disabled             (pipeline.cpp)
#   Q3  load-use stall disabled                     (pipeline.cpp)
#   Q4  LB does not sign-extend                     (exec.cpp)
#
# Usage: scripts/phase2_mutation_check.sh
set -euo pipefail
cd "$(dirname "$0")/.."
prov=$(scripts/env_report.sh)   # provenance first (docs/BUGS.md #2)
out=results/phase2/mutation_check.txt
report=$(mktemp)
src=$(mktemp -d)
trap 'rm -rf "$src" "$report"' EXIT
git ls-files -z --recurse-submodules | tar --null -T - -cf - | tar -xf - -C "$src"

mutate() {  # $1 = mutant id, $2 = tree
  local f
  case $1 in
    Q1) f=$2/src/pipeline/exec.cpp
        # Remove the correction line inside mulhsu only (not the one in mulh).
        awk 'BEGIN{n=0} /uint32_t mulhsu\(uint32_t a, uint32_t b\) \{/{n=1} n==1 && /if \(a & kSign\) h -= b;/{print "  // MUTANT Q1: correction removed"; n=0; next} {print}' "$f" > "$f.tmp" && mv "$f.tmp" "$f" ;;
    Q2) f=$2/src/pipeline/pipeline.cpp
        sed -i 's/    if (ex_mem_.valid \&\& ex_mem_.exc == HaltReason::None \&\& ex_mem_.rd == rs) {/    if (false \&\& ex_mem_.valid \&\& ex_mem_.exc == HaltReason::None \&\& ex_mem_.rd == rs) {  \/\/ MUTANT Q2/' "$f" ;;
    Q3) f=$2/src/pipeline/pipeline.cpp
        sed -i 's/    c.load_use = ex_has_good_load \&\&/    c.load_use = false \&\& ex_has_good_load \&\&  \/\/ MUTANT Q3\n                /' "$f" ;;
    Q4) f=$2/src/pipeline/exec.cpp
        sed -i 's/    case Op::LB: return (raw \& 0x80u) ? (raw | 0xFFFFFF00u) : raw;/    case Op::LB: return raw;  \/\/ MUTANT Q4/' "$f" ;;
  esac
  if ! grep -q "MUTANT $1" "$f"; then
    echo "mutation $1 did not apply; update the pattern in $0" >&2
    exit 1
  fi
}

{
  echo "# Phase 2 mutation check: does lockstep catch injected PIPELINE bugs?"
  echo "command: scripts/phase2_mutation_check.sh"
  echo
  echo "## Provenance (of the unmutated tree)"
  echo "$prov"
  for m in Q1 Q2 Q3 Q4; do
    work=$(mktemp -d)
    cp -r "$src/." "$work"
    mutate "$m" "$work"
    (cd "$work" && cmake --preset release >/dev/null && cmake --build --preset release >/dev/null)
    echo
    echo "## Mutant $m"
    grep -n "MUTANT $m" "$work"/src/pipeline/*.cpp | sed "s#$work/##"
    caught=0
    total=0
    first=""
    for elf in "$work"/build/release/tests/riscv-tests/*.elf; do
      total=$((total + 1))
      if ! "$work/build/release/rvsim-lockstep" "$elf" > "$work/ls.log" 2>&1; then
        caught=$((caught + 1))
        [ -z "$first" ] && first=$(basename "$elf") && cp "$work/ls.log" "$work/first.log"
      fi
    done
    echo "riscv-tests detecting: $caught of $total"
    rnd=$(bash scripts/random_lockstep.sh "$work/build/release" 1 200 "$work/rnd" 2>/dev/null | tail -1 || true)
    echo "random seeds 1-200: $rnd"
    if [ -n "$first" ]; then
      echo "first divergence report ($first), first 20 lines:"
      head -20 "$work/first.log" | sed 's/^/    /'
    fi
    rm -rf "$work"
  done
} > "$report"
mkdir -p "$(dirname "$out")"
mv "$report" "$out"
cat "$out"
