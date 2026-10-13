#include "iss/iss.h"

#include "common/decode.h"

namespace rvx::iss {

Iss::Iss(Memory& mem, uint32_t entry_pc) : mem_(mem), pc_(entry_pc) {}

namespace {
// All register arithmetic is done on uint32_t, where overflow wraps by
// definition. Signed views are produced by conversion (well-defined modular
// in C++20), never by signed arithmetic that could overflow.
constexpr int32_t s32(uint32_t v) { return static_cast<int32_t>(v); }
constexpr uint32_t sext8(uint32_t v) { return static_cast<uint32_t>(static_cast<int8_t>(v)); }
constexpr uint32_t sext16(uint32_t v) { return static_cast<uint32_t>(static_cast<int16_t>(v)); }
}  // namespace

std::optional<RetireRecord> Iss::step() {
  if (halt_ != HaltReason::None) return std::nullopt;
  if (pc_ & 3u) {
    halt(HaltReason::MisalignedFetch);
    return std::nullopt;
  }

  const uint32_t raw = mem_.load(pc_, 4);
  const DecodedInsn d = decode(raw);
  const uint32_t a = x_[d.rs1];
  const uint32_t b = x_[d.rs2];
  const uint32_t imm = static_cast<uint32_t>(d.imm);

  RetireRecord rec;
  rec.seq = retired_;
  rec.pc = pc_;
  rec.insn = raw;

  // Every case computes the result into these locals; architectural state is
  // only updated at the bottom, after all fault checks have passed. That is
  // what guarantees "a faulting instruction changes nothing".
  uint32_t next_pc = pc_ + 4;
  bool writes_rd = false;
  uint32_t result = 0;

  auto branch = [&](bool taken) { if (taken) next_pc = pc_ + imm; };

  switch (d.op) {
    case Op::LUI: writes_rd = true; result = imm; break;
    case Op::AUIPC: writes_rd = true; result = pc_ + imm; break;
    case Op::JAL: writes_rd = true; result = pc_ + 4; next_pc = pc_ + imm; break;
    case Op::JALR:
      writes_rd = true;
      result = pc_ + 4;
      next_pc = (a + imm) & ~1u;  // uses rs1 read above, before rd is written
      break;

    case Op::BEQ: branch(a == b); break;
    case Op::BNE: branch(a != b); break;
    case Op::BLT: branch(s32(a) < s32(b)); break;
    case Op::BGE: branch(s32(a) >= s32(b)); break;
    case Op::BLTU: branch(a < b); break;
    case Op::BGEU: branch(a >= b); break;

    case Op::LB: case Op::LH: case Op::LW: case Op::LBU: case Op::LHU: {
      const unsigned size = (d.op == Op::LW) ? 4 : (d.op == Op::LH || d.op == Op::LHU) ? 2 : 1;
      const uint32_t addr = a + imm;
      if (addr % size != 0) {
        halt(HaltReason::MisalignedLoad);
        return std::nullopt;
      }
      const uint32_t v = mem_.load(addr, size);
      rec.mem = MemKind::Read;
      rec.mem_addr = addr;
      rec.mem_data = v;
      rec.mem_size = static_cast<uint8_t>(size);
      writes_rd = true;
      result = d.op == Op::LB ? sext8(v) : d.op == Op::LH ? sext16(v) : v;
      break;
    }

    case Op::SB: case Op::SH: case Op::SW: {
      const unsigned size = d.op == Op::SW ? 4 : d.op == Op::SH ? 2 : 1;
      const uint32_t addr = a + imm;
      if (addr % size != 0) {
        halt(HaltReason::MisalignedStore);
        return std::nullopt;
      }
      const uint32_t mask = size == 4 ? 0xFFFFFFFFu : (1u << (8 * size)) - 1;
      rec.mem = MemKind::Write;
      rec.mem_addr = addr;
      rec.mem_data = b & mask;
      rec.mem_size = static_cast<uint8_t>(size);
      break;  // the memory write itself happens at commit, below
    }

    case Op::ADDI: writes_rd = true; result = a + imm; break;
    case Op::SLTI: writes_rd = true; result = s32(a) < s32(imm); break;
    case Op::SLTIU: writes_rd = true; result = a < imm; break;
    case Op::XORI: writes_rd = true; result = a ^ imm; break;
    case Op::ORI: writes_rd = true; result = a | imm; break;
    case Op::ANDI: writes_rd = true; result = a & imm; break;
    case Op::SLLI: writes_rd = true; result = a << imm; break;  // imm = shamt (0..31)
    case Op::SRLI: writes_rd = true; result = a >> imm; break;
    case Op::SRAI: writes_rd = true; result = static_cast<uint32_t>(s32(a) >> imm); break;

    case Op::ADD: writes_rd = true; result = a + b; break;
    case Op::SUB: writes_rd = true; result = a - b; break;
    case Op::SLL: writes_rd = true; result = a << (b & 31); break;
    case Op::SLT: writes_rd = true; result = s32(a) < s32(b); break;
    case Op::SLTU: writes_rd = true; result = a < b; break;
    case Op::XOR: writes_rd = true; result = a ^ b; break;
    case Op::SRL: writes_rd = true; result = a >> (b & 31); break;
    case Op::SRA: writes_rd = true; result = static_cast<uint32_t>(s32(a) >> (b & 31)); break;
    case Op::OR: writes_rd = true; result = a | b; break;
    case Op::AND: writes_rd = true; result = a & b; break;

    // M extension. Products are formed in 64 bits; the operand widening
    // decides signedness: int64 from s32() sign-extends, uint64 zero-extends.
    case Op::MUL: writes_rd = true; result = a * b; break;
    case Op::MULH:
      writes_rd = true;
      result = static_cast<uint32_t>((int64_t{s32(a)} * int64_t{s32(b)}) >> 32);
      break;
    case Op::MULHSU:
      // signed a in [-2^31, 2^31) times unsigned b in [0, 2^32) lies within
      // int64 range, so this multiplication cannot overflow.
      writes_rd = true;
      result = static_cast<uint32_t>((int64_t{s32(a)} * static_cast<int64_t>(uint64_t{b})) >> 32);
      break;
    case Op::MULHU:
      writes_rd = true;
      result = static_cast<uint32_t>((uint64_t{a} * uint64_t{b}) >> 32);
      break;
    // Division special cases are defined by the spec (no traps):
    //   x / 0 = -1 (all ones), x % 0 = x, INT_MIN / -1 = INT_MIN, INT_MIN % -1 = 0.
    // The overflow case must be handled explicitly: in C++ it is UB.
    case Op::DIV:
      writes_rd = true;
      if (b == 0) result = 0xFFFFFFFFu;
      else if (a == 0x80000000u && b == 0xFFFFFFFFu) result = 0x80000000u;
      else result = static_cast<uint32_t>(s32(a) / s32(b));
      break;
    case Op::DIVU: writes_rd = true; result = b == 0 ? 0xFFFFFFFFu : a / b; break;
    case Op::REM:
      writes_rd = true;
      if (b == 0) result = a;
      else if (a == 0x80000000u && b == 0xFFFFFFFFu) result = 0;
      else result = static_cast<uint32_t>(s32(a) % s32(b));
      break;
    case Op::REMU: writes_rd = true; result = b == 0 ? a : a % b; break;

    case Op::FENCE:
    case Op::FENCE_I:
      break;  // no caches or reordering in a functional model

    case Op::ECALL: halt(HaltReason::Ecall); return std::nullopt;
    case Op::EBREAK: halt(HaltReason::Ebreak); return std::nullopt;
    case Op::ILLEGAL: halt(HaltReason::IllegalInsn); return std::nullopt;
  }

  // Without the C extension every instruction is 4-byte aligned, so a jump or
  // taken branch to a non-multiple-of-4 target faults on the jump itself.
  if (next_pc & 3u) {
    halt(HaltReason::MisalignedFetch);
    return std::nullopt;
  }

  // ---- Commit: the only place architectural state changes. ----
  if (rec.mem == MemKind::Write) mem_.store(rec.mem_addr, rec.mem_size, rec.mem_data);
  if (writes_rd && d.rd != 0) {
    x_[d.rd] = result;
    rec.rd = d.rd;
    rec.rd_val = result;
  }
  pc_ = next_pc;
  ++retired_;

  if (rec.mem == MemKind::Write && tohost_ && rec.mem_addr == *tohost_) {
    tohost_value_ = rec.mem_data;
    halt(HaltReason::ToHost);
  }
  return rec;
}

}  // namespace rvx::iss
