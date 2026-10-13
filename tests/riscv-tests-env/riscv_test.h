// Minimal riscv-tests environment for rv32im-explorer.
//
// Replaces upstream riscv-test-env/p/riscv_test.h (pinned commit 6de71ed).
// The official test bodies (isa/rv32ui/*.S, isa/rv32um/*.S, and the
// isa/macros/scalar/test_macros.h they use) are compiled UNMODIFIED against
// this header.
//
// Differences from upstream, and why:
//  * No machine-mode setup. Upstream writes mtvec, satp, pmpaddr0, medeleg,
//    mstatus and enters the test via mret. Our models have no CSRs or traps.
//  * No trap vector. Upstream's trap_vector reports ecall/exceptions; here
//    every exception halts the simulator directly with a distinct exit code.
//  * Pass/fail store directly to `tohost` instead of executing ecall, which
//    upstream's trap handler then turns into the same tohost store. The value
//    written is identical: 1 for pass, (TESTNUM << 1) | 1 for fail.
//  * INIT_XREG (zero all registers) and `li TESTNUM, 0` are kept as upstream.
//  * CHECK_XLEN is dropped: we build for rv32 only.
//
// The linker script (link.ld) is identical to upstream p/link.ld.
#ifndef RVX_RISCV_TEST_H
#define RVX_RISCV_TEST_H

#define RVTEST_RV64U \
  .macro init;       \
  .endm

#define RVTEST_RV32U \
  .macro init;       \
  .endm

#define TESTNUM gp

#define INIT_XREG \
  li x1, 0;       \
  li x2, 0;       \
  li x3, 0;       \
  li x4, 0;       \
  li x5, 0;       \
  li x6, 0;       \
  li x7, 0;       \
  li x8, 0;       \
  li x9, 0;       \
  li x10, 0;      \
  li x11, 0;      \
  li x12, 0;      \
  li x13, 0;      \
  li x14, 0;      \
  li x15, 0;      \
  li x16, 0;      \
  li x17, 0;      \
  li x18, 0;      \
  li x19, 0;      \
  li x20, 0;      \
  li x21, 0;      \
  li x22, 0;      \
  li x23, 0;      \
  li x24, 0;      \
  li x25, 0;      \
  li x26, 0;      \
  li x27, 0;      \
  li x28, 0;      \
  li x29, 0;      \
  li x30, 0;      \
  li x31, 0;

#define RVTEST_CODE_BEGIN  \
  .section .text.init;     \
  .align 6;                \
  .globl _start;           \
  _start:                  \
  INIT_XREG;               \
  li TESTNUM, 0;           \
  init;

#define RVTEST_CODE_END unimp

// `sw TESTNUM, tohost, t5` is the assembler's PC-relative store pseudo-op
// (auipc t5 + sw). The simulator halts as soon as that store retires; the
// `j .` spin after it is only a safety net and never executes in simulation.
//
// IMPORTANT: these macros must not define numeric local labels beyond the
// `1:` that upstream's RVTEST_FAIL already has. Test bodies use forward
// references like `2f` that would otherwise bind to a label inside these
// macros (see docs/BUGS.md #1). `j .` jumps to itself without a label.
#define RVTEST_PASS        \
  fence;                   \
  li TESTNUM, 1;           \
  sw TESTNUM, tohost, t5;  \
  j .;

#define RVTEST_FAIL              \
  fence;                         \
  1: beqz TESTNUM, 1b;           \
  sll TESTNUM, TESTNUM, 1;       \
  or TESTNUM, TESTNUM, 1;        \
  sw TESTNUM, tohost, t5;        \
  j .;

#define EXTRA_DATA

#define RVTEST_DATA_BEGIN                                               \
  EXTRA_DATA                                                            \
  .pushsection .tohost, "aw", @progbits;                                \
  .align 6; .global tohost; tohost: .dword 0; .size tohost, 8;          \
  .align 6; .global fromhost; fromhost: .dword 0; .size fromhost, 8;    \
  .popsection;                                                          \
  .align 4; .global begin_signature; begin_signature:

#define RVTEST_DATA_END .align 4; .global end_signature; end_signature:

#endif
