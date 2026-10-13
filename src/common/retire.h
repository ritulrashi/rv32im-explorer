// One retired instruction, as observed architecturally.
//
// This record (and its text form in trace.h) is the contract for lockstep
// comparison between models. It contains only architectural facts, never
// anything timing-related.
#pragma once

#include <cstdint>
#include <string_view>

namespace rvx {

// Why a run ended. Part of the comparison contract: lockstep requires both
// models to stop for the same reason, at the same point.
enum class HaltReason : uint8_t {
  None,             // still running
  ToHost,           // program stored to the tohost address (normal end)
  IllegalInsn,
  MisalignedFetch,  // jump/branch target not 4-byte aligned, or misaligned pc
  MisalignedLoad,
  MisalignedStore,
  Ecall,
  Ebreak,
  MaxInsns,         // ISS instruction budget exhausted
  MaxCycles,        // pipeline cycle budget exhausted
};

std::string_view to_string(HaltReason r);

enum class MemKind : uint8_t { None, Read, Write };

struct RetireRecord {
  uint64_t seq = 0;   // retirement order, starting at 0
  uint32_t pc = 0;
  uint32_t insn = 0;  // raw instruction word
  uint8_t rd = 0;     // 0 means "no register write" (incl. writes to x0)
  uint32_t rd_val = 0;
  MemKind mem = MemKind::None;
  uint32_t mem_addr = 0;
  // Loads: raw bytes read, zero-extended (before any sign extension).
  // Stores: value written, masked to mem_size bytes.
  uint32_t mem_data = 0;
  uint8_t mem_size = 0;  // 1, 2 or 4 when mem != None

  bool operator==(const RetireRecord&) const = default;
};

}  // namespace rvx
