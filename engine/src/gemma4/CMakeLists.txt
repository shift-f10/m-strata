# Gemma 4 26B-A4B (GGUF architecture gemma4): multiengine_gemma4.dll, a family library of multiengine whose only
# export is gemma4_main (g4_run.cpp). The multiengine launcher loads it for a gemma4 model.
#
# ISOLATED like the GLM DLL (third_party/project-maya): it compiles its own copy of the Strata core it runs on
# (core/, VENDORED.txt), never the engine's src/ or include/, so an update of the upstream engine cannot change what
# Gemma 4 computes, and Gemma 4's needs never change the other families. On MSVC a DLL exports only
# __declspec(dllexport), so its copy of the strata:: symbols stays internal (elsewhere: hidden visibility). It shares
# only the pinned ggml (the parent's ggml-cpu / ggml-base targets and the llama.cpp checkout's ggml-cuda MMQ sources:
# third_party/ggml/VERSION.txt, the same commit for every family) and the CUDA toolkit.
#
# Included from cmake/multiengine.cmake at the end of the engine root, where ggml, the llama.cpp checkout and the
# CUDA variables (_strata_gpu_*) are all defined. Its tests and parity tools are engine/tests/gemma4.

if(MSVC)
    # C4324 (a struct padded to its alignas) is informational: the CPU pool's headers align their row buffers on purpose
    set(G4_HOST_WARNINGS /W4 /WX /permissive- /utf-8 /EHsc /Zc:__cplusplus /wd4324)
    set(G4_CUDA_WARNINGS -Werror=all-warnings -Xcompiler=/W4,/WX,/utf-8,/EHsc)
else()
    set(G4_HOST_WARNINGS -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)
    set(G4_CUDA_WARNINGS -Werror=all-warnings -Xcompiler=-Wall,-Wextra,-Werror)
endif()
set(_g4_tests ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/gemma4)
set(_g4_core ${CMAKE_CURRENT_SOURCE_DIR}/core)
if(STRATA_GGML_DIR)
    set(_g4_gg ${STRATA_GGML_DIR}/ggml)
elseif(DEFINED strata_llamacpp_SOURCE_DIR)
    set(_g4_gg ${strata_llamacpp_SOURCE_DIR}/ggml)
else()
    message(FATAL_ERROR "multiengine_gemma4 needs the llama.cpp checkout (STRATA_GGML_DIR or the fetched pin)")
endif()
set(_g4_ggcuda ${_g4_gg}/src/ggml-cuda)

# ---- g4core: the vendored Strata core (upstream Strata v0.2.4.2 + the Gemma 4 generalisations), each unit compiled
# as the engine compiled it when Gemma 4 was validated (the same defines and ISA flags per unit). Left out: the
# trellis and K-quant MMQ instances (STRATA_MMQ_TQ, STRATA_KQ_DSO) - formats Gemma 4 never has; the MMQ switch then
# has no case for them.
set(_g4c_cuda src/core/device.cu src/core/pinned.cu src/kernels/cuda/elementwise.cu src/kernels/cuda/iq_kernels.cu
    src/kernels/cuda/quantize_act.cu src/kernels/cuda/s2_expert_grouped.cu src/kernels/cuda/verify_kernels.cu)
set(_g4c_core_cxx src/core/graph.cpp src/platform/memory.cpp)
set(_g4c_engine src/core/expert_cache.cpp src/core/expert_source.cpp src/core/peer_experts.cpp src/core/remote_expert_opt.cu
    src/core/remote_experts.cpp src/core/vmm.cpp)
set(_g4c_cpu src/kernels/cpu/expert.cpp src/kernels/cpu/expert_layout.cpp src/kernels/cpu/hadamard.cpp
    src/kernels/cpu/iq_avx1.cpp src/kernels/cpu/iq_avx2.cpp src/kernels/cpu/iq_avx512.cpp src/kernels/cpu/kq_avx1.cpp
    src/kernels/cpu/kq_avx2.cpp
    src/kernels/cpu/native_expert.cpp src/kernels/cpu/nvfp4_avx2.cpp src/kernels/cpu/nvfp4_avx512.cpp src/kernels/cpu/pool.cpp
    src/kernels/cpu/q2_avx1.cpp src/kernels/cpu/q2_avx2.cpp src/kernels/cpu/tq_avx2.cpp src/kernels/cpu/tq_avx512.cpp
    src/kernels/cpu/tq_cpu.cpp)
set(_g4c_mmq src/prefill/moe_mmq.cu src/prefill/ggml_cuda_host.cu src/prefill/mmq_nvfp4_w4a8.cu)
set(_g4c_w4a4 src/prefill/mmq_nvfp4_w4a4.cu)
foreach(_list _g4c_cuda _g4c_core_cxx _g4c_engine _g4c_cpu _g4c_mmq _g4c_w4a4)
    list(TRANSFORM ${_list} PREPEND ${_g4_core}/)
endforeach()
set(_g4c_mmq_ggml ${_g4_ggcuda}/quantize.cu)
foreach(_t q2_0 iq2_xxs iq2_xs iq2_s iq3_xxs iq3_s iq4_nl iq4_xs q8_0)
    list(APPEND _g4c_mmq_ggml ${_g4_ggcuda}/template-instances/mmq-instance-${_t}.cu)
endforeach()

add_library(g4core STATIC ${_g4c_cuda} ${_g4c_core_cxx} ${_g4c_engine} ${_g4c_cpu} ${_g4c_mmq} ${_g4c_mmq_ggml})
set_target_properties(g4core PROPERTIES POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden CUDA_VISIBILITY_PRESET hidden
                      VISIBILITY_INLINES_HIDDEN ON)
target_include_directories(g4core BEFORE PUBLIC ${_g4_core}/include
                           PRIVATE ${_g4_core}/src ${_g4_core}/third_party/ggml ${_g4_gg}/include ${_g4_gg}/src ${_g4_ggcuda}
                           ${_strata_gpu_include_directories})
target_compile_definitions(g4core PRIVATE STRATA_VERSION="0.1.49.2" STRATA_Q6K_EXPERTS=1)
set_property(SOURCE ${_g4c_cuda} ${_g4c_core_cxx} APPEND PROPERTY COMPILE_DEFINITIONS STRATA_HAVE_DFLASH2=1)
set_property(SOURCE ${_g4c_engine} APPEND PROPERTY COMPILE_DEFINITIONS STRATA_HAVE_DFLASH2=1 STRATA_NATIVE_EXPERTS=1)
set_property(SOURCE ${_g4c_cpu} APPEND PROPERTY COMPILE_DEFINITIONS STRATA_NATIVE_EXPERTS=1)
set_property(SOURCE ${_g4_core}/src/kernels/cpu/iq_avx2.cpp ${_g4_core}/src/kernels/cpu/q2_avx2.cpp APPEND PROPERTY
             COMPILE_DEFINITIONS ${_strata_avxvnni})
if(MSVC)
    foreach(_f expert iq_avx512 nvfp4_avx512 tq_avx512)
        set_property(SOURCE ${_g4_core}/src/kernels/cpu/${_f}.cpp APPEND PROPERTY COMPILE_OPTIONS /arch:AVX512)
    endforeach()
    foreach(_f iq_avx2 kq_avx2 nvfp4_avx2 q2_avx2 tq_avx2)
        set_property(SOURCE ${_g4_core}/src/kernels/cpu/${_f}.cpp APPEND PROPERTY COMPILE_OPTIONS /arch:AVX2)
    endforeach()
    foreach(_f iq_avx1 kq_avx1 q2_avx1)
        set_property(SOURCE ${_g4_core}/src/kernels/cpu/${_f}.cpp APPEND PROPERTY COMPILE_OPTIONS /arch:AVX)
    endforeach()
    set(_g4_bigobj -Xcompiler=/bigobj)
else()
    message(FATAL_ERROR "multiengine_gemma4: only the MSVC build's per-unit ISA flags are written down here")
endif()
# the MMQ units as the engine's strata_mmq compiles them
set_property(SOURCE ${_g4c_mmq} ${_g4c_mmq_ggml} APPEND PROPERTY COMPILE_OPTIONS --extended-lambda -use_fast_math ${_g4_bigobj})
# the opt-in NVFP4 W4A4 unit on its own, as the engine's strata_mmq_w4a4: RTX 50's FP4 MMA exists only in the
# arch-specific target, so its archs 12x become 12xa (the rest of the build is unchanged)
set(_g4_w4a4_archs ${CMAKE_CUDA_ARCHITECTURES})
list(TRANSFORM _g4_w4a4_archs REPLACE "^(12[0-9])$" "\\1a")
list(TRANSFORM _g4_w4a4_archs REPLACE "^(12[0-9])-(real|virtual)$" "\\1a-\\2")
add_library(g4core_w4a4 STATIC ${_g4c_w4a4})
set_target_properties(g4core_w4a4 PROPERTIES CUDA_ARCHITECTURES "${_g4_w4a4_archs}" POSITION_INDEPENDENT_CODE ON)
target_include_directories(g4core_w4a4 PRIVATE ${_g4_core}/include ${_g4_core}/src ${_g4_gg}/include ${_g4_gg}/src ${_g4_ggcuda})
target_compile_definitions(g4core_w4a4 PRIVATE STRATA_VERSION="0.1.49.2" STRATA_Q6K_EXPERTS=1)
target_compile_options(g4core_w4a4 PRIVATE $<$<COMPILE_LANGUAGE:CUDA>:--extended-lambda -use_fast_math ${_g4_bigobj}>)
target_link_libraries(g4core_w4a4 PUBLIC ggml-base ${_strata_gpu_runtime_target})
target_link_libraries(g4core PRIVATE g4core_w4a4)
target_link_libraries(g4core PUBLIC ggml-cpu ggml-base ${_strata_gpu_runtime_target} ${_strata_gpu_blas_target})
find_package(Threads REQUIRED)
target_link_libraries(g4core PUBLIC Threads::Threads)

# ---- the Gemma 4 sources: the host half (file, GGUF reader, model guard, command line, profile; no CUDA), the
# kernels, then the DLL
add_library(strata_gemma4_host STATIC g4_file.cpp g4_gguf.cpp g4_model.cpp g4_cli.cpp g4_profile.cpp g4_spec.cpp)
target_include_directories(strata_gemma4_host PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(strata_gemma4_host PRIVATE ${G4_HOST_WARNINGS})

add_library(strata_gemma4_kernels STATIC g4_kernels.cu)
set_target_properties(strata_gemma4_kernels PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(strata_gemma4_kernels PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} PRIVATE ${_strata_gpu_include_directories})
target_compile_options(strata_gemma4_kernels PRIVATE $<$<COMPILE_LANGUAGE:CUDA>:${G4_CUDA_WARNINGS}>)
target_link_libraries(strata_gemma4_kernels PRIVATE ${_strata_gpu_runtime_target})

add_library(multiengine_gemma4 SHARED g4_engine.cu g4_lane.cpp g4_mtp.cu g4_run.cpp)
set_target_properties(multiengine_gemma4 PROPERTIES CXX_VISIBILITY_PRESET hidden CUDA_VISIBILITY_PRESET hidden
                      VISIBILITY_INLINES_HIDDEN ON)
target_include_directories(multiengine_gemma4 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${_strata_gpu_include_directories})
target_link_libraries(multiengine_gemma4 PRIVATE strata_gemma4_host strata_gemma4_kernels g4core)
target_compile_options(multiengine_gemma4 PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${G4_HOST_WARNINGS}>
                                                  $<$<COMPILE_LANGUAGE:CUDA>:${G4_CUDA_WARNINGS}>)

# The family's own tests: host cases (GGUF, model guard, CLI, sampler, profile), the CPU lane on its core copy,
# and GPU kernel cases against a CPU reference.
option(STRATA_BUILD_GEMMA4_TESTS "Build the Gemma 4 family's host and kernel tests" OFF)
if(STRATA_BUILD_TESTS OR STRATA_BUILD_GEMMA4_TESTS)
    enable_testing()
    add_executable(g4_host_tests ${_g4_tests}/g4_host_tests.cpp)
    target_include_directories(g4_host_tests PRIVATE ${_g4_tests})
    target_link_libraries(g4_host_tests PRIVATE strata_gemma4_host)
    target_compile_options(g4_host_tests PRIVATE ${G4_HOST_WARNINGS})
    set(G4_HOST_CASES
        gguf_parses_a_valid_header gguf_refuses_bad_magic_version_and_truncation
        gguf_refuses_oversized_counts_and_strings gguf_refuses_bad_tensors tensor_bytes_follow_the_types
        model_guard_accepts_the_26b_a4b_shape model_guard_refuses_other_shapes
        cli_accepts_the_serve_flags cli_refuses_bad_and_test_only_flags_with_serve gen_lines_follow_the_q36_grammar
        sampler_matches_the_q36_rules reuse_keeps_exact_prefixes_within_the_ring
        sorted_candidates_sample_like_the_full_sampler unseeded_requests_draw_fresh_seeds info_line_reports_the_expert_split
        profile_fills_each_layer_with_its_top_experts
        assistant_guard_accepts_the_26b_a4b_drafter assistant_guard_refuses_other_shapes
        assistant_vocabulary_must_match_the_target cli_takes_the_mtp_flags drafts_stop_below_the_min_p
        acceptance_follows_llama_cpps_sample_and_accept_n draft_budget_leaves_room_in_the_answer_and_context)
    foreach(case IN LISTS G4_HOST_CASES)
        add_test(NAME g4_host_tests.${case} COMMAND g4_host_tests --case ${case})
    endforeach()

    # the CPU lane on the vendored core (native rows, the pool, the window dispatcher) at Gemma 4's geometry:
    # synthetic experts, no GPU work
    add_executable(g4_lane_tests ${_g4_tests}/g4_lane_tests.cpp)
    target_include_directories(g4_lane_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(g4_lane_tests PRIVATE g4core)
    target_compile_options(g4_lane_tests PRIVATE ${G4_HOST_WARNINGS})
    set(G4_LANE_CASES native_fmt_takes_the_gemma4_geometry geglu_rows_match_the_reference pool_runs_gemma4_experts
        window_dispatch_uses_the_layers_width lane_plan_layout_grows_with_the_window
        window_dispatch_rows_are_the_single_token_rows)
    foreach(case IN LISTS G4_LANE_CASES)
        add_test(NAME g4_lane_tests.${case} COMMAND g4_lane_tests --case ${case})
    endforeach()

    add_executable(g4_kernel_tests ${_g4_tests}/g4_kernel_tests.cu)
    target_include_directories(g4_kernel_tests PRIVATE ${_g4_tests} ${_strata_gpu_include_directories})
    target_link_libraries(g4_kernel_tests PRIVATE strata_gemma4_kernels ${_strata_gpu_runtime_target})
    target_compile_options(g4_kernel_tests PRIVATE $<$<COMPILE_LANGUAGE:CUDA>:${G4_CUDA_WARNINGS}>)
    set(G4_KERNEL_CASES
        matvec_matches_cpu_for_each_kind expert_matvec_reads_the_table_in_vram_and_pinned_ram norms_match_cpu
        embedding_and_small_ops_match_cpu router_picks_the_top_eight qkv_post_matches_cpu_for_both_layer_kinds
        decode_attention_matches_cpu decode_kernels_read_the_position_from_the_device prompt_softmax_masks_like_the_model
        dequant_and_gather_match_cpu topk_matches_a_cpu_ranking
        window_rows_are_the_single_rows_bitwise window_plan_groups_entries_by_expert kept_rows_count_their_routing
        scaled_sandwich_norm_matches_cpu drafter_attention_reads_the_keys_before_its_position)
    foreach(case IN LISTS G4_KERNEL_CASES)
        add_test(NAME g4_kernel_tests.${case} COMMAND g4_kernel_tests --case ${case})
        set_tests_properties(g4_kernel_tests.${case} PROPERTIES LABELS gpu)
    endforeach()

    # Box-only checks against the real model through the multiengine launcher: configure with -DG4_MODEL_FILE=<the
    # Q8_K_XL gguf> -DG4_PYTHON=<python> to register them (label "model").
    set(G4_MODEL_FILE "" CACHE FILEPATH "The Gemma 4 26B-A4B GGUF for the model-labelled tests (empty: not registered)")
    set(G4_PYTHON "" CACHE FILEPATH "Python for the model-labelled tests")
    if(G4_MODEL_FILE AND G4_PYTHON)
        # "Write a short poem about the sea, with a title. Use exactly four lines." in the chat template, thinking off
        set(G4_POEM_IDS 2,105,2364,107,6974,496,2822,27355,1003,506,5442,236764,607,496,3822,236761,6890,7121,2390,4463,236761,106,107,105,4368,107,100,45518,107,101)
        set(G4_FRANCE_IDS 2,105,2364,107,3689,563,506,5279,529,7001,236881,25685,528,886,13315,236761,106,107,105,4368,107,100,45518,107,101)
        add_test(NAME g4_model.paths_agree COMMAND ${G4_PYTHON} -I ${_g4_tests}/g4_paths_agree.py
                 $<TARGET_FILE:multiengine> ${G4_MODEL_FILE} ${G4_POEM_IDS})
        add_test(NAME g4_model.serve_smoke COMMAND ${G4_PYTHON} -I ${_g4_tests}/g4_serve_smoke.py
                 $<TARGET_FILE:multiengine> ${G4_MODEL_FILE} ${G4_FRANCE_IDS})
        # a 293-token prompt (g4_prompt_text.txt in the chat template, tokenized by llama.cpp): the streamed MMQ path
        # against the grouped kernel
        add_test(NAME g4_model.prompt_paths COMMAND ${G4_PYTHON} -I ${_g4_tests}/g4_prompt_paths.py
                 $<TARGET_FILE:multiengine> ${G4_MODEL_FILE} ${_g4_tests}/g4_prompt_ids.txt)
        set_tests_properties(g4_model.paths_agree g4_model.serve_smoke g4_model.prompt_paths PROPERTIES LABELS "gpu;model"
                             TIMEOUT 900)
        # MTP (spec §12), with -DG4_MTP_FILE=<the gemma4-assistant drafter gguf>: verify windows bitwise the single decode
        # steps; greedy with the drafter = without it (--tokens); the serve protocol's DONE fields, the decode speed and
        # the acceptance with the drafter; and served answers, greedy and seeded sampled, the same tokens with the drafter
        # (a drafting floor, the tier moving) as without, where nothing depends on the tier's timing (the lane off, no MMQ)
        set(G4_MTP_FILE "" CACHE FILEPATH "The gemma4-assistant drafter GGUF for the MTP model tests (empty: not registered)")
        if(G4_MTP_FILE)
            add_test(NAME g4_model.mtp_window COMMAND ${G4_PYTHON} -I ${_g4_tests}/g4_mtp_window.py
                     $<TARGET_FILE:multiengine> ${G4_MODEL_FILE} ${G4_MTP_FILE} ${_g4_tests}/g4_prompt_ids.txt)
            add_test(NAME g4_model.mtp_greedy COMMAND ${G4_PYTHON} -I ${_g4_tests}/g4_mtp_greedy.py
                     $<TARGET_FILE:multiengine> ${G4_MODEL_FILE} ${G4_MTP_FILE} ${_g4_tests}/g4_prompt_ids.txt ${G4_POEM_IDS} ${G4_FRANCE_IDS})
            add_test(NAME g4_model.mtp_serve COMMAND ${G4_PYTHON} -I ${_g4_tests}/g4_mtp_serve.py
                     $<TARGET_FILE:multiengine> ${G4_MODEL_FILE} ${G4_MTP_FILE} ${_g4_tests}/g4_prompt_ids.txt ${G4_POEM_IDS} ${G4_FRANCE_IDS})
            add_test(NAME g4_model.mtp_serve_exact COMMAND ${G4_PYTHON} -I ${_g4_tests}/g4_mtp_serve.py
                     $<TARGET_FILE:multiengine> ${G4_MODEL_FILE} ${G4_MTP_FILE} ${_g4_tests}/g4_prompt_ids.txt ${G4_POEM_IDS} ${G4_FRANCE_IDS}
                     --rounds 1 --both "--cpu-lane off --expert-slots 50" --mtp-args "--spec 4 --spec-min-p 0.6" --no-mmq
                     --same-gate)
            set_tests_properties(g4_model.mtp_window g4_model.mtp_greedy g4_model.mtp_serve g4_model.mtp_serve_exact PROPERTIES
                                 LABELS "gpu;model" TIMEOUT 1800)
        endif()
    endif()
endif()
