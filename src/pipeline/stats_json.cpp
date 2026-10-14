#include "pipeline/stats_json.h"

#include <sstream>

#include "common/kvfile.h"

namespace rvx::pipe {
namespace {

std::string quote(const std::string& s) {
  std::string o = "\"";
  for (char ch : s) {
    if (ch == '"' || ch == '\\') o += '\\';
    o += ch;
  }
  return o + "\"";
}

bool is_number(const std::string& s) {
  if (s.empty()) return false;
  for (char ch : s) {
    if (ch < '0' || ch > '9') return false;
  }
  return true;
}

// One block of counters (a whole run, or a region of interest), as JSON
// members indented by `ind`. `with_caches` controls whether cache counters are
// emitted as objects or null (a disabled cache has no counters).
void counters(std::ostringstream& o, const CounterSnapshot& s, bool icache, bool dcache, const char* ind) {
  const PipeStats& p = s.pipe;
  o << ind << "\"cycles\": " << s.cycles << ",\n";
  o << ind << "\"retired\": " << s.retired << ",\n";
  o << ind << "\"cpi_stack_cycles\": {";
  for (size_t k = 0; k < kCpiCats; ++k) {
    o << (k ? ", " : "") << quote(std::string(to_string(static_cast<CpiCat>(k)))) << ": " << p.cpi[k];
  }
  o << "},\n";
  o << ind << "\"branch_cycles\": {\"mispredict\": " << p.branch_mispredict_cycles
    << ", \"id_redirect\": " << p.branch_id_redirect_cycles << ", \"fence_i\": " << p.branch_fence_i_cycles
    << "},\n";
  o << ind << "\"branch_events\": {\"cond_branches\": " << p.cond_branches
    << ", \"cond_mispredicts\": " << p.cond_mispredicts << ", \"jumps\": " << p.jumps
    << ", \"jump_mispredicts\": " << p.jump_mispredicts << ", \"btb_hits\": " << p.btb_hits
    << ", \"id_redirects\": " << p.id_redirects << "},\n";
  o << ind << "\"memory_ops\": {\"loads\": " << p.loads << ", \"stores\": " << p.stores << "},\n";
  auto cache = [&](const char* name, bool enabled, const cache::CacheStats& c) {
    o << ind << quote(name) << ": ";
    if (!enabled) {
      o << "null,\n";
      return;
    }
    o << "{\"accesses\": " << c.accesses << ", \"misses\": " << c.misses << ", \"writebacks\": " << c.writebacks
      << "},\n";
  };
  cache("icache", icache, s.icache);
  cache("dcache", dcache, s.dcache);
  o << ind << "\"dram\": {\"row_hits\": " << s.dram.row_hits << ", \"row_closed\": " << s.dram.row_closed
    << ", \"row_misses\": " << s.dram.row_misses << "}";
}

}  // namespace

std::string stats_json(const Pipeline& p, const std::string& program) {
  std::ostringstream o;
  const bool ic = p.mem_system().icache() != nullptr;
  const bool dc = p.mem_system().dcache() != nullptr;
  o << "{\n";
  o << "  \"program\": " << quote(program) << ",\n";

  o << "  \"config\": {";
  bool first = true;
  for (const auto& [k, v] : parse_kv(describe(p.config()), "config")) {
    o << (first ? "\n" : ",\n") << "    " << quote(k) << ": "
      << (is_number(v) || v == "true" || v == "false" ? v : quote(v));
    first = false;
  }
  o << "\n  },\n";

  o << "  \"halt\": " << quote(std::string(to_string(p.halt().reason))) << ",\n";
  counters(o, p.snapshot(), ic, dc, "  ");
  o << ",\n  \"roi\": ";
  if (p.roi().complete()) {
    o << "{\n";
    counters(o, p.roi().delta(), ic, dc, "    ");
    o << "\n  }\n";
  } else {
    o << "null\n";
  }
  o << "}\n";
  return o.str();
}

}  // namespace rvx::pipe
