# strata-unified-engine: the standalone GPU-box wrapper (design 2026-10-23 s4;
# plan "Layout"; the SP6 s3.3 / gemma4 pattern). It builds engines/unified with
# STRATA_U_CUDA ON, for the owner's NVIDIA box only.
#
# ROOT OPTION, OFF AND NEVER IN CI. SECURE_STRATA_BUILD_UNIFIED_ENGINE gates the
# CUDA engine. The main secure_strata build (the repo root) leaves it OFF, so the
# root configure and every CI job compile NO CUDA from here -- exactly as the
# host-only engines/unified/ (STRATA_U_CUDA OFF) and its hermetic tests already
# build on linux-gcc / windows-llvm-mingw-cross / windows-msys2-clang. The owner's
# GPU box turns it ON, in one of two equivalent ways:
#
#   * STANDALONE (the usual box build): configure THIS directory as the top-level
#     project. The option then defaults ON, STRATA_U_CUDA is forced ON, and
#     engines/unified is added. For example, from the repo root:
#         cmake -S cmake/unified-engine -B build/unified -DCMAKE_CUDA_ARCHITECTURES=89
#         cmake --build build/unified --target strata-unified-engine
#
#   * FROM A PARENT: if a parent CMake add_subdirectory()'s this file, the option
#     defaults OFF there, so it builds nothing unless the box passes
#     -DSECURE_STRATA_BUILD_UNIFIED_ENGINE=ON. (The repo root is intentionally NOT
#     edited to add that add_subdirectory -- wiring it in, OFF by default, is a
#     one-line change the owner makes when he wants the component in the root tree;
#     it must never be ON in CI.)
#
# This wrapper adds ONLY engines/unified; nothing of upstream/ or the other engines
# is configured, matching the gemma4 standalone project.
cmake_minimum_required(VERSION 3.28)
project(strata_unified_engine_box LANGUAGES CXX)

# Default the gate ON only when this directory is the top-level project (a direct
# box build); OFF when included from a parent, so a parent/CI never builds it by
# accident.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(_u_engine_default ON)
else()
    set(_u_engine_default OFF)
endif()
option(SECURE_STRATA_BUILD_UNIFIED_ENGINE
       "Build the unified engine with CUDA on the GPU box (OFF in the main build and CI)"
       ${_u_engine_default})

if(SECURE_STRATA_BUILD_UNIFIED_ENGINE)
    # Force the engine's own CUDA switch ON for the box; the host build path
    # (STRATA_U_CUDA OFF) is unaffected because it is never reached from here.
    set(STRATA_U_CUDA ON CACHE BOOL "Build the unified engine CUDA substrate (GPU box)" FORCE)
    # engines/unified is two levels up from cmake/unified-engine/.
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../engines/unified
                     ${CMAKE_BINARY_DIR}/engines-unified)
    message(STATUS "strata-unified-engine: box build enabled (STRATA_U_CUDA ON).")
endif()
