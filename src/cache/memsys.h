// L1 I/D caches in front of the DRAM model. Answers one question per access:
// how many extra cycles does it cost? (0 = hit, or the cache is disabled.)
//
// Miss penalty (docs/phase3-design.md §3.2):
//   P = [dirty victim ? dram(victim) : 0] + dram(line) + line_bytes / bus_bytes_per_cycle
#pragma once

#include <cstdint>
#include <optional>

#include "cache/cache.h"
#include "cache/dram.h"

namespace rvx::cache {

class MemSystem {
 public:
  MemSystem(const Config& icache, const Config& dcache, const DramConfig& dram);

  unsigned fetch(uint32_t pc);
  unsigned data(uint32_t addr, bool is_write);
  void invalidate_icache();  // FENCE.I

  const Cache* icache() const { return icache_ ? &*icache_ : nullptr; }
  const Cache* dcache() const { return dcache_ ? &*dcache_ : nullptr; }
  const Dram& dram() const { return dram_; }

 private:
  unsigned miss_penalty(Cache& c, uint32_t addr, const AccessResult& r);

  std::optional<Cache> icache_, dcache_;
  Dram dram_;
};

}  // namespace rvx::cache
