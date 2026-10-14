#!/usr/bin/env bash
# Does RTL-vs-model correlation actually catch RTL bugs? (rule 6)
#
# Each mutant injects one bug into the RTL only, rebuilds rvsim-rtl in a temp
# copy of the tree, and runs scripts/correlate.sh (directed + rv32ui + rvgen
# rv32i seeds 1-50). Records how many programs expose the mutant and at which
# comparison level.
#
#   V1  register file loses WB write-through               (P2 §3.1)
#   V2  forwarding prefers MEM/WB over EX/MEM              (P2 §6.1)
#   V3  load-use stall removed                             (P2 §6.2)
#   V4  FENCE.I does not redirect                          (P2 §6.5)
#   V5  BLT compares unsigned                              (ISA semantics)
#   V6  load-use bubble labelled "mispredict"              (stage view only)
#   V7  misaligned jump target check removed               (P2 §6.6)
#   V8  taken-branch flush leaves ID/EX valid              (P2 §6.3)
#
# Usage: scripts/phase5_mutation_check.sh
set -euo pipefail
cd "$(dirname "$0")/.."
prov=$(scripts/env_report.sh)   # provenance first (docs/BUGS.md #2)
out=results/phase5/mutation_check.txt
report=$(mktemp)
src=$(mktemp -d)
trap 'rm -rf "$src" "$report"' EXIT
git ls-files -z --recurse-submodules | tar --null -T - -cf - | tar -xf - -C "$src"

mutate() {  # $1 = mutant id, $2 = tree
  local c=$2/rtl/rvx_core.sv p=$2/rtl/rvx_pkg.sv f
  case $1 in
    V1) f=$c; sed -i 's|^    if (wb_we \&\& mem_wb_q.rd == r) return mem_wb_q.value;$|    // MUTANT V1: no write-through|' "$f" ;;
    V2) f=$c; sed -i 's|^    if (ex_mem_q.valid \&\& ex_mem_q.exc == HALT_NONE \&\& ex_mem_q.rd == rs) return ex_mem_q.result;$|    if (mem_wb_q.valid \&\& mem_wb_q.exc == HALT_NONE \&\& mem_wb_q.rd == rs) return mem_wb_q.value;  // MUTANT V2\n    if (ex_mem_q.valid \&\& ex_mem_q.exc == HALT_NONE \&\& ex_mem_q.rd == rs) return ex_mem_q.result;|' "$f" ;;
    V3) f=$c; sed -i 's|^      load_use = id_ex_q.valid \&\& id_ex_q.exc == HALT_NONE \&\& !ex_fault \&\&$|      load_use = 1'"'"'b0 \&\& id_ex_q.valid \&\& id_ex_q.exc == HALT_NONE \&\& !ex_fault \&\&  // MUTANT V3|' "$f" ;;
    V4) f=$c; sed -i 's|^            redirect = 1.b1;$|&|; /K_FENCE_I: begin/{n;s|^            redirect = 1.b1;$|            redirect = 1'"'"'b0;  // MUTANT V4|}' "$f" ;;
    V5) f=$p; sed -i 's|^      3.d4:    return \$signed(a) < \$signed(b);$|      3'"'"'d4:    return a < b;  // MUTANT V5|' "$f" ;;
    V6) f=$c; sed -i 's|^      n_id_ex_sel.cause = CAUSE_LOAD_USE;$|      n_id_ex_sel.cause = CAUSE_MISPREDICT;  // MUTANT V6|' "$f" ;;
    V7) f=$c; sed -i '/K_JAL, K_JALR: begin/,/end else begin/{s|^            if (target\[1:0\] != 2.b00) begin$|            if (1'"'"'b0) begin  // MUTANT V7|}' "$f" ;;
    V8) f=$c; sed -i '/end else if (redirect) begin/,/n_pc = redirect_pc;/{s|^      n_id_ex_sel = .0;$|      n_id_ex_sel = n_id_ex;  // MUTANT V8|}' "$f" ;;
  esac
  if ! grep -q "MUTANT $1" "$f"; then
    echo "mutation $1 did not apply; update the pattern in $0" >&2
    exit 1
  fi
}

{
  echo "# Phase 5 mutation check: does RTL-vs-model correlation catch injected RTL bugs?"
  echo "command: scripts/phase5_mutation_check.sh"
  echo
  echo "## Provenance (of the unmutated tree)"
  echo "$prov"
  for m in V1 V2 V3 V4 V5 V6 V7 V8; do
    work=$(mktemp -d)
    cp -r "$src/." "$work"
    mutate "$m" "$work"
    (cd "$work" && cmake --preset release >/dev/null 2>&1 && cmake --build --preset release >/dev/null 2>&1) || {
      echo; echo "## Mutant $m: did not build"; rm -rf "$work"; continue; }
    echo
    echo "## Mutant $m"
    grep -rn "MUTANT $m" "$work/rtl" | sed "s#$work/##"
    bash scripts/correlate.sh "$work/build/release" "$work/corr" 1 50 > "$work/corr.log" 2>&1 || true
    grep -E '^correlate:' "$work/corr.log"
    for level in MISMATCH-1-summary MISMATCH-2-trace MISMATCH-3-diagram MISMATCH-hand-derivation MISMATCH-hand-diagram; do
      n=$(grep -c " $level " "$work/corr/programs.txt" || true)
      [ "$n" -gt 0 ] && echo "  first detected at $level: $n programs"
    done
    echo "  example: $(grep -m1 MISMATCH "$work/corr/programs.txt" | cut -c1-200)"
    rm -rf "$work"
  done
} > "$report"
mkdir -p "$(dirname "$out")"
mv "$report" "$out"
cat "$out"
