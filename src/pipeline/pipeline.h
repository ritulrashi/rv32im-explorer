// Cycle-accurate 5-stage in-order RV32IM pipeline (IF ID EX MEM WB).
//
// Implements docs/phase2-pipeline-contract.md ("P2 §n") extended by
// docs/phase3-design.md ("P3 §n"): branch prediction, L1 caches, DRAM, and
// per-cycle CPI attribution. With the default Config (static not-taken, no
// BTB, caches off) the timing is exactly Phase 2's.
//
// Modeling style: every pipeline register is a flop. tick() computes all
// control signals and next-state values from the CURRENT registers only,
// then commits them together, like a clock edge.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

#include "bpred/predictor.h"
#include "cache/memsys.h"
#include "common/memory.h"
#include "common/retire.h"
#include "pipeline/config.h"
#include "pipeline/latches.h"
#include "pipeline/stats.h"

namespace rvx::pipe {

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
  // `cfg` must be valid (see validate()); the constructor re-checks it.
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
  const Config& config() const { return cfg_; }
  const PipeStats& stats() const { return stats_; }
  const cache::MemSystem& mem_system() const { return memsys_; }

  // Stage occupancy during the most recent tick() (the cycle just simulated).
  const StageView& last_view() const { return view_; }

  // Multi-line human-readable dump of all pipeline registers and the control
  // signals of the most recent cycle (used in divergence reports).
  std::string describe() const;

 private:
  void account(const MemWb& wb);  // P3 §5: charge this cycle to one category

  Memory& mem_;
  Config cfg_;
  std::optional<uint32_t> tohost_;

  // ---- Timing-only structures (P3 §0.2): never affect values ----
  cache::MemSystem memsys_;
  bpred::DirectionPredictor dir_;
  bpred::Btb btb_;

  // ---- Architectural / microarchitectural state (the "flops") ----
  uint32_t pc_;                     // owned by IF: address being (or to be) fetched
  bool fetch_frozen_ = false;       // owned by IF; set by a fault flush (P2 §6.6)
  unsigned if_wait_ = 0;            // owned by IF: I-miss bubble cycles still to come
  bool fetched_ = false;            // owned by IF: the cache access for pc_ is done
  std::optional<uint32_t> pending_redirect_;  // owned by IF: redirect held during a fill
  std::array<uint32_t, 32> regs_{}; // written only by WB
  IfId if_id_;
  IdEx id_ex_;
  ExMem ex_mem_;
  MemWb mem_wb_;
  unsigned md_cycles_ = 0;          // owned by EX (P2 §6.4)
  uint32_t md_a_ = 0, md_b_ = 0;    // owned by EX (P2 §6.4)
  unsigned mem_wait_ = 0;           // owned by MEM: D-miss stall cycles still to come
  bool mem_accessed_ = false;       // owned by MEM: D-cache access for EX/MEM done

  // ---- Bookkeeping (not microarchitectural) ----
  uint64_t cycle_ = 0;
  uint64_t retired_ = 0;
  HaltInfo halt_;
  StageView view_;
  Control last_control_;
  PipeStats stats_;
};

}  // namespace rvx::pipe
