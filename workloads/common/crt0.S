/* Minimal C runtime: set up the stack, call main(), report its return value
 * to tohost (main returns 1 for pass, 3 for fail), then spin. The simulator
 * halts when the tohost store retires. No .bss clearing is needed: the
 * loader zero-fills it (src/common/loader.cpp). */
  .section .text.init
  .globl _start
_start:
  la sp, _stack_top
  call main
  la t0, tohost
  sw a0, 0(t0)
  j .
