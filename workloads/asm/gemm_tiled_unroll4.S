/* GEMM, tiled (T = 8) with the innermost k loop unrolled x4
 * (two unrolled iterations per tile row). */
#include "rvx_workload.h"
#define T 8

  .section .text.init
  .globl _start
_start:
  la s0, mat_a
  la s1, mat_b
  la s2, mat_c
  li s9, N
  ROI_BEGIN
  li s3, 0
ii_loop:
  li s4, 0
jj_loop:
  li s5, 0
kk_loop:
  mv s6, s3
i_loop:
  mv s7, s4
j_loop:
  slli t0, s6, LOG2N + 2
  add t0, s2, t0
  slli t1, s7, 2
  add a4, t0, t1
  lw a3, 0(a4)
  slli t0, s6, LOG2N + 2
  add t0, s0, t0
  slli t1, s5, 2
  add a0, t0, t1
  addi a2, a0, T * 4
  slli t0, s5, LOG2N + 2
  add t0, s1, t0
  slli t1, s7, 2
  add a1, t0, t1
k_loop:
  lw t2, 0(a0)
  lw t3, 0(a1)
  lw t4, 4(a0)
  lw t5, N * 4(a1)
  lw t6, 8(a0)
  lw s8, 2 * N * 4(a1)
  lw s10, 12(a0)
  lw s11, 3 * N * 4(a1)
  mul t2, t2, t3
  mul t4, t4, t5
  mul t6, t6, s8
  mul s10, s10, s11
  add a3, a3, t2
  add a3, a3, t4
  add a3, a3, t6
  add a3, a3, s10
  addi a0, a0, 16
  addi a1, a1, 4 * N * 4
  bne a0, a2, k_loop
  sw a3, 0(a4)
  addi s7, s7, 1
  addi t0, s4, T
  bne s7, t0, j_loop
  addi s6, s6, 1
  addi t0, s3, T
  bne s6, t0, i_loop
  addi s5, s5, T
  bne s5, s9, kk_loop
  addi s4, s4, T
  bne s4, s9, jj_loop
  addi s3, s3, T
  bne s3, s9, ii_loop
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
