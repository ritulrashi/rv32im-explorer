include(FetchContent)
FetchContent_Declare(googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.15.2
  GIT_SHALLOW TRUE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)
include(GoogleTest)

# ---------------------------------------------------------------------------
# Unit tests
# ---------------------------------------------------------------------------
add_executable(rvx_unit_tests
  unit/decode_test.cpp
  unit/memory_test.cpp
  unit/trace_test.cpp
  unit/iss_test.cpp
  unit/loader_test.cpp
  unit/pipeline_timing_test.cpp
  unit/config_test.cpp
  unit/bpred_test.cpp
  unit/cache_test.cpp
  unit/pipeline_phase3_test.cpp)
target_include_directories(rvx_unit_tests PRIVATE unit)
target_link_libraries(rvx_unit_tests PRIVATE rvx_iss rvx_pipeline GTest::gtest_main)
target_compile_options(rvx_unit_tests PRIVATE ${RVX_WARNINGS})
gtest_discover_tests(rvx_unit_tests PROPERTIES LABELS unit)

# ---------------------------------------------------------------------------
# Official riscv-tests (rv32ui, rv32um), built with the RISC-V cross compiler
# against our minimal environment in riscv-tests-env/.
# ---------------------------------------------------------------------------
find_program(RISCV_GCC riscv64-unknown-elf-gcc)
find_program(RISCV_OBJDUMP riscv64-unknown-elf-objdump)
set(RISCV_TESTS_DIR ${PROJECT_SOURCE_DIR}/third_party/riscv-tests)

if(NOT RISCV_GCC)
  message(WARNING "riscv64-unknown-elf-gcc not found: riscv-tests will not be built")
  return()
endif()
if(NOT EXISTS ${RISCV_TESTS_DIR}/isa/rv32ui/Makefrag)
  message(FATAL_ERROR "riscv-tests submodule missing: run `git submodule update --init third_party/riscv-tests`")
endif()

# Parse the test lists from upstream's own Makefrags, so the set of tests is
# exactly upstream's set and cannot silently drift from it.
function(rvx_read_test_list suite out_var)
  file(READ ${RISCV_TESTS_DIR}/isa/${suite}/Makefrag frag)
  string(REPLACE "\\\n" " " frag "${frag}")  # join backslash-continued lines
  string(REGEX MATCH "${suite}_sc_tests = [^\n]*" line "${frag}")
  string(REGEX REPLACE "^${suite}_sc_tests = " "" line "${line}")
  string(REGEX REPLACE "[\t ]+" ";" line "${line}")
  list(FILTER line EXCLUDE REGEX "^$")
  if(NOT line)
    message(FATAL_ERROR "could not parse test list from ${suite}/Makefrag")
  endif()
  set(${out_var} ${line} PARENT_SCOPE)
endfunction()

# Tests excluded by design, each with the reason (also in docs/LIMITATIONS.md).
set(RVX_EXCLUDED_rv32ui_ma_data "misaligned data access; the models halt on misaligned access by design")

# -march deliberately omits Zicsr: if any test or env macro used a CSR
# instruction, assembly would fail rather than the test silently depending on
# behaviour we do not model. Zifencei is needed for rv32ui/fence_i.
set(RISCV_CFLAGS -march=rv32im_zifencei -mabi=ilp32 -mcmodel=medany -static
    -nostdlib -nostartfiles -fvisibility=hidden)

set(RVX_RISCV_TEST_ELFS "")
set(RVX_RISCV_TEST_COUNT 0)
set(RVX_RISCV_TEST_EXCLUDED "")
foreach(suite rv32ui rv32um)
  rvx_read_test_list(${suite} names)
  foreach(name ${names})
    if(DEFINED RVX_EXCLUDED_${suite}_${name})
      list(APPEND RVX_RISCV_TEST_EXCLUDED "${suite}-${name}")
      message(STATUS "riscv-tests: excluding ${suite}-${name} (${RVX_EXCLUDED_${suite}_${name}})")
      continue()
    endif()
    set(src ${RISCV_TESTS_DIR}/isa/${suite}/${name}.S)
    set(elf ${CMAKE_CURRENT_BINARY_DIR}/riscv-tests/${suite}-${name}.elf)
    set(dump ${CMAKE_CURRENT_BINARY_DIR}/riscv-tests/${suite}-${name}.dump)
    add_custom_command(
      OUTPUT ${elf} ${dump}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/riscv-tests
      COMMAND ${RISCV_GCC} ${RISCV_CFLAGS}
              -I${CMAKE_CURRENT_SOURCE_DIR}/riscv-tests-env
              -I${RISCV_TESTS_DIR}/isa/macros/scalar
              -T${CMAKE_CURRENT_SOURCE_DIR}/riscv-tests-env/link.ld
              ${src} -o ${elf}
      COMMAND sh -c "${RISCV_OBJDUMP} -d ${elf} > ${dump}"
      DEPENDS ${src} ${CMAKE_CURRENT_SOURCE_DIR}/riscv-tests-env/riscv_test.h
              ${CMAKE_CURRENT_SOURCE_DIR}/riscv-tests-env/link.ld
      COMMENT "riscv-tests: ${suite}-${name}"
      VERBATIM)
    list(APPEND RVX_RISCV_TEST_ELFS ${elf})
    add_test(NAME ${suite}-${name}
             COMMAND rvsim-iss ${elf} --max-insns 1000000)
    set_tests_properties(${suite}-${name} PROPERTIES LABELS "riscv-tests;${suite}")
    # Phase 2: the pipeline must pass the test on its own...
    add_test(NAME pipe-${suite}-${name}
             COMMAND rvsim-pipe ${elf} --max-cycles 10000000)
    set_tests_properties(pipe-${suite}-${name} PROPERTIES LABELS "pipeline;${suite}")
    # ...and agree with the ISS on every retired instruction.
    add_test(NAME lockstep-${suite}-${name}
             COMMAND rvsim-lockstep ${elf} --max-cycles 10000000)
    set_tests_properties(lockstep-${suite}-${name} PROPERTIES LABELS "lockstep;${suite}")
    math(EXPR RVX_RISCV_TEST_COUNT "${RVX_RISCV_TEST_COUNT} + 1")
  endforeach()
endforeach()
add_custom_target(riscv_tests ALL DEPENDS ${RVX_RISCV_TEST_ELFS})

# Determinism: tracing the same program twice must give identical bytes.
# rv32ui-fence_i is used because it exercises self-modifying code.
add_test(NAME trace-determinism
         COMMAND ${CMAKE_COMMAND}
                 -DSIM=$<TARGET_FILE:rvsim-iss>
                 -DELF=${CMAKE_CURRENT_BINARY_DIR}/riscv-tests/rv32ui-fence_i.elf
                 -DOUT=${CMAKE_CURRENT_BINARY_DIR}/determinism
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/trace_determinism.cmake)
set_tests_properties(trace-determinism PROPERTIES LABELS determinism)

# Random programs in lockstep. CTest runs a small smoke range on every build;
# the Phase 2 exit criterion uses scripts/phase2_exit.sh with seeds 1-1000.
add_test(NAME random-lockstep-smoke
         COMMAND bash ${PROJECT_SOURCE_DIR}/scripts/random_lockstep.sh
                 ${CMAKE_BINARY_DIR} 1 50 ${CMAKE_CURRENT_BINARY_DIR}/random-smoke)
set_tests_properties(random-lockstep-smoke PROPERTIES LABELS "lockstep;random")

# Phase 3: the baseline config must reproduce Phase 2 cycle counts exactly,
# lockstep must agree under every config, and the CPI stack must sum to the
# total cycle count for every run.
set(RVX_CONFIGS baseline bimodal_btb gshare_btb small_caches big)
add_test(NAME phase2-regression
         COMMAND bash ${PROJECT_SOURCE_DIR}/scripts/phase2_regression.sh ${CMAKE_BINARY_DIR})
set_tests_properties(phase2-regression PROPERTIES LABELS "regression")
set(RVX_CONFIG_FILES "")
foreach(cfg ${RVX_CONFIGS})
  list(APPEND RVX_CONFIG_FILES ${PROJECT_SOURCE_DIR}/configs/${cfg}.cfg)
  add_test(NAME random-lockstep-${cfg}
           COMMAND bash ${PROJECT_SOURCE_DIR}/scripts/random_lockstep.sh
                   ${CMAKE_BINARY_DIR} 1 25 ${CMAKE_CURRENT_BINARY_DIR}/random-${cfg}
                   -- --config ${PROJECT_SOURCE_DIR}/configs/${cfg}.cfg)
  set_tests_properties(random-lockstep-${cfg} PROPERTIES LABELS "lockstep;random;configs")
endforeach()
add_test(NAME cpi-sum
         COMMAND python3 ${PROJECT_SOURCE_DIR}/scripts/check_cpi_sum.py ${CMAKE_BINARY_DIR} ${RVX_CONFIG_FILES})
set_tests_properties(cpi-sum PROPERTIES LABELS "cpi")

# Phase 5: directed contract programs for RTL correlation (rv32i only).
set(RVX_RTL_TEST_ELFS "")
file(GLOB RVX_RTL_TEST_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/rtl/*.S)
foreach(src ${RVX_RTL_TEST_SRCS})
  get_filename_component(name ${src} NAME_WE)
  set(elf ${CMAKE_CURRENT_BINARY_DIR}/rtl/${name}.elf)
  add_custom_command(OUTPUT ${elf}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/rtl
    COMMAND ${RISCV_GCC} -march=rv32i_zifencei -mabi=ilp32 -static -nostdlib -nostartfiles
            -T${CMAKE_CURRENT_SOURCE_DIR}/rtl/link0.ld ${src} -o ${elf}
    DEPENDS ${src} ${CMAKE_CURRENT_SOURCE_DIR}/rtl/link0.ld
    COMMENT "rtl directed: ${name}"
    VERBATIM)
  list(APPEND RVX_RTL_TEST_ELFS ${elf})
endforeach()
add_custom_target(rtl_directed_tests ALL DEPENDS ${RVX_RTL_TEST_ELFS})
if(TARGET rvsim-rtl)
  add_test(NAME rtl-correlate-smoke
           COMMAND bash ${PROJECT_SOURCE_DIR}/scripts/correlate.sh ${CMAKE_BINARY_DIR}
                   ${CMAKE_CURRENT_BINARY_DIR}/correlate-smoke 1 20)
  set_tests_properties(rtl-correlate-smoke PROPERTIES LABELS "rtl;correlation")
endif()

# rvgen: the default (rv32im) output must stay byte-identical to what Phase 2
# measured; the hash was recorded from rvgen at commit fa8d50f, before --isa
# was added (seeds 1-200 concatenated).
add_test(NAME rvgen-default-unchanged
         COMMAND sh -c "for s in $(seq 1 200); do $<TARGET_FILE:rvgen> --seed $s; done | sha256sum | grep -q 312d739f6dc474304b880a60baed7d3bd464520ec76f9995f440f50d39f96807")
set_tests_properties(rvgen-default-unchanged PROPERTIES LABELS determinism)
add_test(NAME rvgen-rv32i-determinism
         COMMAND sh -c "$<TARGET_FILE:rvgen> --seed 7 --isa rv32i > ${CMAKE_CURRENT_BINARY_DIR}/i1.S && $<TARGET_FILE:rvgen> --seed 7 --isa rv32i > ${CMAKE_CURRENT_BINARY_DIR}/i2.S && cmp ${CMAKE_CURRENT_BINARY_DIR}/i1.S ${CMAKE_CURRENT_BINARY_DIR}/i2.S && ! grep -qE '^ +(mul|div|rem)' ${CMAKE_CURRENT_BINARY_DIR}/i1.S")
set_tests_properties(rvgen-rv32i-determinism PROPERTIES LABELS determinism)

# rvgen determinism: the same seed must give byte-identical assembly.
add_test(NAME rvgen-determinism
         COMMAND sh -c "$<TARGET_FILE:rvgen> --seed 12345 > ${CMAKE_CURRENT_BINARY_DIR}/g1.S && $<TARGET_FILE:rvgen> --seed 12345 > ${CMAKE_CURRENT_BINARY_DIR}/g2.S && cmp ${CMAKE_CURRENT_BINARY_DIR}/g1.S ${CMAKE_CURRENT_BINARY_DIR}/g2.S")
set_tests_properties(rvgen-determinism PROPERTIES LABELS determinism)
message(STATUS "riscv-tests: ${RVX_RISCV_TEST_COUNT} registered, excluded: ${RVX_RISCV_TEST_EXCLUDED}")
