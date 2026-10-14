# Illegal word on the right path halts without retiring.
# expect: halt=illegal-instruction retired=1 cycles=6
  .text
  .globl _start
_start:
  addi x1, x0, 1
  .word 0
  sw x0, 0x7f0(x0)       # END: tohost store
