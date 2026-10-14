# Load to x0 never stalls.
# expect: halt=tohost retired=3 cycles=7
  .text
  .globl _start
_start:
  lw x0, 0x400(x0)
  add x3, x0, x0
  sw x0, 0x7f0(x0)       # END: tohost store
