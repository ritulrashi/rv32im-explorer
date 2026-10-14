# Dependent chain: EX/MEM forwarding, no stall (P2 §6.1).
# expect: halt=tohost retired=11 cycles=15
  .text
  .globl _start
_start:
  addi x1, x0, 1
  addi x1, x1, 1
  addi x1, x1, 1
  addi x1, x1, 1
  addi x1, x1, 1
  addi x1, x1, 1
  addi x1, x1, 1
  addi x1, x1, 1
  addi x1, x1, 1
  addi x1, x1, 1
  sw x0, 0x7f0(x0)       # END: tohost store
