# Runs the ISS twice on the same program with tracing and requires the two
# traces to be byte-identical (project rule 5: determinism).
# Usage: cmake -DSIM=... -DELF=... -DOUT=... -P trace_determinism.cmake
foreach(run a b)
  execute_process(COMMAND ${SIM} ${ELF} --trace ${OUT}.${run}.trace
                  RESULT_VARIABLE rc ERROR_QUIET)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "run ${run} exited with ${rc}")
  endif()
endforeach()
file(SIZE ${OUT}.a.trace size)
if(size EQUAL 0)
  message(FATAL_ERROR "empty trace")
endif()
execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files ${OUT}.a.trace ${OUT}.b.trace
                RESULT_VARIABLE diff)
if(NOT diff EQUAL 0)
  message(FATAL_ERROR "traces differ between identical runs")
endif()
