# Phase 4 workloads (docs/phase4-design.md). Every (kernel, variant, size,
# optimisation level) becomes one ELF in ${CMAKE_CURRENT_BINARY_DIR}, named
#   asm-<variant>-n<N>.elf    hand-written assembly
#   c-<O0|O3>-<variant>-n<N>.elf  clang-compiled C
# plus a .dump disassembly next to it.
find_program(RISCV_GCC riscv64-unknown-elf-gcc)
find_program(RISCV_OBJDUMP riscv64-unknown-elf-objdump)
find_program(CLANG clang)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
if(NOT RISCV_GCC OR NOT CLANG)
  message(WARNING "riscv64-unknown-elf-gcc or clang not found: Phase 4 workloads will not be built")
  return()
endif()

set(WL ${CMAKE_CURRENT_SOURCE_DIR})
set(WB ${CMAKE_CURRENT_BINARY_DIR})
set(WL_SEED 1)
set(WL_LINK ${RISCV_GCC} -march=rv32im -mabi=ilp32 -mcmodel=medany -static -nostdlib -nostartfiles
    -T${WL}/common/link.ld)
# clang: freestanding, no builtins (no memset/memcpy calls), strict warnings.
set(WL_CLANG ${CLANG} --target=riscv32-unknown-elf -march=rv32im -mabi=ilp32 -mcmodel=medany
    -ffreestanding -fno-builtin -nostdlib -Wall -Wextra -Werror -I${WL}/common)
set(WL_ELFS "")

function(wl_data kernel n pad out_var)
  if(pad EQUAL 0)
    set(out ${WB}/data-${kernel}-n${n}.S)
  else()
    set(out ${WB}/data-${kernel}-n${n}-p${pad}.S)
  endif()
  if(NOT TARGET wl-data-${kernel}-${n}-${pad})
    add_custom_command(OUTPUT ${out}
      COMMAND ${Python3_EXECUTABLE} ${WL}/gen_data.py --kernel ${kernel} --n ${n} --seed ${WL_SEED} --pad ${pad} --out ${out}
      DEPENDS ${WL}/gen_data.py
      COMMENT "workload data: ${kernel} n=${n} pad=${pad}"
      VERBATIM)
    add_custom_target(wl-data-${kernel}-${n}-${pad} DEPENDS ${out})
  endif()
  set(${out_var} ${out} PARENT_SCOPE)
endfunction()

function(wl_asm variant kernel n log2n)
  wl_data(${kernel} ${n} 0 data)
  set(elf ${WB}/asm-${variant}-n${n}.elf)
  add_custom_command(OUTPUT ${elf}
    COMMAND ${WL_LINK} -DN=${n} -DLOG2N=${log2n} -I${WL}/common
            ${WL}/asm/${variant}.S ${WL}/common/check.S ${WL}/common/hostif.S ${data} -o ${elf}
    COMMAND sh -c "${RISCV_OBJDUMP} -d ${elf} > ${elf}.dump"
    DEPENDS ${WL}/asm/${variant}.S ${WL}/common/check.S ${WL}/common/hostif.S ${WL}/common/rvx_workload.h
            ${WL}/common/link.ld ${data}
    COMMENT "workload: asm ${variant} n=${n}"
    VERBATIM)
  set(WL_ELFS ${WL_ELFS} ${elf} PARENT_SCOPE)
endfunction()

function(wl_c variant kernel n opt)
  wl_data(${kernel} ${n} 0 data)
  set(obj ${WB}/c-${opt}-${variant}-n${n}.o)
  set(elf ${WB}/c-${opt}-${variant}-n${n}.elf)
  add_custom_command(OUTPUT ${elf}
    COMMAND ${WL_CLANG} -${opt} -DN=${n} -c ${WL}/c/${variant}.c -o ${obj}
    COMMAND ${WL_LINK} ${WL}/common/crt0.S ${WL}/common/hostif.S ${data} ${obj} -o ${elf}
    COMMAND sh -c "${RISCV_OBJDUMP} -d ${elf} > ${elf}.dump"
    DEPENDS ${WL}/c/${variant}.c ${WL}/common/roi.h ${WL}/common/crt0.S ${WL}/common/hostif.S
            ${WL}/common/link.ld ${data}
    COMMENT "workload: clang -${opt} ${variant} n=${n}"
    VERBATIM)
  set(WL_ELFS ${WL_ELFS} ${elf} PARENT_SCOPE)
endfunction()

# Hand-written assembly.
wl_asm(dot_naive dot 1024 10)
wl_asm(dot_unroll4 dot 1024 10)
foreach(n 32 64)
  if(n EQUAL 32)
    set(lg 5)
  else()
    set(lg 6)
  endif()
  foreach(v gemm_naive gemm_unroll4 gemm_tiled gemm_tiled_unroll4)
    wl_asm(${v} gemm ${n} ${lg})
  endforeach()
endforeach()

# C, compiled by clang at -O0 and -O3.
foreach(opt O0 O3)
  foreach(v dot_naive dot_unroll4)
    wl_c(${v} dot 1024 ${opt})
  endforeach()
  foreach(n 32 64)
    foreach(v gemm_naive gemm_tiled gemm_unroll4)
      wl_c(${v} gemm ${n} ${opt})
    endforeach()
  endforeach()
endforeach()

# Investigation variants (RESULTS.md, cache set conflicts): rows padded by
# PAD words so that consecutive rows no longer map to the same cache sets.
set(WL_PAD 4)
function(wl_c_pad variant kernel n opt pad)
  wl_data(${kernel} ${n} ${pad} data)
  set(obj ${WB}/c-${opt}-${variant}-n${n}.o)
  set(elf ${WB}/c-${opt}-${variant}-n${n}.elf)
  add_custom_command(OUTPUT ${elf}
    COMMAND ${WL_CLANG} -${opt} -DN=${n} -DPAD=${pad} -c ${WL}/c/${variant}.c -o ${obj}
    COMMAND ${WL_LINK} ${WL}/common/crt0.S ${WL}/common/hostif.S ${data} ${obj} -o ${elf}
    COMMAND sh -c "${RISCV_OBJDUMP} -d ${elf} > ${elf}.dump"
    DEPENDS ${WL}/c/${variant}.c ${WL}/common/roi.h ${WL}/common/crt0.S ${WL}/common/hostif.S
            ${WL}/common/link.ld ${data}
    COMMENT "workload: clang -${opt} ${variant} n=${n} pad=${pad}"
    VERBATIM)
  set(WL_ELFS ${WL_ELFS} ${elf} PARENT_SCOPE)
endfunction()
foreach(n 32 64)
  foreach(v gemm_naive_pad gemm_tiled_pad)
    wl_c_pad(${v} gemm ${n} O3 ${WL_PAD})
  endforeach()
endforeach()

add_custom_target(workloads ALL DEPENDS ${WL_ELFS})
list(LENGTH WL_ELFS n_wl)
message(STATUS "workloads: ${n_wl} ELFs")

if(RVX_BUILD_TESTS)
  add_test(NAME gen-data-selftest COMMAND ${Python3_EXECUTABLE} ${WL}/gen_data.py --selftest)
  set_tests_properties(gen-data-selftest PROPERTIES LABELS workloads)
  # Every workload must pass its own result check on the pipeline and agree
  # with the ISS in lockstep. small_caches exercises every Phase 3 mechanism.
  foreach(elf ${WL_ELFS})
    get_filename_component(name ${elf} NAME_WE)
    add_test(NAME wl-pass-${name}
             COMMAND rvsim-pipe ${elf} --config ${PROJECT_SOURCE_DIR}/configs/small_caches.cfg)
    add_test(NAME wl-lockstep-${name}
             COMMAND rvsim-lockstep ${elf} --config ${PROJECT_SOURCE_DIR}/configs/small_caches.cfg)
    set_tests_properties(wl-pass-${name} wl-lockstep-${name} PROPERTIES LABELS workloads)
  endforeach()
endif()
