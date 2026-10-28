#pragma once
// SP4 Task 23: drives run_strata in-process against a private scratch tree.
#include "secure_frontend/config_file.hpp"
#include "secure_frontend/strata_cli.hpp"
#include "support/acquire_fixtures.hpp"

#include <sstream>

namespace secure_frontend_test::cli {

// A private scratch tree with roots.models, a tokenizer root, a fake engine
// file and a config whose pull.downloader does not exist (local verbs never need it).
struct CliHarness final {
  // `models_dir` is UTF-8 (Task 24b: a non-ASCII roots.models proves the engine argv is UTF-8).
  explicit CliHarness(const std::string_view models_dir = "models") : scratch(secure_frontend_test::make_scratch_directory()) {
    models_path = scratch / secure_frontend::path_from_utf8(models_dir);
    secure_frontend_test::make_private_directory(models_path);
    secure_frontend_test::make_private_directory(scratch / "tokenizer");
    secure_frontend_test::make_private_directory(scratch / "bin");
    secure_frontend_test::write_file(scratch / "bin" / "engine", "#!/bin/sh\n", 0700);
    config = scratch / "config.json";
    write_config("");
  }

  // An absolute path that does not exist (local verbs never touch the downloader).
  std::string missing_downloader() const {
    std::string escaped;
    for (const char c : std::filesystem::absolute(scratch / "missing" / "curl").generic_string()) escaped += c == '\\' ? std::string{"\\\\"} : std::string(1, c);
    return "\"" + escaped + "\"";
  }

  // `extra_top_level` (SP7 M2 Task 3): raw JSON appended after the pull object, e.g.
  // ",\n  \"update\": {\"source\": [\"https://release.example/current\"]}". Must start with a comma.
  void write_config(const std::string& extra_members, const std::string& extra_top_level = "") {
    const auto quote = [](const std::filesystem::path& p) {
      // UTF-8, never path::string() (a non-ASCII root must survive on Windows too).
      std::string text = secure_frontend::path_to_utf8(std::filesystem::canonical(p));
      std::string out;
      for (const char c : text) out += c == '\\' ? std::string{"\\\\"} : std::string(1, c);
      return "\"" + out + "\"";
    };
    secure_frontend_test::write_file(config, "{\n  \"schema_version\": 1,\n  \"roots\": {\"models\": " + quote(models_path) +
                                                 ", \"tokenizer_pack\": " + quote(scratch / "tokenizer") + "},\n"
                                                 "  \"token\": {\"source\": \"env:STRATA_TEST_TOKEN\"},\n"
                                                 "  \"engine\": {\"executable\": " + quote(scratch / "bin" / "engine") +
                                                 ", \"model_id\": \"m\"},\n"
                                                 "  \"pull\": {\"downloader\": " + missing_downloader() + extra_members + "}" + extra_top_level + "\n}\n");
  }

  // A managed config (SP4 §4.9 c/d): roots.tokenizer_pack removed, engine.model_id = model_ref,
  // `engine_extra` appended inside the engine object (e.g. `, "profile": "qwen"`).
  void make_managed(const std::string& model_ref, const std::string& engine_extra = "") {
    std::string text = secure_frontend_test::read_file(config);
    const auto start = text.find(", \"tokenizer_pack\"");
    text.erase(start, text.find('}', start) - start);
    const std::string from = "\"model_id\": \"m\"";
    text.replace(text.find(from), from.size(), "\"model_id\": \"" + model_ref + "\"" + engine_extra);
    secure_frontend_test::write_file(config, text);
  }

  secure_frontend::ValidatedConfig validated_config() const {
    auto draft = secure_frontend::read_config_file(config);
    return std::get<secure_frontend::ValidatedConfig>(
        secure_frontend::validate(std::get<secure_frontend::FrontendConfigDraft>(draft)));
  }

  // strata --config <config> <args...>
  int run(std::vector<std::string> args, const std::string& input = "", const bool interactive = false) {
    std::vector<std::string> full{"--config", config.string()};
    full.insert(full.end(), args.begin(), args.end());
    return run_raw(full, input, interactive);
  }

  // strata <args...> (no --config added)
  int run_raw(const std::vector<std::string>& args, const std::string& input = "", const bool interactive = false) {
    std::istringstream in(input);
    std::ostringstream o;
    std::ostringstream e;
    secure_frontend::CliStreams streams{in, o, e, interactive};
    const int code = secure_frontend::run_strata(args, streams, services);
    out = o.str();
    err = e.str();
    return code;
  }

  secure_frontend::CanonicalRoot models() const { return secure_frontend_test::acquire::models_root(models_path); }

  std::filesystem::path scratch;
  std::filesystem::path models_path;
  std::filesystem::path config;
  secure_frontend::CliServices services;
  std::string out;
  std::string err;
};

// Publishes a fixture model <name>:<tag> (sequence `sequence`) straight into the harness's roots.models.
// `vision` (SP5 M1 Task 11) installs the model that reads images: an mmproj artifact + launch.mmproj.
inline secure_frontend::IndexEntry install(CliHarness& h, const std::uint32_t sequence = 1, const std::string_view salt = "",
                                           const secure_frontend::ModelOrigin origin = secure_frontend::ModelOrigin::third_party,
                                           const std::string_view name = "m", const std::string_view tag = "t",
                                           const bool vision = false) {
  namespace fx = secure_frontend_test::acquire;
  const auto root = h.models();
  const auto payload = vision ? fx::qwen_payload_vision(salt) : fx::qwen_payload(salt);
  const auto manifest =
      fx::trusted(fx::manifest_text(name, tag, sequence, payload, true, true, "fixture.example",
                                    vision ? "mmproj/mmproj.gguf" : std::string_view{}),
                  origin);
  fx::stage(root, manifest, payload);
  fx::FakeLoaded nobody;
  const secure_frontend::StoreEnvironment env;
  return std::get<secure_frontend::PublishResult>(
             secure_frontend::publish_staged(root, secure_frontend::staging_key(manifest.parsed.sha256), manifest, nobody, env, true))
      .entry;
}

}  // namespace secure_frontend_test::cli
