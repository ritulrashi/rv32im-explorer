# Load then use two later: no stall.
# expect: halt=tohost retired=4 cycles=8
  .text
  .globl _start
_start:
  lw x1, 0x400(x0)
  nop
  add x3, x1, x1
  sw x0, 0x7f0(x0)       # END: tohost store
  .data
  .word 21
