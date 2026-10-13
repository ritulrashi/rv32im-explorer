// Test-only instruction encoder, written directly from the format tables in
// the RISC-V Unprivileged ISA spec (chapter "RV32I Base Integer Instruction
// Set", figure "RISC-V base instruction formats").
//
// It exists so decode tests do not depend on decode.cpp being right in order
// to construct their inputs, and so ISS tests can place instructions in
// memory without an assembler. It is itself checked against hand-verified
// literal encodings in decode_test.cpp.
#pragma once

#include <cstdint>

namespace enc {

constexpr uint32_t r(uint32_t f7, uint32_t rs2, uint32_t rs1, uint32_t f3, uint32_t rd, uint32_t op) {
  return (f7 << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | op;
}
constexpr uint32_t i(int32_t imm, uint32_t rs1, uint32_t f3, uint32_t rd, uint32_t op) {
  return ((static_cast<uint32_t>(imm) & 0xFFFu) << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | op;
}
constexpr uint32_t s(int32_t imm, uint32_t rs2, uint32_t rs1, uint32_t f3, uint32_t op) {
  const uint32_t u = static_cast<uint32_t>(imm);
  return (((u >> 5) & 0x7Fu) << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) | ((u & 0x1Fu) << 7) | op;
}
// B-type: imm[12|10:5] in bits 31:25, imm[4:1|11] in bits 11:7.
constexpr uint32_t b(int32_t imm, uint32_t rs2, uint32_t rs1, uint32_t f3, uint32_t op) {
  const uint32_t u = static_cast<uint32_t>(imm);
  return (((u >> 12) & 1u) << 31) | (((u >> 5) & 0x3Fu) << 25) | (rs2 << 20) | (rs1 << 15) |
         (f3 << 12) | (((u >> 1) & 0xFu) << 8) | (((u >> 11) & 1u) << 7) | op;
}
constexpr uint32_t u(uint32_t imm20, uint32_t rd, uint32_t op) { return (imm20 << 12) | (rd << 7) | op; }
// J-type: imm[20|10:1|11|19:12] in bits 31:12.
constexpr uint32_t j(int32_t imm, uint32_t rd, uint32_t op) {
  const uint32_t v = static_cast<uint32_t>(imm);
  return (((v >> 20) & 1u) << 31) | (((v >> 1) & 0x3FFu) << 21) | (((v >> 11) & 1u) << 20) |
         (((v >> 12) & 0xFFu) << 12) | (rd << 7) | op;
}

constexpr uint32_t OP = 0x33, OP_IMM = 0x13, LOAD = 0x03, STORE = 0x23, BRANCH = 0x63;
constexpr uint32_t JAL_ = 0x6F, JALR_ = 0x67, LUI_ = 0x37, AUIPC_ = 0x17, MISC_MEM = 0x0F, SYSTEM = 0x73;

// Convenience wrappers used by ISS tests.
constexpr uint32_t add(uint32_t rd, uint32_t a, uint32_t b_) { return r(0, b_, a, 0, rd, OP); }
constexpr uint32_t addi(uint32_t rd, uint32_t a, int32_t imm) { return i(imm, a, 0, rd, OP_IMM); }
constexpr uint32_t lw(uint32_t rd, uint32_t base, int32_t off) { return i(off, base, 2, rd, LOAD); }
constexpr uint32_t sw(uint32_t src, uint32_t base, int32_t off) { return s(off, src, base, 2, STORE); }
constexpr uint32_t jal(uint32_t rd, int32_t off) { return j(off, rd, JAL_); }
constexpr uint32_t jalr(uint32_t rd, uint32_t base, int32_t off) { return i(off, base, 0, rd, JALR_); }
constexpr uint32_t m(uint32_t f3, uint32_t rd, uint32_t a, uint32_t b_) { return r(1, b_, a, f3, rd, OP); }

}  // namespace enc
