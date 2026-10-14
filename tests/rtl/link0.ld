/* Directed contract programs (docs/phase5-design.md §5): code at address 0,
 * data at 0x400, tohost at 0x7F0 - the same layout as the C++ timing tests
 * in tests/unit/pipeline_timing_test.cpp. */
OUTPUT_ARCH("riscv")
ENTRY(_start)
SECTIONS
{
  . = 0x0;
  .text : { *(.text.init) *(.text) }
  . = 0x400;
  .data : { *(.data) }
  tohost = 0x7F0;
}
