#include "common/trace.h"

#include <cstdio>

namespace rvx {

std::string format_retire(const RetireRecord& r) {
  char rd[8] = "-";
  char val[12] = "-";
  if (r.rd != 0) {
    std::snprintf(rd, sizeof rd, "x%02u", static_cast<unsigned>(r.rd));
    std::snprintf(val, sizeof val, "%08x", r.rd_val);
  }
  char mem[2] = "-";
  char addr[12] = "-";
  char data[12] = "-";
  char sz[4] = "-";
  if (r.mem != MemKind::None) {
    mem[0] = r.mem == MemKind::Read ? 'R' : 'W';
    std::snprintf(addr, sizeof addr, "%08x", r.mem_addr);
    std::snprintf(data, sizeof data, "%08x", r.mem_data);
    std::snprintf(sz, sizeof sz, "%u", static_cast<unsigned>(r.mem_size));
  }
  char line[160];
  std::snprintf(line, sizeof line, "%llu pc=%08x insn=%08x rd=%s val=%s mem=%s addr=%s data=%s sz=%s",
                static_cast<unsigned long long>(r.seq), r.pc, r.insn, rd, val, mem, addr, data, sz);
  return line;
}

std::string_view to_string(HaltReason r) {
  switch (r) {
    case HaltReason::None: return "running";
    case HaltReason::ToHost: return "tohost";
    case HaltReason::IllegalInsn: return "illegal-instruction";
    case HaltReason::MisalignedFetch: return "misaligned-fetch";
    case HaltReason::MisalignedLoad: return "misaligned-load";
    case HaltReason::MisalignedStore: return "misaligned-store";
    case HaltReason::Ecall: return "ecall";
    case HaltReason::Ebreak: return "ebreak";
    case HaltReason::MaxInsns: return "max-insns";
    case HaltReason::MaxCycles: return "max-cycles";
  }
  return "?";
}

}  // namespace rvx
