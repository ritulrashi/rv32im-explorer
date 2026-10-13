#include "common/decode.h"

#include <gtest/gtest.h>

#include <climits>

#include "encode.h"

using rvx::decode;
using rvx::Format;
using rvx::Op;

// ---------------------------------------------------------------------------
// 1. The test encoder itself, checked against literal encodings that were
//    computed by hand from the spec bit layouts (and match the standard
//    disassembly seen in any RISC-V toolchain dump).
// ---------------------------------------------------------------------------
TEST(Encoder, MatchesHandComputedLiterals) {
  EXPECT_EQ(enc::addi(10, 10, -1), 0xFFF50513u);              // addi a0,a0,-1
  EXPECT_EQ(enc::jal(1, 0), 0x000000EFu);                     // jal ra,0
  EXPECT_EQ(enc::u(0x12345, 10, enc::LUI_), 0x12345537u);     // lui a0,0x12345
  EXPECT_EQ(enc::sw(11, 10, 8), 0x00B52423u);                 // sw a1,8(a0)
  EXPECT_EQ(enc::b(-4, 0, 0, 0, enc::BRANCH), 0xFE000EE3u);   // beq x0,x0,-4
  EXPECT_EQ(enc::jal(0, -4), 0xFFDFF06Fu);                    // j -4
  EXPECT_EQ(enc::m(0, 10, 11, 12), 0x02C58533u);              // mul a0,a1,a2
  EXPECT_EQ(enc::r(0x20, 31, 10, 5, 10, enc::OP_IMM), 0x41F55513u);  // srai a0,a0,31
  EXPECT_EQ(enc::r(0x20, 12, 11, 0, 10, enc::OP), 0x40C58533u);      // sub a0,a1,a2
  EXPECT_EQ(enc::i(-1, 11, 0, 10, enc::LOAD), 0xFFF58503u);   // lb a0,-1(a1)
  EXPECT_EQ(enc::jalr(0, 1, 0), 0x00008067u);                 // ret
  EXPECT_EQ(enc::u(0, 5, enc::AUIPC_), 0x00000297u);          // auipc t0,0
  EXPECT_EQ(enc::m(5, 10, 11, 12), 0x02C5D533u);              // divu a0,a1,a2
  EXPECT_EQ(enc::m(7, 10, 11, 12), 0x02C5F533u);              // remu a0,a1,a2
}

// ---------------------------------------------------------------------------
// 2. Literal encodings decode to the right op and fields.
// ---------------------------------------------------------------------------
TEST(Decode, WellKnownLiterals) {
  auto nop = decode(0x00000013);
  EXPECT_EQ(nop.op, Op::ADDI);
  EXPECT_EQ(nop.rd, 0);
  EXPECT_EQ(nop.imm, 0);

  auto ret = decode(0x00008067);
  EXPECT_EQ(ret.op, Op::JALR);
  EXPECT_EQ(ret.rs1, 1);
  EXPECT_EQ(ret.rd, 0);

  EXPECT_EQ(decode(0x00000073).op, Op::ECALL);
  EXPECT_EQ(decode(0x00100073).op, Op::EBREAK);
  EXPECT_EQ(decode(0x0000100F).op, Op::FENCE_I);
  EXPECT_EQ(decode(0x0FF0000F).op, Op::FENCE);  // fence iorw,iorw

  auto lui = decode(0x12345537);
  EXPECT_EQ(lui.op, Op::LUI);
  EXPECT_EQ(lui.fmt, Format::U);
  EXPECT_EQ(lui.rd, 10);
  EXPECT_EQ(lui.imm, 0x12345000);
}

// ---------------------------------------------------------------------------
// 3. Every opcode, via the encoder.
// ---------------------------------------------------------------------------
struct RCase { Op op; uint32_t f7, f3; };
class DecodeR : public ::testing::TestWithParam<RCase> {};
TEST_P(DecodeR, FieldsAndOp) {
  const auto c = GetParam();
  auto d = decode(enc::r(c.f7, 21, 17, c.f3, 9, enc::OP));
  EXPECT_EQ(d.op, c.op) << rvx::mnemonic(c.op);
  EXPECT_EQ(d.fmt, Format::R);
  EXPECT_EQ(d.rd, 9);
  EXPECT_EQ(d.rs1, 17);
  EXPECT_EQ(d.rs2, 21);
  EXPECT_EQ(d.imm, 0);
}
INSTANTIATE_TEST_SUITE_P(AllR, DecodeR, ::testing::Values(
    RCase{Op::ADD, 0, 0}, RCase{Op::SUB, 0x20, 0}, RCase{Op::SLL, 0, 1}, RCase{Op::SLT, 0, 2},
    RCase{Op::SLTU, 0, 3}, RCase{Op::XOR, 0, 4}, RCase{Op::SRL, 0, 5}, RCase{Op::SRA, 0x20, 5},
    RCase{Op::OR, 0, 6}, RCase{Op::AND, 0, 7},
    RCase{Op::MUL, 1, 0}, RCase{Op::MULH, 1, 1}, RCase{Op::MULHSU, 1, 2}, RCase{Op::MULHU, 1, 3},
    RCase{Op::DIV, 1, 4}, RCase{Op::DIVU, 1, 5}, RCase{Op::REM, 1, 6}, RCase{Op::REMU, 1, 7}));

struct ICase { Op op; uint32_t opcode, f3; };
class DecodeI : public ::testing::TestWithParam<ICase> {};
TEST_P(DecodeI, ImmediateExtremes) {
  const auto c = GetParam();
  for (int32_t imm : {0, 1, -1, 2047, -2048, 0x555, -0x556}) {
    auto d = decode(enc::i(imm, 3, c.f3, 31, c.opcode));
    EXPECT_EQ(d.op, c.op) << rvx::mnemonic(c.op) << " imm=" << imm;
    EXPECT_EQ(d.fmt, Format::I);
    EXPECT_EQ(d.rd, 31);
    EXPECT_EQ(d.rs1, 3);
    EXPECT_EQ(d.rs2, 0);
    EXPECT_EQ(d.imm, imm);
  }
}
INSTANTIATE_TEST_SUITE_P(AllI, DecodeI, ::testing::Values(
    ICase{Op::JALR, 0x67, 0},
    ICase{Op::LB, 0x03, 0}, ICase{Op::LH, 0x03, 1}, ICase{Op::LW, 0x03, 2},
    ICase{Op::LBU, 0x03, 4}, ICase{Op::LHU, 0x03, 5},
    ICase{Op::ADDI, 0x13, 0}, ICase{Op::SLTI, 0x13, 2}, ICase{Op::SLTIU, 0x13, 3},
    ICase{Op::XORI, 0x13, 4}, ICase{Op::ORI, 0x13, 6}, ICase{Op::ANDI, 0x13, 7}));

TEST(Decode, ShiftImmediatesUseShamt) {
  for (uint32_t sh : {0u, 1u, 17u, 31u}) {
    auto slli = decode(enc::r(0, sh, 4, 1, 5, enc::OP_IMM));
    auto srli = decode(enc::r(0, sh, 4, 5, 5, enc::OP_IMM));
    auto srai = decode(enc::r(0x20, sh, 4, 5, 5, enc::OP_IMM));
    EXPECT_EQ(slli.op, Op::SLLI);
    EXPECT_EQ(srli.op, Op::SRLI);
    EXPECT_EQ(srai.op, Op::SRAI);
    // SRAI's raw I-immediate would include the 0x400 funct7 bit; imm must be
    // only the shift amount.
    for (auto& d : {slli, srli, srai}) {
      EXPECT_EQ(d.imm, static_cast<int32_t>(sh));
      EXPECT_EQ(d.fmt, Format::I);
      EXPECT_EQ(d.rd, 5);
      EXPECT_EQ(d.rs1, 4);
      EXPECT_EQ(d.rs2, 0);
    }
  }
}

TEST(Decode, StoresAllWidthsAndImmediates) {
  const Op ops[] = {Op::SB, Op::SH, Op::SW};
  for (uint32_t f3 = 0; f3 < 3; ++f3) {
    for (int32_t imm : {0, 1, -1, 2047, -2048, 31, 32, -33}) {
      auto d = decode(enc::s(imm, 7, 8, f3, enc::STORE));
      EXPECT_EQ(d.op, ops[f3]);
      EXPECT_EQ(d.fmt, Format::S);
      EXPECT_EQ(d.rs1, 8);
      EXPECT_EQ(d.rs2, 7);
      EXPECT_EQ(d.rd, 0);
      EXPECT_EQ(d.imm, imm) << "f3=" << f3;
    }
  }
}

TEST(Decode, BranchesAllConditionsAndOffsets) {
  const std::pair<uint32_t, Op> conds[] = {{0, Op::BEQ}, {1, Op::BNE}, {4, Op::BLT},
                                           {5, Op::BGE}, {6, Op::BLTU}, {7, Op::BGEU}};
  for (auto [f3, op] : conds) {
    for (int32_t off : {0, 2, -2, 4, -4, 4094, -4096, 2048, -2050, 0x7FE}) {
      auto d = decode(enc::b(off, 30, 29, f3, enc::BRANCH));
      EXPECT_EQ(d.op, op);
      EXPECT_EQ(d.fmt, Format::B);
      EXPECT_EQ(d.rs1, 29);
      EXPECT_EQ(d.rs2, 30);
      EXPECT_EQ(d.rd, 0);
      EXPECT_EQ(d.imm, off) << "f3=" << f3;
    }
  }
}

TEST(Decode, JalOffsets) {
  for (int32_t off : {0, 2, -2, 4, -4, 0xFFFFE, -0x100000, 0x800, -0x802, 0x1000, 0x7FE}) {
    auto d = decode(enc::jal(1, off));
    EXPECT_EQ(d.op, Op::JAL);
    EXPECT_EQ(d.fmt, Format::J);
    EXPECT_EQ(d.rd, 1);
    EXPECT_EQ(d.rs1, 0);
    EXPECT_EQ(d.imm, off);
  }
}

TEST(Decode, UpperImmediates) {
  for (uint32_t imm20 : {0u, 1u, 0x80000u, 0xFFFFFu, 0x12345u}) {
    auto lui = decode(enc::u(imm20, 7, enc::LUI_));
    auto auipc = decode(enc::u(imm20, 7, enc::AUIPC_));
    EXPECT_EQ(lui.op, Op::LUI);
    EXPECT_EQ(auipc.op, Op::AUIPC);
    EXPECT_EQ(lui.imm, static_cast<int32_t>(imm20 << 12));
    EXPECT_EQ(auipc.imm, static_cast<int32_t>(imm20 << 12));
    EXPECT_EQ(auipc.rd, 7);
  }
}

TEST(Decode, SystemAndFenceHaveNoRegisterOperands) {
  for (uint32_t raw : {0x00000073u, 0x00100073u, 0x0000100Fu, 0x0FF0000Fu}) {
    auto d = decode(raw);
    EXPECT_EQ(d.fmt, Format::None);
    EXPECT_EQ(d.rd, 0);
    EXPECT_EQ(d.rs1, 0);
    EXPECT_EQ(d.rs2, 0);
  }
}

// ---------------------------------------------------------------------------
// 4. Encodings that must be rejected.
// ---------------------------------------------------------------------------
TEST(Decode, IllegalEncodings) {
  const uint32_t illegal[] = {
      0x00000000,                                // all zeros (defined illegal)
      0xFFFFFFFF,                                // all ones
      0x00000001,                                // compressed (low bits != 11)
      0xF1402573,                                // csrr a0, mhartid (no Zicsr)
      0x30200073,                                // mret
      0x10500073,                                // wfi
      enc::r(0x02, 2, 1, 0, 3, enc::OP),         // OP with bad funct7
      enc::r(0x20, 2, 1, 1, 3, enc::OP),         // "sub"-style funct7 on SLL
      enc::r(0x20, 3, 1, 1, 2, enc::OP_IMM),     // slli with funct7=0x20
      0x02051513,                                // slli a0,a0,32 (RV64 shamt[5])
      enc::r(0x01, 3, 1, 5, 2, enc::OP_IMM),     // srli with junk in funct7
      enc::b(8, 1, 2, 2, enc::BRANCH),           // branch funct3=2
      enc::b(8, 1, 2, 3, enc::BRANCH),           // branch funct3=3
      enc::i(0, 1, 3, 2, enc::LOAD),             // ld (RV64)
      enc::i(0, 1, 6, 2, enc::LOAD),             // lwu (RV64)
      enc::i(0, 1, 7, 2, enc::LOAD),             // load funct3=7
      enc::s(0, 1, 2, 3, enc::STORE),            // sd (RV64)
      enc::s(0, 1, 2, 4, enc::STORE),            // store funct3=4
      enc::i(0, 1, 1, 2, enc::JALR_),            // jalr funct3!=0
      enc::r(0, 2, 1, 0, 3, 0x3B),               // addw (RV64 OP-32)
      enc::i(0, 1, 2, 2, enc::MISC_MEM),         // misc-mem funct3=2
  };
  for (uint32_t raw : illegal) {
    auto d = decode(raw);
    EXPECT_EQ(d.op, Op::ILLEGAL) << std::hex << "raw=0x" << raw << " decoded as "
                                 << rvx::mnemonic(d.op);
    EXPECT_EQ(d.fmt, Format::None);
    EXPECT_EQ(d.raw, raw);
  }
}

TEST(Decode, MnemonicsAreNonEmpty) {
  for (int k = 0; k <= static_cast<int>(Op::ILLEGAL); ++k) {
    EXPECT_FALSE(rvx::mnemonic(static_cast<Op>(k)).empty()) << k;
  }
  EXPECT_EQ(rvx::mnemonic(Op::MULHSU), "mulhsu");
  EXPECT_EQ(rvx::mnemonic(Op::FENCE_I), "fence.i");
}
