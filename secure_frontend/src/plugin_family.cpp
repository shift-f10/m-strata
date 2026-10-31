// An engine plugin's family (plugin spec 2026-10-30 section 5, phase 2's second step): its descriptor from its
// manifest's engine block, and the families this server runs.
#include "secure_frontend/plugin_family.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <utility>

namespace secure_frontend {

PluginFamily::PluginFamily(const PluginManifest& manifest, std::string library_sha256, const EngineProfile profile)
    : engine_(manifest.engine.value_or(PluginEngine{})),
      id_(manifest.id),
      name_(manifest.name),
      library_sha256_(std::move(library_sha256)),
      engine_words_("the " + manifest.name + " engine"),
      template_words_(manifest.name + "'s template") {
  // its options, in its order but its GPUs; then the server's; then its GPUs
  choices_.reserve(engine_.options.size());
  const PluginEngineOption* devices = nullptr;
  const auto spec_of = [this](const PluginEngineOption& option) {
    ModelOptionSpec spec{};
    spec.name = option.name;
    spec.kind = option.kind;
    spec.label = option.label;
    spec.help = option.help;
    spec.minimum = option.minimum;
    spec.maximum = option.maximum;
    spec.minimum_decimal = option.minimum_decimal;
    spec.maximum_decimal = option.maximum_decimal;
    auto& choices = choices_.emplace_back(option.choices.begin(), option.choices.end());
    spec.choices = choices;
    spec.values = option.values;
    if (option.kind == ModelOptionKind::devices) spec.minimum = 1;
    return spec;
  };
  for (const auto& option : engine_.options) {
    if (option.kind == ModelOptionKind::devices) {
      devices = &option;
      continue;
    }
    options_.push_back(spec_of(option));
    if (std::find(flags_.begin(), flags_.end(), option.flag) == flags_.end()) flags_.emplace_back(option.flag);
  }
  const auto server = [this](const std::string_view name) {
    if (const auto* spec = server_card_option(name)) options_.push_back(*spec);
  };
  server("temperature");
  server("top_p");
  if (engine_.sampling.top_k) server("top_k");
  if (engine_.sampling.min_p) server("min_p");
  server("answer_minutes");
  if (engine_.web_search) server("web_search");
  if (devices != nullptr) options_.push_back(spec_of(*devices));

  // the renderer's own family says whether a request's reasoning_effort means anything to it
  bool effort = true;
  for (const auto& built_in : family_descriptors()) {
    if (built_in.chat_profile == engine_.chat) {
      effort = built_in.per_request_reasoning_effort;
      break;
    }
  }

  FamilyDescriptor& family = descriptor_;
  family.profile = profile;
  family.id = id_;
  family.chat_profile = engine_.chat;
  family.protocol_flavor = engine_.dialect;
  family.environment_family = EngineFamily::strata;  // the closed allowlist alone: a manifest declares no environment
  family.arguments_profile = EngineArgumentsProfile::plugin;
  family.tokenizer_mode = engine_.tokenizer_mode;
  family.tokenizer_source = engine_.tokenizer_source;
  family.per_request_reasoning_effort = effort;
  // it composes no qwen tails, so qwen's launch rows would do nothing: refused, in its own words
  family.refuses_qwen_launch_rows = true;
  family.engine_words = engine_words_;
  family.template_words = template_words_;
  family.reads_images = engine_.vision;
  family.checks_mtp_side_file = engine_.mtp == DraftLayerKind::file;
  family.sweep_keep_margin = engine_.keep_margin;
  family.calibrates_before_first_start = engine_.calibrates_before_first_start;
  family.options = options_;
  family.option_controlled_flags = flags_;
  family.sampling = engine_.sampling;
  family.draft_layer = engine_.mtp;
  family.plugin = &engine_;
  family.library_sha256 = library_sha256_;
}

std::string_view PluginFamily::option_flag(const std::string_view option) const noexcept {
  const auto found = std::find_if(engine_.options.begin(), engine_.options.end(),
                                  [option](const PluginEngineOption& each) { return each.name == option; });
  return found == engine_.options.end() ? std::string_view{} : std::string_view{found->flag};
}

namespace {

using Families = std::vector<std::unique_ptr<const PluginFamily>>;

// The families this server runs: replaced as a whole, never changed in place; the ones replaced are kept, since a
// reader may still hold one of them.
std::atomic<const Families*> g_families{nullptr};
std::mutex g_replaced_lock;
std::vector<std::unique_ptr<const Families>>& replaced() {
  static std::vector<std::unique_ptr<const Families>> kept;
  return kept;
}

const Families& current() {
  static const Families kNone;
  const auto* families = g_families.load(std::memory_order_acquire);
  return families == nullptr ? kNone : *families;
}

}  // namespace

void set_plugin_families(std::vector<PluginFamilySource> plugins) {
  auto families = std::make_unique<Families>();
  for (auto& plugin : plugins) {
    if (plugin.manifest.kind != PluginKind::engine || !plugin.manifest.engine) continue;
    const auto& id = plugin.manifest.id;
    const bool built_in = std::any_of(family_descriptors().begin(), family_descriptors().end(),
                                      [&id](const FamilyDescriptor& family) { return family.id == id; });
    const bool taken = std::any_of(families->begin(), families->end(),
                                   [&id](const std::unique_ptr<const PluginFamily>& family) { return family->descriptor().id == id; });
    if (built_in || taken) continue;
    const auto profile = static_cast<EngineProfile>(kFirstPluginProfile + static_cast<int>(families->size()));
    families->push_back(std::make_unique<const PluginFamily>(plugin.manifest, std::move(plugin.library_sha256), profile));
  }
  const std::lock_guard<std::mutex> lock{g_replaced_lock};
  auto& kept = replaced();
  kept.push_back(std::move(families));
  g_families.store(kept.back().get(), std::memory_order_release);
}

const PluginFamily* plugin_family(const EngineProfile profile) noexcept {
  const auto place = static_cast<int>(profile) - kFirstPluginProfile;
  const auto& families = current();
  if (place < 0 || static_cast<std::size_t>(place) >= families.size()) return nullptr;
  return families[static_cast<std::size_t>(place)].get();
}

const PluginFamily* plugin_family_named(const std::string_view id) noexcept {
  for (const auto& family : current()) {
    if (family->descriptor().id == id) return family.get();
  }
  return nullptr;
}

std::vector<const PluginFamily*> plugin_families() {
  std::vector<const PluginFamily*> out;
  for (const auto& family : current()) out.push_back(family.get());
  return out;
}

}  // namespace secure_frontend
