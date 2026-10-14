// Branch predictor unit tests. Expected sequences are traced by hand in the
// comments: counter values are 0..3 (0,1 predict not-taken; 2,3 taken),
// initialised to 1 (weakly not-taken).
#include "bpred/predictor.h"

#include <gtest/gtest.h>

#include <vector>

using namespace rvx::bpred;

namespace {
Config make(Type t, unsigned entries, unsigned hist = 0) {
  Config c;
  c.type = t;
  c.entries = entries;
  c.history_bits = hist;
  return c;
}

// Runs a sequence of outcomes for one branch pc, predicting then training
// exactly as the pipeline does; returns the number of mispredictions.
int mispredicts(DirectionPredictor& p, uint32_t pc, const std::vector<bool>& outcomes) {
  int wrong = 0;
  for (bool taken : outcomes) {
    const uint32_t idx = p.index(pc);
    if (p.predict_at(idx) != taken) ++wrong;
    p.train(idx, taken);
  }
  return wrong;
}

std::vector<bool> alternating(int n) {
  std::vector<bool> v;
  for (int k = 0; k < n; ++k) v.push_back(k % 2 == 0);  // T, N, T, N, ...
  return v;
}
}  // namespace

TEST(StaticNT, AlwaysNotTaken) {
  DirectionPredictor p(make(Type::StaticNT, 1));
  for (int k = 0; k < 5; ++k) {
    EXPECT_FALSE(p.predict(0x100));
    p.train(p.index(0x100), true);
  }
}

TEST(Bimodal, CounterTransitionsAndSaturation) {
  DirectionPredictor p(make(Type::Bimodal, 16));
  const uint32_t i = p.index(0x40);  // (0x40 >> 2) & 15 = 0
  EXPECT_EQ(i, 0u);
  EXPECT_EQ(p.counter(i), 1);
  EXPECT_FALSE(p.predict_at(i));
  p.train(i, true);   // 1 -> 2
  EXPECT_EQ(p.counter(i), 2);
  EXPECT_TRUE(p.predict_at(i));
  p.train(i, true);   // 2 -> 3
  p.train(i, true);   // saturates at 3
  EXPECT_EQ(p.counter(i), 3);
  p.train(i, false);  // 3 -> 2: one not-taken does not flip the prediction
  EXPECT_TRUE(p.predict_at(i));
  p.train(i, false);  // 2 -> 1
  p.train(i, false);  // 1 -> 0
  p.train(i, false);  // saturates at 0
  EXPECT_EQ(p.counter(i), 0);
}

TEST(Bimodal, PcsThatShareLowBitsAlias) {
  DirectionPredictor p(make(Type::Bimodal, 16));
  EXPECT_EQ(p.index(0x04), p.index(0x44));  // (1 and 17) & 15 = 1
  p.train(p.index(0x04), true);
  EXPECT_TRUE(p.predict(0x44));  // trained by the other branch
}

TEST(Bimodal, LoopBranchPattern) {
  // 9 taken then 1 not-taken: wrong on the first (counter 1) and on the exit.
  DirectionPredictor p(make(Type::Bimodal, 16));
  std::vector<bool> loop(9, true);
  loop.push_back(false);
  EXPECT_EQ(mispredicts(p, 0x8, loop), 2);
}

TEST(Bimodal, AlternatingPatternDefeatsIt) {
  // T: counter 1 -> wrong -> 2. N: predicts T -> wrong -> 1. Repeats: all wrong.
  DirectionPredictor p(make(Type::Bimodal, 16));
  EXPECT_EQ(mispredicts(p, 0, alternating(20)), 20);
}

TEST(Gshare, IndexIsPcXorHistory) {
  DirectionPredictor p(make(Type::Gshare, 16, 4));
  EXPECT_EQ(p.history(), 0u);
  EXPECT_EQ(p.index(0x30), 12u);  // (0x30 >> 2) = 12, ^ 0
  p.train(p.index(0x30), true);   // history = 0b0001
  p.train(p.index(0x30), false);  // history = 0b0010
  p.train(p.index(0x30), true);   // history = 0b0101
  EXPECT_EQ(p.history(), 5u);
  EXPECT_EQ(p.index(0x30), 12u ^ 5u);
  for (int k = 0; k < 4; ++k) p.train(0, true);  // history keeps only 4 bits
  EXPECT_EQ(p.history(), 0xFu);
}

TEST(Gshare, LearnsAlternatingPatternAfterWarmup) {
  // pc 0, 2 history bits, so index = history. Hand trace (ctr[i], ghr):
  //  1: idx0 ctr1 -> N, actual T: wrong.  ctr0=2, ghr=01
  //  2: idx1 ctr1 -> N, actual N: right.  ctr1=0, ghr=10
  //  3: idx2 ctr1 -> N, actual T: wrong.  ctr2=2, ghr=01
  //  4: idx1 ctr0 -> N, actual N: right.  ghr=10
  //  5: idx2 ctr2 -> T, actual T: right.  ctr2=3, ghr=01
  //  from here idx alternates 1 (N) and 2 (T): always right.
  DirectionPredictor p(make(Type::Gshare, 16, 2));
  EXPECT_EQ(mispredicts(p, 0, alternating(20)), 2);
}

TEST(Btb, MissThenHitThenReplace) {
  Btb b(4);
  EXPECT_TRUE(b.enabled());
  EXPECT_FALSE(b.lookup(0x10));
  b.update(0x10, 0x80, BranchKind::Cond);
  auto e = b.lookup(0x10);
  ASSERT_TRUE(e);
  EXPECT_EQ(e->target, 0x80u);
  EXPECT_EQ(e->kind, BranchKind::Cond);
  // 0x20 maps to the same slot: (0x20 >> 2) & 3 = 0 = (0x10 >> 2) & 3.
  b.update(0x20, 0x100, BranchKind::Jal);
  EXPECT_FALSE(b.lookup(0x10));  // replaced
  ASSERT_TRUE(b.lookup(0x20));
  EXPECT_EQ(b.lookup(0x20)->kind, BranchKind::Jal);
}

TEST(Btb, FullTagsPreventAliasing) {
  Btb b(4);
  b.update(0x10, 0x80, BranchKind::Cond);
  EXPECT_FALSE(b.lookup(0x110));  // same slot, different pc
}

TEST(Btb, ZeroEntriesIsDisabled) {
  Btb b(0);
  EXPECT_FALSE(b.enabled());
  b.update(0x10, 0x80, BranchKind::Cond);
  EXPECT_FALSE(b.lookup(0x10));
}
