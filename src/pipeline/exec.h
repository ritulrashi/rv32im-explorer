// Execution semantics for the pipeline model.
//
// Rule 4: written independently of src/iss. Where a natural alternative
// formulation exists, this file uses it, so the two models are unlikely to
// share a mistake:
//   * signed compare by flipping sign bits, then comparing unsigned;
//   * arithmetic shift right built from a logical shift plus a sign mask;
//   * MULH/MULHSU derived from the unsigned high product with sign corrections;
//   * DIV/REM via a restoring shift-subtract divider (as hardware does it),
//     not the C++ '/' and '%' operators.
#pragma once

#include <cstdint>

#include "common/decode.h"

namespace rvx::pipe::exec {

bool is_load(Op op);
bool is_store(Op op);
bool is_branch(Op op);
bool is_mul(Op op);  // MUL, MULH, MULHSU, MULHU
bool is_div(Op op);  // DIV, DIVU, REM, REMU

unsigned access_size(Op op);  // loads and stores only: 1, 2 or 4

// Register-register or register-immediate ALU op. `b` is rs2 or the immediate.
uint32_t alu(Op op, uint32_t a, uint32_t b);
bool branch_taken(Op op, uint32_t a, uint32_t b);
uint32_t muldiv(Op op, uint32_t a, uint32_t b);
uint32_t extend_load(Op op, uint32_t raw);
uint32_t store_mask(unsigned size);

}  // namespace rvx::pipe::exec
