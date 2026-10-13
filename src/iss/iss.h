// Golden functional instruction-set simulator for RV32IM.
//
// Executes one instruction per step() with no notion of time. This is the
// reference model; the Phase 2 pipeline is checked against its retire trace.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

#include "iss/memory.h"
#include "iss/retire.h"

namespace rvx::iss {

enum class HaltReason : uint8_t {
  None,             // still running
  ToHost,           // program stored to the tohost address (normal end)
  IllegalInsn,
  MisalignedFetch,  // jump/branch target not 4-byte aligned, or misaligned pc
  MisalignedLoad,
  MisalignedStore,
  Ecall,
  Ebreak,
  MaxInsns,         // instruction budget exhausted (only set by run())
};

std::string_view to_string(HaltReason r);

struct RunResult {
  HaltReason reason = HaltReason::None;
  uint64_t retired = 0;
  uint32_t pc = 0;            // pc of the faulting/next instruction
  uint32_t tohost_value = 0;  // valid when reason == ToHost
};

class Iss {
 public:
  Iss(Memory& mem, uint32_t entry_pc);

  // When set, a store whose address equals this value retires normally and
  // then halts the simulation with HaltReason::ToHost.
  void set_tohost(std::optional<uint32_t> addr) { tohost_ = addr; }

  // Executes one instruction. Returns its retire record, or nullopt if the
  // instruction did not retire (it faulted, or the ISS was already halted).
  // A faulting instruction leaves all architectural state unchanged.
  std::optional<RetireRecord> step();

  // Steps until halted or `max_insns` instructions have retired. The optional
  // sink is called for every retired instruction, in order.
  template <typename Sink>
  RunResult run(uint64_t max_insns, Sink&& sink) {
    while (halt_ == HaltReason::None) {
      if (retired_ >= max_insns) {
        halt_ = HaltReason::MaxInsns;
        break;
      }
      if (auto rec = step()) sink(*rec);
    }
    return result();
  }
  RunResult run(uint64_t max_insns) {
    return run(max_insns, [](const RetireRecord&) {});
  }

  RunResult result() const { return {halt_, retired_, pc_, tohost_value_}; }
  bool halted() const { return halt_ != HaltReason::None; }

  uint32_t pc() const { return pc_; }
  uint32_t reg(unsigned i) const { return x_[i]; }
  void set_reg(unsigned i, uint32_t v) {
    if (i != 0) x_[i] = v;
  }
  uint64_t retired() const { return retired_; }

 private:
  void halt(HaltReason r) { halt_ = r; }

  Memory& mem_;
  std::array<uint32_t, 32> x_{};
  uint32_t pc_;
  uint64_t retired_ = 0;
  HaltReason halt_ = HaltReason::None;
  std::optional<uint32_t> tohost_;
  uint32_t tohost_value_ = 0;
};

}  // namespace rvx::iss
