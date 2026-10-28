// strata-unified-engine: the abstract per-family graph the serve loop drives, and the
// host-only stub that stands in for it when CUDA is off (STRATA_U_CUDA OFF). On the GPU box
// each family (glm5next, ds4, raon1, qwen35moe, qwen4exp, gemma4) implements FamilyGraph on
// the shared kernels; until then, and in every CI/dev-host build, make_family_graph returns
// the stub so the host binary links and serves without a CUDA toolkit. The stub reports
// "not implemented" for any generation.
#pragma once

#include "u_dispatch.hpp"
#include "u_geometry.hpp"
#include "u_gguf.hpp"
#include "u_protocol.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace u {

// A sink the graph calls with each inbound wire line it produces (T / PP / STAT / ...).
using LineSink = std::function<void(const std::string&)>;

class FamilyGraph {
 public:
  virtual ~FamilyGraph() = default;

  [[nodiscard]] virtual FamilyId family() const = 0;
  [[nodiscard]] virtual std::uint32_t context() const = 0;
  // False on the host stub (no CUDA): the engine can parse, guard and dispatch, but cannot
  // run a graph off the GPU box.
  [[nodiscard]] virtual bool available() const = 0;
  // A short human description for the INFO record and logs.
  [[nodiscard]] virtual std::string describe() const = 0;

  // Runs one generation request, emitting inbound lines through `sink`. Returns true when it
  // ends with a terminal DONE; false sets `error` to the ERR text. The host stub always
  // returns false.
  [[nodiscard]] virtual bool generate(const Gen& request, const LineSink& sink, std::string& error) = 0;

  // Uploads the model weights from the memory-mapped GGUF and sizes the runtime state; the engine
  // (u_main) calls it once, after mapping the file, before generate(). `mapped` is the GGUF data
  // section base (the file base + GgufFile::data_offset), so every GgufTensor::offset is relative
  // to it. The default (host stub, and any family whose device bring-up is not yet wired) returns
  // false; a family on the GPU box overrides it with its real device upload. Not called on a
  // host-only build (u_main gates the map+load on STRATA_U_CUDA).
  [[nodiscard]] virtual bool load(const GgufFile& /*gguf*/, const std::uint8_t* /*mapped*/, std::string& error) {
    error = "load() is not implemented for this family graph on this build";
    return false;
  }
};

namespace detail {

// The host stub: no kernels, no weights. It knows its family and the clamped context from the
// guarded geometry, and refuses to generate.
class HostStubGraph final : public FamilyGraph {
 public:
  HostStubGraph(FamilyId family, std::uint32_t context, std::string arch)
      : family_(family), context_(context), arch_(std::move(arch)) {}

  [[nodiscard]] FamilyId family() const override { return family_; }
  [[nodiscard]] std::uint32_t context() const override { return context_; }
  [[nodiscard]] bool available() const override { return false; }
  [[nodiscard]] std::string describe() const override { return "host-stub(" + arch_ + ")"; }

  [[nodiscard]] bool generate(const Gen&, const LineSink&, std::string& error) override {
    error = "not implemented on this build (host-only, CUDA disabled); run on the GPU box";
    return false;
  }

 private:
  FamilyId family_;
  std::uint32_t context_;
  std::string arch_;
};

}  // namespace detail

// Builds the graph for a guarded geometry. `context` is the serving context (the geometry's
// context_length clamped by --max-context). On a host build this is always the stub; it never
// fails here (a real CUDA factory would set `error` and return nullptr on a load failure).
[[nodiscard]] inline std::unique_ptr<FamilyGraph> make_family_graph(const Geometry& geom, std::uint32_t context,
                                                                    std::string& error) {
  error.clear();
#if defined(STRATA_U_CUDA)
  // The GPU box links the real per-family graphs here (design §3.2, decision 1). Each family
  // lane implements `make_<fam>_graph` in src/family/<fam>_graph.{hpp,cu}; this branch
  // forward-declares the six factories (so no family header is required here -- the task's
  // "prefer forward declarations") and dispatches on the GUARDED arch->family mapping. The
  // binary selects a family only from the GGUF arch key the geometry guard already re-checked;
  // nothing is inferred from tensor data (the frontend's "no behavior from model data"
  // invariant, held on the engine side too). A factory sets `error` and returns nullptr on a
  // load failure.
  //
  // LINKING NOTE: this references all six factories, so the engine executable links only once
  // every family's <fam>.cu is present. A family lane not yet started should drop a one-line
  // "unavailable" stub for its factory so incremental box builds (plan T1.3+: GLM/DS4/Raon
  // first, then qwen, then gemma4) link; the shared substrate (src/kernels/*.cu, u_experts.cu)
  // compiles independently of any family.
  extern std::unique_ptr<FamilyGraph> make_glm_graph(const Geometry&, std::uint32_t, std::string&);
  extern std::unique_ptr<FamilyGraph> make_ds4_graph(const Geometry&, std::uint32_t, std::string&);
  extern std::unique_ptr<FamilyGraph> make_raon1_graph(const Geometry&, std::uint32_t, std::string&);
  extern std::unique_ptr<FamilyGraph> make_qwen35moe_graph(const Geometry&, std::uint32_t, std::string&);
  extern std::unique_ptr<FamilyGraph> make_qwen4exp_graph(const Geometry&, std::uint32_t, std::string&);
  extern std::unique_ptr<FamilyGraph> make_gemma4_graph(const Geometry&, std::uint32_t, std::string&);
  switch (geom.family) {
    case FamilyId::glm5next:  return make_glm_graph(geom, context, error);
    case FamilyId::ds4:       return make_ds4_graph(geom, context, error);
    case FamilyId::raon1:     return make_raon1_graph(geom, context, error);
    case FamilyId::qwen35moe: return make_qwen35moe_graph(geom, context, error);
    case FamilyId::qwen4exp:  return make_qwen4exp_graph(geom, context, error);
    case FamilyId::gemma4:    return make_gemma4_graph(geom, context, error);
    // flint H1 (spec 2026-10-25-flint-engine-design) lands parse + dispatch + geometry only; the
    // graph factory lands with H2 (NVFP4 kernels first). Fail closed honestly until then — no stub
    // .cu to link, unlike the started lanes.
    case FamilyId::flint:
      error = "flint: engine graph lands with the H2 lane (H1 gave GGUF parse + geometry; "
              "spec 2026-10-25-flint-engine-design)";
      return nullptr;
  }
  error = "unknown family";
  return nullptr;
#else
  return std::make_unique<detail::HostStubGraph>(geom.family, context, geom.arch);
#endif
}

}  // namespace u
