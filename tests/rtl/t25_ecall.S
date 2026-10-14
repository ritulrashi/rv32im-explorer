# ECALL halts.
# expect: halt=ecall retired=0 cycles=5
  .text
  .globl _start
_start:
  ecall
  sw x0, 0x7f0(x0)       # END: tohost store
