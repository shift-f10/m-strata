// engines/unified/src/family/ds4_graph.hpp
// -----------------------------------------------------------------------------
// The DeepSeek-V4.1-Flash (ds4) family graph for the unified engine: a
// u::FamilyGraph the serve loop drives (u_family.hpp). It runs ds4's Multi-head
// Latent Attention with the compressed/latent KV window and the learned sparse
// index, a sigmoid-routed MoE through the shared expert tier, and the DSpark
// speculative drafter — ported from engines/maya-ref/src/ds4/ (Maya lineage,
// design 2026-10-23 §3.2, plan T1.4).
//
// This header is HOST-SAFE: it declares the class and factory only (pImpl), so it
// parses on CI / the dev host without any CUDA toolkit. All device state and the
// kernel calls live in ds4_graph.cu behind STRATA_U_CUDA; make_ds4_graph is linked
// only in the GPU-box build. On a host build the engine uses u::make_family_graph's
// stub instead (u_family.hpp), exactly as Plan 0 established.
// -----------------------------------------------------------------------------
#pragma once

#include "u_family.hpp"     // u::FamilyGraph, u::Gen, u::LineSink
#include "u_geometry.hpp"   // u::Geometry
#include "u_gguf.hpp"       // u::GgufFile (the load() seam parameter; host-safe header)
#include "u_protocol.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace u {

// The ds4 graph. Owns its device weights, KV caches and window scratch (in the
// pImpl). One instance serves one loaded model; generate() runs one request.
class Ds4Graph final : public FamilyGraph {
 public:
  ~Ds4Graph() override;

  [[nodiscard]] FamilyId family() const override { return FamilyId::ds4; }
  [[nodiscard]] std::uint32_t context() const override;
  [[nodiscard]] bool available() const override;     // true once weights are resident
  [[nodiscard]] std::string describe() const override;

  // Runs one generation request. Feeds request.ids (emitting PP progress), then
  // decodes up to request.max_new tokens (DSpark speculative rounds when enabled,
  // else a plain step per token), emitting each as a T line and finishing with a
  // terminal DONE. Returns false and sets `error` on a hard failure (also emits ERR
  // at the wire via the caller). Mirrors engines/maya-ref/src/ds4/ds4_main.cpp
  // serve()/round(), re-expressed on the converged `u` protocol.
  [[nodiscard]] bool generate(const Gen& request, const LineSink& sink, std::string& error) override;

  // The weights seam: the substrate calls this with the merged multi-part GGUF right
  // after the factory returns (FamilyGraph::load, u_family.hpp:48 / u_main.cpp:423 —
  // the same seam GlmGraph::load proved on the box). It parses the deepseek41.*
  // geometry keys, uploads the non-expert tensors, repacks the grouped output
  // projections, registers the routed experts with the shared tier and finalizes
  // VRAM residency. available() reports false until this succeeds.
  [[nodiscard]] bool load(const GgufFile& gguf, const std::uint8_t* mapped, std::string& error) override;

  // Defined in ds4_graph.cu. Opaque device-side state.
  struct Impl;

 private:
  explicit Ds4Graph(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> p_;
  friend std::unique_ptr<FamilyGraph> make_ds4_graph(const Geometry& geom, std::uint32_t context, std::string& error);
};

// Builds the ds4 graph for a guarded geometry and the serving context (the
// geometry's context_length clamped by --max-context). Reads the extended ds4.*
// geometry, loads weights onto the GPU, and returns the graph; on any load failure
// it sets `error` and returns nullptr. Linked only in the CUDA build; the host
// foundation reaches the stub via u::make_family_graph instead.
[[nodiscard]] std::unique_ptr<FamilyGraph> make_ds4_graph(const Geometry& geom, std::uint32_t context,
                                                          std::string& error);

}  // namespace u
