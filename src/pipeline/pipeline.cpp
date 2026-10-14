#include "pipeline/pipeline.h"

#include <cstdio>
#include <stdexcept>

#include "pipeline/exec.h"

namespace rvx::pipe {

std::string_view to_string(BubbleCause c) {
  switch (c) {
    case BubbleCause::None: return "none";
    case BubbleCause::Fill: return "fill";
    case BubbleCause::LoadUse: return "load-use";
    case BubbleCause::Redirect: return "redirect";
    case BubbleCause::MulDiv: return "muldiv";
    case BubbleCause::FaultFlush: return "fault-flush";
  }
  return "?";
}

Pipeline::Pipeline(Memory& mem, uint32_t entry_pc, Config cfg)
    : mem_(mem), cfg_(cfg), pc_(entry_pc) {
  if (cfg_.mul_latency < 1 || cfg_.div_latency < 1) {
    throw std::invalid_argument("mul/div latency must be >= 1");
  }
}

namespace {

template <typename Latch>
Latch bubble(BubbleCause cause) {
  Latch l;
  l.valid = false;
  l.cause = cause;
  return l;
}

HaltReason fault_from_decode(Op op) {
  switch (op) {
    case Op::ILLEGAL: return HaltReason::IllegalInsn;
    case Op::ECALL: return HaltReason::Ecall;
    case Op::EBREAK: return HaltReason::Ebreak;
    default: return HaltReason::None;
  }
}

}  // namespace

std::optional<RetireRecord> Pipeline::tick() {
  if (halted()) return std::nullopt;
  ++cycle_;
  Control c;

  // =========================================================================
  // WB (reads MEM/WB). Retirement point (§5). Its register write is visible
  // to ID's register read in this same cycle (§3.1).
  // =========================================================================
  std::optional<RetireRecord> retired;
  bool wb_write = false;
  uint8_t wb_rd = 0;
  uint32_t wb_val = 0;
  std::optional<HaltInfo> halting;

  if (mem_wb_.valid) {
    if (mem_wb_.exc != HaltReason::None) {
      halting = HaltInfo{mem_wb_.exc, mem_wb_.pc, 0};  // faults never retire (§6.6)
    } else {
      RetireRecord r;
      r.seq = retired_;
      r.pc = mem_wb_.pc;
      r.insn = mem_wb_.insn;
      if (mem_wb_.rd != 0) {
        wb_write = true;
        wb_rd = mem_wb_.rd;
        wb_val = mem_wb_.value;
        r.rd = wb_rd;
        r.rd_val = wb_val;
      }
      r.mem = mem_wb_.mem;
      if (r.mem != MemKind::None) {
        r.mem_addr = mem_wb_.mem_addr;
        r.mem_data = mem_wb_.mem_data;
        r.mem_size = mem_wb_.mem_size;
      }
      retired = r;
      if (r.mem == MemKind::Write && tohost_ && r.mem_addr == *tohost_) {
        halting = HaltInfo{HaltReason::ToHost, mem_wb_.pc + 4, r.mem_data};  // §7
      }
    }
  }

  auto read_reg = [&](uint8_t r) -> uint32_t {
    if (r == 0) return 0;
    if (wb_write && wb_rd == r) return wb_val;  // write-through
    return regs_[r];
  };

  // =========================================================================
  // MEM (reads EX/MEM) -> next MEM/WB. Loads read now; a store's write is
  // applied at the end of the cycle (§3.0).
  // =========================================================================
  MemWb n_mem_wb = bubble<MemWb>(ex_mem_.cause);
  bool store_now = false;
  if (ex_mem_.valid) {
    n_mem_wb.valid = true;
    n_mem_wb.cause = BubbleCause::None;
    n_mem_wb.pc = ex_mem_.pc;
    n_mem_wb.insn = ex_mem_.insn;
    n_mem_wb.rd = ex_mem_.rd;
    n_mem_wb.exc = ex_mem_.exc;
    n_mem_wb.mem = ex_mem_.mem;
    n_mem_wb.mem_addr = ex_mem_.mem_addr;
    n_mem_wb.mem_size = ex_mem_.mem_size;
    if (ex_mem_.mem == MemKind::Read) {
      const uint32_t raw = mem_.load(ex_mem_.mem_addr, ex_mem_.mem_size);
      n_mem_wb.mem_data = raw;
      n_mem_wb.value = exec::extend_load(ex_mem_.op, raw);
    } else if (ex_mem_.mem == MemKind::Write) {
      n_mem_wb.mem_data = ex_mem_.store_data;
      store_now = true;
    } else {
      n_mem_wb.value = ex_mem_.result;
    }
  }

  // =========================================================================
  // EX (reads ID/EX; forwarding from EX/MEM and MEM/WB, §6.1).
  // =========================================================================
  auto forward = [&](uint8_t rs, uint32_t latched, Fwd& sel) -> uint32_t {
    sel = Fwd::None;
    if (rs == 0) return 0;
    if (ex_mem_.valid && ex_mem_.exc == HaltReason::None && ex_mem_.rd == rs) {
      // The load-use stall guarantees a load's consumer is never in EX while
      // the load is in EX/MEM (its data does not exist yet).
      if (ex_mem_.mem == MemKind::Read) {
        throw std::logic_error("contract violation: load result needed from EX/MEM");
      }
      sel = Fwd::FromExMem;
      return ex_mem_.result;
    }
    if (mem_wb_.valid && mem_wb_.exc == HaltReason::None && mem_wb_.rd == rs) {
      sel = Fwd::FromMemWb;
      return mem_wb_.value;
    }
    return latched;
  };

  ExMem n_ex_mem = bubble<ExMem>(id_ex_.cause);
  unsigned n_md_cycles = md_cycles_;
  uint32_t n_md_a = md_a_, n_md_b = md_b_;
  bool ex_fault = false;

  if (id_ex_.valid) {
    const DecodedInsn& d = id_ex_.d;
    const uint32_t imm = static_cast<uint32_t>(d.imm);
    const uint32_t a = forward(d.rs1, id_ex_.rs1_val, c.fwd_rs1);
    const uint32_t b = forward(d.rs2, id_ex_.rs2_val, c.fwd_rs2);

    ExMem o;
    o.valid = true;
    o.cause = BubbleCause::None;
    o.pc = id_ex_.pc;
    o.insn = id_ex_.insn;
    o.op = d.op;
    o.rd = d.rd;  // decode forces rd = 0 for formats that do not write it
    o.exc = id_ex_.exc;

    auto fault = [&](HaltReason why) {
      o.exc = why;
      o.rd = 0;
      o.mem = MemKind::None;
      ex_fault = true;
    };
    auto jump_to = [&](uint32_t target) {
      if (target & 3u) {
        fault(HaltReason::MisalignedFetch);  // §6.6: fault on the jump, no redirect
      } else {
        c.redirect = true;
        c.redirect_pc = target;
      }
    };

    if (o.exc != HaltReason::None) {
      fault(o.exc);  // detected in ID; acted on now
    } else if (exec::is_mul(d.op) || exec::is_div(d.op)) {
      // §6.4: capture operands on the first EX cycle, use them every cycle.
      const bool first = md_cycles_ == 0;
      const uint32_t ma = first ? a : md_a_;
      const uint32_t mb = first ? b : md_b_;
      const unsigned latency = exec::is_mul(d.op) ? cfg_.mul_latency : cfg_.div_latency;
      const unsigned done = md_cycles_ + 1;
      if (done < latency) {
        c.ex_busy = true;
        n_md_cycles = done;
        n_md_a = ma;
        n_md_b = mb;
      } else {
        o.result = exec::muldiv(d.op, ma, mb);
        n_md_cycles = 0;
      }
    } else if (exec::is_load(d.op) || exec::is_store(d.op)) {
      const unsigned size = exec::access_size(d.op);
      const uint32_t addr = a + imm;
      if (addr % size != 0) {
        fault(exec::is_load(d.op) ? HaltReason::MisalignedLoad : HaltReason::MisalignedStore);
      } else {
        o.mem = exec::is_load(d.op) ? MemKind::Read : MemKind::Write;
        o.mem_addr = addr;
        o.mem_size = static_cast<uint8_t>(size);
        if (o.mem == MemKind::Write) o.store_data = b & exec::store_mask(size);
      }
    } else if (exec::is_branch(d.op)) {
      if (exec::branch_taken(d.op, a, b)) jump_to(id_ex_.pc + imm);
    } else {
      switch (d.op) {
        case Op::JAL:
          o.result = id_ex_.pc + 4;
          jump_to(id_ex_.pc + imm);
          break;
        case Op::JALR:
          o.result = id_ex_.pc + 4;
          jump_to((a + imm) & ~1u);
          break;
        case Op::FENCE_I:
          c.redirect = true;  // §6.5: refetch everything younger
          c.redirect_pc = id_ex_.pc + 4;
          break;
        case Op::FENCE: break;
        case Op::LUI: o.result = imm; break;
        case Op::AUIPC: o.result = id_ex_.pc + imm; break;
        default:
          // R-type uses rs2; I-type ALU ops use the immediate.
          o.result = exec::alu(d.op, a, d.fmt == Format::I ? imm : b);
          break;
      }
    }
    if (ex_fault) c.redirect = false;
    if (!c.ex_busy) n_ex_mem = o;
    c.fault_flush = ex_fault;
  }
  if (c.ex_busy) n_ex_mem = bubble<ExMem>(BubbleCause::MulDiv);

  // =========================================================================
  // ID (reads IF/ID) -> next ID/EX; hazard detection (§6.2).
  // =========================================================================
  IdEx n_id_ex = bubble<IdEx>(if_id_.cause);
  if (if_id_.valid) {
    const DecodedInsn d = decode(if_id_.insn);
    n_id_ex.valid = true;
    n_id_ex.cause = BubbleCause::None;
    n_id_ex.pc = if_id_.pc;
    n_id_ex.insn = if_id_.insn;
    n_id_ex.d = d;
    n_id_ex.rs1_val = read_reg(d.rs1);
    n_id_ex.rs2_val = read_reg(d.rs2);
    n_id_ex.exc = fault_from_decode(d.op);

    // Unused source fields are 0 after decode, and rd != 0 is required, so a
    // format that does not read a register can never match here.
    const bool ex_has_good_load = id_ex_.valid && id_ex_.exc == HaltReason::None && !ex_fault &&
                                  exec::is_load(id_ex_.d.op) && id_ex_.d.rd != 0;
    c.load_use = ex_has_good_load && (d.rs1 == id_ex_.d.rd || d.rs2 == id_ex_.d.rd);
  }

  // =========================================================================
  // IF: fetch at PC unless frozen by a fault (§6.6).
  // =========================================================================
  IfId n_if_id = bubble<IfId>(BubbleCause::FaultFlush);
  if (!fetch_frozen_) {
    n_if_id.valid = true;
    n_if_id.cause = BubbleCause::None;
    n_if_id.pc = pc_;
    n_if_id.insn = mem_.load(pc_, 4);
  }

  // =========================================================================
  // Control resolution (§6.7): at most one event per cycle.
  // =========================================================================
  const int events = int{c.redirect} + int{c.fault_flush} + int{c.ex_busy} + int{c.load_use};
  if (events > 1) throw std::logic_error("contract violation: more than one control event");

  uint32_t n_pc = fetch_frozen_ ? pc_ : pc_ + 4;
  bool n_frozen = fetch_frozen_;
  if (c.fault_flush) {
    n_if_id = bubble<IfId>(BubbleCause::FaultFlush);
    n_id_ex = bubble<IdEx>(BubbleCause::FaultFlush);
    n_frozen = true;
    n_pc = pc_;
  } else if (c.redirect) {
    n_if_id = bubble<IfId>(BubbleCause::Redirect);
    n_id_ex = bubble<IdEx>(BubbleCause::Redirect);
    n_pc = c.redirect_pc;
  } else if (c.ex_busy) {
    n_if_id = if_id_;
    n_id_ex = id_ex_;
    n_pc = pc_;
  } else if (c.load_use) {
    n_if_id = if_id_;
    n_id_ex = bubble<IdEx>(BubbleCause::LoadUse);
    n_pc = pc_;
  }

  // What each stage held during this cycle (before the clock edge).
  view_.if_pc = fetch_frozen_ ? std::nullopt : std::optional<uint32_t>(pc_);
  view_.id_pc = if_id_.valid ? std::optional<uint32_t>(if_id_.pc) : std::nullopt;
  view_.ex_pc = id_ex_.valid ? std::optional<uint32_t>(id_ex_.pc) : std::nullopt;
  view_.mem_pc = ex_mem_.valid ? std::optional<uint32_t>(ex_mem_.pc) : std::nullopt;
  view_.wb_pc = mem_wb_.valid ? std::optional<uint32_t>(mem_wb_.pc) : std::nullopt;
  view_.id_cause = if_id_.cause;
  view_.ex_cause = id_ex_.cause;
  view_.mem_cause = ex_mem_.cause;
  view_.wb_cause = mem_wb_.cause;
  last_control_ = c;

  // =========================================================================
  // Clock edge. In the halting cycle only WB's own effects commit: nothing
  // younger than the halting instruction may change state (§7).
  // =========================================================================
  if (wb_write) regs_[wb_rd] = wb_val;
  if (retired) ++retired_;
  if (halting) {
    halt_ = *halting;
    return retired;
  }
  if (store_now) mem_.store(ex_mem_.mem_addr, ex_mem_.mem_size, ex_mem_.store_data);
  pc_ = n_pc;
  fetch_frozen_ = n_frozen;
  if_id_ = n_if_id;
  id_ex_ = n_id_ex;
  ex_mem_ = n_ex_mem;
  mem_wb_ = n_mem_wb;
  md_cycles_ = n_md_cycles;
  md_a_ = n_md_a;
  md_b_ = n_md_b;
  return retired;
}

std::string Pipeline::describe() const {
  std::string s;
  char line[200];
  auto stage = [&](const char* name, bool valid, BubbleCause cause, uint32_t pc, uint32_t insn) {
    if (valid) {
      std::snprintf(line, sizeof line, "  %-6s pc=%08x insn=%08x\n", name, pc, insn);
    } else {
      std::snprintf(line, sizeof line, "  %-6s bubble (%s)\n", name,
                    std::string(to_string(cause)).c_str());
    }
    s += line;
  };
  std::snprintf(line, sizeof line, "pipeline state after cycle %llu (registers feeding the next cycle):\n",
                static_cast<unsigned long long>(cycle_));
  s += line;
  std::snprintf(line, sizeof line, "  PC     %08x%s\n", pc_, fetch_frozen_ ? " (fetch frozen)" : "");
  s += line;
  stage("IF/ID", if_id_.valid, if_id_.cause, if_id_.pc, if_id_.insn);
  stage("ID/EX", id_ex_.valid, id_ex_.cause, id_ex_.pc, id_ex_.insn);
  stage("EX/MEM", ex_mem_.valid, ex_mem_.cause, ex_mem_.pc, ex_mem_.insn);
  stage("MEM/WB", mem_wb_.valid, mem_wb_.cause, mem_wb_.pc, mem_wb_.insn);
  auto fwd = [](Fwd f) { return f == Fwd::FromExMem ? "EX/MEM" : f == Fwd::FromMemWb ? "MEM/WB" : "-"; };
  std::snprintf(line, sizeof line,
                "  last cycle control: redirect=%d (to %08x) fault_flush=%d ex_busy=%d (md_cycles=%u) "
                "load_use=%d fwd_rs1=%s fwd_rs2=%s\n",
                last_control_.redirect, last_control_.redirect_pc, last_control_.fault_flush,
                last_control_.ex_busy,
                md_cycles_, last_control_.load_use, fwd(last_control_.fwd_rs1),
                fwd(last_control_.fwd_rs2));
  s += line;
  return s;
}

}  // namespace rvx::pipe
