// strata-unified-engine: the command line, the geometry guard, the dispatch, and the serve
// loop skeleton (design §3.1, plan T0.4/T0.6). It reads a GGUF header, guards the geometry
// for its `general.architecture`, dispatches to a FamilyGraph, and serves the converged
// "unified" wire dialect. On a host-only build (STRATA_U_CUDA OFF) the FamilyGraph is the
// stub, so the binary builds and serves without a CUDA toolkit but refuses to generate.
#include "u_calibration.hpp"
#include "u_dispatch.hpp"
#include "u_family.hpp"
#include "u_geometry.hpp"
#include "u_gguf.hpp"
#include "u_protocol.hpp"
#if defined(STRATA_U_CUDA)
#include "kernels/selftest.hpp"
#endif

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <variant>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#if defined(STRATA_U_CUDA) && !defined(_WIN32)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

struct Options final {
  std::string model;        // --native (required)
  bool serve{false};        // --serve
  std::vector<int> gpus;    // --gpu i,j (argv mechanism, design §3.4)
  int max_context{32768};   // --max-context 1..262144
  int chunk{512};           // --chunk 1..8192
  std::string script;       // --script FILE: read the --serve lines from FILE instead of stdin
                            // (a profiler such as nsys on Windows does not forward the target's stdin)
};

bool parse_int(std::string_view text, long long lo, long long hi, long long& out) {
  if (text.empty() || text.size() > 19) return false;
  if (text.front() == '+' || (text.size() > 1 && text.front() == '0')) return false;
  long long v = 0;
  const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), v);
  if (ec != std::errc{} || end != text.data() + text.size() || v < lo || v > hi) return false;
  out = v;
  return true;
}

std::variant<Options, std::string> parse_cli(const std::vector<std::string>& args) {
  Options o;
  std::set<std::string> seen;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    if (a.rfind("--", 0) != 0) return std::string{"unexpected positional argument"};
    if (!seen.insert(a).second) return std::string{"a flag is given twice"};
    auto value = [&](std::string& out) -> bool {
      if (i + 1 >= args.size()) return false;
      out = args[++i];
      return true;
    };
    auto integer = [&](long long lo, long long hi, int& out) -> bool {
      std::string v;
      long long n = 0;
      if (!value(v) || !parse_int(v, lo, hi, n)) return false;
      out = static_cast<int>(n);
      return true;
    };
    if (a == "--serve") { o.serve = true; continue; }
    if (a == "--native") {
      if (!value(o.model) || o.model.empty()) return std::string{"--native needs the model file"};
      continue;
    }
    if (a == "--gpu") {
      std::string v;
      if (!value(v) || v.empty()) return std::string{"--gpu needs a device list"};
      std::size_t at = 0;
      while (at <= v.size()) {
        const std::size_t comma = std::min(v.find(',', at), v.size());
        long long d = 0;
        if (!parse_int(std::string_view(v).substr(at, comma - at), 0, 255, d)) return std::string{"--gpu takes device numbers 0-255"};
        if (std::find(o.gpus.begin(), o.gpus.end(), static_cast<int>(d)) != o.gpus.end()) return std::string{"--gpu lists a device twice"};
        o.gpus.push_back(static_cast<int>(d));
        if (comma == v.size()) break;
        at = comma + 1;
      }
      if (o.gpus.empty() || o.gpus.size() > 16) return std::string{"--gpu takes 1-16 devices"};
      continue;
    }
    if (a == "--max-context") { if (!integer(1, 262144, o.max_context)) return std::string{"--max-context must be 1-262144"}; continue; }
    if (a == "--chunk") { if (!integer(1, 8192, o.chunk)) return std::string{"--chunk must be 1-8192"}; continue; }
    if (a == "--script") {
      if (!value(o.script) || o.script.empty()) return std::string{"--script needs a file"};
      continue;
    }
    return std::string{"unknown flag"};
  }
  if (o.model.empty()) return std::string{"--native is required"};
  return o;
}

#if defined(STRATA_U_CUDA)
bool env_is_1(const char* key) {  // (only the GPU build reads switches here)
#if defined(_WIN32)
  char* v = nullptr;
  std::size_t n = 0;
  const bool on = _dupenv_s(&v, &n, key) == 0 && v != nullptr && v[0] == '1';
  std::free(v);
  return on;
#else
  const char* v = std::getenv(key);
  return v != nullptr && v[0] == '1';
#endif
}
#endif

void set_env(const char* key, const std::string& value) {
#if defined(_WIN32)
  _putenv_s(key, value.c_str());
#else
  setenv(key, value.c_str(), 1);
#endif
}

#if defined(STRATA_U_CUDA)
#include <cuda_runtime.h>  // cudaGetDeviceProperties for the calibration hardware key
// A whole-file, read-only memory map of the model, kept alive for the engine's lifetime so a
// family graph can read its weights straight from the map (the ExpertTier pins/streams the expert
// region from here; non-expert tensors are uploaded to VRAM during load()). Only the GPU-box build
// maps and loads; the host-only build serves off the stub and never calls this.
struct MappedModel final {
  const std::uint8_t* base{nullptr};
  std::uint64_t size{0};
#if defined(_WIN32)
  void* file{nullptr};     // HANDLE
  void* mapping{nullptr};  // HANDLE
#else
  int fd{-1};
#endif

  MappedModel() = default;
  MappedModel(const MappedModel&) = delete;
  MappedModel& operator=(const MappedModel&) = delete;
  ~MappedModel() {
#if defined(_WIN32)
    if (base) UnmapViewOfFile(static_cast<LPCVOID>(base));
    if (mapping) CloseHandle(static_cast<HANDLE>(mapping));
    if (file && file != INVALID_HANDLE_VALUE) CloseHandle(static_cast<HANDLE>(file));
#else
    if (base) munmap(const_cast<std::uint8_t*>(base), static_cast<std::size_t>(size));
    if (fd >= 0) close(fd);
#endif
  }

  [[nodiscard]] bool map(const std::string& path_utf8, std::string& err) {
#if defined(_WIN32)
    const int wn = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path_utf8.c_str(), -1, nullptr, 0);
    if (wn <= 0) { err = "model path is not valid UTF-8"; return false; }
    std::wstring w(static_cast<std::size_t>(wn), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path_utf8.c_str(), -1, w.data(), wn);
    w.resize(static_cast<std::size_t>(wn - 1));
    HANDLE h = CreateFileW(w.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { err = "cannot open the model file for mapping"; return false; }
    file = h;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz)) { err = "cannot size the model file"; return false; }
    size = static_cast<std::uint64_t>(sz.QuadPart);
    HANDLE m = CreateFileMappingW(h, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!m) { err = "cannot create the model file mapping"; return false; }
    mapping = m;
    LPVOID view = MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
    if (!view) { err = "cannot map the model file into memory"; return false; }
    base = static_cast<const std::uint8_t*>(view);
    return true;
#else
    fd = ::open(path_utf8.c_str(), O_RDONLY);
    if (fd < 0) { err = "cannot open the model file for mapping"; return false; }
    struct stat st {};
    if (fstat(fd, &st) != 0) { err = "cannot size the model file"; return false; }
    size = static_cast<std::uint64_t>(st.st_size);
    void* a = mmap(nullptr, static_cast<std::size_t>(size), PROT_READ, MAP_PRIVATE, fd, 0);
    if (a == MAP_FAILED) { err = "cannot map the model file into memory"; return false; }
    base = static_cast<const std::uint8_t*>(a);
    return true;
#endif
  }
};

// The shard file list for a (possibly split) model. A split GGUF is named "...-00001-of-000NN.gguf";
// from shard 1's path this regenerates the sibling names "...-000ii-of-000NN.gguf" for ii in 1..NN
// (NN read from the "-of-" field of the name). A path that does not match returns just itself (the
// single-file case). Mirrors engines/maya-ref/src/program/generate.cpp model_shards().
std::vector<std::string> model_shards(const std::string& first) {
  const std::string tag = "-00001-of-";
  const std::size_t at = first.rfind(tag);
  if (at == std::string::npos || first.size() < at + tag.size() + 5) return {first};
  const int total = std::atoi(first.substr(at + tag.size(), 5).c_str());
  if (total <= 1 || total > 99) return {first};
  std::vector<std::string> out;
  for (int i = 1; i <= total; ++i) {
    char num[8];
    std::snprintf(num, sizeof num, "%05d", i);
    std::string p = first;
    p.replace(at + 1, 5, num);  // overwrite the 5-digit "00001" shard field with ii
    out.push_back(std::move(p));
  }
  return out;
}
#endif  // STRATA_U_CUDA

std::uint32_t serving_context(const u::Geometry& geom, int max_context) {
  const long long want = std::min<long long>(geom.context_length, max_context);
  return static_cast<std::uint32_t>(std::max<long long>(1, want));
}

u::Record info_record(const u::Geometry& geom, const u::FamilyGraph& graph) {
  u::Info info;
  info.fields.push_back({"engine", "strata-unified"});
  info.fields.push_back({"arch", geom.arch});
  info.fields.push_back({"family", std::string(u::name_of(geom.family))});
  info.fields.push_back({"context", std::to_string(graph.context())});
  info.fields.push_back({"experts", std::to_string(geom.expert_count)});
  info.fields.push_back({"experts_used", std::to_string(geom.expert_used_count)});
  info.fields.push_back({"router", geom.router == u::RouterKind::sigmoid ? "sigmoid" : "softmax"});
  info.fields.push_back({"rope", geom.rope == u::RopeKind::rope ? "rope" : "nope"});
  info.fields.push_back({"available", graph.available() ? "1" : "0"});
  return u::Record{std::move(info)};
}

void emit_line(const u::Record& r) {
  const std::string line = u::emit(r);
  std::printf("%s\n", line.c_str());
  std::fflush(stdout);
}

int serve(const u::Geometry& geom, u::FamilyGraph& graph, const std::string& script) {
  std::ifstream script_in;
  std::istream* in = &std::cin;
  if (!script.empty()) {
    script_in.open(script);
    if (!script_in) {
      std::fprintf(stderr, "strata-unified-engine: cannot open --script %s\n", script.c_str());
      return 2;
    }
    in = &script_in;
  }
  emit_line(info_record(geom, graph));
  emit_line(u::Record{u::Ready{graph.context(), /*stop_honored=*/true}});

  std::string line;
  while (std::getline(*in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;

    const auto parsed = u::parse_line(line);
    if (const auto* perr = std::get_if<u::ParseError>(&parsed)) {
      emit_line(u::Record{u::Err{perr->message}});
      continue;
    }
    const auto& record = std::get<u::Record>(parsed);

    if (std::holds_alternative<u::Quit>(record)) return 0;
    if (std::holds_alternative<u::Stop>(record)) continue;  // nothing is generating in the skeleton
    if (const auto* gen = std::get_if<u::Gen>(&record)) {
      std::string error;
      const bool ok = graph.generate(*gen, [](const std::string& out) {
        std::printf("%s\n", out.c_str());
        std::fflush(stdout);
      }, error);
      if (!ok) emit_line(u::Record{u::Err{error}});
      continue;
    }
    // Any other record (an inbound/engine record arriving from the client) fails closed.
    emit_line(u::Record{u::Err{"unexpected record from the client"}});
  }
  return 0;
}

int summary(const u::Geometry& geom, const u::FamilyGraph& graph) {
  std::printf(
      "strata-unified: arch=%s family=%s blocks=%lld hidden=%lld heads=%lld head_dim=%lld ff=%lld moe_ff=%lld "
      "experts=%lld used=%lld shared=%lld vocab=%lld context=%u router=%s rope=%s available=%d\n",
      geom.arch.c_str(), std::string(u::name_of(geom.family)).c_str(), static_cast<long long>(geom.block_count),
      static_cast<long long>(geom.embedding_length), static_cast<long long>(geom.head_count),
      static_cast<long long>(geom.head_dim), static_cast<long long>(geom.feed_forward_length),
      static_cast<long long>(geom.expert_feed_forward_length), static_cast<long long>(geom.expert_count),
      static_cast<long long>(geom.expert_used_count), static_cast<long long>(geom.expert_shared_count),
      static_cast<long long>(geom.vocab_size), graph.context(),
      geom.router == u::RouterKind::sigmoid ? "sigmoid" : "softmax", geom.rope == u::RopeKind::rope ? "rope" : "nope",
      graph.available() ? 1 : 0);
  return 0;
}

int run(const std::vector<std::string>& args) {
#if defined(STRATA_U_CUDA)
  // STRATA_KERNEL_SELFTEST=1: the GPU kernel self-checks (kernels/selftest.hpp) instead of a model run.
  if (env_is_1("STRATA_KERNEL_SELFTEST")) {
    std::string report;
    const bool ok = u::k::selftest_kernels(report);
    std::fprintf(stderr, "%s", report.c_str());
    return ok ? 0 : 1;
  }
#endif
  const auto parsed = parse_cli(args);
  if (const auto* err = std::get_if<std::string>(&parsed)) {
    std::fprintf(stderr, "strata-unified-engine: %s\n", err->c_str());
    return 2;
  }
  const auto& o = std::get<Options>(parsed);

  if (!o.gpus.empty()) {
    std::string list;
    for (const int g : o.gpus) list += (list.empty() ? "" : ",") + std::to_string(g);
    set_env("CUDA_DEVICE_ORDER", "PCI_BUS_ID");
    set_env("CUDA_VISIBLE_DEVICES", list);
  }

  const auto header = u::read_gguf_header(o.model);
  if (const auto* err = std::get_if<u::GgufError>(&header)) {
    std::fprintf(stderr, "strata-unified-engine: %s\n", err->message.c_str());
    return 1;
  }
  const u::GgufFile& gguf = std::get<u::GgufFile>(header);
  const auto geometry = u::read_geometry(gguf);
  if (const auto* err = std::get_if<u::GeometryError>(&geometry)) {
    std::fprintf(stderr, "strata-unified-engine: %s\n", err->message.c_str());
    return 1;
  }
  const auto& geom = std::get<u::Geometry>(geometry);

  const std::uint32_t context = serving_context(geom, o.max_context);
  std::string error;
  auto graph = u::make_family_graph(geom, context, error);
  if (!graph) {
    std::fprintf(stderr, "strata-unified-engine: %s\n", error.c_str());
    return 1;
  }

#if defined(STRATA_U_CUDA)
  // Per-box calibration (spec 2026-10-24, Stage C1): load the adaptive-tier profile for this GPU +
  // model + context BEFORE the family graph's load() reads it. Defaults = the pre-C1 constants, so an
  // un-calibrated box is unchanged. Keyed by the device name + VRAM (device 0 under CUDA_VISIBLE_DEVICES).
  {
    cudaDeviceProp prop{};
    std::string gpu_name = "unknown";
    std::uint64_t vram = 0;
    if (cudaGetDeviceProperties(&prop, 0) == cudaSuccess) {
      gpu_name = prop.name;
      vram = (std::uint64_t) prop.totalGlobalMem;
    }
    u::Calibration::init(gpu_name, vram, o.model, context);
  }

  // GPU box: memory-map the whole model and upload its weights. `mm` is kept alive for the rest of
  // run() so the mapped bytes stay valid while the engine serves (the ExpertTier streams experts
  // from the map). The host-only build skips this entirely and serves off the stub.
  // Map every shard (one file for a non-split model) and build ONE merged tensor directory whose
  // entries carry a resolved absolute host pointer (host_data) into the owning shard. Each shard's
  // tensor offsets are relative to that shard's own data section, so host_data = shard.base +
  // shard.data_offset + tensor.offset. First shard wins on a duplicate name; the metadata/geometry is
  // shard 1's (already parsed as `gguf`). The shard maps are kept alive for the rest of run(). Single
  // file => one shard => identical to before (and gemma4's load still uses mapped = base + data_offset).
  std::vector<std::unique_ptr<MappedModel>> shards;
  u::GgufFile merged = gguf;  // copy the shard-1 metadata; the tensor list is rebuilt across shards
  merged.tensors.clear();
  {
    std::string lerr;
    const std::vector<std::string> paths = model_shards(o.model);
    std::set<std::string> seen;
    for (std::size_t si = 0; si < paths.size(); ++si) {
      // Shard 0's header is already parsed as `gguf`; parse the rest. `sgf` points at the shard's file.
      std::variant<u::GgufFile, u::GgufError> sh_hdr;
      const u::GgufFile* sgf = &gguf;
      if (si != 0) {
        sh_hdr = u::read_gguf_header(paths[si]);
        if (const auto* e = std::get_if<u::GgufError>(&sh_hdr)) {
          std::fprintf(stderr, "strata-unified-engine: shard %zu (%s): %s\n", si + 1, paths[si].c_str(),
                       e->message.c_str());
          return 1;
        }
        sgf = &std::get<u::GgufFile>(sh_hdr);
      }
      auto mm = std::make_unique<MappedModel>();
      if (!mm->map(paths[si], lerr)) {
        std::fprintf(stderr, "strata-unified-engine: shard %zu (%s): %s\n", si + 1, paths[si].c_str(), lerr.c_str());
        return 1;
      }
      if (sgf->data_offset > mm->size) {
        std::fprintf(stderr, "strata-unified-engine: shard %zu GGUF data section starts past the end of the file\n",
                     si + 1);
        return 1;
      }
      for (const auto& t : sgf->tensors) {
        if (!seen.insert(t.name).second) continue;  // a later shard repeating a name: first wins
        u::GgufTensor rt = t;
        rt.host_data = mm->base + sgf->data_offset + t.offset;  // resolved absolute host pointer
        merged.tensors.push_back(std::move(rt));
      }
      shards.push_back(std::move(mm));
    }
    if (paths.size() > 1)
      std::fprintf(stderr, "strata-unified-engine: %zu-shard split model, %zu tensors merged\n", paths.size(),
                   merged.tensors.size());
    if (!graph->load(merged, shards[0]->base + merged.data_offset, lerr)) {
      std::fprintf(stderr, "strata-unified-engine: load failed: %s\n", lerr.c_str());
      return 1;
    }
  }
#endif

  std::fprintf(stderr, "strata-unified-engine: %s ready (context %u, chunk %d); generation %s\n",
               std::string(u::name_of(geom.family)).c_str(), context, o.chunk,
               graph->available() ? "available" : "not available on this host build");

  if (o.serve) return serve(geom, *graph, o.script);
  return summary(geom, *graph);
}

}  // namespace

#if defined(_WIN32)
int wmain(int argc, wchar_t** argv) {
  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i) {
    const int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) {
      std::fprintf(stderr, "strata-unified-engine: an argument is not valid UTF-16\n");
      return 2;
    }
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, s.data(), n, nullptr, nullptr);
    s.resize(static_cast<std::size_t>(n - 1));
    args.push_back(std::move(s));
  }
  return run(args);
}
#else
int main(int argc, char** argv) {
  std::vector<std::string> args(argv + 1, argv + argc);
  return run(args);
}
#endif
