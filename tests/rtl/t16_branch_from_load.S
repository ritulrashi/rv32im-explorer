# Branch fed by a load (x1 = 0, taken): +1 +2.
# expect: halt=tohost retired=3 cycles=10
  .text
  .globl _start
_start:
  lw x1, 0x400(x0)
  beq x1, x0, 1f
  addi x9, x0, 1
1:
  sw x0, 0x7f0(x0)       # END: tohost store
  .data
  .word 0
