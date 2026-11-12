# Remove upstream's compiler-wide approximate-math opt-in from the shipped
# CUDA build by default. Explicit MULTIENGINE_FAST_MATH restores upstream's
# flags. This leaves source-written intrinsics and normal FMA unchanged.
# Apply after all targets exist, including in the separate Maya configure.
include_guard(GLOBAL)
option(MULTIENGINE_FAST_MATH "Keep upstream CUDA fast-math flags (may reduce numerical accuracy; requires rebuild)" OFF)
if(MULTIENGINE_FAST_MATH)
  message(WARNING "MULTIENGINE_FAST_MATH is ON: upstream CUDA approximate math may reduce accuracy. Rebuild with OFF to restore the default.")
endif()

function(_multiengine_without_fast_math output)
  set(_options "${ARGN}")
  # Options also occur inside generator expressions and space-separated option
  # groups. Retain delimiters so nested IF / COMPILE_LANGUAGE expressions stay
  # valid; do not match definitions or filenames merely containing the name.
  set(_previous "")
  while(NOT "${_previous}" STREQUAL "${_options}")
    set(_previous "${_options}")
    string(REGEX REPLACE " +--?use_fast_math( +|[;>,]|$)" "\\1"
           _options "${_options}")
    string(REGEX REPLACE "(^|[;:,]) *--?use_fast_math *([;>,]|$)" "\\1\\2"
           _options "${_options}")
  endwhile()
  set(${output} "${_options}" PARENT_SCOPE)
endfunction()

function(multiengine_preserve_cuda_accuracy directory)
  if(MULTIENGINE_FAST_MATH)
    return()
  endif()
  get_property(_targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
  foreach(_target IN LISTS _targets)
    get_target_property(_options "${_target}" COMPILE_OPTIONS)
    if(_options)
      _multiengine_without_fast_math(_options "${_options}")
      set_property(TARGET "${_target}" PROPERTY COMPILE_OPTIONS "${_options}")
    endif()
    get_target_property(_sources "${_target}" SOURCES)
    foreach(_source IN LISTS _sources)
      if(_source MATCHES "\\$<" OR _source STREQUAL "_sources-NOTFOUND")
        continue()
      endif()
      # Source properties are directory-scoped: the same source can carry
      # distinct options in the root, Gemma, and raon1 directories.
      if(NOT IS_ABSOLUTE "${_source}")
        get_filename_component(_source "${_source}" ABSOLUTE BASE_DIR "${directory}")
      endif()
      get_source_file_property(_options "${_source}" DIRECTORY "${directory}" COMPILE_OPTIONS)
      if(_options)
        _multiengine_without_fast_math(_options "${_options}")
        set_property(SOURCE "${_source}" DIRECTORY "${directory}"
                     PROPERTY COMPILE_OPTIONS "${_options}")
      endif()
    endforeach()
  endforeach()
  get_property(_children DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
  foreach(_child IN LISTS _children)
    multiengine_preserve_cuda_accuracy("${_child}")
  endforeach()
endfunction()
