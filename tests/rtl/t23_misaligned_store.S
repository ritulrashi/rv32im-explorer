# Misaligned store halts and does not write.
# expect: halt=misaligned-store retired=2 cycles=7
  .text
  .globl _start
_start:
  addi x5, x0, 0x402
  addi x7, x0, -1
  sw x7, 0(x5)
  sw x0, 0x7f0(x0)       # END: tohost store
