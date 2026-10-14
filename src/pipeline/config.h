// Full machine configuration for the pipeline model (docs/phase3-design.md §1).
// The defaults are the Phase 2 baseline: static not-taken, no BTB, caches off.
#pragma once

#include <string>
#include <vector>

#include "bpred/predictor.h"
#include "cache/cache.h"
#include "cache/dram.h"
#include "common/kvfile.h"

namespace rvx::pipe {

struct Config {
  unsigned mul_latency = 3;   // cycles a MUL/MULH* occupies EX (>= 1)
  unsigned div_latency = 33;  // cycles a DIV/DIVU/REM/REMU occupies EX (>= 1)
  bpred::Config bpred;
  cache::Config icache;
  cache::Config dcache;
  cache::DramConfig dram;
};

// Applies one setting. Throws ConfigError for an unknown key or bad value.
void apply_setting(Config& cfg, const std::string& key, const std::string& value);

// Throws ConfigError if the combination is invalid (sizes not powers of two,
// history longer than the table index, etc.).
void validate(const Config& cfg);

// Defaults, then the file (if non-empty path), then overrides; validated.
Config load_config(const std::string& path, const std::vector<std::string>& overrides);

// Every setting as `key = value` lines in a fixed order. Parsing this text
// back yields the same Config.
std::string describe(const Config& cfg);

}  // namespace rvx::pipe
