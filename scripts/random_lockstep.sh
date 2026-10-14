#!/usr/bin/env bash
# Generates random programs with rvgen, assembles them with GNU as, and runs
# each one in lockstep (ISS vs pipeline).
#
#   scripts/random_lockstep.sh BUILD_DIR FIRST_SEED LAST_SEED [WORK_DIR] [-- LOCKSTEP_ARGS...]
#
# Prints one line per failing seed and a final summary line:
#   random-lockstep: seeds A-B programs=N agree=N diverge=N incomplete=N other=N retirements_compared=N
# retirements_compared is the total number of retired instructions that were
# compared record-by-record across all agreeing programs.
# Exit status is 0 only if every program agreed. Artifacts (.S, .elf, .log)
# of failing seeds are kept in WORK_DIR for debugging.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
build=$(cd "$1" && pwd)
first=$2
last=$3
work=${4:-$(mktemp -d)}
shift $(( $# >= 4 ? 4 : $# ))
[ "${1:-}" = "--" ] && shift
extra=("$@")
mkdir -p "$work"

export RVGEN="$build/rvgen"
export LOCKSTEP="$build/rvsim-lockstep"
export CC="riscv64-unknown-elf-gcc"
export CFLAGS="-march=rv32im_zifencei -mabi=ilp32 -mcmodel=medany -static -nostdlib -nostartfiles -T$here/tests/riscv-tests-env/link.ld"
export WORK="$work"
export EXTRA="${extra[*]:-}"

one() {
  local s=$1 base="$WORK/seed-$1" rc=0 retired
  "$RVGEN" --seed "$s" > "$base.S"
  # shellcheck disable=SC2086
  if ! $CC $CFLAGS "$base.S" -o "$base.elf" 2> "$base.asm.log"; then
    echo "$s other assemble-failed"
    return
  fi
  # shellcheck disable=SC2086
  "$LOCKSTEP" "$base.elf" $EXTRA > "$base.log" 2>&1 || rc=$?
  case $rc in
    0)
      retired=$(grep -oE 'retired=[0-9]+' "$base.log" | cut -d= -f2)
      echo "$s agree $retired"
      rm -f "$base.S" "$base.elf" "$base.log" "$base.asm.log"
      ;;
    1) echo "$s diverge" ;;
    2) echo "$s incomplete" ;;
    *) echo "$s other rc=$rc" ;;
  esac
}
export -f one

results=$(seq "$first" "$last" | xargs -P "$(nproc)" -I{} bash -c 'one {}' | sort -n)
total=$(echo "$results" | wc -l)
agree=$(echo "$results" | awk '$2 == "agree"' | wc -l)
diverge=$(echo "$results" | awk '$2 == "diverge"' | wc -l)
incomplete=$(echo "$results" | awk '$2 == "incomplete"' | wc -l)
other=$((total - agree - diverge - incomplete))
compared=$(echo "$results" | awk '$2 == "agree" { s += $3 } END { print s + 0 }')
echo "$results" | awk '$2 != "agree" { print "  failing seed: " $0 }'
echo "random-lockstep: seeds $first-$last programs=$total agree=$agree diverge=$diverge incomplete=$incomplete other=$other retirements_compared=$compared"
[ "$agree" -eq "$total" ]
