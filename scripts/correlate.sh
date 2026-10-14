#!/usr/bin/env bash
# RTL vs C++ model correlation (docs/phase5-design.md §5).
#
#   scripts/correlate.sh BUILD_DIR OUT_DIR [FIRST_SEED LAST_SEED]
#
# Programs: directed contract programs (BUILD_DIR/tests/rtl/*.elf), the rv32ui
# riscv-tests, and rvgen --isa rv32i programs for the given seed range.
# For each program, rvsim-rtl and rvsim-pipe --config configs/baseline.cfg are
# compared at three levels:
#   1. summary: halt reason, retired count, total cycles, halt pc
#   2. retire trace (byte-identical)
#   3. per-cycle stage diagram (byte-identical)
# Directed programs are also checked against the "# expect:" line in their
# source (hand-derived from the contract), and t26 against its hand-drawn
# diagram. One line per program goes to OUT_DIR/programs.txt; artefacts of
# mismatching programs are kept in OUT_DIR. Exit status 0 only if all match.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
build=$(cd "$1" && pwd)
out=$2
first=${3:-1}
last=${4:-0}
mkdir -p "$out"
out=$(cd "$out" && pwd)

export HERE="$here" BUILD="$build" OUT="$out"
export CC="riscv64-unknown-elf-gcc"
# rv32i (no M) so the assembler itself rejects any M instruction.
export CFLAGS="-march=rv32i_zifencei -mabi=ilp32 -mcmodel=medany -static -nostdlib -nostartfiles -T$here/tests/riscv-tests-env/link.ld"

summary_of() {  # stderr summary line -> "halt=.. retired=.. cycles=.. pc=.."
  grep -oE 'halt=[^ ]+ retired=[0-9]+ cycles=[0-9]+ pc=0x[0-9a-f]+' "$1" || echo "no-summary"
}
export -f summary_of

one() {  # $1 = kind (directed|riscv|random), $2 = elf
  local kind=$1 elf=$2 name base rtl_rc=0 model_rc=0
  name=$(basename "$elf" .elf)
  base="$OUT/$kind-$name"
  # Budget far above any correlation program (they run thousands of cycles),
  # so a hung side (e.g. a broken RTL looping forever) fails fast as a
  # summary mismatch instead of grinding through the default 100M cycles.
  # stdout is captured too: Verilator prints $error/assertion messages there,
  # and they must not leak into this function's one-line result (BUGS.md #6).
  "$BUILD/rvsim-rtl" "$elf" --max-cycles 1000000 --trace "$base.rtl.trace" --diagram "$base.rtl.diag" \
    > "$base.rtl.out" 2> "$base.rtl.err" || rtl_rc=$?
  "$BUILD/rvsim-pipe" "$elf" --config "$HERE/configs/baseline.cfg" --max-cycles 1000000 \
    --trace "$base.model.trace" --diagram "$base.model.diag" > "$base.model.out" 2> "$base.model.err" || model_rc=$?
  local rs ms
  rs=$(summary_of "$base.rtl.err")
  ms=$(summary_of "$base.model.err")
  local verdict="match" detail=""
  if [ "$rs" != "$ms" ]; then
    verdict="MISMATCH-1-summary"
    detail="rtl: $rs | model: $ms"
  elif ! cmp -s "$base.rtl.trace" "$base.model.trace"; then
    verdict="MISMATCH-2-trace"
    detail=$(diff "$base.model.trace" "$base.rtl.trace" | head -4 | tr '\n' ' ')
  elif ! cmp -s "$base.rtl.diag" "$base.model.diag"; then
    verdict="MISMATCH-3-diagram"
    detail=$(diff "$base.model.diag" "$base.rtl.diag" | head -4 | tr '\n' ' ')
  fi
  if [ "$kind" = directed ] && [ "$verdict" = match ]; then
    local src="$HERE/tests/rtl/$name.S" exp
    exp=$(grep -oE 'expect: halt=[^ ]+ retired=[0-9]+ cycles=[0-9]+' "$src" | sed 's/expect: //')
    if [ "${rs% pc=*}" != "$exp" ]; then
      verdict="MISMATCH-hand-derivation"
      detail="expected: $exp | both models: ${rs% pc=*}"
    fi
    if [ -f "$HERE/tests/rtl/$name.expected" ] && ! cmp -s "$base.rtl.diag" "$HERE/tests/rtl/$name.expected"; then
      verdict="MISMATCH-hand-diagram"
      detail=$(diff "$HERE/tests/rtl/$name.expected" "$base.rtl.diag" | head -4 | tr '\n' ' ')
    fi
  fi
  local cycles
  cycles=$(echo "$rs" | grep -oE 'cycles=[0-9]+' | cut -d= -f2 || echo 0)
  if [ "$verdict" = match ]; then
    rm -f "$base".*
  fi
  echo "$kind $name $verdict cycles=${cycles:-0} rtl_rc=$rtl_rc model_rc=$model_rc $detail"
}
export -f one

gen_random() {  # $1 = seed -> prints the elf path (or nothing if assembly failed)
  local s=$1 b="$OUT/gen/rv32i-seed-$1"
  "$BUILD/rvgen" --seed "$s" --isa rv32i > "$b.S"
  # shellcheck disable=SC2086
  if $CC $CFLAGS "$b.S" -o "$b.elf" 2> "$b.asm.log"; then echo "$b.elf"; else echo "ASM-FAIL $s" >&2; fi
}
export -f gen_random

mkdir -p "$out/gen"
{
  for e in "$build"/tests/rtl/*.elf; do echo "directed $e"; done
  for e in "$build"/tests/riscv-tests/rv32ui-*.elf; do echo "riscv $e"; done
  if [ "$last" -ge "$first" ]; then
    seq "$first" "$last" | xargs -P "$(nproc)" -I{} bash -c 'gen_random {}' | sort -V | sed 's/^/random /'
  fi
} > "$out/jobs.txt"

xargs -P "$(nproc)" -L 1 bash -c 'one "$0" "$1"' < "$out/jobs.txt" | sort > "$out/programs.txt"

total=$(wc -l < "$out/programs.txt")
matched=$(awk '$3 == "match"' "$out/programs.txt" | wc -l)
cycles=$(awk '$3 == "match" { split($4, a, "="); s += a[2] } END { print s + 0 }' "$out/programs.txt")
for k in directed riscv random; do
  n=$(awk -v k=$k '$1 == k' "$out/programs.txt" | wc -l)
  m=$(awk -v k=$k '$1 == k && $3 == "match"' "$out/programs.txt" | wc -l)
  echo "correlate: $k: $m of $n programs match at all levels"
done
awk '$3 != "match" { print "  " $0 }' "$out/programs.txt"
echo "correlate: total: $matched of $total programs match; cycles compared stage-by-stage in matching programs: $cycles"
[ "$matched" -eq "$total" ]
