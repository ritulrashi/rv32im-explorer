// JSON statistics for one pipeline run (docs/phase3-design.md §5).
//
// Only integer counts are emitted; ratios (CPI, hit rate, accuracy) are left
// to the consumer, so the output is exact and byte-identical across runs.
#pragma once

#include <string>

#include "pipeline/pipeline.h"

namespace rvx::pipe {

std::string stats_json(const Pipeline& p, const std::string& program);

}  // namespace rvx::pipe
