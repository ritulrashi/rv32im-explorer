// Branch prediction structures (docs/phase3-design.md §2). Timing-only: they
// decide which pc is fetched next, never what an instruction computes.
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace rvx::bpred {

enum class Type : uint8_t { StaticNT, Bimodal, Gshare };
std::string_view to_string(Type t);

struct Config {
  Type type = Type::StaticNT;
  unsigned entries = 512;      // 2-bit counters; power of two
  unsigned history_bits = 8;   // gshare only; <= log2(entries)
  unsigned btb_entries = 0;    // 0 = no BTB; otherwise power of two
};

// Direction predictor: a table of 2-bit saturating counters, indexed by pc
// (bimodal) or pc XOR global history (gshare). StaticNT always says not taken.
//
// The pipeline reads index(pc) at fetch and carries that index to EX, where
// train(index, taken) updates exactly the counter that made the prediction
// (the history may have moved on in between; see design note §2.1).
class DirectionPredictor {
 public:
  explicit DirectionPredictor(const Config& cfg);

  uint32_t index(uint32_t pc) const;
  bool predict_at(uint32_t index) const;
  bool predict(uint32_t pc) const { return predict_at(index(pc)); }
  // Updates the counter at `index`; for gshare also shifts `taken` into the
  // global history.
  void train(uint32_t index, bool taken);

  uint8_t counter(uint32_t index) const { return table_.empty() ? 0 : table_[index]; }
  uint32_t history() const { return ghr_; }
  Type type() const { return type_; }

 private:
  Type type_;
  std::vector<uint8_t> table_;  // values 0..3; 0,1 = not taken, 2,3 = taken
  uint32_t mask_ = 0;           // entries - 1
  uint32_t ghr_ = 0;
  uint32_t ghr_mask_ = 0;
};

enum class BranchKind : uint8_t { Cond, Jal, Jalr };

// Branch target buffer: direct-mapped, full pc tags (so no aliasing between
// different branches), entries allocated when a branch is taken or a jump
// executes.
class Btb {
 public:
  explicit Btb(unsigned entries);

  struct Entry {
    uint32_t target = 0;
    BranchKind kind = BranchKind::Cond;
  };
  std::optional<Entry> lookup(uint32_t pc) const;
  void update(uint32_t pc, uint32_t target, BranchKind kind);
  bool enabled() const { return !slots_.empty(); }

 private:
  struct Slot {
    bool valid = false;
    uint32_t tag = 0;  // the full pc
    Entry e;
  };
  std::vector<Slot> slots_;
  uint32_t mask_ = 0;
};

}  // namespace rvx::bpred
