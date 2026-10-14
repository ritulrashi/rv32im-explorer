// 5-stage in-order RV32I core implementing docs/phase2-pipeline-contract.md
// ("P2 §n") for the baseline machine: full forwarding into EX, 1-cycle
// load-use stall, branches/jumps/FENCE.I resolved in EX with a 2-bubble flush
// (static not-taken), faults acted on in EX and halting at WB.
//
// Structure mirrors the contract's clocking model (P2 §2): every control
// signal and next-state value is computed in always_comb from the CURRENT
// pipeline registers; every register updates in one always_ff.
//
// Memory (docs/phase5-design.md §4): reads are combinational inputs whose
// addresses come straight from flops (PC and EX/MEM), supplied by the
// testbench; the store write request is applied by the testbench at the
// clock edge (P2 §3.0).
module rvx_core
  import rvx_pkg::*;
(
    input  logic        clk,
    input  logic        rst,
    input  logic [31:0] entry_pc,
    input  logic        tohost_en,
    input  logic [31:0] tohost_addr,

    // Instruction fetch (IF): word at imem_addr.
    output logic [31:0] imem_addr,
    input  logic [31:0] imem_rdata,
    // Data access (MEM): dmem_size bytes at dmem_addr, zero-extended.
    output logic        dmem_re,
    output logic        dmem_we,
    output logic [31:0] dmem_addr,
    output logic [1:0]  dmem_size,   // 0 byte, 1 half, 2 word
    output logic [31:0] dmem_wdata,  // already masked to the size
    input  logic [31:0] dmem_rdata,

    // Retirement (WB), in the same form as the C++ RetireRecord.
    output logic        retire_valid,
    output logic [31:0] retire_pc,
    output logic [31:0] retire_insn,
    output logic [4:0]  retire_rd,
    output logic [31:0] retire_rd_val,
    output memk_e       retire_mem,
    output logic [31:0] retire_mem_addr,
    output logic [31:0] retire_mem_data,
    output logic [1:0]  retire_mem_size,

    // Halt (WB): the run ends in this cycle.
    output logic        halt_valid,
    output halt_e       halt_reason,
    output logic [31:0] halt_pc,
    output logic [31:0] halt_tohost_value,

    // Stage occupancy this cycle, for per-cycle correlation (phase5 §1).
    output logic [31:0] view_if_pc,
    output logic        view_if_frozen,
    output logic        view_id_valid,
    output logic [31:0] view_id_pc,
    output cause_e      view_id_cause,
    output logic        view_ex_valid,
    output logic [31:0] view_ex_pc,
    output cause_e      view_ex_cause,
    output logic        view_mem_valid,
    output logic [31:0] view_mem_pc,
    output cause_e      view_mem_cause,
    output logic        view_wb_valid,
    output logic [31:0] view_wb_pc,
    output cause_e      view_wb_cause
);

  // ---------------------------------------------------------------------------
  // State (the flops)
  // ---------------------------------------------------------------------------
  logic [31:0] pc_q;      // owned by IF
  logic        frozen_q;  // owned by IF: fetch frozen after a fault flush
  if_id_t      if_id_q;
  id_ex_t      id_ex_q;
  ex_mem_t     ex_mem_q;
  mem_wb_t     mem_wb_q;
  logic [31:0] rf_q[32];  // rf_q[0] is never written and never read

  // ---------------------------------------------------------------------------
  // WB (reads MEM/WB): retirement point, halt detection (P2 §5, §6.6, §7)
  // ---------------------------------------------------------------------------
  logic wb_retire, wb_we;
  logic halting;
  always_comb begin
    wb_retire = mem_wb_q.valid && mem_wb_q.exc == HALT_NONE;
    wb_we = wb_retire && mem_wb_q.rd != 5'd0;
    halting = 1'b0;
    halt_reason = HALT_NONE;
    halt_pc = '0;
    halt_tohost_value = '0;
    if (mem_wb_q.valid && mem_wb_q.exc != HALT_NONE) begin
      halting = 1'b1;  // faults never retire
      halt_reason = mem_wb_q.exc;
      halt_pc = mem_wb_q.pc;
    end else if (wb_retire && mem_wb_q.mem == MEM_WRITE && tohost_en && mem_wb_q.mem_addr == tohost_addr) begin
      halting = 1'b1;
      halt_reason = HALT_TOHOST;
      halt_pc = mem_wb_q.pc + 32'd4;
      halt_tohost_value = mem_wb_q.mem_data;
    end
  end
  assign halt_valid = halting;

  assign retire_valid = wb_retire;
  assign retire_pc = mem_wb_q.pc;
  assign retire_insn = mem_wb_q.insn;
  assign retire_rd = mem_wb_q.rd;
  assign retire_rd_val = mem_wb_q.rd != 5'd0 ? mem_wb_q.value : 32'd0;
  assign retire_mem = mem_wb_q.mem;
  assign retire_mem_addr = mem_wb_q.mem != MEM_NONE ? mem_wb_q.mem_addr : 32'd0;
  assign retire_mem_data = mem_wb_q.mem != MEM_NONE ? mem_wb_q.mem_data : 32'd0;
  assign retire_mem_size = mem_wb_q.mem_size;

  // Register file read with WB write-through (P2 §3.1).
  function automatic logic [31:0] read_reg(input logic [4:0] r);
    if (r == 5'd0) return 32'd0;
    if (wb_we && mem_wb_q.rd == r) return mem_wb_q.value;
    return rf_q[r];
  endfunction

  // ---------------------------------------------------------------------------
  // MEM (reads EX/MEM) -> next MEM/WB
  // ---------------------------------------------------------------------------
  mem_wb_t n_mem_wb;
  assign dmem_addr = ex_mem_q.mem_addr;
  assign dmem_size = ex_mem_q.funct3[1:0];
  assign dmem_re = ex_mem_q.valid && ex_mem_q.mem == MEM_READ;
  assign dmem_wdata = ex_mem_q.store_data;
  // The halting cycle commits only WB's own effects (P2 §7).
  assign dmem_we = ex_mem_q.valid && ex_mem_q.mem == MEM_WRITE && !halting;

  always_comb begin
    n_mem_wb = '0;
    n_mem_wb.cause = ex_mem_q.cause;
    if (ex_mem_q.valid) begin
      n_mem_wb.valid = 1'b1;
      n_mem_wb.cause = CAUSE_NONE;
      n_mem_wb.pc = ex_mem_q.pc;
      n_mem_wb.insn = ex_mem_q.insn;
      n_mem_wb.rd = ex_mem_q.rd;
      n_mem_wb.exc = ex_mem_q.exc;
      n_mem_wb.mem = ex_mem_q.mem;
      n_mem_wb.mem_addr = ex_mem_q.mem_addr;
      n_mem_wb.mem_size = ex_mem_q.funct3[1:0];
      case (ex_mem_q.mem)
        MEM_READ: begin
          n_mem_wb.mem_data = dmem_rdata;
          n_mem_wb.value = load_extend(ex_mem_q.funct3, dmem_rdata);
        end
        MEM_WRITE: n_mem_wb.mem_data = ex_mem_q.store_data;
        default:   n_mem_wb.value = ex_mem_q.result;
      endcase
    end
  end

  // ---------------------------------------------------------------------------
  // EX (reads ID/EX; forwarding from EX/MEM and MEM/WB, P2 §6.1)
  // ---------------------------------------------------------------------------
  function automatic logic [31:0] forward(input logic [4:0] rs, input logic [31:0] latched);
    if (rs == 5'd0) return 32'd0;
    if (ex_mem_q.valid && ex_mem_q.exc == HALT_NONE && ex_mem_q.rd == rs) return ex_mem_q.result;
    if (mem_wb_q.valid && mem_wb_q.exc == HALT_NONE && mem_wb_q.rd == rs) return mem_wb_q.value;
    return latched;
  endfunction

  ex_mem_t     n_ex_mem;
  logic        ex_fault;
  logic        redirect;
  logic [31:0] redirect_pc;
  cause_e      redirect_cause;
  logic [31:0] ex_a, ex_b;

  always_comb begin
    logic [31:0] addr, target;
    logic [1:0] size;
    ex_fault = 1'b0;
    redirect = 1'b0;
    redirect_pc = '0;
    redirect_cause = CAUSE_MISPREDICT;
    ex_a = forward(id_ex_q.d.rs1, id_ex_q.rs1_val);
    ex_b = forward(id_ex_q.d.rs2, id_ex_q.rs2_val);
    addr = ex_a + id_ex_q.d.imm;
    size = id_ex_q.d.funct3[1:0];
    target = '0;

    n_ex_mem = '0;
    n_ex_mem.cause = id_ex_q.cause;
    if (id_ex_q.valid) begin
      n_ex_mem.valid = 1'b1;
      n_ex_mem.cause = CAUSE_NONE;
      n_ex_mem.pc = id_ex_q.pc;
      n_ex_mem.insn = id_ex_q.insn;
      n_ex_mem.rd = id_ex_q.d.rd;
      n_ex_mem.funct3 = id_ex_q.d.funct3;
      n_ex_mem.exc = id_ex_q.exc;
      if (id_ex_q.exc != HALT_NONE) begin
        ex_fault = 1'b1;  // detected in ID, acted on now (P2 §6.6)
      end else begin
        case (id_ex_q.d.kind)
          K_LOAD, K_STORE: begin
            if (misaligned(size, addr[1:0])) begin
              ex_fault = 1'b1;
              n_ex_mem.exc = id_ex_q.d.kind == K_LOAD ? HALT_MISALIGNED_LOAD : HALT_MISALIGNED_STORE;
            end else begin
              n_ex_mem.mem = id_ex_q.d.kind == K_LOAD ? MEM_READ : MEM_WRITE;
              n_ex_mem.mem_addr = addr;
              n_ex_mem.store_data = store_mask(size, ex_b);
            end
          end
          K_BRANCH: begin
            target = id_ex_q.pc + id_ex_q.d.imm;
            if (branch_taken(id_ex_q.d.funct3, ex_a, ex_b)) begin
              // Static not-taken: every taken branch is a misprediction.
              if (target[1:0] != 2'b00) begin
                ex_fault = 1'b1;
                n_ex_mem.exc = HALT_MISALIGNED_FETCH;
              end else begin
                redirect = 1'b1;
                redirect_pc = target;
              end
            end
          end
          K_JAL, K_JALR: begin
            target = id_ex_q.d.kind == K_JAL ? id_ex_q.pc + id_ex_q.d.imm : (ex_a + id_ex_q.d.imm) & ~32'd1;
            n_ex_mem.result = id_ex_q.pc + 32'd4;
            if (target[1:0] != 2'b00) begin
              ex_fault = 1'b1;
              n_ex_mem.exc = HALT_MISALIGNED_FETCH;
            end else begin
              redirect = 1'b1;
              redirect_pc = target;
            end
          end
          K_FENCE_I: begin  // refetch everything younger (P2 §6.5)
            redirect = 1'b1;
            redirect_pc = id_ex_q.pc + 32'd4;
            redirect_cause = CAUSE_FENCE_I;
          end
          K_LUI:   n_ex_mem.result = id_ex_q.d.imm;
          K_AUIPC: n_ex_mem.result = id_ex_q.pc + id_ex_q.d.imm;
          K_ALU:   n_ex_mem.result = alu(id_ex_q.d.alu, ex_a, id_ex_q.d.use_imm ? id_ex_q.d.imm : ex_b);
          default: ;  // K_FENCE: nothing to do
        endcase
      end
      if (ex_fault) begin
        n_ex_mem.rd = 5'd0;
        n_ex_mem.mem = MEM_NONE;
      end
    end
  end

  // ---------------------------------------------------------------------------
  // ID (reads IF/ID) -> next ID/EX; load-use detection (P2 §6.2)
  // ---------------------------------------------------------------------------
  id_ex_t n_id_ex;
  logic   load_use;
  always_comb begin
    dec_t d;
    d = decode(if_id_q.insn);
    n_id_ex = '0;
    n_id_ex.cause = if_id_q.cause;
    load_use = 1'b0;
    if (if_id_q.valid) begin
      n_id_ex.valid = 1'b1;
      n_id_ex.cause = CAUSE_NONE;
      n_id_ex.pc = if_id_q.pc;
      n_id_ex.insn = if_id_q.insn;
      n_id_ex.d = d;
      n_id_ex.rs1_val = read_reg(d.rs1);
      n_id_ex.rs2_val = read_reg(d.rs2);
      case (d.kind)
        K_ILLEGAL: n_id_ex.exc = HALT_ILLEGAL;
        K_ECALL:   n_id_ex.exc = HALT_ECALL;
        K_EBREAK:  n_id_ex.exc = HALT_EBREAK;
        default:   n_id_ex.exc = HALT_NONE;
      endcase
      // A valid, non-faulting load in EX whose rd this instruction reads.
      // Unused source fields are 0 and rd != 0, so formats that do not read
      // a register can never match.
      load_use = id_ex_q.valid && id_ex_q.exc == HALT_NONE && !ex_fault &&
                 id_ex_q.d.kind == K_LOAD && id_ex_q.d.rd != 5'd0 &&
                 (d.rs1 == id_ex_q.d.rd || d.rs2 == id_ex_q.d.rd);
    end
  end

  // ---------------------------------------------------------------------------
  // IF
  // ---------------------------------------------------------------------------
  if_id_t if_out;
  assign imem_addr = pc_q;
  always_comb begin
    if_out = '0;
    if_out.cause = CAUSE_FAULT_FLUSH;
    if (!frozen_q) begin
      if_out.valid = 1'b1;
      if_out.cause = CAUSE_NONE;
      if_out.pc = pc_q;
      if_out.insn = imem_rdata;
    end
  end

  // ---------------------------------------------------------------------------
  // Control resolution (P2 §6.7): at most one event; priority mux.
  // ---------------------------------------------------------------------------
  logic [31:0] n_pc;
  logic        n_frozen;
  if_id_t      n_if_id;
  id_ex_t      n_id_ex_sel;
  always_comb begin
    n_pc = frozen_q ? pc_q : pc_q + 32'd4;
    n_frozen = frozen_q;
    n_if_id = if_out;
    n_id_ex_sel = n_id_ex;
    if (ex_fault) begin
      n_if_id = '0;
      n_if_id.cause = CAUSE_FAULT_FLUSH;
      n_id_ex_sel = '0;
      n_id_ex_sel.cause = CAUSE_FAULT_FLUSH;
      n_frozen = 1'b1;
      n_pc = pc_q;
    end else if (redirect) begin
      n_if_id = '0;
      n_if_id.cause = redirect_cause;
      n_id_ex_sel = '0;
      n_id_ex_sel.cause = redirect_cause;
      n_pc = redirect_pc;
    end else if (load_use) begin
      n_if_id = if_id_q;
      n_id_ex_sel = '0;
      n_id_ex_sel.cause = CAUSE_LOAD_USE;
      n_pc = pc_q;
    end
  end

`ifndef SYNTHESIS
  // Contract assertions (P2 §6.1, §6.7), checked in simulation.
  always_ff @(posedge clk) begin
    if (!rst) begin
      if (32'(ex_fault) + 32'(redirect) + 32'(load_use) > 32'd1)
        $error("contract violation: more than one control event");
      if (id_ex_q.valid && ex_mem_q.valid && ex_mem_q.mem == MEM_READ && ex_mem_q.exc == HALT_NONE &&
          ex_mem_q.rd != 5'd0 && (id_ex_q.d.rs1 == ex_mem_q.rd || id_ex_q.d.rs2 == ex_mem_q.rd))
        $error("contract violation: load result needed from EX/MEM");
    end
  end
`endif

  // ---------------------------------------------------------------------------
  // Clock edge
  // ---------------------------------------------------------------------------
  always_ff @(posedge clk) begin
    if (rst) begin
      pc_q <= entry_pc;
      frozen_q <= 1'b0;
      if_id_q <= '{valid: 1'b0, cause: CAUSE_FILL, default: '0};
      id_ex_q <= '{valid: 1'b0, cause: CAUSE_FILL, exc: HALT_NONE, default: '0};
      ex_mem_q <= '{valid: 1'b0, cause: CAUSE_FILL, mem: MEM_NONE, exc: HALT_NONE, default: '0};
      mem_wb_q <= '{valid: 1'b0, cause: CAUSE_FILL, mem: MEM_NONE, exc: HALT_NONE, default: '0};
      for (int r = 0; r < 32; ++r) rf_q[r] <= 32'd0;
    end else begin
      pc_q <= n_pc;
      frozen_q <= n_frozen;
      if_id_q <= n_if_id;
      id_ex_q <= n_id_ex_sel;
      ex_mem_q <= n_ex_mem;
      mem_wb_q <= n_mem_wb;
      if (wb_we) rf_q[mem_wb_q.rd] <= mem_wb_q.value;
    end
  end

  // ---------------------------------------------------------------------------
  // Stage view: what each stage holds during this cycle (before the edge).
  // ---------------------------------------------------------------------------
  assign view_if_pc = pc_q;
  assign view_if_frozen = frozen_q;
  assign view_id_valid = if_id_q.valid;
  assign view_id_pc = if_id_q.pc;
  assign view_id_cause = if_id_q.cause;
  assign view_ex_valid = id_ex_q.valid;
  assign view_ex_pc = id_ex_q.pc;
  assign view_ex_cause = id_ex_q.cause;
  assign view_mem_valid = ex_mem_q.valid;
  assign view_mem_pc = ex_mem_q.pc;
  assign view_mem_cause = ex_mem_q.cause;
  assign view_wb_valid = mem_wb_q.valid;
  assign view_wb_pc = mem_wb_q.pc;
  assign view_wb_cause = mem_wb_q.cause;

endmodule
