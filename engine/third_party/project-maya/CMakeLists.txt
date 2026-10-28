# multiengine_raon1 — the raon1 family DLL: our raon1 port onto Project Maya's GLM engine (Maya v1.1.27's GLM sources,
# vendored here under src/ + include/ with the raon1 changes; MIT).  Until 2026-10-27 this was multiengine_glm, which
# also ran GLM-5.3-Flash; GLM now runs on Maya's own program in multiengine_maya (third_party/maya), and the launcher
# sends this library the raon1 packs only.
#
# add_subdirectory'd from the engine root when STRATA_BUILD_RAON1=ON, so it inherits the parent's ggml targets
# (ggml-cpu/ggml-base), the CUDA toolkit vars (_strata_gpu_*), STRATA_GGML_DIR and the CUDA language setup.
#
# Self-contained and ISOLATED: it compiles ONLY against Maya's headers in THIS dir's include/ (which diverge from
# our Strata core's same-named headers), never engine/include. It reuses the identical pinned ggml (same
# third_party/ggml/VERSION.txt across Strata, Maya and here; the ggml-cuda internal headers are byte-identical).
# Its ONLY exported symbol is glm_main (src/glm_driver.cpp): on MSVC a DLL exports only __declspec(dllexport), so
# the rest of the engine stays internal and cannot clash with the exe's own strata:: symbols.

if(NOT STRATA_ENABLE_CUDA)
  message(FATAL_ERROR "multiengine_raon1 needs CUDA (STRATA_ENABLE_CUDA=ON)")
endif()
if(NOT STRATA_GGML_DIR)
  message(FATAL_ERROR "multiengine_raon1 needs -DSTRATA_GGML_DIR=<llama.cpp checkout> (for ggml.h and ggml-cuda headers)")
endif()

set(_glm_root ${CMAKE_CURRENT_SOURCE_DIR})
file(GLOB_RECURSE _glm_cu  CONFIGURE_DEPENDS ${_glm_root}/src/*.cu)
file(GLOB_RECURSE _glm_cpp CONFIGURE_DEPENDS ${_glm_root}/src/*.cpp)
# moe_fused_rdna4.cu is HIP-only (AMD RDNA4 WMMA prompt experts; __HIPCC__-guarded). Maya does not compile it in a
# CUDA build, so drop it here too (its header stays for the CUDA callers' guarded declarations).
list(FILTER _glm_cu EXCLUDE REGEX "moe_fused_rdna4\\.cu$")

add_library(multiengine_raon1 SHARED ${_glm_cu} ${_glm_cpp})

set_target_properties(multiengine_raon1 PROPERTIES
  CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}"
  CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON
  CUDA_STANDARD 20 CUDA_STANDARD_REQUIRED ON
  CXX_VISIBILITY_PRESET hidden
  CUDA_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON)

# Maya's headers FIRST (isolated), then the shared/identical ggml pieces, then the CUDA toolkit includes.
target_include_directories(multiengine_raon1 BEFORE PRIVATE
  ${_glm_root}/include
  ${_glm_root}/src
  ${_glm_root}/src/core
  ${_glm_root}/src/kernels/cuda
  ${STRATA_GGML_DIR}/ggml/include
  ${STRATA_GGML_DIR}/ggml/src
  ${STRATA_GGML_DIR}/ggml/src/ggml-cuda
  ${CMAKE_SOURCE_DIR}/third_party/ggml
  ${_strata_gpu_include_directories})

target_compile_definitions(multiengine_raon1 PRIVATE STRATA_NATIVE_EXPERTS=1)

target_link_libraries(multiengine_raon1 PRIVATE
  ggml-cpu ggml-base
  ${_strata_gpu_runtime_target} ${_strata_gpu_blas_target})
find_package(Threads REQUIRED)
target_link_libraries(multiengine_raon1 PRIVATE Threads::Threads)

# The GLM prompt path (moe_mmq.cu) multiplies experts with ggml-cuda's MMQ. Reuse the pinned ggml-cuda sources in
# place (same commit, byte-identical headers), UNPATCHED — moe_mmq falls back to the separate SwiGLU kernel, so no
# STRATA_MMQ_GLU. Quant types are Maya's GLM set (iq1_s/k-quants: GLM experts; q8_0: the drafter).
set(_glm_ggcuda ${STRATA_GGML_DIR}/ggml/src/ggml-cuda)
set(_glm_mmq_cuda ${_glm_ggcuda}/quantize.cu ${_glm_ggcuda}/convert.cu)
foreach(_t q2_0 iq2_xxs iq2_xs iq2_s iq3_xxs iq3_s iq4_nl iq4_xs q8_0 iq1_s q2_k q3_k q4_k q5_k q6_k)
  list(APPEND _glm_mmq_cuda ${_glm_ggcuda}/template-instances/mmq-instance-${_t}.cu)
endforeach()
target_sources(multiengine_raon1 PRIVATE ${_glm_mmq_cuda})
target_compile_options(multiengine_raon1 PRIVATE $<$<COMPILE_LANGUAGE:CUDA>:--extended-lambda>)
# Maya's MMQ flags on the prompt-path CUDA only: fast-math stays OFF the GLM kernels to keep their numerics.
set_source_files_properties(${_glm_mmq_cuda}
  ${_glm_root}/src/prefill/moe_mmq.cu ${_glm_root}/src/prefill/ggml_cuda_host.cu
  PROPERTIES COMPILE_OPTIONS "-use_fast_math;-Xcompiler=/bigobj")

# The CPU i-quant / KQ lanes need their ISA. Default AVX2; the *_avx512 units want AVX-512.
if(MSVC)
  # /wd4068: the vendored CPU lane carries portable `#pragma GCC ...` directives (meaningful under GCC/clang,
  # unknown to MSVC -> C4068). Suppress it rather than edit the proven upstream code; it is not a real defect.
  target_compile_options(multiengine_raon1 PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/arch:AVX2> $<$<COMPILE_LANGUAGE:CXX>:/wd4068>)
  foreach(_f src/kernels/cpu/iq_avx512.cpp src/kernels/cpu/iq_avx512_one.cpp src/kernels/cpu/kq_avx512.cpp
             src/kernels/cpu/expert.cpp)
    if(EXISTS ${_glm_root}/${_f})
      set_source_files_properties(${_glm_root}/${_f} PROPERTIES COMPILE_OPTIONS "/arch:AVX512")
    endif()
  endforeach()
endif()

# -Werror is a deliberate SECOND pass (STRATA_GLM_WERROR=ON) once the vendored tree compiles clean, per the
# project rule; the first bring-up pass resolves includes/symbols without it.
option(STRATA_GLM_WERROR "Treat warnings as errors in multiengine_raon1 (turn on after first clean compile)" OFF)
if(STRATA_GLM_WERROR AND MSVC)
  # Match Maya/Strata's posture: /WX on the C++ host compile only. CUDA (.cu via nvcc) warnings — the ggml-cuda
  # #177-D "declared but never referenced" and #221-D 1e300 diagnostics — are NOT treated as errors there.
  target_compile_options(multiengine_raon1 PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/WX>)
endif()
