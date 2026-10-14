// Types, decoder and execution functions for the RV32I RTL core.
//
// Implements docs/phase2-pipeline-contract.md ("P2 §n") for the baseline
// machine (static not-taken, ideal memory). Written from scratch: nothing here
// is derived from the C++ models, so the RTL is a third independent
// implementation of the ISA semantics (docs/phase5-design.md §3).
package rvx_pkg;

  // Why a pipeline register holds a bubble (P2 §4). Names printed by the
  // testbench match the C++ model's (fill, load-use, mispredict, ...).
  typedef enum logic [2:0] {
    CAUSE_NONE,
    CAUSE_FILL,
    CAUSE_LOAD_USE,
    CAUSE_MISPREDICT,
    CAUSE_FENCE_I,
    CAUSE_FAULT_FLUSH
  } cause_e;

  typedef enum logic [3:0] {
    HALT_NONE,
    HALT_TOHOST,
    HALT_ILLEGAL,
    HALT_MISALIGNED_FETCH,
    HALT_MISALIGNED_LOAD,
    HALT_MISALIGNED_STORE,
    HALT_ECALL,
    HALT_EBREAK
  } halt_e;

  typedef enum logic [1:0] {MEM_NONE, MEM_READ, MEM_WRITE} memk_e;

  typedef enum logic [3:0] {
    ALU_ADD, ALU_SUB, ALU_SLL, ALU_SLT, ALU_SLTU, ALU_XOR, ALU_SRL, ALU_SRA, ALU_OR, ALU_AND
  } alu_e;

  typedef enum logic [3:0] {
    K_ALU, K_LUI, K_AUIPC, K_JAL, K_JALR, K_BRANCH, K_LOAD, K_STORE,
    K_FENCE, K_FENCE_I, K_ECALL, K_EBREAK, K_ILLEGAL
  } kind_e;

  // Decoded instruction. Register fields the format does not use are 0, so
  // hazard logic can compare them without knowing the format (P2 §6.2).
  typedef struct packed {
    kind_e       kind;
    alu_e        alu;
    logic        use_imm;  // ALU operand b is the immediate (I-type)
    logic [2:0]  funct3;   // branch condition / load-store size and sign
    logic [4:0]  rd;
    logic [4:0]  rs1;
    logic [4:0]  rs2;
    logic [31:0] imm;
  } dec_t;

  // ---- Pipeline registers (P2 §4) ----
  typedef struct packed {
    logic        valid;
    cause_e      cause;
    logic [31:0] pc;
    logic [31:0] insn;
  } if_id_t;

  typedef struct packed {
    logic        valid;
    cause_e      cause;
    logic [31:0] pc;
    logic [31:0] insn;
    dec_t        d;
    logic [31:0] rs1_val;  // as read in ID; EX forwarding may override
    logic [31:0] rs2_val;
    halt_e       exc;      // fault detected in ID (illegal, ecall, ebreak)
  } id_ex_t;

  typedef struct packed {
    logic        valid;
    cause_e      cause;
    logic [31:0] pc;
    logic [31:0] insn;
    logic [4:0]  rd;          // 0: no register write
    logic [31:0] result;      // ALU / link value
    logic [31:0] store_data;  // already masked to the access size
    memk_e       mem;
    logic [31:0] mem_addr;
    logic [2:0]  funct3;      // load/store size and sign
    halt_e       exc;
  } ex_mem_t;

  typedef struct packed {
    logic        valid;
    cause_e      cause;
    logic [31:0] pc;
    logic [31:0] insn;
    logic [4:0]  rd;
    logic [31:0] value;     // final value for rd (loads already extended)
    memk_e       mem;
    logic [31:0] mem_addr;
    logic [31:0] mem_data;  // load: raw bytes read; store: data written
    logic [1:0]  mem_size;  // 0: byte, 1: half, 2: word
    halt_e       exc;
  } mem_wb_t;

  // ---- Decoder (RV32I + FENCE/FENCE.I; everything else is illegal) ----
  function automatic dec_t decode(input logic [31:0] i);
    dec_t d;
    logic [6:0] op;
    logic [2:0] f3;
    logic [6:0] f7;
    logic [31:0] imm_i, imm_s, imm_b, imm_u, imm_j;
    logic legal;
    legal = 1'b0;
    op = i[6:0];
    f3 = i[14:12];
    f7 = i[31:25];
    imm_i = {{20{i[31]}}, i[31:20]};
    imm_s = {{20{i[31]}}, i[31:25], i[11:7]};
    imm_b = {{19{i[31]}}, i[31], i[7], i[30:25], i[11:8], 1'b0};
    imm_u = {i[31:12], 12'b0};
    imm_j = {{11{i[31]}}, i[31], i[19:12], i[20], i[30:21], 1'b0};

    d = '0;
    d.kind = K_ILLEGAL;
    d.alu = ALU_ADD;
    d.funct3 = f3;
    case (op)
      7'h37: begin d.kind = K_LUI;   d.rd = i[11:7]; d.imm = imm_u; end
      7'h17: begin d.kind = K_AUIPC; d.rd = i[11:7]; d.imm = imm_u; end
      7'h6F: begin d.kind = K_JAL;   d.rd = i[11:7]; d.imm = imm_j; end
      7'h67:
        if (f3 == 3'd0) begin d.kind = K_JALR; d.rd = i[11:7]; d.rs1 = i[19:15]; d.imm = imm_i; end
      7'h63:
        if (f3 != 3'd2 && f3 != 3'd3) begin
          d.kind = K_BRANCH; d.rs1 = i[19:15]; d.rs2 = i[24:20]; d.imm = imm_b;
        end
      7'h03:
        if (f3 == 3'd0 || f3 == 3'd1 || f3 == 3'd2 || f3 == 3'd4 || f3 == 3'd5) begin
          d.kind = K_LOAD; d.rd = i[11:7]; d.rs1 = i[19:15]; d.imm = imm_i;
        end
      7'h23:
        if (f3 <= 3'd2) begin d.kind = K_STORE; d.rs1 = i[19:15]; d.rs2 = i[24:20]; d.imm = imm_s; end
      7'h13: begin  // OP-IMM
        legal = 1'b1;
        case (f3)
          3'd0: d.alu = ALU_ADD;
          3'd2: d.alu = ALU_SLT;
          3'd3: d.alu = ALU_SLTU;
          3'd4: d.alu = ALU_XOR;
          3'd6: d.alu = ALU_OR;
          3'd7: d.alu = ALU_AND;
          3'd1: begin
            d.alu = ALU_SLL;
            legal = (f7 == 7'h00);  // also rejects RV64 shamt[5]
          end
          default: begin  // 3'd5
            if (f7 == 7'h00) d.alu = ALU_SRL;
            else if (f7 == 7'h20) d.alu = ALU_SRA;
            else legal = 1'b0;
          end
        endcase
        if (legal) begin  // illegal encodings keep every register field at 0
          d.kind = K_ALU;
          d.rd = i[11:7];
          d.rs1 = i[19:15];
          d.use_imm = 1'b1;
          d.imm = (f3 == 3'd1 || f3 == 3'd5) ? {27'b0, i[24:20]} : imm_i;  // shamt for shifts
        end
      end
      7'h33: begin  // OP. funct7 = 1 (M extension) is not implemented: illegal.
        if (f7 == 7'h00) begin
          d.kind = K_ALU; d.rd = i[11:7]; d.rs1 = i[19:15]; d.rs2 = i[24:20];
          case (f3)
            3'd0: d.alu = ALU_ADD;
            3'd1: d.alu = ALU_SLL;
            3'd2: d.alu = ALU_SLT;
            3'd3: d.alu = ALU_SLTU;
            3'd4: d.alu = ALU_XOR;
            3'd5: d.alu = ALU_SRL;
            3'd6: d.alu = ALU_OR;
            default: d.alu = ALU_AND;
          endcase
        end else if (f7 == 7'h20 && (f3 == 3'd0 || f3 == 3'd5)) begin
          d.kind = K_ALU; d.rd = i[11:7]; d.rs1 = i[19:15]; d.rs2 = i[24:20];
          d.alu = (f3 == 3'd0) ? ALU_SUB : ALU_SRA;
        end
      end
      7'h0F: begin
        if (f3 == 3'd0) d.kind = K_FENCE;
        else if (f3 == 3'd1) d.kind = K_FENCE_I;
      end
      7'h73: begin
        if (i == 32'h0000_0073) d.kind = K_ECALL;
        else if (i == 32'h0010_0073) d.kind = K_EBREAK;
      end
      default: ;
    endcase
    return d;
  endfunction

  // ---- Execution ----
  function automatic logic [31:0] alu(input alu_e op, input logic [31:0] a, input logic [31:0] b);
    case (op)
      ALU_ADD:  return a + b;
      ALU_SUB:  return a - b;
      ALU_SLL:  return a << b[4:0];
      ALU_SLT:  return {31'b0, $signed(a) < $signed(b)};
      ALU_SLTU: return {31'b0, a < b};
      ALU_XOR:  return a ^ b;
      ALU_SRL:  return a >> b[4:0];
      ALU_SRA:  return $unsigned($signed(a) >>> b[4:0]);
      ALU_OR:   return a | b;
      default:  return a & b;
    endcase
  endfunction

  function automatic logic branch_taken(input logic [2:0] f3, input logic [31:0] a, input logic [31:0] b);
    case (f3)
      3'd0:    return a == b;
      3'd1:    return a != b;
      3'd4:    return $signed(a) < $signed(b);
      3'd5:    return $signed(a) >= $signed(b);
      3'd6:    return a < b;
      default: return a >= b;  // 3'd7 (2 and 3 never decode as branches)
    endcase
  endfunction

  // Access size in bytes-code: 0 byte, 1 half, 2 word (funct3[1:0] for loads/stores).
  function automatic logic misaligned(input logic [1:0] size, input logic [1:0] addr_lo);
    case (size)
      2'd0:    return 1'b0;
      2'd1:    return addr_lo[0];
      default: return addr_lo != 2'b00;
    endcase
  endfunction

  function automatic logic [31:0] store_mask(input logic [1:0] size, input logic [31:0] v);
    case (size)
      2'd0:    return {24'b0, v[7:0]};
      2'd1:    return {16'b0, v[15:0]};
      default: return v;
    endcase
  endfunction

  // raw: the size-byte value read, zero-extended (testbench contract).
  function automatic logic [31:0] load_extend(input logic [2:0] f3, input logic [31:0] raw);
    case (f3)
      3'd0:    return {{24{raw[7]}}, raw[7:0]};    // LB
      3'd1:    return {{16{raw[15]}}, raw[15:0]};  // LH
      default: return raw;                         // LW, LBU, LHU
    endcase
  endfunction

endpackage
