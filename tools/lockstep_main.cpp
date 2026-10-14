// rvsim-lockstep: run the golden ISS and the pipeline side by side and stop at
// the first architectural divergence (contract §8).
//
//   rvsim-lockstep PROGRAM [--config FILE] [--set key=value]... [--max-cycles N]
//
// The pipeline configuration (predictor, caches, latencies) must never change
// architectural results, so lockstep must agree under every configuration.
//
// Each model gets its OWN memory, loaded from the same ELF. Every time the
// pipeline retires an instruction, the ISS executes exactly one, and the two
// RetireRecords are compared as structs, field by field. At the end the halt
// reason, halt pc, tohost value and retired count must also agree.
//
// Exit codes: 0 = the models agree on every retirement and on how the run
//                 ended (whether by tohost or by an identical fault)
//             1 = divergence (report on stdout)
//             2 = no divergence, but the run did not finish (cycle budget)
//             6 = usage / load error
#include <array>
#include <cstdio>
#include <string>
#include <vector>

#include "cli_common.h"
#include "common/decode.h"
#include "common/loader.h"
#include "common/trace.h"
#include "iss/iss.h"
#include "pipeline/pipeline.h"

namespace {

using namespace rvx;

constexpr size_t kHistory = 8;

struct History {
  std::array<RetireRecord, kHistory> buf{};
  size_t n = 0;
  void push(const RetireRecord& r) { buf[n++ % kHistory] = r; }
  void print() const {
    const size_t count = n < kHistory ? n : kHistory;
    std::printf("last %zu retired instructions (identical in both models):\n", count);
    for (size_t k = n - count; k < n; ++k) {
      const auto& r = buf[k % kHistory];
      std::printf("  %s  %s\n", format_retire(r).c_str(),
                  std::string(mnemonic(decode(r.insn).op)).c_str());
    }
  }
};

void row(const char* field, const std::string& a, const std::string& b) {
  std::printf("  %-6s %-12s %-12s%s\n", field, a.c_str(), b.c_str(), a == b ? "" : "  <-- MISMATCH");
}
std::string hex(uint32_t v) {
  char b[12];
  std::snprintf(b, sizeof b, "%08x", v);
  return b;
}
std::string rd_str(const RetireRecord& r) {
  if (r.rd == 0) return "-";
  char b[8];
  std::snprintf(b, sizeof b, "x%02u", static_cast<unsigned>(r.rd));
  return b;
}
std::string mem_str(MemKind m) { return m == MemKind::Read ? "R" : m == MemKind::Write ? "W" : "-"; }

void report_records(const RetireRecord& iss, const RetireRecord& pipe) {
  std::printf("  %-6s %-12s %-12s\n", "field", "iss", "pipeline");
  row("seq", std::to_string(iss.seq), std::to_string(pipe.seq));
  row("pc", hex(iss.pc), hex(pipe.pc));
  row("insn", hex(iss.insn), hex(pipe.insn));
  std::printf("         (%s / %s)\n", std::string(mnemonic(decode(iss.insn).op)).c_str(),
              std::string(mnemonic(decode(pipe.insn).op)).c_str());
  row("rd", rd_str(iss), rd_str(pipe));
  row("val", iss.rd ? hex(iss.rd_val) : "-", pipe.rd ? hex(pipe.rd_val) : "-");
  row("mem", mem_str(iss.mem), mem_str(pipe.mem));
  row("addr", iss.mem != MemKind::None ? hex(iss.mem_addr) : "-",
      pipe.mem != MemKind::None ? hex(pipe.mem_addr) : "-");
  row("data", iss.mem != MemKind::None ? hex(iss.mem_data) : "-",
      pipe.mem != MemKind::None ? hex(pipe.mem_data) : "-");
  row("sz", std::to_string(iss.mem_size), std::to_string(pipe.mem_size));
}

}  // namespace

int main(int argc, char** argv) {
  std::string program, config_path;
  std::vector<std::string> overrides;
  pipe::Config cfg;
  uint64_t max_cycles = 100'000'000;
  try {
    for (int k = 1; k < argc; ++k) {
      const std::string a = argv[k];
      if (a == "--config") config_path = cli::next_str(k, argc, argv);
      else if (a == "--set") overrides.push_back(cli::next_str(k, argc, argv));
      else if (a == "--max-cycles") max_cycles = cli::next_num(k, argc, argv);
      else if (!a.empty() && a[0] == '-') throw std::runtime_error("unknown option " + a);
      else if (program.empty()) program = a;
      else throw std::runtime_error("more than one PROGRAM given");
    }
    if (program.empty()) throw std::runtime_error("no PROGRAM given");
    cfg = pipe::load_config(config_path, overrides);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "rvsim-lockstep: %s\n", e.what());
    return 6;
  }

  Memory iss_mem, pipe_mem;  // separate instances: the models share no state
  LoadResult loaded;
  try {
    loaded = load_elf_file(program, iss_mem);
    load_elf_file(program, pipe_mem);
  } catch (const LoadError& e) {
    std::fprintf(stderr, "rvsim-lockstep: %s\n", e.what());
    return 6;
  }
  const auto tohost = loaded.symbol("tohost");

  iss::Iss golden(iss_mem, loaded.entry);
  golden.set_tohost(tohost);
  pipe::Pipeline model(pipe_mem, loaded.entry, cfg);
  model.set_tohost(tohost);

  History hist;
  auto diverge = [&](const char* what) {
    std::printf("DIVERGENCE (%s) at cycle %llu, after %llu matching retirements\n", what,
                static_cast<unsigned long long>(model.cycles()),
                static_cast<unsigned long long>(hist.n));
  };
  auto context = [&] {
    hist.print();
    std::printf("%s", model.describe().c_str());
  };

  try {
    while (!model.halted() && model.cycles() < max_cycles) {
      auto p = model.tick();
      if (!p) continue;
      auto g = golden.step();
      if (!g) {
        diverge("pipeline retired an instruction the ISS did not");
        std::printf("  ISS halted: %s at pc %08x\n", std::string(to_string(golden.result().reason)).c_str(),
                    golden.result().pc);
        std::printf("  pipeline retired: %s\n", format_retire(*p).c_str());
        context();
        return 1;
      }
      if (!(*g == *p)) {
        diverge("retire record mismatch");
        report_records(*g, *p);
        context();
        return 1;
      }
      hist.push(*p);
    }
  } catch (const std::logic_error& e) {
    diverge(e.what());
    context();
    return 1;
  }

  if (!model.halted()) {
    std::printf("INCOMPLETE: no divergence in %llu cycles (%llu retired), but the program did not finish\n",
                static_cast<unsigned long long>(model.cycles()),
                static_cast<unsigned long long>(hist.n));
    return 2;
  }

  // The pipeline halted. A fault halts it without retiring; the ISS discovers
  // the same fault on its next step. After a tohost store both are halted.
  if (!golden.halted()) {
    if (auto extra = golden.step()) {
      diverge("ISS retired an instruction after the pipeline halted");
      std::printf("  ISS retired: %s\n", format_retire(*extra).c_str());
      context();
      return 1;
    }
  }
  const auto& ph = model.halt();
  const auto gr = golden.result();
  if (ph.reason != gr.reason || ph.pc != gr.pc || model.retired() != gr.retired ||
      (ph.reason == HaltReason::ToHost && ph.tohost_value != gr.tohost_value)) {
    diverge("halt state mismatch");
    std::printf("  %-8s %-20s %-20s\n", "", "iss", "pipeline");
    std::printf("  %-8s %-20s %-20s\n", "reason", std::string(to_string(gr.reason)).c_str(),
                std::string(to_string(ph.reason)).c_str());
    std::printf("  %-8s %-20s %-20s\n", "pc", hex(gr.pc).c_str(), hex(ph.pc).c_str());
    std::printf("  %-8s %-20llu %-20llu\n", "retired", static_cast<unsigned long long>(gr.retired),
                static_cast<unsigned long long>(model.retired()));
    std::printf("  %-8s %-20s %-20s\n", "tohost", hex(gr.tohost_value).c_str(), hex(ph.tohost_value).c_str());
    context();
    return 1;
  }

  std::printf("LOCKSTEP OK %s: retired=%llu cycles=%llu halt=%s tohost=%08x\n", program.c_str(),
              static_cast<unsigned long long>(model.retired()),
              static_cast<unsigned long long>(model.cycles()), std::string(to_string(ph.reason)).c_str(),
              ph.tohost_value);
  return 0;
}
