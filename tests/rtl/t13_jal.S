# JAL: +2.
# expect: halt=tohost retired=2 cycles=8
  .text
  .globl _start
_start:
  jal x1, 1f
  addi x9, x0, 1
1:
  sw x0, 0x7f0(x0)       # END: tohost store
