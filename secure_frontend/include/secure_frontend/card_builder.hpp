#pragma once

#include "secure_frontend/frontend_config.hpp"
#include "secure_frontend/gguf_metadata.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace secure_frontend {

// Cards the page writes (docs/superpowers/specs/2026-10-27-gui-first-install-design.md §4.2-§4.5).

struct GgufCardRequest final {
  std::filesystem::path folder;   // the model folder: the card's roots.models
  std::string file;               // the GGUF file in it (a split model's first shard)
  std::filesystem::path engine;   // the engine the card runs (models.default_engine)
  std::string source;             // where the model came from: "folder", or the download's id (the Download list hides it)
};
struct CardText final {
  std::string name;
  EngineProfile profile{EngineProfile::qwen};
  std::string json;               // a card file's text (parse_model_card reads it)
};
// `message` is fixed text and the model's architecture at most; never a path.
struct CardError final {
  std::string message;
};

// The card for a GGUF model file from its header: the profile from general.architecture (flint, gemma4: the families
// that run from one GGUF with --native; any other is refused by name), the name from the file's name (made a valid
// card name; a split model's -NNNNN-of-NNNNN goes), the model id the file's stem, no tokenizer pack (the tokenizer is
// read from the GGUF), and the options: the context up to the model's own (<arch>.context_length, at most 256K;
// 64K by default), the expert VRAM auto, thinking as the template does it (flint on, Gemma 4 off), and the sampling
// the GGUF recommends (general.sampling.temp / top_p / top_k; top_p to two places).
[[nodiscard]] std::variant<CardText, CardError> card_for_gguf(const GgufMetadata& metadata, const GgufCardRequest& request);

// The families whose card needs a pack built first (owner 2026-10-27, strata-prepare beside the engine): a Qwen3.8
// model (qwen4exp: upstream's iq_pack, the card's --pack beside the model) and GLM-5.3 (glm5-next, unsloth's glm5next:
// Maya's pack beside the shards, which the card reads as its model folder). nullopt: neither.
// raon1 (Raon-1-mini, owner 2026-10-27): strata-prepare's raon1 pack beside the file, as for GLM-5.3.
// minimax (MiniMax-M3.1-Flash, owner 2026-10-28): Maya's maya-mm3 pack beside the shards, as for DeepSeek-V4.1.
enum class PackedFamily { qwen38, glm53, deepseek, raon1, minimax };
[[nodiscard]] std::optional<PackedFamily> packed_family(const GgufMetadata& metadata);

// Maya's served model name (maya.py MODEL_NAME): a GLM pack's marker file is <pack>/glm-5.3-flash.gguf, the card's model.
inline constexpr std::string_view kGlmModelName = "glm-5.3-flash";
// DeepSeek-V4.1's (maya.py DS4_MODEL_NAME): its pack's marker is <pack>/deepseek-v4.1-flash.gguf.
inline constexpr std::string_view kDeepseekModelName = "deepseek-v4.1-flash";
// raon-1-mini's, as the managed raon1 card served it: its pack's marker is <pack>/raon1-mini.gguf.
inline constexpr std::string_view kRaon1ModelName = "raon1-mini";
// MiniMax-M3.1-Flash's (maya.py MM3_MODEL_NAME): its pack's marker is <pack>/minimax-m3.1-flash.gguf.
inline constexpr std::string_view kMinimaxModelName = "minimax-m3.1-flash";

struct PackedCardRequest final {
  PackedFamily family{PackedFamily::qwen38};
  std::string name;                          // the card's name
  std::filesystem::path engine;              // the engine the card runs (models.default_engine)
  std::string source;                        // "folder", or "download:" and the download's id
  std::filesystem::path pack;                // the pack strata-prepare built
  // qwen38 only: the model folder and the first shard's stem (the model), the PLE shard, the bundled expert profile
  std::filesystem::path folder;
  std::string model_id;
  std::optional<std::string> ple_gguf;       // UTF-8
  std::optional<std::filesystem::path> expert_profile;
  // qwen38: the MTP draft layer's runtime folder (upstream's mtp/rt: dense.bin, experts.bin, draft_vocab.bin), when one
  // was found (find_mtp_runtime); the card then runs it as upstream's setup does and lists its settings
  std::optional<std::filesystem::path> mtp;
};
// The card of a model whose pack strata-prepare built. qwen38: profile qwen, the folder and first shard as the model,
// --pack / --ple-gguf / --expert-profile, the options upstream's setup gives a pack (128K, int8 KV, 32K resident, spec
// 4, the expert cache and prompt chunk auto, thinking high). glm53: profile glm53, the pack as roots.models (its marker
// file Maya's model name) and its tokenizer, Maya's GLM menus (contexts 8K-128K, 32K; temperature 1.0, top_p 0.95).
// deepseek: profile deepseek, the pack likewise (its marker deepseek-v4.1-flash.gguf), Maya's one-GPU flags (--tp 1
// --threads on), Maya's DeepSeek menus (contexts 32K-1M, 64K; thinking high; temperature 1.0, top_p 1.0). raon1:
// profile raon1, the pack likewise (its marker raon1-mini.gguf), KRAFTON's sampling (temperature 1.0, top_p 0.95, top_k
// 20, presence_penalty 0 over the last 2048 tokens, 32K answers by default) and the GLM engine's menus (contexts 8K-256K,
// 64K; the KV cache q8_0).
[[nodiscard]] CardText card_for_pack(const PackedCardRequest& request);

// A Maya pack's marker file (<pack>/<model name>.gguf, the card's model: the inventory lists the pack by it) when it is
// not there yet. The pack is a folder the server trusts as a model folder (open_root: no other user may write it). False
// when it cannot be written.
[[nodiscard]] bool write_pack_marker(const std::filesystem::path& pack, std::string_view model_name);
// write_pack_marker with Maya's GLM model name (<pack>/glm-5.3-flash.gguf).
[[nodiscard]] bool write_glm_pack_marker(const std::filesystem::path& pack);

// The card name a model file's stem gives (a split model's -NNNNN-of-NNNNN goes; made a valid card name; empty: none).
[[nodiscard]] std::string card_name_for_file(std::string_view stem);
// Whether a model file's stem is a whole model's or a split model's first shard (-00001-of-NNNNN).
[[nodiscard]] bool first_shard_or_whole(std::string_view stem);

// Upstream's MTP draft layer for a Qwen3.8 model in `folder`: the first of folder/mtp/rt, its parent's and its
// grandparent's holding dense.bin, experts.bin and draft_vocab.bin as regular files (not links) - upstream's setup keeps
// it at <data>/mtp/rt beside models/ and packs/. nullopt: none. Its privacy is the caller's to check.
[[nodiscard]] std::optional<std::filesystem::path> find_mtp_runtime(const std::filesystem::path& folder);

// A card's text with roots.models set to `folder` (the model moved); everything else as it was.
[[nodiscard]] std::variant<std::string, CardError> card_with_folder(std::string_view card_json, const std::filesystem::path& folder);

// The danger zone's first check (§4.5): a one-time code the page shows and the user types back. XXXX-XXXX from an
// alphabet with no look-alikes, for one card, valid kLifetime, spent by one right or one wrong try. Thread-safe.
class DeleteCodes final {
 public:
  using Clock = std::chrono::steady_clock;
  static constexpr std::chrono::minutes kLifetime{5};
  [[nodiscard]] std::optional<std::string> issue(std::string_view card, Clock::time_point now);
  [[nodiscard]] bool claim(std::string_view card, std::string_view code, Clock::time_point now);

 private:
  struct Pending final {
    std::string code;
    Clock::time_point expires;
  };
  std::mutex mutex_;
  std::map<std::string, Pending, std::less<>> pending_;
};

// The danger zone's delete (§4.5): <folder>/<model_id>.gguf and, for a split model (<prefix>-00001-of-NNNNN), every
// shard of the set; then the folder, only when nothing is left in it. Nothing else is touched. Every file is checked
// (a regular file, not a link) before the first one is removed; the model's own file missing is an error.
struct DeletedFiles final {
  std::size_t files{0};
  std::uint64_t bytes{0};
  bool folder_removed{false};
};
[[nodiscard]] std::variant<DeletedFiles, CardError> delete_gguf_model_files(const std::filesystem::path& folder,
                                                                           std::string_view model_id);

}  // namespace secure_frontend
