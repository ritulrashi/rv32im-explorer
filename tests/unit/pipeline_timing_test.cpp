// Directed timing tests for the 5-stage pipeline. Every expected cycle count
// is derived by hand from docs/phase2-pipeline-contract.md:
//
//   cycles = (retired instructions) + 4          (pipeline fill, §2)
//          + 1 per load-use stall                (§6.2)
//          + 2 per taken branch / jump / FENCE.I (§6.3, §6.5)
//          + (L - 1) per MUL/DIV of latency L    (§6.4)
//
// Programs live at address 0. The final instruction of every program is
// END = `sw x0, 0x7F0(x0)`, the tohost store (tohost = 0x7F0). It retires in
// WB and ends the run (§7). Data lives at 0x400.
#include "pipeline/pipeline.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

#include "encode.h"

using rvx::HaltReason;
using rvx::Memory;
using rvx::pipe::BubbleCause;
using rvx::pipe::Config;
using rvx::pipe::HaltInfo;
using rvx::pipe::Pipeline;

namespace {

constexpr uint32_t kToHost = 0x7F0;
constexpr uint32_t kData = 0x400;
const uint32_t END = enc::sw(0, 0, static_cast<int32_t>(kToHost));
const uint32_t NOP = enc::addi(0, 0, 0);
constexpr uint32_t ILLEGAL = 0x00000000;

class PipeTest : public ::testing::Test {
 protected:
  Memory mem;
  Config cfg;
  std::unique_ptr<Pipeline> p;

  void prog(std::initializer_list<uint32_t> words) {
    uint32_t a = 0;
    for (uint32_t w : words) {
      mem.store(a, 4, w);
      a += 4;
    }
  }
  HaltInfo run(uint64_t max_cycles = 1000) {
    p = std::make_unique<Pipeline>(mem, 0, cfg);
    p->set_tohost(kToHost);
    return p->run(max_cycles);
  }
  // Runs to completion and checks the normal tohost ending.
  void expect_cycles(uint64_t cycles, uint64_t retired) {
    auto h = run();
    EXPECT_EQ(h.reason, HaltReason::ToHost) << rvx::to_string(h.reason);
    EXPECT_EQ(p->retired(), retired);
    EXPECT_EQ(p->cycles(), cycles);
  }
};

uint32_t beq(int32_t off) { return enc::b(off, 0, 0, 0, enc::BRANCH); }  // beq x0,x0,off: always taken
uint32_t bne(int32_t off) { return enc::b(off, 0, 0, 1, enc::BRANCH); }  // bne x0,x0,off: never taken
uint32_t mul(uint32_t rd, uint32_t a, uint32_t b) { return enc::m(0, rd, a, b); }
uint32_t div(uint32_t rd, uint32_t a, uint32_t b) { return enc::m(4, rd, a, b); }

}  // namespace

// --- No hazards ----------------------------------------------------------------

TEST_F(PipeTest, StraightLineTakesNPlusFour) {
  prog({enc::addi(1, 0, 1), enc::addi(2, 0, 2), enc::addi(3, 0, 3), enc::addi(4, 0, 4),
        enc::addi(5, 0, 5), enc::addi(6, 0, 6), enc::addi(7, 0, 7), enc::addi(8, 0, 8),
        enc::addi(9, 0, 9), enc::addi(10, 0, 10), END});
  expect_cycles(11 + 4, 11);
  EXPECT_EQ(p->reg(10), 10u);
}

TEST_F(PipeTest, SingleTohostStore) {
  prog({END});
  expect_cycles(1 + 4, 1);
  EXPECT_EQ(p->halt().pc, 4u);  // like the ISS: pc of the next instruction
  EXPECT_EQ(p->halt().tohost_value, 0u);
}

// --- Forwarding (§6.1): no stalls ---------------------------------------------

TEST_F(PipeTest, DependentAluChainForwardsWithoutStalling) {
  prog({enc::addi(1, 0, 1), enc::addi(1, 1, 1), enc::addi(1, 1, 1), enc::addi(1, 1, 1),
        enc::addi(1, 1, 1), enc::addi(1, 1, 1), enc::addi(1, 1, 1), enc::addi(1, 1, 1),
        enc::addi(1, 1, 1), enc::addi(1, 1, 1), END});
  expect_cycles(11 + 4, 11);
  EXPECT_EQ(p->reg(1), 10u);
}

TEST_F(PipeTest, YoungestProducerWinsForwarding) {
  // x1 is in MEM/WB (=1) and EX/MEM (=2) when `add` is in EX: EX/MEM must win.
  prog({enc::addi(1, 0, 1), enc::addi(1, 0, 2), enc::add(2, 1, 1), END});
  expect_cycles(4 + 4, 4);
  EXPECT_EQ(p->reg(2), 4u);
}

TEST_F(PipeTest, RegisterFileWriteThroughCoversDistanceThree) {
  // Producer in WB while consumer is in ID (§3.1).
  prog({enc::addi(1, 0, 5), NOP, NOP, enc::add(2, 1, 1), END});
  expect_cycles(5 + 4, 5);
  EXPECT_EQ(p->reg(2), 10u);
}

// --- Load-use (§6.2) ----------------------------------------------------------

TEST_F(PipeTest, LoadUseStallsOneCycle) {
  mem.store(kData, 4, 21);
  prog({enc::lw(1, 0, kData), enc::add(3, 1, 1), END});
  expect_cycles(3 + 4 + 1, 3);
  EXPECT_EQ(p->reg(3), 42u);
}

TEST_F(PipeTest, LoadThenUseTwoLaterDoesNotStall) {
  mem.store(kData, 4, 21);
  prog({enc::lw(1, 0, kData), NOP, enc::add(3, 1, 1), END});
  expect_cycles(4 + 4, 4);
  EXPECT_EQ(p->reg(3), 42u);
}

TEST_F(PipeTest, LoadThenStoreOfLoadedValueStalls) {
  // No MEM->MEM forwarding in this design: store data waits like any use.
  mem.store(kData, 4, 21);
  prog({enc::lw(1, 0, kData), enc::sw(1, 0, kData + 4), END});
  expect_cycles(3 + 4 + 1, 3);
  EXPECT_EQ(mem.load(kData + 4, 4), 21u);
}

TEST_F(PipeTest, LoadThenStoreBaseStalls) {
  mem.store(kData, 4, kData + 8);
  prog({enc::lw(1, 0, kData), enc::sw(0, 1, 0), END});
  expect_cycles(3 + 4 + 1, 3);
}

TEST_F(PipeTest, LoadToX0NeverStalls) {
  prog({enc::lw(0, 0, kData), enc::add(3, 0, 0), END});
  expect_cycles(3 + 4, 3);
}

TEST_F(PipeTest, LoadUseIgnoresRegisterBitsTheFormatDoesNotRead) {
  // lui's immediate bits overlap the rs1/rs2 fields; here both happen to be 5.
  const uint32_t lui = enc::u(0x528, 6, enc::LUI_);
  ASSERT_EQ((lui >> 15) & 31, 5u);
  ASSERT_EQ((lui >> 20) & 31, 5u);
  prog({enc::lw(5, 0, kData), lui, END});
  expect_cycles(3 + 4, 3);
}

// --- Branches and jumps (§6.3) --------------------------------------------------

TEST_F(PipeTest, TakenBranchCostsTwo) {
  prog({beq(8), enc::addi(9, 0, 1), END});
  expect_cycles(2 + 4 + 2, 2);
  EXPECT_EQ(p->reg(9), 0u);  // wrong-path instruction had no effect
}

TEST_F(PipeTest, NotTakenBranchIsFree) {
  prog({bne(8), enc::addi(9, 0, 1), END});
  expect_cycles(3 + 4, 3);
  EXPECT_EQ(p->reg(9), 1u);
}

TEST_F(PipeTest, JalCostsTwoAndLinks) {
  prog({enc::jal(1, 8), enc::addi(9, 0, 1), END});
  expect_cycles(2 + 4 + 2, 2);
  EXPECT_EQ(p->reg(1), 4u);
  EXPECT_EQ(p->reg(9), 0u);
}

TEST_F(PipeTest, JalrCostsTwoWithForwardedBase) {
  prog({enc::addi(5, 0, 12), enc::jalr(1, 5, 0), enc::addi(9, 0, 1), END});
  expect_cycles(3 + 4 + 2, 3);
  EXPECT_EQ(p->reg(1), 8u);
  EXPECT_EQ(p->reg(9), 0u);
}

TEST_F(PipeTest, JalrFedByLoadPaysLoadUseAndRedirect) {
  mem.store(kData, 4, 12);
  prog({enc::lw(5, 0, kData), enc::jalr(0, 5, 0), enc::addi(9, 0, 1), END});
  expect_cycles(3 + 4 + 1 + 2, 3);
}

TEST_F(PipeTest, BranchFedByLoadPaysLoadUseAndRedirect) {
  // x1 = mem[0x400] = 0, so beq x1, x0 is taken.
  prog({enc::lw(1, 0, kData), enc::b(8, 0, 1, 0, enc::BRANCH), enc::addi(9, 0, 1), END});
  expect_cycles(3 + 4 + 1 + 2, 3);
}

// --- MUL / DIV (§6.4) ----------------------------------------------------------

TEST_F(PipeTest, MulDefaultLatency) {
  prog({mul(3, 0, 0), END});
  expect_cycles(2 + 4 + (3 - 1), 2);
}

TEST_F(PipeTest, DivDefaultLatency) {
  prog({div(3, 0, 0), END});
  expect_cycles(2 + 4 + (33 - 1), 2);
}

TEST_F(PipeTest, MulLatencyIsConfigurable) {
  cfg.mul_latency = 1;
  prog({mul(3, 0, 0), END});
  expect_cycles(2 + 4, 2);
  cfg.mul_latency = 7;
  expect_cycles(2 + 4 + 6, 2);
}

TEST_F(PipeTest, MulResultForwardsToDependentWithNoExtraStall) {
  prog({enc::addi(1, 0, 6), enc::addi(2, 0, 7), mul(3, 1, 2), enc::add(4, 3, 3), END});
  expect_cycles(5 + 4 + 2, 5);
  EXPECT_EQ(p->reg(4), 84u);
}

TEST_F(PipeTest, MulOperandsAreCapturedOnFirstExCycle) {
  // With L = 10 the producer (addi) retires while mul is still busy. If the
  // operands were not captured on the first EX cycle, mul would use the stale
  // x1 = 0 read in ID.
  cfg.mul_latency = 10;
  prog({enc::addi(1, 0, 7), mul(3, 1, 1), END});
  expect_cycles(3 + 4 + 9, 3);
  EXPECT_EQ(p->reg(3), 49u);
}

TEST_F(PipeTest, DivisionCornerCasesThroughThePipeline) {
  prog({enc::addi(1, 0, -7), enc::addi(2, 0, 2), enc::addi(5, 0, -1), enc::u(0x80000, 6, enc::LUI_),
        div(10, 1, 2),             // -7 / 2 = -3
        enc::m(6, 11, 1, 2),       // rem: -7 % 2 = -1
        div(12, 1, 0),             // x / 0 = -1
        enc::m(6, 13, 1, 0),       // x % 0 = x
        div(14, 6, 5),             // INT_MIN / -1 = INT_MIN
        enc::m(6, 15, 6, 5),       // INT_MIN % -1 = 0
        enc::m(5, 16, 1, 2),       // divu 0xFFFFFFF9 / 2 = 0x7FFFFFFC
        enc::m(7, 17, 1, 0),       // remu x % 0 = x
        END});
  cfg.div_latency = 1;
  run();
  EXPECT_EQ(p->reg(10), static_cast<uint32_t>(-3));
  EXPECT_EQ(p->reg(11), static_cast<uint32_t>(-1));
  EXPECT_EQ(p->reg(12), 0xFFFFFFFFu);
  EXPECT_EQ(p->reg(13), static_cast<uint32_t>(-7));
  EXPECT_EQ(p->reg(14), 0x80000000u);
  EXPECT_EQ(p->reg(15), 0u);
  EXPECT_EQ(p->reg(16), 0x7FFFFFFCu);
  EXPECT_EQ(p->reg(17), static_cast<uint32_t>(-7));
}

// --- FENCE.I (§6.5) and memory timing (§3.0) -------------------------------------

TEST_F(PipeTest, FenceICostsTwo) {
  prog({0x0000100F, END});
  expect_cycles(2 + 4 + 2, 2);
}

TEST_F(PipeTest, FenceIMakesAPrecedingStoreVisibleToFetch) {
  // 0x00 lw   x5, 0x400(x0)     x5 = encoding of `addi x9, x0, 5`
  // 0x04 sw   x5, 0x10(x0)      overwrite the instruction at 0x10
  // 0x08 fence.i                redirect to 0x0C; 0x10 is refetched
  // 0x0C nop
  // 0x10 addi x9, x0, 1         (stale copy was fetched before the store)
  // 0x14 END
  mem.store(kData, 4, enc::addi(9, 0, 5));
  prog({enc::lw(5, 0, kData), enc::sw(5, 0, 0x10), 0x0000100F, NOP, enc::addi(9, 0, 1), END});
  expect_cycles(6 + 4 + 1 + 2, 6);  // load-use (lw->sw data) + fence.i redirect
  EXPECT_EQ(p->reg(9), 5u);
}

// --- Faults (§6.6) ------------------------------------------------------------

TEST_F(PipeTest, IllegalWordOnWrongPathIsSquashed) {
  prog({beq(8), ILLEGAL, END});
  expect_cycles(2 + 4 + 2, 2);
}

TEST_F(PipeTest, IllegalOnRightPathHaltsWithoutRetiring) {
  prog({enc::addi(1, 0, 1), ILLEGAL, END});
  auto h = run();
  EXPECT_EQ(h.reason, HaltReason::IllegalInsn);
  EXPECT_EQ(h.pc, 4u);
  EXPECT_EQ(p->retired(), 1u);
  EXPECT_EQ(p->cycles(), 6u);  // illegal word is instruction 2: WB in cycle 2 + 4
}

TEST_F(PipeTest, MisalignedLoadHaltsAndYoungerStoreNeverWrites) {
  prog({enc::addi(5, 0, kData + 1), enc::addi(7, 0, 99), enc::lw(6, 5, 0), enc::sw(7, 0, 0x500), END});
  auto h = run();
  EXPECT_EQ(h.reason, HaltReason::MisalignedLoad);
  EXPECT_EQ(h.pc, 8u);
  EXPECT_EQ(p->retired(), 2u);
  EXPECT_EQ(p->cycles(), 7u);
  EXPECT_EQ(mem.load(0x500, 4), 0u);
}

TEST_F(PipeTest, FaultingLoadWithDependentConsumerDoesNotLoadUseStall) {
  prog({enc::addi(5, 0, kData + 2), enc::lw(6, 5, 0), enc::add(7, 6, 6), END});
  auto h = run();
  EXPECT_EQ(h.reason, HaltReason::MisalignedLoad);
  EXPECT_EQ(p->retired(), 1u);
  EXPECT_EQ(p->cycles(), 6u);
}

TEST_F(PipeTest, MisalignedStoreDoesNotWrite) {
  prog({enc::addi(5, 0, kData + 2), enc::addi(7, 0, -1), enc::sw(7, 5, 0), END});
  auto h = run();
  EXPECT_EQ(h.reason, HaltReason::MisalignedStore);
  EXPECT_EQ(mem.load(kData, 4), 0u);
  EXPECT_EQ(mem.load(kData + 4, 4), 0u);
}

TEST_F(PipeTest, MisalignedJumpTargetFaultsOnTheJump) {
  prog({enc::jal(1, 6), END});
  auto h = run();
  EXPECT_EQ(h.reason, HaltReason::MisalignedFetch);
  EXPECT_EQ(h.pc, 0u);
  EXPECT_EQ(p->retired(), 0u);
  EXPECT_EQ(p->reg(1), 0u);
  EXPECT_EQ(p->cycles(), 5u);
}

TEST_F(PipeTest, EcallHalts) {
  prog({0x00000073, END});
  auto h = run();
  EXPECT_EQ(h.reason, HaltReason::Ecall);
  EXPECT_EQ(p->retired(), 0u);
}

TEST_F(PipeTest, CycleBudget) {
  prog({enc::jal(0, 0)});  // j .
  auto h = run(50);
  EXPECT_EQ(h.reason, HaltReason::MaxCycles);
  EXPECT_EQ(p->cycles(), 50u);
}

// --- Per-cycle stage occupancy against a hand-drawn diagram ---------------------

namespace {
std::string cell(const std::optional<uint32_t>& pc) {
  if (!pc) return "..";
  char b[8];
  std::snprintf(b, sizeof b, "%02x", *pc);
  return b;
}
}  // namespace

TEST_F(PipeTest, StageOccupancyMatchesHandDrawnDiagram) {
  // 0x00 A lw   x1, 0x400(x0)
  // 0x04 B add  x2, x1, x1     load-use on A
  // 0x08 C beq  x0, x0, +8     taken -> 0x10
  // 0x0C D addi x9, x0, 1      wrong path
  // 0x10 E mul  x3, x2, x2     latency 3
  // 0x14 F END (tohost store)
  // 0x18 G 0x00000000          illegal, fetched behind F, never retires
  prog({enc::lw(1, 0, kData), enc::add(2, 1, 1), beq(8), enc::addi(9, 0, 1), mul(3, 2, 2), END});
  // Columns: IF ID EX MEM WB (pc low byte, ".." = bubble / frozen fetch).
  const std::vector<std::string> expected = {
      "00 .. .. .. ..",  // c1
      "04 00 .. .. ..",  // c2
      "08 04 00 .. ..",  // c3  A in EX, B in ID -> load-use stall
      "08 04 .. 00 ..",  // c4  PC and IF/ID held; LoadUse bubble in EX
      "0c 08 04 .. 00",  // c5  A retires; B gets x1 from MEM/WB
      "10 0c 08 04 ..",  // c6  C taken in EX -> redirect to 0x10
      "10 .. .. 08 04",  // c7  two Redirect bubbles; refetch 0x10
      "14 10 .. .. 08",  // c8
      "18 14 10 .. ..",  // c9  E in EX, cycle 1 of 3 (busy)
      "18 14 10 .. ..",  // c10 cycle 2 of 3 (busy)
      "18 14 10 .. ..",  // c11 cycle 3 of 3 (completes)
      "1c 18 14 10 ..",  // c12
      "20 1c 18 14 10",  // c13 E retires; G (illegal) in EX -> fault flush
      ".. .. .. 18 14",  // c14 F retires -> tohost halt
  };
  p = std::make_unique<Pipeline>(mem, 0, cfg);
  p->set_tohost(kToHost);
  std::vector<std::string> got;
  std::vector<rvx::pipe::StageView> views;
  while (!p->halted() && p->cycles() < 100) {
    p->tick();
    const auto& v = p->last_view();
    views.push_back(v);
    got.push_back(cell(v.if_pc) + " " + cell(v.id_pc) + " " + cell(v.ex_pc) + " " +
                  cell(v.mem_pc) + " " + cell(v.wb_pc));
  }
  EXPECT_EQ(got, expected);
  EXPECT_EQ(p->halt().reason, HaltReason::ToHost);
  EXPECT_EQ(p->retired(), 5u);
  ASSERT_EQ(views.size(), 14u);
  EXPECT_EQ(views[3].ex_cause, BubbleCause::LoadUse);    // c4
  EXPECT_EQ(views[6].id_cause, BubbleCause::Redirect);   // c7
  EXPECT_EQ(views[6].ex_cause, BubbleCause::Redirect);
  EXPECT_EQ(views[9].mem_cause, BubbleCause::MulDiv);    // c10
  EXPECT_EQ(views[13].id_cause, BubbleCause::FaultFlush);  // c14
  EXPECT_EQ(views[0].wb_cause, BubbleCause::Fill);       // c1
}
