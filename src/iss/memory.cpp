#include "iss/memory.h"

#include <cassert>

namespace rvx::iss {

uint8_t Memory::load8(uint32_t addr) const {
  auto it = pages_.find(addr >> kPageBits);
  if (it == pages_.end()) return 0;
  return (*it->second)[addr & (kPageSize - 1)];
}

void Memory::store8(uint32_t addr, uint8_t value) {
  auto& page = pages_[addr >> kPageBits];
  if (!page) page = std::make_unique<Page>();  // value-initialised: all zero
  (*page)[addr & (kPageSize - 1)] = value;
}

// Byte-at-a-time is slower than a word access, but it makes cross-page and
// unaligned accesses correct with no special cases. Speed is not a goal here.
uint32_t Memory::load(uint32_t addr, unsigned size) const {
  assert(size == 1 || size == 2 || size == 4);
  uint32_t v = 0;
  for (unsigned k = 0; k < size; ++k) v |= uint32_t{load8(addr + k)} << (8 * k);
  return v;
}

void Memory::store(uint32_t addr, unsigned size, uint32_t value) {
  assert(size == 1 || size == 2 || size == 4);
  for (unsigned k = 0; k < size; ++k) store8(addr + k, static_cast<uint8_t>(value >> (8 * k)));
}

void Memory::write_block(uint32_t addr, std::span<const uint8_t> bytes) {
  for (size_t k = 0; k < bytes.size(); ++k) store8(addr + static_cast<uint32_t>(k), bytes[k]);
}

}  // namespace rvx::iss
