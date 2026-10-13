#include "common/memory.h"

#include <gtest/gtest.h>

#include <vector>

using rvx::Memory;

TEST(Memory, UntouchedReadsZeroWithoutAllocating) {
  Memory m;
  EXPECT_EQ(m.load(0x80000000, 4), 0u);
  EXPECT_EQ(m.load(0xFFFFFFFC, 4), 0u);
  EXPECT_EQ(m.load8(0x1234), 0u);
  EXPECT_EQ(m.pages_allocated(), 0u);
}

TEST(Memory, LittleEndian) {
  Memory m;
  m.store(0x100, 4, 0x11223344);
  EXPECT_EQ(m.load8(0x100), 0x44);
  EXPECT_EQ(m.load8(0x101), 0x33);
  EXPECT_EQ(m.load8(0x102), 0x22);
  EXPECT_EQ(m.load8(0x103), 0x11);
  EXPECT_EQ(m.load(0x100, 2), 0x3344u);
  EXPECT_EQ(m.load(0x102, 2), 0x1122u);
  EXPECT_EQ(m.load(0x101, 1), 0x33u);
}

TEST(Memory, PartialStoresOnlyTouchTheirBytes) {
  Memory m;
  m.store(0x200, 4, 0xAABBCCDD);
  m.store(0x201, 1, 0x00);
  EXPECT_EQ(m.load(0x200, 4), 0xAABB00DDu);
  m.store(0x202, 2, 0x1234);
  EXPECT_EQ(m.load(0x200, 4), 0x123400DDu);
  // Upper bits of the value beyond `size` are ignored.
  m.store(0x200, 1, 0xFFFFFF77);
  EXPECT_EQ(m.load(0x200, 4), 0x12340077u);
}

TEST(Memory, AccessSpanningTwoPages) {
  Memory m;
  const uint32_t a = Memory::kPageSize - 2;
  m.store(a, 4, 0xDEADBEEF);
  EXPECT_EQ(m.load(a, 4), 0xDEADBEEFu);
  EXPECT_EQ(m.load(Memory::kPageSize, 2), 0xDEADu);
  EXPECT_EQ(m.pages_allocated(), 2u);
}

TEST(Memory, WriteBlock) {
  Memory m;
  std::vector<uint8_t> bytes(10000);
  for (size_t k = 0; k < bytes.size(); ++k) bytes[k] = static_cast<uint8_t>(k * 7);
  m.write_block(0x80000FF0, bytes);
  for (size_t k = 0; k < bytes.size(); ++k) {
    ASSERT_EQ(m.load8(0x80000FF0 + static_cast<uint32_t>(k)), bytes[k]) << k;
  }
}
