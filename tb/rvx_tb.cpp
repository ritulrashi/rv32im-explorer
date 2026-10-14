// rvsim-rtl: runs a program on the Verilated RTL core (rtl/rvx_core.sv).
//
//   rvsim-rtl PROGRAM [--trace FILE|-] [--diagram FILE|-] [--max-cycles N]
//
// Output formats are exactly those of rvsim-pipe (retire trace and per-cycle
// stage diagram), and the stderr summary line has the same fields, so the
// correlation script can compare the two byte for byte (docs/phase5-design.md).
//
// Per cycle (docs/phase5-design.md §4):
//   1. eval: state is settled; the read addresses come straight from flops
//   2. read the backing memory at the fetch and data addresses, drive rdata
//   3. eval: all combinational logic settles
//   4. sample retire / stage-view / halt outputs and the store request
//   5. apply the store, then the clock edge
// The backing store and ELF loader are the project's shared harness
// (src/common), as in lockstep; all ISA semantics are in the RTL.
#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

#include "Vrvx_core.h"
#include "cli_common.h"
#include "common/loader.h"
#include "common/trace.h"
#include "verilated.h"

namespace {

using namespace rvx;

// Must match rtl/rvx_pkg.sv enum order.
enum : uint8_t { CAUSE_NONE, CAUSE_FILL, CAUSE_LOAD_USE, CAUSE_MISPREDICT, CAUSE_FENCE_I, CAUSE_FAULT_FLUSH };
enum : uint8_t {
  HALT_NONE, HALT_TOHOST, HALT_ILLEGAL, HALT_MISALIGNED_FETCH, HALT_MISALIGNED_LOAD,
  HALT_MISALIGNED_STORE, HALT_ECALL, HALT_EBREAK
};
enum : uint8_t { MEM_NONE, MEM_READ, MEM_WRITE };

const char* cause_name(uint8_t c) {  // the C++ model's names (pipeline.cpp to_string)
  switch (c) {
    case CAUSE_FILL: return "fill";
    case CAUSE_LOAD_USE: return "load-use";
    case CAUSE_MISPREDICT: return "mispredict";
    case CAUSE_FENCE_I: return "fence.i";
    case CAUSE_FAULT_FLUSH: return "fault-flush";
    default: return "none";
  }
}

HaltReason halt_reason(uint8_t h) {
  switch (h) {
    case HALT_TOHOST: return HaltReason::ToHost;
    case HALT_ILLEGAL: return HaltReason::IllegalInsn;
    case HALT_MISALIGNED_FETCH: return HaltReason::MisalignedFetch;
    case HALT_MISALIGNED_LOAD: return HaltReason::MisalignedLoad;
    case HALT_MISALIGNED_STORE: return HaltReason::MisalignedStore;
    case HALT_ECALL: return HaltReason::Ecall;
    case HALT_EBREAK: return HaltReason::Ebreak;
    default: return HaltReason::None;
  }
}

std::string cell(bool valid, uint32_t pc, uint8_t cause) {
  char b[32];
  if (valid) std::snprintf(b, sizeof b, "%08x", pc);
  else std::snprintf(b, sizeof b, "-%s", cause_name(cause));
  return b;
}

}  // namespace

int main(int argc, char** argv) {
  std::string program, trace_path, diagram_path;
  uint64_t max_cycles = 100'000'000;
  try {
    for (int k = 1; k < argc; ++k) {
      const std::string a = argv[k];
      if (a == "--trace") trace_path = cli::next_str(k, argc, argv);
      else if (a == "--diagram") diagram_path = cli::next_str(k, argc, argv);
      else if (a == "--max-cycles") max_cycles = cli::next_num(k, argc, argv);
      else if (!a.empty() && a[0] == '-') throw std::runtime_error("unknown option " + a);
      else if (program.empty()) program = a;
      else throw std::runtime_error("more than one PROGRAM given");
    }
    if (program.empty()) throw std::runtime_error("no PROGRAM given");
  } catch (const std::exception& e) {
    std::fprintf(stderr, "rvsim-rtl: %s\n", e.what());
    return 6;
  }

  Memory mem;
  LoadResult loaded;
  try {
    loaded = load_elf_file(program, mem);
  } catch (const LoadError& e) {
    std::fprintf(stderr, "rvsim-rtl: %s\n", e.what());
    return 6;
  }
  const auto tohost = loaded.symbol("tohost");

  std::ofstream tf, df;
  std::ostream* trace_os = trace_path == "-" ? &std::cout : nullptr;
  std::ostream* diag_os = diagram_path == "-" ? &std::cout : nullptr;
  if (!trace_path.empty() && trace_path != "-") {
    tf.open(trace_path);
    trace_os = &tf;
  }
  if (!diagram_path.empty() && diagram_path != "-") {
    df.open(diagram_path);
    diag_os = &df;
  }

  auto ctx = std::make_unique<VerilatedContext>();
  auto top = std::make_unique<Vrvx_core>(ctx.get());
  top->entry_pc = loaded.entry;
  top->tohost_en = tohost ? 1 : 0;
  top->tohost_addr = tohost ? *tohost : 0;
  top->imem_rdata = 0;
  top->dmem_rdata = 0;

  // Synchronous reset: one clock edge with rst high.
  top->clk = 0;
  top->rst = 1;
  top->eval();
  top->clk = 1;
  top->eval();
  top->clk = 0;
  top->rst = 0;
  top->eval();

  if (diag_os) *diag_os << "# cycle IF ID EX MEM WB\n";
  uint64_t cycle = 0, retired = 0;
  HaltReason reason = HaltReason::None;
  uint32_t halt_pc = 0, tohost_value = 0;
  while (true) {
    if (cycle >= max_cycles) {
      reason = HaltReason::MaxCycles;
      halt_pc = top->imem_addr;
      break;
    }
    ++cycle;
    top->eval();
    top->imem_rdata = mem.load(top->imem_addr, 4);
    const unsigned dsize = 1u << top->dmem_size;
    top->dmem_rdata = top->dmem_re ? mem.load(top->dmem_addr, dsize) : 0;
    top->eval();

    if (top->retire_valid) {
      RetireRecord r;
      r.seq = retired++;
      r.pc = top->retire_pc;
      r.insn = top->retire_insn;
      r.rd = top->retire_rd;
      r.rd_val = top->retire_rd_val;
      r.mem = top->retire_mem == MEM_READ ? MemKind::Read : top->retire_mem == MEM_WRITE ? MemKind::Write : MemKind::None;
      if (r.mem != MemKind::None) {
        r.mem_addr = top->retire_mem_addr;
        r.mem_data = top->retire_mem_data;
        r.mem_size = static_cast<uint8_t>(1u << top->retire_mem_size);
      }
      if (trace_os) *trace_os << format_retire(r) << '\n';
    }
    if (diag_os) {
      *diag_os << cycle << ' ' << (top->view_if_frozen ? std::string("-frozen") : cell(true, top->view_if_pc, 0)) << ' '
               << cell(top->view_id_valid, top->view_id_pc, top->view_id_cause) << ' '
               << cell(top->view_ex_valid, top->view_ex_pc, top->view_ex_cause) << ' '
               << cell(top->view_mem_valid, top->view_mem_pc, top->view_mem_cause) << ' '
               << cell(top->view_wb_valid, top->view_wb_pc, top->view_wb_cause) << '\n';
    }
    if (top->halt_valid) {
      reason = halt_reason(top->halt_reason);
      halt_pc = top->halt_pc;
      tohost_value = top->halt_tohost_value;
      break;
    }
    if (top->dmem_we) mem.store(top->dmem_addr, dsize, top->dmem_wdata);
    top->clk = 1;
    top->eval();
    top->clk = 0;
  }
  top->final();

  const int code = cli::exit_code(reason, tohost_value);
  std::fprintf(stderr, "rvsim-rtl: %s halt=%s retired=%llu cycles=%llu pc=0x%08x",
               code == 0 ? "PASS" : code == 1 ? "FAIL" : "ERROR", std::string(to_string(reason)).c_str(),
               static_cast<unsigned long long>(retired), static_cast<unsigned long long>(cycle), halt_pc);
  if (reason == HaltReason::ToHost) std::fprintf(stderr, " tohost=0x%08x", tohost_value);
  std::fprintf(stderr, "\n");
  return code;
}
