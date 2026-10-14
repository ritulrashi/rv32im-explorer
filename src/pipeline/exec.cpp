#include "pipeline/exec.h"

namespace rvx::pipe::exec {
namespace {

constexpr uint32_t kSign = 0x80000000u;

// Signed less-than without signed types: flipping the sign bit maps
// [-2^31, 2^31) monotonically onto [0, 2^32).
bool lt_signed(uint32_t a, uint32_t b) { return (a ^ kSign) < (b ^ kSign); }

uint32_t shift_right_arith(uint32_t a, unsigned s) {
  const uint32_t logical = a >> s;
  // If negative, fill the s vacated top bits with ones.
  return (a & kSign) ? (logical | ~(0xFFFFFFFFu >> s)) : logical;
}

uint32_t mulhu(uint32_t a, uint32_t b) {
  return static_cast<uint32_t>((uint64_t{a} * uint64_t{b}) >> 32);
}

// Reading a 32-bit pattern as signed means value = u - 2^32*sign. Expanding
// (a_u - 2^32*sa)(b_u - 2^32*sb) and keeping bits 63:32 (mod 2^32) gives
//   high_signed = mulhu(a,b) - sa*b - sb*a.
// MULHSU only has the correction for a.
uint32_t mulh(uint32_t a, uint32_t b) {
  uint32_t h = mulhu(a, b);
  if (a & kSign) h -= b;
  if (b & kSign) h -= a;
  return h;
}
uint32_t mulhsu(uint32_t a, uint32_t b) {
  uint32_t h = mulhu(a, b);
  if (a & kSign) h -= b;
  return h;
}

struct QuotRem {
  uint32_t q;
  uint32_t r;
};

// Restoring division, one quotient bit per step, MSB first. With d == 0 every
// trial subtraction succeeds, which yields q = all ones and r = n: exactly
// the values RISC-V specifies for division by zero.
QuotRem udivrem(uint32_t n, uint32_t d) {
  uint64_t r = 0;  // 33 bits are enough; 64 keeps the shift simple
  uint32_t q = 0;
  for (int i = 31; i >= 0; --i) {
    r = (r << 1) | ((n >> i) & 1u);
    if (r >= d) {
      r -= d;
      q |= 1u << i;
    }
  }
  return {q, static_cast<uint32_t>(r)};
}

uint32_t negate(uint32_t v) { return 0u - v; }
uint32_t magnitude(uint32_t v) { return (v & kSign) ? negate(v) : v; }

// Signed division on magnitudes, then sign fix-up:
//  * quotient is negative iff operand signs differ, except for d == 0, where
//    the spec requires -1 regardless of the dividend's sign;
//  * remainder takes the sign of the dividend.
// INT_MIN / -1 needs no special case: |INT_MIN| = 0x80000000, the quotient
// 0x80000000 negates to itself, and the remainder is 0.
QuotRem sdivrem(uint32_t n, uint32_t d) {
  QuotRem m = udivrem(magnitude(n), magnitude(d));
  const bool neg_q = d != 0 && ((n ^ d) & kSign);
  return {neg_q ? negate(m.q) : m.q, (n & kSign) ? negate(m.r) : m.r};
}

}  // namespace

bool is_load(Op op) {
  return op == Op::LB || op == Op::LH || op == Op::LW || op == Op::LBU || op == Op::LHU;
}
bool is_store(Op op) { return op == Op::SB || op == Op::SH || op == Op::SW; }
bool is_branch(Op op) {
  return op == Op::BEQ || op == Op::BNE || op == Op::BLT || op == Op::BGE || op == Op::BLTU ||
         op == Op::BGEU;
}
bool is_mul(Op op) { return op == Op::MUL || op == Op::MULH || op == Op::MULHSU || op == Op::MULHU; }
bool is_div(Op op) { return op == Op::DIV || op == Op::DIVU || op == Op::REM || op == Op::REMU; }

unsigned access_size(Op op) {
  switch (op) {
    case Op::LB: case Op::LBU: case Op::SB: return 1;
    case Op::LH: case Op::LHU: case Op::SH: return 2;
    default: return 4;
  }
}

uint32_t alu(Op op, uint32_t a, uint32_t b) {
  const unsigned sh = b & 31u;
  switch (op) {
    case Op::ADD: case Op::ADDI: return a + b;
    case Op::SUB: return a - b;
    case Op::SLL: case Op::SLLI: return a << sh;
    case Op::SLT: case Op::SLTI: return lt_signed(a, b) ? 1u : 0u;
    case Op::SLTU: case Op::SLTIU: return a < b ? 1u : 0u;
    case Op::XOR: case Op::XORI: return a ^ b;
    case Op::SRL: case Op::SRLI: return a >> sh;
    case Op::SRA: case Op::SRAI: return shift_right_arith(a, sh);
    case Op::OR: case Op::ORI: return a | b;
    case Op::AND: case Op::ANDI: return a & b;
    default: return 0;
  }
}

bool branch_taken(Op op, uint32_t a, uint32_t b) {
  switch (op) {
    case Op::BEQ: return a == b;
    case Op::BNE: return a != b;
    case Op::BLT: return lt_signed(a, b);
    case Op::BGE: return !lt_signed(a, b);
    case Op::BLTU: return a < b;
    case Op::BGEU: return !(a < b);
    default: return false;
  }
}

uint32_t muldiv(Op op, uint32_t a, uint32_t b) {
  switch (op) {
    case Op::MUL: return a * b;
    case Op::MULH: return mulh(a, b);
    case Op::MULHSU: return mulhsu(a, b);
    case Op::MULHU: return mulhu(a, b);
    case Op::DIV: return sdivrem(a, b).q;
    case Op::DIVU: return udivrem(a, b).q;
    case Op::REM: return sdivrem(a, b).r;
    case Op::REMU: return udivrem(a, b).r;
    default: return 0;
  }
}

uint32_t extend_load(Op op, uint32_t raw) {
  switch (op) {
    case Op::LB: return (raw & 0x80u) ? (raw | 0xFFFFFF00u) : raw;
    case Op::LH: return (raw & 0x8000u) ? (raw | 0xFFFF0000u) : raw;
    default: return raw;  // LBU, LHU, LW: raw is already zero-extended
  }
}

uint32_t store_mask(unsigned size) {
  return size == 1 ? 0xFFu : size == 2 ? 0xFFFFu : 0xFFFFFFFFu;
}

}  // namespace rvx::pipe::exec
