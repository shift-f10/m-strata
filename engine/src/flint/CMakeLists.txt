# flint (GGUF architecture flint): multiengine_flint.dll, a family library of multiengine whose only export is
# flint_main (f_run.cpp). The multiengine launcher loads it for a flint model. Design:
# docs/superpowers/specs/2026-10-27-flint-engine-design.md.
#
# Built as Gemma 4 is (engine/src/gemma4), on the same pieces: the bounded GGUF reader and the file access are Gemma 4's
# sources (g4_gguf.cpp, g4_file.cpp, unchanged), compiled here as flint's own objects; the Strata core is Gemma 4's
# frozen copy (the g4core library, src/gemma4/core), linked into this DLL as its own copy. A core file flint would have
# to change is copied into src/flint first, so Gemma 4 never moves. On MSVC a DLL exports only __declspec(dllexport),
# so the strata:: and g4:: symbols stay internal. Included from cmake/multiengine.cmake after src/gemma4.

if(MSVC)
    set(FL_HOST_WARNINGS /W4 /WX /permissive- /utf-8 /EHsc /Zc:__cplusplus /wd4324)
    set(FL_CUDA_WARNINGS -Werror=all-warnings -Xcompiler=/W4,/WX,/utf-8,/EHsc)
else()
    set(FL_HOST_WARNINGS -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)
    set(FL_CUDA_WARNINGS -Werror=all-warnings -Xcompiler=-Wall,-Wextra,-Werror)
endif()
set(_fl_g4 ${CMAKE_CURRENT_SOURCE_DIR}/../gemma4)
set(_fl_tests ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/flint)
set(_fl_g4_tests ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/gemma4)

# ---- the host half: file, GGUF reader (Gemma 4's), model guard, command line, profile fill (no CUDA)
add_library(strata_flint_host STATIC ${_fl_g4}/g4_file.cpp ${_fl_g4}/g4_gguf.cpp f_model.cpp f_cli.cpp f_profile.cpp)
target_include_directories(strata_flint_host PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${_fl_g4})
target_compile_options(strata_flint_host PRIVATE ${FL_HOST_WARNINGS})

# ---- the kernels
add_library(strata_flint_kernels STATIC f_kernels.cu)
set_target_properties(strata_flint_kernels PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(strata_flint_kernels PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} PRIVATE ${_strata_gpu_include_directories})
target_compile_options(strata_flint_kernels PRIVATE $<$<COMPILE_LANGUAGE:CUDA>:${FL_CUDA_WARNINGS}>)
target_link_libraries(strata_flint_kernels PRIVATE ${_strata_gpu_runtime_target})

# ---- the DLL: the engine and the entry
add_library(multiengine_flint SHARED f_engine.cu f_lane.cpp f_run.cpp)
set_target_properties(multiengine_flint PROPERTIES CXX_VISIBILITY_PRESET hidden CUDA_VISIBILITY_PRESET hidden
                      VISIBILITY_INLINES_HIDDEN ON)
target_include_directories(multiengine_flint PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${_strata_gpu_include_directories})
target_link_libraries(multiengine_flint PRIVATE strata_flint_host strata_flint_kernels g4core)
target_compile_options(multiengine_flint PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${FL_HOST_WARNINGS}>
                                                 $<$<COMPILE_LANGUAGE:CUDA>:${FL_CUDA_WARNINGS}>)

# The family's own tests: host cases (shard names, the model guard, the command line, the sampler, the profile fill).
option(STRATA_BUILD_FLINT_TESTS "Build the flint family's host and kernel tests" OFF)
if(STRATA_BUILD_TESTS OR STRATA_BUILD_FLINT_TESTS)
    enable_testing()
    add_executable(f_host_tests ${_fl_tests}/f_host_tests.cpp)
    target_include_directories(f_host_tests PRIVATE ${_fl_tests} ${_fl_g4_tests})
    target_link_libraries(f_host_tests PRIVATE strata_flint_host)
    target_compile_options(f_host_tests PRIVATE ${FL_HOST_WARNINGS})
    set(FL_HOST_CASES split_names_follow_gguf_split model_guard_accepts_the_flint_shape
        model_guard_takes_one_file_and_q8_0_weights model_guard_reads_the_rope_pattern_on_its_own
        model_guard_refuses_other_shapes cli_accepts_the_serve_flags cli_refuses_bad_and_test_only_flags_with_serve
        gen_lines_follow_the_q36_grammar sampler_and_seeds_match_gemma4s profile_fills_each_moe_layer_with_its_top_experts
        info_line_reports_the_expert_split)
    foreach(case IN LISTS FL_HOST_CASES)
        add_test(NAME f_host_tests.${case} COMMAND f_host_tests --case ${case})
    endforeach()

    # GPU kernel cases against a double-precision CPU reference
    add_executable(f_kernel_tests ${_fl_tests}/f_kernel_tests.cu)
    target_include_directories(f_kernel_tests PRIVATE ${_fl_tests} ${_fl_g4_tests} ${_strata_gpu_include_directories})
    target_link_libraries(f_kernel_tests PRIVATE strata_flint_kernels ${_strata_gpu_runtime_target})
    target_compile_options(f_kernel_tests PRIVATE $<$<COMPILE_LANGUAGE:CUDA>:${FL_CUDA_WARNINGS}>)
    set(FL_KERNEL_CASES matvec_matches_cpu_for_each_kind norms_adds_and_embedding_match_cpu qkv_post_follows_the_layer
        decode_attention_with_sinks_matches_cpu prompt_softmax_masks_and_sinks_like_the_model router_picks_the_top_six_by_sigmoid
        swiglu_and_combines_match_cpu argmax_topk_dequant_and_plan_match_cpu)
    foreach(case IN LISTS FL_KERNEL_CASES)
        add_test(NAME f_kernel_tests.${case} COMMAND f_kernel_tests --case ${case})
        set_tests_properties(f_kernel_tests.${case} PROPERTIES LABELS gpu)
    endforeach()
endif()
