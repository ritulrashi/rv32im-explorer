// Cache, DRAM and miss-penalty unit tests. Expected outcomes are traced by
// hand in the comments.
#include <gtest/gtest.h>

#include "cache/cache.h"
#include "cache/dram.h"
#include "cache/memsys.h"

using namespace rvx::cache;

namespace {
Config small() {  // 64 B, 2-way, 16 B lines -> 2 sets; set = (addr / 16) % 2
  Config c;
  c.enabled = true;
  c.size_bytes = 64;
  c.assoc = 2;
  c.line_bytes = 16;
  return c;
}
}  // namespace

TEST(Cache, Geometry) {
  Cache c(small());
  EXPECT_EQ(c.sets(), 2u);
  EXPECT_EQ(c.line_bytes(), 16u);
}

TEST(Cache, HandTracedLruAndWritebackSequence) {
  Cache c(small());
  struct Step { uint32_t addr; bool write; bool hit; bool wb; uint32_t victim; };
  // Set 0 lines are 0x00, 0x20, 0x40, 0x60, 0x80 ...; set 1 lines 0x10, 0x30 ...
  const Step steps[] = {
      {0x00, false, false, false, 0},     //  1 miss, fill way A
      {0x20, false, false, false, 0},     //  2 miss, fill way B
      {0x04, false, true, false, 0},      //  3 hit 0x00; LRU in set 0 is now 0x20
      {0x40, false, false, false, 0},     //  4 miss, evict 0x20 (clean)
      {0x08, true, true, false, 0},       //  5 hit 0x00, now dirty
      {0x60, false, false, false, 0},     //  6 miss, LRU of {0x00@5, 0x40@4} = 0x40 (clean)
      {0x80, false, false, true, 0x00},   //  7 miss, LRU of {0x00@5, 0x60@6} = 0x00 (dirty)
      {0x10, false, false, false, 0},     //  8 set 1 miss, empty way
      {0x14, true, true, false, 0},       //  9 hit, dirty
      {0x00, false, false, false, 0},     // 10 miss, LRU of {0x60@6, 0x80@7} = 0x60 (clean)
  };
  int k = 1;
  for (const auto& s : steps) {
    auto r = c.access(s.addr, s.write);
    EXPECT_EQ(r.hit, s.hit) << "step " << k;
    EXPECT_EQ(r.writeback, s.wb) << "step " << k;
    if (s.wb) {
      EXPECT_EQ(r.victim_line, s.victim) << "step " << k;
    }
    ++k;
  }
  EXPECT_EQ(c.stats().accesses, 10u);
  EXPECT_EQ(c.stats().misses, 7u);
  EXPECT_EQ(c.stats().writebacks, 1u);
  EXPECT_TRUE(c.contains(0x80));
  EXPECT_TRUE(c.contains(0x00));
  EXPECT_FALSE(c.contains(0x60));
}

TEST(Cache, WriteAllocateMakesLineDirty) {
  Cache c(small());
  EXPECT_FALSE(c.access(0x00, true).hit);  // store miss allocates
  EXPECT_TRUE(c.contains(0x00));
  c.access(0x20, false);
  auto r = c.access(0x40, false);           // evicts 0x00 (LRU), which is dirty
  EXPECT_TRUE(r.writeback);
  EXPECT_EQ(r.victim_line, 0x00u);
}

TEST(Cache, DirectMappedConflicts) {
  Config cfg = small();
  cfg.assoc = 1;  // 64 B / 16 B = 4 sets
  Cache c(cfg);
  EXPECT_FALSE(c.access(0x00, false).hit);
  EXPECT_FALSE(c.access(0x40, false).hit);  // same set (0x40/16 % 4 = 0)
  EXPECT_FALSE(c.access(0x00, false).hit);  // evicted by 0x40
}

TEST(Cache, InvalidateAll) {
  Cache c(small());
  c.access(0x00, true);
  c.access(0x10, false);
  c.invalidate_all();
  EXPECT_FALSE(c.contains(0x00));
  EXPECT_FALSE(c.contains(0x10));
  auto r = c.access(0x00, false);
  EXPECT_FALSE(r.hit);
  EXPECT_FALSE(r.writeback);  // dropped, not written back
}

TEST(Dram, HandTracedRowBufferSequence) {
  // 4 banks, 2048-byte rows: bank = (addr / 2048) % 4, row = addr / 8192.
  Dram d(DramConfig{});
  RowOutcome o;
  EXPECT_EQ(d.access(0x0000, &o), 30u);  // bank 0 closed
  EXPECT_EQ(o, RowOutcome::Closed);
  EXPECT_EQ(d.access(0x0100, &o), 20u);  // bank 0 row 0 open: hit
  EXPECT_EQ(o, RowOutcome::Hit);
  EXPECT_EQ(d.access(0x0800, &o), 30u);  // bank 1 closed
  EXPECT_EQ(d.access(0x2000, &o), 40u);  // bank 0, row 1: miss
  EXPECT_EQ(o, RowOutcome::Miss);
  EXPECT_EQ(d.access(0x0000, &o), 40u);  // bank 0 back to row 0: miss
  EXPECT_EQ(d.access(0x0810, &o), 20u);  // bank 1 row 0 still open: hit
  EXPECT_EQ(d.stats().row_hits, 2u);
  EXPECT_EQ(d.stats().row_closed, 2u);
  EXPECT_EQ(d.stats().row_misses, 2u);
  EXPECT_EQ(d.bank_of(0x2000), 0u);
  EXPECT_EQ(d.row_of(0x2000), 1u);
}

TEST(MemSystem, DisabledCachesCostNothingAndTouchNoDram) {
  MemSystem m(Config{}, Config{}, DramConfig{});
  EXPECT_EQ(m.fetch(0x0), 0u);
  EXPECT_EQ(m.data(0x400, true), 0u);
  EXPECT_EQ(m.icache(), nullptr);
  EXPECT_EQ(m.dram().stats().row_closed + m.dram().stats().row_hits + m.dram().stats().row_misses, 0u);
}

TEST(MemSystem, MissPenaltyIncludesWritebackFillAndTransfer) {
  // Direct-mapped 32 B D-cache with 16 B lines: 2 sets. Transfer = 16/8 = 2.
  Config d;
  d.enabled = true;
  d.size_bytes = 32;
  d.assoc = 1;
  d.line_bytes = 16;
  MemSystem m(Config{}, d, DramConfig{});
  EXPECT_EQ(m.data(0x400, true), 30u + 2u);           // closed row + transfer; line now dirty
  EXPECT_EQ(m.data(0x404, false), 0u);                // hit
  // 0x2400: set (0x2400/16)%2 = 0 -> evicts dirty 0x400.
  //   write-back 0x400: bank 0 row 0 open -> 20
  //   fill 0x2400:      bank 0 row 1      -> 40
  //   transfer                             ->  2
  EXPECT_EQ(m.data(0x2400, false), 20u + 40u + 2u);
  EXPECT_EQ(m.dcache()->stats().writebacks, 1u);
}

TEST(MemSystem, FenceIInvalidatesOnlyTheICache) {
  Config i;
  i.enabled = true;
  i.size_bytes = 64;
  i.line_bytes = 16;
  MemSystem m(i, Config{}, DramConfig{});
  EXPECT_EQ(m.fetch(0x0), 32u);  // closed row 30 + transfer 2
  EXPECT_EQ(m.fetch(0x4), 0u);
  m.invalidate_icache();
  EXPECT_EQ(m.fetch(0x4), 22u);  // row hit 20 + transfer 2
}
