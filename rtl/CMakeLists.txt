# Phase 5: Verilated RTL core + testbench (docs/phase5-design.md).
find_package(verilator HINTS $ENV{VERILATOR_ROOT} /usr/share/verilator)
if(NOT verilator_FOUND)
  message(WARNING "Verilator not found: rvsim-rtl will not be built")
  return()
endif()

set(RVX_RTL_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/rvx_pkg.sv
  ${CMAKE_CURRENT_SOURCE_DIR}/rvx_core.sv)

add_executable(rvsim-rtl ${PROJECT_SOURCE_DIR}/tb/rvx_tb.cpp)
target_include_directories(rvsim-rtl PRIVATE ${PROJECT_SOURCE_DIR}/tools)
target_link_libraries(rvsim-rtl PRIVATE rvx_common)
# Next to the other tools (rvsim-pipe, rvsim-lockstep) in the build root.
set_target_properties(rvsim-rtl PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
# -Wall: every Verilator lint warning is fatal. --assert enables the contract
# assertions in rvx_core.sv.
verilate(rvsim-rtl
  SOURCES ${RVX_RTL_SOURCES}
  TOP_MODULE rvx_core
  PREFIX Vrvx_core
  VERILATOR_ARGS -Wall --assert)

if(RVX_BUILD_TESTS)
  add_test(NAME rtl-lint
           COMMAND verilator --lint-only -Wall ${RVX_RTL_SOURCES} --top-module rvx_core)
  set_tests_properties(rtl-lint PROPERTIES LABELS rtl)
endif()
