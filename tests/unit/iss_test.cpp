// Directed semantic tests for the golden ISS. Each test sets registers
// directly, places one or a few instruction words (from the test encoder), and
// checks architectural results. Expected values are computed by hand from the
// ISA spec, with the arithmetic written out in comments.
#include "iss/iss.h"

#include <gtest/gtest.h>

#include <climits>
#include <initializer_list>
#include <vector>

#include "encode.h"

using rvx::MemKind;
using rvx::RetireRecord;
using rvx::iss::HaltReason;
using rvx::iss::Iss;
using rvx::Memory;

namespace {

constexpr uint32_t kBase = 0x1000;

class IssTest : public ::testing::Test {
 protected:
  Memory mem;
  Iss iss{mem, kBase};

  void place(std::initializer_list<uint32_t> words, uint32_t at = kBase) {
    for (uint32_t w : words) {
      mem.store(at, 4, w);
      at += 4;
    }
  }
  // Places one instruction at the current pc and executes it.
  std::optional<RetireRecord> exec(uint32_t insn) {
    mem.store(iss.pc(), 4, insn);
    return iss.step();
  }
  // rd = op(a, b) for an R-type instruction; uses x5, x6 -> x7.
  uint32_t rtype(uint32_t insn_with_regs_5_6_7, uint32_t a, uint32_t b) {
    iss.set_reg(5, a);
    iss.set_reg(6, b);
    auto rec = exec(insn_with_regs_5_6_7);
    EXPECT_TRUE(rec.has_value());
    return iss.reg(7);
  }
  uint32_t m_op(uint32_t f3, uint32_t a, uint32_t b) { return rtype(enc::m(f3, 7, 5, 6), a, b); }
};

constexpr uint32_t MUL = 0, MULH = 1, MULHSU = 2, MULHU = 3, DIV = 4, DIVU = 5, REM = 6, REMU = 7;
constexpr uint32_t u32(int64_t v) { return static_cast<uint32_t>(v); }

}  // namespace

// --- Basic retire behaviour --------------------------------------------------

TEST_F(IssTest, AddiRetiresAndAdvancesPc) {
  auto rec = exec(enc::addi(10, 0, 42));
  ASSERT_TRUE(rec);
  EXPECT_EQ(iss.reg(10), 42u);
  EXPECT_EQ(iss.pc(), kBase + 4);
  EXPECT_EQ(rec->seq, 0u);
  EXPECT_EQ(rec->pc, kBase);
  EXPECT_EQ(rec->rd, 10);
  EXPECT_EQ(rec->rd_val, 42u);
  EXPECT_EQ(rec->mem, MemKind::None);
  EXPECT_EQ(iss.retired(), 1u);
}

TEST_F(IssTest, X0IsHardwiredAndNotReportedAsWrite) {
  auto rec = exec(enc::addi(0, 0, 5));
  ASSERT_TRUE(rec);
  EXPECT_EQ(iss.reg(0), 0u);
  EXPECT_EQ(rec->rd, 0);
  EXPECT_EQ(rec->rd_val, 0u);
  iss.set_reg(0, 123);
  EXPECT_EQ(iss.reg(0), 0u);
}

TEST_F(IssTest, SequenceNumbersIncrement) {
  place({enc::addi(1, 0, 1), enc::addi(1, 1, 1), enc::addi(1, 1, 1)});
  EXPECT_EQ(iss.step()->seq, 0u);
  EXPECT_EQ(iss.step()->seq, 1u);
  EXPECT_EQ(iss.step()->seq, 2u);
  EXPECT_EQ(iss.reg(1), 3u);
}

// --- ALU ---------------------------------------------------------------------

TEST_F(IssTest, AddWrapsWithoutUndefinedBehaviour) {
  EXPECT_EQ(rtype(enc::add(7, 5, 6), 0x7FFFFFFF, 1), 0x80000000u);
  EXPECT_EQ(rtype(enc::add(7, 5, 6), 0xFFFFFFFF, 1), 0u);
  EXPECT_EQ(rtype(enc::r(0x20, 6, 5, 0, 7, enc::OP), 0x80000000, 1), 0x7FFFFFFFu);  // sub
}

TEST_F(IssTest, SignedVersusUnsignedCompare) {
  // slt: -1 < 1 is true; sltu: 0xFFFFFFFF < 1 is false.
  EXPECT_EQ(rtype(enc::r(0, 6, 5, 2, 7, enc::OP), u32(-1), 1), 1u);
  EXPECT_EQ(rtype(enc::r(0, 6, 5, 3, 7, enc::OP), u32(-1), 1), 0u);
  // sltiu with imm -1 compares against 0xFFFFFFFF (sign-extended, then unsigned).
  iss.set_reg(5, 5);
  exec(enc::i(-1, 5, 3, 7, enc::OP_IMM));
  EXPECT_EQ(iss.reg(7), 1u);
}

TEST_F(IssTest, ShiftAmountUsesLowFiveBitsOfRs2) {
  // sll by 33 == sll by 1
  EXPECT_EQ(rtype(enc::r(0, 6, 5, 1, 7, enc::OP), 1, 33), 2u);
  // srl vs sra of 0x80000000 by 31
  EXPECT_EQ(rtype(enc::r(0, 6, 5, 5, 7, enc::OP), 0x80000000, 31), 1u);
  EXPECT_EQ(rtype(enc::r(0x20, 6, 5, 5, 7, enc::OP), 0x80000000, 31), 0xFFFFFFFFu);
  // srai by immediate
  iss.set_reg(5, 0xF0000000);
  exec(enc::r(0x20, 4, 5, 5, 7, enc::OP_IMM));
  EXPECT_EQ(iss.reg(7), 0xFF000000u);
}

TEST_F(IssTest, LuiAndAuipc) {
  exec(enc::u(0xABCDE, 3, enc::LUI_));
  EXPECT_EQ(iss.reg(3), 0xABCDE000u);
  const uint32_t pc = iss.pc();
  exec(enc::u(0xFFFFF, 4, enc::AUIPC_));  // pc + 0xFFFFF000 = pc - 0x1000
  EXPECT_EQ(iss.reg(4), pc - 0x1000u);
}

// --- M extension (spec table "Semantics for division by zero and division overflow")

TEST_F(IssTest, MulLowBits) {
  EXPECT_EQ(m_op(MUL, 0x80000000, 2), 0u);
  EXPECT_EQ(m_op(MUL, u32(-3), 7), u32(-21));
  EXPECT_EQ(m_op(MUL, 0x12345678, 0x9ABCDEF0), u32(0x12345678ull * 0x9ABCDEF0ull));
}

TEST_F(IssTest, MulHighSignedness) {
  // (-2^31) * (-2^31) = 2^62 -> high word 0x40000000
  EXPECT_EQ(m_op(MULH, 0x80000000, 0x80000000), 0x40000000u);
  // (-1) * (-1) = 1 -> high word 0
  EXPECT_EQ(m_op(MULH, u32(-1), u32(-1)), 0u);
  // (-1) * 1 = -1 -> high word all ones
  EXPECT_EQ(m_op(MULH, u32(-1), 1), 0xFFFFFFFFu);
  // unsigned: (2^32-1)^2 = 0xFFFFFFFE_00000001
  EXPECT_EQ(m_op(MULHU, 0xFFFFFFFF, 0xFFFFFFFF), 0xFFFFFFFEu);
  // signed(-1) * unsigned(2^32-1) = -(2^32-1) = 0xFFFFFFFF_00000001
  EXPECT_EQ(m_op(MULHSU, 0xFFFFFFFF, 0xFFFFFFFF), 0xFFFFFFFFu);
  // signed(-2^31) * unsigned(2^32-1) = -2^63 + 2^31 = 0x80000000_80000000
  EXPECT_EQ(m_op(MULHSU, 0x80000000, 0xFFFFFFFF), 0x80000000u);
  // rs2 is unsigned in MULHSU: 2 * 0x80000000 = 2^32 -> high 1 (not -1)
  EXPECT_EQ(m_op(MULHSU, 2, 0x80000000), 1u);
}

TEST_F(IssTest, DivisionTruncatesTowardZero) {
  EXPECT_EQ(m_op(DIV, u32(-7), 2), u32(-3));
  EXPECT_EQ(m_op(REM, u32(-7), 2), u32(-1));
  EXPECT_EQ(m_op(DIV, 7, u32(-2)), u32(-3));
  EXPECT_EQ(m_op(REM, 7, u32(-2)), 1u);
  EXPECT_EQ(m_op(DIVU, u32(-7), 2), 0x7FFFFFFCu);
  EXPECT_EQ(m_op(REMU, u32(-7), 2), 1u);
}

TEST_F(IssTest, DivisionByZero) {
  EXPECT_EQ(m_op(DIV, 123, 0), 0xFFFFFFFFu);
  EXPECT_EQ(m_op(DIVU, 123, 0), 0xFFFFFFFFu);
  EXPECT_EQ(m_op(REM, u32(-123), 0), u32(-123));
  EXPECT_EQ(m_op(REMU, 123, 0), 123u);
}

TEST_F(IssTest, SignedDivisionOverflow) {
  EXPECT_EQ(m_op(DIV, 0x80000000, u32(-1)), 0x80000000u);
  EXPECT_EQ(m_op(REM, 0x80000000, u32(-1)), 0u);
}

// --- Loads and stores --------------------------------------------------------

TEST_F(IssTest, LoadSignAndZeroExtension) {
  mem.store(0x2000, 4, 0x8001FF80);
  iss.set_reg(5, 0x2000);
  auto lb = exec(enc::i(0, 5, 0, 7, enc::LOAD));
  EXPECT_EQ(iss.reg(7), 0xFFFFFF80u);
  EXPECT_EQ(lb->mem, MemKind::Read);
  EXPECT_EQ(lb->mem_addr, 0x2000u);
  EXPECT_EQ(lb->mem_data, 0x80u);  // raw byte, before sign extension
  EXPECT_EQ(lb->mem_size, 1);
  EXPECT_EQ(lb->rd_val, 0xFFFFFF80u);

  exec(enc::i(0, 5, 4, 7, enc::LOAD));  // lbu
  EXPECT_EQ(iss.reg(7), 0x80u);
  exec(enc::i(2, 5, 1, 7, enc::LOAD));  // lh 0x2002 -> 0x8001
  EXPECT_EQ(iss.reg(7), 0xFFFF8001u);
  exec(enc::i(2, 5, 5, 7, enc::LOAD));  // lhu
  EXPECT_EQ(iss.reg(7), 0x8001u);
  exec(enc::lw(7, 5, 0));
  EXPECT_EQ(iss.reg(7), 0x8001FF80u);
}

TEST_F(IssTest, LoadWithNegativeOffset) {
  mem.store(0x1FFC, 4, 0xCAFEF00D);
  iss.set_reg(5, 0x2000);
  exec(enc::lw(7, 5, -4));
  EXPECT_EQ(iss.reg(7), 0xCAFEF00Du);
}

TEST_F(IssTest, StoreRecordsMaskedData) {
  iss.set_reg(5, 0x3000);
  iss.set_reg(6, 0x12345678);
  auto sb = exec(enc::s(1, 6, 5, 0, enc::STORE));
  EXPECT_EQ(sb->mem, MemKind::Write);
  EXPECT_EQ(sb->mem_addr, 0x3001u);
  EXPECT_EQ(sb->mem_data, 0x78u);
  EXPECT_EQ(sb->mem_size, 1);
  EXPECT_EQ(sb->rd, 0);
  auto sh = exec(enc::s(2, 6, 5, 1, enc::STORE));
  EXPECT_EQ(sh->mem_data, 0x5678u);
  EXPECT_EQ(mem.load(0x3000, 4), 0x56787800u);
  auto sw = exec(enc::sw(6, 5, 4));
  EXPECT_EQ(sw->mem_data, 0x12345678u);
  EXPECT_EQ(mem.load(0x3004, 4), 0x12345678u);
}

TEST_F(IssTest, MisalignedLoadHaltsWithoutSideEffects) {
  iss.set_reg(5, 0x2002);
  iss.set_reg(7, 99);
  auto rec = exec(enc::lw(7, 5, 0));
  EXPECT_FALSE(rec);
  EXPECT_EQ(iss.result().reason, HaltReason::MisalignedLoad);
  EXPECT_EQ(iss.reg(7), 99u);
  EXPECT_EQ(iss.pc(), kBase);
  EXPECT_EQ(iss.retired(), 0u);
  EXPECT_FALSE(iss.step());  // stays halted
}

TEST_F(IssTest, MisalignedStoreHaltsWithoutWriting) {
  iss.set_reg(5, 0x2001);
  iss.set_reg(6, 0xFFFFFFFF);
  EXPECT_FALSE(exec(enc::s(0, 6, 5, 1, enc::STORE)));  // sh to odd address
  EXPECT_EQ(iss.result().reason, HaltReason::MisalignedStore);
  EXPECT_EQ(mem.load(0x2000, 4), 0u);
}

TEST_F(IssTest, ByteAccessesAreNeverMisaligned) {
  iss.set_reg(5, 0x2003);
  EXPECT_TRUE(exec(enc::i(0, 5, 0, 7, enc::LOAD)));
  EXPECT_TRUE(exec(enc::s(0, 7, 5, 0, enc::STORE)));
}

// --- Control flow ------------------------------------------------------------

TEST_F(IssTest, JalLinksAndJumps) {
  auto rec = exec(enc::jal(1, -8));
  ASSERT_TRUE(rec);
  EXPECT_EQ(iss.reg(1), kBase + 4);
  EXPECT_EQ(iss.pc(), kBase - 8);
  EXPECT_EQ(rec->rd_val, kBase + 4);
}

TEST_F(IssTest, JalrClearsBitZeroAndUsesOldRs1WhenRdEqualsRs1) {
  iss.set_reg(5, 0x4001);
  exec(enc::jalr(5, 5, 4));  // target = (0x4001 + 4) & ~1 = 0x4004
  EXPECT_EQ(iss.pc(), 0x4004u);
  EXPECT_EQ(iss.reg(5), kBase + 4);
}

TEST_F(IssTest, BranchesSignedAndUnsigned) {
  struct Case { uint32_t f3; uint32_t a, b; bool taken; };
  const Case cases[] = {
      {0, 5, 5, true},   {0, 5, 6, false},          // beq
      {1, 5, 6, true},   {1, 5, 5, false},          // bne
      {4, u32(-1), 1, true}, {4, 1, u32(-1), false},  // blt (signed)
      {5, 1, u32(-1), true}, {5, 3, 3, true}, {5, u32(-1), 1, false},  // bge
      {6, 1, u32(-1), true}, {6, u32(-1), 1, false},  // bltu
      {7, u32(-1), 1, true}, {7, 1, u32(-1), false},  // bgeu
  };
  for (const auto& c : cases) {
    Memory m;
    Iss local(m, kBase);
    local.set_reg(5, c.a);
    local.set_reg(6, c.b);
    m.store(kBase, 4, enc::b(-16, 6, 5, c.f3, enc::BRANCH));
    auto rec = local.step();
    ASSERT_TRUE(rec);
    EXPECT_EQ(local.pc(), c.taken ? kBase - 16 : kBase + 4)
        << "f3=" << c.f3 << " a=" << c.a << " b=" << c.b;
    EXPECT_EQ(rec->rd, 0);
  }
}

TEST_F(IssTest, MisalignedJumpTargetHaltsOnTheJump) {
  iss.set_reg(5, 0x4002);
  EXPECT_FALSE(exec(enc::jalr(1, 5, 0)));
  EXPECT_EQ(iss.result().reason, HaltReason::MisalignedFetch);
  EXPECT_EQ(iss.reg(1), 0u);  // link register not written
  EXPECT_EQ(iss.pc(), kBase);
}

TEST_F(IssTest, MisalignedBranchTargetOnlyFaultsWhenTaken) {
  iss.set_reg(5, 1);
  iss.set_reg(6, 2);
  EXPECT_TRUE(exec(enc::b(6, 6, 5, 0, enc::BRANCH)));  // beq not taken: fine
  EXPECT_FALSE(exec(enc::b(6, 6, 5, 1, enc::BRANCH)));  // bne taken to +6: fault
  EXPECT_EQ(iss.result().reason, HaltReason::MisalignedFetch);
}

// --- Halting -----------------------------------------------------------------

TEST_F(IssTest, IllegalInstructionHalts) {
  EXPECT_FALSE(exec(0x00000000));
  EXPECT_EQ(iss.result().reason, HaltReason::IllegalInsn);
  EXPECT_EQ(iss.result().pc, kBase);
}

TEST_F(IssTest, EcallAndEbreakHalt) {
  EXPECT_FALSE(exec(0x00000073));
  EXPECT_EQ(iss.result().reason, HaltReason::Ecall);
  Memory m;
  Iss other(m, 0);
  m.store(0, 4, 0x00100073);
  EXPECT_FALSE(other.step());
  EXPECT_EQ(other.result().reason, HaltReason::Ebreak);
}

TEST_F(IssTest, FencesAreNops) {
  EXPECT_TRUE(exec(0x0FF0000F));
  EXPECT_TRUE(exec(0x0000100F));
  EXPECT_EQ(iss.pc(), kBase + 8);
}

TEST_F(IssTest, StoreToTohostRetiresThenHalts) {
  iss.set_tohost(0x5000);
  iss.set_reg(5, 0x5000);
  iss.set_reg(6, 1);
  auto rec = exec(enc::sw(6, 5, 0));
  ASSERT_TRUE(rec);
  EXPECT_EQ(rec->mem, MemKind::Write);
  auto r = iss.result();
  EXPECT_EQ(r.reason, HaltReason::ToHost);
  EXPECT_EQ(r.tohost_value, 1u);
  EXPECT_EQ(r.retired, 1u);
  EXPECT_FALSE(iss.step());
}

TEST_F(IssTest, RunStopsAtInstructionBudget) {
  place({enc::jal(0, 0)});  // j .  (infinite loop)
  uint64_t seen = 0;
  auto r = iss.run(10, [&](const RetireRecord&) { ++seen; });
  EXPECT_EQ(r.reason, HaltReason::MaxInsns);
  EXPECT_EQ(r.retired, 10u);
  EXPECT_EQ(seen, 10u);
}

TEST_F(IssTest, SmallProgramComputesSum) {
  // sum = 0; for (i = 10; i != 0; --i) sum += i;   => 55
  place({
      enc::addi(10, 0, 0),                    // a0 = 0
      enc::addi(11, 0, 10),                   // a1 = 10
      enc::add(10, 10, 11),                   // loop: a0 += a1
      enc::addi(11, 11, -1),                  //       a1 -= 1
      enc::b(-8, 0, 11, 1, enc::BRANCH),      //       bne a1, x0, loop
      enc::sw(10, 0, 0x100),                  // sw a0, 0x100(x0)  -> tohost
  });
  iss.set_tohost(0x100);
  auto r = iss.run(1000);
  EXPECT_EQ(r.reason, HaltReason::ToHost);
  EXPECT_EQ(r.tohost_value, 55u);
  EXPECT_EQ(r.retired, 2u + 10u * 3u + 1u);
}

TEST(IssDeterminism, SameProgramSameTrace) {
  auto run_once = [] {
    Memory m;
    Iss s(m, 0);
    m.store(0, 4, enc::addi(1, 1, 3));
    m.store(4, 4, enc::m(0, 2, 1, 1));
    m.store(8, 4, enc::jal(0, -8));
    std::vector<RetireRecord> out;
    s.run(100, [&](const RetireRecord& r) { out.push_back(r); });
    return out;
  };
  EXPECT_EQ(run_once(), run_once());
}
