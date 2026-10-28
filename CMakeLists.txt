# Secure Strata root build: our secure_frontend/ plus upstream Strata's engine
# from the pinned, unmodified git submodule upstream/ (docs/AGENT_HANDOFF.md,
# "Repository layout"). Upstream's own CMakeLists.txt is used as-is.
cmake_minimum_required(VERSION 3.24)
project(secure_strata LANGUAGES CXX)

# Default to Release for the whole build (frontend and engine alike), before
# upstream's own FORCE sees it. project() has already created an empty
# CMAKE_BUILD_TYPE cache entry, so FORCE is needed; the guard fires only when
# the value is empty, so a -DCMAKE_BUILD_TYPE=... is never overwritten.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Upstream calls enable_testing() only in its own directories; the root ctest
# sees any test only when the root enables testing.
include(CTest)

option(SECURE_STRATA_BUILD_ENGINE "Configure upstream's engine from upstream/" ON)
option(STRATA_BUILD_SECURE_FRONTEND_TESTS "Build the portable frontend tests" ${BUILD_TESTING})
# Declared here, before upstream's option(STRATA_WERROR OFF) exists, so the
# frontend keeps -Werror whatever STRATA_WERROR is.
option(SECURE_FRONTEND_WERROR "Treat warnings as errors in the secure frontend" ON)

# Cache defaults; -D overrides them. Only STRATA_NATIVE_EXPERTS changes
# upstream's default (ON there, and it fetches llama.cpp over the network);
# CUDA and HIP are already OFF upstream and are seeded here defensively.
set(STRATA_ENABLE_CUDA OFF CACHE BOOL "Build the CUDA targets")
set(STRATA_ENABLE_HIP OFF CACHE BOOL "Build the HIP targets")
set(STRATA_NATIVE_EXPERTS OFF CACHE BOOL "Native (IQ) experts through ggml-cpu (fetches llama.cpp)")

# The frontend keeps -Werror because SECURE_FRONTEND_WERROR is declared above,
# not because of the order below; the frontend is still added first.
add_subdirectory(secure_frontend)

if(SECURE_STRATA_BUILD_ENGINE)
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/upstream/CMakeLists.txt")
    message(FATAL_ERROR "upstream/ is empty: run `git submodule update --init`")
  endif()
  add_subdirectory(upstream)
endif()
