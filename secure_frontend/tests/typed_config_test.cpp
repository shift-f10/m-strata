#include "secure_frontend/config_file.hpp"
#include "secure_frontend/engine_arguments.hpp"
#include "secure_frontend/frontend_config.hpp"
#include "secure_frontend/launch_composition.hpp"
#include "secure_frontend/parity_table.hpp"
#include "secure_frontend/secret_bytes.hpp"
#include "secure_frontend/secret_source.hpp"
#include "secure_frontend/token_reader.hpp"
#include "secure_frontend/vision_supervisor.hpp"
#include "support/cli_harness.hpp"
#include "support/config_json.hpp"
#include "support/process_runner.hpp"
#include "support/test_support.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace {
int failures = 0;
void expect(const bool condition, const std::string& message) {
  if (!condition) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
}
using secure_frontend::ConfigIssueCode;
using secure_frontend::ConfigValueType;
using secure_frontend_test::ConfigFixture;
using secure_frontend_test::render_config_json;

secure_frontend::ConfigValidationResult validate_text(const std::string& text, const std::filesystem::path& dir) {
  auto draft = secure_frontend::parse_config_text(text, dir);
  if (auto* errors = std::get_if<secure_frontend::ConfigErrors>(&draft)) return *errors;
  return secure_frontend::validate(std::get<secure_frontend::FrontendConfigDraft>(draft));
}

bool has_issue(const secure_frontend::ConfigValidationResult& result, const std::string& key, const ConfigIssueCode code) {
  const auto* errors = std::get_if<secure_frontend::ConfigErrors>(&result);
  return errors != nullptr && std::any_of(errors->begin(), errors->end(), [&](const auto& issue) {
           return issue.key_path == key && issue.code == code;
         });
}

// Pre-flight F-4 again, for path rows that load never touches: a POSIX
// absolute path is drive-relative on Windows (the config refuses it there), so
// those cases spell it with a drive.
std::string host_absolute(const std::string& posix) {
#if defined(_WIN32)
  return "C:" + posix;
#else
  return posix;
#endif
}
std::string quoted(const std::string& text) { return "\"" + text + "\""; }

std::string issues_of(const secure_frontend::ConfigValidationResult& result) {
  std::string text;
  if (const auto* errors = std::get_if<secure_frontend::ConfigErrors>(&result)) {
    for (const auto& issue : *errors) text += secure_frontend::format_issue(issue) + "; ";
  }
  return text;
}

std::string describe_of(const secure_frontend::ConfigValidationResult& result) {
  const auto* validated = std::get_if<secure_frontend::ValidatedConfig>(&result);
  if (validated == nullptr) return {};
  return secure_frontend::describe(validated->get());
}

std::string valid_sample(const secure_frontend::ConfigKey& key) {
  switch (key.type) {
    case ConfigValueType::integer: return std::to_string(key.minimum);
    case ConfigValueType::decimal: {
      // The midpoint is inside every decimal bound pair this schema uses,
      // including the pin's open bounds.
      const double mid = (key.minimum_decimal + key.maximum_decimal) / 2.0;
      char buffer[32];
      const auto [end, ec] = std::to_chars(buffer, buffer + sizeof(buffer), mid);
      return ec == std::errc{} ? std::string{buffer, end} : std::string{"0.5"};
    }
    case ConfigValueType::boolean: return "false";
    case ConfigValueType::string_array: return "[]";
    case ConfigValueType::string: return key.default_string.empty() ? "\"x\"" : "\"" + std::string{key.default_string} + "\"";
  }
  return "null";
}

std::vector<std::string> wrong_type_samples(const ConfigValueType type) {
  switch (type) {
    case ConfigValueType::integer: return {"\"8080\"", "true", "[1]", "null"};
    case ConfigValueType::decimal: return {"\"1.5\"", "true", "[1]", "null"};
    case ConfigValueType::boolean: return {"1", "\"true\"", "null"};
    case ConfigValueType::string: return {"1", "true", "[\"x\"]", "{}"};
    case ConfigValueType::string_array: return {"\"x\"", "[1]", "{}", "true"};
  }
  return {};
}

void typed_config_accepts_minimal_file_and_applies_defaults() {
  ConfigFixture f;
  const auto result = validate_text(f.document(), f.dir);
  const auto* validated = std::get_if<secure_frontend::ValidatedConfig>(&result);
  expect(validated != nullptr, "the minimal config must validate: " + issues_of(result));
  if (validated == nullptr) return;
  const auto& c = validated->get();
  expect(c.listener.host.canonical_text() == "127.0.0.1" && c.listener.port == 8080U && !c.listener.allow_remote &&
             !c.listener.allow_wildcard_bind && c.listener.backlog == 16U, "listener defaults");
  expect(c.limits.http.request_deadline == std::chrono::milliseconds(30000) && c.limits.http.max_request_bytes == 268435456U &&
             c.limits.http.max_header_bytes == 16384U && c.limits.http.max_headers == 64U &&
             c.limits.http.max_request_line_bytes == 4096U, "http limit defaults");
  // upstream's scale since 2026-10-29 (owner: a follow-up to a long answer was refused at the old 48 KiB body)
  expect(c.limits.chat.max_body_bytes == 268419072U && c.limits.chat.max_messages == 65536U &&
             c.limits.chat.max_completion_tokens == 1048576U && c.limits.chat.default_max_tokens == 0U, "chat defaults");
  expect(c.limits.engine.ready_timeout == std::chrono::seconds(900), "ready_timeout defaults to 900 s");
  expect(c.limits.concurrency.max_concurrent_requests == 8U, "eight workers (SP3 B11)");
  // owner 2026-10-27 ("it does not seem to be doing the multi threaded downloading"): upstream's downloader's 16
  // connections (upstream/setup.py DL_CONNECTIONS), which this one mirrors, unless a config sets pull.connections
  expect(c.pull.connections == 16U, "a large download uses upstream's 16 connections by default");
  expect(c.token.kind() == secure_frontend::SecretSourceKind::protected_file && c.token.location() == "token",
         "token.source defaults to file:token");
  expect(!c.engine.profile.has_value() && c.engine.model_id.value() == "qwen-local",
         "engine.profile has no default: a managed model's manifest sets it (SP4 §4.9 c)");
}

void cfg01_unknown_key_rejected() {
  ConfigFixture f;
  std::vector<std::string> unknown{"bogus", "listener.bogus", "limits.bogus", "limits.http.bogus", "limits.chat.bogus",
                                   "limits.engine.bogus", "limits.concurrency.bogus", "roots.bogus", "token.bogus",
                                   "engine.bogus"};
  for (const auto& key : secure_frontend::config_schema()) unknown.push_back(std::string{key.path} + "_typo");
  for (const auto& path : unknown) {
    auto literals = f.literals;
    literals[path] = "1";
    const auto result = validate_text(render_config_json(literals), f.dir);
    // SP5 M1 Task 10 (Ruling T10-b): `replicas` is BOTH our typed section (replicas.parallel)
    // and a rejected pinned name (the N-child scheduler dict, serve/replicas.py:44). An object
    // under it that uses none of the typed children IS the pinned dict — it still fails at load
    // by name, with the C-4 rule (excluded_key), not the generic unknown-key code.
    if (path.rfind("replicas.", 0) == 0) {
      expect(has_issue(result, "replicas", ConfigIssueCode::excluded_key),
             "a typo under the shared name replicas fails with its narrowing rule: " + path);
      continue;
    }
    expect(has_issue(result, path, ConfigIssueCode::unknown_key), "unknown key must be rejected by name: " + path);
  }
}

void cfg01_duplicate_key_rejected() {
  ConfigFixture f;
  for (const auto& key : secure_frontend::config_schema()) {
    auto literals = f.literals;
    const std::string path{key.path};
    if (!literals.contains(path)) literals[path] = valid_sample(key);
    const auto result = validate_text(render_config_json(literals, path), f.dir);
    expect(has_issue(result, "config file", ConfigIssueCode::duplicate_key), "duplicate must be rejected: " + path);
  }
  // A member name containing '.' never aliases a nested key: the nested form
  // plus the flat dotted spelling of the same key must not be "last wins".
  const auto with_extra_member = [](std::string document, const std::string& member) {
    document.insert(document.size() - 1U, "," + member);
    return document;
  };
  for (const auto& key : secure_frontend::config_schema()) {
    auto literals = f.literals;
    const std::string path{key.path};
    if (!literals.contains(path)) literals[path] = valid_sample(key);
    const auto text = with_extra_member(render_config_json(literals),
                                        secure_frontend_test::json_literal(path) + ":" + valid_sample(key));
    const auto result = validate_text(text, f.dir);
    const bool rejected = path.find('.') == std::string::npos
                              ? has_issue(result, "config file", ConfigIssueCode::duplicate_key)
                              : has_issue(result, path, ConfigIssueCode::unknown_key);
    expect(rejected && std::holds_alternative<secure_frontend::ConfigErrors>(result),
           "nested plus dotted spelling must be rejected: " + path);
  }
  // A flat dotted top-level key alone is not a nested key either.
  {
    auto literals = f.literals;
    literals.erase("engine.model_id");
    const auto result = validate_text(with_extra_member(render_config_json(literals), "\"engine.model_id\":\"qwen-local\""), f.dir);
    expect(has_issue(result, "engine.model_id", ConfigIssueCode::unknown_key), "flat dotted top-level key rejected");
  }
  {
    const auto result = validate_text(with_extra_member(f.document(), "\"listener.port\":9000"), f.dir);
    expect(has_issue(result, "listener.port", ConfigIssueCode::unknown_key), "flat dotted listener.port rejected");
  }
  // Dotted section names and empty names, at any depth.
  for (const auto& [member, issue_path] :
       std::vector<std::pair<std::string, std::string>>{
           {"\"limits\":{\"http\":{\"max_headers\":10}},\"limits.http\":{\"max_headers\":20}", "limits.http"},
           {"\"limits\":{\"http.max_headers\":20}", "limits.http.max_headers"},
           {"\"\":1", ""},
           {"\"listener\":{\"\":1}", "listener."}}) {
    const auto result = validate_text(with_extra_member(f.document(), member), f.dir);
    expect(has_issue(result, issue_path, ConfigIssueCode::unknown_key), "dotted or empty member name rejected: " + member);
  }
}

void cfg01_type_confusion_rejected() {
  ConfigFixture f;
  for (const auto& key : secure_frontend::config_schema()) {
    const std::string path{key.path};
    for (const auto& sample : wrong_type_samples(key.type)) {
      auto literals = f.literals;
      literals[path] = sample;
      const auto result = validate_text(render_config_json(literals), f.dir);
      expect(has_issue(result, path, ConfigIssueCode::wrong_type), "type confusion " + sample + " for " + path);
    }
    if (key.type == ConfigValueType::integer) {
      for (const char* number : {"8080.0", "1e3"}) {
        auto literals = f.literals;
        literals[path] = number;
        const auto result = validate_text(render_config_json(literals), f.dir);
        expect(has_issue(result, "config file", ConfigIssueCode::malformed_document),
               std::string{"a float is never coerced: "} + number + " for " + path);
      }
    }
  }
}

void cfg01_missing_required_rejected() {
  ConfigFixture f;
  std::vector<std::string> required{"schema_version"};
  for (const auto& key : secure_frontend::config_schema()) if (key.required) required.emplace_back(key.path);
  expect(required.size() == 4U, "schema_version, roots.models, engine.executable, engine.model_id (roots.tokenizer_pack is --expert only, SP4 §4.9 d)");
  for (const auto& path : required) {
    auto literals = f.literals;
    literals.erase(path);
    const auto result = validate_text(render_config_json(literals), f.dir);
    expect(has_issue(result, path, ConfigIssueCode::missing_required), "missing required key: " + path);
  }
}

void typed_config_model_reference_accepts_name_tag() {
  ConfigFixture f;
  auto literals = f.literals;
  literals["engine.model_id"] = secure_frontend_test::json_literal("qwen3.8-flash-next:iq2_xs");
  literals.erase("roots.tokenizer_pack");
  const auto result = validate_text(render_config_json(literals), f.dir);
  expect(std::holds_alternative<secure_frontend::ValidatedConfig>(result), "name:tag model reference, no tokenizer root");
  literals["engine.model_id"] = secure_frontend_test::json_literal("a:b:c");
  expect(has_issue(validate_text(render_config_json(literals), f.dir), "engine.model_id", ConfigIssueCode::invalid_value),
         "at most one ':'");
}

void cfg01_out_of_range_rejected() {
  ConfigFixture f;
  for (const auto& key : secure_frontend::config_schema()) {
    if (key.type != ConfigValueType::integer) continue;
    for (const auto value : {key.minimum - 1, key.maximum + 1}) {
      auto literals = f.literals;
      literals[std::string{key.path}] = std::to_string(value);
      const auto result = validate_text(render_config_json(literals), f.dir);
      expect(has_issue(result, std::string{key.path}, ConfigIssueCode::out_of_range),
             "out of range " + std::to_string(value) + " for " + std::string{key.path});
    }
  }
}

// Spec §9.5: a value the schema accepts must be one the server can honour. Session log
// step 13 (owner: Strata runs contexts up to 1M with YaRN) raised the enforced caps to
// upstream's scale: the prompt renderers take 65,536 messages and 256 MiB a message
// (src/qwen_prompt_renderer.cpp, src/glm_prompt_renderer.cpp); the engine protocol takes
// 1,048,576 prompt, new and generated tokens (EngineProtocolLimits); a request may be
// 256 MiB (upstream's body limit). The defaults are unchanged.
void cfg01_chat_ranges_match_enforced_caps() {
  ConfigFixture f;
  const auto with = [&](const std::string& key, const std::string& value) {
    auto literals = f.literals;
    literals[key] = value;
    if (key == "limits.chat.max_body_bytes") literals["limits.http.max_request_bytes"] = "268435456";
    if (key == "limits.chat.default_max_tokens") literals["limits.chat.max_completion_tokens"] = "1048576";
    return validate_text(render_config_json(literals), f.dir);
  };
  for (const auto& [key, cap] : std::vector<std::pair<std::string, std::int64_t>>{
           {"limits.chat.max_messages", 65'536},
           {"limits.chat.max_message_bytes", 268'435'456},
           {"limits.chat.max_prompt_tokens", 1'048'576},
           {"limits.chat.max_completion_tokens", 1'048'576},
           {"limits.chat.default_max_tokens", 1'048'576},
           {"limits.http.max_request_bytes", 268'435'456}}) {
    expect(std::holds_alternative<secure_frontend::ValidatedConfig>(with(key, std::to_string(cap))),
           "the enforced cap is accepted: " + key);
    expect(has_issue(with(key, std::to_string(cap + 1)), key, ConfigIssueCode::out_of_range),
           "one above the enforced cap is rejected: " + key);
  }
  // The chat body stays below the request less its header (the cross-field rule).
  expect(std::holds_alternative<secure_frontend::ValidatedConfig>(with("limits.chat.max_body_bytes", "268419072")),
         "a 256 MiB request takes a body of 256 MiB less the 16 KiB header");
  expect(has_issue(with("limits.chat.max_body_bytes", "268435457"), "limits.chat.max_body_bytes", ConfigIssueCode::out_of_range),
         "one above 256 MiB is out of range for the body too");
}

void typed_config_cross_field_rules() {
  ConfigFixture f;
  const auto with = [&](std::map<std::string, std::string> extra) {
    auto literals = f.literals;
    for (auto& [k, v] : extra) literals[k] = v;
    return validate_text(render_config_json(literals), f.dir);
  };
  expect(has_issue(with({{"limits.http.max_request_bytes", "8192"}, {"limits.http.max_header_bytes", "16384"}}),
                   "limits.http.max_header_bytes", ConfigIssueCode::conflicting_values), "header > request");
  expect(has_issue(with({{"limits.http.max_header_bytes", "2048"}, {"limits.http.max_request_line_bytes", "4096"}}),
                   "limits.http.max_request_line_bytes", ConfigIssueCode::conflicting_values), "line > header");
  expect(has_issue(with({{"limits.http.max_request_bytes", "65536"}, {"limits.chat.max_body_bytes", "262144"}}),
                   "limits.chat.max_body_bytes", ConfigIssueCode::conflicting_values), "body > request - header");
  expect(has_issue(with({{"limits.chat.max_completion_tokens", "4096"}, {"limits.chat.default_max_tokens", "5000"}}),
                   "limits.chat.default_max_tokens", ConfigIssueCode::conflicting_values), "default_max_tokens > max_completion_tokens");
  expect(has_issue(with({{"engine.profile", "\"glm53\""}, {"engine.args", "[\"--native\",\"x\"]"}}), "engine.args",
                   ConfigIssueCode::invalid_value),
         "glm53 refuses engine.args outright (SP5 M2 Task 7 D-8: the launch is composed; the SP1 "
         "\"at least one argument\" rule is retired — a bare glm53 config is covered by "
         "glm_max_context_typed_and_glm53_args_refused)");
  expect(has_issue(with({{"engine.args", "[\"--native\",\"x\"]"}}), "engine.args", ConfigIssueCode::invalid_value),
         "the SP0 deny-list applies to engine.args");
  expect(has_issue(with({{"listener.host", "\"192.0.2.10\""}}), "listener.host", ConfigIssueCode::remote_not_enabled),
         "remote needs listener.allow_remote");
  expect(has_issue(with({{"listener.host", "\"::0\""}, {"listener.allow_remote", "true"}}), "listener.host",
                   ConfigIssueCode::wildcard_bind_not_enabled), "wildcard needs its own opt-in");
  expect(has_issue(with({{"token.source", "\"cmd:echo\""}}), "token.source", ConfigIssueCode::invalid_value),
         "token.source is env:NAME or file:PATH");
  expect(has_issue(with({{"roots.models", "\"relative/models\""}}), "roots.models", ConfigIssueCode::invalid_value),
         "roots must be absolute");
}

void typed_config_errors_never_contain_values() {
  ConfigFixture f;
  auto literals = f.literals;
  const std::string secret_like = "sk-PASTED-SECRET-0123456789";
  literals["listener.host"] = secure_frontend_test::json_literal(secret_like);
  literals["token.source"] = secure_frontend_test::json_literal("env:" + secret_like + "!");
  literals["engine.model_id"] = secure_frontend_test::json_literal(secret_like + "/x");
  const auto text = issues_of(validate_text(render_config_json(literals), f.dir));
  expect(!text.empty() && text.find(secret_like) == std::string::npos, "issues name keys, never values: " + text);
}

#if !defined(_WIN32)
// Spec §3.2/§4: roots.secret defaults to the config file's folder only for a
// file: token. An env: token reads nothing below a secret root, so a loose
// config folder must not fail validation.
void cfg02_env_token_ignores_config_folder_permissions() {
  ConfigFixture f;
  auto literals = f.literals;
  literals.erase("roots.secret");
  literals["token.source"] = secure_frontend_test::json_literal("env:SF_UNUSED_TOKEN");
  chmod(f.dir.c_str(), 0775);
  const auto result = validate_text(render_config_json(literals), f.dir);
  expect(std::holds_alternative<secure_frontend::ValidatedConfig>(result),
         "env token with a 0775 config folder is valid: " + issues_of(result));
  if (const auto* config = std::get_if<secure_frontend::ValidatedConfig>(&result)) {
    expect(!config->get().roots.secret.has_value(), "no secret root is opened for an env token");
  }
}

void cfg02_file_token_default_secret_root_reports_one_issue() {
  ConfigFixture f;
  auto literals = f.literals;
  literals.erase("roots.secret");
  literals["token.source"] = secure_frontend_test::json_literal("file:token");
  chmod(f.dir.c_str(), 0775);
  const auto result = validate_text(render_config_json(literals), f.dir);
  const auto* errors = std::get_if<secure_frontend::ConfigErrors>(&result);
  expect(errors != nullptr, "file token with a 0775 config folder is rejected");
  if (errors == nullptr) return;
  const auto count = std::count_if(errors->begin(), errors->end(),
                                   [](const auto& issue) { return issue.key_path == "roots.secret"; });
  expect(count == 1, "exactly one roots.secret issue: " + issues_of(result));
  expect(has_issue(result, "roots.secret", ConfigIssueCode::invalid_value), "the issue is invalid_value");
  const std::string text = issues_of(result);
  expect(text.find("roots.secret: is unset, so it defaults to the config file's folder, which is not usable: ") !=
             std::string::npos,
         "the reason reads as one sentence: " + text);
  expect(text.find(f.dir.string()) == std::string::npos, "the reason never contains the path: " + text);
  chmod(f.dir.c_str(), 0700);
  const auto ok = validate_text(render_config_json(literals), f.dir);
  const auto* valid = std::get_if<secure_frontend::ValidatedConfig>(&ok);
  expect(valid != nullptr && valid->get().roots.secret.has_value(),
         "a private config folder is the file token's secret root: " + issues_of(ok));
}
#endif

bool untrusted(const std::variant<secure_frontend::FrontendConfigDraft, secure_frontend::ConfigErrors>& result) {
  const auto* errors = std::get_if<secure_frontend::ConfigErrors>(&result);
  return errors != nullptr && !errors->empty() && errors->front().key_path == "config file" &&
         errors->front().code == secure_frontend::ConfigIssueCode::file_untrusted;
}

#if !defined(_WIN32)
void cfg01_group_writable_config_rejected() {
  ConfigFixture f;
  const auto file = f.dir / "config.json";
  secure_frontend_test::write_file(file, f.document(), 0664);
  expect(untrusted(secure_frontend::read_config_file(file)), "a group-writable config file is rejected");
  chmod(file.c_str(), 0644);
  expect(std::holds_alternative<secure_frontend::FrontendConfigDraft>(secure_frontend::read_config_file(file)),
         "0644 is fine: the config is not secret");
  const auto shared = secure_frontend_test::make_scratch_directory();
  secure_frontend_test::write_file(shared / "config.json", f.document(), 0600);
  chmod(shared.c_str(), 0777);
  expect(untrusted(secure_frontend::read_config_file(shared / "config.json")),
         "a config in a directory others can write is rejected (they could swap it)");
  std::filesystem::create_symlink(file, f.dir / "linked.json");
  expect(untrusted(secure_frontend::read_config_file(f.dir / "linked.json")), "a symlinked config file is rejected");
}
#else
void cfg01_config_with_everyone_write_ace_rejected() {
  ConfigFixture f;
  const auto file = f.dir / "config.json";
  secure_frontend_test::write_file(file, f.document());
  expect(std::holds_alternative<secure_frontend::FrontendConfigDraft>(secure_frontend::read_config_file(file)),
         "a config with the inherited user ACL is accepted");
  expect(secure_frontend_test::grant_everyone_write(file), "test setup: add an Everyone write ACE");
  expect(untrusted(secure_frontend::read_config_file(file)), "a config Everyone can write is rejected");
}
#endif

// engine.lib_dirs: each entry an absolute directory without the platform's
// list separator (':' POSIX, ';' Windows), which would split LD_LIBRARY_PATH /
// PATH into extra, unvalidated directories.
void engine_lib_dirs_must_be_absolute_directories() {
  ConfigFixture f;
#if defined(_WIN32)
  const std::string separator = ";";
#else
  const std::string separator = ":";
#endif
  const auto with_lib_dirs = [&f](const std::vector<std::string>& entries) {
    auto literals = f.literals;
    std::string array = "[";
    for (const auto& entry : entries) array += (array.size() > 1U ? "," : "") + secure_frontend_test::json_literal(entry);
    literals["engine.lib_dirs"] = array + "]";
    return validate_text(render_config_json(literals), f.dir);
  };
  const std::string good = (f.dir / "lib").string();
  const auto accepted = with_lib_dirs({good, (f.dir / "lib2").string()});
  const auto* validated = std::get_if<secure_frontend::ValidatedConfig>(&accepted);
  expect(validated != nullptr, "absolute lib dirs validate: " + issues_of(accepted));
  if (validated != nullptr) {
    expect(validated->get().engine.lib_dirs ==
               std::vector<std::filesystem::path>{f.dir / "lib", f.dir / "lib2"}, "lib dirs pass through in order");
  }
  for (const auto& bad : std::vector<std::string>{"lib", "", good + separator + (f.dir / "other").string()}) {
    expect(has_issue(with_lib_dirs({good, bad}), "engine.lib_dirs", ConfigIssueCode::invalid_value),
           "rejected lib dir entry of " + std::to_string(bad.size()) + " bytes");
  }
}

void cfg02_logs_and_cache_roots_are_created_private() {
  ConfigFixture f;
  auto literals = f.literals;
  literals["roots.logs"] = secure_frontend_test::json_literal((f.dir / "logs").string());
  literals["roots.cache"] = secure_frontend_test::json_literal((f.dir / "cache").string());
  const auto result = validate_text(render_config_json(literals), f.dir);
  const auto* validated = std::get_if<secure_frontend::ValidatedConfig>(&result);
  expect(validated != nullptr, "absent logs/cache roots under a private parent validate: " + issues_of(result));
  expect(!std::filesystem::exists(f.dir / "logs") && !std::filesystem::exists(f.dir / "cache"),
         "validate() creates nothing (--check has no side effects)");
  if (validated == nullptr) return;
  expect(secure_frontend::create_runtime_roots(validated->get()).empty(), "serve/init create the missing roots");
  expect(std::filesystem::is_directory(f.dir / "logs") && std::filesystem::is_directory(f.dir / "cache"), "both exist");
#if !defined(_WIN32)
  struct stat st {};
  stat((f.dir / "logs").c_str(), &st);
  expect((st.st_mode & 0777) == 0700, "created 0700");
#endif
  expect(secure_frontend::create_runtime_roots(validated->get()).empty(), "existing private roots are accepted again");
#if !defined(_WIN32)
  const auto shared = f.dir / "shared";
  secure_frontend_test::make_private_directory(shared);
  chmod(shared.c_str(), 0777);
  literals["roots.logs"] = secure_frontend_test::json_literal((shared / "logs").string());
  expect(has_issue(validate_text(render_config_json(literals), f.dir), "roots.logs", ConfigIssueCode::invalid_value),
         "an absent root whose parent others can write is rejected at validation");
  expect(!std::filesystem::exists(shared / "logs"), "and nothing was created");
#endif
}

template <typename T>
constexpr bool is_secret_v = std::is_same_v<std::remove_cv_t<T>, secure_frontend::SecretBytes>;

// CFG-06 at compile time: the long-lived configuration cannot hold a token.
static_assert(!is_secret_v<decltype(secure_frontend::FrontendConfig::listener)>);
static_assert(!is_secret_v<decltype(secure_frontend::FrontendConfig::limits)>);
static_assert(!is_secret_v<decltype(secure_frontend::FrontendConfig::roots)>);
static_assert(!is_secret_v<decltype(secure_frontend::FrontendConfig::token)>);
static_assert(!is_secret_v<decltype(secure_frontend::FrontendConfig::engine)>);
static_assert(std::is_same_v<std::remove_cv_t<decltype(secure_frontend::FrontendConfig::token)>, secure_frontend::SecretSource>,
              "the token member is source metadata (kind + env name or relative path)");
static_assert(!std::is_constructible_v<secure_frontend::SecretSource, secure_frontend::SecretBytes>,
              "a secret cannot become a SecretSource");
static_assert(!std::is_copy_constructible_v<secure_frontend::SecretBytes>,
              "a secret cannot be copied into any config value");

void cfg06_config_has_no_secret_typed_member() {
  // The proof is the static_asserts above (this file would not compile otherwise).
  // At run time, re-check the same traits so the case records real values.
  using secure_frontend::FrontendConfig;
  expect(std::is_same_v<std::remove_cv_t<decltype(FrontendConfig::token)>, secure_frontend::SecretSource>,
         "FrontendConfig::token is a SecretSource (metadata)");
  expect(!std::is_constructible_v<secure_frontend::SecretSource, secure_frontend::SecretBytes> &&
             !std::is_copy_constructible_v<secure_frontend::SecretBytes>,
         "a secret can neither become a SecretSource nor be copied");
}

void cfg06_validated_config_bytes_do_not_contain_token() {
  ConfigFixture f;
  const std::string token = "cfg06-token-0123456789abcdefghijklmnopqrstu";
#if defined(_WIN32)
  _putenv_s("SF_CFG06_TOKEN", token.c_str());
#else
  setenv("SF_CFG06_TOKEN", token.c_str(), 1);
#endif
  // Read the token through the production reader first, so its bytes are live
  // in this process while the config is validated and described: a config that
  // captured the resolved token would make the scans below fail.
  const auto source = secure_frontend::make_environment_secret_source("SF_CFG06_TOKEN");
  expect(std::holds_alternative<secure_frontend::SecretSource>(source), "test setup: env secret source");
  if (!std::holds_alternative<secure_frontend::SecretSource>(source)) return;
  auto read = secure_frontend::read_token(std::get<secure_frontend::SecretSource>(source), f.dir / "secret");
  const auto* secret = std::get_if<secure_frontend::SecretBytes>(&read);
  expect(secret != nullptr && secret->view() == token, "the token was read from its configured source");
  if (secret == nullptr) return;
  auto literals = f.literals;
  literals["token.source"] = secure_frontend_test::json_literal("env:SF_CFG06_TOKEN");
  const auto result = validate_text(render_config_json(literals), f.dir);
  const auto* validated = std::get_if<secure_frontend::ValidatedConfig>(&result);
  expect(validated != nullptr, "config validates: " + issues_of(result));
  if (validated == nullptr) return;
  const std::string described = secure_frontend::describe(validated->get());
  expect(described.find("env:SF_CFG06_TOKEN") != std::string::npos, "the source is described as metadata");
  expect(described.find(secret->view()) == std::string::npos, "the described object graph never contains the token");
  const std::string_view bytes{reinterpret_cast<const char*>(&validated->get()), sizeof(secure_frontend::FrontendConfig)};
  expect(bytes.find(secret->view()) == std::string_view::npos, "the object's own bytes never contain the token");
}

// D3: the README key table is generated from config_schema(); a schema change
// that is not copied into the README fails here and prints the new table.
void readme_config_table_matches_schema() {
  const auto& args = secure_frontend_test::extra_arguments();
  expect(!args.empty(), "the README path is passed as ARGS");
  if (args.empty()) return;
  std::string readme = secure_frontend_test::read_file(args.front());
  // A Windows checkout with core.autocrlf=true (GitHub's windows-latest) has CRLF.
  readme.erase(std::remove(readme.begin(), readme.end(), '\r'), readme.end());
  const std::string begin = "<!-- config-keys:begin -->\n";
  const std::string end = "<!-- config-keys:end -->";
  const auto start = readme.find(begin);
  const auto stop = readme.find(end);
  expect(start != std::string::npos && stop != std::string::npos && start < stop, "README has the config-keys markers");
  if (start == std::string::npos || stop == std::string::npos || start > stop) return;
  const std::string table = readme.substr(start + begin.size(), stop - start - begin.size());
  const std::string expected = secure_frontend::render_config_schema_markdown();
  expect(table == expected, "README key table is stale; replace it with:\n" + expected);
}
// A minimal valid draft over scratch roots; validate() opens them.
secure_frontend::FrontendConfigDraft pull_minimal_draft(const std::filesystem::path& scratch) {
  using secure_frontend::ConfigDraftEntry;
  using secure_frontend::ConfigSource;
  secure_frontend_test::make_private_directory(scratch / "models");
  secure_frontend_test::make_private_directory(scratch / "tokenizer");
  secure_frontend_test::make_private_directory(scratch / "bin");
  secure_frontend_test::write_file(scratch / "bin" / "engine", "#!/bin/sh\n", 0700);
  secure_frontend::FrontendConfigDraft draft;
  draft.entries["roots.models"] = ConfigDraftEntry{(scratch / "models").string(), ConfigSource::file};
  draft.entries["roots.tokenizer_pack"] = ConfigDraftEntry{(scratch / "tokenizer").string(), ConfigSource::file};
  draft.entries["engine.executable"] =
      ConfigDraftEntry{std::filesystem::canonical(scratch / "bin" / "engine").string(), ConfigSource::file};
  draft.entries["engine.model_id"] = ConfigDraftEntry{std::string{"m"}, ConfigSource::file};
  draft.entries["token.source"] = ConfigDraftEntry{std::string{"env:STRATA_TEST_TOKEN"}, ConfigSource::file};
  return draft;
}

bool has_issue_for_key(const secure_frontend::ConfigValidationResult& result, const std::string& key) {
  const auto* issues = std::get_if<secure_frontend::ConfigErrors>(&result);
  if (issues == nullptr) return false;
  for (const auto& issue : *issues) {
    if (issue.key_path == key) return true;
  }
  return false;
}

// Spec §4.9 delta e: every pull.* key, table-driven like CFG-01.
void cfg01_pull_keys_table_driven() {
  using Value = secure_frontend::ConfigDraftValue;
  struct Row final {
    std::string key;
    Value good;
    std::vector<Value> bad;
  };
  // Pre-flight F-4: the case runs on Windows too, so absolute paths come from a
  // scratch directory ("/opt/curl/bin/curl" is not absolute there).
  const auto elsewhere = secure_frontend_test::make_scratch_directory();
  const std::string good_downloader = (elsewhere / "opt" / "curl").string();
  const std::string missing_downloader = (elsewhere / "nonexistent" / "curl").string();
  const std::vector<Row> rows{
      {"pull.downloader", Value{good_downloader}, {Value{std::string{"curl"}}, Value{std::int64_t{1}}}},
      {"pull.proxy", Value{std::string{"http://proxy.corp:3128"}},
       {Value{std::string{"http://u:p@proxy.corp:3128"}}, Value{std::string{"socks5://p:1"}}, Value{std::string{"http://p"}}}},
      {"pull.allow_private_addresses", Value{true}, {Value{std::string{"yes"}}}},
      {"pull.max_total_bytes", Value{std::int64_t{1} << 20}, {Value{(std::int64_t{1} << 20) - 1}, Value{(std::int64_t{1} << 44) + 1}}},
      {"pull.max_duration_s", Value{std::int64_t{60}}, {Value{std::int64_t{59}}, Value{std::int64_t{604801}}}},
      {"pull.stall_timeout_s", Value{std::int64_t{3600}}, {Value{std::int64_t{9}}, Value{std::int64_t{3601}}}},
      {"pull.connect_timeout_s", Value{std::int64_t{1}}, {Value{std::int64_t{0}}, Value{std::int64_t{301}}}},
  };
  for (const auto& row : rows) {
    const auto scratch = secure_frontend_test::make_scratch_directory();
    auto draft = pull_minimal_draft(scratch);
    draft.entries[row.key] = secure_frontend::ConfigDraftEntry{row.good, secure_frontend::ConfigSource::file};
    const auto accepted = secure_frontend::validate(draft);
    expect(std::holds_alternative<secure_frontend::ValidatedConfig>(accepted),
           row.key + " accepts a good value: " + issues_of(accepted));
    for (const auto& bad : row.bad) {
      draft.entries[row.key] = secure_frontend::ConfigDraftEntry{bad, secure_frontend::ConfigSource::file};
      expect(has_issue_for_key(secure_frontend::validate(draft), row.key), row.key + " rejects a bad value");
    }
  }
  const auto scratch = secure_frontend_test::make_scratch_directory();
  auto defaults = pull_minimal_draft(scratch);
  defaults.entries["pull.downloader"] = secure_frontend::ConfigDraftEntry{missing_downloader, secure_frontend::ConfigSource::file};
  const auto validated = secure_frontend::validate(defaults);
  expect(std::holds_alternative<secure_frontend::ValidatedConfig>(validated),
         "pull.downloader is validated lazily: a missing file does not fail validation (CFG-01 exception)");
  if (const auto* config = std::get_if<secure_frontend::ValidatedConfig>(&validated)) {
    const auto& pull = config->get().pull;
    expect(pull.downloader == std::filesystem::path{missing_downloader}, "the configured downloader path is kept as written");
    expect(pull.max_total_bytes == (std::uint64_t{1} << 40U) && pull.max_duration.count() == 172800 &&
               pull.stall_timeout.count() == 120 && pull.connect_timeout.count() == 30 && !pull.allow_private_addresses &&
               !pull.proxy.has_value(),
           "spec §3.9 defaults");
    expect(secure_frontend::describe(config->get()).find("pull.max_total_bytes = 1099511627776\n") != std::string::npos,
           "--check lists the pull keys");
  }
}
// SP4 Task 24b (spec §4.9 c/d; F-8): the managed-model rejections are config errors that
// strata-unified-server --check reports too. ARGS: <strata-unified-server>.
secure_frontend_test::ProcessResult server_check(const secure_frontend_test::cli::CliHarness& h, const bool expert) {
  const auto& args = secure_frontend_test::extra_arguments();
  if (args.empty()) return {-2, {}, "needs ARGS <strata-unified-server>"};
  std::vector<std::string> argv{"--check", "--config", secure_frontend::path_to_utf8(h.config)};
  if (expert) argv.insert(argv.begin(), "--expert");
  return secure_frontend_test::run_to_completion(secure_frontend_test::ProcessSpec{
      args[0], argv, {{"STRATA_TEST_TOKEN", "check-token-0123456789abcdefghijklmnop"}}});
}

using secure_frontend_test::contains;

void cfg01_engine_profile_rejected_for_managed_model() {
  secure_frontend_test::cli::CliHarness h;
  (void)secure_frontend_test::cli::install(h);
  h.make_managed("m:t", ", \"profile\": \"qwen\"");
  const auto composed = secure_frontend::compose_engine_launch(h.validated_config().get(), false);
  const auto* failure = std::get_if<secure_frontend::LaunchFailure>(&composed);
  expect(failure != nullptr && failure->messages.front().find("engine.profile") != std::string::npos &&
             failure->messages.front().find("engine_profile_conflicts_with_manifest") != std::string::npos,
         "engine.profile is rejected for a managed model unless --expert");
  const auto checked = server_check(h, false);
  expect(checked.exit_code == 1 && contains(checked.err, "Configuration error: engine.profile") &&
             !contains(checked.out, "Configuration is valid."),
         "--check reports engine.profile as a config error: " + checked.err);
}

void cfg01_tokenizer_pack_root_rejected_for_managed_model() {
  secure_frontend_test::cli::CliHarness h;
  (void)secure_frontend_test::cli::install(h);
  std::string text = secure_frontend_test::read_file(h.config);
  text.replace(text.find("\"model_id\": \"m\""), 15, "\"model_id\": \"m:t\"");  // roots.tokenizer_pack stays set
  secure_frontend_test::write_file(h.config, text);
  const auto composed = secure_frontend::compose_engine_launch(h.validated_config().get(), false);
  const auto* failure = std::get_if<secure_frontend::LaunchFailure>(&composed);
  expect(failure != nullptr && failure->messages.front().find("roots.tokenizer_pack") != std::string::npos,
         "roots.tokenizer_pack together with a managed model is a config error (no silent ignore)");
  const auto checked = server_check(h, false);
  expect(checked.exit_code == 1 && contains(checked.err, "Configuration error: roots.tokenizer_pack") &&
             !contains(checked.out, "Configuration is valid."),
         "--check reports roots.tokenizer_pack as a config error: " + checked.err);
  const auto expert = server_check(h, true);
  expect(expert.exit_code == 0 && contains(expert.out, "Configuration is valid."),
         "--check --expert accepts the operator's tokenizer root: " + expert.err);
}

// make_model_id accepts "index.json"; the model index grammar never names it (two grammars, F-8).
void cfg01_model_reference_rejected_for_managed_model() {
  secure_frontend_test::cli::CliHarness h;
  h.make_managed("index.json");
  const auto issues = secure_frontend::managed_config_issues(h.validated_config().get());
  expect(issues.size() == 1U && issues.front().rfind("engine.model_id", 0) == 0, "a model id that is not a name[:tag] reference");
  const auto checked = server_check(h, false);
  expect(checked.exit_code == 1 && contains(checked.err, "Configuration error: engine.model_id"),
         "--check reports engine.model_id: " + checked.err);
  const auto expert = server_check(h, true);
  expect(expert.exit_code == 1 && contains(expert.err, "Configuration error: roots.tokenizer_pack") &&
             !contains(expert.err, "engine.model_id"),
         "--check --expert needs roots.tokenizer_pack and takes an inventory id: " + expert.err);
}
// SP4 §4.9 c/d (Task 24a addendum): --check prints "(unset)" for the --expert-only keys.
void describe_reports_unset_expert_keys() {
  ConfigFixture f;
  auto literals = f.literals;
  literals.erase("roots.tokenizer_pack");
  literals.erase("engine.profile");
  literals["engine.model_id"] = secure_frontend_test::json_literal("m:t");
  const auto result = validate_text(render_config_json(literals), f.dir);
  const auto* config = std::get_if<secure_frontend::ValidatedConfig>(&result);
  expect(config != nullptr, "a managed config validates: " + issues_of(result));
  if (config == nullptr) return;
  const auto text = secure_frontend::describe(config->get());
  expect(text.find("\nroots.tokenizer_pack = (unset)\n") != std::string::npos, "roots.tokenizer_pack = (unset): " + text);
  expect(text.find("\nengine.profile = (unset)\n") != std::string::npos, "engine.profile = (unset)");
  expect(text.find("\nengine.model_id = \"m:t\"\n") != std::string::npos, "engine.model_id as given");
}

// SP3 SP1-addendum A1 (spec §5.1, §4.3, §4.4): string-array rows, Host/CORS
// grammar, HTTP deadline keys and logging.requests.
struct Sp3Row { const char* path; ConfigValueType type; std::int64_t minimum; std::int64_t maximum; };
constexpr Sp3Row kSp3RowsA1[] = {
    {"listener.cors_origins", ConfigValueType::string_array, 0, 0},
    {"listener.host_aliases", ConfigValueType::string_array, 0, 0},
    {"limits.http.header_deadline_ms", ConfigValueType::integer, 1, 3'600'000},
    {"limits.http.write_stall_ms", ConfigValueType::integer, 100, 300'000},
    {"limits.http.response_write_deadline_ms", ConfigValueType::integer, 1'000, 3'600'000},
    {"limits.http.stream_keepalive_ms", ConfigValueType::integer, 1'000, 300'000},
    {"logging.requests", ConfigValueType::boolean, 0, 0},
};

std::string string_array(const std::vector<std::string>& values) {
  std::string out = "[";
  for (std::size_t i = 0; i < values.size(); ++i) out += (i ? "," : "") + secure_frontend_test::json_literal(values[i]);
  return out + "]";
}

secure_frontend::ConfigValidationResult with_literals(const ConfigFixture& f, const std::map<std::string, std::string>& extra) {
  auto literals = f.literals;
  for (const auto& [k, v] : extra) literals[k] = v;
  return validate_text(render_config_json(literals), f.dir);
}

void cfg01_sp3_rows_type_and_range() {
  ConfigFixture f;
  for (const auto& row : kSp3RowsA1) {
    const auto* key = secure_frontend::find_config_key(row.path);
    expect(key != nullptr && key->type == row.type, std::string{"row exists with its type: "} + row.path);
    if (key != nullptr && row.type == ConfigValueType::integer) {
      expect(key->minimum == row.minimum && key->maximum == row.maximum, std::string{"range of "} + row.path);
    }
  }
  const auto defaults = with_literals(f, {});
  const auto* validated = std::get_if<secure_frontend::ValidatedConfig>(&defaults);
  expect(validated != nullptr, "defaults validate: " + issues_of(defaults));
  if (validated != nullptr) {
    const auto& c = validated->get();
    expect(c.listener.cors_origins.empty() && c.listener.host_aliases.empty(), "array defaults are empty");
    expect(c.limits.http.header_deadline == std::chrono::milliseconds(10000) && c.limits.http.write_stall == std::chrono::milliseconds(10000) &&
           c.limits.http.response_write_deadline == std::chrono::milliseconds(60000) &&
           c.limits.http.stream_keepalive == std::chrono::milliseconds(15000), "HTTP deadline defaults");
    expect(!c.logging.requests, "request logging is off by default");
    const std::string text = secure_frontend::describe(c);
    for (const auto& row : kSp3RowsA1) expect(text.find(row.path) != std::string::npos, std::string{"--check prints "} + row.path);
  }
  std::vector<std::string> seventeen;
  for (int i = 0; i < 17; ++i) seventeen.push_back("h" + std::to_string(i) + ".lan:8080");
  expect(has_issue(with_literals(f, {{"listener.host_aliases", string_array(seventeen)}}), "listener.host_aliases",
                   ConfigIssueCode::out_of_range), "17 aliases are rejected");
  seventeen.pop_back();
  expect(!has_issue(with_literals(f, {{"listener.host_aliases", string_array(seventeen)}}), "listener.host_aliases",
                    ConfigIssueCode::out_of_range), "16 aliases are accepted");
  expect(has_issue(with_literals(f, {{"listener.cors_origins", "[1]"}}), "listener.cors_origins", ConfigIssueCode::wrong_type),
         "a non-string entry is a type error");
}

void cfg01_header_deadline_le_request_deadline() {
  ConfigFixture f;
  expect(has_issue(with_literals(f, {{"limits.http.header_deadline_ms", "30001"}}), "limits.http.header_deadline_ms",
                   ConfigIssueCode::conflicting_values), "header deadline above the request deadline is rejected");
  expect(std::holds_alternative<secure_frontend::ValidatedConfig>(with_literals(f, {{"limits.http.header_deadline_ms", "30000"}})),
         "equal deadlines are valid");
}

void cfg01_response_deadline_ge_write_stall() {
  ConfigFixture f;
  expect(has_issue(with_literals(f, {{"limits.http.write_stall_ms", "20000"}, {"limits.http.response_write_deadline_ms", "19999"}}),
                   "limits.http.response_write_deadline_ms", ConfigIssueCode::conflicting_values),
         "response deadline below the stall bound is rejected");
  expect(std::holds_alternative<secure_frontend::ValidatedConfig>(
             with_literals(f, {{"limits.http.write_stall_ms", "20000"}, {"limits.http.response_write_deadline_ms", "20000"}})),
         "equal values are valid");
}

void cfg01_wildcard_bind_requires_host_aliases() {
  ConfigFixture f;
  const std::map<std::string, std::string> wildcard{{"listener.host", "\"0.0.0.0\""}, {"listener.allow_remote", "true"},
                                                    {"listener.allow_wildcard_bind", "true"}};
  expect(has_issue(with_literals(f, wildcard), "listener.host_aliases", ConfigIssueCode::invalid_value),
         "a wildcard bind without aliases names listener.host_aliases");
  auto with_alias = wildcard;
  with_alias["listener.host_aliases"] = string_array({"box.lan:8080"});
  expect(std::holds_alternative<secure_frontend::ValidatedConfig>(with_literals(f, with_alias)), "one alias is enough");
}

void cfg01_host_alias_grammar_and_count() {
  ConfigFixture f;
  for (const std::string bad : {"http://x", "x:", "x:99999", "a b", "user@x", "x.:80", ""}) {
    expect(has_issue(with_literals(f, {{"listener.host_aliases", string_array({bad})}}), "listener.host_aliases",
                     ConfigIssueCode::invalid_value), "alias rejected: " + bad);
  }
  expect(has_issue(with_literals(f, {{"listener.host_aliases", string_array({"a.lan:1", "a.lan:1"})}}), "listener.host_aliases",
                   ConfigIssueCode::invalid_value), "duplicate aliases are rejected");
  const auto ok = with_literals(f, {{"listener.host_aliases", string_array({"Strata.LAN:8080", "[fe80::1]:8080", "box"})}});
  expect(std::holds_alternative<secure_frontend::ValidatedConfig>(ok), "valid aliases: " + issues_of(ok));
  const auto text = issues_of(with_literals(f, {{"listener.host_aliases", string_array({"secret-host-value:"})}}));
  expect(text.find("secret-host-value") == std::string::npos, "the issue never echoes the value");
}

void web05_cors_origin_config_rejects_wildcard_null_path_duplicate_and_17_entries() {
  ConfigFixture f;
  for (const std::string bad : {"*", "null", "http://app.example/", "http://app.example/path", "http://App.example",
                                "ftp://app.example", "app.example:3000", "http://app.example:3000?x"}) {
    expect(has_issue(with_literals(f, {{"listener.cors_origins", string_array({bad})}}), "listener.cors_origins",
                     ConfigIssueCode::invalid_value), "origin rejected: " + bad);
  }
  expect(has_issue(with_literals(f, {{"listener.cors_origins", string_array({"http://a.example", "http://a.example"})}}),
                   "listener.cors_origins", ConfigIssueCode::invalid_value), "duplicate origins are rejected");
  std::vector<std::string> seventeen;
  for (int i = 0; i < 17; ++i) seventeen.push_back("http://a" + std::to_string(i) + ".example");
  expect(has_issue(with_literals(f, {{"listener.cors_origins", string_array(seventeen)}}), "listener.cors_origins",
                   ConfigIssueCode::out_of_range), "17 origins are rejected");
  const auto ok = with_literals(f, {{"listener.cors_origins", string_array({"http://app.example:3000", "https://ui.example"})}});
  expect(std::holds_alternative<secure_frontend::ValidatedConfig>(ok), "two exact origins: " + issues_of(ok));
}

// SP3 SP1-addendum A2 (spec §5.1, owner decisions B1/B11): concurrency rows,
// defaults derived from the worker count, default_max_tokens 0 = auto.
constexpr Sp3Row kSp3RowsA2[] = {
    {"limits.concurrency.max_concurrent_requests", ConfigValueType::integer, 1, 256},
    {"limits.concurrency.max_queued_generations", ConfigValueType::integer, 0, 254},
    {"limits.concurrency.queue_timeout_ms", ConfigValueType::integer, 1'000, 3'600'000},
    {"limits.concurrency.max_connections_per_remote_address", ConfigValueType::integer, 1, 256},
    {"limits.chat.default_max_tokens", ConfigValueType::integer, 0, 1'048'576},  // session log step 13
};

const secure_frontend::FrontendConfig* config_of(const secure_frontend::ConfigValidationResult& result) {
  const auto* validated = std::get_if<secure_frontend::ValidatedConfig>(&result);
  return validated == nullptr ? nullptr : &validated->get();
}

void cfg01_sp3_concurrency_rows_and_defaults() {
  ConfigFixture f;
  for (const auto& row : kSp3RowsA2) {
    const auto* key = secure_frontend::find_config_key(row.path);
    expect(key != nullptr && key->type == row.type && key->minimum == row.minimum && key->maximum == row.maximum,
           std::string{"row and range: "} + row.path);
  }
  const auto result = with_literals(f, {});
  const auto* c = config_of(result);
  expect(c != nullptr, "defaults validate: " + issues_of(result));
  if (c == nullptr) return;
  expect(c->limits.concurrency.max_concurrent_requests == 8U && c->limits.concurrency.max_queued_generations == 4U &&
         c->limits.concurrency.queue_timeout == std::chrono::milliseconds(300000) &&
         c->limits.concurrency.max_connections_per_remote_address == 4U, "spec §5.1 defaults (B11)");
  expect(c->limits.concurrency.queue_derived && c->limits.concurrency.remote_cap_derived, "absent keys are derived");
  expect(c->limits.chat.default_max_tokens == 0U, "default_max_tokens defaults to 0 = auto (B1)");
  const std::string text = secure_frontend::describe(*c);
  expect(text.find("limits.concurrency.max_queued_generations = 4 (derived)") != std::string::npos &&
         text.find("limits.concurrency.max_connections_per_remote_address = 4 (derived)") != std::string::npos,
         "--check marks derived values: " + text);
}

void cfg01_queue_plus_two_le_workers() {
  ConfigFixture f;
  expect(has_issue(with_literals(f, {{"limits.concurrency.max_concurrent_requests", "5"}, {"limits.concurrency.max_queued_generations", "4"}}),
                   "limits.concurrency.max_queued_generations", ConfigIssueCode::conflicting_values), "4 + 2 > 5 is rejected");
  expect(config_of(with_literals(f, {{"limits.concurrency.max_concurrent_requests", "6"}, {"limits.concurrency.max_queued_generations", "4"}})) != nullptr,
         "4 + 2 = 6 is valid");
}

void cfg01_serial_requires_zero_queue() {
  ConfigFixture f;
  expect(has_issue(with_literals(f, {{"limits.concurrency.max_concurrent_requests", "1"}, {"limits.concurrency.max_queued_generations", "1"}}),
                   "limits.concurrency.max_queued_generations", ConfigIssueCode::conflicting_values), "serial needs queue 0");
  expect(config_of(with_literals(f, {{"limits.concurrency.max_concurrent_requests", "1"}, {"limits.concurrency.max_queued_generations", "0"}})) != nullptr,
         "serial with queue 0 is valid");
}

void cfg01_remote_cap_le_workers() {
  ConfigFixture f;
  expect(has_issue(with_literals(f, {{"limits.concurrency.max_concurrent_requests", "3"},
                                     {"limits.concurrency.max_connections_per_remote_address", "4"}}),
                   "limits.concurrency.max_connections_per_remote_address", ConfigIssueCode::conflicting_values), "cap above workers");
}

void cfg01_sp1_config_with_one_worker_still_valid() {
  ConfigFixture f;
  // config_of views into its argument: the result must outlive every read (a
  // dangling view of the temporary crashed the MSVC suite once, session log step 10).
  const auto result = with_literals(f, {{"limits.concurrency.max_concurrent_requests", "1"}});
  const auto* c = config_of(result);
  expect(c != nullptr, "an SP1-era file with one worker validates");
  if (c != nullptr) {
    expect(c->limits.concurrency.max_queued_generations == 0U && c->limits.concurrency.max_connections_per_remote_address == 1U,
           "queue 0 and remote cap 1 are derived");
  }
}

void cfg01_derived_defaults_follow_worker_count() {
  ConfigFixture f;
  const std::pair<int, std::pair<unsigned, unsigned>> table[] = {{1, {0U, 1U}}, {2, {0U, 2U}}, {3, {1U, 3U}},
                                                                  {6, {4U, 4U}}, {8, {4U, 4U}}, {256, {4U, 4U}}};
  for (const auto& [workers, expected] : table) {
    const auto result = with_literals(f, {{"limits.concurrency.max_concurrent_requests", std::to_string(workers)}});
    const auto* c = config_of(result);  // `result` outlives the reads (see above)
    expect(c != nullptr && c->limits.concurrency.max_queued_generations == expected.first &&
           c->limits.concurrency.max_connections_per_remote_address == expected.second,
           "derived for " + std::to_string(workers) + " workers");
    expect(secure_frontend::derived_max_queued_generations(static_cast<std::uint32_t>(workers)) == expected.first &&
           secure_frontend::derived_max_connections_per_remote_address(static_cast<std::uint32_t>(workers), false) == expected.second,
           "derivation functions for " + std::to_string(workers));
  }
}

// Owner 2026-10-23 ("Raise it", after an iPhone's page load lost app.js to the cap): a browser
// opens up to six connections to one server for the page's files, so with listener.web_ui the
// per-remote-address cap derives min(6, workers) instead of min(4, workers). An explicit value
// is still used as written.
void cfg01_web_ui_raises_the_remote_cap() {
  ConfigFixture f;
  const std::pair<int, unsigned> table[] = {{1, 1U}, {4, 4U}, {6, 6U}, {8, 6U}, {256, 6U}};
  for (const auto& [workers, expected] : table) {
    const auto result = with_literals(f, {{"listener.web_ui", "true"}, {"limits.concurrency.max_concurrent_requests", std::to_string(workers)}});
    const auto* c = config_of(result);  // `result` outlives the reads
    expect(c != nullptr && c->limits.concurrency.max_connections_per_remote_address == expected &&
               c->limits.concurrency.remote_cap_derived,
           "with the page, derived for " + std::to_string(workers) + " workers: " + issues_of(result));
    expect(secure_frontend::derived_max_connections_per_remote_address(static_cast<std::uint32_t>(workers), true) == expected,
           "the derivation function with the page, " + std::to_string(workers) + " workers");
  }
  const auto page = with_literals(f, {{"listener.web_ui", "true"}});
  const auto* c = config_of(page);
  expect(c != nullptr && secure_frontend::describe(*c).find("limits.concurrency.max_connections_per_remote_address = 6 (derived)") !=
                             std::string::npos,
         "--check shows the page's derived cap");
  const auto off = with_literals(f, {{"listener.web_ui", "false"}});
  const auto* o = config_of(off);
  expect(o != nullptr && o->limits.concurrency.max_connections_per_remote_address == 4U &&
             secure_frontend::derived_max_connections_per_remote_address(8U, false) == 4U,
         "without the page the cap stays min(4, workers)");
  const auto explicit_cap = with_literals(f, {{"listener.web_ui", "true"}, {"limits.concurrency.max_connections_per_remote_address", "3"}});
  const auto* e = config_of(explicit_cap);
  expect(e != nullptr && e->limits.concurrency.max_connections_per_remote_address == 3U && !e->limits.concurrency.remote_cap_derived,
         "an explicit cap is used as written");
  const auto* key = secure_frontend::find_config_key("limits.concurrency.max_connections_per_remote_address");
  expect(key != nullptr, "the row exists");
}

void cfg01_explicit_values_are_not_adjusted() {
  ConfigFixture f;
  const auto result = with_literals(f, {{"limits.concurrency.max_concurrent_requests", "1"}, {"limits.concurrency.max_queued_generations", "4"}});
  expect(config_of(result) == nullptr && has_issue(result, "limits.concurrency.max_queued_generations", ConfigIssueCode::conflicting_values),
         "an explicit queue of 4 with one worker is an error naming the key, never silently lowered");
  const auto kept = with_literals(f, {{"limits.concurrency.max_queued_generations", "2"}});
  const auto* c = config_of(kept);  // `kept` outlives the reads (see above)
  expect(c != nullptr && c->limits.concurrency.max_queued_generations == 2U && !c->limits.concurrency.queue_derived,
         "an explicit valid value is kept as written");
}
// SP5 M1 Task 3 (spec D-3): every rejected parity row fails at load, naming
// the key and (via the fixed reason) its rule — before unknown-key detection.
void each_rejected_upstream_key_errors_at_load_naming_it() {
  ConfigFixture f;
  for (const auto& row : secure_frontend::parity::strata_parity_rows()) {
    if (row.disposition != secure_frontend::parity::ParityDisposition::rejected) continue;
    const auto result = with_literals(f, {{std::string{row.key}, "1"}});
    expect(has_issue(result, std::string{row.key}, ConfigIssueCode::excluded_key),
           "config with excluded key '" + std::string{row.key} + "' must fail at load naming the key: " + issues_of(result));
  }
  const auto nested = with_literals(f, {{"engine.mcp", "1"}});
  expect(has_issue(nested, "engine.mcp", ConfigIssueCode::excluded_key),
         "a nested excluded member names its full path: " + issues_of(nested));
  // A schema-typed path that shares a rejected flat name stays valid (typed wins).
  const auto typed = with_literals(f, {{"engine.lib_dirs", "[" + quoted(host_absolute("/opt/lib")) + "]"}});
  expect(std::holds_alternative<secure_frontend::ValidatedConfig>(typed),
         "engine.lib_dirs remains a typed key: " + issues_of(typed));
}
// SP5 M1 Task 4: config defaults.sampling.* carry the pin's kinds and bounds.
// Wire form / clamps: serve/server.py:1360-1394 (sampling_keys). Config bounds
// copied from clean_shared_defaults serve/server.py:7331-7367 (temperature
// 0..2, top_p 0<x<=1, top_k int 1..64, seed a positive integer); min_p
// (0,1] from the wire condition at :1372-1374; the caps on presence/frequency
// (2) and penalty_last_n (8192) and the 32-bit seed cap are ADDED security
// bounds (C-1) — the pin validates none.
void sampling_defaults_cover_the_pinned_validator() {
  ConfigFixture f;
  struct Row { const char* path; const char* low_ok; const char* high_ok; const char* below; const char* above; };
  const Row rows[] = {
      {"defaults.sampling.temperature", "0", "2", "-0.1", "2.1"},
      {"defaults.sampling.top_p", "0.5", "1", "0", "1.5"},
      {"defaults.sampling.top_k", "1", "64", "0", "65"},
      {"defaults.sampling.min_p", "0", "1", "-0.1", "1.1"},
      {"defaults.sampling.presence_penalty", "0", "2", "-0.1", "2.1"},
      {"defaults.sampling.repetition_penalty", "0.5", "2", "0", "2.1"},
      {"defaults.sampling.frequency_penalty", "0", "2", "-0.1", "2.1"},
      {"defaults.sampling.penalty_last_n", "0", "8192", "-1", "8193"},
      {"defaults.sampling.seed", "1", "4294967295", "0", "4294967296"},
  };
  for (const auto& row : rows) {
    const auto* key = secure_frontend::find_config_key(row.path);
    expect(key != nullptr, std::string{"schema row exists: "} + row.path);
    for (const char* accept : {row.low_ok, row.high_ok}) {
      const auto* c = config_of(with_literals(f, {{row.path, accept}}));
      expect(c != nullptr, std::string{"accepts "} + accept + " for " + row.path + ": " + issues_of(with_literals(f, {{row.path, accept}})));
    }
    for (const char* reject : {row.below, row.above}) {
      const auto result = with_literals(f, {{row.path, reject}});
      expect(std::holds_alternative<secure_frontend::ConfigErrors>(result),
             std::string{"rejects "} + reject + " for " + row.path);
    }
  }
  // Absent sampling rows stay UNSET (never a sent key), and a set row lands in
  // the typed config. (config_of views into its argument: keep the result alive
  // for the whole read-back — a dangling view here was a latent UAF.)
  const auto bare_result = with_literals(f, {});
  const auto* bare = config_of(bare_result);
  expect(bare != nullptr, "a config without sampling rows still validates");
  const auto set_result = with_literals(f, {{"defaults.sampling.min_p", "0.1"}});
  const auto* set = config_of(set_result);
  expect(set != nullptr && set->sampling.min_p.has_value() && *set->sampling.min_p == 0.1,
         "defaults.sampling.min_p is typed: " + issues_of(set_result));
  expect(!set->sampling.temperature.has_value() && !set->sampling.seed.has_value(),
         "unset rows stay unset");
}

// SP5 M1 Task 5 (parity rows reasoning_* / fit_max_tokens): kinds, bounds and defaults copied from
// the pin — clean_shared_defaults serve/server.py:7341-7343 (effort none|low|medium|high);
// reasoning_budget_tokens :8120-8127 with reasoning_budget :3794-3807 ("a whole number", 0: none;
// the 0..4294967295 bounds are ADDED security bounds, C-1); reasoning_close_retry bool :8015;
// reasoning_loop_recovery false|"stop"|"recover"|true :8129-8136 (the typed row takes the three
// string words); effort_position start|end :2841-2843; anthropic_thinking model|on_request
// :8102-8104; fit_max_tokens bool default true :3485-3488 read at :7996.
// SP5 M2 Task 3: the glm.* surface at Maya's names (C-3), kinds/defaults at
// the vendored pin (C-1), and the profile gate (Review Focus 5: an explicit
// qwen profile refuses glm.* by name). Behaviour-only assertions (the M1
// cfg01_sp3_rows pattern) so the RED lands as failing cases, not a compile error.
void glm_keys_parse_with_mayas_kinds_and_defaults() {
  ConfigFixture f;
  // SP5 M2 Task 7 (D-8): the glm53 launch is composed, so no engine.args row is needed
  // (and none is accepted — see glm_max_context_typed_and_glm53_args_refused).
  const auto glm53 = [](std::map<std::string, std::string> extra) {
    std::map<std::string, std::string> literals{{"engine.profile", "\"glm53\""}};
    for (auto& [key, value] : extra) literals[key] = value;
    return literals;
  };
  for (const char* path : {"glm.thinking_budget", "glm.reasoning_effort", "glm.fit_max_tokens",
                           "glm.experimental_speed_projection", "glm.gpu", "glm.layer_split",
                           "glm.tokenizer", "glm.model_name"}) {
    expect(secure_frontend::find_config_key(path) != nullptr, std::string{"the schema row exists: "} + path);
  }
  const auto accepted = with_literals(f, glm53({
      {"glm.thinking_budget", "4096"},
      {"glm.reasoning_effort", "\"max\""},
      {"glm.fit_max_tokens", "true"},
      {"glm.experimental_speed_projection", "true"},
      {"glm.layer_split", "\"18,32\""},
      {"glm.gpu", "\"0,1\""},
      {"glm.tokenizer", quoted(host_absolute("/opt/tokenizer-pack"))},
      {"glm.model_name", "\"glm-5.3-flash\""}}));
  expect(std::holds_alternative<secure_frontend::ValidatedConfig>(accepted),
         "the full glm.* block validates on the glm53 profile: " + issues_of(accepted));
  const std::string described = describe_of(accepted);
  expect(described.find("glm.thinking_budget = 4096") != std::string::npos, "describe() carries the budget");
  expect(described.find("glm.reasoning_effort = max") != std::string::npos, "describe() carries the effort");
  for (const char* v : {"\"none\"", "\"low\"", "\"high\"", "\"max\""}) {
    expect(std::holds_alternative<secure_frontend::ValidatedConfig>(
               with_literals(f, glm53({{"glm.reasoning_effort", v}}))),
           std::string{"accepts the canonical level "} + v);
  }
  const auto rejected = [&](const std::map<std::string, std::string>& row) {
    const auto result = with_literals(f, glm53(row));
    expect(std::holds_alternative<secure_frontend::ConfigErrors>(result),
           "rejects a bad glm row: " + issues_of(result));
  };
  // Maya normalizes "medium" away (vendored serve/frontend.py:124: medium maps to
  // high) — the typed row admits only the four canonical level words.
  rejected({{"glm.reasoning_effort", "\"medium\""}});
  rejected({{"glm.reasoning_effort", "\"ultrathink\""}});
  rejected({{"glm.thinking_budget", "-1"}});
  rejected({{"glm.thinking_budget", "1000000001"}});  // added bound (C-1)
  rejected({{"glm.thinking_budget", "\"4096\""}});
  rejected({{"glm.experimental_speed_projection", "\"true\""}});
  rejected({{"glm.layer_split", "\"auto;rm\""}});  // closed grammar (SETTINGS.md:44)
  rejected({{"glm.tokenizer", "\"relative/path\""}});  // absolute like session.slot_save_path
  const auto bare_result = with_literals(f, glm53({}));
  expect(std::holds_alternative<secure_frontend::ValidatedConfig>(bare_result),
         "a glm53 config without glm rows validates: " + issues_of(bare_result));
  const std::string bare = describe_of(bare_result);
  expect(bare.find("glm.layer_split = \"auto\"") != std::string::npos, "layer_split defaults to auto (serve/server.py:1276)");
}

void glm_keys_rejected_on_strata_profile_by_name() {
  ConfigFixture f;
  const auto result = with_literals(f, {{"engine.profile", "\"qwen\""}, {"glm.thinking_budget", "4096"},
                                        {"glm.model_name", "\"glm\""}});
  expect(has_issue(result, "glm.thinking_budget", ConfigIssueCode::conflicting_values),
         "glm.thinking_budget is refused by name on the qwen profile: " + issues_of(result));
  expect(has_issue(result, "glm.model_name", ConfigIssueCode::conflicting_values),
         "glm.model_name is refused by name on the qwen profile: " + issues_of(result));
  // A managed model takes its profile from the SP4 manifest, which the loader
  // cannot see: the rows validate here and the Task-7 launch refuses them for a
  // qwen manifest (Ruling T3-a).
  const auto managed = with_literals(f, {{"glm.thinking_budget", "4096"}});
  expect(std::holds_alternative<secure_frontend::ValidatedConfig>(managed),
         "without an explicit profile the rows validate (the manifest decides at launch): " + issues_of(managed));
}

// Gemma 4 plan 1 Task 1: engine.profile takes "gemma4"; --check prints it; its engine.args
// follow the strata_gemma4 allowlist (engine_arguments_test has the full table).
void engine_profile_accepts_gemma4_and_describes_it() {
  ConfigFixture f;
  const auto result = with_literals(f, {{"engine.profile", "\"gemma4\""}});
  const auto* validated = std::get_if<secure_frontend::ValidatedConfig>(&result);
  expect(validated != nullptr, "the bare gemma4 config validates: " + issues_of(result));
  if (validated != nullptr) {
    expect(validated->get().engine.profile == secure_frontend::EngineProfile::gemma4, "engine.profile parses as gemma4");
    expect(describe_of(result).find("engine.profile = \"gemma4\"") != std::string::npos, "--check prints the gemma4 profile");
  }
  const auto args = with_literals(f, {{"engine.profile", "\"gemma4\""},
                                      {"engine.args", "[\"--max-context\", \"32768\", \"--kv\", \"f16\"]"}});
  expect(std::holds_alternative<secure_frontend::ValidatedConfig>(args), "allowlisted gemma4 engine.args validate: " + issues_of(args));
  for (const char* refused : {"[\"--mtp\", \"3\"]", "[\"--batch\", \"2\"]", "[\"--pack\", \"p\"]", "[\"--chunk\", \"0\"]"}) {
    expect(has_issue(with_literals(f, {{"engine.profile", "\"gemma4\""}, {"engine.args", refused}}), "engine.args",
                     ConfigIssueCode::invalid_value),
           "gemma4 refuses engine.args " + std::string{refused});
  }
  const auto gpu = with_literals(f, {{"engine.profile", "\"gemma4\""}, {"engine.gpu", "\"0\""}});
  expect(std::holds_alternative<secure_frontend::ValidatedConfig>(gpu), "engine.gpu is accepted on gemma4, as on qwen: " + issues_of(gpu));
  expect(has_issue(with_literals(f, {{"engine.profile", "\"gemma5\""}}), "engine.profile", ConfigIssueCode::invalid_value),
         "an unknown profile is still refused");
}

// The glm.* guard is "every profile but glm53", so a third profile cannot take the rows.
void glm_keys_rejected_on_gemma4_profile_by_name() {
  ConfigFixture f;
  const auto result = with_literals(f, {{"engine.profile", "\"gemma4\""}, {"glm.thinking_budget", "4096"},
                                        {"glm.model_name", "\"glm\""}});
  expect(has_issue(result, "glm.thinking_budget", ConfigIssueCode::conflicting_values),
         "glm.thinking_budget is refused by name on the gemma4 profile: " + issues_of(result));
  expect(has_issue(result, "glm.model_name", ConfigIssueCode::conflicting_values),
         "glm.model_name is refused by name on the gemma4 profile: " + issues_of(result));
}

// Gemma 4 plan 1 Task 3 (ruling): the rows the qwen launch turns into engine flags (VRAM and
// split, the learned-profile save, --batch, the MTP strip, the effort-end tail) have no Gemma
// counterpart; on an explicit gemma4 profile each is refused by name instead of being ignored.
void qwen_launch_rows_rejected_on_gemma4_profile_by_name() {
  ConfigFixture f;
  const std::vector<std::pair<std::string, std::string>> rows{
      {"engine.vram_reserve_mib", "700"},       {"engine.vram_elastic", "true"},
      {"engine.vram_segment_mib", "512"},       {"engine.layer_split", "\"24\""},
      {"engine.split_skip_if_fits", "true"},    {"session.expert_profile_save_every", "5"},
      {"replicas.parallel", "2"},               {"engine.mtp", "false"},
      {"reasoning.effort_position", "\"end\""}};
  for (const auto& [key, literal] : rows) {
    const auto gemma = with_literals(f, {{"engine.profile", "\"gemma4\""}, {key, literal}});
    expect(has_issue(gemma, key, ConfigIssueCode::conflicting_values), key + " is refused by name on gemma4: " + issues_of(gemma));
    const auto qwen = with_literals(f, {{"engine.profile", "\"qwen\""}, {key, literal}});
    expect(std::holds_alternative<secure_frontend::ValidatedConfig>(qwen), key + " still validates on qwen: " + issues_of(qwen));
  }
  const auto managed = with_literals(f, {{"replicas.parallel", "2"}});
  expect(std::holds_alternative<secure_frontend::ValidatedConfig>(managed),
         "without an explicit profile the rows validate (the manifest decides at launch): " + issues_of(managed));
  const auto plain = with_literals(f, {{"engine.profile", "\"gemma4\""}, {"reasoning.effort", "\"low\""},
                                       {"engine.lazy_load", "true"}, {"engine.gpu", "\"0\""}});
  expect(std::holds_alternative<secure_frontend::ValidatedConfig>(plain),
         "reasoning.effort, lifecycle rows and engine.gpu stay accepted on gemma4: " + issues_of(plain));
}

// flint: engine.profile takes "flint", --check prints it, its engine.args follow the strata_flint allowlist, and the
// qwen launch rows the flint engine has no counterpart for are refused by name, as on gemma4.
void engine_profile_accepts_flint_and_refuses_qwen_rows() {
  ConfigFixture f;
  const auto result = with_literals(f, {{"engine.profile", "\"flint\""}});
  const auto* validated = std::get_if<secure_frontend::ValidatedConfig>(&result);
  expect(validated != nullptr, "the bare flint config validates: " + issues_of(result));
  if (validated != nullptr) {
    expect(validated->get().engine.profile == secure_frontend::EngineProfile::flint, "engine.profile parses as flint");
    expect(describe_of(result).find("engine.profile = \"flint\"") != std::string::npos, "--check prints the flint profile");
  }
  const auto args = with_literals(f, {{"engine.profile", "\"flint\""},
                                      {"engine.args", "[\"--max-context\", \"65536\", \"--cpu-lane\", \"off\"]"}});
  expect(std::holds_alternative<secure_frontend::ValidatedConfig>(args), "allowlisted flint engine.args validate: " + issues_of(args));
  for (const char* refused : {"[\"--kv\", \"f16\"]", "[\"--hybrid-rows\", \"8\"]", "[\"--mtp\", \"3\"]", "[\"--chunk\", \"0\"]"}) {
    expect(has_issue(with_literals(f, {{"engine.profile", "\"flint\""}, {"engine.args", refused}}), "engine.args",
                     ConfigIssueCode::invalid_value),
           "flint refuses engine.args " + std::string{refused});
  }
  for (const auto& [key, literal] : std::vector<std::pair<std::string, std::string>>{
           {"engine.vram_reserve_mib", "700"}, {"replicas.parallel", "2"}, {"engine.mtp", "false"},
           {"reasoning.effort_position", "\"end\""}, {"glm.model_name", "\"glm\""}}) {
    const auto flint = with_literals(f, {{"engine.profile", "\"flint\""}, {key, literal}});
    expect(has_issue(flint, key, ConfigIssueCode::conflicting_values), key + " is refused by name on flint: " + issues_of(flint));
  }
  const auto plain = with_literals(f, {{"engine.profile", "\"flint\""}, {"reasoning.effort", "\"none\""}, {"engine.gpu", "\"0\""}});
  expect(std::holds_alternative<secure_frontend::ValidatedConfig>(plain),
         "reasoning.effort and engine.gpu stay accepted on flint: " + issues_of(plain));
  expect(secure_frontend::arguments_profile_for(secure_frontend::EngineProfile::flint) ==
             secure_frontend::EngineArgumentsProfile::strata_flint,
         "flint selects strata_flint");
}

// Each profile selects its own argument profile (an exhaustive switch, no shared default).
void every_profile_selects_its_own_argument_profile() {
  using secure_frontend::EngineArgumentsProfile;
  using secure_frontend::EngineProfile;
  expect(secure_frontend::arguments_profile_for(EngineProfile::qwen) == EngineArgumentsProfile::strata_0143,
         "qwen selects strata_0143");
  expect(secure_frontend::arguments_profile_for(EngineProfile::glm53) == EngineArgumentsProfile::lighttransport_glm53,
         "glm53 selects lighttransport_glm53");
  expect(secure_frontend::arguments_profile_for(EngineProfile::gemma4) == EngineArgumentsProfile::strata_gemma4,
         "gemma4 selects strata_gemma4");
  expect(secure_frontend::arguments_profile_for(EngineProfile::deepseek) == EngineArgumentsProfile::maya_ds4,
         "deepseek selects maya-ds4's allowlist (Maya's DeepSeek engine, 2026-10-27)");
  expect(secure_frontend::arguments_profile_for(EngineProfile::raon1) == EngineArgumentsProfile::lighttransport_glm53,
         "raon1 rides the glm_pack engine (lighttransport_glm53)");
}

void thinking_budget_default_is_mayas_32768() {
  ConfigFixture f;
  const auto result = with_literals(f, {{"engine.profile", "\"glm53\""}});
  expect(std::holds_alternative<secure_frontend::ValidatedConfig>(result),
         "the bare glm53 config validates: " + issues_of(result));
  const std::string described = describe_of(result);
  expect(described.find("glm.thinking_budget = 32768") != std::string::npos,
         "Maya's thinking_budget default is 32768 (vendored serve/server.py:4090; C-3 keeps Maya's name)");
  expect(described.find("glm.fit_max_tokens = false") != std::string::npos,
         "Maya's fit_max_tokens defaults false (serve/server.py:4077 reads `is True`)");
  expect(described.find("glm.experimental_speed_projection = false") != std::string::npos,
         "speed projection defaults false (serve/server.py:3819)");
}

// SP5 M2 Task 7 (D-8, C-5): the GLM launch surface is typed, not passed through.
// glm.max_context carries Maya's DEFAULT_CONTEXT (maya.py:3148; bound added, C-1)
// and any engine.args row under glm53 is refused by name at config load.
void glm_max_context_typed_and_glm53_args_refused() {
  ConfigFixture f;
  expect(secure_frontend::find_config_key("glm.max_context") != nullptr, "the schema row glm.max_context exists");
  const auto accepted = with_literals(f, {{"engine.profile", "\"glm53\""}});
  expect(std::holds_alternative<secure_frontend::ValidatedConfig>(accepted),
         "a glm53 config needs no engine argument any more: " + issues_of(accepted));
  if (std::holds_alternative<secure_frontend::ValidatedConfig>(accepted)) {
    const std::string described = describe_of(accepted);
    expect(described.find("glm.max_context = 32768") != std::string::npos, "Maya's --max-context default is 32768 (maya.py:3148)");
  }
  for (const char* args : {"[\"--max-context\", \"32768\"]", "[\"--model\"]", "[\"--gpu\", \"0\"]", "[\"--serve\"]"}) {
    const auto result = with_literals(f, {{"engine.profile", "\"glm53\""}, {"engine.args", args}});
    expect(has_issue(result, "engine.args", ConfigIssueCode::invalid_value), "glm53 refuses engine.args: " + std::string{args});
  }
  const auto high = with_literals(f, {{"engine.profile", "\"glm53\""}, {"glm.max_context", "1048577"}});
  expect(has_issue(high, "glm.max_context", ConfigIssueCode::out_of_range), "the added upper bound refuses (C-1)");
  const auto zero = with_literals(f, {{"engine.profile", "\"glm53\""}, {"glm.max_context", "0"}});
  expect(has_issue(zero, "glm.max_context", ConfigIssueCode::out_of_range), "a context of 0 tokens is refused");
}

// SP5-P2 Task 4 (D-7 refresh; SP6 §4.9 sheet M2): glm.ram_headroom_gb is the
// typed value source for the STRATA_GLM_RAM_HEADROOM_GB child-env row — integer
// 6..1024, default 16 (the owner's M2 value: MAYA's own default is 6, but
// SETTINGS.md:28 reports a desktop that stayed responsive at 16 and swapped at
// 12; the engine reads it at glm_fast_path.cu:1110/:1510). It projects at the
// default, so the row is always forced on the glm child unless the operator moves it.
void glm_ram_headroom_gb_typed_and_always_projected() {
  ConfigFixture f;
  expect(secure_frontend::find_config_key("glm.ram_headroom_gb") != nullptr, "the schema row glm.ram_headroom_gb exists");
  const auto accepted = with_literals(f, {{"engine.profile", "\"glm53\""}});
  expect(std::holds_alternative<secure_frontend::ValidatedConfig>(accepted), "the bare glm53 config validates: " + issues_of(accepted));
  if (std::holds_alternative<secure_frontend::ValidatedConfig>(accepted))
    expect(describe_of(accepted).find("glm.ram_headroom_gb = 16") != std::string::npos, "the M2 default is 16 GB (SP6 §4.9 M2)");
  const auto* bare = config_of(accepted);
  if (bare != nullptr) {
    const auto knobs = secure_frontend::glm_environment_knobs(*bare);
    const auto row = std::find_if(knobs.begin(), knobs.end(),
                                  [](const auto& k) { return k.name == "STRATA_GLM_RAM_HEADROOM_GB"; });
    expect(row != knobs.end() && row->value == "16", "the headroom projects at its default onto the closed allowlist row");
  }
  const auto moved = with_literals(f, {{"engine.profile", "\"glm53\""}, {"glm.ram_headroom_gb", "6"}});
  const auto* moved_config = config_of(moved);
  expect(moved_config != nullptr, "6 GB validates: " + issues_of(moved));
  if (moved_config != nullptr) {
    const auto knobs = secure_frontend::glm_environment_knobs(*moved_config);
    const auto row = std::find_if(knobs.begin(), knobs.end(),
                                  [](const auto& k) { return k.name == "STRATA_GLM_RAM_HEADROOM_GB"; });
    expect(row != knobs.end() && row->value == "6", "the operator moves the typed row (MAYA's own default 6)");
  }
  const auto low = with_literals(f, {{"engine.profile", "\"glm53\""}, {"glm.ram_headroom_gb", "5"}});
  expect(has_issue(low, "glm.ram_headroom_gb", ConfigIssueCode::out_of_range), "5 GB is refused (the M2 floor, 6)");
  const auto over = with_literals(f, {{"engine.profile", "\"glm53\""}, {"glm.ram_headroom_gb", "1025"}});
  expect(has_issue(over, "glm.ram_headroom_gb", ConfigIssueCode::out_of_range), "1025 GB is refused (the M2 bound, 1024)");
}

// Calibration (owner 2026-10-28, Maya's tools/calibrate_glm.py): glm.pcie_share and glm.cpu_lane are the typed sources
// of the GLM engine's STRATA_GLM_PCIE_SHARE and STRATA_GLM_CPU_LANE (glm_fast_path.cu reads both at its start). Unset,
// nothing projects (the engine measures its own split, its lane the physical cores); set, they render; out of range
// they are refused.
void glm_cpu_lane_rows_project_when_set() {
  ConfigFixture f;
  const auto knob = [](const secure_frontend::FrontendConfig& config, const std::string_view name) -> std::optional<std::string> {
    for (const auto& k : secure_frontend::glm_environment_knobs(config)) {
      if (k.name == name) return k.value;
    }
    return std::nullopt;
  };
  const auto bare = with_literals(f, {{"engine.profile", "\"glm53\""}});
  if (const auto* config = config_of(bare)) {
    expect(!knob(*config, "STRATA_GLM_PCIE_SHARE") && !knob(*config, "STRATA_GLM_CPU_LANE"), "unset: the engine's own choice");
  } else {
    expect(false, "the bare glm53 config validates: " + issues_of(bare));
  }
  const auto set = with_literals(f, {{"engine.profile", "\"glm53\""}, {"glm.pcie_share", "0.25"}, {"glm.cpu_lane", "9"}});
  if (const auto* config = config_of(set)) {
    expect(knob(*config, "STRATA_GLM_PCIE_SHARE") == std::optional<std::string>{"0.250"} &&
               knob(*config, "STRATA_GLM_CPU_LANE") == std::optional<std::string>{"9"},
           "set: both render onto the closed allowlist rows");
  } else {
    expect(false, "the calibrated rows validate: " + issues_of(set));
  }
  expect(has_issue(with_literals(f, {{"engine.profile", "\"glm53\""}, {"glm.pcie_share", "1.5"}}), "glm.pcie_share", ConfigIssueCode::out_of_range) &&
             has_issue(with_literals(f, {{"engine.profile", "\"glm53\""}, {"glm.cpu_lane", "65"}}), "glm.cpu_lane", ConfigIssueCode::out_of_range) &&
             has_issue(with_literals(f, {{"engine.profile", "\"glm53\""}, {"glm.cpu_lane", "0"}}), "glm.cpu_lane", ConfigIssueCode::out_of_range),
         "a share past 1 and a lane of 0 or past the engine's 64 are refused");
}

// glm.kv: the GLM engine's --kv as a closed choice - fp16 (the engine's default: glm5-next's FP16 latent, raon1's
// exact F32 cache), int8 (glm5-next's INT8 latent), q8_0 / q4_0 (raon1's quantized cache). Default fp16; anything
// else is refused at load.
void glm_kv_is_a_closed_choice() {
  ConfigFixture f;
  expect(secure_frontend::find_config_key("glm.kv") != nullptr, "the schema row glm.kv exists");
  const auto bare = with_literals(f, {{"engine.profile", "\"glm53\""}});
  const auto* c = config_of(bare);
  expect(c != nullptr, "the bare glm53 config validates: " + issues_of(bare));
  if (c != nullptr) {
    expect(c->glm.kv == "fp16", "glm.kv defaults to fp16 (the engine's own default)");
    expect(describe_of(bare).find("glm.kv = fp16") != std::string::npos, "the description shows glm.kv");
  }
  for (const std::string kv : {"fp16", "int8", "q8_0", "q4_0"}) {
    const auto set = with_literals(f, {{"engine.profile", "\"glm53\""}, {"glm.kv", "\"" + kv + "\""}});
    const auto* sc = config_of(set);
    expect(sc != nullptr && sc->glm.kv == kv, "glm.kv " + kv + " validates and is kept: " + issues_of(set));
  }
  for (const std::string bad : {"q5_0", "f32", "Q4_0", ""}) {
    const auto refused = with_literals(f, {{"engine.profile", "\"glm53\""}, {"glm.kv", "\"" + bad + "\""}});
    expect(has_issue(refused, "glm.kv", ConfigIssueCode::invalid_value), "glm.kv '" + bad + "' is refused");
  }
}

// SP5-P2 Task 4 (D-7): Maya v1.1.21's server gained the idle-unload timer —
// the config's "idle_unload_s" (serve/server.py:8298, 0 = never, the default
// at :3753). The glm profile names it glm.idle_unload_s and it wins over
// engine.idle_unload_s for that family; unset keeps the M1 key's value.
void glm_idle_unload_s_wins_for_the_glm_profile() {
  ConfigFixture f;
  expect(secure_frontend::find_config_key("glm.idle_unload_s") != nullptr, "the schema row glm.idle_unload_s exists");
  const auto both = with_literals(f, {{"engine.profile", "\"glm53\""}, {"engine.idle_unload_s", "30"}, {"glm.idle_unload_s", "120"}});
  const auto* c = config_of(both);
  expect(c != nullptr, "both windows validate together: " + issues_of(both));
  if (c != nullptr) expect(c->engine.idle_unload == std::chrono::seconds(120), "the Maya-named key drives the glm53 window");
  const auto only_engine = with_literals(f, {{"engine.profile", "\"glm53\""}, {"engine.idle_unload_s", "30"}});
  const auto* kept = config_of(only_engine);
  expect(kept != nullptr && kept->engine.idle_unload == std::chrono::seconds(30), "unset (0) keeps the M1 engine.idle_unload_s");
  const auto over = with_literals(f, {{"engine.profile", "\"glm53\""}, {"glm.idle_unload_s", "86401"}});
  expect(has_issue(over, "glm.idle_unload_s", ConfigIssueCode::out_of_range), "the same 86400 cap as engine.idle_unload_s (C-1)");
}

// SP5 M2 Task 4 (D-7): the config-to-env projection — glm.layer_split moves
// the STRATA_GLM_SPLIT forced default in the glm child environment
// (maya-ref docs/SETTINGS.md:44), and glm.gpu renders the family's CUDA
// selector (serve/server.py:1280-1288); the fixture carries no vision, and
// Task 8's case pins what a vision turn adds.
void glm_environment_projection_moves_the_split_default() {
  ConfigFixture f;
  const auto result = with_literals(f, {{"engine.profile", "\"glm53\""},
                                        {"glm.layer_split", "\"7,12\""},
                                        {"glm.gpu", "\"1\""}});
  const auto* config = config_of(result);
  expect(config != nullptr, "the glm53 config validates: " + issues_of(result));
  if (config == nullptr) return;
  const auto knobs = secure_frontend::glm_environment_knobs(*config);
  // SP5-P2 Task 4: the always-projected RAM headrow row joins the moved split
  // (glm.ram_headroom_gb, default 16, SP6 §4.9 M2) — exactly these two project here.
  const auto knob_named = [&knobs](const char* name) -> const secure_frontend::ForcedEnvironmentValue* {
    const auto it = std::find_if(knobs.begin(), knobs.end(),
                                 [name](const auto& k) { return k.name == name; });
    return it == knobs.end() ? nullptr : &*it;
  };
  expect(knobs.size() == 2U && knob_named("STRATA_GLM_SPLIT") != nullptr &&
             knob_named("STRATA_GLM_SPLIT")->value == "7,12" && knob_named("STRATA_GLM_RAM_HEADROOM_GB") != nullptr &&
             knob_named("STRATA_GLM_RAM_HEADROOM_GB")->value == "16",
         "layer_split projects to the SPLIT row and the headroom projects at its default; nothing else does");
  const secure_frontend::EngineEnvironment environment{
      .lib_dirs = {},
      .temp_dir = "/state/engine-tmp/1",
      .cuda_cache_dir = "/state/cache/cuda",
      .family = secure_frontend::EngineFamily::glm,
      .glm_gpus = config->glm.gpus,
      .glm_knobs = knobs,
  };
  const auto block = secure_frontend::render_family_environment(environment);
  expect(block != std::nullopt && block->at("STRATA_GLM_SPLIT") == "7,12" && block->at("CUDA_VISIBLE_DEVICES") == "1" &&
             block->at("CUDA_DEVICE_ORDER") == "PCI_BUS_ID" && block->at("STRATA_GLM_SLOTS") == "0" &&
             block->at("STRATA_GLM_RAM_HEADROOM_GB") == "16",
         "the rendered glm child environment carries the moved split, the headroom and the selector");
}

// SP5 M2 Task 8 (D-8): the GLM vision rows reach BOTH children. On the engine side
// the VLEND-family row (STRATA_GLM_VISION_LEND_MB, the typed engine_env row at
// serve/server.py:4026) projects and renders only when a vision turn is configured
// — vision.enabled and the encoder not resident (the on-demand flow, server.py:4015,
// where the engine lends the expert-cache tail; resident vision holds its own VRAM
// and never lends). Without a turn the row never reaches the child, even if the
// internal knob channel carries one (fail closed). On the encoder side the argv
// carries Maya's flag form (--no-flash-attn, server.py:1055/:1222).
void glm_vision_flags_reach_both_children() {
  ConfigFixture f;
  const auto on_demand = with_literals(f, {{"engine.profile", "\"glm53\""}, {"glm.gpu", "\"1\""},
                                           {"vision", "{\"enabled\": true, \"resident\": false, \"need_mib\": 12288}"}});
  const auto* config = config_of(on_demand);
  expect(config != nullptr, "glm53 with on-demand vision validates: " + issues_of(on_demand));
  if (config == nullptr) return;
  const auto has_lend = [](const std::vector<secure_frontend::ForcedEnvironmentValue>& rows) {
    return std::any_of(rows.begin(), rows.end(),
                       [](const auto& row) { return row.name == "STRATA_GLM_VISION_LEND_MB"; });
  };
  const auto knobs = secure_frontend::glm_environment_knobs(*config);
  const auto lend = std::find_if(knobs.begin(), knobs.end(), [](const auto& row) {
    return row.name == "STRATA_GLM_VISION_LEND_MB";
  });
  expect(lend != knobs.end() && lend->value == "12288",
         "the vision turn projects the LEND row from vision.need_mib (server.py:4026, images.py:156)");
  secure_frontend::EngineEnvironment environment{
      .lib_dirs = {},
      .temp_dir = "/state/engine-tmp/1",
      .cuda_cache_dir = "/state/cache/cuda",
      .family = secure_frontend::EngineFamily::glm,
      .glm_gpus = config->glm.gpus,
      .glm_knobs = knobs,
      .glm_vision_turn = true,
  };
  const auto block = secure_frontend::render_family_environment(environment);
  const auto in_block = block && block->find("STRATA_GLM_VISION_LEND_MB") != block->end();
  expect(in_block && block->at("STRATA_GLM_VISION_LEND_MB") == "12288",
         "the engine child carries the LEND row when a vision turn is configured");
  // No vision at all: nothing projects, and a smuggled knob row is suppressed.
  const auto no_vision = with_literals(f, {{"engine.profile", "\"glm53\""}});
  const auto* plain = config_of(no_vision);
  expect(plain != nullptr, "the bare glm53 config validates: " + issues_of(no_vision));
  if (plain != nullptr) expect(!has_lend(secure_frontend::glm_environment_knobs(*plain)), "nothing projects without vision");
  secure_frontend::EngineEnvironment off = environment;
  off.glm_knobs.push_back({"STRATA_GLM_VISION_LEND_MB", "12288"});  // the internal channel, smuggled
  off.glm_vision_turn = false;
  const auto rendered_off = secure_frontend::render_family_environment(off);
  expect(rendered_off != std::nullopt && rendered_off->find("STRATA_GLM_VISION_LEND_MB") == rendered_off->end(),
         "without a vision turn the child never carries the LEND row (fail closed)");
  // Resident vision holds the encoder's VRAM beside the model (server.py:1041's resident
  // flow): no lending, no row.
  const auto resident = with_literals(f, {{"engine.profile", "\"glm53\""},
                                          {"vision", "{\"enabled\": true, \"need_mib\": 12288}"}});
  const auto* resident_config = config_of(resident);
  expect(resident_config != nullptr, "glm53 with resident vision validates: " + issues_of(resident));
  if (resident_config != nullptr)  // vision.resident defaults true (the SP6 shared key, M1 Task 12)
    expect(!has_lend(secure_frontend::glm_environment_knobs(*resident_config)), "resident vision does not lend (:4015)");
  // The encoder child: Maya's form (server.py:1047-1055) — --mmproj/--model first,
  // the threads/max-tokens pair in the same shape, --no-flash-attn when configured (:1055).
  const secure_frontend::VisionChildSpec spec{
      .mmproj = "/p/mmproj.gguf", .model = "/p/model.gguf", .threads = 4, .max_tokens = std::nullopt, .no_flash_attn = true};
  expect(secure_frontend::compose_vision_arguments(spec) ==
             std::vector<std::string>{"--mmproj", "/p/mmproj.gguf", "--model", "/p/model.gguf", "--threads", "4",
                                      "--no-flash-attn"},
         "the encoder argv carries Maya's form (--no-flash-attn: server.py:1055, :1222)");
}

void reasoning_rows_match_the_pinned_validator() {
  ConfigFixture f;
  const auto accept = [&](const char* path, const char* value) {
    const auto result = with_literals(f, {{path, value}});
    expect(std::holds_alternative<secure_frontend::ValidatedConfig>(result),
           std::string{"accepts "} + value + " for " + path + ": " + issues_of(result));
  };
  const auto reject = [&](const char* path, const char* value) {
    const auto result = with_literals(f, {{path, value}});
    expect(std::holds_alternative<secure_frontend::ConfigErrors>(result),
           std::string{"rejects "} + value + " for " + path);
  };
  for (const char* v : {"\"none\"", "\"low\"", "\"medium\"", "\"high\""}) accept("reasoning.effort", v);
  reject("reasoning.effort", "\"max\"");  // not one of the pin's levels (:7342)
  reject("reasoning.effort", "1");
  accept("reasoning.budget_tokens", "0");
  accept("reasoning.budget_tokens", "4294967295");
  reject("reasoning.budget_tokens", "-1");       // added bound (C-1): the pin treats <= 0 as no budget
  reject("reasoning.budget_tokens", "4294967296");  // added 32-bit cap (C-1)
  accept("reasoning.close_retry", "true");
  accept("reasoning.close_retry", "false");
  reject("reasoning.close_retry", "1");
  for (const char* v : {"\"off\"", "\"stop\"", "\"recover\""}) accept("reasoning.loop_recovery", v);
  reject("reasoning.loop_recovery", "\"retry\"");  // not one of the pin's words (:8134)
  reject("reasoning.loop_recovery", "true");       // the pin's bool form: the typed row names the strings
  for (const char* v : {"\"start\"", "\"end\""}) accept("reasoning.effort_position", v);
  reject("reasoning.effort_position", "\"middle\"");
  for (const char* v : {"\"model\"", "\"on_request\""}) accept("reasoning.anthropic_thinking", v);
  reject("reasoning.anthropic_thinking", "\"on\"");
  accept("engine.fit_max_tokens", "false");
  reject("engine.fit_max_tokens", "\"false\"");
  // The typed defaults follow the pin; an unset effort row stays unset. (config_of views into its
  // argument: the validation result stays alive for the whole read-back.)
  const auto bare_result = with_literals(f, {});
  const auto* bare = config_of(bare_result);
  expect(bare != nullptr, "a config without reasoning rows still validates: " + issues_of(bare_result));
  if (bare != nullptr) {
    using namespace secure_frontend;
    expect(!bare->reasoning.effort.has_value() && bare->reasoning.budget_tokens == 0 && !bare->reasoning.close_retry &&
               bare->reasoning.loop_recovery == LoopRecovery::off &&
               bare->reasoning.effort_position == EffortPosition::start &&
               bare->reasoning.anthropic_thinking == AnthropicThinking::model && bare->engine.fit_max_tokens,
           "the pin's defaults: budget 0, close_retry false, recovery off, position start, thinking model, fit true");
    const std::string described = describe(*bare);
    expect(described.find("reasoning.effort = (unset)") != std::string::npos &&
               described.find("reasoning.effort_position = start") != std::string::npos &&
               described.find("engine.fit_max_tokens = true") != std::string::npos,
           "describe reports the reasoning family");
  }
  const auto set_result = with_literals(f, {{"reasoning.effort", "\"low\""},
                                            {"reasoning.budget_tokens", "2048"},
                                            {"reasoning.effort_position", "\"end\""}});
  const auto* set = config_of(set_result);
  expect(set != nullptr && set->reasoning.effort.has_value() &&
             *set->reasoning.effort == secure_frontend::ReasoningEffort::low &&
             set->reasoning.budget_tokens == 2048 &&
             set->reasoning.effort_position == secure_frontend::EffortPosition::end,
         "reasoning rows are typed: " + issues_of(set_result));
}

// SP5 M1 Task 6 (parity rows lazy_load / idle_unload_s / mtp): runconfig.py:35-38 —
// lazy_load bool "Start without loading the model; the first request loads it (text only)";
// idle_unload_s num>=0, "0 or empty: never" (typed whole seconds; the 86400 cap is ADDED, C-1);
// mtp bool, and draft_layer.is_off demands an explicit false (absent or true: on).
void lifecycle_rows_match_the_pin() {
  ConfigFixture f;
  const auto accept = [&](const char* path, const char* value) {
    const auto result = with_literals(f, {{path, value}});
    expect(std::holds_alternative<secure_frontend::ValidatedConfig>(result),
           std::string{"accepts "} + value + " for " + path + ": " + issues_of(result));
  };
  const auto reject = [&](const char* path, const char* value) {
    const auto result = with_literals(f, {{path, value}});
    expect(std::holds_alternative<secure_frontend::ConfigErrors>(result), std::string{"rejects "} + value + " for " + path);
  };
  accept("engine.lazy_load", "true");
  accept("engine.lazy_load", "false");
  reject("engine.lazy_load", "1");
  accept("engine.idle_unload_s", "0");
  accept("engine.idle_unload_s", "86400");
  reject("engine.idle_unload_s", "-1");      // the pin: num>=0 (:35 "0 or empty: never")
  reject("engine.idle_unload_s", "86401");   // added cap (C-1)
  reject("engine.idle_unload_s", "\"60\"");
  accept("engine.mtp", "false");
  accept("engine.mtp", "true");
  reject("engine.mtp", "\"false\"");
  const auto bare_result = with_literals(f, {});
  const auto* bare = config_of(bare_result);
  expect(bare != nullptr, "a config without lifecycle rows still validates: " + issues_of(bare_result));
  if (bare != nullptr) {
    expect(!bare->engine.lazy_load && bare->engine.idle_unload == std::chrono::seconds(0) && bare->engine.mtp,
           "the pin's defaults: lazy false, idle 0 (never), mtp on unless explicitly false");
    const std::string described = secure_frontend::describe(*bare);
    expect(described.find("engine.lazy_load = false") != std::string::npos &&
               described.find("engine.idle_unload_s = 0") != std::string::npos &&
               described.find("engine.mtp = true") != std::string::npos,
           "describe reports the lifecycle family");
  }
  const auto set_result = with_literals(f, {{"engine.lazy_load", "true"}, {"engine.idle_unload_s", "600"}, {"engine.mtp", "false"}});
  const auto* set = config_of(set_result);
  expect(set != nullptr && set->engine.lazy_load && set->engine.idle_unload == std::chrono::seconds(600) && !set->engine.mtp,
         "lifecycle rows are typed: " + issues_of(set_result));
}

// SP5 M1 Task 7 (parity rows vram_reserve_mib / min_free_vram_mib / vram_elastic /
// vram_segment_mib / elastic / elastic_idle_s / elastic_mib / gpu_order / gpu_policy /
// layer_split / split_skip_if_fits / hip_ordinal / uma): kinds and defaults verbatim from
// the pin — runconfig.py:42, serve/server.py:2550-2552/2870-2948/3023/3112/8083,
// serve/images.py:23/45/173 — the caps and the enum vocabularies' closed sets are added (C-1).
void vram_family_rows_match_the_pin() {
  ConfigFixture f;
  const auto bare_result = with_literals(f, {});
  const auto* bare = config_of(bare_result);
  expect(bare != nullptr, "a config without the vram family still validates: " + issues_of(bare_result));
  if (bare != nullptr) {
    expect(!bare->engine.vram_reserve_mib.has_value() && bare->engine.min_free_vram == 0 && !bare->engine.vram_elastic &&
               !bare->engine.vram_segment_mib.has_value() && !bare->engine.elastic &&
               bare->engine.elastic_idle == std::chrono::seconds(120) && bare->engine.elastic_mib == 2048 &&
               bare->engine.gpu_order == secure_frontend::GpuOrder::auto_order &&
               bare->engine.gpu_policy == secure_frontend::GpuTurnPolicy::auto_policy &&
               bare->engine.layer_split == "auto" && !bare->engine.split_skip_if_fits && !bare->engine.hip_ordinal.has_value() &&
               !bare->engine.uma,
           "the pin's defaults: unset MiB rows, elastic 2048 MiB / 120 s, auto vocabularies, uma false");
    const std::string described = secure_frontend::describe(*bare);
    expect(described.find("engine.layer_split = auto\n") != std::string::npos &&
               described.find("engine.elastic_mib = 2048\n") != std::string::npos &&
               described.find("engine.min_free_vram_mib = 0\n") != std::string::npos &&
               described.find("engine.uma = false\n") != std::string::npos,
           "describe reports the vram family");
  }
  const auto accept = [&](const char* path, const char* value) {
    const auto result = with_literals(f, {{path, value}});
    expect(std::holds_alternative<secure_frontend::ValidatedConfig>(result),
           std::string{"accepts "} + value + " for " + path + ": " + issues_of(result));
  };
  const auto reject = [&](const char* path, const char* value) {
    const auto result = with_literals(f, {{path, value}});
    expect(std::holds_alternative<secure_frontend::ConfigErrors>(result), std::string{"rejects "} + value + " for " + path);
  };
  accept("engine.vram_elastic", "true");
  reject("engine.vram_elastic", "\"true\"");
  accept("engine.split_skip_if_fits", "false");
  accept("engine.elastic", "true");
  accept("engine.uma", "true");
  accept("engine.gpu_order", "\"as_given\"");
  reject("engine.gpu_order", "\"faster\"");   // the pin reorders for anything but "as_given"; the typed row closes the set
  accept("engine.gpu_order", "\"auto\"");
  accept("engine.gpu_policy", "\"shrink\"");
  reject("engine.gpu_policy", "\"release\"");  // POLICIES is (auto, keep, shrink, unload) (images.py:45)
  accept("engine.layer_split", "\"24,36,42\"");
  reject("engine.layer_split", "\"24,16,12,12\"");  // the pin's per-card-counts error (:2888-2892)
  reject("engine.layer_split", "1.5");
}

// SP5 M1 Task 7: each MiB row accepts 0 / its minimum and the row's max, and rejects one
// past the cap — the caps are added bounds (C-1); the pin's floors are kept (segment > 0,
// :2947). hip_ordinal mirrors hip_visible's number space with an added cap.
void bounds_are_the_pins_at_each_edge() {
  ConfigFixture f;
  const auto accept = [&](const char* path, const char* value) {
    const auto result = with_literals(f, {{path, value}});
    expect(std::holds_alternative<secure_frontend::ValidatedConfig>(result),
           std::string{"accepts "} + value + " for " + path + ": " + issues_of(result));
  };
  const auto reject = [&](const char* path, const char* value) {
    const auto result = with_literals(f, {{path, value}});
    expect(std::holds_alternative<secure_frontend::ConfigErrors>(result), std::string{"rejects "} + value + " for " + path);
  };
  accept("engine.vram_reserve_mib", "0");
  accept("engine.vram_reserve_mib", "1048576");
  reject("engine.vram_reserve_mib", "1048577");
  accept("engine.min_free_vram_mib", "0");
  accept("engine.min_free_vram_mib", "1048576");
  reject("engine.min_free_vram_mib", "1048577");
  accept("engine.vram_segment_mib", "1");  // the pin: isinstance int and > 0 (:2947)
  reject("engine.vram_segment_mib", "0");
  reject("engine.vram_segment_mib", "-1");
  accept("engine.elastic_mib", "1");
  accept("engine.elastic_mib", "1048576");
  reject("engine.elastic_mib", "1048577");
  accept("engine.elastic_idle_s", "1");
  accept("engine.elastic_idle_s", "86400");
  reject("engine.elastic_idle_s", "86401");
  reject("engine.elastic_idle_s", "0");  // the pin's "0 == empty: 120" falsy quirk is not copied (Ruling T7-h)
  accept("engine.hip_ordinal", "0");
  accept("engine.hip_ordinal", "127");
  reject("engine.hip_ordinal", "128");
  reject("engine.hip_ordinal", "-1");
  const auto set_result = with_literals(f, {{"engine.vram_reserve_mib", "900"}, {"engine.hip_ordinal", "1"},
                                            {"engine.layer_split", "\"18\""}, {"engine.vram_elastic", "true"}});
  const auto* set = config_of(set_result);
  expect(set != nullptr && set->engine.vram_reserve_mib.has_value() && *set->engine.vram_reserve_mib == 900 &&
             set->engine.hip_ordinal.has_value() && *set->engine.hip_ordinal == 1 && set->engine.layer_split == "18" &&
             set->engine.vram_elastic,
         "the vram family is typed: " + issues_of(set_result));
}

// SP5 M1 Task 8 (parity rows engine_silence_s / answer_room / repeat_stop_tokens /
// max_tokens / min_tokens / model.aliases). Defaults verbatim from the pin: silence 300 s
// (runconfig.py:39), answer_room true (serve/server.py:8014 `is not False`), repeat 256
// (REPEAT_STOP_TOKENS :197, :8115), the MiB-style token bounds unset (added rows, C-1),
// aliases empty; model.aliases grammar is set_aliases' name rule (:3764-3777).
void timeout_shape_rows_match_the_pin() {
  ConfigFixture f;
  const auto bare_result = with_literals(f, {});
  const auto* bare = config_of(bare_result);
  expect(bare != nullptr, "a config without the timeout/shape rows still validates: " + issues_of(bare_result));
  if (bare != nullptr) {
    expect(bare->engine.silence == std::chrono::seconds(300) && bare->engine.answer_room &&
               bare->engine.repeat_stop_tokens == 256U && !bare->engine.max_tokens.has_value() &&
               !bare->engine.min_tokens.has_value() && bare->engine.aliases.empty(),
           "the pin's defaults: silence 300 s, answer_room on, repeat_stop_tokens 256, unset token bounds");
    const std::string described = secure_frontend::describe(*bare);
    expect(described.find("engine.silence_s = 300\n") != std::string::npos &&
               described.find("engine.answer_room = true\n") != std::string::npos &&
               described.find("engine.repeat_stop_tokens = 256\n") != std::string::npos,
           "describe reports the timeout/shape family");
  }
  const auto accept = [&](const char* path, const char* value) {
    const auto result = with_literals(f, {{path, value}});
    expect(std::holds_alternative<secure_frontend::ValidatedConfig>(result),
           std::string{"accepts "} + value + " for " + path + ": " + issues_of(result));
  };
  const auto reject = [&](const char* path, const char* value) {
    const auto result = with_literals(f, {{path, value}});
    expect(std::holds_alternative<secure_frontend::ConfigErrors>(result), std::string{"rejects "} + value + " for " + path);
  };
  accept("engine.answer_room", "false");
  reject("engine.answer_room", "\"true\"");
  accept("model.aliases", R"(["Alt.Model","secondary"])");
  reject("model.aliases", "[1]");
  reject("model.aliases", R"([""])");        // set_aliases drops empties; the typed row rejects them outright
  reject("model.aliases", R"(["bad name"])");  // a name is [A-Za-z0-9._-], like model ids
  reject("model.aliases", R"(["a:b"])");       // aliases are plain names, not name:tag references
  reject("model.aliases", R"(["dup","dup"])");  // like every SP3/SP4 list row: no repeats (the pin dedups at :3774; Ruling T8-h)
  const auto set_result = with_literals(f, {{"engine.silence_s", "0"}, {"engine.repeat_stop_tokens", "0"},
                                            {"engine.max_tokens", "2048"}, {"engine.min_tokens", "16"},
                                            {"model.aliases", R"(["Alt.Model","secondary"])"}});
  const auto* set = config_of(set_result);
  expect(set != nullptr && set->engine.silence == std::chrono::seconds(0) && set->engine.repeat_stop_tokens == 0U &&
             set->engine.max_tokens.has_value() && *set->engine.max_tokens == 2048 && set->engine.min_tokens.has_value() &&
             *set->engine.min_tokens == 16 && set->engine.aliases.size() == 2U,
         "0 = off for silence/repeat (the pin's), token bounds and aliases typed: " + issues_of(set_result));
}

// SP5 M1 Task 8: edge behaviour of the new rows — the added caps (C-1) accept their max
// and reject one past it; the silence row keeps the pin's num>=0 shape with an added cap.
void timeout_shape_rows_at_each_edge() {
  ConfigFixture f;
  const auto accept = [&](const char* path, const std::string& value) {
    const auto result = with_literals(f, {{path, value.c_str()}});
    expect(std::holds_alternative<secure_frontend::ValidatedConfig>(result),
           std::string{"accepts "} + value + " for " + path + ": " + issues_of(result));
  };
  const auto reject = [&](const char* path, const std::string& value) {
    const auto result = with_literals(f, {{path, value.c_str()}});
    expect(std::holds_alternative<secure_frontend::ConfigErrors>(result), std::string{"rejects "} + value + " for " + path);
  };
  accept("engine.silence_s", "0");
  accept("engine.silence_s", "86400");
  reject("engine.silence_s", "86401");
  reject("engine.silence_s", "-1");  // the pin rejects negatives too (engine_silence_s() :2864)
  accept("engine.repeat_stop_tokens", "0");
  accept("engine.repeat_stop_tokens", "65536");
  reject("engine.repeat_stop_tokens", "65537");
  reject("engine.repeat_stop_tokens", "-1");
  accept("engine.max_tokens", "1");
  accept("engine.max_tokens", "1048576");
  reject("engine.max_tokens", "1048577");
  reject("engine.max_tokens", "0");
  accept("engine.min_tokens", "1");
  reject("engine.min_tokens", "0");
  accept("model.aliases", "[\"" + std::string(64, 'a') + "\"]");
  reject("model.aliases", "[\"" + std::string(65, 'a') + "\"]");
}

// SP5 M1 Task 9 (parity rows slot_save_path / expert_profile_save / expert_profile_save_every /
// tool_call_recovery / env_to_user / codex_compaction_cache / codex_thread_titles / keep_files).
// Defaults verbatim from the pin: the four booleans default false (:8043-8044 `is True`,
// :8106/:8110 strict bool), the expert pair unset with every defaulting to 10 minutes (:2996),
// keep_files 64 (images.py:188 `or 64`).
void session_rows_match_the_pin() {
  ConfigFixture f;
  const auto bare_result = with_literals(f, {});
  const auto* bare = config_of(bare_result);
  expect(bare != nullptr, "a config without the session rows still validates: " + issues_of(bare_result));
  if (bare != nullptr) {
    expect(!bare->session.tool_call_recovery && !bare->session.env_to_user && !bare->session.codex_thread_titles &&
               !bare->session.codex_compaction_cache && bare->session.slot_save_path.empty() &&
               bare->session.expert_profile_save.empty() && bare->session.expert_profile_save_every == 10 &&
               bare->session.keep_files == 64,
           "the pin's defaults: four opt-in falses, unset pair, every 10 (:2996), keep_files 64 (images.py:188)");
    const std::string described = secure_frontend::describe(*bare);
    expect(described.find("session.keep_files = 64\n") != std::string::npos &&
               described.find("session.expert_profile_save_every = 10\n") != std::string::npos &&
               described.find("session.tool_call_recovery = false\n") != std::string::npos,
           "describe reports the session family");
  }
  const auto accept = [&](const char* path, const std::string& value) {
    const auto result = with_literals(f, {{path, value}});
    expect(std::holds_alternative<secure_frontend::ValidatedConfig>(result),
           std::string{"accepts "} + value + " for " + path + ": " + issues_of(result));
  };
  const auto reject = [&](const char* path, const std::string& value) {
    const auto result = with_literals(f, {{path, value}});
    expect(std::holds_alternative<secure_frontend::ConfigErrors>(result), std::string{"rejects "} + value + " for " + path);
  };
  accept("session.tool_call_recovery", "true");
  accept("session.env_to_user", "false");
  reject("session.codex_thread_titles", "\"true\"");  // the pin's strict bool, :8043
  reject("session.codex_compaction_cache", "1");
  accept("session.slot_save_path", quoted(host_absolute("/state/slots")));
  accept("session.expert_profile_save", quoted(host_absolute("/state/learned.bin")));
  const auto set_result = with_literals(f, {{"session.slot_save_path", quoted(host_absolute("/state/slots"))},
                                            {"session.expert_profile_save", quoted(host_absolute("/state/learned.bin"))},
                                            {"session.expert_profile_save_every", "0"},
                                            {"session.tool_call_recovery", "true"}, {"session.env_to_user", "true"},
                                            {"session.codex_thread_titles", "true"}, {"session.codex_compaction_cache", "true"},
                                            {"session.keep_files", "1"}});
  const auto* set = config_of(set_result);
  expect(set != nullptr && set->session.slot_save_path == host_absolute("/state/slots") &&
             set->session.expert_profile_save == host_absolute("/state/learned.bin") &&
             set->session.expert_profile_save_every == 0 && set->session.tool_call_recovery && set->session.env_to_user &&
             set->session.codex_thread_titles && set->session.codex_compaction_cache && set->session.keep_files == 1,
         "the session family is typed: " + issues_of(set_result));
}

// SP5 M1 Task 9 (plan test; Ruling T9-b): the slot path keeps the pin's shape (non-empty, no
// control characters, serve/server.py:3472-3475) and rides our added absolute rule (C-1, like
// engine.lib_dirs :794-797 — we have no config-cwd to resolve a relative path against).
void slot_save_path_is_a_bounded_private_path() {
  ConfigFixture f;
  const auto accept = [&](const char* path, const std::string& value) {
    const auto result = with_literals(f, {{path, value}});
    expect(std::holds_alternative<secure_frontend::ValidatedConfig>(result),
           std::string{"accepts "} + value + " for " + path + ": " + issues_of(result));
  };
  const auto reject = [&](const char* path, const std::string& value) {
    const auto result = with_literals(f, {{path, value}});
    expect(std::holds_alternative<secure_frontend::ConfigErrors>(result), std::string{"rejects "} + value + " for " + path);
  };
  accept("session.slot_save_path", quoted(host_absolute("/state/slots")));
  reject("session.slot_save_path", "\"slots\"");           // no cwd concept (T9-b)
  reject("session.slot_save_path", "\"\"");                // the pin's "must be a directory path" (:3472)
  reject("session.slot_save_path", "\"a\\u0001b\"");       // control characters (:3475)
  accept("session.expert_profile_save", quoted(host_absolute("/state/learned.bin")));
  reject("session.expert_profile_save", "\"learned.bin\"");
  reject("session.expert_profile_save", "\"\"");
}

// SP5 M1 Task 9 (plan test): the rows match the Task-2 table as re-read — the four booleans and
// the two file-store rows are typed/config_only (the pin's consumers are its OutputParser, its
// frontend and the Codex routes, none of which we serve), and the expert pair is typed/engine_arg.
// slot_save_path sits config_only after Ruling T9-a (no argv append at the pin: every use is
// svc-side, :8096/:3698-3715); keep_files after T9-e (ImageService is D-9 out).
// Owner decisions 2026-10-21 (session log step 12): listener.web_ui serves the built-in page;
// listener.loopback_without_key is the one keyless listener — loopback only, no token.source.
// GUI-first start-up (spec 2026-10-27 §5a): email alerts are optional at build time. Built in
// (SECURE_FRONTEND_EMAIL_ALERTS), alerts.email.* is read and checked - the server, the IONOS-style TLS ports and modes,
// the mailbox, 1-5 recipients, the events - with nothing that could break a mail header; built without, every
// alerts.* row is refused by name.
void email_alert_rows_follow_the_build() {
  ConfigFixture f;
  const std::map<std::string, std::string> ionos{{"alerts.email.enabled", "true"},
                                                 {"alerts.email.host", "\"smtp.ionos.com\""},
                                                 {"alerts.email.port", "465"},
                                                 {"alerts.email.security", "\"ssl\""},
                                                 {"alerts.email.username", "\"alerts@example.com\""},
                                                 {"alerts.email.from", "\"alerts@example.com\""},
                                                 {"alerts.email.to", "[\"owner@example.com\"]"},
                                                 {"alerts.email.events", "[\"download_finished\", \"engine_stopped\"]"}};
  const auto result = with_literals(f, ionos);
#if defined(SECURE_FRONTEND_EMAIL_ALERTS)
  const auto* config = config_of(result);
  expect(config != nullptr, "an IONOS account's settings validate: " + issues_of(result));
  if (config != nullptr) {
    const auto& email = config->alerts.email;
    expect(email.enabled && email.host == "smtp.ionos.com" && email.port == 465 && email.security == secure_frontend::SmtpSecurity::ssl &&
               email.username == "alerts@example.com" && email.from == "alerts@example.com" &&
               email.to == std::vector<std::string>{"owner@example.com"} &&
               email.events == std::vector<std::string>{"download_finished", "engine_stopped"},
           "every row is read");
  }
  auto starttls = ionos;
  starttls["alerts.email.port"] = "587";
  starttls["alerts.email.security"] = "\"starttls\"";
  const auto alternative_result = with_literals(f, starttls);
  const auto* alternative = config_of(alternative_result);
  expect(alternative != nullptr && alternative->alerts.email.security == secure_frontend::SmtpSecurity::starttls,
         "IONOS's alternative: 587 with STARTTLS");
  auto no_events = ionos;
  no_events.erase("alerts.email.events");
  const auto every_result = with_literals(f, no_events);
  const auto* every = config_of(every_result);
  expect(every != nullptr && every->alerts.email.events.size() == 6U, "no events named: every alert");
  for (const auto& [row, literal] : std::vector<std::pair<std::string, std::string>>{
           {"alerts.email.security", "\"tls\""},
           {"alerts.email.host", "\"smtp ionos.com\""},
           {"alerts.email.from", "\"alerts@example.com\\r\\nBcc: x@y.z\""},
           {"alerts.email.username", "\"name\\r\\nRCPT TO:<x@y.z>\""},
           {"alerts.email.to", "[]"},
           {"alerts.email.to", "[\"a@b.c\", \"d@e.f\", \"g@h.i\", \"j@k.l\", \"m@n.o\", \"p@q.r\"]"},
           {"alerts.email.events", "[\"everything\"]"},
           {"alerts.email.port", "0"}}) {
    auto bad = ionos;
    bad[row] = literal;
    expect(has_issue(with_literals(f, bad), row, ConfigIssueCode::invalid_value) ||
               has_issue(with_literals(f, bad), row, ConfigIssueCode::out_of_range),
           "refused: " + row + " = " + literal);
  }
  auto missing = ionos;
  missing.erase("alerts.email.host");
  expect(has_issue(with_literals(f, missing), "alerts.email.host", ConfigIssueCode::missing_required), "on needs a server");
  const auto off_result = with_literals(f, {});
  const auto* off = config_of(off_result);
  expect(off != nullptr && !off->alerts.email.enabled, "off by default");
  // the page's settings are checked alone, before they are written to the config file
  secure_frontend::FrontendConfigDraft rows;
  rows.entries["alerts.email.enabled"] = secure_frontend::ConfigDraftEntry{true, secure_frontend::ConfigSource::file};
  rows.entries["alerts.email.host"] = secure_frontend::ConfigDraftEntry{std::string{"smtp.ionos.com"}, secure_frontend::ConfigSource::file};
  rows.entries["alerts.email.username"] = secure_frontend::ConfigDraftEntry{std::string{"a@example.com"}, secure_frontend::ConfigSource::file};
  rows.entries["alerts.email.from"] = secure_frontend::ConfigDraftEntry{std::string{"a@example.com"}, secure_frontend::ConfigSource::file};
  rows.entries["alerts.email.to"] =
      secure_frontend::ConfigDraftEntry{std::vector<std::string>{"b@example.com"}, secure_frontend::ConfigSource::file};
  const auto alone = secure_frontend::alerts_from_draft(rows);
  expect(std::holds_alternative<secure_frontend::AlertsConfig>(alone) &&
             std::get<secure_frontend::AlertsConfig>(alone).email.host == "smtp.ionos.com",
         "the alert rows validate alone");
  rows.entries["alerts.email.from"] = secure_frontend::ConfigDraftEntry{std::string{"not an address"}, secure_frontend::ConfigSource::file};
  expect(std::holds_alternative<secure_frontend::ConfigErrors>(secure_frontend::alerts_from_draft(rows)), "and refuse alone");
#else
  expect(has_issue(result, "alerts.email.enabled", ConfigIssueCode::invalid_value),
         "a build without email alerts refuses the rows by name: " + issues_of(result));
#endif
}

// Web search (spec 2026-10-27-web-search-design §2): optional at build time. Built in (SECURE_FRONTEND_WEB_SEARCH),
// search.* is read and checked - the SearXNG URL (http or https, no user, query or fragment), 1-10 results, 1-8 rounds,
// 5-120 s - and off by default; built without, every search.* row is refused by name.
void web_search_rows_follow_the_build() {
  ConfigFixture f;
  const std::map<std::string, std::string> searxng{{"search.enabled", "true"},
                                                   {"search.url", "\"http://127.0.0.1:8888/searxng/\""},
                                                   {"search.results", "3"},
                                                   {"search.max_rounds", "2"},
                                                   {"search.timeout_s", "20"}};
  const auto result = with_literals(f, searxng);
#if defined(SECURE_FRONTEND_WEB_SEARCH)
  const auto* config = config_of(result);
  expect(config != nullptr, "a SearXNG instance's settings validate: " + issues_of(result));
  if (config != nullptr) {
    const auto& search = config->search;
    expect(search.enabled && search.url == "http://127.0.0.1:8888/searxng/" && search.results == 3U && search.max_rounds == 2U &&
               search.timeout == std::chrono::seconds(20),
           "every row is read");
  }
  for (const auto& [row, literal] : std::vector<std::pair<std::string, std::string>>{
           {"search.url", "\"ftp://127.0.0.1\""},
           {"search.url", "\"http://user@127.0.0.1\""},
           {"search.url", "\"http://127.0.0.1/?q=x\""},
           {"search.url", "\"http://a b\""},
           {"search.url", "\"https://\""},
           {"search.results", "0"},
           {"search.results", "11"},
           {"search.max_rounds", "0"},
           {"search.max_rounds", "9"},
           {"search.timeout_s", "4"},
           {"search.timeout_s", "121"}}) {
    auto bad = searxng;
    bad[row] = literal;
    expect(has_issue(with_literals(f, bad), row, ConfigIssueCode::invalid_value) ||
               has_issue(with_literals(f, bad), row, ConfigIssueCode::out_of_range),
           "refused: " + row + " = " + literal);
  }
  auto missing = searxng;
  missing.erase("search.url");
  expect(has_issue(with_literals(f, missing), "search.url", ConfigIssueCode::missing_required), "on needs an instance");
  const auto off_result = with_literals(f, {});
  const auto* off = config_of(off_result);
  expect(off != nullptr && !off->search.enabled && off->search.results == 5U && off->search.max_rounds == 8U &&
             off->search.timeout == std::chrono::seconds(30),
         "off by default: 5 results, 8 rounds, 30 s");
  const auto https_result = with_literals(f, {{"search.enabled", "true"}, {"search.url", "\"https://search.example.org\""}});
  expect(config_of(https_result) != nullptr, "an https instance validates: " + issues_of(https_result));
  // the page's settings are checked alone, before they are written to the config file
  secure_frontend::FrontendConfigDraft rows;
  rows.entries["search.enabled"] = secure_frontend::ConfigDraftEntry{true, secure_frontend::ConfigSource::file};
  rows.entries["search.url"] = secure_frontend::ConfigDraftEntry{std::string{"http://127.0.0.1:8888"}, secure_frontend::ConfigSource::file};
  const auto alone = secure_frontend::search_from_draft(rows);
  expect(std::holds_alternative<secure_frontend::SearchConfig>(alone) && std::get<secure_frontend::SearchConfig>(alone).enabled,
         "the search rows validate alone");
  rows.entries["search.url"] = secure_frontend::ConfigDraftEntry{std::string{"not a url"}, secure_frontend::ConfigSource::file};
  expect(std::holds_alternative<secure_frontend::ConfigErrors>(secure_frontend::search_from_draft(rows)), "and refuse alone");
#else
  expect(has_issue(result, "search.enabled", ConfigIssueCode::invalid_value) && has_issue(result, "search.url", ConfigIssueCode::invalid_value),
         "a build without web search refuses the rows by name: " + issues_of(result));
#endif
}

void web_ui_and_loopback_without_key_rows() {
  ConfigFixture f;
  const auto bare_result = with_literals(f, {});
  const auto* bare = config_of(bare_result);
  expect(bare != nullptr && !bare->listener.web_ui && !bare->listener.loopback_without_key, "both default to false");
  const std::string described = describe_of(bare_result);
  expect(described.find("listener.web_ui = false\n") != std::string::npos &&
             described.find("listener.loopback_without_key = false\n") != std::string::npos,
         "--check reports both: " + described);
  const auto page_result = with_literals(f, {{"listener.web_ui", "true"}});
  const auto* page = config_of(page_result);
  expect(page != nullptr && page->listener.web_ui && !page->listener.loopback_without_key, "web_ui alone keeps the key");
  expect(std::holds_alternative<secure_frontend::ConfigErrors>(with_literals(f, {{"listener.web_ui", "\"yes\""}})), "a strict boolean");
  for (const char* host : {"\"127.0.0.1\"", "\"::1\"", "\"localhost\""}) {
    const auto keyless_result = with_literals(f, {{"listener.host", host}, {"listener.loopback_without_key", "true"}});
    const auto* keyless = config_of(keyless_result);
    expect(keyless != nullptr && keyless->listener.loopback_without_key,
           std::string{"keyless on a loopback host: "} + host + ": " + issues_of(keyless_result));
  }
  const auto remote = with_literals(f, {{"listener.host", "\"192.0.2.10\""}, {"listener.allow_remote", "true"},
                                        {"listener.loopback_without_key", "true"}});
  expect(has_issue(remote, "listener.loopback_without_key", ConfigIssueCode::conflicting_values),
         "a remote host never runs keyless: " + issues_of(remote));
  const auto wildcard = with_literals(f, {{"listener.host", "\"0.0.0.0\""}, {"listener.allow_remote", "true"},
                                          {"listener.allow_wildcard_bind", "true"}, {"listener.host_aliases", "[\"box:8080\"]"},
                                          {"listener.loopback_without_key", "true"}});
  expect(has_issue(wildcard, "listener.loopback_without_key", ConfigIssueCode::conflicting_values),
         "a wildcard bind never runs keyless: " + issues_of(wildcard));
  const auto named_key = with_literals(f, {{"listener.loopback_without_key", "true"}, {"token.source", "\"env:STRATA_TOKEN\""}});
  expect(has_issue(named_key, "token.source", ConfigIssueCode::conflicting_values),
         "a configured key that would never be read is refused, not ignored: " + issues_of(named_key));
}

// Session log step 16: listener.monitor turns on GET /metrics (the page's Monitor tab). Off by
// default, a strict boolean, config file only, and independent of the page and the key.
void monitor_row() {
  ConfigFixture f;
  const auto bare_result = with_literals(f, {});
  const auto* bare = config_of(bare_result);
  expect(bare != nullptr && !bare->listener.monitor, "listener.monitor defaults to false");
  expect(describe_of(bare_result).find("listener.monitor = false\n") != std::string::npos, "--check reports it");
  const auto on_result = with_literals(f, {{"listener.monitor", "true"}});
  const auto* on = config_of(on_result);
  expect(on != nullptr && on->listener.monitor && !on->listener.web_ui && !on->listener.loopback_without_key,
         "the Monitor alone turns nothing else on: " + issues_of(on_result));
  expect(describe_of(on_result).find("listener.monitor = true\n") != std::string::npos, "--check reports it on");
  expect(std::holds_alternative<secure_frontend::ConfigErrors>(with_literals(f, {{"listener.monitor", "1"}})), "a strict boolean");
  const auto schema = secure_frontend::config_schema();
  const auto row = std::find_if(schema.begin(), schema.end(),
                                [](const secure_frontend::ConfigKey& key) { return key.path == "listener.monitor"; });
  expect(row != schema.end() && row->flag.empty() && row->type == secure_frontend::ConfigValueType::boolean && !row->default_boolean,
         "a schema row: a boolean, false, config file only (no flag)");
}

// Session log step 13: model.name, the served name (the pin's model_name), in the alias grammar.
void model_name_row() {
  ConfigFixture f;
  const auto bare_result = with_literals(f, {});
  const auto* bare = config_of(bare_result);
  expect(bare != nullptr && !bare->engine.served_name.has_value(), "unset by default: the model's own id is served");
  expect(describe_of(bare_result).find("model.name = (unset)\n") != std::string::npos, "--check reports it unset");
  const auto named_result = with_literals(f, {{"model.name", "\"qwen3.8-flash-next\""}});
  const auto* named = config_of(named_result);
  expect(named != nullptr && named->engine.served_name == std::optional<std::string>{"qwen3.8-flash-next"},
         "a name in the alias grammar is kept: " + issues_of(named_result));
  expect(describe_of(named_result).find("model.name = \"qwen3.8-flash-next\"\n") != std::string::npos, "--check reports it");
  for (const char* bad : {"\"\"", "\"has space\"", "\"a/b\"", "\"-lead\"", "1"}) {
    expect(has_issue(with_literals(f, {{"model.name", bad}}), "model.name", ConfigIssueCode::invalid_value) ||
               has_issue(with_literals(f, {{"model.name", bad}}), "model.name", ConfigIssueCode::wrong_type),
           std::string{"refused: "} + bad);
  }
  expect(has_issue(with_literals(f, {{"model.name", "\"" + std::string(65, 'a') + "\""}}), "model.name", ConfigIssueCode::invalid_value),
         "at most 64 characters, like an alias");
}

// Model launcher (plan 2026-10-23 Task 1): models.cards (the card files), models.start (the card
// started at boot), models.control and models.control_remote (who may start and stop, D2).
void models_keys_typed_and_described() {
  ConfigFixture f;
  struct Row {
    const char* path;
    ConfigValueType type;
  };
  for (const Row row : {Row{"models.cards", ConfigValueType::string_array}, Row{"models.start", ConfigValueType::string},
                        Row{"models.control", ConfigValueType::boolean}, Row{"models.control_remote", ConfigValueType::boolean}}) {
    const auto* key = secure_frontend::find_config_key(row.path);
    expect(key != nullptr && key->type == row.type && !key->required && key->flag.empty() && !key->default_boolean,
           std::string{"a config-file row with its type, off by default: "} + row.path);
  }
  const auto bare_result = with_literals(f, {});
  const auto* bare = config_of(bare_result);
  expect(bare != nullptr && bare->models.cards.empty() && !bare->models.start.has_value() && !bare->models.control &&
             !bare->models.control_remote,
         "no cards, no start, control off by default: " + issues_of(bare_result));
  const std::string described = describe_of(bare_result);
  for (const char* line : {"models.cards = []\n", "models.start = (unset)\n", "models.control = false\n", "models.control_remote = false\n"}) {
    expect(described.find(line) != std::string::npos, std::string{"--check reports "} + line);
  }
  const std::string card_a = host_absolute("/srv/cards/qwen.json");
  const std::string card_b = host_absolute("/srv/cards/gemma.json");
  const auto on_result = with_literals(f, {{"models.cards", string_array({card_a, card_b})}, {"models.start", "\"Gemma-4\""},
                                           {"models.control", "true"}, {"models.control_remote", "true"}});
  const auto* on = config_of(on_result);
  expect(on != nullptr && on->models.cards.size() == 2U && on->models.cards[0] == secure_frontend::path_from_utf8(card_a) &&
             on->models.start == std::optional<std::string>{"Gemma-4"} && on->models.control && on->models.control_remote,
         "the rows are kept: " + issues_of(on_result));
  expect(describe_of(on_result).find("models.control = true\n") != std::string::npos, "--check reports the switch on");
  expect(has_issue(with_literals(f, {{"models.cards", string_array({"cards/qwen.json"})}}), "models.cards", ConfigIssueCode::invalid_value),
         "a card path is absolute");
  expect(has_issue(with_literals(f, {{"models.cards", string_array({card_a, card_a})}}), "models.cards", ConfigIssueCode::invalid_value),
         "a card is listed once");
  std::vector<std::string> seventeen;
  for (int i = 0; i < 17; ++i) seventeen.push_back(host_absolute("/srv/cards/" + std::to_string(i) + ".json"));
  expect(has_issue(with_literals(f, {{"models.cards", string_array(seventeen)}}), "models.cards", ConfigIssueCode::out_of_range),
         "at most 16 cards");
  expect(has_issue(with_literals(f, {{"models.control", "true"}}), "models.control", ConfigIssueCode::conflicting_values),
         "models.control needs models.cards");
  expect(has_issue(with_literals(f, {{"models.start", "\"Gemma-4\""}}), "models.start", ConfigIssueCode::conflicting_values),
         "models.start needs models.cards");
  expect(has_issue(with_literals(f, {{"models.cards", string_array({card_a})}, {"models.control_remote", "true"}}),
                   "models.control_remote", ConfigIssueCode::conflicting_values),
         "models.control_remote needs models.control");
  expect(has_issue(with_literals(f, {{"models.cards", string_array({card_a})}, {"models.start", "\"has space\""}}), "models.start",
                   ConfigIssueCode::invalid_value),
         "models.start is a model name");
  // The launcher starts and stops engines itself: the lifecycle rows have no meaning beside it and are refused by
  // name. (Vision for cards, spec 2026-10-28: vision.enabled, vision.mmproj and vision.gpu are a card's own rows now; a
  // main config that sets them beside models.cards is refused as card-owned rows - model_cards_test's
  // card_refusals_name_the_rule and the launcher's bad_card_refused_at_boot_and_main_rows_refused.)
  for (const auto& [row, literal] : std::vector<std::pair<std::string, std::string>>{
           {"engine.lazy_load", "true"}, {"engine.idle_unload_s", "30"}}) {
    expect(has_issue(with_literals(f, {{"models.cards", string_array({card_a})}, {row, literal}}), row,
                     ConfigIssueCode::conflicting_values),
           "refused with models.cards: " + row);
  }
}

// models.control_remote_networks (owner 2026-10-27: "add 172.16.1.0/24 and 172.16.2.0/24 to the control_remote", "it
// should allow hosts list or subnets"): the hosts and subnets other machines may control models from.
void control_remote_networks_typed_and_checked() {
  ConfigFixture f;
  const auto* key = secure_frontend::find_config_key("models.control_remote_networks");
  expect(key != nullptr && key->type == ConfigValueType::string_array && !key->required && key->flag.empty(),
         "a config-file string array");
  const auto bare_result = with_literals(f, {});
  const auto* bare = config_of(bare_result);
  expect(bare != nullptr && bare->models.control_remote_networks.empty(), "empty by default: " + issues_of(bare_result));
  expect(describe_of(bare_result).find("models.control_remote_networks = []\n") != std::string::npos, "--check reports the empty list");
  const std::string card = host_absolute("/srv/cards/qwen.json");
  const auto with_networks = [&](const std::vector<std::string>& networks, const bool remote) {
    std::map<std::string, std::string> rows{{"models.cards", string_array({card})}, {"models.control", "true"},
                                            {"models.control_remote_networks", string_array(networks)}};
    if (remote) rows.emplace("models.control_remote", "true");
    return with_literals(f, rows);
  };
  const auto on_result = with_networks({"172.16.1.0/24", "172.16.2.0/24", "10.1.2.3"}, true);
  const auto* on = config_of(on_result);
  expect(on != nullptr && on->models.control_remote_networks.size() == 3U && on->models.control_remote_networks[0].prefix == 24U &&
             on->models.control_remote_networks[2].prefix == 32U,
         "two subnets and a host are kept: " + issues_of(on_result));
  expect(describe_of(on_result).find("models.control_remote_networks = [\"172.16.1.0/24\",\"172.16.2.0/24\",\"10.1.2.3\"]\n") !=
             std::string::npos,
         "--check reports them: " + describe_of(on_result));
  for (const std::string& bad : std::vector<std::string>{"172.16.1.5/24", "myhost", "172.16.1.0/33", "localhost"}) {
    expect(has_issue(with_networks({bad}, true), "models.control_remote_networks", ConfigIssueCode::invalid_value), "refused: " + bad);
  }
  expect(has_issue(with_networks({"172.16.1.0/24"}, false), "models.control_remote_networks", ConfigIssueCode::conflicting_values),
         "the list needs models.control_remote");
}

// Model downloads (plan 2026-10-23-model-downloads Task 4): the download switch, the store root,
// and the prepare step's Python and upstream tools; the card a prepared Qwen download borrows its
// engine from.
void download_keys_typed_and_described() {
  ConfigFixture f;
  for (const char* path : {"models.downloads", "models.downloads_root", "models.prepare_python", "models.prepare_tools",
                           "models.download_engine_card"}) {
    const auto* key = secure_frontend::find_config_key(path);
    expect(key != nullptr && !key->required && key->flag.empty() && !key->default_boolean, std::string{"a config-file row: "} + path);
  }
  const auto bare = with_literals(f, {});
  const auto* config = config_of(bare);
  expect(config != nullptr && !config->models.downloads && !config->models.downloads_root.has_value() &&
             !config->models.prepare_python.has_value() && !config->models.prepare_tools.has_value() &&
             !config->models.download_engine_card.has_value(),
         "off and unset by default: " + issues_of(bare));
  for (const char* line : {"models.downloads = false\n", "models.downloads_root = (unset)\n", "models.prepare_python = (unset)\n",
                           "models.prepare_tools = (unset)\n", "models.download_engine_card = (unset)\n"}) {
    expect(describe_of(bare).find(line) != std::string::npos, std::string{"--check reports "} + line);
  }
  const std::string cards = string_array({host_absolute("/srv/cards/qwen.json")});
  const std::string root = secure_frontend_test::json_literal(host_absolute("/srv/downloads"));
  const std::string python = secure_frontend_test::json_literal(host_absolute("/srv/venv/python.exe"));
  const std::string tools = secure_frontend_test::json_literal(host_absolute("/srv/upstream/tools"));
  const auto on = with_literals(f, {{"models.cards", cards}, {"models.downloads", "true"}, {"models.downloads_root", root},
                                    {"models.prepare_python", python}, {"models.prepare_tools", tools},
                                    {"models.download_engine_card", "\"Qwen3.8-Flash-Next\""}});
  const auto* with = config_of(on);
  expect(with != nullptr && with->models.downloads && with->models.downloads_root.has_value() &&
             with->models.prepare_python.has_value() && with->models.prepare_tools.has_value() &&
             with->models.download_engine_card == std::optional<std::string>{"Qwen3.8-Flash-Next"},
         "the rows are kept: " + issues_of(on));
  expect(has_issue(with_literals(f, {{"models.downloads", "true"}}), "models.downloads", ConfigIssueCode::conflicting_values),
         "downloads need models.cards");
  expect(has_issue(with_literals(f, {{"models.cards", cards}, {"models.downloads", "true"}}), "models.downloads",
                   ConfigIssueCode::conflicting_values),
         "downloads need a store root");
  expect(has_issue(with_literals(f, {{"models.cards", cards}, {"models.downloads_root", "\"downloads\""}}), "models.downloads_root",
                   ConfigIssueCode::invalid_value),
         "the store root is absolute");
  expect(has_issue(with_literals(f, {{"models.cards", cards}, {"models.prepare_python", python}}), "models.prepare_python",
                   ConfigIssueCode::conflicting_values),
         "the prepare step needs both its Python and the tools");
  expect(has_issue(with_literals(f, {{"models.cards", cards}, {"models.prepare_tools", "\"tools\""}, {"models.prepare_python", python}}),
                   "models.prepare_tools", ConfigIssueCode::invalid_value),
         "the tools folder is absolute");
  expect(has_issue(with_literals(f, {{"models.cards", cards}, {"models.download_engine_card", "\"has space\""}}),
                   "models.download_engine_card", ConfigIssueCode::invalid_value),
         "the engine card is a card name");
}

void codex_and_tool_call_rows_are_dispositioned() {
  struct Expected { const char* key; const char* path; };
  for (const Expected e : {Expected{"slot_save_path", "session.slot_save_path"},
                           Expected{"expert_profile_save", "session.expert_profile_save"},
                           Expected{"expert_profile_save_every", "session.expert_profile_save_every"},
                           Expected{"tool_call_recovery", "session.tool_call_recovery"},
                           Expected{"env_to_user", "session.env_to_user"},
                           Expected{"codex_compaction_cache", "session.codex_compaction_cache"},
                           Expected{"codex_thread_titles", "session.codex_thread_titles"},
                           Expected{"keep_files", "session.keep_files"}}) {
    const auto* row = secure_frontend::parity::strata_parity_row(e.key);
    expect(row != nullptr && row->disposition == secure_frontend::parity::ParityDisposition::typed,
           std::string{e.key} + " stays a typed row in the table");
    expect(secure_frontend::find_config_key(e.path) != nullptr, std::string{e.path} + " is a schema row");
  }
  using ParityDestination = secure_frontend::parity::ParityDestination;
  expect(secure_frontend::parity::strata_parity_row("slot_save_path")->destination == ParityDestination::config_only,
         "slot_save_path: config_only (Ruling T9-a)");
  expect(secure_frontend::parity::strata_parity_row("keep_files")->destination == ParityDestination::config_only,
         "keep_files: config_only (Ruling T9-e)");
  expect(secure_frontend::parity::strata_parity_row("expert_profile_save")->destination == ParityDestination::engine_arg &&
             secure_frontend::parity::strata_parity_row("expert_profile_save_every")->destination == ParityDestination::engine_arg,
         "the expert pair reaches the engine argv (:3002-3005)");
  for (const char* key : {"tool_call_recovery", "env_to_user", "codex_compaction_cache", "codex_thread_titles"})
    expect(secure_frontend::parity::strata_parity_row(key)->destination == ParityDestination::config_only,
           std::string{key} + ": typed, stored, no counterpart consumer in our stack");
}

// SP5 M1 Task 9: edge behaviour — every keeps the pin's >=0 with an added cap (1440 minutes =
// a day), keep_files gets an added 1..1024 (the pin's falsy 0 == unset is not copied, cf T7-h).
void session_rows_at_each_edge() {
  ConfigFixture f;
  const auto accept = [&](const char* path, const std::string& value) {
    const auto result = with_literals(f, {{path, value}});
    expect(std::holds_alternative<secure_frontend::ValidatedConfig>(result),
           std::string{"accepts "} + value + " for " + path + ": " + issues_of(result));
  };
  const auto reject = [&](const char* path, const std::string& value) {
    const auto result = with_literals(f, {{path, value}});
    expect(std::holds_alternative<secure_frontend::ConfigErrors>(result), std::string{"rejects "} + value + " for " + path);
  };
  accept("session.expert_profile_save_every", "0");
  accept("session.expert_profile_save_every", "1440");
  reject("session.expert_profile_save_every", "1441");
  reject("session.expert_profile_save_every", "-1");
  accept("session.keep_files", "1");
  accept("session.keep_files", "1024");
  reject("session.keep_files", "0");
  reject("session.keep_files", "1025");
}

// SP5 M1 Task 10 (parity row parallel; serve/server.py:2955-2975): the typed half of the
// C-4 narrowing. replicas.parallel defaults to 1 — one request at a time (the pin's
// docstring, :2957-2958) — with the added 1..1024 cap taken from the pin's OWN --parallel
// bound (:7845; C-1). 2..8 is the engine's full batch window (PARALLEL_MAX :2954); 9..1024
// stay pin-valid: the pin forwards them and the engine warns and adjusts (:2969-2974).
void replicas_parallel_row_at_the_pin() {
  ConfigFixture f;
  const auto bare_result = with_literals(f, {});
  const auto* bare = config_of(bare_result);
  expect(bare != nullptr, "a config without the parallel row still validates: " + issues_of(bare_result));
  if (bare != nullptr) {
    expect(bare->replicas.parallel == 1, "default 1: one request at a time, like the pin (:2957)");
    expect(secure_frontend::describe(*bare).find("replicas.parallel = 1\n") != std::string::npos, "describe reports it");
  }
  const auto accept = [&](const char* path, const std::string& value) {
    const auto result = with_literals(f, {{path, value}});
    expect(std::holds_alternative<secure_frontend::ValidatedConfig>(result),
           std::string{"accepts "} + value + " for " + path + ": " + issues_of(result));
  };
  const auto reject = [&](const char* path, const std::string& value) {
    const auto result = with_literals(f, {{path, value}});
    expect(std::holds_alternative<secure_frontend::ConfigErrors>(result), std::string{"rejects "} + value + " for " + path);
  };
  accept("replicas.parallel", "2");
  accept("replicas.parallel", "8");   // PARALLEL_MAX: the full engine window (:2954)
  accept("replicas.parallel", "1024");
  reject("replicas.parallel", "1025");
  reject("replicas.parallel", "0");   // one-at-a-time is 1/absent (the pin's), not a typed zero
  reject("replicas.parallel", "-1");
}

// SP5 M1 Task 10 (plan test): the N-child half — the replicas dict (serve/replicas.py:44)
// and its replica_* members fail at load BY NAME (Task 3's rule-first scan already covers
// every rejected row; this names the replicas family and checks the fixed reason points at
// the README's narrowing sentence, per the plan's step 2).
void multi_child_replicas_reject_with_the_ruling() {
  ConfigFixture f;
  const auto dict = with_literals(f, {{"replicas", R"({"count":4})"}});
  expect(has_issue(dict, "replicas", secure_frontend::ConfigIssueCode::excluded_key),
         "the replicas dict fails at load naming the key: " + issues_of(dict));
  expect(issues_of(dict).find("README") != std::string::npos,
         "the rejection reason cites README where C-4 is written down: " + issues_of(dict));
  for (const char* key : {"replica_cpus", "replica_load_tokens"}) {
    const auto result = with_literals(f, {{key, "2"}});
    expect(has_issue(result, std::string{key}, secure_frontend::ConfigIssueCode::excluded_key),
           std::string{key} + " is rejected by name (the C-4 narrowing): " + issues_of(result));
  }
}

// SP5 M1 Task 11 (D-9): the seven vision rows at the pin — enabled gates the whole family;
// max_side/max_n default at the images-block values (serve/images.py:102-103 `or 2048`/`or 4`,
// caps added C-1); need_mib unset (images.py:156 override; the VRAM gate is owner-box, cf
// T7-c); no_flash_attn/resident per the spec family list (resident = the pin's eager encoder,
// server.py:2511/:2552); threads 0 = the pin's unset (truthy -> --threads, :2562).
void vision_rows_match_the_pin() {
  ConfigFixture f;
  const auto bare_result = with_literals(f, {});
  const auto* bare = config_of(bare_result);
  expect(bare != nullptr, "a config without the vision rows still validates: " + issues_of(bare_result));
  if (bare != nullptr) {
    expect(!bare->vision.enabled && bare->vision.max_side == 2048 && bare->vision.max_n == 4 &&
               !bare->vision.need_mib.has_value() && !bare->vision.no_flash_attn && bare->vision.resident &&
               bare->vision.threads == 0 && bare->vision.ready_timeout_s == 300,
           "defaults at the pin: off, 2048, 4, unset, off, resident, unset, 300 s (:2511-2562, :327; images.py:102-103,156)");
    const std::string described = secure_frontend::describe(*bare);
    expect(described.find("vision.enabled = false\n") != std::string::npos &&
               described.find("vision.max_side = 2048\n") != std::string::npos &&
               described.find("vision.resident = true\n") != std::string::npos,
           "describe reports the vision family");
  }
  const auto accept = [&](const char* path, const std::string& value) {
    const auto result = with_literals(f, {{path, value}});
    expect(std::holds_alternative<secure_frontend::ValidatedConfig>(result),
           std::string{"accepts "} + value + " for " + path + ": " + issues_of(result));
  };
  const auto reject = [&](const char* path, const std::string& value) {
    const auto result = with_literals(f, {{path, value}});
    expect(std::holds_alternative<secure_frontend::ConfigErrors>(result), std::string{"rejects "} + value + " for " + path);
  };
  accept("vision.enabled", "true");
  reject("vision.enabled", "\"true\"");   // strict booleans, like every boolean row
  accept("vision.max_side", "1");
  accept("vision.max_side", "16384");     // added cap (C-1)
  reject("vision.max_side", "16385");
  reject("vision.max_side", "0");
  accept("vision.max_n", "1");
  accept("vision.max_n", "64");           // added cap (C-1)
  reject("vision.max_n", "65");
  accept("vision.need_mib", "1048576");
  reject("vision.need_mib", "0");
  reject("vision.need_mib", "-1");
  accept("vision.threads", "0");
  accept("vision.threads", "256");        // added cap (C-1)
  reject("vision.threads", "257");
  accept("vision.ready_timeout_s", "0");     // 0: wait for ever — the pin's :291 rule
  accept("vision.ready_timeout_s", "86400"); // added cap (C-1)
  reject("vision.ready_timeout_s", "86401");
  const auto set_result = with_literals(f, {{"vision.enabled", "true"}, {"vision.max_side", "1024"}, {"vision.max_n", "2"},
                                            {"vision.need_mib", "1126"}, {"vision.no_flash_attn", "true"},
                                            {"vision.resident", "false"}, {"vision.threads", "8"},
                                            {"vision.ready_timeout_s", "45"}});
  const auto* set = config_of(set_result);
  expect(set != nullptr && set->vision.enabled && set->vision.max_side == 1024 && set->vision.max_n == 2 &&
             set->vision.need_mib == 1126 && set->vision.no_flash_attn && !set->vision.resident && set->vision.threads == 8 &&
             set->vision.ready_timeout_s == 45,
         "the vision family is typed: " + issues_of(set_result));
}
// SP7 M2 Task 3 (spec D-10): the typed `update.source` key — release mirrors,
// HTTPS-only entries, the standard list cap, empty default, --check row.
void cfg_update_source_typed() {
  ConfigFixture f;
  const auto* row = secure_frontend::find_config_key("update.source");
  expect(row != nullptr && row->type == ConfigValueType::string_array, "update.source is a typed string array");
  const auto defaults = with_literals(f, {});
  const auto* validated = std::get_if<secure_frontend::ValidatedConfig>(&defaults);
  expect(validated != nullptr, "defaults validate: " + issues_of(defaults));
  if (validated != nullptr) {
    expect(validated->get().update.source.empty(), "update.source defaults to empty");
    const std::string text = secure_frontend::describe(validated->get());
    expect(text.find("update.source = []") != std::string::npos, "--check prints update.source");
  }
  const auto ok = with_literals(
      f, {{"update.source", string_array({"https://releases.example/strata/release.env", "https://mirror.example/release.env"})}});
  const auto* good = std::get_if<secure_frontend::ValidatedConfig>(&ok);
  expect(good != nullptr, "two HTTPS mirrors validate: " + issues_of(ok));
  if (good != nullptr) {
    expect(good->get().update.source.size() == 2 && good->get().update.source[0] == "https://releases.example/strata/release.env",
           "mirrors preserved verbatim, in order");
  }
  expect(has_issue(with_literals(f, {{"update.source", string_array({"http://insecure.example/release.env"})}}), "update.source",
                   ConfigIssueCode::invalid_value), "plain HTTP mirrors are rejected");
  expect(has_issue(with_literals(f, {{"update.source", string_array({"https://user@releases.example/release.env"})}}),
                   "update.source", ConfigIssueCode::invalid_value), "userinfo is rejected");
  expect(has_issue(with_literals(f, {{"update.source", string_array({"https://releases.example"})}}), "update.source",
                   ConfigIssueCode::invalid_value), "host without a path is rejected");
  std::vector<std::string> seventeen;
  for (int i = 0; i < 17; ++i) seventeen.push_back("https://mirror" + std::to_string(i) + ".example/release.env");
  expect(has_issue(with_literals(f, {{"update.source", string_array(seventeen)}}), "update.source", ConfigIssueCode::out_of_range),
         "17 mirrors hit the standard list cap");
}
}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"cfg01_engine_profile_rejected_for_managed_model", cfg01_engine_profile_rejected_for_managed_model},
      {"cfg01_tokenizer_pack_root_rejected_for_managed_model", cfg01_tokenizer_pack_root_rejected_for_managed_model},
      {"cfg01_model_reference_rejected_for_managed_model", cfg01_model_reference_rejected_for_managed_model},
      {"describe_reports_unset_expert_keys", describe_reports_unset_expert_keys},
      {"each_rejected_upstream_key_errors_at_load_naming_it", each_rejected_upstream_key_errors_at_load_naming_it},
      {"sampling_defaults_cover_the_pinned_validator", sampling_defaults_cover_the_pinned_validator},
      {"reasoning_rows_match_the_pinned_validator", reasoning_rows_match_the_pinned_validator},
      {"glm_keys_parse_with_mayas_kinds_and_defaults", glm_keys_parse_with_mayas_kinds_and_defaults},
      {"glm_keys_rejected_on_strata_profile_by_name", glm_keys_rejected_on_strata_profile_by_name},
      {"engine_profile_accepts_gemma4_and_describes_it", engine_profile_accepts_gemma4_and_describes_it},
      {"glm_keys_rejected_on_gemma4_profile_by_name", glm_keys_rejected_on_gemma4_profile_by_name},
      {"qwen_launch_rows_rejected_on_gemma4_profile_by_name", qwen_launch_rows_rejected_on_gemma4_profile_by_name},
      {"every_profile_selects_its_own_argument_profile", every_profile_selects_its_own_argument_profile},
      {"engine_profile_accepts_flint_and_refuses_qwen_rows", engine_profile_accepts_flint_and_refuses_qwen_rows},
      {"thinking_budget_default_is_mayas_32768", thinking_budget_default_is_mayas_32768},
      {"glm_environment_projection_moves_the_split_default", glm_environment_projection_moves_the_split_default},
      {"glm_max_context_typed_and_glm53_args_refused", glm_max_context_typed_and_glm53_args_refused},
      {"glm_ram_headroom_gb_typed_and_always_projected", glm_ram_headroom_gb_typed_and_always_projected},
      {"glm_cpu_lane_rows_project_when_set", glm_cpu_lane_rows_project_when_set},
      {"glm_kv_is_a_closed_choice", glm_kv_is_a_closed_choice},
      {"glm_idle_unload_s_wins_for_the_glm_profile", glm_idle_unload_s_wins_for_the_glm_profile},
      {"glm_vision_flags_reach_both_children", glm_vision_flags_reach_both_children},
      {"lifecycle_rows_match_the_pin", lifecycle_rows_match_the_pin},
      {"vram_family_rows_match_the_pin", vram_family_rows_match_the_pin},
      {"timeout_shape_rows_match_the_pin", timeout_shape_rows_match_the_pin},
      {"timeout_shape_rows_at_each_edge", timeout_shape_rows_at_each_edge},
      {"session_rows_match_the_pin", session_rows_match_the_pin},
      {"slot_save_path_is_a_bounded_private_path", slot_save_path_is_a_bounded_private_path},
      {"web_ui_and_loopback_without_key_rows", web_ui_and_loopback_without_key_rows},
      {"email_alert_rows_follow_the_build", email_alert_rows_follow_the_build},
      {"web_search_rows_follow_the_build", web_search_rows_follow_the_build},
      {"model_name_row", model_name_row},
      {"models_keys_typed_and_described", models_keys_typed_and_described},
      {"control_remote_networks_typed_and_checked", control_remote_networks_typed_and_checked},
      {"download_keys_typed_and_described", download_keys_typed_and_described},
      {"monitor_row", monitor_row},
      {"codex_and_tool_call_rows_are_dispositioned", codex_and_tool_call_rows_are_dispositioned},
      {"session_rows_at_each_edge", session_rows_at_each_edge},
      {"replicas_parallel_row_at_the_pin", replicas_parallel_row_at_the_pin},
      {"multi_child_replicas_reject_with_the_ruling", multi_child_replicas_reject_with_the_ruling},
      {"vision_rows_match_the_pin", vision_rows_match_the_pin},
      {"cfg_update_source_typed", cfg_update_source_typed},
      {"bounds_are_the_pins_at_each_edge", bounds_are_the_pins_at_each_edge},
      {"typed_config_accepts_minimal_file_and_applies_defaults", typed_config_accepts_minimal_file_and_applies_defaults},
      {"cfg01_unknown_key_rejected", cfg01_unknown_key_rejected},
      {"cfg01_duplicate_key_rejected", cfg01_duplicate_key_rejected},
      {"cfg01_type_confusion_rejected", cfg01_type_confusion_rejected},
      {"cfg01_missing_required_rejected", cfg01_missing_required_rejected},
      {"typed_config_model_reference_accepts_name_tag", typed_config_model_reference_accepts_name_tag},
      {"cfg01_out_of_range_rejected", cfg01_out_of_range_rejected},
      {"cfg01_chat_ranges_match_enforced_caps", cfg01_chat_ranges_match_enforced_caps},
      {"typed_config_cross_field_rules", typed_config_cross_field_rules},
      {"typed_config_errors_never_contain_values", typed_config_errors_never_contain_values},
      {"engine_lib_dirs_must_be_absolute_directories", engine_lib_dirs_must_be_absolute_directories},
      {"cfg02_logs_and_cache_roots_are_created_private", cfg02_logs_and_cache_roots_are_created_private},
      {"cfg06_config_has_no_secret_typed_member", cfg06_config_has_no_secret_typed_member},
      {"cfg06_validated_config_bytes_do_not_contain_token", cfg06_validated_config_bytes_do_not_contain_token},
      {"readme_config_table_matches_schema", readme_config_table_matches_schema},
      {"cfg01_pull_keys_table_driven", cfg01_pull_keys_table_driven},
      {"cfg01_sp3_rows_type_and_range", cfg01_sp3_rows_type_and_range},
      {"cfg01_header_deadline_le_request_deadline", cfg01_header_deadline_le_request_deadline},
      {"cfg01_response_deadline_ge_write_stall", cfg01_response_deadline_ge_write_stall},
      {"cfg01_wildcard_bind_requires_host_aliases", cfg01_wildcard_bind_requires_host_aliases},
      {"cfg01_host_alias_grammar_and_count", cfg01_host_alias_grammar_and_count},
      {"web05_cors_origin_config_rejects_wildcard_null_path_duplicate_and_17_entries",
       web05_cors_origin_config_rejects_wildcard_null_path_duplicate_and_17_entries},
      {"cfg01_sp3_concurrency_rows_and_defaults", cfg01_sp3_concurrency_rows_and_defaults},
      {"cfg01_queue_plus_two_le_workers", cfg01_queue_plus_two_le_workers},
      {"cfg01_serial_requires_zero_queue", cfg01_serial_requires_zero_queue},
      {"cfg01_remote_cap_le_workers", cfg01_remote_cap_le_workers},
      {"cfg01_sp1_config_with_one_worker_still_valid", cfg01_sp1_config_with_one_worker_still_valid},
      {"cfg01_derived_defaults_follow_worker_count", cfg01_derived_defaults_follow_worker_count},
      {"cfg01_web_ui_raises_the_remote_cap", cfg01_web_ui_raises_the_remote_cap},
      {"cfg01_explicit_values_are_not_adjusted", cfg01_explicit_values_are_not_adjusted},
#if !defined(_WIN32)
      {"cfg02_env_token_ignores_config_folder_permissions", cfg02_env_token_ignores_config_folder_permissions},
      {"cfg02_file_token_default_secret_root_reports_one_issue", cfg02_file_token_default_secret_root_reports_one_issue},
      {"cfg01_group_writable_config_rejected", cfg01_group_writable_config_rejected},
#else
      {"cfg01_config_with_everyone_write_ace_rejected", cfg01_config_with_everyone_write_ace_rejected},
#endif
  }, failures);
}
