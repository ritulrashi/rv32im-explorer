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

}  // namespace

std::string stats_json(const Pipeline& p, const std::string& program) {
  std::ostringstream o;
  const PipeStats& s = p.stats();
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
  o << "  \"cycles\": " << p.cycles() << ",\n";
  o << "  \"retired\": " << p.retired() << ",\n";

  o << "  \"cpi_stack_cycles\": {";
  for (size_t k = 0; k < kCpiCats; ++k) {
    o << (k ? ", " : "") << quote(std::string(to_string(static_cast<CpiCat>(k)))) << ": " << s.cpi[k];
  }
  o << "},\n";
  o << "  \"branch_cycles\": {\"mispredict\": " << s.branch_mispredict_cycles
    << ", \"id_redirect\": " << s.branch_id_redirect_cycles << ", \"fence_i\": " << s.branch_fence_i_cycles
    << "},\n";
  o << "  \"branch_events\": {\"cond_branches\": " << s.cond_branches
    << ", \"cond_mispredicts\": " << s.cond_mispredicts << ", \"jumps\": " << s.jumps
    << ", \"jump_mispredicts\": " << s.jump_mispredicts << ", \"btb_hits\": " << s.btb_hits
    << ", \"id_redirects\": " << s.id_redirects << "},\n";

  auto cache = [&](const char* name, const cache::Cache* c) {
    o << "  " << quote(name) << ": ";
    if (!c) {
      o << "null,\n";
      return;
    }
    o << "{\"accesses\": " << c->stats().accesses << ", \"misses\": " << c->stats().misses
      << ", \"writebacks\": " << c->stats().writebacks << "},\n";
  };
  cache("icache", p.mem_system().icache());
  cache("dcache", p.mem_system().dcache());
  const auto& d = p.mem_system().dram().stats();
  o << "  \"dram\": {\"row_hits\": " << d.row_hits << ", \"row_closed\": " << d.row_closed
    << ", \"row_misses\": " << d.row_misses << "}\n";
  o << "}\n";
  return o.str();
}

}  // namespace rvx::pipe
