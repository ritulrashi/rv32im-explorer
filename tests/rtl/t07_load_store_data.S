# Load then store of the loaded value: +1 (no MEM->MEM forwarding).
# expect: halt=tohost retired=3 cycles=8
  .text
  .globl _start
_start:
  lw x1, 0x400(x0)
  sw x1, 0x404(x0)
  sw x0, 0x7f0(x0)       # END: tohost store
  .data
  .word 21
