# Producer in WB while consumer in ID: register-file write-through (P2 §3.1).
# expect: halt=tohost retired=5 cycles=9
  .text
  .globl _start
_start:
  addi x1, x0, 5
  nop
  nop
  add x2, x1, x1
  sw x0, 0x7f0(x0)       # END: tohost store
