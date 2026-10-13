#include "common/decode.h"

namespace rvx {
namespace {

// Field extractors (bit positions from the spec's base instruction formats).
constexpr uint32_t opcode(uint32_t r) { return r & 0x7F; }
constexpr uint8_t rd(uint32_t r) { return static_cast<uint8_t>((r >> 7) & 0x1F); }
constexpr uint32_t funct3(uint32_t r) { return (r >> 12) & 0x7; }
constexpr uint8_t rs1(uint32_t r) { return static_cast<uint8_t>((r >> 15) & 0x1F); }
constexpr uint8_t rs2(uint32_t r) { return static_cast<uint8_t>((r >> 20) & 0x1F); }
constexpr uint32_t funct7(uint32_t r) { return r >> 25; }

// Immediates. The trick for sign extension: cast to int32_t first, so the
// right shift of the word is arithmetic and copies bit 31 (always the sign bit
// of every RISC-V immediate) into the upper bits. C++20 guarantees this.
constexpr int32_t imm_i(uint32_t r) { return static_cast<int32_t>(r) >> 20; }
constexpr int32_t imm_s(uint32_t r) {
  return (static_cast<int32_t>(r & 0xFE000000u) >> 20) | static_cast<int32_t>((r >> 7) & 0x1F);
}
// B: imm[12]=r[31], imm[11]=r[7], imm[10:5]=r[30:25], imm[4:1]=r[11:8]
constexpr int32_t imm_b(uint32_t r) {
  return (static_cast<int32_t>(r & 0x80000000u) >> 19) |
         static_cast<int32_t>(((r & 0x80u) << 4) | ((r >> 20) & 0x7E0u) | ((r >> 7) & 0x1Eu));
}
constexpr int32_t imm_u(uint32_t r) { return static_cast<int32_t>(r & 0xFFFFF000u); }
// J: imm[20]=r[31], imm[19:12]=r[19:12], imm[11]=r[20], imm[10:1]=r[30:21]
constexpr int32_t imm_j(uint32_t r) {
  return (static_cast<int32_t>(r & 0x80000000u) >> 11) |
         static_cast<int32_t>((r & 0xFF000u) | ((r >> 9) & 0x800u) | ((r >> 20) & 0x7FEu));
}

DecodedInsn make_r(uint32_t r, Op op) { return {r, op, Format::R, rd(r), rs1(r), rs2(r), 0}; }
DecodedInsn make_i(uint32_t r, Op op) { return {r, op, Format::I, rd(r), rs1(r), 0, imm_i(r)}; }
DecodedInsn make_shift(uint32_t r, Op op) {
  return {r, op, Format::I, rd(r), rs1(r), 0, static_cast<int32_t>(rs2(r))};
}
DecodedInsn make_s(uint32_t r, Op op) { return {r, op, Format::S, 0, rs1(r), rs2(r), imm_s(r)}; }
DecodedInsn make_b(uint32_t r, Op op) { return {r, op, Format::B, 0, rs1(r), rs2(r), imm_b(r)}; }
DecodedInsn make_u(uint32_t r, Op op) { return {r, op, Format::U, rd(r), 0, 0, imm_u(r)}; }
DecodedInsn make_j(uint32_t r, Op op) { return {r, op, Format::J, rd(r), 0, 0, imm_j(r)}; }
DecodedInsn make_none(uint32_t r, Op op) { return {r, op, Format::None, 0, 0, 0, 0}; }
DecodedInsn illegal(uint32_t r) { return make_none(r, Op::ILLEGAL); }

}  // namespace

DecodedInsn decode(uint32_t r) {
  const uint32_t f3 = funct3(r);
  const uint32_t f7 = funct7(r);
  switch (opcode(r)) {
    case 0x37: return make_u(r, Op::LUI);
    case 0x17: return make_u(r, Op::AUIPC);
    case 0x6F: return make_j(r, Op::JAL);
    case 0x67: return f3 == 0 ? make_i(r, Op::JALR) : illegal(r);

    case 0x63: {
      static constexpr Op kBranch[8] = {Op::BEQ, Op::BNE, Op::ILLEGAL, Op::ILLEGAL,
                                        Op::BLT, Op::BGE, Op::BLTU,    Op::BGEU};
      return kBranch[f3] == Op::ILLEGAL ? illegal(r) : make_b(r, kBranch[f3]);
    }
    case 0x03: {
      static constexpr Op kLoad[8] = {Op::LB,  Op::LH,  Op::LW,      Op::ILLEGAL,
                                      Op::LBU, Op::LHU, Op::ILLEGAL, Op::ILLEGAL};
      return kLoad[f3] == Op::ILLEGAL ? illegal(r) : make_i(r, kLoad[f3]);
    }
    case 0x23: {
      static constexpr Op kStore[3] = {Op::SB, Op::SH, Op::SW};
      return f3 < 3 ? make_s(r, kStore[f3]) : illegal(r);
    }

    case 0x13:  // OP-IMM
      switch (f3) {
        case 0: return make_i(r, Op::ADDI);
        case 2: return make_i(r, Op::SLTI);
        case 3: return make_i(r, Op::SLTIU);
        case 4: return make_i(r, Op::XORI);
        case 6: return make_i(r, Op::ORI);
        case 7: return make_i(r, Op::ANDI);
        // On RV32 the shift amount is 5 bits; bit 25 (shamt[5] on RV64) must
        // be 0, which the funct7 check enforces.
        case 1: return f7 == 0x00 ? make_shift(r, Op::SLLI) : illegal(r);
        case 5:
          if (f7 == 0x00) return make_shift(r, Op::SRLI);
          if (f7 == 0x20) return make_shift(r, Op::SRAI);
          return illegal(r);
      }
      return illegal(r);  // unreachable: f3 is 3 bits

    case 0x33:  // OP
      if (f7 == 0x00) {
        static constexpr Op kBase[8] = {Op::ADD, Op::SLL, Op::SLT, Op::SLTU,
                                        Op::XOR, Op::SRL, Op::OR,  Op::AND};
        return make_r(r, kBase[f3]);
      }
      if (f7 == 0x20) {
        if (f3 == 0) return make_r(r, Op::SUB);
        if (f3 == 5) return make_r(r, Op::SRA);
        return illegal(r);
      }
      if (f7 == 0x01) {
        static constexpr Op kMul[8] = {Op::MUL, Op::MULH, Op::MULHSU, Op::MULHU,
                                       Op::DIV, Op::DIVU, Op::REM,    Op::REMU};
        return make_r(r, kMul[f3]);
      }
      return illegal(r);

    case 0x0F:  // MISC-MEM. Unused FENCE fields are ignored, as the spec
                // requires for forward compatibility.
      if (f3 == 0) return make_none(r, Op::FENCE);
      if (f3 == 1) return make_none(r, Op::FENCE_I);
      return illegal(r);

    case 0x73:  // SYSTEM. Only ECALL/EBREAK; everything else (CSRs, MRET,
                // WFI, ...) is outside the supported ISA.
      if (r == 0x00000073u) return make_none(r, Op::ECALL);
      if (r == 0x00100073u) return make_none(r, Op::EBREAK);
      return illegal(r);
  }
  // Includes every encoding whose low two bits are not 0b11 (compressed).
  return illegal(r);
}

std::string_view mnemonic(Op op) {
  switch (op) {
    case Op::LUI: return "lui";
    case Op::AUIPC: return "auipc";
    case Op::JAL: return "jal";
    case Op::JALR: return "jalr";
    case Op::BEQ: return "beq";
    case Op::BNE: return "bne";
    case Op::BLT: return "blt";
    case Op::BGE: return "bge";
    case Op::BLTU: return "bltu";
    case Op::BGEU: return "bgeu";
    case Op::LB: return "lb";
    case Op::LH: return "lh";
    case Op::LW: return "lw";
    case Op::LBU: return "lbu";
    case Op::LHU: return "lhu";
    case Op::SB: return "sb";
    case Op::SH: return "sh";
    case Op::SW: return "sw";
    case Op::ADDI: return "addi";
    case Op::SLTI: return "slti";
    case Op::SLTIU: return "sltiu";
    case Op::XORI: return "xori";
    case Op::ORI: return "ori";
    case Op::ANDI: return "andi";
    case Op::SLLI: return "slli";
    case Op::SRLI: return "srli";
    case Op::SRAI: return "srai";
    case Op::ADD: return "add";
    case Op::SUB: return "sub";
    case Op::SLL: return "sll";
    case Op::SLT: return "slt";
    case Op::SLTU: return "sltu";
    case Op::XOR: return "xor";
    case Op::SRL: return "srl";
    case Op::SRA: return "sra";
    case Op::OR: return "or";
    case Op::AND: return "and";
    case Op::FENCE: return "fence";
    case Op::FENCE_I: return "fence.i";
    case Op::ECALL: return "ecall";
    case Op::EBREAK: return "ebreak";
    case Op::MUL: return "mul";
    case Op::MULH: return "mulh";
    case Op::MULHSU: return "mulhsu";
    case Op::MULHU: return "mulhu";
    case Op::DIV: return "div";
    case Op::DIVU: return "divu";
    case Op::REM: return "rem";
    case Op::REMU: return "remu";
    case Op::ILLEGAL: return "illegal";
  }
  return "?";
}

}  // namespace rvx
