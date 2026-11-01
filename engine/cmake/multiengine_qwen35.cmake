# cmake/multiengine_qwen35.cmake - multiengine_strata's Qwen3.6 engine: upstream's src/qwen35/q36_engine.cu with our
# load progress and read-ahead put in (owner 2026-11-01, the Azure VM: a start cut off by the server's start limit while
# the engine said nothing for 13 minutes).
#
# Upstream's files under engine/ stay as upstream ships them (docs/PROVENANCE.md; src/multiengine/sync_upstream.py writes
# them), so the change is made here, at configure time: upstream's file is read, each insertion is put at a line of its
# that must be there exactly once - an upstream update that moves one stops the configure, naming it - and the result is
# written to <build>/multiengine_qwen35/q36_engine.cu, which multiengine_qwen35 builds with strata_qwen35's own settings.
# multiengine_strata links it instead of strata_qwen35; upstream's strata and strata-q36 executables keep upstream's.
#   - progress: a line at most every 10 s while the weights are read and while the routed experts are placed
#     (src/multiengine/q36_progress.hpp)
#   - read-ahead: upstream's strata::platform::advise_willneed (src/platform/memory.cpp) before each tensor's copy to
#     the GPU and before each layer's experts are copied, so the OS reads them in long runs instead of a page fault at a
#     time (Linux and macOS; nothing on Windows or with STRATA_READ_AHEAD=0)
# Included by cmake/multiengine.cmake when strata_qwen35 exists.

set(_me_q36_in ${CMAKE_CURRENT_SOURCE_DIR}/src/qwen35/q36_engine.cu)
# (not <build>/multiengine/: on Linux that is the launcher itself, the file multiengine)
set(_me_q36_out ${CMAKE_CURRENT_BINARY_DIR}/multiengine_qwen35/q36_engine.cu)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_me_q36_in} ${CMAKE_CURRENT_LIST_FILE})
file(READ ${_me_q36_in} _me_q36)
string(REPLACE "\r\n" "\n" _me_q36 "${_me_q36}")

# _me_q36_put(BEFORE|AFTER <anchor> <text>): <text> beside the one <anchor> in the file
function(_me_q36_put where anchor text)
  string(REPLACE "\r" "" anchor "${anchor}")   # this file's own line endings (a CRLF checkout)
  string(REPLACE "\r" "" text "${text}")
  string(FIND "${_me_q36}" "${anchor}" _first)
  string(FIND "${_me_q36}" "${anchor}" _last REVERSE)
  if(_first EQUAL -1 OR NOT _first EQUAL _last)
    message(FATAL_ERROR "multiengine: upstream's src/qwen35/q36_engine.cu changed - this line is not in it exactly once, "
                        "so cmake/multiengine_qwen35.cmake must be brought up to date:\n${anchor}")
  endif()
  if(where STREQUAL "BEFORE")
    string(REPLACE "${anchor}" "${text}${anchor}" _me_q36 "${_me_q36}")
  else()
    string(REPLACE "${anchor}" "${anchor}${text}" _me_q36 "${_me_q36}")
  endif()
  set(_me_q36 "${_me_q36}" PARENT_SCOPE)
endfunction()

_me_q36_put(AFTER [=[#include "strata/qwen35/engine.hpp"
]=] [=[#include "strata/platform/memory.hpp"
#include "q36_progress.hpp"
]=])

# load(): each tensor read ahead of its copy (the f32 rows, the matrices, the embedding), a line per 10 s per layer
_me_q36_put(BEFORE [=[        bytes += t->elements() * 4;
        return upload_f32(*t, gm.shard(s).tensor_data(*t));]=] [=[        strata::platform::advise_willneed(gm.shard(s).tensor_data(*t), strata::tensor_payload_bytes(*t));
]=])
_me_q36_put(BEFORE [=[        CK(cudaMemcpy(d, gm.shard(s).tensor_data(*t), nb, cudaMemcpyHostToDevice));
        bytes += nb;
        w.d = d;]=] [=[        strata::platform::advise_willneed(gm.shard(s).tensor_data(*t), strata::tensor_payload_bytes(*t));
]=])
_me_q36_put(AFTER [=[        m.embd_row = row_bytes(m.embd_type, (int64_t) t->shape[0]);
]=] [=[        strata::platform::advise_willneed(gm.shard(s).tensor_data(*t), strata::tensor_payload_bytes(*t));
]=])
_me_q36_put(BEFORE [=[    for (int64_t l = 0; l < g.n_layers + (m.has_mtp ? 1 : 0); ++l) {
        const bool is_mtp = l == g.n_layers;]=] [=[    multiengine::LoadProgress me_progress("reading the weights");
]=])
_me_q36_put(AFTER [=[        y.src_gsz = gsz;
        y.src_dsz = dsz;
]=] [=[        me_progress.say(ms_since(t0) / 1000.0, (size_t) l + 1, (size_t) (g.n_layers + (m.has_mtp ? 1 : 0)), bytes, 0);
]=])

# place_experts(): each layer's experts read ahead of their copy, in each of its three placements (all in VRAM; the main
# layers in VRAM and the MTP block's hybrid; hybrid), a line per 10 s per layer placed
_me_q36_put(BEFORE [=[/// Expert e of layer L as its native blob]=] [=[/// Asks the OS to read layer L's routed experts out of the GGUF ahead of their copy (multiengine, upstream's
/// advise_willneed).
void me_advise_experts(const Layer& L, int64_t ne) {
    strata::platform::advise_willneed(L.src_g, (uint64_t) L.src_gsz * (uint64_t) ne);
    strata::platform::advise_willneed(L.src_u, (uint64_t) L.src_gsz * (uint64_t) ne);
    strata::platform::advise_willneed(L.src_d, (uint64_t) L.src_dsz * (uint64_t) ne);
}

]=])
_me_q36_put(BEFORE [=[    size_t fr = 0, tot = 0;
    CK(cudaMemGetInfo(&fr, &tot));
    const size_t reserve = (size_t) 768 << 20;]=] [=[    multiengine::LoadProgress me_progress("placing the routed experts");
    size_t me_placed = 0, me_layers = 0;
    auto me_placed_layer = [&](const Layer& L) {
        me_placed += L.stride * (size_t) ne + kMmqTail;
        me_progress.say(ms_since(t0) / 1000.0, ++me_layers, all.size(), me_placed, total);
    };
]=])
_me_q36_put(AFTER [=[            blob.assign(L->L.bytes, 0);
]=] [=[            me_advise_experts(*L, ne);
]=])
_me_q36_put(AFTER [=[            L->n_slots = (int) ne;
]=] [=[            me_placed_layer(*L);
]=])
_me_q36_put(AFTER [=[            std::vector<uint8_t> blob(L.L.bytes, 0);
]=] [=[            me_advise_experts(L, ne);
]=])
_me_q36_put(BEFORE [=[            continue;
        }
        L.hblobs = arena + off;]=] [=[            me_placed_layer(L);
]=])
_me_q36_put(BEFORE [=[        for (int64_t e = 0; e < ne; ++e) expert_blob(L, e, L.hblobs + (size_t) e * L.stride);
]=] [=[        me_advise_experts(L, ne);
]=])
_me_q36_put(AFTER [=[        for (int64_t e = 0; e < ne; ++e) expert_blob(L, e, L.hblobs + (size_t) e * L.stride);
]=] [=[        me_placed_layer(L);
]=])

# written only when it changes, so a reconfigure does not rebuild it
file(WRITE ${_me_q36_out}.new "${_me_q36}")
file(COPY_FILE ${_me_q36_out}.new ${_me_q36_out} ONLY_IF_DIFFERENT)
file(REMOVE ${_me_q36_out}.new)

add_library(multiengine_qwen35 STATIC ${_me_q36_out})
foreach(_p LINK_LIBRARIES INCLUDE_DIRECTORIES COMPILE_DEFINITIONS COMPILE_OPTIONS COMPILE_FEATURES)
  get_target_property(_v strata_qwen35 ${_p})
  if(_v)
    set_property(TARGET multiengine_qwen35 APPEND PROPERTY ${_p} ${_v})
  endif()
endforeach()
# its own folder's headers (q36_kernels.cuh) and ours (q36_progress.hpp)
target_include_directories(multiengine_qwen35 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/qwen35 ${CMAKE_CURRENT_SOURCE_DIR}/src/multiengine)
