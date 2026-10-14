/* dot product, unrolled x4 with four independent accumulators.
 * All eight loads are issued before the first multiply, so no multiply
 * waits on the load right before it. N must be a multiple of 4. */
#include "rvx_workload.h"

  .section .text.init
  .globl _start
_start:
  la a0, vec_a
  la a1, vec_b
  li t3, N * 4
  add a2, a0, t3
  li a3, 0
  li a4, 0
  li a5, 0
  li a6, 0
  ROI_BEGIN
loop:
  lw t0, 0(a0)
  lw t1, 0(a1)
  lw t2, 4(a0)
  lw t3, 4(a1)
  lw t4, 8(a0)
  lw t5, 8(a1)
  lw t6, 12(a0)
  lw s1, 12(a1)
  mul t0, t0, t1
  mul t2, t2, t3
  mul t4, t4, t5
  mul t6, t6, s1
  add a3, a3, t0
  add a4, a4, t2
  add a5, a5, t4
  add a6, a6, t6
  addi a0, a0, 16
  addi a1, a1, 16
  bne a0, a2, loop
  ROI_END
  add a3, a3, a4
  add a5, a5, a6
  add a3, a3, a5
  la t0, dot_expected
  lw t0, 0(t0)
  bne a3, t0, fail
  PASS
fail:
  FAIL
