# cmake/multiengine.cmake - multiengine: the one engine executable over its model families' libraries.
#
# Included by the last line of upstream's root CMakeLists.txt (the one line we add there), when every upstream target
# exists.  Every family is its own shared library, compiled from its own sources, so none can change what another
# computes:
#   multiengine_strata    upstream Strata's engine: the `strata` target's program (src/program/generate.cpp,
#                         unchanged) with its libraries, flags and definitions as upstream builds them
#                         (src/multiengine/strata_family.cpp)
#   multiengine_maya      Project Maya's three programs, from third_party/maya (Maya's tree, unchanged) built as a
#   multiengine_maya_ds4  project of its own, as Maya's setup configures it: GLM-5.3-Flash, DeepSeek-V4.1-Flash and
#   multiengine_maya_mm3  MiniMax-M3.1-Flash (cmake/multiengine_maya.cmake adds the libraries inside that build)
#   multiengine_raon1     raon1 on our port of Maya's GLM engine, third_party/project-maya
#   multiengine_gemma4    Gemma 4 26B-A4B on its own copy of the Strata core, src/gemma4
#   multiengine           the launcher (src/multiengine/launcher.cpp): reads which family a command line is for and
#                         loads only that library, from its own folder - where the build puts them all
# The libraries share only the pinned ggml (third_party/ggml/VERSION.txt, one commit for every family) and the CUDA
# toolkit.  Upstream's own `strata` executable is still built as upstream builds it.  NVIDIA builds only (the Gemma 4,
# Maya and raon1 libraries are built for CUDA here; the launcher's switch is written for them).
if(NOT STRATA_ENABLE_CUDA OR STRATA_ENABLE_HIP OR STRATA_HIP_GFX906 OR NOT TARGET strata)
  return()
endif()

if(MSVC)
  set(_me_warnings /W4 /WX /permissive- /utf-8 /EHsc /Zc:__cplusplus)
else()
  set(_me_warnings -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)
endif()

# ---- the Gemma 4 and raon1 family libraries (their own subdirectories) --------------------------------------------
# They inherit this directory's definitions, which carry the engine version (STRATA_VERSION).  The raon1 library (a GLM
# engine) keys its calibration cache and its saved sessions on it, and both families were validated on 0.1.49.2
# (upstream v0.2.4.2's engine), so they keep that version while upstream's engine moves on: an upstream update does not
# invalidate their caches or sessions, and their compile lines stay as they were.
set(MULTIENGINE_FAMILY_ENGINE_VERSION "0.1.49.2")
get_directory_property(_me_root_defs COMPILE_DEFINITIONS)
set(_me_family_defs ${_me_root_defs})
list(TRANSFORM _me_family_defs REPLACE "^STRATA_VERSION=.*$" "STRATA_VERSION=\"${MULTIENGINE_FAMILY_ENGINE_VERSION}\"")
set_directory_properties(PROPERTIES COMPILE_DEFINITIONS "${_me_family_defs}")

set(_me_families multiengine_strata)
set(_me_defs)
if(TARGET strata_qwen35)
  list(APPEND _me_defs MULTIENGINE_HAVE_QWEN35=1)
endif()

# Gemma 4 (arch gemma4), with the native experts: its decode misses run on the CPU pool's ggml-cpu rows (the CPU lane)
if(STRATA_NATIVE_EXPERTS)
  add_subdirectory(src/gemma4)
  list(APPEND _me_families multiengine_gemma4)
  list(APPEND _me_defs MULTIENGINE_HAVE_GEMMA4=1)
  # flint (arch flint), on the same pieces as Gemma 4 (its GGUF reader, its frozen core library g4core)
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/flint/CMakeLists.txt)
    add_subdirectory(src/flint)
    list(APPEND _me_families multiengine_flint)
    list(APPEND _me_defs MULTIENGINE_HAVE_FLINT=1)
  endif()
endif()

# raon1 (arch raon1, its --glm-pack packs): our port onto Project Maya's GLM engine at v1.1.27 (MIT), vendored under
# third_party/project-maya with its own CMake.  OFF by default, and guarded on that CMake existing, so a tree without it
# configures and builds the other families unchanged.
option(STRATA_BUILD_RAON1 "Build the raon1 family library (multiengine_raon1)" OFF)
if(STRATA_BUILD_RAON1 AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/project-maya/CMakeLists.txt)
  add_subdirectory(third_party/project-maya)
  list(APPEND _me_families multiengine_raon1)
  list(APPEND _me_defs MULTIENGINE_HAVE_RAON1=1)
endif()

set_directory_properties(PROPERTIES COMPILE_DEFINITIONS "${_me_root_defs}")

# ---- Project Maya's programs as libraries (Maya's own build) -------------------------------------------------------
# third_party/maya is Maya's tree as Maya ships it (written by src/multiengine/sync_maya.py).  Its targets carry the
# same names as upstream Strata's (strata_core, strata_kernels, ...), so it is configured and built as a CMake project of
# its own, in <build>/maya, with the options Maya's own setup passes (maya.py: Release, CUDA, the native experts, the
# same pinned ggml checkout) and the engine's compilers and GPU targets.  cmake/multiengine_maya.cmake, read inside that
# build, adds one library per Maya program and has them written here, beside the launcher.  OFF by default, and guarded
# on the tree being there.  MULTIENGINE_MAYA_TESTS also configures Maya's own tests there (built only when asked for).
option(STRATA_BUILD_MAYA "Build Project Maya's family libraries (multiengine_maya, _maya_ds4, _maya_mm3)" OFF)
option(MULTIENGINE_MAYA_TESTS "Configure Project Maya's own tests in its build (STRATA_BUILD_TESTS there)" OFF)
set(_me_external)
if(STRATA_BUILD_MAYA AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/maya/CMakeLists.txt)
  include(ExternalProject)
  set(_me_maya_args
    -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
    -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
    -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
    -DCMAKE_CUDA_COMPILER=${CMAKE_CUDA_COMPILER}
    # its static libraries go into the Maya shared libraries: position-independent (Linux needs it; MSVC ignores it)
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    -DSTRATA_ENABLE_CUDA=ON
    -DSTRATA_NATIVE_EXPERTS=ON
    -DSTRATA_BUILD_TESTS=${MULTIENGINE_MAYA_TESTS}
    -DSTRATA_GGML_DIR=${STRATA_GGML_DIR}
    -DCMAKE_PROJECT_strata_INCLUDE=${CMAKE_CURRENT_SOURCE_DIR}/cmake/multiengine_maya.cmake
    -DMULTIENGINE_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
    -DMULTIENGINE_MAYA_OUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR})
  if(CMAKE_MAKE_PROGRAM)
    list(APPEND _me_maya_args -DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM})
  endif()
  if(CUDAToolkit_ROOT)
    list(APPEND _me_maya_args -DCUDAToolkit_ROOT=${CUDAToolkit_ROOT})
  endif()
  ExternalProject_Add(multiengine_maya_build
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/maya
    BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/maya
    CMAKE_GENERATOR ${CMAKE_GENERATOR}
    CMAKE_ARGS ${_me_maya_args}
    CMAKE_CACHE_ARGS -DCMAKE_CUDA_ARCHITECTURES:STRING=${CMAKE_CUDA_ARCHITECTURES}
    BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target multiengine_maya_libraries
    INSTALL_COMMAND ""
    BUILD_ALWAYS ON
    USES_TERMINAL_CONFIGURE ON
    USES_TERMINAL_BUILD ON)
  list(APPEND _me_external multiengine_maya_build)
  list(APPEND _me_defs MULTIENGINE_HAVE_MAYA=1)
endif()

# ---- upstream's engine as a library: the `strata` target's program, compiled and linked as upstream does it ---------
add_library(multiengine_strata SHARED src/multiengine/strata_family.cpp)
foreach(_p LINK_LIBRARIES INCLUDE_DIRECTORIES COMPILE_DEFINITIONS COMPILE_OPTIONS COMPILE_FEATURES LINK_OPTIONS)
  get_target_property(_v strata ${_p})
  if(_v)
    set_property(TARGET multiengine_strata APPEND PROPERTY ${_p} ${_v})
  endif()
endforeach()
set_target_properties(multiengine_strata PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)

# every family library beside the launcher, which loads them from its own folder
foreach(_t IN LISTS _me_families)
  set_target_properties(${_t} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
                                         LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
endforeach()

# ---- the launcher -------------------------------------------------------------------------------------------------
add_executable(multiengine src/multiengine/launcher.cpp)
strata_utf8_manifest(multiengine)   # argv, fopen and std::ifstream in UTF-8, for the launcher and every family it loads
target_compile_definitions(multiengine PRIVATE ${_me_defs})
target_compile_options(multiengine PRIVATE ${_me_warnings})
if(WIN32)
  target_link_libraries(multiengine PRIVATE dbghelp)   # the crash filter symbolizes the faulting stack
else()
  target_link_libraries(multiengine PRIVATE ${CMAKE_DL_LIBS})
endif()
add_dependencies(multiengine ${_me_families} ${_me_external})   # `--target multiengine` builds the whole engine

# ---- strata-prepare: what a download needs before the engine runs it, with no Python ------------------------------
# src/prepare (docs/superpowers/specs/2026-10-27-strata-prepare-design.md): upstream's tools/iq_pack.py as a native
# program, byte for byte; built beside the launcher, with it (`--target multiengine`).
set(_me_prepare)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/prepare/CMakeLists.txt)
  add_subdirectory(src/prepare)
  set_target_properties(strata-prepare PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
  add_dependencies(multiengine strata-prepare)
  set(_me_prepare strata-prepare)
endif()

# ---- installing: the runtime files only ---------------------------------------------------------------------------
# `cmake --install <build> --component runtime --prefix <out>` writes the launcher, every family library and strata_kq
# (opened on demand by the prompt path) into <out>/engine, with the MSVC runtime beside them on Windows (Microsoft's
# app-local deployment); make.py at the repository root runs it. `--component cuda` adds the CUDA toolkit's cuBLAS when
# MULTIENGINE_BUNDLE_CUBLAS is on (the NVIDIA CUDA EULA's Attachment A lists cuBLAS as redistributable), so the folder
# needs only an NVIDIA driver. Import libraries go to a `development` component that make.py never installs.
option(MULTIENGINE_BUNDLE_CUBLAS "Install the CUDA toolkit's cuBLAS libraries beside the engine (component cuda)" OFF)
set(_me_install_targets multiengine ${_me_families} ${_me_prepare})
# the expert profiles upstream ships (data/, MIT), which a Qwen3.8 family card names (--expert-profile), and the ones
# Maya's setup starts its families from (third_party/maya/data, MIT): the server passes expert-profile-glm.bin to every
# GLM-5.3 launch, as Maya's setup writes it into every GLM config
install(FILES data/expert-profile.bin data/expert-profile-coder.bin DESTINATION engine/profiles COMPONENT runtime)
# the MTP draft layer's token subset upstream's setup copies into every mtp/rt (data/draft_vocab.bin, MIT; setup.py
# refresh_draft_vocab, cjk by default), for strata-prepare mtp --draft-vocab (owner 2026-10-29)
install(FILES data/draft_vocab.bin DESTINATION engine/data COMPONENT runtime)
if(_me_external)
  install(FILES third_party/maya/data/expert-profile-glm.bin third_party/maya/data/expert-profile-ds4.bin
                third_party/maya/data/expert-profile-mm3.bin DESTINATION engine/profiles COMPONENT runtime)
  # MiniMax-M3.1-Flash (owner 2026-10-28): Maya's chat template, which the server hands strata-prepare for every MiniMax
  # pack, and the MiniMax Community License that covers it (its notice goes with every copy)
  install(FILES third_party/maya/tools/mm3/chat_template.jinja third_party/maya/LICENSE.minimax-m31
          DESTINATION engine/templates/minimax-m3.1 COMPONENT runtime)
endif()
if(TARGET strata_kq)
  list(APPEND _me_install_targets strata_kq)
endif()
if(NOT WIN32)
  # the installed libraries find each other, and the bundled cuBLAS, in their own folder
  set_target_properties(${_me_install_targets} PROPERTIES INSTALL_RPATH "$ORIGIN")
endif()
install(TARGETS ${_me_install_targets}
        RUNTIME DESTINATION engine COMPONENT runtime
        LIBRARY DESTINATION engine COMPONENT runtime
        ARCHIVE DESTINATION lib COMPONENT development)
if(_me_external)
  # Maya's libraries are built by Maya's own project into this folder (cmake/multiengine_maya.cmake)
  foreach(_lib multiengine_maya multiengine_maya_ds4 multiengine_maya_mm3)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_SHARED_LIBRARY_PREFIX}${_lib}${CMAKE_SHARED_LIBRARY_SUFFIX}
            DESTINATION engine COMPONENT runtime)
  endforeach()
endif()
if(MSVC)
  set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION engine)
  set(CMAKE_INSTALL_SYSTEM_RUNTIME_COMPONENT runtime)
  set(CMAKE_INSTALL_UCRT_LIBRARIES OFF)   # the Universal CRT is part of Windows 10 and later
  include(InstallRequiredSystemLibraries)
endif()
if(MULTIENGINE_BUNDLE_CUBLAS)
  find_package(CUDAToolkit REQUIRED)
  set(_me_cublas)
  if(WIN32)
    file(GLOB _me_cublas "${CUDAToolkit_BIN_DIR}/cublas64_*.dll" "${CUDAToolkit_BIN_DIR}/cublasLt64_*.dll"
                         "${CUDAToolkit_BIN_DIR}/x64/cublas64_*.dll" "${CUDAToolkit_BIN_DIR}/x64/cublasLt64_*.dll")
    if(_me_cublas)
      install(FILES ${_me_cublas} DESTINATION engine COMPONENT cuda)
    endif()
  else()
    # the real files, each under the name the libraries load (libcublas.so.13: its SONAME link), never a link
    file(GLOB _me_cublas_links "${CUDAToolkit_LIBRARY_DIR}/libcublas.so.*" "${CUDAToolkit_LIBRARY_DIR}/libcublasLt.so.*")
    foreach(_link IN LISTS _me_cublas_links)
      get_filename_component(_name ${_link} NAME)
      if(_name MATCHES "^libcublas(Lt)?\\.so\\.[0-9]+$")
        get_filename_component(_real ${_link} REALPATH)
        install(FILES ${_real} DESTINATION engine RENAME ${_name} COMPONENT cuda)
        list(APPEND _me_cublas ${_real})
      endif()
    endforeach()
  endif()
  if(NOT _me_cublas)
    message(FATAL_ERROR "MULTIENGINE_BUNDLE_CUBLAS: no cuBLAS library found in the CUDA toolkit (${CUDAToolkit_BIN_DIR})")
  endif()
endif()
