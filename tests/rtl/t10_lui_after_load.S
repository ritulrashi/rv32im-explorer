# lui reads no register even though its immediate bits equal x5 in the rs1/rs2 fields.
# expect: halt=tohost retired=3 cycles=7
  .text
  .globl _start
_start:
  lw x5, 0x400(x0)
  lui x6, 0x528
  sw x0, 0x7f0(x0)       # END: tohost store
