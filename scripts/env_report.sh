#!/usr/bin/env bash
# Prints the provenance block that must accompany every reported result:
# time, commit, tree cleanliness, submodule pins, machine, and tool versions.
set -uo pipefail
cd "$(dirname "$0")/.."

echo "date_utc: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
echo "commit: $(git rev-parse HEAD 2>/dev/null || echo unknown)"
if ! git rev-parse --git-dir >/dev/null 2>&1; then
  echo "tree: NOT A GIT CHECKOUT (results not attributable to any commit)"
elif [ -n "$(git status --porcelain --ignore-submodules=none)" ]; then
  echo "tree: DIRTY (results not attributable to the commit above)"
else
  echo "tree: clean"
fi
echo "riscv-tests: $(git -C third_party/riscv-tests rev-parse HEAD 2>/dev/null || echo missing)"
echo "host: $(uname -srm)"
echo "os: $(. /etc/os-release && echo "$PRETTY_NAME")"
echo "lscpu:"
lscpu | grep -E '^(Architecture|Model name|CPU\(s\)|Thread\(s\) per core|Core\(s\) per socket|Socket\(s\)|CPU max MHz|L1d|L1i|L2|L3)' | sed 's/^/  /'
echo "mem_total: $(grep MemTotal /proc/meminfo | awk '{print $2 " kB"}')"
echo "tools:"
echo "  c++: $(c++ --version | head -1)"
echo "  cmake: $(cmake --version | head -1)"
echo "  ninja: $(ninja --version)"
echo "  riscv-gcc: $(riscv64-unknown-elf-gcc --version | head -1)"
echo "  riscv-binutils: $(riscv64-unknown-elf-as --version | head -1)"
echo "  clang: $(clang --version 2>/dev/null | head -1 || echo not installed)"
echo "  python3: $(python3 --version 2>&1)"
echo "  verilator: $(verilator --version 2>/dev/null || echo not installed)"
