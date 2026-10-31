#pragma once

// An engine plugin's family (plugin spec 2026-10-30 section 5, phase 2's second step): the descriptor its manifest's
// engine block makes, beside the built-in families' (family_descriptor.hpp), so the cards, the options, the engine's
// arguments and the launch read a plugin's family as they read a built-in one. What a manifest declares comes from the
// server's closed lists (its renderer, tokenizer, dialect, value kinds), so it never widens what the server does; what
// only a plugin has - its flags, each option's flag, its calibration, its Monitor keys, its models - the descriptor's
// `plugin` points at, and its library's SHA-256 is what a start hands the launcher (--family-sha256).
//
// Its card options are its own, in its order (but its GPUs), then the server's: temperature and top_p, top_k and min_p
// when its sampling names them, answer_minutes, web_search when it searches, then its GPUs.

#include "secure_frontend/engine_family_kinds.hpp"
#include "secure_frontend/family_descriptor.hpp"
#include "secure_frontend/model_option_spec.hpp"
#include "secure_frontend/plugin_manifest.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace secure_frontend {

class PluginFamily final {
 public:
  // `manifest` is an engine plugin's (kind engine, its block read); `profile` its place among the families.
  PluginFamily(const PluginManifest& manifest, std::string library_sha256, EngineProfile profile);
  PluginFamily(const PluginFamily&) = delete;  // the descriptor's views point into it
  PluginFamily& operator=(const PluginFamily&) = delete;

  [[nodiscard]] const FamilyDescriptor& descriptor() const noexcept { return descriptor_; }
  [[nodiscard]] const PluginEngine& engine() const noexcept { return engine_; }
  [[nodiscard]] std::string_view name() const noexcept { return name_; }
  // The flag a declared option sets (empty: one of the server's options, or its GPUs).
  [[nodiscard]] std::string_view option_flag(std::string_view option) const noexcept;

 private:
  PluginEngine engine_;
  std::string id_;
  std::string name_;
  std::string library_sha256_;
  std::string engine_words_;
  std::string template_words_;
  std::vector<std::vector<std::string_view>> choices_;  // each declared choice option's, viewing engine_
  std::vector<ModelOptionSpec> options_;
  std::vector<std::string_view> flags_;
  FamilyDescriptor descriptor_{};
};

// What a plugin family is made from: an approved engine plugin's manifest and its library's SHA-256.
struct PluginFamilySource final {
  PluginManifest manifest;
  std::string library_sha256;
};

// The engine plugins' families this server runs, set once when it starts, before any thread reads a family (as
// set_debugging_logs is): each takes the profile after the built-ins' and the ones before it, in this order. A manifest
// of another kind, or one whose id a built-in family or an earlier one holds, is left out. A later call replaces them
// (the ones it replaces are kept: a reader may still hold one).
void set_plugin_families(std::vector<PluginFamilySource> plugins);
// The plugin family a profile or a name names (nullptr: none).
[[nodiscard]] const PluginFamily* plugin_family(EngineProfile profile) noexcept;
[[nodiscard]] const PluginFamily* plugin_family_named(std::string_view id) noexcept;
// Every plugin family, in profile order.
[[nodiscard]] std::vector<const PluginFamily*> plugin_families();

}  // namespace secure_frontend
