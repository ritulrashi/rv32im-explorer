/* GEMM, naive i-j-k: C[i][j] = sum_k A[i][k] * B[k][j]. Row-major int32.
 * B is walked down a column: stride N*4 bytes per k step.
 * Build with -DN=<size> -DLOG2N=<log2 size>. */
#include "rvx_workload.h"

  .section .text.init
  .globl _start
_start:
  la s0, mat_a
  la s1, mat_b
  la s2, mat_c
  li s5, N
  ROI_BEGIN
  li s3, 0                      /* i */
i_loop:
  li s4, 0                      /* j */
j_loop:
  slli t0, s3, LOG2N + 2
  add a0, s0, t0                /* a0 = &A[i][0] */
  slli t1, s4, 2
  add a1, s1, t1                /* a1 = &B[0][j] */
  addi a2, a0, N * 4            /* end of row i of A */
  li a3, 0                      /* accumulator */
k_loop:
  lw t2, 0(a0)
  lw t3, 0(a1)
  mul t2, t2, t3
  add a3, a3, t2
  addi a0, a0, 4
  addi a1, a1, N * 4
  bne a0, a2, k_loop
  slli t0, s3, LOG2N + 2
  add t0, s2, t0
  slli t1, s4, 2
  add t0, t0, t1
  sw a3, 0(t0)                  /* C[i][j] */
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
