# EX/MEM wins over MEM/WB (P2 §6.1).
# expect: halt=tohost retired=4 cycles=8
  .text
  .globl _start
_start:
  addi x1, x0, 1
  addi x1, x0, 2
  add x2, x1, x1
  sw x0, 0x7f0(x0)       # END: tohost store
