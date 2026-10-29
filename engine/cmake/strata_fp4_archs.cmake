# The CUDA architectures of the opt-in NVFP4 W4A4 units (strata_mmq_w4a4 and gemma4's g4core_w4a4). RTX 50's FP4 MMA
# (mma with block scale) exists only in the arch-specific target, so each 12x becomes 12xa; the rest of the build keeps
# its plain targets. "native", "all" and "all-major" are spelled out first with the lists CMake found (3.24+): left as
# words, a native build on an RTX 5090 compiled these units for a plain sm_120 and ptxas refused them (owner 2026-10-29).
function(strata_fp4_cuda_archs out)
  set(_archs)
  foreach(_a IN LISTS CMAKE_CUDA_ARCHITECTURES)
    if(_a STREQUAL "native")
      list(APPEND _archs ${CMAKE_CUDA_ARCHITECTURES_NATIVE})
    elseif(_a STREQUAL "all")
      list(APPEND _archs ${CMAKE_CUDA_ARCHITECTURES_ALL})
    elseif(_a STREQUAL "all-major")
      list(APPEND _archs ${CMAKE_CUDA_ARCHITECTURES_ALL_MAJOR})
    else()
      list(APPEND _archs ${_a})
    endif()
  endforeach()
  # (two steps: CMake before 3.29 rejects a \2 whose optional group did not match, "120" alone)
  list(TRANSFORM _archs REPLACE "^(12[0-9])$" "\\1a")
  list(TRANSFORM _archs REPLACE "^(12[0-9])-(real|virtual)$" "\\1a-\\2")
  set(${out} ${_archs} PARENT_SCOPE)
endfunction()
