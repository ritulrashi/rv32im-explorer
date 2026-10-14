# A faulting load does not cause a load-use stall (P2 §6.2).
# expect: halt=misaligned-load retired=1 cycles=6
  .text
  .globl _start
_start:
  addi x5, x0, 0x402
  lw x6, 0(x5)
  add x7, x6, x6
  sw x0, 0x7f0(x0)       # END: tohost store
