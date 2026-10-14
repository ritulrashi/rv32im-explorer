#include "cache/cache.h"

namespace rvx::cache {

Cache::Cache(const Config& cfg)
    : assoc_(cfg.assoc),
      sets_(cfg.size_bytes / (cfg.assoc * cfg.line_bytes)),
      line_(cfg.line_bytes),
      lines_(static_cast<size_t>(sets_) * assoc_) {}

AccessResult Cache::access(uint32_t addr, bool is_write) {
  ++tick_;
  ++stats_.accesses;
  const uint32_t line_index = addr / line_;
  const uint32_t set = line_index % sets_;
  const uint32_t tag = line_index / sets_;
  Line* ways = set_begin(set);

  for (unsigned w = 0; w < assoc_; ++w) {
    if (ways[w].valid && ways[w].tag == tag) {
      ways[w].last_use = tick_;
      if (is_write) ways[w].dirty = true;
      return {true, false, 0};
    }
  }

  // Miss: prefer an invalid way, otherwise the least recently used one.
  ++stats_.misses;
  Line* victim = nullptr;
  for (unsigned w = 0; w < assoc_ && !victim; ++w) {
    if (!ways[w].valid) victim = &ways[w];
  }
  if (!victim) {
    victim = &ways[0];
    for (unsigned w = 1; w < assoc_; ++w) {
      if (ways[w].last_use < victim->last_use) victim = &ways[w];
    }
  }
  AccessResult r;
  if (victim->valid && victim->dirty) {
    r.writeback = true;
    r.victim_line = (victim->tag * sets_ + set) * line_;
    ++stats_.writebacks;
  }
  *victim = Line{true, is_write, tag, tick_};
  return r;
}

bool Cache::contains(uint32_t addr) const {
  const uint32_t line_index = addr / line_;
  const Line* ways = set_begin(line_index % sets_);
  for (unsigned w = 0; w < assoc_; ++w) {
    if (ways[w].valid && ways[w].tag == line_index / sets_) return true;
  }
  return false;
}

void Cache::invalidate_all() {
  for (auto& l : lines_) l = Line{};
}

}  // namespace rvx::cache
