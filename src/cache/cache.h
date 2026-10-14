// Tag-only set-associative cache with true LRU (docs/phase3-design.md §3).
//
// It holds no data: values always come from the backing store. Its only job
// is to decide hit or miss, which line to evict, and whether that line was
// dirty. Write policy: write-back, write-allocate.
#pragma once

#include <cstdint>
#include <vector>

namespace rvx::cache {

struct Config {
  bool enabled = false;
  unsigned size_bytes = 4096;
  unsigned assoc = 2;
  unsigned line_bytes = 32;
};

struct AccessResult {
  bool hit = false;
  bool writeback = false;       // a dirty line was evicted
  uint32_t victim_line = 0;     // its line address, valid if writeback
};

struct CacheStats {
  uint64_t accesses = 0;
  uint64_t misses = 0;
  uint64_t writebacks = 0;
};

class Cache {
 public:
  explicit Cache(const Config& cfg);  // cfg must be valid (see pipe::validate)

  // Looks up `addr`; on a miss allocates the line (evicting the LRU way).
  // `is_write` marks the line dirty after the access.
  AccessResult access(uint32_t addr, bool is_write);

  bool contains(uint32_t addr) const;  // no side effects (tests)
  void invalidate_all();               // drops every line, dirty or not

  uint32_t line_bytes() const { return line_; }
  unsigned sets() const { return sets_; }
  const CacheStats& stats() const { return stats_; }

 private:
  struct Line {
    bool valid = false;
    bool dirty = false;
    uint32_t tag = 0;
    uint64_t last_use = 0;  // LRU: larger = more recently used
  };
  Line* set_begin(uint32_t set) { return &lines_[static_cast<size_t>(set) * assoc_]; }
  const Line* set_begin(uint32_t set) const { return &lines_[static_cast<size_t>(set) * assoc_]; }

  unsigned assoc_, sets_;
  uint32_t line_;
  std::vector<Line> lines_;
  uint64_t tick_ = 0;  // access counter for LRU stamps (deterministic)
  CacheStats stats_;
};

}  // namespace rvx::cache
