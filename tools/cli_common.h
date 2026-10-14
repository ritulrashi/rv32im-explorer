// Small helpers shared by the command-line tools (not by the models).
#pragma once

#include <charconv>
#include <cstdint>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>

#include "common/retire.h"

namespace rvx::cli {

inline std::optional<uint64_t> parse_num(const char* s) {
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

// Reads the numeric value following argv[k] (and advances k).
inline uint64_t next_num(int& k, int argc, char** argv) {
  const std::string opt = argv[k];
  if (k + 1 >= argc) throw std::runtime_error(opt + " needs a value");
  auto v = parse_num(argv[++k]);
  if (!v) throw std::runtime_error("bad number for " + opt + ": " + argv[k]);
  return *v;
}

inline std::string next_str(int& k, int argc, char** argv) {
  const std::string opt = argv[k];
  if (k + 1 >= argc) throw std::runtime_error(opt + " needs a value");
  return argv[++k];
}

// Exit codes shared by rvsim-iss and rvsim-pipe (see tools/iss_main.cpp).
inline int exit_code(HaltReason r, uint32_t tohost_value) {
  switch (r) {
    case HaltReason::ToHost: return tohost_value == 1 ? 0 : 1;
    case HaltReason::IllegalInsn: return 2;
    case HaltReason::MisalignedFetch:
    case HaltReason::MisalignedLoad:
    case HaltReason::MisalignedStore: return 3;
    case HaltReason::Ecall:
    case HaltReason::Ebreak: return 4;
    case HaltReason::MaxInsns:
    case HaltReason::MaxCycles: return 5;
    case HaltReason::None: return 6;
  }
  return 6;
}

}  // namespace rvx::cli
