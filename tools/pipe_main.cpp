// rvsim-pipe: run a program on the cycle-accurate pipeline model.
//
//   rvsim-pipe PROGRAM [--config FILE] [--set key=value]... [--max-cycles N]
//              [--trace FILE|-] [--diagram FILE|-] [--stats FILE|-]
//
// Prints a one-line summary on stderr. Exit codes as rvsim-iss.
// --stats writes JSON: resolved config, cycles, CPI stack, predictor, cache
// and DRAM counters (docs/phase3-design.md §5).
// --diagram writes one line per cycle: cycle, then the pc held by each of
// IF ID EX MEM WB, or "--------" for a bubble (with its cause).
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "cli_common.h"
#include "common/loader.h"
#include "common/trace.h"
#include "pipeline/pipeline.h"
#include "pipeline/stats_json.h"

namespace {

using namespace rvx;
using namespace rvx::pipe;

std::string stage_cell(const std::optional<uint32_t>& pc, BubbleCause cause) {
  char b[32];
  if (pc) {
    std::snprintf(b, sizeof b, "%08x", *pc);
  } else {
    std::snprintf(b, sizeof b, "--%-6.6s", std::string(to_string(cause)).c_str());
  }
  return b;
}

}  // namespace

int main(int argc, char** argv) {
  std::string program, trace_path, diagram_path, stats_path, config_path;
  std::vector<std::string> overrides;
  Config cfg;
  uint64_t max_cycles = 100'000'000;
  try {
    for (int k = 1; k < argc; ++k) {
      const std::string a = argv[k];
      if (a == "--trace") trace_path = cli::next_str(k, argc, argv);
      else if (a == "--diagram") diagram_path = cli::next_str(k, argc, argv);
      else if (a == "--stats") stats_path = cli::next_str(k, argc, argv);
      else if (a == "--config") config_path = cli::next_str(k, argc, argv);
      else if (a == "--set") overrides.push_back(cli::next_str(k, argc, argv));
      else if (a == "--max-cycles") max_cycles = cli::next_num(k, argc, argv);
      else if (!a.empty() && a[0] == '-') throw std::runtime_error("unknown option " + a);
      else if (program.empty()) program = a;
      else throw std::runtime_error("more than one PROGRAM given");
    }
    if (program.empty()) throw std::runtime_error("no PROGRAM given");
    cfg = load_config(config_path, overrides);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "rvsim-pipe: %s\n", e.what());
    return 6;
  }

  Memory mem;
  LoadResult loaded;
  try {
    loaded = load_elf_file(program, mem);
  } catch (const LoadError& e) {
    std::fprintf(stderr, "rvsim-pipe: %s\n", e.what());
    return 6;
  }

  auto open = [](const std::string& path, std::ofstream& f) -> std::ostream* {
    if (path.empty()) return nullptr;
    if (path == "-") return &std::cout;
    f.open(path);
    if (!f) throw std::runtime_error("cannot open " + path);
    return &f;
  };
  std::ofstream tf, df, sf;
  std::ostream* trace_os = nullptr;
  std::ostream* diag_os = nullptr;
  std::ostream* stats_os = nullptr;
  try {
    trace_os = open(trace_path, tf);
    diag_os = open(diagram_path, df);
    stats_os = open(stats_path, sf);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "rvsim-pipe: %s\n", e.what());
    return 6;
  }

  Pipeline p(mem, loaded.entry, cfg);
  p.set_tohost(loaded.symbol("tohost"));
  if (diag_os) *diag_os << "cycle    IF       ID       EX       MEM      WB\n";
  try {
    while (!p.halted()) {
      if (p.cycles() >= max_cycles) {
        p.run(max_cycles);  // records MaxCycles
        break;
      }
      auto rec = p.tick();
      if (trace_os && rec) *trace_os << format_retire(*rec) << '\n';
      if (diag_os) {
        const auto& v = p.last_view();
        char c[24];
        std::snprintf(c, sizeof c, "%-8llu ", static_cast<unsigned long long>(p.cycles()));
        *diag_os << c << stage_cell(v.if_pc, BubbleCause::FaultFlush) << ' '
                 << stage_cell(v.id_pc, v.id_cause) << ' ' << stage_cell(v.ex_pc, v.ex_cause) << ' '
                 << stage_cell(v.mem_pc, v.mem_cause) << ' ' << stage_cell(v.wb_pc, v.wb_cause) << '\n';
      }
    }
  } catch (const std::logic_error& e) {
    std::fprintf(stderr, "rvsim-pipe: %s\n%s", e.what(), p.describe().c_str());
    return 6;
  }

  const auto& h = p.halt();
  const int code = cli::exit_code(h.reason, h.tohost_value);
  std::fprintf(stderr, "rvsim-pipe: %s halt=%s retired=%llu cycles=%llu pc=0x%08x",
               code == 0 ? "PASS" : code == 1 ? "FAIL" : "ERROR",
               std::string(to_string(h.reason)).c_str(),
               static_cast<unsigned long long>(p.retired()),
               static_cast<unsigned long long>(p.cycles()), h.pc);
  if (h.reason == HaltReason::ToHost) std::fprintf(stderr, " tohost=0x%08x", h.tohost_value);
  if (stats_os) *stats_os << stats_json(p, program);
  std::fprintf(stderr, "\n");
  return code;
}
