// Sparse, byte-addressable, little-endian 32-bit memory for the golden ISS.
//
// Backed by 4 KiB pages allocated on first write. Bytes that were never
// written read as zero, so the full 4 GiB address space is valid and a run's
// result never depends on uninitialised host memory.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>

namespace rvx::iss {

class Memory {
 public:
  static constexpr uint32_t kPageBits = 12;
  static constexpr uint32_t kPageSize = 1u << kPageBits;

  // size must be 1, 2 or 4. No alignment requirement here; alignment policy
  // belongs to the ISS. Addresses wrap modulo 2^32.
  uint32_t load(uint32_t addr, unsigned size) const;
  void store(uint32_t addr, unsigned size, uint32_t value);

  uint8_t load8(uint32_t addr) const;
  void store8(uint32_t addr, uint8_t value);
  void write_block(uint32_t addr, std::span<const uint8_t> bytes);

  size_t pages_allocated() const { return pages_.size(); }

 private:
  using Page = std::array<uint8_t, kPageSize>;
  // Only ever looked up by key, never iterated, so unordered_map's iteration
  // order cannot leak into results.
  std::unordered_map<uint32_t, std::unique_ptr<Page>> pages_;
};

}  // namespace rvx::iss
