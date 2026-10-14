#include "bpred/predictor.h"

namespace rvx::bpred {

std::string_view to_string(Type t) {
  switch (t) {
    case Type::StaticNT: return "static_nt";
    case Type::Bimodal: return "bimodal";
    case Type::Gshare: return "gshare";
  }
  return "?";
}

DirectionPredictor::DirectionPredictor(const Config& cfg) : type_(cfg.type) {
  if (type_ == Type::StaticNT) return;
  table_.assign(cfg.entries, 1);  // weakly not-taken
  mask_ = cfg.entries - 1;
  if (type_ == Type::Gshare) ghr_mask_ = (1u << cfg.history_bits) - 1;
}

uint32_t DirectionPredictor::index(uint32_t pc) const {
  if (type_ == Type::StaticNT) return 0;
  const uint32_t base = pc >> 2;
  return (type_ == Type::Gshare ? (base ^ ghr_) : base) & mask_;
}

bool DirectionPredictor::predict_at(uint32_t index) const {
  if (type_ == Type::StaticNT) return false;
  return table_[index] >= 2;
}

void DirectionPredictor::train(uint32_t index, bool taken) {
  if (type_ == Type::StaticNT) return;
  uint8_t& c = table_[index];
  if (taken && c < 3) ++c;
  if (!taken && c > 0) --c;
  if (type_ == Type::Gshare) ghr_ = ((ghr_ << 1) | (taken ? 1u : 0u)) & ghr_mask_;
}

Btb::Btb(unsigned entries) {
  if (entries == 0) return;
  slots_.resize(entries);
  mask_ = entries - 1;
}

std::optional<Btb::Entry> Btb::lookup(uint32_t pc) const {
  if (slots_.empty()) return std::nullopt;
  const Slot& s = slots_[(pc >> 2) & mask_];
  if (!s.valid || s.tag != pc) return std::nullopt;
  return s.e;
}

void Btb::update(uint32_t pc, uint32_t target, BranchKind kind) {
  if (slots_.empty()) return;
  Slot& s = slots_[(pc >> 2) & mask_];
  s.valid = true;
  s.tag = pc;
  s.e = {target, kind};
}

}  // namespace rvx::bpred
