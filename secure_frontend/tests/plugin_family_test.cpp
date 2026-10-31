// An engine plugin's family (plugin spec 2026-10-30 section 5, phase 2's second step): its manifest's engine block made
// into a descriptor beside the built-in ones - pinned value by value - found by its name once the server runs it, its
// cards' options written as its flags, its engine taking its declared flags and nothing else, and a card naming it.
#include "secure_frontend/calibration.hpp"
#include "secure_frontend/canonical_path.hpp"
#include "secure_frontend/config_file.hpp"
#include "secure_frontend/engine_arguments.hpp"
#include "secure_frontend/family_descriptor.hpp"
#include "secure_frontend/frontend_config.hpp"
#include "secure_frontend/launch_composition.hpp"
#include "secure_frontend/model_cards.hpp"
#include "secure_frontend/model_options.hpp"
#include "secure_frontend/plugin_family.hpp"
#include "secure_frontend/plugin_manifest.hpp"
#include "support/cli_harness.hpp"
#include "support/config_json.hpp"
#include "support/test_support.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace {
int failures = 0;
void expect(const bool condition, const std::string& message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
  }
}

using secure_frontend::DraftLayerKind;
using secure_frontend::EngineArgumentsProfile;
using secure_frontend::EngineFamily;
using secure_frontend::EngineProfile;
using secure_frontend::EngineProtocolFlavor;
using secure_frontend::FamilyDescriptor;
using secure_frontend::ModelOptionKind;
using secure_frontend::ModelOptionValues;
using secure_frontend::NativeChatProfile;
using secure_frontend::PluginFamily;
using secure_frontend::PluginFamilySource;
using secure_frontend::PluginManifest;
using secure_frontend::TokenizerMode;
using secure_frontend::TokenizerSource;
using Strings = std::vector<std::string>;

const std::string kSha = std::string(64, 'a');
#if defined(_WIN32)
const std::string kAbsoluteGguf = "C:\\models\\acme3-mtp.gguf";
#else
const std::string kAbsoluteGguf = "/models/acme3-mtp.gguf";
#endif

// The manifest test's engine block, with a flag that takes no value (--no-graph) set by a boolean.
const std::string kAcmeEngine = R"({
    "abi": 1, "dialect": "strata_0143", "chat": "flint",
    "tokenizer": {"source": "gguf", "mode": "flint_bpe"},
    "models": {"gguf_architectures": ["acme3", "acme3moe"], "context_key": "acme3.context_length", "mtp_key": "acme3.nextn_predict_layers"},
    "arguments": [
      {"flag": "--max-context", "value": "integer", "min": 4096, "max": 262144},
      {"flag": "--expert-vram", "value": "gibibytes", "min": 0, "max": 1024},
      {"flag": "--cpu-threads", "value": "integer", "min": 1, "max": 256},
      {"flag": "--pcie-frac", "value": "fraction"},
      {"flag": "--cpu-lane", "value": "on_off"},
      {"flag": "--no-graph", "value": "none"},
      {"flag": "--kv", "value": "choice", "choices": ["f16", "q8"]},
      {"flag": "--mtp", "value": "gguf_file"}],
    "options": [
      {"name": "max_context", "kind": "integer", "label": "Context length", "help": "Tokens the model reads.", "flag": "--max-context",
       "min": 4096, "max": 262144, "values": [8192, 32768, 65536, 131072], "default": 65536},
      {"name": "expert_vram", "kind": "integer_or_auto", "label": "Expert VRAM", "flag": "--expert-vram", "min": 0, "max": 1024, "default": "auto"},
      {"name": "gpus", "kind": "devices", "label": "GPUs", "max": 8},
      {"name": "pcie_frac", "kind": "decimal", "label": "PCIe share", "flag": "--pcie-frac", "min": 0, "max": 1},
      {"name": "cpu_lane", "kind": "boolean", "label": "CPU lane", "flag": "--cpu-lane", "default": true},
      {"name": "graph_off", "kind": "boolean", "label": "No CUDA graph", "flag": "--no-graph", "default": false},
      {"name": "kv", "kind": "choice", "label": "KV cache", "flag": "--kv", "choices": ["f16", "q8"], "default": "q8"}],
    "sampling": {"temperature": 1.0, "top_p": 0.95, "top_k": 64, "min_p": 0.05},
    "calibration": {"before_first_start": true, "keep_margin": 0.03,
                    "steps": [{"flag": "--cpu-threads", "values": ["{cores}", "{cores*2/3}", "{cores/2}"]},
                              {"flag": "--pcie-frac", "values": ["0", "0.25", "0.5", "0.75"]}]},
    "monitor": {"experts_vram": "vram_slots", "experts_ram": "pinned_gib"},
    "capabilities": {"vision": false, "web_search": true, "mtp": "file"}
  })";

std::string manifest_text(const std::string& id, const std::string& name, const std::string& engine) {
  return R"({"schema": "m-strata.plugin/1", "id": ")" + id + R"(", "kind": "engine", "name": ")" + name +
         R"(", "version": "0.1.0", "publisher": "Example Labs", "license": "Apache-2.0", "about": "A family.",
             "files": {"windows": ["multiengine_)" + id + R"(.dll"], "linux": ["libmultiengine_)" + id + R"(.so"]}, "engine": )" +
         engine + "}";
}

PluginManifest manifest_of(const std::string& id, const std::string& name, const std::string& engine) {
  auto parsed = secure_frontend::parse_plugin_manifest(manifest_text(id, name, engine), id);
  if (const auto* reason = std::get_if<std::string>(&parsed)) {
    expect(false, "the test's manifest " + id + " reads: " + *reason);
    return PluginManifest{};
  }
  return std::get<PluginManifest>(std::move(parsed));
}

PluginManifest acme3() { return manifest_of("acme3", "Acme-3", kAcmeEngine); }

// The least an engine plugin declares: no options, sampling or calibration; a pack's tokenizer, the unified dialect.
const std::string kBetaEngine = R"({"abi": 1, "dialect": "unified", "chat": "qwen", "tokenizer": {"source": "pack", "mode": "byte_bpe"},
    "models": {"gguf_architectures": ["beta"], "context_key": "beta.context_length"},
    "arguments": [{"flag": "--max-context", "value": "integer", "min": 1, "max": 65536}],
    "capabilities": {"vision": true, "web_search": false, "mtp": "none"}})";

std::vector<std::string> names_of(const FamilyDescriptor& family) {
  std::vector<std::string> out;
  for (const auto& spec : family.options) out.emplace_back(spec.name);
  return out;
}

std::string joined(const std::vector<std::string>& items) {
  std::string out;
  for (const auto& item : items) out += (out.empty() ? "" : " ") + item;
  return out;
}

// --- The descriptor ----------------------------------------------------------------------------

// Each part from its manifest, from the server's closed lists; what it composes no tails, rows or files for, off.
void an_engine_plugin_makes_its_family() {
  const PluginFamily family{acme3(), kSha, static_cast<EngineProfile>(secure_frontend::kFirstPluginProfile)};
  const auto& d = family.descriptor();
  expect(d.profile == static_cast<EngineProfile>(secure_frontend::kFirstPluginProfile) && d.id == "acme3" && family.name() == "Acme-3",
         "its profile, id and name");
  expect(d.chat_profile == NativeChatProfile::flint && d.protocol_flavor == EngineProtocolFlavor::strata_0143 &&
             d.environment_family == EngineFamily::strata && d.arguments_profile == EngineArgumentsProfile::plugin,
         "its renderer and dialect as declared; the strata environment's allowlist alone; its own arguments");
  expect(d.tokenizer_mode == TokenizerMode::flint_bpe && d.tokenizer_source == TokenizerSource::gguf, "its tokenizer");
  expect(d.per_request_reasoning_effort, "its renderer's own family (flint) takes a request's reasoning_effort");
  expect(d.refuses_qwen_launch_rows && d.engine_words == "the Acme-3 engine" && d.template_words == "Acme-3's template",
         "qwen's launch rows refused, in its own words: " + std::string{d.engine_words});
  expect(!d.glm_pack_rows && !d.composes_strata_tails && !d.effort_end_tail && !d.reads_conversation_cache && !d.download_template_card &&
             !d.shipped_expert_profile && !d.prefill_calibration && !d.maya_pack_model_file && !d.yarn_trained_context,
         "no built-in family's rows, tails or files");
  expect(!d.reads_images && d.checks_mtp_side_file && d.draft_layer == DraftLayerKind::file, "its capabilities: no pictures, an MTP file");
  expect(d.calibrates_before_first_start && d.sweep_keep_margin == 0.03, "its calibration");
  expect(d.sampling.temperature == 1.0 && d.sampling.top_p == 0.95 && d.sampling.top_k == 64 && d.sampling.min_p == 0.05, "its sampling");
  expect(d.plugin == &family.engine() && d.plugin->arguments.size() == 8U && d.library_sha256 == kSha,
         "its block (the flags its engine takes) and its library's SHA-256");
  expect(d.monitor_tiers.experts_vram == "vram_slots" && d.monitor_tiers.experts_ram == "pinned_gib", "its Monitor's tier keys");
  const auto& qwen = secure_frontend::family_descriptor(EngineProfile::qwen).monitor_tiers;
  expect(qwen.experts_vram.empty() && qwen.experts_ram.empty(), "a built-in family names none: the Monitor tells them apart");

  const PluginFamily beta{manifest_of("beta", "Beta", kBetaEngine), kSha, static_cast<EngineProfile>(secure_frontend::kFirstPluginProfile + 1)};
  const auto& b = beta.descriptor();
  expect(b.chat_profile == NativeChatProfile::qwen && b.protocol_flavor == EngineProtocolFlavor::unified &&
             b.tokenizer_source == TokenizerSource::pack && b.tokenizer_mode == TokenizerMode::byte_bpe,
         "the least plugin: its renderer, dialect and tokenizer");
  expect(b.reads_images && !b.checks_mtp_side_file && b.draft_layer == DraftLayerKind::none && !b.calibrates_before_first_start &&
             b.sweep_keep_margin == 0.03 && b.sampling.temperature == 1.0 && b.sampling.top_p == 0.95 && !b.sampling.top_k && !b.sampling.min_p,
         "the least plugin: pictures, no draft layer, the default calibration and sampling");
  expect(b.monitor_tiers.experts_vram.empty() && b.monitor_tiers.experts_ram.empty(), "the least plugin: no Monitor keys");
}

// Its own options in its order (but its GPUs), then the server's - temperature, top_p, top_k and min_p (its sampling names
// both), answer_minutes, web_search (it searches) - then its GPUs; each as declared.
void its_cards_offer_its_options_then_the_servers() {
  const PluginFamily family{acme3(), kSha, static_cast<EngineProfile>(secure_frontend::kFirstPluginProfile)};
  const auto& d = family.descriptor();
  expect(names_of(d) == Strings{"max_context", "expert_vram", "pcie_frac", "cpu_lane", "graph_off", "kv", "temperature", "top_p",
                                "top_k", "min_p", "answer_minutes", "web_search", "gpus"},
         "its options: " + joined(names_of(d)));
  const auto spec = [&d](const std::string_view name) {
    const auto found = std::find_if(d.options.begin(), d.options.end(), [name](const auto& each) { return each.name == name; });
    return found == d.options.end() ? nullptr : &*found;
  };
  const auto* context = spec("max_context");
  expect(context != nullptr && context->kind == ModelOptionKind::integer && context->label == "Context length" &&
             context->help == "Tokens the model reads." && context->minimum == 4096 && context->maximum == 262144 &&
             std::vector<std::int64_t>(context->values.begin(), context->values.end()) == std::vector<std::int64_t>{8192, 32768, 65536, 131072},
         "max_context as declared");
  const auto* vram = spec("expert_vram");
  expect(vram != nullptr && vram->kind == ModelOptionKind::integer_or_auto && vram->minimum == 0 && vram->maximum == 1024, "expert_vram");
  const auto* share = spec("pcie_frac");
  expect(share != nullptr && share->kind == ModelOptionKind::decimal && share->minimum_decimal == 0.0 && share->maximum_decimal == 1.0,
         "pcie_frac");
  const auto* kv = spec("kv");
  expect(kv != nullptr && kv->kind == ModelOptionKind::choice &&
             std::vector<std::string_view>(kv->choices.begin(), kv->choices.end()) == std::vector<std::string_view>{"f16", "q8"},
         "kv's choices");
  const auto* gpus = spec("gpus");
  expect(gpus != nullptr && gpus->kind == ModelOptionKind::devices && gpus->minimum == 1 && gpus->maximum == 8, "its GPUs, up to 8");
  const auto* temperature = spec("temperature");
  expect(temperature != nullptr && temperature->label == "Temperature" && temperature->maximum_decimal == 2.0, "the server's temperature");
  expect(std::vector<std::string_view>(d.option_controlled_flags.begin(), d.option_controlled_flags.end()) ==
             std::vector<std::string_view>{"--max-context", "--expert-vram", "--pcie-frac", "--cpu-lane", "--no-graph", "--kv"},
         "the flags its options own");
  expect(family.option_flag("kv") == "--kv" && family.option_flag("gpus").empty() && family.option_flag("temperature").empty(),
         "each option's flag");

  const PluginFamily beta{manifest_of("beta", "Beta", kBetaEngine), kSha, static_cast<EngineProfile>(secure_frontend::kFirstPluginProfile + 1)};
  expect(names_of(beta.descriptor()) == Strings{"temperature", "top_p", "answer_minutes"},
         "the least plugin: the server's options only (no top_k or min_p, no search, no GPUs): " + joined(names_of(beta.descriptor())));
}

// A declared option may not take a server option's name: its cards have that one already.
void a_manifest_may_not_take_a_server_options_name() {
  for (const std::string name : {"temperature", "top_p", "top_k", "min_p", "answer_minutes", "web_search"}) {
    std::string engine = kBetaEngine;
    const std::string arguments = R"("arguments": [{"flag": "--max-context", "value": "integer", "min": 1, "max": 65536}])";
    engine.replace(engine.find(arguments), arguments.size(),
                   arguments + R"(, "options": [{"name": ")" + name + R"(", "kind": "integer", "label": "X", "flag": "--max-context", "min": 1, "max": 2}])");
    const auto parsed = secure_frontend::parse_plugin_manifest(manifest_text("beta", "Beta", engine), "beta");
    const auto* reason = std::get_if<std::string>(&parsed);
    expect(reason != nullptr && *reason == "engine.options: " + name + " is one of the server's own options (every card has it)",
           name + " refused: " + (reason != nullptr ? *reason : std::string{"(accepted)"}));
  }
}

// --- The families the server runs ---------------------------------------------------------------

// Set once at the start: each takes the profile after the built-ins' and the ones before it; family_named and
// family_descriptor find it; the built-ins are as they were. Another kind, a built-in's id or one taken twice is left out.
void the_server_finds_the_families_it_runs() {
  PluginManifest tool = acme3();
  tool.id = "searcher";
  tool.kind = secure_frontend::PluginKind::tool;
  PluginManifest qwen = acme3();
  qwen.id = "qwen";
  secure_frontend::set_plugin_families({PluginFamilySource{tool, kSha}, PluginFamilySource{acme3(), kSha}, PluginFamilySource{qwen, kSha},
                                        PluginFamilySource{acme3(), std::string(64, 'b')},
                                        PluginFamilySource{manifest_of("beta", "Beta", kBetaEngine), kSha}});
  const auto first = static_cast<EngineProfile>(secure_frontend::kFirstPluginProfile);
  const auto second = static_cast<EngineProfile>(secure_frontend::kFirstPluginProfile + 1);
  const auto* acme = secure_frontend::family_named("acme3");
  const auto* beta = secure_frontend::family_named("beta");
  expect(acme != nullptr && acme->profile == first && acme->library_sha256 == kSha, "acme3: the first plugin profile, its first SHA-256");
  expect(beta != nullptr && beta->profile == second, "beta: the next one");
  expect(secure_frontend::family_named("searcher") == nullptr, "a tool plugin is no family");
  expect(secure_frontend::family_named("qwen") == &secure_frontend::family_descriptor(EngineProfile::qwen) &&
             secure_frontend::family_named("qwen")->plugin == nullptr,
         "qwen is still the built-in");
  expect(&secure_frontend::family_descriptor(first) == acme && &secure_frontend::family_descriptor(second) == beta,
         "family_descriptor finds each by its profile");
  expect(secure_frontend::plugin_families().size() == 2U && secure_frontend::all_families().size() == 9U, "two plugin families of nine");
  expect(secure_frontend::family_choices() == "qwen, glm53, gemma4, deepseek, raon1, flint, minimax, acme3 or beta",
         "the names a refusal lists: " + secure_frontend::family_choices());
  expect(secure_frontend::is_model_option_name("pcie_frac") && secure_frontend::find_model_option(first, "pcie_frac") != nullptr,
         "its options are options");

  secure_frontend::set_plugin_families({});
  expect(secure_frontend::family_named("acme3") == nullptr && secure_frontend::plugin_families().empty() &&
             secure_frontend::family_choices() == "qwen, glm53, gemma4, deepseek, raon1, flint or minimax" &&
             !secure_frontend::is_model_option_name("pcie_frac"),
         "replaced: none");
  expect(acme != nullptr && acme->id == "acme3", "a family it replaced stays readable");
}

// --- Its cards and its engine ------------------------------------------------------------------

Strings args_of(const secure_frontend::FrontendConfigDraft& draft) {
  const auto found = draft.entries.find("engine.args");
  const auto* list = found == draft.entries.end() ? nullptr : std::get_if<Strings>(&found->second.value);
  return list == nullptr ? Strings{} : *list;
}

// Each declared option writes its flag as its argument takes it; its GPUs engine.gpu; the server's options their rows.
void its_options_write_their_flags() {
  secure_frontend::set_plugin_families({PluginFamilySource{acme3(), kSha}});
  const auto* named = secure_frontend::family_named("acme3");
  expect(named != nullptr, "acme3 is a family once the server runs it");
  if (named == nullptr) return;
  const auto profile = named->profile;
  const auto written = [profile](const ModelOptionValues& values) {
    secure_frontend::FrontendConfigDraft draft;
    secure_frontend::apply_option_values(profile, values, draft);
    return draft;
  };
  const auto draft = written(ModelOptionValues{{"max_context", std::int64_t{65536}}, {"expert_vram", std::string{"auto"}},
                                               {"pcie_frac", 0.25}, {"cpu_lane", false}, {"graph_off", true}, {"kv", std::string{"q8"}},
                                               {"gpus", std::string{"0,1"}}, {"temperature", 0.7}, {"top_k", std::int64_t{40}},
                                               {"answer_minutes", std::int64_t{10}}});
  expect(args_of(draft) == Strings{"--max-context", "65536", "--pcie-frac", "0.25", "--cpu-lane", "off", "--no-graph", "--kv", "q8"},
         "its flags, auto writing none: " + joined(args_of(draft)));
  const auto row = [&draft](const std::string& key) { return draft.entries.find(key) != draft.entries.end(); };
  expect(row("engine.gpu") && std::get<std::string>(draft.entries.at("engine.gpu").value) == "0,1", "its GPUs: engine.gpu");
  expect(row("defaults.sampling.temperature") && row("defaults.sampling.top_k") && row("limits.engine.generation_timeout_s"),
         "the server's options: their rows");
  const auto other = written(ModelOptionValues{{"expert_vram", std::int64_t{12}}, {"cpu_lane", true}, {"graph_off", false},
                                               {"pcie_frac", 0.12345}, {"gpus", std::string{"auto"}}});
  expect(args_of(other) == Strings{"--expert-vram", "12", "--pcie-frac", "0.123", "--cpu-lane", "on"},
         "a number, a fraction at three places, on; a flag without a value left out: " + joined(args_of(other)));
  expect(other.entries.find("engine.gpu") == other.entries.end(), "GPUs auto: the card's own rows stand");
  expect(secure_frontend::option_flags(profile, "kv") == std::vector<std::string_view>{"--kv"} &&
             secure_frontend::option_flags(profile, "gpus").empty() && secure_frontend::option_flags(profile, "temperature").empty(),
         "the flags each option owns");
  secure_frontend::set_plugin_families({});
}

// Its engine takes the flags its manifest declares, each of its kind within its bounds, once; nothing else.
void its_engine_takes_only_its_flags() {
  secure_frontend::set_plugin_families({PluginFamilySource{acme3(), kSha}});
  const auto* named = secure_frontend::family_named("acme3");
  expect(named != nullptr, "acme3 is a family once the server runs it");
  if (named == nullptr) return;
  const auto& family = *named;
  const auto check = [&family](const Strings& args) { return secure_frontend::validate_engine_user_arguments(family, args); };
  const auto refused = [&check](const Strings& args, const std::string& words) {
    const auto error = check(args);
    expect(error.has_value() && error->message == words,
           joined(args) + " refused: " + (error ? error->message : std::string{"(accepted)"}) + " (wanted: " + words + ")");
  };
  const auto accepted = check({"--max-context", "65536", "--expert-vram", "12.5", "--cpu-threads", "8", "--pcie-frac", "0.25",
                               "--cpu-lane", "on", "--no-graph", "--kv", "f16", "--mtp", kAbsoluteGguf});
  expect(!accepted.has_value(), "each declared flag: " + (accepted ? accepted->message : std::string{}));
  refused({"--chunk", "512"}, "engine.profile acme3: the engine argument is not one of the Acme-3 engine's flags");
  refused({"--max-context", "1024"}, "engine.profile acme3: --max-context must be 4096-262144");
  refused({"--pcie-frac", "1.5"}, "engine.profile acme3: --pcie-frac must be 0-1");
  refused({"--cpu-lane", "yes"}, "engine.profile acme3: --cpu-lane must be on or off");
  refused({"--kv", "q4"}, "engine.profile acme3: --kv must be f16 or q8");
  refused({"--mtp", "draft.gguf"}, "engine.profile acme3: --mtp takes the absolute path of a .gguf file");
  refused({"--kv", "f16", "--kv", "q8"}, "engine.profile acme3: --kv is given more than once");
  refused({"--max-context=65536"}, "engine.profile acme3: --max-context takes its value as the next argument");
  const auto owned = check({"--gpu", "0"});
  expect(owned.has_value() && owned->code == secure_frontend::EngineArgumentsErrorCode::owned_flag, "--gpu is the frontend's");
  const auto by_profile = secure_frontend::validate_engine_user_arguments(EngineArgumentsProfile::plugin, Strings{"--max-context", "65536"});
  expect(by_profile.has_value(), "the plugin arguments profile alone takes nothing: its family checks them");
  expect(!secure_frontend::validate_engine_user_arguments(secure_frontend::family_descriptor(EngineProfile::gemma4), Strings{"--chunk", "512"})
              .has_value(),
         "a built-in family: its arguments profile, as before");
  secure_frontend::set_plugin_families({});
}

// A card names a plugin family by its id: its options are the family's, its engine.args may not fix one of their flags,
// and a start's config is that family's.
void a_card_names_a_plugin_family() {
  secure_frontend::set_plugin_families({PluginFamilySource{acme3(), kSha}});
  const auto* named = secure_frontend::family_named("acme3");
  expect(named != nullptr, "acme3 is a family once the server runs it");
  if (named == nullptr) return;
  const auto profile = named->profile;
  const auto directory = secure_frontend_test::make_scratch_directory();
  const auto card = [&directory](const std::string& args, const std::string& options) {
    return secure_frontend::parse_model_card(
        R"({"schema_version": 1, "name": "Acme-3-30B", "profile": "acme3", "config": {"engine": {"args": )" + args +
            R"(}}, "options": )" + options + "}",
        directory);
  };
  const auto parsed = card(R"(["--cpu-threads", "8"])", R"({"max_context": {"default": 65536}, "kv": {"default": "q8"}, "pcie_frac": {"default": 0.25}})");
  const auto* read = std::get_if<secure_frontend::ModelCard>(&parsed);
  std::string issues;
  if (const auto* errors = std::get_if<secure_frontend::ConfigErrors>(&parsed)) {
    for (const auto& issue : *errors) issues += secure_frontend::format_issue(issue) + "; ";
  }
  expect(read != nullptr && read->profile == profile && read->options.size() == 3U, "a card of acme3 reads, its options its family's: " + issues);
  if (read != nullptr) {
    const auto draft = secure_frontend::card_draft(secure_frontend::FrontendConfigDraft{}, *read,
                                                   ModelOptionValues{{"max_context", std::int64_t{32768}}, {"kv", std::string{"f16"}}});
    expect(std::get<std::string>(draft.entries.at("engine.profile").value) == "acme3", "its start's engine.profile names the family");
    expect(args_of(draft) == Strings{"--cpu-threads", "8", "--max-context", "32768", "--kv", "f16"}, "its args, then its options' flags: " +
                                                                                                         joined(args_of(draft)));
  }
  const auto fixed = card(R"(["--kv", "q8"])", R"({"kv": {"default": "q8"}})");
  expect(std::holds_alternative<secure_frontend::ConfigErrors>(fixed), "a card whose engine.args fixes an option's flag is refused");
  const auto foreign = card(R"([])", R"({"chunk": {"default": 512}})");
  expect(std::holds_alternative<secure_frontend::ConfigErrors>(foreign), "another family's option is not one of it");

  // its start's whole config: the family's profile, its flags checked as its engine takes them
  secure_frontend_test::ConfigFixture f;
  auto rows = f.literals;
  rows.erase("roots.secret");
  rows.erase("schema_version");
  const auto whole = [&f, &rows](const std::string& args) {
    auto card_rows = rows;
    card_rows["engine.args"] = args;
    const auto text = R"({"schema_version": 1, "name": "Acme-3-30B", "profile": "acme3", "config": )" +
                      secure_frontend::render_config_literals(card_rows) + R"(, "options": {"kv": {"default": "q8"}}})";
    auto main_text = secure_frontend::render_config_literals(
        {{"schema_version", "1"}, {"roots.secret", f.literals.at("roots.secret")},
         {"models.cards", "[" + secure_frontend_test::json_literal((f.dir / "cards" / "a.json").string()) + "]"}});
    const auto main = secure_frontend::parse_config_text(main_text, f.dir);
    const auto parsed_card = secure_frontend::parse_model_card(text, f.dir);
    const auto* main_draft = std::get_if<secure_frontend::FrontendConfigDraft>(&main);
    const auto* read_card = std::get_if<secure_frontend::ModelCard>(&parsed_card);
    expect(main_draft != nullptr && read_card != nullptr, "the main config and the card read");
    return main_draft != nullptr && read_card != nullptr
               ? secure_frontend::card_config(*main_draft, *read_card, ModelOptionValues{{"kv", std::string{"f16"}}})
               : secure_frontend::ConfigValidationResult{secure_frontend::ConfigErrors{}};
  };
  const auto started = whole(R"(["--cpu-threads", "8"])");
  const auto* validated = std::get_if<secure_frontend::ValidatedConfig>(&started);
  std::string why_not;
  if (const auto* errors = std::get_if<secure_frontend::ConfigErrors>(&started)) {
    for (const auto& issue : *errors) why_not += secure_frontend::format_issue(issue) + "; ";
  }
  expect(validated != nullptr, "its start validates: " + why_not);
  if (validated != nullptr) {
    expect(validated->get().engine.profile == std::optional<EngineProfile>{profile} &&
               validated->get().engine.args == Strings{"--cpu-threads", "8", "--kv", "f16"},
           "its profile and its flags");
  }
  const auto refused_start = whole(R"(["--cpu-threads", "999"])");
  const auto* refusal = std::get_if<secure_frontend::ConfigErrors>(&refused_start);
  expect(refusal != nullptr && std::any_of(refusal->begin(), refusal->end(), [](const auto& issue) {
           return issue.key_path == "engine.args" && issue.reason == "engine.profile acme3: --cpu-threads must be 1-256";
         }),
         "a flag past its bounds: engine.args refused in its family's words");
  secure_frontend::set_plugin_families({});
  const auto gone = card(R"([])", R"({})");
  expect(std::holds_alternative<secure_frontend::ConfigErrors>(gone), "without the plugin, its card is refused");
}

// --- Its start -----------------------------------------------------------------------------------

// Acme-3's block with its draft file on another flag than --mtp.
PluginManifest gamma() {
  std::string engine = kAcmeEngine;
  const std::string from = R"({"flag": "--mtp", "value": "gguf_file"})";
  engine.replace(engine.find(from), from.size(), R"({"flag": "--drafter", "value": "gguf_file"})");
  return manifest_of("gamma", "Gamma", engine);
}

using Composed = std::variant<secure_frontend::ComposedLaunch, secure_frontend::LaunchFailure>;

std::string failure_of(const Composed& composed) {
  std::string out;
  if (const auto* failure = std::get_if<secure_frontend::LaunchFailure>(&composed)) {
    for (const auto& message : failure->messages) out += message + "; ";
  }
  return out;
}

// A card's start as the launcher composes it: <models>/<family>/a.gguf, a config naming the family and `engine_members`.
struct Start final {
  explicit Start(const std::string& id) : family(id) {
    secure_frontend_test::make_private_directory(h.models_path / id);
    secure_frontend_test::write_file(h.models_path / id / "a.gguf", "GGUF");
  }
  Composed launch(const std::string& engine_members) {
    h.write_config("");
    std::string text = secure_frontend_test::read_file(h.config);
    const std::string from = "\"model_id\": \"m\"";
    text.replace(text.find(from), from.size(), "\"model_id\": \"a\", \"profile\": \"" + family + "\"" + engine_members);
    secure_frontend_test::write_file(h.config, text);
    return secure_frontend::compose_engine_launch(h.validated_config().get(), true);
  }
  std::string native() const {
    return secure_frontend::path_to_utf8((h.validated_config().get().roots.models.path() / family / "a.gguf").make_preferred());
  }
  secure_frontend_test::cli::CliHarness h;
  std::string family;
};

// The launcher reads --family <id> --family-sha256 <hex> first: they are the start's own launcher arguments, which the
// process puts before --serve. Its engine's are its card's declared flags, then --gpu, then --native last: no qwen tails,
// no MTP strip, nothing of another family's.
void its_card_starts_through_the_launcher() {
  secure_frontend::set_plugin_families({PluginFamilySource{acme3(), kSha}});
  const auto* named = secure_frontend::family_named("acme3");
  expect(named != nullptr, "acme3 is a family once the server runs it");
  if (named == nullptr) return;
  Start start{"acme3"};
  const auto composed = start.launch(R"(, "args": ["--max-context", "65536", "--kv", "q8", "--no-graph"], "gpu": "0,1")");
  const auto* launch = std::get_if<secure_frontend::ComposedLaunch>(&composed);
  expect(launch != nullptr, "its start composes: " + failure_of(composed));
  if (launch != nullptr) {
    expect(launch->launcher_args == Strings{"--family", "acme3", "--family-sha256", kSha},
           "the launcher's: its family and its library's SHA-256: " + joined(launch->launcher_args));
    expect(launch->engine_args == Strings{"--max-context", "65536", "--kv", "q8", "--no-graph", "--gpu", "0,1", "--native", start.native()},
           "its flags, then --gpu, then --native: " + joined(launch->engine_args));
    expect(launch->profile == named->profile && !launch->managed && launch->model_id == "a" && launch->mtp_removed.empty(),
           "its profile; an expert start of the inventory's model; nothing stripped");
  }
  const auto bare = start.launch("");
  const auto* bare_launch = std::get_if<secure_frontend::ComposedLaunch>(&bare);
  expect(bare_launch != nullptr && bare_launch->engine_args == Strings{"--native", start.native()} &&
             bare_launch->launcher_args == Strings{"--family", "acme3", "--family-sha256", kSha},
         "no args and no GPU: --native alone, and the launcher's: " + failure_of(bare));
  const auto one = start.launch(R"(, "gpu": "0", "hip_ordinal": 1)");
  const auto* one_launch = std::get_if<secure_frontend::ComposedLaunch>(&one);
  expect(one_launch != nullptr && one_launch->engine_args == Strings{"--gpu", "1", "--native", start.native()},
         "engine.hip_ordinal wins over one card, as on the built-ins: " + failure_of(one));
  Start built_in{"gemma4"};
  const auto gemma = built_in.launch("");
  const auto* gemma_launch = std::get_if<secure_frontend::ComposedLaunch>(&gemma);
  expect(gemma_launch != nullptr && gemma_launch->launcher_args.empty(), "a built-in family's start: none for the launcher");
  secure_frontend::set_plugin_families({});
}

// Each file its engine's flags name (a gguf_file argument - its draft file, on whatever flag) is held at each start to the
// model folders' rule, as a built-in card's MTP file is: a regular file, not a link, in a folder no other user of this PC
// may change. One that is not is refused by its flag, never its path.
void its_files_are_held_to_the_model_folders_rule() {
  secure_frontend::set_plugin_families({PluginFamilySource{gamma(), kSha}});
  expect(secure_frontend::family_named("gamma") != nullptr, "gamma is a family once the server runs it");
  Start start{"gamma"};
  const auto folder = start.h.models_path / "gamma" / "draft";
  secure_frontend_test::make_private_directory(folder);
  const auto file = folder / "draft-a.gguf";
  const auto with_file = [&start, &file] {
    return start.launch(R"(, "args": ["--drafter", )" + secure_frontend_test::json_literal(secure_frontend::path_to_utf8(file)) + "]");
  };
  const auto refused_by_flag = [](const Composed& composed) {
    const auto* failure = std::get_if<secure_frontend::LaunchFailure>(&composed);
    return failure != nullptr && !failure->messages.empty() && failure->messages.front().find("--drafter") != std::string::npos &&
           failure->messages.front().find("draft-a") == std::string::npos;
  };
  const auto missing = with_file();
  expect(refused_by_flag(missing), "a file that is not there is refused by its flag: " + failure_of(missing));
  secure_frontend_test::write_file(file, "GGUF");
  const auto composed = with_file();
  const auto* launch = std::get_if<secure_frontend::ComposedLaunch>(&composed);
  expect(launch != nullptr && launch->engine_args == Strings{"--drafter", secure_frontend::path_to_utf8(file), "--native", start.native()},
         "a file in a folder only this user changes: as given, before --native: " + failure_of(composed));
#if defined(_WIN32)
  expect(secure_frontend_test::grant_everyone_write(folder), "the test widens the draft file's folder");
#else
  ::chmod(folder.c_str(), 0777);
#endif
  const auto widened = with_file();
  expect(refused_by_flag(widened), "a file in a folder other users may change is refused by its flag: " + failure_of(widened));
  secure_frontend::set_plugin_families({});
}

std::string described(const std::vector<secure_frontend::SweepStep>& plan) {
  std::string out;
  for (const auto& step : plan) out += (out.empty() ? "" : "; ") + step.flag + " " + joined(step.values);
  return out;
}

// Its sweep is its manifest's steps, each candidate this PC's own count - {cores} its physical cores, {threads} its logical
// ones, {cores/N} and {cores*N/M} whole cores (rounded down, at least one) - within the flag's bounds and without repeats;
// its margin and first start as declared. A result kept for its card holds only its steps' flags, each within its bounds,
// and names the engine it was measured with: the launcher and the plugin's library.
void its_calibration_is_its_manifests() {
  std::string threads_engine = kAcmeEngine;
  const std::string half = R"("{cores/2}")";
  threads_engine.replace(threads_engine.find(half), half.size(), R"("{threads}")");
  secure_frontend::set_plugin_families({PluginFamilySource{acme3(), kSha}, PluginFamilySource{manifest_of("delta", "Delta", threads_engine), kSha},
                                        PluginFamilySource{manifest_of("beta", "Beta", kBetaEngine), kSha}});
  const auto* acme = secure_frontend::family_named("acme3");
  const auto* delta = secure_frontend::family_named("delta");
  const auto* beta = secure_frontend::family_named("beta");
  expect(acme != nullptr && delta != nullptr && beta != nullptr, "the three are families once the server runs them");
  if (acme == nullptr || delta == nullptr || beta == nullptr) return;
  using secure_frontend::SweepMachine;
  const auto plan = secure_frontend::sweep_plan(acme->profile, SweepMachine{16U, 8U});
  expect(plan.size() == 2U && plan[0].flag == "--cpu-threads" && plan[0].values == Strings{"8", "5", "4"} && plan[0].joint.empty() &&
             plan[0].max_tokens == 128U && plan[0].stop_below == 0.0 && plan[1].flag == "--pcie-frac" &&
             plan[1].values == Strings{"0", "0.25", "0.5", "0.75"},
         "its steps on 8 cores and 16 threads: " + described(plan));
  const auto big = secure_frontend::sweep_plan(acme->profile, SweepMachine{512U, 300U});
  expect(!big.empty() && big[0].values == Strings{"256", "200", "150"}, "within --cpu-threads' bounds (1-256): " + described(big));
  const auto one = secure_frontend::sweep_plan(acme->profile, SweepMachine{1U, 1U});
  expect(!one.empty() && one[0].values == Strings{"1"}, "one core: one candidate, at least one: " + described(one));
  const auto threads = secure_frontend::sweep_plan(delta->profile, SweepMachine{16U, 8U});
  expect(!threads.empty() && threads[0].values == Strings{"8", "5", "16"}, "{threads}: the logical CPUs: " + described(threads));
  expect(secure_frontend::sweep_keep_margin(acme->profile) == 0.03 && secure_frontend::calibrates_before_first_start(acme->profile),
         "its margin and its first start");
  expect(secure_frontend::sweep_plan(beta->profile, SweepMachine{16U, 8U}).empty() && !secure_frontend::calibrates_before_first_start(beta->profile),
         "no calibration block: no sweep");

  const auto stored = [](const std::string& args) {
    return R"({"schema_version": 1, "card": "Acme-3-30B", "args": [)" + args +
           R"(], "baseline_tok_s": 10.0, "best_tok_s": 12.0, "date": "2026-10-31T00:00:00Z", "auto_recalibrate": true,
              "key": {"gpu": "", "cpu": "x", "ram_bytes": 1, "engine": "e", "model": "m", "options": ""}})";
  };
  const auto read = [&stored](const std::string& args, const FamilyDescriptor& family) {
    return secure_frontend::parse_calibration(stored(args), family).has_value();
  };
  expect(read(R"("--cpu-threads", "5", "--pcie-frac", "0.25")", *acme), "its own steps' flags within their bounds");
  expect(!read(R"("--cpu-threads", "300")", *acme), "a count past its flag's bound");
  expect(!read(R"("--pcie-frac", "1.5")", *acme), "a share past 1");
  expect(!read(R"("--max-context", "65536")", *acme), "a flag it declares but does not sweep");
  expect(!read(R"("--pool-workers", "4")", *acme), "a built-in sweep's flag is none of its");
  expect(read(R"("--pool-workers", "4")", secure_frontend::family_descriptor(EngineProfile::qwen)),
         "a built-in family's: the built-in sweeps' flags, as before");

  const auto directory = secure_frontend_test::make_scratch_directory();
  const auto launcher = directory / "multiengine";
  secure_frontend_test::write_file(launcher, "MZ");
  expect(secure_frontend::engine_fact(launcher, *acme) == secure_frontend::file_fact(launcher) + ";" + kSha,
         "its result's engine: the launcher and its library's SHA-256");
  expect(secure_frontend::engine_fact(launcher, secure_frontend::family_descriptor(EngineProfile::qwen)) == secure_frontend::file_fact(launcher),
         "a built-in family's: the launcher alone, as before");
  secure_frontend::set_plugin_families({});
}

}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"an_engine_plugin_makes_its_family", an_engine_plugin_makes_its_family},
      {"its_cards_offer_its_options_then_the_servers", its_cards_offer_its_options_then_the_servers},
      {"a_manifest_may_not_take_a_server_options_name", a_manifest_may_not_take_a_server_options_name},
      {"the_server_finds_the_families_it_runs", the_server_finds_the_families_it_runs},
      {"its_options_write_their_flags", its_options_write_their_flags},
      {"its_engine_takes_only_its_flags", its_engine_takes_only_its_flags},
      {"a_card_names_a_plugin_family", a_card_names_a_plugin_family},
      {"its_card_starts_through_the_launcher", its_card_starts_through_the_launcher},
      {"its_files_are_held_to_the_model_folders_rule", its_files_are_held_to_the_model_folders_rule},
      {"its_calibration_is_its_manifests", its_calibration_is_its_manifests},
  }, failures);
}
