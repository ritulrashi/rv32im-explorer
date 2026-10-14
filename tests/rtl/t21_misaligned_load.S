# Misaligned load halts; the younger store never writes.
# expect: halt=misaligned-load retired=2 cycles=7
  .text
  .globl _start
_start:
  addi x5, x0, 0x401
  addi x7, x0, 99
  lw x6, 0(x5)
  sw x7, 0x500(x0)
  sw x0, 0x7f0(x0)       # END: tohost store
