// Pipeline registers and per-cycle control signals (contract §4, §6.7).
#pragma once

#include <cstdint>
#include <string_view>

#include "common/decode.h"
#include "common/retire.h"

namespace rvx::pipe {

// Why a pipeline register holds a bubble. Bubbles keep their cause as they
// move down the pipe, so when a bubble reaches WB we know which hazard cost
// that cycle (used for the Phase 3 CPI stack).
enum class BubbleCause : uint8_t {
  None,        // not a bubble: the register holds a real instruction
  Fill,        // pipeline not yet filled after reset
  LoadUse,     // §6.2
  Redirect,    // §6.3: flushed by a branch/jump misprediction resolved in EX
  MulDiv,      // §6.4: EX busy with a multi-cycle MUL/DIV
  FaultFlush,  // §6.6: flushed or fetch frozen behind a faulting instruction
  // Phase 3 (docs/phase3-design.md)
  IdRedirect,  // §2.2: fetch redirected from ID (predicted-taken branch or JAL)
  FenceI,      // FENCE.I refetch (redirect from EX)
  ICache,      // §3.3: IF waiting for an I-cache fill
  DCache,      // §3.3: MEM waiting for a D-cache fill
};

std::string_view to_string(BubbleCause c);

struct IfId {
  bool valid = false;
  BubbleCause cause = BubbleCause::Fill;
  uint32_t pc = 0;
  uint32_t insn = 0;
  // Front-end prediction made in IF (docs/phase3-design.md §2.2).
  uint32_t pred_npc = 0;      // pc fetched after this one
  uint32_t bp_index = 0;      // direction-predictor entry that was read
  bool dir_taken = false;     // direction predictor said taken
  bool pred_taken = false;    // IF predicted taken (followed a BTB target)
};

struct IdEx {
  bool valid = false;
  BubbleCause cause = BubbleCause::Fill;
  uint32_t pc = 0;
  uint32_t insn = 0;
  DecodedInsn d;
  // Prediction checked in EX: mispredict iff the taken bit differs, or both
  // say taken and the targets differ (never by comparing next-pc values;
  // docs/BUGS.md #4).
  bool pred_taken = false;
  uint32_t pred_npc = 0;  // predicted target if pred_taken, else pc + 4
  uint32_t bp_index = 0;
  uint32_t rs1_val = 0;  // as read in ID; EX forwarding may override
  uint32_t rs2_val = 0;
  HaltReason exc = HaltReason::None;  // fault detected in ID (illegal, ecall, ebreak)
};

struct ExMem {
  bool valid = false;
  BubbleCause cause = BubbleCause::Fill;
  uint32_t pc = 0;
  uint32_t insn = 0;
  Op op = Op::ILLEGAL;
  uint8_t rd = 0;          // 0 if the instruction writes no register
  uint32_t result = 0;     // ALU / link / MUL-DIV result; for loads unused
  uint32_t store_data = 0; // forwarded rs2, already masked to the access size
  MemKind mem = MemKind::None;
  uint32_t mem_addr = 0;
  uint8_t mem_size = 0;
  HaltReason exc = HaltReason::None;
};

struct MemWb {
  bool valid = false;
  BubbleCause cause = BubbleCause::Fill;
  uint32_t pc = 0;
  uint32_t insn = 0;
  uint8_t rd = 0;
  uint32_t value = 0;     // final value for rd (load data already extended)
  MemKind mem = MemKind::None;
  uint32_t mem_addr = 0;
  uint32_t mem_data = 0;  // load: raw bytes read; store: data written
  uint8_t mem_size = 0;
  HaltReason exc = HaltReason::None;
};

// The four mutually exclusive control events of §6.7, plus forwarding
// selects, as computed in one cycle. Kept for divergence reports.
enum class Fwd : uint8_t { None, FromExMem, FromMemWb };

struct Control {
  bool redirect = false;
  uint32_t redirect_pc = 0;
  bool fault_flush = false;
  bool ex_busy = false;
  bool load_use = false;
  // Phase 3
  bool mem_stall = false;    // MEM waiting on a D-cache miss (highest priority)
  bool id_redirect = false;  // ID redirected fetch (lowest priority)
  uint32_t id_redirect_pc = 0;
  BubbleCause redirect_cause = BubbleCause::Redirect;  // Redirect or FenceI
  Fwd fwd_rs1 = Fwd::None;
  Fwd fwd_rs2 = Fwd::None;
};

}  // namespace rvx::pipe
