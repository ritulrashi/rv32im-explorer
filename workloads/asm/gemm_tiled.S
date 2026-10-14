/* GEMM, tiled (blocked) with T x T tiles, T = 8:
 *   for ii, jj, kk (step T):  for i in tile, j in tile:
 *       C[i][j] += sum_{k in kk..kk+T} A[i][k] * B[k][j]
 * One T x T tile of each matrix is 256 bytes, so three tiles fit in a 1 KiB
 * cache. Same i-j-k order inside the tile as the naive kernel. */
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
  li s3, 0                      /* ii */
ii_loop:
  li s4, 0                      /* jj */
jj_loop:
  li s5, 0                      /* kk */
kk_loop:
  mv s6, s3                     /* i = ii */
i_loop:
  mv s7, s4                     /* j = jj */
j_loop:
  slli t0, s6, LOG2N + 2
  add t0, s2, t0
  slli t1, s7, 2
  add a4, t0, t1                /* a4 = &C[i][j] */
  lw a3, 0(a4)                  /* accumulator starts from C */
  slli t0, s6, LOG2N + 2
  add t0, s0, t0
  slli t1, s5, 2
  add a0, t0, t1                /* a0 = &A[i][kk] */
  addi a2, a0, T * 4
  slli t0, s5, LOG2N + 2
  add t0, s1, t0
  slli t1, s7, 2
  add a1, t0, t1                /* a1 = &B[kk][j] */
k_loop:
  lw t2, 0(a0)
  lw t3, 0(a1)
  mul t2, t2, t3
  add a3, a3, t2
  addi a0, a0, 4
  addi a1, a1, N * 4
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
