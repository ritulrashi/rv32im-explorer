# Load then store using it as the base: +1.
# expect: halt=tohost retired=3 cycles=8
  .text
  .globl _start
_start:
  lw x1, 0x400(x0)
  sw x0, 0(x1)
  sw x0, 0x7f0(x0)       # END: tohost store
  .data
  .word 0x408
