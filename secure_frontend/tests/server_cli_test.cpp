#include "secure_frontend/frontend_config.hpp"
#include "secure_frontend/server_cli.hpp"
#include "secure_frontend/token_reader.hpp"
#include "support/acquire_fixtures.hpp"  // SP5 M1 Task 13: the managed model for `strata serve`
#include "support/server_fixture.hpp"
#include "support/test_support.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#if !defined(_WIN32)
#include <csignal>
#include <sys/types.h>
#endif

namespace {
int failures = 0;
void expect(const bool condition, const std::string& message) {
  if (!condition) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
}
using secure_frontend_test::contains;
using secure_frontend_test::ServerFixture;
// SP5 M1 Task 13 (the serve cases): the fixture vocabulary this file did not need before.
using secure_frontend_test::ChildProcess;
using secure_frontend_test::json_literal;
using secure_frontend_test::kTestTokenVariable;
using secure_frontend_test::render_config_json;
using secure_frontend_test::write_file;

std::vector<std::string> with(std::vector<std::string> base, const std::vector<std::string>& extra) {
  base.insert(base.end(), extra.begin(), extra.end());
  return base;
}

std::vector<std::string> without_port(std::vector<std::string> flags) {
  for (std::size_t i = 0; i + 1 < flags.size(); ++i) {
    if (flags[i] == "--port") { flags.erase(flags.begin() + static_cast<long>(i), flags.begin() + static_cast<long>(i) + 2); break; }
  }
  return flags;
}

void cli_config_file_and_flags_share_validator() {
  ServerFixture f;
  auto literals = f.base_literals();
  literals["listener.port"] = "0";
  f.write_config(literals);
  const auto from_file = f.run({"--check", "--expert", "--config", f.config.string()});
  const auto from_flag = f.run(with(without_port(f.base_flags()), {"--check", "--port", "0"}));
  const std::string expected = "listener.port: must be between 1 and 65535";
  expect(from_file.exit_code != 0 && contains(from_file.err, expected), "file value is validated: " + from_file.err);
  expect(from_flag.exit_code != 0 && contains(from_flag.err, expected), "flag value is validated: " + from_flag.err);
}

void cli_flag_overrides_file_and_is_reported() {
  ServerFixture f;
  f.write_config(f.base_literals());
  const std::string other_port = std::to_string(f.port == 65535 ? 65534 : f.port + 1);
  const auto result = f.run({"--check", "--expert", "--config", f.config.string(), "--port", other_port});
  expect(result.exit_code == 0, "a valid file plus a flag passes --check: " + result.err);
  expect(contains(result.out, "listener.port = " + other_port), "the flag wins: " + result.out);
  expect(contains(result.out, "listener.port: the command line overrides the config file"),
         "the override is reported, never silent: " + result.out);
}

void cli_check_prints_no_token() {
  ServerFixture f;
  f.write_config(f.base_literals());
  const auto before = secure_frontend_test::snapshot_tree(f.dir);
  const auto result = f.run({"--check", "--expert", "--config", f.config.string()});
  expect(result.exit_code == 0, "--check on a valid config exits 0: " + result.err);
  expect(secure_frontend_test::snapshot_tree(f.dir) == before, "--check has no side effects below the fixture");
  expect(contains(result.out, "token.source = \"env:SF_TEST_TOKEN\""), "the token source is shown as metadata");
  const auto available = result.out.find("token: available (value not shown)");
  const auto verdict = result.out.find("Configuration is valid.");
  expect(available != std::string::npos && verdict != std::string::npos && available < verdict,
         "--check confirms the token is readable, then gives the verdict: " + result.out);
  expect(!contains(result.out, secure_frontend_test::kTestToken) && !contains(result.err, secure_frontend_test::kTestToken),
         "--check never prints the token");
}

// F3: the verdict follows the token read; an unreadable token is never
// preceded by "Configuration is valid.".
void cli_check_unreadable_token_is_not_reported_valid() {
  ServerFixture f;
  f.write_config(f.base_literals());
  const std::string short_token = "short-token-value";
  const auto result = secure_frontend_test::run_to_completion(secure_frontend_test::ProcessSpec{
      f.server_executable, {"--check", "--expert", "--config", f.config.string()},
      {{secure_frontend_test::kTestTokenVariable, short_token}}});
  expect(result.exit_code != 0, "--check with an unreadable token exits 1");
  expect(!contains(result.out, "Configuration is valid."), "no valid verdict before the failure: " + result.out);
  expect(contains(result.err, "token: unavailable") && contains(result.err, "at least 32 bytes"),
         "the failure and its reason go to stderr: " + result.err);
  expect(!contains(result.out, short_token) && !contains(result.err, short_token), "the token value is never printed");
}

void cli_unknown_option_does_not_echo_value() {
  ServerFixture f;
  const std::string secret = "sk-PASTED-SECRET-0123456789";
  const auto equals_form = f.run({"--token=" + secret});
  expect(equals_form.exit_code != 0 && contains(equals_form.err, "unknown option: --token"), "flag name is echoed");
  expect(!contains(equals_form.err, secret), "the value after = is never echoed");
  const auto bare = f.run(with(f.base_flags(), {secret}));
  expect(bare.exit_code != 0 && !contains(bare.err, secret), "a stray argument is never echoed: " + bare.err);
  const auto long_flag = f.run({"--" + std::string(200, 'x')});
  expect(long_flag.exit_code != 0 && !contains(long_flag.err, std::string(65, 'x')), "at most 64 bytes are echoed");
  const auto missing = f.run({"--port"});
  expect(missing.exit_code != 0 && contains(missing.err, "missing value for --port"), "known flag without value");
}

void cli_check_has_no_side_effects() {
  ServerFixture f;
  auto literals = f.base_literals();
  literals["roots.logs"] = secure_frontend_test::json_literal((f.dir / "logs").string());
  literals["roots.cache"] = secure_frontend_test::json_literal((f.dir / "cache").string());
  f.write_config(literals);
  const auto before = secure_frontend_test::snapshot_tree(f.dir);
  const auto checked = f.run({"--check", "--expert", "--config", f.config.string()});
  expect(checked.exit_code == 0, "--check accepts absent-but-creatable logs/cache roots: " + checked.err);
  expect(secure_frontend_test::snapshot_tree(f.dir) == before, "--check created, wrote, renamed or removed nothing");
  expect(!std::filesystem::exists(f.dir / "logs") && !std::filesystem::exists(f.dir / "cache"), "--check created no root");
  secure_frontend_test::ChildProcess server;
  expect(f.start(server, {"--expert", "--config", f.config.string()}), "serve starts: " + server.stderr_text());
  expect(std::filesystem::is_directory(f.dir / "logs") && std::filesystem::is_directory(f.dir / "cache"),
         "serve creates both roots before it listens");
#if !defined(_WIN32)
  struct stat st {};
  stat((f.dir / "logs").c_str(), &st);
  expect((st.st_mode & 0777) == 0700, "serve creates them 0700");
#endif
}

std::vector<std::string> init_flags(const ServerFixture& f, const std::filesystem::path& config) {
  return {"init", "--config", config.string(), "--models-root", f.models.string(), "--tokenizer-pack",
          f.tokenizer.string(), "--engine", f.engine.string(), "--model-id", "qwen-local", "--state-dir", f.state.string(),
          "--port", std::to_string(f.port)};
}

void init_writes_config_and_token_then_server_starts() {
  ServerFixture f;
  const auto config = f.dir / "fresh" / "config.json";
  const auto init = f.run(init_flags(f, config));
  expect(init.exit_code == 0, "init succeeds: " + init.err);
  expect(std::filesystem::exists(config) && std::filesystem::exists(f.dir / "fresh" / "token"), "config and token written");
  const auto source = std::get<secure_frontend::SecretSource>(secure_frontend::make_protected_file_secret_source("token"));
  auto token = secure_frontend::read_token(source, f.dir / "fresh");
  const auto* value = std::get_if<secure_frontend::SecretBytes>(&token);
  expect(value != nullptr && value->size() == 43U, "a generated 43-character token is readable");
  if (value == nullptr) return;
  expect(!contains(init.out, value->view()) && !contains(init.err, value->view()), "init never prints the token");
  secure_frontend_test::ChildProcess server;
  expect(f.start(server, {"--expert", "--config", config.string()}), "the server starts from the written config alone: " + server.stderr_text());
  expect(f.wait_engine_ready(std::chrono::seconds(10), "Bearer " + std::string{value->view()}),
         "engine ready: /health is 503 while it starts");
  const auto response = f.request("GET", "/health", "Bearer " + std::string{value->view()});
  expect(response.rfind("HTTP/1.1 200", 0) == 0, "the generated token authenticates: " + response.substr(0, 16));
}

void init_refuses_to_overwrite_without_force() {
  ServerFixture f;
  const auto config = f.dir / "fresh" / "config.json";
  expect(f.run(init_flags(f, config)).exit_code == 0, "first init");
  const std::string config_before = secure_frontend_test::read_file(config);
  const std::string token_before = secure_frontend_test::read_file(f.dir / "fresh" / "token");
  const auto again = f.run(init_flags(f, config));
  expect(again.exit_code != 0 && contains(again.err, "already exists"), "a second init without --force fails: " + again.err);
  expect(secure_frontend_test::read_file(config) == config_before &&
             secure_frontend_test::read_file(f.dir / "fresh" / "token") == token_before,
         "nothing was changed");
  auto forced = init_flags(f, config);
  forced.insert(forced.begin() + 1, "--force");
  expect(f.run(forced).exit_code == 0, "--force succeeds");
  expect(secure_frontend_test::read_file(f.dir / "fresh" / "token") != token_before, "--force rotates the token");
}

void init_force_refuses_a_non_regular_config_target() {
  ServerFixture f;
  const auto config = f.dir / "fresh" / "config.json";
  expect(f.run(init_flags(f, config)).exit_code == 0, "first init");
  const std::string token_before = secure_frontend_test::read_file(f.dir / "fresh" / "token");
  const auto occupied = f.dir / "fresh" / "occupied";
  secure_frontend_test::make_private_directory(occupied);
  auto forced = init_flags(f, occupied);
  forced.insert(forced.begin() + 1, "--force");
  const auto result = f.run(forced);
  expect(result.exit_code != 0 && contains(result.err, "not a regular file"),
         "--force against a directory fails before writing: " + result.err);
  expect(secure_frontend_test::read_file(f.dir / "fresh" / "token") == token_before, "the token was not rotated");
  expect(std::filesystem::is_directory(occupied), "the directory is left in place");
}

void init_failure_creates_nothing() {
  ServerFixture f;
  const auto before = secure_frontend_test::snapshot_tree(f.dir);
  auto flags = init_flags(f, f.dir / "never" / "config.json");
  flags.back() = "0";  // --port 0 is invalid
  const auto result = f.run(flags);
  expect(result.exit_code != 0 && contains(result.err, "listener.port"), "an invalid option fails init: " + result.err);
  expect(!std::filesystem::exists(f.dir / "never"), "no config directory was created");
  expect(secure_frontend_test::snapshot_tree(f.dir) == before, "nothing below the scratch tree changed");
}

// roots.state (--state-dir) is optional for --check and init but required to
// serve: without it the server refuses before anything is spawned.
void serve_requires_state_root() {
  ServerFixture f;
  const auto marker = f.dir / "engine-started";
  auto flags = f.base_flags();
  const auto state_flag = std::find(flags.begin(), flags.end(), "--state-dir");
  expect(state_flag != flags.end(), "the fixture passes --state-dir");
  if (state_flag == flags.end()) return;
  flags.erase(state_flag, state_flag + 2);
  const auto checked = f.run(with(flags, {"--check"}));
  expect(checked.exit_code == 0, "--check accepts a config without roots.state: " + checked.err);
  const auto result = f.run(with(flags, {"--engine-arg", "--marker=" + marker.string()}));
  expect(result.exit_code != 0 && contains(result.err, "roots.state (--state-dir) is required to serve."),
         "serve without roots.state is refused: " + result.err);
  expect(!std::filesystem::exists(marker), "no engine was spawned");
}

// Task 29: each SP2 option sets exactly one SP1 schema key (a regression pin
// for the rows Tasks 18, 19 and 26 landed).
void cli_sp2_flags_map_to_schema_keys() {
  const char* argv[] = {"strata-unified-server", "--state-dir", "/srv/state", "--engine-lib-dir", "/opt/a",
                        "--engine-lib-dir", "/opt/b", "--gpu", "0,1", "--allow-new-privileges"};
  const auto parsed = secure_frontend::parse_server_command_line(static_cast<int>(std::size(argv)), argv);
  const auto* invocation = std::get_if<secure_frontend::ServerInvocation>(&parsed);
  expect(invocation != nullptr, "the SP2 options parse");
  if (invocation == nullptr) return;
  const auto& entries = invocation->flags.entries;
  const auto text = [&entries](const std::string_view key) {
    const auto found = entries.find(key);
    return found != entries.end() && std::holds_alternative<std::string>(found->second.value) ? std::get<std::string>(found->second.value)
                                                                                              : std::string{"<missing>"};
  };
  expect(text("roots.state") == "/srv/state", "--state-dir sets roots.state");
  expect(text("engine.gpu") == "0,1", "--gpu sets engine.gpu");
  const auto libs = entries.find("engine.lib_dirs");
  expect(libs != entries.end() && std::holds_alternative<std::vector<std::string>>(libs->second.value) &&
             std::get<std::vector<std::string>>(libs->second.value) == std::vector<std::string>{"/opt/a", "/opt/b"},
         "--engine-lib-dir appends to engine.lib_dirs");
  const auto nnp = entries.find("engine.allow_new_privileges");
  expect(nnp != entries.end() && std::holds_alternative<bool>(nnp->second.value) && std::get<bool>(nnp->second.value),
         "--allow-new-privileges sets engine.allow_new_privileges");
  for (const char* key : {"roots.state", "engine.lib_dirs", "engine.gpu", "engine.allow_new_privileges"}) {
    expect(secure_frontend::find_config_key(key) != nullptr, std::string("schema key ") + key);
  }
  // Issues (key_path, code) that load_server_config reports for a flag-only invocation.
  const auto issues_for = [](const std::vector<const char*>& arguments) {
    const auto bad_parsed = secure_frontend::parse_server_command_line(static_cast<int>(arguments.size()), arguments.data());
    const auto* bad_invocation = std::get_if<secure_frontend::ServerInvocation>(&bad_parsed);
    const auto validated = bad_invocation != nullptr ? secure_frontend::load_server_config(*bad_invocation, nullptr)
                                                     : secure_frontend::ConfigValidationResult{secure_frontend::ConfigErrors{}};
    const auto* errors = std::get_if<secure_frontend::ConfigErrors>(&validated);
    return errors != nullptr ? *errors : secure_frontend::ConfigErrors{};
  };
  bool gpu_issue = false;
  for (const auto& issue : issues_for({"strata-unified-server", "--gpu", "0,0"})) {
    gpu_issue = gpu_issue || (issue.key_path == "engine.gpu" && issue.reason.find("0,0") == std::string::npos);
  }
  expect(gpu_issue, "a malformed engine.gpu is a validation issue that never echoes the value");
  // The production refusal path for GPUs under the GLM profile (spec §3.9).
  bool glm_conflict = false;
  for (const auto& issue : issues_for({"strata-unified-server", "--profile", "glm53", "--gpu", "0"})) {
    glm_conflict = glm_conflict || (issue.key_path == "engine.gpu" &&
                                    issue.code == secure_frontend::ConfigIssueCode::conflicting_values);
  }
  expect(glm_conflict, "engine.gpu with engine.profile glm53 is an engine.gpu conflicting_values issue");
}

void auth04_empty_token_rejected_on_loopback() {
  ServerFixture f;
  const auto marker = f.dir / "engine-started";
  const auto flags = with(f.base_flags(), {"--engine-arg", "--marker=" + marker.string()});
  for (const char* value : {"", "   "}) {
    const auto result = secure_frontend_test::run_to_completion(
        secure_frontend_test::ProcessSpec{f.server_executable, flags, {{secure_frontend_test::kTestTokenVariable, value}}});
    expect(result.exit_code != 0 && contains(result.err, "Unable to read API token"),
           std::string{"an empty or blank token stops the loopback server: \""} + value + "\"");
  }
  secure_frontend_test::write_file(f.secret / "token", "");
  std::vector<std::string> file_flags = f.base_flags();
  for (std::size_t i = 0; i + 1 < file_flags.size(); ++i) {
    if (file_flags[i] == "--token-env") { file_flags[i] = "--token-file"; file_flags[i + 1] = "token"; }
  }
  file_flags.insert(file_flags.end(), {"--secret-root", f.secret.string(), "--engine-arg", "--marker=" + marker.string()});
  const auto empty_file = f.run(file_flags);
  expect(empty_file.exit_code != 0 && contains(empty_file.err, "Unable to read API token"), "an empty token file too");
  expect(!std::filesystem::exists(marker), "the engine never started");
  expect(!secure_frontend_test::can_connect(f.port), "and nothing listens");
}

// Every "--name" token in `text` (the name ends at a byte outside [a-z0-9-]).
std::set<std::string> option_names_in(std::string_view text) {
  std::set<std::string> names;
  for (std::size_t at = text.find("--"); at != std::string_view::npos; at = text.find("--", at + 2)) {
    std::size_t end = at + 2;
    while (end < text.size() && ((text[end] >= 'a' && text[end] <= 'z') || (text[end] >= '0' && text[end] <= '9') ||
                                 text[end] == '-')) {
      ++end;
    }
    if (end > at + 2) names.emplace(text.substr(at, end - at));
  }
  return names;
}

// Every STRATA_[A-Z0-9_]+ name in `text`.
void strata_names_in(std::string_view text, std::set<std::string>* names) {
  for (std::size_t at = text.find("STRATA_"); at != std::string_view::npos; at = text.find("STRATA_", at + 7)) {
    std::size_t end = at + 7;
    while (end < text.size() && ((text[end] >= 'A' && text[end] <= 'Z') || (text[end] >= '0' && text[end] <= '9') ||
                                 text[end] == '_')) {
      ++end;
    }
    if (end > at + 7) names->emplace(text.substr(at, end - at));
  }
}

void auth04_no_switch_disables_auth() {
  constexpr std::array kForbidden{"auth", "insecure", "unauth", "dev", "anonymous", "disable", "no_token", "no-token"};
  const auto forbidden_in = [&kForbidden](const std::string& text) {
    return std::any_of(kForbidden.begin(), kForbidden.end(),
                       [&text](const char* word) { return text.find(word) != std::string::npos; });
  };
  // Derived from the schema and the usage text, so a key or flag added later is examined by name.
  std::set<std::string> flags = option_names_in(secure_frontend::server_usage());
  std::set<std::string> strata_names;
  strata_names_in(secure_frontend::server_usage(), &strata_names);
  for (const auto& key : secure_frontend::config_schema()) {
    const std::string path{key.path};
    expect(!forbidden_in(path), "no config key can switch off authentication: " + path);
    for (const auto& flag : option_names_in(key.flag)) flags.insert(flag);
    for (const std::string_view text : {key.path, key.flag, key.description, key.default_string}) {
      strata_names_in(text, &strata_names);
    }
    if (key.type == secure_frontend::ConfigValueType::boolean) {
      // SP2 Task 26 (owner-visible edit of this SP1 security test): the engine's
      // Linux NNP opt-out; SP4 §3.5: pull.allow_private_addresses widens the
      // downloader's address policy; SP3 §8: logging.requests turns on the
      // per-request stderr line; SP5 M1 Task 5 (ruling T5-d): engine.fit_max_tokens and
      // reasoning.close_retry are file-only behaviour booleans (the pin's JSON knobs);
      // SP5 M1 Task 6 (same ruling pattern): engine.lazy_load and engine.mtp are the
      // pin's lifecycle booleans (runconfig.py:36-38); SP5 M1 Task 7: engine.vram_elastic,
      // engine.split_skip_if_fits, engine.elastic and engine.uma are the pin's VRAM/turn
      // booleans (serve/server.py:2945, :2941, :2550; images.py:23); SP5 M1 Task 8:
      // engine.answer_room is the pin's thinking-room boolean (serve/server.py:8014);
      // SP5 M1 Task 9: session.tool_call_recovery, session.env_to_user, session.codex_thread_titles
      // and session.codex_compaction_cache are the pin's opt-in falses (:8043-:8044, :8106, :8110);
      // SP5 M1 Task 11: vision.enabled/no_flash_attn/resident are the pin's image-INPUT
      // booleans (serve/server.py:2511, :2930), and vision.gpu (2026-10-28, a card's encoder on the GPU: the
      // encoder's --gpu) chooses where pictures are encoded. SP5 M2 Task 3: glm.fit_max_tokens and
      // glm.experimental_speed_projection are Maya's file-only GLM booleans
      // (serve/server.py:4077, :3819). None of those touches authentication.
      // Owner decisions 2026-10-21 (session log step 12): listener.web_ui serves the built-in
      // page and leaves every API route keyed; listener.loopback_without_key is the ONE switch
      // that turns the key off — config-file only (no flag, no environment variable), false
      // by default, loopback binds only (refused at load and again at bind). Session log step
      // 16: listener.monitor serves GET /metrics, a keyed route like the rest; it reads no key.
      // Model launcher (owner decisions D1-D10, 2026-10-23): models.control serves the keyed
      // model-control routes and models.control_remote opens them to non-loopback key holders;
      // neither reads, skips or replaces the key. GUI-first start-up (2026-10-27), reviewed when the owner asked for
      // the open issues on 2026-10-28: listener.login_link prints a one-time link that hands the page the key from
      // this PC only (it needs a token.source and the key stays required on every route); roots.allow_untrusted_media
      // uses model folders as they are; alerts.email.enabled sends email alerts; search.enabled lets a model search
      // through SearXNG. None of them reads, skips or replaces the key.
      if (path == "listener.loopback_without_key") {
        expect(key.flag.empty() && key.default_string.empty(), "the keyless switch has no flag and is off by default");
        continue;
      }
      expect(path == "listener.allow_remote" || path == "listener.allow_wildcard_bind" || path == "listener.web_ui" ||
                 path == "listener.monitor" || path == "engine.allow_new_privileges" || path == "pull.allow_private_addresses" ||
                 path == "logging.requests" || path == "engine.fit_max_tokens" || path == "reasoning.close_retry" ||
                 path == "engine.lazy_load" || path == "engine.mtp" || path == "engine.vram_elastic" ||
                 path == "engine.split_skip_if_fits" || path == "engine.elastic" || path == "engine.uma" ||
                 path == "engine.answer_room" || path == "session.tool_call_recovery" || path == "session.env_to_user" ||
                 path == "session.codex_thread_titles" || path == "session.codex_compaction_cache" ||
                 path == "vision.enabled" || path == "vision.no_flash_attn" || path == "vision.resident" || path == "vision.gpu" ||
                 path == "glm.fit_max_tokens" || path == "glm.experimental_speed_projection" ||
                 path == "models.control" || path == "models.control_remote" || path == "models.downloads" ||
                 path == "listener.login_link" || path == "roots.allow_untrusted_media" || path == "alerts.email.enabled" ||
                 path == "search.enabled",
             "the only switches are the two bind opt-ins, the page and the Monitor, the engine's NNP opt-out, "
             "the pull address opt-in, request logging and the SP5 behaviour, lifecycle, vram/turn, answer-room, "
             "session and vision opt-ins, the model launcher's two control switches, the login link, model folders "
             "as they are, email alerts and web search: " + path);
    }
  }
  expect(flags.count("--allow-remote") == 1U && flags.count("--token-env") == 1U && flags.count("--check") == 1U,
         "the flag sweep sees the schema's and the usage text's options");
  for (const auto& flag : flags) expect(!forbidden_in(flag), "no option can switch off authentication: " + flag);
  expect(strata_names.count("STRATA_ENGINE_READY_S") == 1U, "the STRATA_* sweep is seeded from the schema text");
  ServerFixture f;
  for (const char* flag : {"--no-auth", "--insecure", "--dev", "--disable-auth", "--allow-unauthenticated", "--auth=none"}) {
    const auto result = f.run(with(f.base_flags(), {flag}));
    expect(result.exit_code != 0 && contains(result.err, "unknown option"), std::string{"no such option: "} + flag);
  }
  // Upstream's STRATA_* knobs are never read (product spec §4); set them all and probe without a token.
  auto environment = f.environment();
  // The schema-derived names plus guesses at knobs that could exist upstream.
  for (const char* name : {"STRATA_API_KEY", "STRATA_NO_AUTH", "STRATA_DEV", "STRATA_INSECURE", "STRATA_DEBUG", "STRATA_AUTH"}) {
    strata_names.emplace(name);
  }
  for (const auto& name : strata_names) environment.emplace_back(name, name == "STRATA_API_KEY" ? "" : "1");
  secure_frontend_test::ChildProcess server;
  expect(server.start(secure_frontend_test::ProcessSpec{f.server_executable, f.base_flags(), environment}), "server spawned");
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (!secure_frontend_test::can_connect(f.port) && std::chrono::steady_clock::now() < deadline &&
         !server.wait_for(std::chrono::milliseconds(50)).has_value()) {
  }
  const auto response = f.request("GET", "/v1/models", "");
  expect(response.rfind("HTTP/1.1 401", 0) == 0, "no environment variable disables authentication");
}
// SP4 §4.9 d (Task 24a/24b addendum): --expert parses once, with serve, --check and init.
void cli_expert_flag_parsing() {
  const auto parse = [](std::vector<const char*> argv) {
    argv.insert(argv.begin(), "strata-unified-server");
    return secure_frontend::parse_server_command_line(static_cast<int>(argv.size()), argv.data());
  };
  const auto twice = parse({"--expert", "--expert"});
  const auto* error = std::get_if<secure_frontend::CliError>(&twice);
  expect(error != nullptr && error->message == "duplicate --expert", "a second --expert is rejected");
  for (const auto& [argv, command] : std::vector<std::pair<std::vector<const char*>, secure_frontend::ServerCommand>>{
           {{"--expert"}, secure_frontend::ServerCommand::serve},
           {{"--check", "--expert"}, secure_frontend::ServerCommand::check},
           {{"--expert", "--check"}, secure_frontend::ServerCommand::check},
           {{"init", "--config", "c.json", "--expert"}, secure_frontend::ServerCommand::init}}) {
    const auto parsed = parse(argv);
    const auto* invocation = std::get_if<secure_frontend::ServerInvocation>(&parsed);
    expect(invocation != nullptr && invocation->expert && invocation->command == command, "--expert accepted");
  }
  const auto plain = parse({"--check"});
  const auto* invocation = std::get_if<secure_frontend::ServerInvocation>(&plain);
  expect(invocation != nullptr && !invocation->expert, "--expert is off by default");
}

// Gemma 4 plan 1 Task 1: the help names every engine.profile value (2026-10-28: all seven, and what the server serves
// today: streaming, the page's model cards, downloads, alerts and web search, never "disabled").
void usage_names_the_three_profiles() {
  const std::string usage = secure_frontend::server_usage();
  expect(usage.find("engine.profile is qwen, glm53, gemma4, deepseek, raon1, flint or minimax") != std::string::npos,
         "the usage text names every engine.profile value");
  expect(usage.find("Streaming, tools, downloads") == std::string::npos && usage.find("streamed or not") != std::string::npos,
         "the usage text says chat streams and no longer calls downloads and MCP disabled");
  expect(usage.find("[--data-dir DIR] [options]") != std::string::npos, "the usage shows options on the data folder");
}

// --- SP5 M1 Task 13: `strata serve` (foreground) -------------------------------------
// The managed model m:t installed with the acquire fixtures (real byte-tokenizer trio,
// Ruling T13-g) and managed config literals: roots.tokenizer_pack belongs to --expert,
// and a managed bundle carries its own pack (Ruling T13-a).
void install_fx_model(ServerFixture& f) {
  namespace fx = secure_frontend_test::acquire;
  const auto root = fx::models_root(f.models);
  const auto payload = fx::qwen_payload_pack();
  const auto manifest = fx::trusted(fx::manifest_text("m", "t", 1U, payload), secure_frontend::ModelOrigin::third_party);
  fx::stage(root, manifest, payload);
  fx::FakeLoaded nobody;
  const secure_frontend::StoreEnvironment env;
  const auto published =
      secure_frontend::publish_staged(root, secure_frontend::staging_key(manifest.parsed.sha256), manifest, nobody, env, true);
  expect(std::holds_alternative<secure_frontend::PublishResult>(published), "managed model m:t is installed for serve");
}

std::map<std::string, std::string> managed_serve_literals(const ServerFixture& f) {
  auto literals = f.base_literals();
  literals.erase("roots.tokenizer_pack");            // expert-only; the bundle carries the pack
  literals["engine.model_id"] = json_literal("m:t");  // the served id is name:tag
  return literals;
}

void serve_prints_ready_then_serves() {
  ServerFixture f;
  install_fx_model(f);
  // Ruling T13-h: the same config validation as the run verbs — a broken file answers
  // before anything is bound or spawned.
  write_file(f.dir / "broken.json",
             render_config_json({{"schema_version", "1"},
                                 {"listener.port", "0"},
                                 {"roots.models", json_literal(f.models.string())},
                                 {"roots.state", json_literal(f.state.string())},
                                 {"engine.executable", json_literal(f.engine.string())},
                                 {"engine.model_id", json_literal("m")},
                                 {"token.source", json_literal(std::string{"env:"} + kTestTokenVariable)}}));
  const auto broken = f.run_cli({"--config", (f.dir / "broken.json").string(), "serve"});
  expect(broken.exit_code != 0 && contains(broken.err, "listener.port: must be between 1 and 65535"),
         "serve validates the config like the other verbs: " + broken.err);
  f.write_config(managed_serve_literals(f));
  ChildProcess cli;
  expect(f.start_cli(cli, {"--config", f.config.string(), "serve"}), "strata serve runs in the foreground and the port answers");
  expect(f.wait_engine_ready(), "the managed model reaches READY under strata serve");
  const std::string reply = f.request("POST", "/v1/chat/completions", f.bearer(),
                                      R"({"model":"m:t","messages":[{"role":"user","content":"Hi"}],"max_tokens":2})");
  expect(contains(reply, "200 OK") && contains(reply, "\"choices\""), "a chat against the CLI-served model answers: " + reply.substr(0, 200));
  // Ruling T13-d: the boot's own ready literals — stdout's human line and stderr's
  // structured line, both with the served authority.
  expect(contains(cli.stdout_text(), "Secure server listening on http://127.0.0.1:" + std::to_string(f.port)),
         "the stdout ready line: " + cli.stdout_text().substr(0, 300));
  expect(contains(cli.stderr_text(), "strata: listening 127.0.0.1:" + std::to_string(f.port)),
         "the stderr structured ready line: " + cli.stderr_text().substr(0, 300));
}

void serve_runs_foreground_and_shuts_clean_on_sigterm() {
  ServerFixture f;
  install_fx_model(f);
  f.write_config(managed_serve_literals(f));
  ChildProcess cli;
  expect(f.start_cli(cli, {"--config", f.config.string(), "serve"}), "the CLI process itself listens (no daemonization)");
#if !defined(_WIN32)
  const bool running = !cli.wait_for(std::chrono::milliseconds(0)).has_value();
  (void)kill(static_cast<pid_t>(cli.pid()), SIGTERM);
  const auto code = cli.wait_for(std::chrono::seconds(5));
  expect(running && code.has_value() && *code == 0,
         "SIGTERM: exit 0 within 5 s (the SP3 I1 shutdown path), code=" + std::to_string(code.value_or(-9)) +
             " err=" + cli.stderr_text().substr(0, 300));
#endif
  // No lifecycle files (Ruling T13-f): the PX server.json writer does not exist, and
  // no lock is taken — the whole scratch tree stays free of both.
  std::error_code ec;
  bool lifecycle_file = false;
  for (auto it = std::filesystem::recursive_directory_iterator(f.dir, ec);
       !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
    const std::string name = it->path().filename().string();
    if (name == "server.json" || (name.size() > 5 && name.compare(name.size() - 5, 5, ".lock") == 0)) lifecycle_file = true;
  }
  expect(!lifecycle_file, "serve writes no server.json, no lock, nothing for a lifecycle");
}
}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"cli_config_file_and_flags_share_validator", cli_config_file_and_flags_share_validator},
      {"cli_flag_overrides_file_and_is_reported", cli_flag_overrides_file_and_is_reported},
      {"cli_check_prints_no_token", cli_check_prints_no_token},
      {"cli_check_unreadable_token_is_not_reported_valid", cli_check_unreadable_token_is_not_reported_valid},
      {"cli_check_has_no_side_effects", cli_check_has_no_side_effects},
      {"cli_expert_flag_parsing", cli_expert_flag_parsing},
      {"usage_names_the_three_profiles", usage_names_the_three_profiles},
      {"cli_unknown_option_does_not_echo_value", cli_unknown_option_does_not_echo_value},
      {"init_writes_config_and_token_then_server_starts", init_writes_config_and_token_then_server_starts},
      {"init_refuses_to_overwrite_without_force", init_refuses_to_overwrite_without_force},
      {"init_force_refuses_a_non_regular_config_target", init_force_refuses_a_non_regular_config_target},
      {"init_failure_creates_nothing", init_failure_creates_nothing},
      {"auth04_empty_token_rejected_on_loopback", auth04_empty_token_rejected_on_loopback},
      {"auth04_no_switch_disables_auth", auth04_no_switch_disables_auth},
      {"serve_requires_state_root", serve_requires_state_root},
      {"cli_sp2_flags_map_to_schema_keys", cli_sp2_flags_map_to_schema_keys},
      {"serve_prints_ready_then_serves", serve_prints_ready_then_serves},
      {"serve_runs_foreground_and_shuts_clean_on_sigterm", serve_runs_foreground_and_shuts_clean_on_sigterm},
  }, failures);
}
