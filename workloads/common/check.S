/* rvx_checksum(a0 = pointer, a1 = word count) -> a0
 * Order-sensitive hash: h = h * 31 + x (mod 2^32), the same function as
 * workloads/gen_data.py and workloads/common/roi.h. Leaf: no stack. */
  .text
  .globl rvx_checksum
rvx_checksum:
  li t0, 0
  li t2, 31
1:
  lw t1, 0(a0)
  mul t0, t0, t2
  add t0, t0, t1
  addi a0, a0, 4
  addi a1, a1, -1
  bnez a1, 1b
  mv a0, t0
  ret
