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
    case BubbleCause::Redirect: return "mispredict";
    case BubbleCause::MulDiv: return "muldiv";
    case BubbleCause::FaultFlush: return "fault-flush";
    case BubbleCause::IdRedirect: return "id-redirect";
    case BubbleCause::FenceI: return "fence.i";
    case BubbleCause::ICache: return "icache";
    case BubbleCause::DCache: return "dcache";
  }
  return "?";
}

std::string_view to_string(CpiCat c) {
  switch (c) {
    case CpiCat::Base: return "base";
    case CpiCat::LoadUse: return "load_use";
    case CpiCat::Branch: return "branch";
    case CpiCat::ICache: return "icache";
    case CpiCat::DCache: return "dcache";
    case CpiCat::MulDiv: return "muldiv";
  }
  return "?";
}

namespace {
Config validated(Config c) {
  validate(c);
  return c;
}
}  // namespace

Pipeline::Pipeline(Memory& mem, uint32_t entry_pc, Config cfg)
    : mem_(mem),
      cfg_(validated(cfg)),
      memsys_(cfg_.icache, cfg_.dcache, cfg_.dram),
      dir_(cfg_.bpred),
      btb_(cfg_.bpred.btb_entries),
      pc_(entry_pc) {}

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

// P3 §5: retire-slot accounting. Whatever occupies WB this cycle owns it.
void Pipeline::account(const MemWb& wb) {
  CpiCat cat = CpiCat::Base;  // retiring (or halting) instruction, or pipeline fill
  if (!wb.valid) {
    switch (wb.cause) {
      case BubbleCause::LoadUse: cat = CpiCat::LoadUse; break;
      case BubbleCause::Redirect:
        cat = CpiCat::Branch;
        ++stats_.branch_mispredict_cycles;
        break;
      case BubbleCause::IdRedirect:
        cat = CpiCat::Branch;
        ++stats_.branch_id_redirect_cycles;
        break;
      case BubbleCause::FenceI:
        cat = CpiCat::Branch;
        ++stats_.branch_fence_i_cycles;
        break;
      case BubbleCause::MulDiv: cat = CpiCat::MulDiv; break;
      case BubbleCause::ICache: cat = CpiCat::ICache; break;
      case BubbleCause::DCache: cat = CpiCat::DCache; break;
      case BubbleCause::None:
      case BubbleCause::Fill:
      case BubbleCause::FaultFlush: break;
    }
  }
  ++stats_.cpi[static_cast<size_t>(cat)];
}

std::optional<RetireRecord> Pipeline::tick() {
  if (halted()) return std::nullopt;
  ++cycle_;
  Control c;

  // =========================================================================
  // WB (reads MEM/WB). Retirement point (P2 §5). Its register write is
  // visible to ID's register read in this same cycle (P2 §3.1).
  // =========================================================================
  account(mem_wb_);
  std::optional<RetireRecord> retired;
  bool wb_write = false;
  uint8_t wb_rd = 0;
  uint32_t wb_val = 0;
  std::optional<HaltInfo> halting;

  if (mem_wb_.valid) {
    if (mem_wb_.exc != HaltReason::None) {
      halting = HaltInfo{mem_wb_.exc, mem_wb_.pc, 0};  // faults never retire (P2 §6.6)
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
        halting = HaltInfo{HaltReason::ToHost, mem_wb_.pc + 4, r.mem_data};  // P2 §7
      }
    }
  }

  auto read_reg = [&](uint8_t r) -> uint32_t {
    if (r == 0) return 0;
    if (wb_write && wb_rd == r) return wb_val;  // write-through
    return regs_[r];
  };

  // =========================================================================
  // MEM (reads EX/MEM) -> next MEM/WB. D-cache lookup on the first cycle an
  // access is in MEM; a miss stalls MEM for P cycles (P3 §3.3). Data values
  // always come from the backing store; a store's write is applied at the end
  // of the cycle in which it leaves MEM (P2 §3.0).
  // =========================================================================
  unsigned n_mem_wait = mem_wait_;
  bool n_mem_accessed = mem_accessed_;
  if (ex_mem_.valid && ex_mem_.mem != MemKind::None) {
    if (!mem_accessed_) {
      const unsigned p = memsys_.data(ex_mem_.mem_addr, ex_mem_.mem == MemKind::Write);
      if (p > 0) {
        c.mem_stall = true;
        n_mem_wait = p - 1;
        n_mem_accessed = true;
      }
    } else if (mem_wait_ > 0) {
      c.mem_stall = true;
      n_mem_wait = mem_wait_ - 1;
    }
  }

  MemWb n_mem_wb = bubble<MemWb>(c.mem_stall ? BubbleCause::DCache : ex_mem_.cause);
  bool store_now = false;
  if (!c.mem_stall) {
    n_mem_accessed = false;  // whatever is in EX/MEM leaves MEM this cycle
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
  }

  // =========================================================================
  // EX (reads ID/EX; forwarding from EX/MEM and MEM/WB, P2 §6.1). Resolves
  // the actual next pc and compares it with the predicted one (P3 §2.2).
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
  uint32_t ex_a = 0, ex_b = 0;  // forwarded operands, latched back on an EX hold

  // Predictor / BTB training, applied at the clock edge if the instruction
  // leaves EX this cycle.
  struct Train {
    bool cond = false;
    bool jump = false;
    bpred::BranchKind kind = bpred::BranchKind::Cond;
    bool taken = false;
    uint32_t target = 0;
    bool mispredict = false;
    bool fence_i = false;
  } tr;

  if (id_ex_.valid) {
    const DecodedInsn& d = id_ex_.d;
    const uint32_t imm = static_cast<uint32_t>(d.imm);
    ex_a = forward(d.rs1, id_ex_.rs1_val, c.fwd_rs1);
    ex_b = forward(d.rs2, id_ex_.rs2_val, c.fwd_rs2);
    const uint32_t a = ex_a, b = ex_b;

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
    uint32_t actual_npc = id_ex_.pc + 4;

    if (o.exc != HaltReason::None) {
      fault(o.exc);  // detected in ID; acted on now
    } else if (exec::is_mul(d.op) || exec::is_div(d.op)) {
      // P2 §6.4: capture operands on the first EX cycle. The counter keeps
      // running during a D-miss stall (P3 §3.3); a finished operation waits
      // in EX (counter saturated) until EX can advance.
      const bool first = md_cycles_ == 0;
      const uint32_t ma = first ? a : md_a_;
      const uint32_t mb = first ? b : md_b_;
      const unsigned latency = exec::is_mul(d.op) ? cfg_.mul_latency : cfg_.div_latency;
      const unsigned done = md_cycles_ + 1;
      n_md_a = ma;
      n_md_b = mb;
      if (done < latency) {
        c.ex_busy = true;
        n_md_cycles = done;
      } else {
        o.result = exec::muldiv(d.op, ma, mb);
        n_md_cycles = c.mem_stall ? latency - 1 : 0;
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
      tr.cond = true;
      tr.kind = bpred::BranchKind::Cond;
      tr.taken = exec::branch_taken(d.op, a, b);
      tr.target = id_ex_.pc + imm;
      if (tr.taken) {
        if (tr.target & 3u) fault(HaltReason::MisalignedFetch);  // P2 §6.6
        actual_npc = tr.target;
      }
    } else {
      switch (d.op) {
        case Op::JAL:
        case Op::JALR:
          tr.jump = true;
          tr.kind = d.op == Op::JAL ? bpred::BranchKind::Jal : bpred::BranchKind::Jalr;
          tr.taken = true;
          tr.target = d.op == Op::JAL ? id_ex_.pc + imm : (a + imm) & ~1u;
          o.result = id_ex_.pc + 4;
          if (tr.target & 3u) fault(HaltReason::MisalignedFetch);
          actual_npc = tr.target;
          break;
        case Op::FENCE_I: tr.fence_i = true; break;
        case Op::FENCE: break;
        case Op::LUI: o.result = imm; break;
        case Op::AUIPC: o.result = id_ex_.pc + imm; break;
        default:
          // R-type uses rs2; I-type ALU ops use the immediate.
          o.result = exec::alu(d.op, a, d.fmt == Format::I ? imm : b);
          break;
      }
    }

    if (!ex_fault && !c.ex_busy) {
      if (tr.fence_i) {
        c.redirect = true;  // P2 §6.5: refetch everything younger
        c.redirect_pc = id_ex_.pc + 4;
        c.redirect_cause = BubbleCause::FenceI;
      } else if (tr.taken != id_ex_.pred_taken || (tr.taken && actual_npc != id_ex_.pred_npc)) {
        // Misprediction (P3 §2.2): the taken bit differs, or both say taken
        // and the targets differ. A taken jump to pc + 4 that was predicted
        // not-taken is still a misprediction, exactly as in Phase 2, where
        // every taken branch and jump flushes (docs/BUGS.md #4).
        c.redirect = true;
        c.redirect_pc = actual_npc;
        c.redirect_cause = BubbleCause::Redirect;
        tr.mispredict = true;
      }
    }
    if (!c.ex_busy) n_ex_mem = o;
    c.fault_flush = ex_fault;
  }
  if (c.ex_busy) n_ex_mem = bubble<ExMem>(BubbleCause::MulDiv);

  // =========================================================================
  // ID (reads IF/ID) -> next ID/EX; load-use detection (P2 §6.2) and the
  // decode-stage redirect (P3 §2.2).
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
    n_id_ex.bp_index = if_id_.bp_index;
    n_id_ex.pred_taken = if_id_.pred_taken;
    n_id_ex.pred_npc = if_id_.pred_npc;

    // Unused source fields are 0 after decode, and rd != 0 is required, so a
    // format that does not read a register can never match here.
    const bool ex_has_good_load = id_ex_.valid && id_ex_.exc == HaltReason::None && !ex_fault &&
                                  exec::is_load(id_ex_.d.op) && id_ex_.d.rd != 0;
    c.load_use = ex_has_good_load && (d.rs1 == id_ex_.d.rd || d.rs2 == id_ex_.d.rd);

    if (n_id_ex.exc == HaltReason::None) {
      const uint32_t seq = if_id_.pc + 4;
      const uint32_t target = if_id_.pc + static_cast<uint32_t>(d.imm);
      // static_nt is the Phase 2 front end: no decode-stage redirects of its
      // own. BTB-made predictions are still checked against the decode.
      const bool dynamic = cfg_.bpred.type != bpred::Type::StaticNT;
      // The prediction ID wants, as (taken, target). Compared with IF's by the
      // same rule EX uses: taken bit, then target (docs/BUGS.md #4).
      bool want_taken = if_id_.pred_taken;
      uint32_t want_npc = if_id_.pred_npc;
      if (d.op == Op::JAL) {
        if (dynamic || if_id_.pred_taken) {
          want_taken = true;
          want_npc = target;
        }
      } else if (exec::is_branch(d.op)) {
        if (if_id_.pred_taken || (dynamic && if_id_.dir_taken)) {
          want_taken = true;  // keep a BTB prediction (fixing a stale target) or take the direction one
          want_npc = target;
        }
      } else if (d.op != Op::JALR) {
        want_taken = false;  // not a control instruction: undo any BTB prediction
        want_npc = seq;
      }
      if (want_taken != if_id_.pred_taken || (want_taken && want_npc != if_id_.pred_npc)) {
        c.id_redirect = true;
        c.id_redirect_pc = want_npc;
      }
      n_id_ex.pred_taken = want_taken;
      n_id_ex.pred_npc = want_npc;
    }
  }

  // =========================================================================
  // IF: I-cache lookup, prediction, fetch (P3 §2.2, §3.3). An I-miss fill
  // cannot be cancelled; a redirect that arrives during it is held.
  // =========================================================================
  uint32_t fetch_pc = pc_;
  bool f_fetched = fetched_;
  unsigned n_if_wait = if_wait_;
  std::optional<uint32_t> n_pending = pending_redirect_;
  IfId if_out = bubble<IfId>(BubbleCause::FaultFlush);
  bool if_btb_hit = false;
  if (!fetch_frozen_) {
    if (if_wait_ > 0) {
      if_out = bubble<IfId>(BubbleCause::ICache);
      n_if_wait = if_wait_ - 1;
    } else {
      if (pending_redirect_) {  // the fill finished: now follow the held redirect
        fetch_pc = *pending_redirect_;
        n_pending.reset();
        f_fetched = false;
      }
      bool ready = f_fetched;
      if (!ready) {
        const unsigned p = memsys_.fetch(fetch_pc);
        f_fetched = true;
        if (p > 0) {
          if_out = bubble<IfId>(BubbleCause::ICache);
          n_if_wait = p - 1;
        } else {
          ready = true;
        }
      }
      if (ready) {
        if_out.valid = true;
        if_out.cause = BubbleCause::None;
        if_out.pc = fetch_pc;
        if_out.insn = mem_.load(fetch_pc, 4);
        if_out.bp_index = dir_.index(fetch_pc);
        if_out.dir_taken = dir_.predict_at(if_out.bp_index);
        if_out.pred_npc = fetch_pc + 4;
        if (auto e = btb_.lookup(fetch_pc)) {
          if_btb_hit = true;
          if (e->kind != bpred::BranchKind::Cond || if_out.dir_taken) {
            if_out.pred_npc = e->target;
            if_out.pred_taken = true;
          }
        }
      }
    }
  }

  // =========================================================================
  // Control resolution (P3 §3.4): one event takes effect, by priority.
  // =========================================================================
  const int events = int{c.redirect} + int{c.fault_flush} + int{c.ex_busy} + int{c.load_use};
  if (events > 1) throw std::logic_error("contract violation: more than one EX control event");

  uint32_t n_pc = fetch_pc;
  bool n_fetched = f_fetched;
  bool n_frozen = fetch_frozen_;
  IfId n_if_id = if_out;
  bool id_redirect_taken = false;
  bool if_delivered = false;

  auto hold_ex = [&]() {  // P3 §3.4 clarification: latch the forwarded operands
    IdEx h = id_ex_;
    if (h.valid) {
      h.rs1_val = ex_a;
      h.rs2_val = ex_b;
    }
    return h;
  };
  auto redirect_fetch = [&](uint32_t target) {
    if (n_if_wait > 0) {
      n_pending = target;  // fill in progress: follow the redirect when it ends
    } else {
      n_pc = target;
      n_fetched = false;
      n_pending.reset();
    }
  };

  if (c.mem_stall) {
    n_if_id = if_id_;
    n_id_ex = hold_ex();
    n_ex_mem = ex_mem_;
  } else if (c.fault_flush) {
    n_if_id = bubble<IfId>(BubbleCause::FaultFlush);
    n_id_ex = bubble<IdEx>(BubbleCause::FaultFlush);
    n_frozen = true;
  } else if (c.redirect) {
    n_if_id = bubble<IfId>(c.redirect_cause);
    n_id_ex = bubble<IdEx>(c.redirect_cause);
    redirect_fetch(c.redirect_pc);
  } else if (c.ex_busy) {
    n_if_id = if_id_;
    n_id_ex = hold_ex();
  } else if (c.load_use) {
    n_if_id = if_id_;
    n_id_ex = bubble<IdEx>(BubbleCause::LoadUse);
  } else if (c.id_redirect) {
    n_if_id = bubble<IfId>(BubbleCause::IdRedirect);
    redirect_fetch(c.id_redirect_pc);
    id_redirect_taken = true;
  } else if (if_out.valid) {
    n_pc = if_out.pred_npc;
    n_fetched = false;
    if_delivered = true;
  }

  // What each stage held during this cycle (before the clock edge).
  view_.if_pc = fetch_frozen_ ? std::nullopt : std::optional<uint32_t>(fetch_pc);
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
  // younger than the halting instruction may change state (P2 §7).
  // =========================================================================
  if (wb_write) regs_[wb_rd] = wb_val;
  if (retired) ++retired_;
  if (halting) {
    if (stats_.cpi_total() != cycle_) {
      throw std::logic_error("CPI stack does not sum to total cycles");
    }
    halt_ = *halting;
    return retired;
  }
  if (store_now) mem_.store(ex_mem_.mem_addr, ex_mem_.mem_size, ex_mem_.store_data);

  // Training when the instruction leaves EX (on the correct path by
  // construction: anything in EX is older than every pending redirect).
  if (id_ex_.valid && !c.mem_stall && !c.ex_busy && !ex_fault) {
    if (tr.cond) {
      dir_.train(id_ex_.bp_index, tr.taken);
      if (tr.taken) btb_.update(id_ex_.pc, tr.target, bpred::BranchKind::Cond);
      ++stats_.cond_branches;
      if (tr.mispredict) ++stats_.cond_mispredicts;
    }
    if (tr.jump) {
      btb_.update(id_ex_.pc, tr.target, tr.kind);
      ++stats_.jumps;
      if (tr.mispredict) ++stats_.jump_mispredicts;
    }
    if (tr.fence_i) memsys_.invalidate_icache();
  }
  if (id_redirect_taken) ++stats_.id_redirects;
  if (if_delivered && if_btb_hit) ++stats_.btb_hits;

  pc_ = n_pc;
  fetched_ = n_fetched;
  if_wait_ = n_if_wait;
  pending_redirect_ = n_pending;
  fetch_frozen_ = n_frozen;
  if_id_ = n_if_id;
  id_ex_ = n_id_ex;
  ex_mem_ = n_ex_mem;
  mem_wb_ = n_mem_wb;
  md_cycles_ = n_md_cycles;
  md_a_ = n_md_a;
  md_b_ = n_md_b;
  mem_wait_ = n_mem_wait;
  mem_accessed_ = n_mem_accessed;
  return retired;
}

std::string Pipeline::describe() const {
  std::string s;
  char line[240];
  auto stage = [&](const char* name, bool valid, BubbleCause cause, uint32_t pc, uint32_t insn) {
    if (valid) {
      std::snprintf(line, sizeof line, "  %-6s pc=%08x insn=%08x\n", name, pc, insn);
    } else {
      std::snprintf(line, sizeof line, "  %-6s bubble (%s)\n", name, std::string(to_string(cause)).c_str());
    }
    s += line;
  };
  std::snprintf(line, sizeof line, "pipeline state after cycle %llu (registers feeding the next cycle):\n",
                static_cast<unsigned long long>(cycle_));
  s += line;
  std::snprintf(line, sizeof line, "  PC     %08x%s if_wait=%u fetched=%d pending_redirect=%s\n", pc_,
                fetch_frozen_ ? " (fetch frozen)" : "", if_wait_, fetched_,
                pending_redirect_ ? std::to_string(*pending_redirect_).c_str() : "-");
  s += line;
  stage("IF/ID", if_id_.valid, if_id_.cause, if_id_.pc, if_id_.insn);
  stage("ID/EX", id_ex_.valid, id_ex_.cause, id_ex_.pc, id_ex_.insn);
  stage("EX/MEM", ex_mem_.valid, ex_mem_.cause, ex_mem_.pc, ex_mem_.insn);
  stage("MEM/WB", mem_wb_.valid, mem_wb_.cause, mem_wb_.pc, mem_wb_.insn);
  auto fwd = [](Fwd f) { return f == Fwd::FromExMem ? "EX/MEM" : f == Fwd::FromMemWb ? "MEM/WB" : "-"; };
  const Control& c = last_control_;
  std::snprintf(line, sizeof line,
                "  last cycle control: mem_stall=%d (mem_wait=%u) redirect=%d (to %08x, %s) fault_flush=%d "
                "ex_busy=%d (md_cycles=%u) load_use=%d id_redirect=%d (to %08x) fwd_rs1=%s fwd_rs2=%s\n",
                c.mem_stall, mem_wait_, c.redirect, c.redirect_pc,
                std::string(to_string(c.redirect_cause)).c_str(), c.fault_flush, c.ex_busy, md_cycles_,
                c.load_use, c.id_redirect, c.id_redirect_pc, fwd(c.fwd_rs1), fwd(c.fwd_rs2));
  s += line;
  return s;
}

}  // namespace rvx::pipe
