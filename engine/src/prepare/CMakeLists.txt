# src/prepare - strata-prepare: native packs, no Python (docs/superpowers/specs/2026-10-27-strata-prepare-design.md).
#
# Built two ways: inside the engine build (cmake/multiengine.cmake adds it; ggml-base is already a target there), or on
# its own for its tests, which takes seconds instead of the CUDA engine's build:
#   cmake -S engine/src/prepare -B <dir> -DSTRATA_GGML_DIR=<llama.cpp checkout> && cmake --build <dir> && ctest --test-dir <dir>
# It needs only ggml-base (ggml's dequantizers, for --compat-bf16).
cmake_minimum_required(VERSION 3.21)

set(_sp_standalone OFF)
if(NOT TARGET ggml-base)
  set(_sp_standalone ON)
  project(strata_prepare C CXX)
  set(STRATA_GGML_DIR "" CACHE PATH "the llama.cpp checkout to take ggml from (the engine's pinned one)")
  if(NOT STRATA_GGML_DIR)
    message(FATAL_ERROR "strata-prepare on its own: -DSTRATA_GGML_DIR=<llama.cpp checkout>")
  endif()
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)   # ggml-base static, as in the engine build
  set(GGML_CUDA OFF CACHE BOOL "" FORCE)
  set(GGML_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(GGML_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  add_subdirectory(${STRATA_GGML_DIR}/ggml ${CMAKE_BINARY_DIR}/ggml EXCLUDE_FROM_ALL)
  enable_testing()
endif()

if(MSVC)
  set(_sp_warnings /W4 /WX /permissive- /utf-8 /EHsc /Zc:__cplusplus)
else()
  set(_sp_warnings -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)
endif()

add_library(strata_prepare_core STATIC gguf_header.cpp py_json.cpp sha256.cpp pack_common.cpp iq_pack.cpp maya_pack.cpp
                                       maya_moe_pack.cpp tokenizer_export.cpp)
target_compile_features(strata_prepare_core PUBLIC cxx_std_20)
target_include_directories(strata_prepare_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(strata_prepare_core PUBLIC ggml-base)
target_compile_options(strata_prepare_core PRIVATE ${_sp_warnings})

add_executable(strata-prepare prepare_main.cpp)
target_link_libraries(strata-prepare PRIVATE strata_prepare_core)
if(WIN32)
  target_link_libraries(strata-prepare PRIVATE shell32)   # CommandLineToArgvW: the arguments in UTF-8
endif()
target_compile_options(strata-prepare PRIVATE ${_sp_warnings})

# the unit tests (synthetic GGUFs); the byte-for-byte check against upstream's Python is tools/secure/strata_prepare_parity.py
set(_sp_tests ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/prepare/prepare_test.cpp)
if(EXISTS ${_sp_tests} AND (_sp_standalone OR STRATA_BUILD_TESTS))
  add_executable(strata_prepare_tests ${_sp_tests})
  target_link_libraries(strata_prepare_tests PRIVATE strata_prepare_core)
  target_compile_options(strata_prepare_tests PRIVATE ${_sp_warnings})
  foreach(_case sha256_known_answers python_float_repr_matches_python json_dumps_matches_python half_and_bf16_round_to_even
                gguf_header_reads_like_gguf_reader single_file_pack_tables split_layer_pack_is_v4 conversions_and_refusals
                compat_bf16_dequantizes experts_bin_blobs download_protocol utf8_replace_like_python
                tokenizer_export_like_strata_tokenizer tokenizer_export_refusals maya_glm_pack_tables maya_glm_refusals
                maya_raon1_pack_tables maya_split_layer_is_v4 maya_qwen4exp_routers download_maya_protocol maya_ds4_pack_tables maya_mm3_pack_tables
                download_maya_ds4_step download_maya_mm3_step)
    add_test(NAME strata_prepare_tests.${_case} COMMAND strata_prepare_tests --case ${_case} $<TARGET_FILE:strata-prepare>)
  endforeach()
endif()
