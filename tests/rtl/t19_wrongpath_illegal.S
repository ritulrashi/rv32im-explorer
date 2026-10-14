# Illegal word on the wrong path is squashed (P2 §6.6).
# expect: halt=tohost retired=2 cycles=8
  .text
  .globl _start
_start:
  beq x0, x0, 1f
  .word 0
1:
  sw x0, 0x7f0(x0)       # END: tohost store
