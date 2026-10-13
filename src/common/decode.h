// Instruction decode for RV32IM.
//
// This is the ONLY code shared between the golden ISS (src/iss) and the
// pipeline model (src/pipeline). It maps instruction bits to fields and an
// operation enum. It deliberately contains no semantics: no ALU, no branch
// conditions, no memory sizing. Each model implements those independently so
// that lockstep comparison is meaningful.
#pragma once

#include <cstdint>
#include <string_view>

namespace rvx {

enum class Op : uint8_t {
  // RV32I
  LUI, AUIPC, JAL, JALR,
  BEQ, BNE, BLT, BGE, BLTU, BGEU,
  LB, LH, LW, LBU, LHU,
  SB, SH, SW,
  ADDI, SLTI, SLTIU, XORI, ORI, ANDI, SLLI, SRLI, SRAI,
  ADD, SUB, SLL, SLT, SLTU, XOR, SRL, SRA, OR, AND,
  FENCE, FENCE_I, ECALL, EBREAK,
  // M extension
  MUL, MULH, MULHSU, MULHU, DIV, DIVU, REM, REMU,
  ILLEGAL,
};

// Encoding format. Determines which register fields are meaningful:
//   R: rd, rs1, rs2    I: rd, rs1    S/B: rs1, rs2    U/J: rd
//   None: no register operands (FENCE, FENCE.I, ECALL, EBREAK, ILLEGAL)
enum class Format : uint8_t { R, I, S, B, U, J, None };

struct DecodedInsn {
  uint32_t raw = 0;
  Op op = Op::ILLEGAL;
  Format fmt = Format::None;
  // Register fields that the format does not use are forced to 0, so a
  // consumer that reads them by mistake sees x0 rather than stray bits.
  uint8_t rd = 0;
  uint8_t rs1 = 0;
  uint8_t rs2 = 0;
  // Sign-extended immediate. For SLLI/SRLI/SRAI this is the 5-bit shift
  // amount. For U-type it is already shifted (imm[31:12] << 12). For B/J it
  // is the byte offset (bit 0 always 0). 0 when the format has no immediate.
  int32_t imm = 0;
};

DecodedInsn decode(uint32_t raw);

std::string_view mnemonic(Op op);

}  // namespace rvx
