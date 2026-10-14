#include "cache/dram.h"

namespace rvx::cache {

Dram::Dram(const DramConfig& cfg) : cfg_(cfg), open_row_(cfg.banks) {}

unsigned Dram::access(uint32_t addr, RowOutcome* outcome) {
  auto& open = open_row_[bank_of(addr)];
  const uint32_t row = row_of(addr);
  RowOutcome o;
  unsigned latency;
  if (!open) {
    o = RowOutcome::Closed;
    latency = cfg_.row_closed_cycles;
    ++stats_.row_closed;
  } else if (*open == row) {
    o = RowOutcome::Hit;
    latency = cfg_.row_hit_cycles;
    ++stats_.row_hits;
  } else {
    o = RowOutcome::Miss;
    latency = cfg_.row_miss_cycles;
    ++stats_.row_misses;
  }
  open = row;  // open-page policy: the row stays open
  if (outcome) *outcome = o;
  return latency;
}

}  // namespace rvx::cache
