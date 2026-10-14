// Phase 3 pipeline integration tests: branch prediction, caches, DRAM and the
// CPI stack. Every expected total and every CPI-stack component is derived by
// hand in the comments, from docs/phase3-design.md and the Phase 2 contract.
//
// Conventions as in pipeline_timing_test.cpp: program at 0, data at 0x400,
// END = `sw x0, 0x7F0(x0)` (tohost = 0x7F0). "base" = retired + 4 fill cycles.
// DRAM defaults: row hit 20, closed 30, miss 40; 4 banks x 2048-byte rows;
// 8 bytes/cycle, so a 16-byte line transfers in 2 cycles. Every address used
// here is below 0x800, i.e. bank 0, row 0, unless stated otherwise.
#include <gtest/gtest.h>

#include <initializer_list>
#include <memory>

#include "encode.h"
#include "pipeline/pipeline.h"

using rvx::HaltReason;
using rvx::Memory;
using rvx::pipe::Config;
using rvx::pipe::CpiCat;
using rvx::pipe::Pipeline;

namespace {

constexpr uint32_t kToHost = 0x7F0;
const uint32_t END = enc::sw(0, 0, static_cast<int32_t>(kToHost));
const uint32_t NOP = enc::addi(0, 0, 0);
constexpr uint32_t FENCE_I = 0x0000100F;

struct Stack {
  uint64_t base, load_use, branch, icache, dcache, muldiv;
};

class P3 : public ::testing::Test {
 public:  // loop_program() fills the program from outside the fixture
  Memory mem;
  Config cfg;
  std::unique_ptr<Pipeline> p;

  void prog(std::initializer_list<uint32_t> words, uint32_t at = 0) {
    for (uint32_t w : words) {
      mem.store(at, 4, w);
      at += 4;
    }
  }
  void enable_icache() {
    cfg.icache.enabled = true;
    cfg.icache.size_bytes = 1024;
    cfg.icache.assoc = 2;
    cfg.icache.line_bytes = 16;
  }
  void enable_dcache(unsigned size = 1024, unsigned assoc = 2) {
    cfg.dcache.enabled = true;
    cfg.dcache.size_bytes = size;
    cfg.dcache.assoc = assoc;
    cfg.dcache.line_bytes = 16;
  }
  // Runs to tohost and checks total cycles and every CPI-stack component.
  void expect(uint64_t cycles, Stack s) {
    p = std::make_unique<Pipeline>(mem, 0, cfg);
    p->set_tohost(kToHost);
    auto h = p->run(100000);
    ASSERT_EQ(h.reason, HaltReason::ToHost) << rvx::to_string(h.reason);
    const auto& st = p->stats();
    EXPECT_EQ(p->cycles(), cycles);
    EXPECT_EQ(st[CpiCat::Base], s.base);
    EXPECT_EQ(st[CpiCat::LoadUse], s.load_use);
    EXPECT_EQ(st[CpiCat::Branch], s.branch);
    EXPECT_EQ(st[CpiCat::ICache], s.icache);
    EXPECT_EQ(st[CpiCat::DCache], s.dcache);
    EXPECT_EQ(st[CpiCat::MulDiv], s.muldiv);
    // The CPI stack must account for every cycle exactly once.
    EXPECT_EQ(st.cpi_total(), p->cycles());
  }
};

// 0x0 addi x1, x0, 10
// 0x4 addi x1, x1, -1     <- L
// 0x8 bne  x1, x0, L      taken 9 times, then falls through
// 0xC END
// Retired = 1 + 10*2 + 1 = 22, base = 26.
void loop_program(P3& t) {
  t.prog({enc::addi(1, 0, 10), enc::addi(1, 1, -1), enc::b(-4, 0, 1, 1, enc::BRANCH), END});
}

}  // namespace

// --- Branch prediction (design note §2.3) ------------------------------------

TEST_F(P3, LoopStaticNotTaken) {
  // 9 taken branches x 2-cycle EX redirect = 18 (identical to Phase 2).
  loop_program(*this);
  expect(26 + 18, {26, 0, 18, 0, 0, 0});
  EXPECT_EQ(p->stats().cond_branches, 10u);
  EXPECT_EQ(p->stats().cond_mispredicts, 9u);
  EXPECT_EQ(p->stats().branch_mispredict_cycles, 18u);
}

TEST_F(P3, LoopBimodalNoBtb) {
  // iter 1: counter 1 -> NT, actually T: EX mispredict, 2.
  // iters 2-9: counter >= 2 -> T, ID redirect: 8 x 1.
  // iter 10: T, actually NT: ID redirect then EX mispredict. The mispredict
  //   flushes the ID-redirect bubble, so only 2 bubbles reach WB, both Redirect.
  // branch = 2 + 8 + 2 = 12.
  loop_program(*this);
  cfg.bpred.type = rvx::bpred::Type::Bimodal;
  expect(26 + 12, {26, 0, 12, 0, 0, 0});
  EXPECT_EQ(p->stats().cond_mispredicts, 2u);
  EXPECT_EQ(p->stats().branch_mispredict_cycles, 4u);
  EXPECT_EQ(p->stats().branch_id_redirect_cycles, 8u);
}

TEST_F(P3, LoopBimodalWithBtb) {
  // iter 1: BTB miss, counter says NT: EX mispredict 2 (BTB allocated).
  // iters 2-9: BTB hit + counter T: fetch redirected in IF, 0 bubbles.
  // iter 10: predicted T, actually NT: 2. branch = 4.
  loop_program(*this);
  cfg.bpred.type = rvx::bpred::Type::Bimodal;
  cfg.bpred.btb_entries = 8;
  expect(26 + 4, {26, 0, 4, 0, 0, 0});
  EXPECT_EQ(p->stats().btb_hits, 9u);  // iterations 2-10
}

TEST_F(P3, JalUsesIdRedirectOnlyWithADynamicPredictor) {
  // static_nt is the Phase 2 front end: JAL resolves in EX (2 cycles).
  prog({enc::jal(1, 8), NOP, END});
  expect(2 + 4 + 2, {6, 0, 2, 0, 0, 0});
  // bimodal: JAL redirects from ID (1 cycle).
  cfg.bpred.type = rvx::bpred::Type::Bimodal;
  expect(2 + 4 + 1, {6, 0, 1, 0, 0, 0});
  EXPECT_EQ(p->stats().id_redirects, 1u);
}

TEST_F(P3, GshareTrainsTheCounterItPredictedWith) {
  // Design note §9.3. Gshare with 4 entries and 2 history bits:
  //   index = ((pc >> 2) ^ ghr) & 3; all counters start at 1; ghr = 0.
  //
  // 0x00 addi x1, x0, 3
  // 0x04 B2: bne x0, x0, +8     never taken          (pc>>2 = 1)
  // 0x08 addi x1, x1, -1
  // 0x0C B1: bne x1, x0, -8     taken, taken, not    (pc>>2 = 3)
  // 0x10 END
  //
  // B1 is fetched while B2 is in EX, so B1 looks up the history from before
  // B2 resolves. B2 then shifts the history, so by the time B1 reaches EX the
  // history differs from the one B1 was predicted with.
  //
  //  iter 1: B2 ghr0 idx1 c1 NT ok -> c[1]=0, ghr 0
  //          B1 ghr0 idx3 c1 NT, actual T: mispredict 2 -> c[3]=2, ghr 1
  //  iter 2: B2 ghr1 idx0 c1 NT ok -> c[0]=0, ghr 2
  //          B1 ghr1 (lookup before B2's update) idx2 c1 NT, actual T:
  //          mispredict 2 -> trains idx2 (the counter it used): c[2]=2, ghr 1
  //          [re-indexing at EX would use ghr 2 -> idx1, leaving c[2]=1]
  //  iter 3: B2 ghr1 idx0 c0 NT ok -> ghr 2
  //          B1 ghr1 idx2 c2 -> predicted TAKEN, actual NT (loop exit):
  //          ID redirect, then EX mispredict, 2 (the ID bubble is flushed)
  //          [with the wrong training c[2]=1 -> NT, correct, 0]
  // branch = 2 + 2 + 2 = 6; retired = 1 + 3*3 + 1 = 11, base 15.
  prog({enc::addi(1, 0, 3), enc::b(8, 0, 0, 1, enc::BRANCH), enc::addi(1, 1, -1),
        enc::b(-8, 0, 1, 1, enc::BRANCH), END});
  cfg.bpred.type = rvx::bpred::Type::Gshare;
  cfg.bpred.entries = 4;
  cfg.bpred.history_bits = 2;
  expect(15 + 6, {15, 0, 6, 0, 0, 0});
  EXPECT_EQ(p->stats().cond_branches, 6u);
  EXPECT_EQ(p->stats().cond_mispredicts, 3u);
}

TEST_F(P3, TakenJumpToNextInstructionIsStillAMisprediction) {
  // docs/BUGS.md #4: a taken branch/jump whose target is pc + 4 must still be
  // treated as taken. Misprediction is decided by the taken bit plus target,
  // never by comparing next-pc values.
  // static_nt: predicted not-taken, actually taken -> EX flush, 2 (as Phase 2).
  prog({enc::jal(1, 4), END});
  expect(2 + 4 + 2, {6, 0, 2, 0, 0, 0});
  prog({enc::b(4, 0, 0, 0, enc::BRANCH), END});  // beq x0, x0, +4
  expect(2 + 4 + 2, {6, 0, 2, 0, 0, 0});
  // bimodal: JAL is predicted taken in ID -> 1-cycle ID redirect to pc + 4.
  prog({enc::jal(1, 4), END});
  cfg.bpred.type = rvx::bpred::Type::Bimodal;
  expect(2 + 4 + 1, {6, 0, 1, 0, 0, 0});
}

// --- I-cache and DRAM (design note §4) ----------------------------------------

TEST_F(P3, ColdICacheStraightLine) {
  // 31 addi + END = 32 instructions = 8 lines of 16 B, all in bank 0 row 0.
  // First line: closed 30 + 2 = 32. Next 7 lines: hit 20 + 2 = 22 each.
  // icache = 32 + 7*22 = 186; base = 32 + 4 = 36.
  for (uint32_t k = 0; k < 31; ++k) mem.store(4 * k, 4, enc::addi(1, 1, 1));
  mem.store(31 * 4, 4, END);
  enable_icache();
  expect(36 + 186, {36, 0, 0, 186, 0, 0});
  // The 9th miss is the fetch of 0x80 that starts while END is in flight;
  // its bubbles never reach WB because END halts the run first.
  EXPECT_EQ(p->mem_system().icache()->stats().misses, 9u);
}

TEST_F(P3, WrongPathIMissInFlightDelaysTheRedirect) {
  // 0x00 nop, 0x04 nop, 0x08 nop, 0x0C beq x0,x0,+0x34 (-> 0x40), 0x40 END
  // c1: miss line 0x00 (32): ICache bubbles c1-c32, nop delivered c33.
  // c33-36 deliver nop, nop, nop, beq. c37: fetch 0x10 misses (row hit: 22),
  //   bubbles c37-58. beq is in EX at c38 -> redirect to 0x40 while the fill
  //   is in flight, so it is held (fill cannot be cancelled).
  // c59: fetch 0x40, miss (22), bubbles c59-80; END delivered c81, WB c85.
  // WB tally: 4 fill + 5 retired = base 9; ICache 32 + 20 + 22 = 74 (the
  //   ICache bubble from c37 is replaced by a Redirect bubble); Redirect 2.
  prog({NOP, NOP, NOP, enc::b(0x34, 0, 0, 0, enc::BRANCH)});
  prog({END}, 0x40);
  enable_icache();
  expect(85, {9, 0, 2, 74, 0, 0});
}

TEST_F(P3, FenceIInvalidatesICacheAndRefetches) {
  // nop, fence.i, END in line 0. c1 miss (32) -> nop c33, fence.i c34,
  // END fetched c35 (hit), 0x0C c36 (hit). fence.i in EX at c36: redirect to
  // 0x08 and invalidate the I-cache. c37: 0x08 misses again (row hit 22),
  // END delivered c59, WB c63.
  // base 3 + 4 = 7; icache 32 + 22 = 54; branch (fence.i flush) 2.
  prog({NOP, FENCE_I, END});
  enable_icache();
  expect(63, {7, 0, 2, 54, 0, 0});
  EXPECT_EQ(p->stats().branch_fence_i_cycles, 2u);
}

// --- D-cache (design note §3) -----------------------------------------------------

TEST_F(P3, DCacheFirstTouchMissesSameLineHits) {
  // lw 0x400: closed 30 + 2 = 32.  lw 0x404, 0x408: hits.
  // lw 0x410: new line, row hit 22.  END stores 0x7F0: new line, 22.
  // dcache = 76; base = 5 + 4 = 9.
  prog({enc::lw(1, 0, 0x400), enc::lw(2, 0, 0x404), enc::lw(3, 0, 0x408), enc::lw(4, 0, 0x410), END});
  enable_dcache();
  expect(9 + 76, {9, 0, 0, 0, 76, 0});
  EXPECT_EQ(p->mem_system().dcache()->stats().misses, 3u);
}

TEST_F(P3, DirtyEvictionAddsWritebackAndRowMiss) {
  // Direct-mapped 32 B, 16 B lines: 2 sets.
  // 0x00 sw   x0, 0x400(x0)   miss (set 0): closed 30 + 2 = 32; line dirty
  // 0x04 lui  x5, 0x2
  // 0x08 addi x5, x5, 0x400   x5 = 0x2400 (bank 0, row 1)
  // 0x0C lw   x1, 0(x5)       set 0: evict dirty 0x400 -> write-back, row 0
  //                           open: 20; fill 0x2400: row miss 40; +2 = 62
  // 0x10 END (0x7F0, set 1)   clean miss; bank 0 has row 1 open: 40 + 2 = 42
  // dcache = 32 + 62 + 42 = 136; base = 5 + 4 = 9.
  prog({enc::sw(0, 0, 0x400), enc::u(0x2, 5, enc::LUI_), enc::addi(5, 5, 0x400), enc::lw(1, 5, 0), END});
  enable_dcache(32, 1);
  expect(9 + 136, {9, 0, 0, 0, 136, 0});
  const auto& d = p->mem_system().dram().stats();
  EXPECT_EQ(d.row_closed, 1u);
  EXPECT_EQ(d.row_hits, 1u);
  EXPECT_EQ(d.row_misses, 2u);
  EXPECT_EQ(p->mem_system().dcache()->stats().writebacks, 1u);
}

TEST_F(P3, MulOverlapsWithADCacheMiss) {
  // lw x1, 0x7F4(x0): miss (closed, 32): in MEM c4-c35, WB c37.
  // mul enters EX at c4 and keeps counting during the stall (memory-side and
  // EX timers run, design note §3.3); it is done by c6 and leaves EX at c36.
  // END (0x7F0, same line as 0x7F4: hit) WB c39.
  // base 7, dcache 32, muldiv 0: the multiply is completely hidden.
  prog({enc::lw(1, 0, 0x7F4), enc::m(0, 2, 3, 4), END});
  enable_dcache();
  expect(39, {7, 0, 0, 0, 32, 0});
}

TEST_F(P3, OperandHeldInExSurvivesProducerRetiringDuringDMiss) {
  // 0x0 addi x7, x0, 5     WB c5
  // 0x4 lw   x1, 0x7F4(x0) miss in MEM c5..c36
  // 0x8 add  x8, x7, x7    in EX from c5: x7 forwarded from MEM/WB in c5,
  //                        but the addi retires that cycle. EX is held until
  //                        c37, so the forwarded value must be latched (§3.4
  //                        clarification), or the add would use stale x7 = 0.
  // 0xC END (same line as 0x7F4: hit)
  prog({enc::addi(7, 0, 5), enc::lw(1, 0, 0x7F4), enc::add(8, 7, 7), END});
  enable_dcache();
  expect(8 + 32, {8, 0, 0, 0, 32, 0});
  EXPECT_EQ(p->reg(8), 10u);
}

TEST_F(P3, LoadUseAndMulDivAreAttributed) {
  // lw x1 (data ideal); add x2,x1,x1 (load-use 1); mul x3,x2,x2 (L=3: 2); END
  prog({enc::lw(1, 0, 0x400), enc::add(2, 1, 1), enc::m(0, 3, 2, 2), END});
  expect(4 + 4 + 1 + 2, {8, 1, 0, 0, 0, 2});
}
