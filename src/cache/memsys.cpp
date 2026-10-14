#include "cache/memsys.h"

namespace rvx::cache {

MemSystem::MemSystem(const Config& icache, const Config& dcache, const DramConfig& dram) : dram_(dram) {
  if (icache.enabled) icache_.emplace(icache);
  if (dcache.enabled) dcache_.emplace(dcache);
}

unsigned MemSystem::miss_penalty(Cache& c, uint32_t addr, const AccessResult& r) {
  unsigned p = 0;
  if (r.writeback) p += dram_.access(r.victim_line);  // write the victim back first
  const uint32_t line_addr = addr & ~(c.line_bytes() - 1);
  p += dram_.access(line_addr);
  p += c.line_bytes() / dram_.config().bus_bytes_per_cycle;
  return p;
}

unsigned MemSystem::fetch(uint32_t pc) {
  if (!icache_) return 0;
  const AccessResult r = icache_->access(pc, false);
  return r.hit ? 0 : miss_penalty(*icache_, pc, r);
}

unsigned MemSystem::data(uint32_t addr, bool is_write) {
  if (!dcache_) return 0;
  const AccessResult r = dcache_->access(addr, is_write);
  return r.hit ? 0 : miss_penalty(*dcache_, addr, r);
}

void MemSystem::invalidate_icache() {
  if (icache_) icache_->invalidate_all();
}

}  // namespace rvx::cache
