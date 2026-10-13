#!/usr/bin/env bash
# Sanity check that the riscv-tests harness can actually FAIL (rule 6: a
# result that looks too good is a suspected bug). Copies the tree to a temp
# dir, injects two known bugs into the ISS, and records which riscv-tests catch
# them. Nothing in the real tree is modified.
#
#   M1: DIV by zero returns 0 instead of 0xFFFFFFFF
#   M2: SRA/SRAI shift logically instead of arithmetically
#
# Usage: scripts/phase1_mutation_check.sh
set -euo pipefail
cd "$(dirname "$0")/.."
out=results/phase1/mutation_check.txt
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir -p results/phase1

git ls-files -z --recurse-submodules | tar --null -T - -cf - | tar -xf - -C "$tmp"
sed -i 's/if (b == 0) result = 0xFFFFFFFFu;/if (b == 0) result = 0;  \/\/ MUTANT M1/' "$tmp/src/iss/iss.cpp"
sed -i 's/result = static_cast<uint32_t>(s32(a) >> imm); break;/result = a >> imm; break;  \/\/ MUTANT M2/' "$tmp/src/iss/iss.cpp"
sed -i 's/result = static_cast<uint32_t>(s32(a) >> (b \& 31)); break;/result = a >> (b \& 31); break;  \/\/ MUTANT M2/' "$tmp/src/iss/iss.cpp"
n=$(grep -c MUTANT "$tmp/src/iss/iss.cpp")
if [ "$n" -ne 3 ]; then
  echo "expected 3 mutated lines, got $n; the sed patterns no longer match iss.cpp" >&2
  exit 1
fi

(cd "$tmp" && cmake --preset release >/dev/null && cmake --build --preset release >/dev/null)
{
  echo "# Phase 1 mutation check: does the riscv-tests harness detect injected ISS bugs?"
  echo "command: scripts/phase1_mutation_check.sh"
  echo
  scripts/env_report.sh
  echo
  echo "## Injected mutations"
  grep -n MUTANT "$tmp/src/iss/iss.cpp"
  echo
  echo "## riscv-tests that FAIL with the mutations (expected: div, sra, srai, and lui, whose case 3 uses sra)"
  for elf in "$tmp"/build/release/tests/riscv-tests/*.elf; do
    name=$(basename "$elf" .elf)
    if ! msg=$("$tmp/build/release/rvsim-iss" "$elf" 2>&1); then
      echo "$name: $msg"
    fi
  done
} | tee "$out"
