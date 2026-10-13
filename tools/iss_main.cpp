// rvsim-iss: command-line driver for the golden ISS.
//
// Exit codes (stable; scripts and CTest rely on them):
//   0  program wrote 1 to tohost (PASS)
//   1  program wrote any other value to tohost (FAIL; test case = value >> 1)
//   2  illegal instruction
//   3  misaligned fetch/load/store
//   4  ecall / ebreak
//   5  instruction budget exhausted (--max-insns)
//   6  usage or load error
#include <charconv>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>

#include "iss/iss.h"
#include "common/loader.h"
#include "common/trace.h"

namespace {

using namespace rvx;
using namespace rvx::iss;

void usage() {
  std::fprintf(stderr,
               "usage: rvsim-iss PROGRAM [options]\n"
               "  --flat                 PROGRAM is a raw binary (default: ELF)\n"
               "  --base ADDR            load address for --flat (default 0x80000000)\n"
               "  --tohost ADDR          tohost address (default: ELF symbol 'tohost')\n"
               "  --trace FILE           write retire trace ('-' for stdout)\n"
               "  --max-insns N          instruction budget (default 10000000)\n");
}

std::optional<uint64_t> parse_num(const char* s) {
  int base = 10;
  if (std::strncmp(s, "0x", 2) == 0 || std::strncmp(s, "0X", 2) == 0) {
    base = 16;
    s += 2;
  }
  uint64_t v = 0;
  const char* end = s + std::strlen(s);
  auto [p, ec] = std::from_chars(s, end, v, base);
  if (ec != std::errc() || p != end || s == end) return std::nullopt;
  return v;
}

}  // namespace

int main(int argc, char** argv) {
  std::string program;
  bool flat = false;
  uint32_t base = 0x80000000;
  std::optional<uint32_t> tohost;
  std::string trace_path;
  uint64_t max_insns = 10'000'000;

  for (int k = 1; k < argc; ++k) {
    const std::string arg = argv[k];
    auto next_num = [&]() -> uint64_t {
      if (k + 1 >= argc) throw std::runtime_error(arg + " needs a value");
      auto v = parse_num(argv[++k]);
      if (!v) throw std::runtime_error("bad number for " + arg + ": " + argv[k]);
      return *v;
    };
    try {
      if (arg == "--flat") flat = true;
      else if (arg == "--base") base = static_cast<uint32_t>(next_num());
      else if (arg == "--tohost") tohost = static_cast<uint32_t>(next_num());
      else if (arg == "--max-insns") max_insns = next_num();
      else if (arg == "--trace") {
        if (k + 1 >= argc) throw std::runtime_error("--trace needs a value");
        trace_path = argv[++k];
      } else if (arg == "-h" || arg == "--help") {
        usage();
        return 0;
      } else if (!arg.empty() && arg[0] == '-') {
        throw std::runtime_error("unknown option " + arg);
      } else if (program.empty()) {
        program = arg;
      } else {
        throw std::runtime_error("more than one PROGRAM given");
      }
    } catch (const std::exception& e) {
      std::fprintf(stderr, "rvsim-iss: %s\n", e.what());
      usage();
      return 6;
    }
  }
  if (program.empty()) {
    usage();
    return 6;
  }

  Memory mem;
  LoadResult loaded;
  try {
    loaded = flat ? load_flat_file(program, base, mem) : load_elf_file(program, mem);
  } catch (const LoadError& e) {
    std::fprintf(stderr, "rvsim-iss: %s\n", e.what());
    return 6;
  }
  if (!tohost) tohost = loaded.symbol("tohost");

  Iss iss(mem, loaded.entry);
  iss.set_tohost(tohost);

  std::ofstream trace_file;
  std::ostream* trace_os = nullptr;
  if (trace_path == "-") {
    trace_os = &std::cout;
  } else if (!trace_path.empty()) {
    trace_file.open(trace_path);
    if (!trace_file) {
      std::fprintf(stderr, "rvsim-iss: cannot open trace file %s\n", trace_path.c_str());
      return 6;
    }
    trace_os = &trace_file;
  }

  RunResult r;
  if (trace_os) {
    TraceWriter w(*trace_os);
    r = iss.run(max_insns, [&](const RetireRecord& rec) { w.write(rec); });
  } else {
    r = iss.run(max_insns);
  }

  int code = 6;
  const char* verdict = "ERROR";
  switch (r.reason) {
    case HaltReason::ToHost:
      code = r.tohost_value == 1 ? 0 : 1;
      verdict = code == 0 ? "PASS" : "FAIL";
      break;
    case HaltReason::IllegalInsn: code = 2; break;
    case HaltReason::MisalignedFetch:
    case HaltReason::MisalignedLoad:
    case HaltReason::MisalignedStore: code = 3; break;
    case HaltReason::Ecall:
    case HaltReason::Ebreak: code = 4; break;
    case HaltReason::MaxInsns:
    case HaltReason::MaxCycles: code = 5; break;
    case HaltReason::None: break;
  }
  std::fprintf(stderr, "rvsim-iss: %s halt=%s retired=%llu pc=0x%08x", verdict,
               std::string(to_string(r.reason)).c_str(),
               static_cast<unsigned long long>(r.retired), r.pc);
  if (r.reason == HaltReason::ToHost) {
    std::fprintf(stderr, " tohost=0x%08x", r.tohost_value);
    if (code == 1) std::fprintf(stderr, " failed_test=%u", r.tohost_value >> 1);
  }
  if (!tohost) std::fprintf(stderr, " (no tohost address)");
  std::fprintf(stderr, "\n");
  return code;
}
