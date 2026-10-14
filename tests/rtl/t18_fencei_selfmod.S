# FENCE.I makes a preceding store visible to fetch: load-use +1, fence.i +2.
# The store rewrites the instruction at 0x10 to addi x9, x0, 5 (0x00500493).
# expect: halt=tohost retired=6 cycles=13
  .text
  .globl _start
_start:
  lw x5, 0x400(x0)
  sw x5, 0x10(x0)
  fence.i
  nop
  addi x9, x0, 1
  sw x0, 0x7f0(x0)       # END: tohost store
  .data
  .word 0x00500493
