// Stable text format for retire records (see docs/phase1-design.md):
//
//   <seq> pc=<hex8> insn=<hex8> rd=<xNN|-> val=<hex8|-> mem=<R|W|-> addr=<hex8|-> data=<hex8|-> sz=<1|2|4|->
//
// Fixed field order, lowercase hex, '-' for absent fields. Two models that
// agree architecturally always produce byte-identical lines.
#pragma once

#include <ostream>
#include <string>

#include "common/retire.h"

namespace rvx {

std::string format_retire(const RetireRecord& r);

class TraceWriter {
 public:
  explicit TraceWriter(std::ostream& os) : os_(os) {}
  void write(const RetireRecord& r) { os_ << format_retire(r) << '\n'; }

 private:
  std::ostream& os_;
};

}  // namespace rvx
