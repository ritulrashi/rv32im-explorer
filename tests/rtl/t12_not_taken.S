# Not-taken branch: free.
# expect: halt=tohost retired=3 cycles=7
  .text
  .globl _start
_start:
  bne x0, x0, 1f
  addi x9, x0, 1
1:
  sw x0, 0x7f0(x0)       # END: tohost store
