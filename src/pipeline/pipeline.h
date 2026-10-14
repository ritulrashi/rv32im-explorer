// Cycle-accurate 5-stage in-order RV32IM pipeline (IF ID EX MEM WB).
//
// Implements docs/phase2-pipeline-contract.md exactly. Section numbers in
// comments ("§6.2") refer to that document.
//
// Modeling style: every pipeline register is a flop. tick() computes all
// control signals and next-state values from the CURRENT registers only,
// then commits them together, like a clock edge. Stage evaluation order
// therefore does not matter.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

#include "common/memory.h"
#include "common/retire.h"
#include "pipeline/latches.h"

namespace rvx::pipe {

struct Config {
  unsigned mul_latency = 3;   // cycles a MUL/MULH* occupies EX (>= 1)
  unsigned div_latency = 33;  // cycles a DIV/DIVU/REM/REMU occupies EX (>= 1)
};

// What each stage holds during one cycle, for pipeline diagrams, the
// lockstep checker's divergence report, and (Phase 5) RTL correlation.
struct StageView {
  std::optional<uint32_t> if_pc;  // pc being fetched this cycle (nullopt: fetch frozen)
  std::optional<uint32_t> id_pc, ex_pc, mem_pc, wb_pc;  // nullopt: bubble
  BubbleCause id_cause = BubbleCause::None, ex_cause = BubbleCause::None,
              mem_cause = BubbleCause::None, wb_cause = BubbleCause::None;
};

struct HaltInfo {
  HaltReason reason = HaltReason::None;
  uint32_t pc = 0;            // faulting pc, or (tohost) pc of the next instruction
  uint32_t tohost_value = 0;  // valid when reason == ToHost
};

class Pipeline {
 public:
  Pipeline(Memory& mem, uint32_t entry_pc, Config cfg = {});

  void set_tohost(std::optional<uint32_t> addr) { tohost_ = addr; }

  // Simulates one clock cycle. Returns the instruction retired in WB this
  // cycle, if any. Does nothing once halted.
  std::optional<RetireRecord> tick();

  // Ticks until halted or `max_cycles` cycles have elapsed (then MaxCycles).
  template <typename Sink>
  HaltInfo run(uint64_t max_cycles, Sink&& sink) {
    while (!halted()) {
      if (cycle_ >= max_cycles) {
        halt_.reason = HaltReason::MaxCycles;
        halt_.pc = pc_;
        break;
      }
      if (auto r = tick()) sink(*r);
    }
    return halt_;
  }
  HaltInfo run(uint64_t max_cycles) {
    return run(max_cycles, [](const RetireRecord&) {});
  }

  bool halted() const { return halt_.reason != HaltReason::None; }
  const HaltInfo& halt() const { return halt_; }
  uint64_t cycles() const { return cycle_; }
  uint64_t retired() const { return retired_; }
  uint32_t reg(unsigned i) const { return regs_[i]; }

  // Stage occupancy during the most recent tick() (the cycle just simulated).
  const StageView& last_view() const { return view_; }

  // Multi-line human-readable dump of all pipeline registers and the control
  // signals of the most recent cycle (used in divergence reports).
  std::string describe() const;

 private:
  Memory& mem_;
  Config cfg_;
  std::optional<uint32_t> tohost_;

  // ---- Architectural / microarchitectural state (the "flops") ----
  uint32_t pc_;                     // owned by IF
  bool fetch_frozen_ = false;       // owned by IF; set by a fault flush (§6.6)
  std::array<uint32_t, 32> regs_{}; // written only by WB
  IfId if_id_;
  IdEx id_ex_;
  ExMem ex_mem_;
  MemWb mem_wb_;
  unsigned md_cycles_ = 0;          // owned by EX (§6.4)
  uint32_t md_a_ = 0, md_b_ = 0;    // owned by EX (§6.4)

  // ---- Bookkeeping (not microarchitectural) ----
  uint64_t cycle_ = 0;
  uint64_t retired_ = 0;
  HaltInfo halt_;
  StageView view_;
  Control last_control_;
};

}  // namespace rvx::pipe
