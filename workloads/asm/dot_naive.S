/* dot product, naive: one multiply-accumulate per iteration.
 * Build with -DN=<elements>. Result checked against dot_expected. */
#include "rvx_workload.h"

  .section .text.init
  .globl _start
_start:
  la a0, vec_a
  la a1, vec_b
  li t3, N * 4
  add a2, a0, t3          /* a2 = end of vec_a */
  li a3, 0                /* accumulator */
  ROI_BEGIN
loop:
  lw t0, 0(a0)
  lw t1, 0(a1)
  mul t0, t0, t1          /* uses t1 loaded by the previous instruction: load-use */
  add a3, a3, t0
  addi a0, a0, 4
  addi a1, a1, 4
  bne a0, a2, loop
  ROI_END
  la t0, dot_expected
  lw t0, 0(t0)
  bne a3, t0, fail
  PASS
fail:
  FAIL
