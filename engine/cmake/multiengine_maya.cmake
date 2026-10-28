# cmake/multiengine_maya.cmake - Project Maya's three programs as family libraries of multiengine.
#
# Read inside Maya's own build, never the engine's: cmake/multiengine.cmake configures third_party/maya (Maya's tree as
# Maya ships it, not edited) as a project of its own, with this file as CMAKE_PROJECT_strata_INCLUDE.  CMake reads it
# right after Maya's project() call; it then puts itself off to the end of Maya's root directory, where every Maya
# target exists, and adds there - in Maya's directory, so its directory settings and source-file settings apply as they
# do to Maya's own programs - one shared library per Maya program, each compiled from that program's own sources with
# its libraries, definitions and flags:
#   multiengine_maya      `maya`      GLM-5.3-Flash (glm5-next)       maya_main      (src/multiengine/maya_family.cpp)
#   multiengine_maya_ds4  `maya-ds4`  DeepSeek-V4.1-Flash (deepseek41) maya_ds4_main  (src/multiengine/maya_ds4_family.cpp)
#   multiengine_maya_mm3  `maya-mm3`  MiniMax-M3.1-Flash (minimax-m3)  maya_mm3_main  (src/multiengine/maya_mm3_family.cpp)
# A library per program, as Maya builds a program per model: their sources were never linked together, so none of their
# same-named definitions can meet.  Each program's main source is compiled by its wrapper with main() renamed; the
# target multiengine_maya_libraries builds them all into MULTIENGINE_MAYA_OUTPUT_DIR (beside the launcher).  Maya's own
# executables stay as Maya builds them, for checking the libraries against.
if(NOT DEFINED _me_maya_deferred)
  set(_me_maya_deferred ON)
  # (a deferred call reads its arguments when it runs, so the path is written into it now)
  cmake_language(EVAL CODE "cmake_language(DEFER CALL include [==[${CMAKE_CURRENT_LIST_FILE}]==])")
  return()
endif()

if(NOT MULTIENGINE_SOURCE_DIR OR NOT MULTIENGINE_MAYA_OUTPUT_DIR)
  message(FATAL_ERROR "multiengine_maya.cmake: MULTIENGINE_SOURCE_DIR and MULTIENGINE_MAYA_OUTPUT_DIR are set by "
                      "cmake/multiengine.cmake")
endif()

set(_me_maya_libraries)
function(_me_maya_family lib program main_source wrapper)
  if(NOT TARGET ${program})
    message(STATUS "multiengine: Maya's ${program} is not built here, so ${lib} is not either")
    return()
  endif()
  get_target_property(_src ${program} SOURCES)
  list(FIND _src ${main_source} _at)
  if(_at LESS 0)
    message(FATAL_ERROR "multiengine: ${program} no longer compiles ${main_source} - update ${wrapper}")
  endif()
  list(REMOVE_AT _src ${_at})
  add_library(${lib} SHARED ${MULTIENGINE_SOURCE_DIR}/src/multiengine/${wrapper} ${_src})
  foreach(_p LINK_LIBRARIES INCLUDE_DIRECTORIES COMPILE_DEFINITIONS COMPILE_OPTIONS COMPILE_FEATURES LINK_OPTIONS
             CUDA_ARCHITECTURES)
    get_target_property(_v ${program} ${_p})
    if(_v)
      set_property(TARGET ${lib} PROPERTY ${_p} ${_v})
    endif()
  endforeach()
  # (DEFINE_SYMBOL empty: no <lib>_EXPORTS define, so every compile line is the program's own, token for token)
  set_target_properties(${lib} PROPERTIES DEFINE_SYMBOL "" CXX_VISIBILITY_PRESET hidden CUDA_VISIBILITY_PRESET hidden
                                          VISIBILITY_INLINES_HIDDEN ON
                                          RUNTIME_OUTPUT_DIRECTORY ${MULTIENGINE_MAYA_OUTPUT_DIR}
                                          LIBRARY_OUTPUT_DIRECTORY ${MULTIENGINE_MAYA_OUTPUT_DIR})
  if(NOT WIN32)
    # installed beside the launcher (cmake/multiengine.cmake's install rules): find the bundled cuBLAS there
    set_target_properties(${lib} PROPERTIES BUILD_RPATH "$ORIGIN")
  endif()
  set(_me_maya_libraries ${_me_maya_libraries} ${lib} PARENT_SCOPE)
endfunction()

_me_maya_family(multiengine_maya maya src/program/generate.cpp maya_family.cpp)
_me_maya_family(multiengine_maya_ds4 maya-ds4 src/ds4/ds4_main.cpp maya_ds4_family.cpp)
_me_maya_family(multiengine_maya_mm3 maya-mm3 src/mm3/mm3_main.cpp maya_mm3_family.cpp)

add_custom_target(multiengine_maya_libraries)
if(_me_maya_libraries)
  add_dependencies(multiengine_maya_libraries ${_me_maya_libraries})
endif()
