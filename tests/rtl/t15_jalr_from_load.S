# JALR fed by a load: load-use +1, jump +2.
# expect: halt=tohost retired=3 cycles=10
  .text
  .globl _start
_start:
  lw x5, 0x400(x0)
  jalr x0, 0(x5)
  addi x9, x0, 1
  sw x0, 0x7f0(x0)       # END: tohost store
  .data
  .word 12
