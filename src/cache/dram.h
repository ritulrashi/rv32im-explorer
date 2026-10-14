// Banked, open-page DRAM latency model (docs/phase3-design.md §4).
//
// Each bank remembers its open row. An access costs row_hit_cycles if that
// row is open, row_closed_cycles if the bank has no open row (only before its
// first access), and row_miss_cycles if another row is open (precharge +
// activate + CAS). The accessed row is open afterwards. No refresh, queueing
// or bank parallelism (see LIMITATIONS).
#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace rvx::cache {

struct DramConfig {
  unsigned banks = 4;
  unsigned row_bytes = 2048;
  unsigned row_hit_cycles = 20;
  unsigned row_closed_cycles = 30;
  unsigned row_miss_cycles = 40;
  unsigned bus_bytes_per_cycle = 8;
};

enum class RowOutcome : uint8_t { Hit, Closed, Miss };

struct DramStats {
  uint64_t row_hits = 0;
  uint64_t row_closed = 0;
  uint64_t row_misses = 0;
};

class Dram {
 public:
  explicit Dram(const DramConfig& cfg);

  // Row-interleaved mapping: consecutive rows go to consecutive banks.
  uint32_t bank_of(uint32_t addr) const { return (addr / cfg_.row_bytes) % cfg_.banks; }
  uint32_t row_of(uint32_t addr) const { return addr / (cfg_.row_bytes * cfg_.banks); }

  // Returns the access latency in cycles and updates the bank's open row.
  unsigned access(uint32_t addr, RowOutcome* outcome = nullptr);

  const DramConfig& config() const { return cfg_; }
  const DramStats& stats() const { return stats_; }

 private:
  DramConfig cfg_;
  std::vector<std::optional<uint32_t>> open_row_;
  DramStats stats_;
};

}  // namespace rvx::cache
