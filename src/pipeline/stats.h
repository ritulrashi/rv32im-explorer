// Pipeline statistics and the CPI stack (docs/phase3-design.md §5).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <string>
#include <string_view>

#include "cache/cache.h"
#include "cache/dram.h"

namespace rvx::pipe {

// Every simulated cycle is charged to exactly one category, according to what
// occupies WB in that cycle (retire-slot accounting).
enum class CpiCat : uint8_t { Base, LoadUse, Branch, ICache, DCache, MulDiv };
constexpr size_t kCpiCats = 6;
std::string_view to_string(CpiCat c);

struct PipeStats {
  std::array<uint64_t, kCpiCats> cpi{};

  // Breakdown of CpiCat::Branch (these three sum to cpi[Branch]).
  uint64_t branch_mispredict_cycles = 0;
  uint64_t branch_id_redirect_cycles = 0;
  uint64_t branch_fence_i_cycles = 0;

  // Events (counted when the instruction leaves EX, i.e. on the correct path).
  uint64_t cond_branches = 0;
  uint64_t cond_mispredicts = 0;
  uint64_t jumps = 0;             // JAL + JALR
  uint64_t jump_mispredicts = 0;
  // Front-end events (counted when the instruction is written into IF/ID or
  // ID/EX; may include wrong-path instructions flushed later).
  uint64_t btb_hits = 0;
  uint64_t id_redirects = 0;
  // Retired memory operations (counted in WB).
  uint64_t loads = 0;
  uint64_t stores = 0;

  uint64_t operator[](CpiCat c) const { return cpi[static_cast<size_t>(c)]; }
  uint64_t cpi_total() const { return std::accumulate(cpi.begin(), cpi.end(), uint64_t{0}); }
};

// Every counter at one instant. Differences of two snapshots give the
// statistics of a region (docs/phase4-design.md §1.4). Because each snapshot's
// CPI stack sums to its cycle count, so does the difference.
struct CounterSnapshot {
  uint64_t cycles = 0;
  uint64_t retired = 0;
  PipeStats pipe;
  cache::CacheStats icache, dcache;
  cache::DramStats dram;
};
CounterSnapshot operator-(const CounterSnapshot& a, const CounterSnapshot& b);

// Region of interest, delimited by stores of 1 (begin) and 2 (end) to the
// marker address retiring in WB. The first begin and the first end after it
// are used; any further markers are ignored.
struct RoiStats {
  bool begun = false;
  bool ended = false;
  CounterSnapshot begin, end;
  bool complete() const { return begun && ended; }
  CounterSnapshot delta() const { return end - begin; }
};

}  // namespace rvx::pipe
