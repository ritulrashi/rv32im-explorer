// Program loaders: ELF32 (little-endian, RISC-V) and flat binary.
//
// The ELF loader is hand-written and deliberately minimal: it copies PT_LOAD
// segments to their physical addresses, zero-fills BSS, and reads the symbol
// table so callers can find `tohost`. Every offset is bounds-checked; a
// malformed file throws LoadError rather than reading out of range.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "iss/memory.h"

namespace rvx::iss {

struct LoadError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

struct LoadResult {
  uint32_t entry = 0;
  // std::map, not unordered_map: deterministic iteration if anyone dumps it.
  std::map<std::string, uint32_t> symbols;

  std::optional<uint32_t> symbol(const std::string& name) const {
    auto it = symbols.find(name);
    if (it == symbols.end()) return std::nullopt;
    return it->second;
  }
};

LoadResult load_elf(std::span<const uint8_t> image, Memory& mem);
LoadResult load_elf_file(const std::string& path, Memory& mem);

// Copies the whole file to `base`. Entry = base.
LoadResult load_flat_file(const std::string& path, uint32_t base, Memory& mem);

std::vector<uint8_t> read_file(const std::string& path);

}  // namespace rvx::iss
