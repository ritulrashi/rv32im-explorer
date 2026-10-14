/* Linker script for Phase 4 workloads (asm and C).
 * Code at 0x80000000; host interface (tohost, rvx_roi) on its own page;
 * data page-aligned after it; stack grows down from 0x80400000. */
OUTPUT_ARCH("riscv")
ENTRY(_start)

SECTIONS
{
  . = 0x80000000;
  .text.init : { *(.text.init) }
  .text : { *(.text .text.*) }
  . = ALIGN(0x1000);
  .tohost : { *(.tohost) }
  . = ALIGN(0x1000);
  .rodata : { *(.rodata .rodata.* .srodata .srodata.*) }
  .data : { *(.data .data.* .sdata .sdata.*) }
  .bss : { *(.bss .bss.* .sbss .sbss.* COMMON) }
  . = ALIGN(16);
  _end = .;
  _stack_top = 0x80400000;
}
