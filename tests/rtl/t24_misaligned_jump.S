# JAL to pc+6 faults on the jump itself (0x006000ef = jal x1, 6).
# expect: halt=misaligned-fetch retired=0 cycles=5
  .text
  .globl _start
_start:
  .word 0x006000ef
  sw x0, 0x7f0(x0)       # END: tohost store
