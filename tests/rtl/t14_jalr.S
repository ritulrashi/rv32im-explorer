# JALR with forwarded base: +2.
# expect: halt=tohost retired=3 cycles=9
  .text
  .globl _start
_start:
  addi x5, x0, 12
  jalr x1, 0(x5)
  addi x9, x0, 1
  sw x0, 0x7f0(x0)       # END: tohost store
