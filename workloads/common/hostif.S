/* Host interface words, each on its own 64-byte line so that touching one
 * never evicts or shares a cache line with workload data.
 *   tohost   1 = pass, other odd values = fail (as in riscv-tests)
 *   rvx_roi  1 = region of interest begins, 2 = ends (docs/phase4-design.md) */
  .section .tohost, "aw", @progbits
  .align 6
  .globl tohost
tohost: .dword 0
  .align 6
  .globl fromhost
fromhost: .dword 0
  .align 6
  .globl rvx_roi
rvx_roi: .word 0
  .align 6
