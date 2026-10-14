# Hand-drawn per-cycle diagram (expected output: t26_diagram.expected).
# load-use, taken branch, fence.i, fault flush behind the tohost store.
# expect: halt=tohost retired=5 cycles=14
  .text
  .globl _start
_start:
  lw x1, 0x400(x0)     # 0x00 A
  add x2, x1, x1       # 0x04 B  load-use on A
  beq x0, x0, 1f       # 0x08 C  taken -> 0x10
  addi x9, x0, 1       # 0x0c D  wrong path
1:
  fence.i              # 0x10 E  redirect to 0x14
  sw x0, 0x7f0(x0)       # END: tohost store
  .word 0              # 0x18 G  illegal: fault flush, never retires
