#include "pipeline/config.h"

#include <charconv>
#include <fstream>
#include <sstream>

namespace rvx::pipe {
namespace {

unsigned to_uint(const std::string& key, const std::string& v) {
  unsigned out = 0;
  auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), out, 10);
  if (v.empty() || ec != std::errc() || p != v.data() + v.size()) {
    throw ConfigError(key + ": expected a non-negative integer, got `" + v + "`");
  }
  return out;
}

bool to_bool(const std::string& key, const std::string& v) {
  if (v == "true") return true;
  if (v == "false") return false;
  throw ConfigError(key + ": expected true or false, got `" + v + "`");
}

bpred::Type to_type(const std::string& key, const std::string& v) {
  if (v == "static_nt") return bpred::Type::StaticNT;
  if (v == "bimodal") return bpred::Type::Bimodal;
  if (v == "gshare") return bpred::Type::Gshare;
  throw ConfigError(key + ": expected static_nt, bimodal or gshare, got `" + v + "`");
}

bool pow2(unsigned v) { return v != 0 && (v & (v - 1)) == 0; }
unsigned log2u(unsigned v) {
  unsigned n = 0;
  while (v > 1) {
    v >>= 1;
    ++n;
  }
  return n;
}

void check(bool ok, const std::string& msg) {
  if (!ok) throw ConfigError(msg);
}

void validate_cache(const std::string& name, const cache::Config& c, const cache::DramConfig& d) {
  if (!c.enabled) return;
  check(pow2(c.line_bytes) && c.line_bytes >= 4, name + ".line_bytes must be a power of two >= 4");
  check(pow2(c.size_bytes), name + ".size_bytes must be a power of two");
  check(c.assoc >= 1, name + ".assoc must be >= 1");
  check(c.size_bytes % (c.assoc * c.line_bytes) == 0 && c.size_bytes >= c.assoc * c.line_bytes,
        name + ": size_bytes must be a multiple of assoc * line_bytes");
  check(pow2(c.size_bytes / (c.assoc * c.line_bytes)), name + ": number of sets must be a power of two");
  check(c.line_bytes % d.bus_bytes_per_cycle == 0,
        name + ".line_bytes must be a multiple of dram.bus_bytes_per_cycle");
}

}  // namespace

void apply_setting(Config& c, const std::string& key, const std::string& v) {
  if (key == "core.mul_latency") c.mul_latency = to_uint(key, v);
  else if (key == "core.div_latency") c.div_latency = to_uint(key, v);
  else if (key == "bpred.type") c.bpred.type = to_type(key, v);
  else if (key == "bpred.entries") c.bpred.entries = to_uint(key, v);
  else if (key == "bpred.history_bits") c.bpred.history_bits = to_uint(key, v);
  else if (key == "btb.entries") c.bpred.btb_entries = to_uint(key, v);
  else if (key == "icache.enabled") c.icache.enabled = to_bool(key, v);
  else if (key == "icache.size_bytes") c.icache.size_bytes = to_uint(key, v);
  else if (key == "icache.assoc") c.icache.assoc = to_uint(key, v);
  else if (key == "icache.line_bytes") c.icache.line_bytes = to_uint(key, v);
  else if (key == "dcache.enabled") c.dcache.enabled = to_bool(key, v);
  else if (key == "dcache.size_bytes") c.dcache.size_bytes = to_uint(key, v);
  else if (key == "dcache.assoc") c.dcache.assoc = to_uint(key, v);
  else if (key == "dcache.line_bytes") c.dcache.line_bytes = to_uint(key, v);
  else if (key == "dram.banks") c.dram.banks = to_uint(key, v);
  else if (key == "dram.row_bytes") c.dram.row_bytes = to_uint(key, v);
  else if (key == "dram.row_hit_cycles") c.dram.row_hit_cycles = to_uint(key, v);
  else if (key == "dram.row_closed_cycles") c.dram.row_closed_cycles = to_uint(key, v);
  else if (key == "dram.row_miss_cycles") c.dram.row_miss_cycles = to_uint(key, v);
  else if (key == "dram.bus_bytes_per_cycle") c.dram.bus_bytes_per_cycle = to_uint(key, v);
  else throw ConfigError("unknown config key `" + key + "`");
}

void validate(const Config& c) {
  check(c.mul_latency >= 1 && c.div_latency >= 1, "core.mul_latency and core.div_latency must be >= 1");
  // Checked even for static_nt: an impossible value in a config file is an
  // error regardless of which predictor is selected.
  check(pow2(c.bpred.entries), "bpred.entries must be a power of two");
  if (c.bpred.type == bpred::Type::Gshare) {
    check(c.bpred.history_bits >= 1 && c.bpred.history_bits <= log2u(c.bpred.entries),
          "bpred.history_bits must be between 1 and log2(bpred.entries)");
  }
  check(c.bpred.btb_entries == 0 || pow2(c.bpred.btb_entries), "btb.entries must be 0 or a power of two");
  check(pow2(c.dram.banks), "dram.banks must be a power of two");
  check(pow2(c.dram.row_bytes) && c.dram.row_bytes >= 64, "dram.row_bytes must be a power of two >= 64");
  check(pow2(c.dram.bus_bytes_per_cycle), "dram.bus_bytes_per_cycle must be a power of two");
  validate_cache("icache", c.icache, c.dram);
  validate_cache("dcache", c.dcache, c.dram);
}

Config load_config(const std::string& path, const std::vector<std::string>& overrides) {
  Config c;
  if (!path.empty()) {
    std::ifstream in(path);
    if (!in) throw ConfigError("cannot open config file " + path);
    std::stringstream ss;
    ss << in.rdbuf();
    for (const auto& [k, v] : parse_kv(ss.str(), path)) apply_setting(c, k, v);
  }
  for (const auto& o : overrides) {
    const auto [k, v] = parse_override(o);
    apply_setting(c, k, v);
  }
  validate(c);
  return c;
}

std::string describe(const Config& c) {
  std::ostringstream o;
  auto b = [](bool x) { return x ? "true" : "false"; };
  o << "core.mul_latency = " << c.mul_latency << "\n"
    << "core.div_latency = " << c.div_latency << "\n"
    << "bpred.type = " << bpred::to_string(c.bpred.type) << "\n"
    << "bpred.entries = " << c.bpred.entries << "\n"
    << "bpred.history_bits = " << c.bpred.history_bits << "\n"
    << "btb.entries = " << c.bpred.btb_entries << "\n";
  for (const auto& [name, cc] : {std::pair<const char*, const cache::Config&>{"icache", c.icache},
                                 std::pair<const char*, const cache::Config&>{"dcache", c.dcache}}) {
    o << name << ".enabled = " << b(cc.enabled) << "\n"
      << name << ".size_bytes = " << cc.size_bytes << "\n"
      << name << ".assoc = " << cc.assoc << "\n"
      << name << ".line_bytes = " << cc.line_bytes << "\n";
  }
  o << "dram.banks = " << c.dram.banks << "\n"
    << "dram.row_bytes = " << c.dram.row_bytes << "\n"
    << "dram.row_hit_cycles = " << c.dram.row_hit_cycles << "\n"
    << "dram.row_closed_cycles = " << c.dram.row_closed_cycles << "\n"
    << "dram.row_miss_cycles = " << c.dram.row_miss_cycles << "\n"
    << "dram.bus_bytes_per_cycle = " << c.dram.bus_bytes_per_cycle << "\n";
  return o.str();
}

}  // namespace rvx::pipe
