# FENCE.I: +2 (P2 §6.5).
# expect: halt=tohost retired=2 cycles=8
  .text
  .globl _start
_start:
  fence.i
  sw x0, 0x7f0(x0)       # END: tohost store
