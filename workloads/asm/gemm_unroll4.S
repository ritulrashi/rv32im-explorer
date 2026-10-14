/* GEMM, naive i-j-k with the k loop unrolled x4 (one accumulator).
 * All eight loads come before the multiplies, so no load-use stalls.
 * N must be a multiple of 4. */
#include "rvx_workload.h"

  .section .text.init
  .globl _start
_start:
  la s0, mat_a
  la s1, mat_b
  la s2, mat_c
  li s5, N
  ROI_BEGIN
  li s3, 0
i_loop:
  li s4, 0
j_loop:
  slli t0, s3, LOG2N + 2
  add a0, s0, t0
  slli t1, s4, 2
  add a1, s1, t1
  addi a2, a0, N * 4
  li a3, 0
k_loop:
  lw t2, 0(a0)
  lw t3, 0(a1)
  lw t4, 4(a0)
  lw t5, N * 4(a1)
  lw t6, 8(a0)
  lw s8, 2 * N * 4(a1)
  lw s9, 12(a0)
  lw s10, 3 * N * 4(a1)
  mul t2, t2, t3
  mul t4, t4, t5
  mul t6, t6, s8
  mul s9, s9, s10
  add a3, a3, t2
  add a3, a3, t4
  add a3, a3, t6
  add a3, a3, s9
  addi a0, a0, 16
  addi a1, a1, 4 * N * 4
  bne a0, a2, k_loop
  slli t0, s3, LOG2N + 2
  add t0, s2, t0
  slli t1, s4, 2
  add t0, t0, t1
  sw a3, 0(t0)
  addi s4, s4, 1
  bne s4, s5, j_loop
  addi s3, s3, 1
  bne s3, s5, i_loop
  ROI_END
  la a0, mat_c
  li a1, N * N
  call rvx_checksum
  la t0, gemm_expected
  lw t0, 0(t0)
  bne a0, t0, fail
  PASS
fail:
  FAIL
