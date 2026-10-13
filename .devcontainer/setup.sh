#!/usr/bin/env bash
# Installs the full toolchain for all phases. Versions come from the
# Ubuntu 24.04 archive; scripts/env_report.sh records the exact versions
# next to every measured result.
set -euo pipefail
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
  build-essential cmake ninja-build clang lld llvm \
  gcc-riscv64-unknown-elf binutils-riscv64-unknown-elf \
  python3 python3-pip python3-venv \
  verilator \
  git ca-certificates
git submodule update --init --recursive || true
