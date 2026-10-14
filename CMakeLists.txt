cmake_minimum_required(VERSION 3.24)
project(rv32im_explorer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(RVX_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
option(RVX_BUILD_TESTS "Build unit tests and riscv-tests" ON)

if(RVX_SANITIZE)
  # -fno-sanitize-recover makes any UBSan finding abort the process, so a
  # sanitizer report always turns into a test failure instead of a log line.
  add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# Strict warnings for project code only (not third-party targets).
set(RVX_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Werror)

add_library(rvx_common STATIC
  src/common/decode.cpp
  src/common/trace.cpp
  src/common/memory.cpp
  src/common/loader.cpp
  src/common/kvfile.cpp)
target_include_directories(rvx_common PUBLIC src)
target_compile_options(rvx_common PRIVATE ${RVX_WARNINGS})

add_library(rvx_iss STATIC
  src/iss/iss.cpp)
target_include_directories(rvx_iss PUBLIC src)
target_link_libraries(rvx_iss PUBLIC rvx_common)
target_compile_options(rvx_iss PRIVATE ${RVX_WARNINGS})

add_library(rvx_bpred STATIC src/bpred/predictor.cpp)
target_include_directories(rvx_bpred PUBLIC src)
target_compile_options(rvx_bpred PRIVATE ${RVX_WARNINGS})

add_library(rvx_cache STATIC
  src/cache/cache.cpp
  src/cache/dram.cpp
  src/cache/memsys.cpp)
target_include_directories(rvx_cache PUBLIC src)
target_compile_options(rvx_cache PRIVATE ${RVX_WARNINGS})

add_library(rvx_pipeline STATIC
  src/pipeline/pipeline.cpp
  src/pipeline/exec.cpp
  src/pipeline/config.cpp
  src/pipeline/stats_json.cpp)
target_include_directories(rvx_pipeline PUBLIC src)
target_link_libraries(rvx_pipeline PUBLIC rvx_common rvx_bpred rvx_cache)
target_compile_options(rvx_pipeline PRIVATE ${RVX_WARNINGS})

add_executable(rvsim-iss tools/iss_main.cpp)
target_link_libraries(rvsim-iss PRIVATE rvx_iss)
target_compile_options(rvsim-iss PRIVATE ${RVX_WARNINGS})

add_executable(rvsim-pipe tools/pipe_main.cpp)
target_link_libraries(rvsim-pipe PRIVATE rvx_pipeline)
target_compile_options(rvsim-pipe PRIVATE ${RVX_WARNINGS})

add_executable(rvsim-lockstep tools/lockstep_main.cpp)
target_link_libraries(rvsim-lockstep PRIVATE rvx_iss rvx_pipeline)
target_compile_options(rvsim-lockstep PRIVATE ${RVX_WARNINGS})

add_executable(rvgen tools/rvgen.cpp)
target_include_directories(rvgen PRIVATE src)
target_compile_options(rvgen PRIVATE ${RVX_WARNINGS})

# Phase 5 RTL (Verilator); before tests, which refer to rvsim-rtl.
add_subdirectory(rtl)

if(RVX_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# Phase 4 workloads (needs the RISC-V GCC toolchain and clang).
add_subdirectory(workloads)
