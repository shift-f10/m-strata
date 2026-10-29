// GUI-first start-up (docs/superpowers/specs/2026-10-27-gui-first-install-design.md §3): the data folder the first
// start creates beside the binaries, the card-less launcher config it writes, the install folder made owner-only, and
// the one-time login link.
#include "secure_frontend/canonical_path.hpp"
#include "secure_frontend/config_file.hpp"
#include "secure_frontend/first_start.hpp"
#include "secure_frontend/frontend_config.hpp"
#include "secure_frontend/json.hpp"
#include "secure_frontend/login_link.hpp"
#include "secure_frontend/model_cards.hpp"
#include "secure_frontend/server_cli.hpp"
#include "support/test_support.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
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

using secure_frontend::ConfigDraftEntry;
using secure_frontend::ConfigSource;
using secure_frontend::FrontendConfigDraft;

std::string string_entry(const FrontendConfigDraft& draft, const std::string_view key) {
  const auto it = draft.entries.find(key);
  if (it == draft.entries.end()) return "<absent>";
  const auto* text = std::get_if<std::string>(&it->second.value);
  return text != nullptr ? *text : "<not a string>";
}

bool bool_entry(const FrontendConfigDraft& draft, const std::string_view key) {
  const auto it = draft.entries.find(key);
  const auto* value = it == draft.entries.end() ? nullptr : std::get_if<bool>(&it->second.value);
  return value != nullptr && *value;
}

// An install folder as make.py leaves it: engine/multiengine(.exe) under a private folder.
struct Install final {
  std::filesystem::path root = secure_frontend_test::make_scratch_directory();
  std::filesystem::path engine;
  Install() {
    secure_frontend_test::make_private_directory(root / "engine");
    engine = secure_frontend::bundled_engine(root);
    secure_frontend_test::write_file(engine, "MZ engine", 0700);
  }
};

void config_writer_renders_decimals() {
  FrontendConfigDraft draft;
  draft.entries["schema_version"] = ConfigDraftEntry{std::int64_t{1}, ConfigSource::file};
  draft.entries["defaults.sampling.temperature"] = ConfigDraftEntry{0.6, ConfigSource::file};
  draft.entries["defaults.sampling.top_p"] = ConfigDraftEntry{0.95, ConfigSource::file};
  draft.entries["defaults.sampling.top_k"] = ConfigDraftEntry{std::int64_t{20}, ConfigSource::file};
  draft.entries["defaults.sampling.min_p"] = ConfigDraftEntry{1.0, ConfigSource::file};
  const std::string text = secure_frontend::render_config_document(draft);
  expect(text.find("\"temperature\": 0.6") != std::string::npos && text.find("\"top_p\": 0.95") != std::string::npos,
         "decimals are written as JSON numbers: " + text);
  expect(text.find("\"min_p\": 1.0") != std::string::npos, "a whole decimal keeps its fraction (1.0): " + text);
  const auto parsed = secure_frontend::parse_config_text(text, secure_frontend_test::make_scratch_directory());
  const auto* back = std::get_if<FrontendConfigDraft>(&parsed);
  expect(back != nullptr, "the written document parses back");
  if (back == nullptr) return;
  const auto* temperature = std::get_if<double>(&back->entries.at("defaults.sampling.temperature").value);
  const auto* min_p = std::get_if<double>(&back->entries.at("defaults.sampling.min_p").value);
  expect(temperature != nullptr && *temperature == 0.6 && min_p != nullptr && *min_p == 1.0, "and round-trips exactly");
}

void first_start_creates_the_data_folder() {
  Install install;
  const auto folder = secure_frontend::data_folder_layout(install.root / "data");
  const auto result = secure_frontend::ensure_data_folder(folder, install.engine);
  const auto* done = std::get_if<secure_frontend::FirstStartResult>(&result);
  expect(done != nullptr && done->created, "the first start creates the data folder: " +
                                               (done == nullptr ? std::get<secure_frontend::FirstStartError>(result).message : ""));
  for (const auto& dir : {folder.root, folder.secret, folder.state, folder.downloads, folder.models, folder.logs, folder.cache}) {
    expect(std::holds_alternative<secure_frontend::CanonicalRoot>(secure_frontend::open_root(dir, secure_frontend::RootPolicy::existing)),
           "an owner-only folder: " + secure_frontend::path_to_utf8(dir.filename()));
  }
  expect(std::filesystem::is_regular_file(folder.secret / "token"), "a new API key in secret/token");
  const auto draft = secure_frontend::read_config_file(folder.config_file);
  const auto* main = std::get_if<FrontendConfigDraft>(&draft);
  expect(main != nullptr, "data/config.json is an ordinary config file");
  if (main == nullptr) return;
  const auto cards = main->entries.find("models.cards");
  expect(cards != main->entries.end() && std::get<std::vector<std::string>>(cards->second.value).empty(),
         "launcher mode with no card yet");
  expect(bool_entry(*main, "models.control") && bool_entry(*main, "models.downloads") && bool_entry(*main, "listener.web_ui") &&
             bool_entry(*main, "listener.monitor") && bool_entry(*main, "listener.login_link"),
         "the page, the monitor, the login link, model control and downloads are on");
  expect(string_entry(*main, "listener.host") == "127.0.0.1", "loopback only");
  expect(string_entry(*main, "token.source") == "file:token" &&
             string_entry(*main, "roots.secret") == secure_frontend::path_to_utf8(folder.secret),
         "the key is the secret folder's token file");
  expect(string_entry(*main, "models.default_engine") == secure_frontend::path_to_utf8(install.engine) &&
             string_entry(*main, "models.default_root") == secure_frontend::path_to_utf8(folder.models) &&
             string_entry(*main, "models.downloads_root") == secure_frontend::path_to_utf8(folder.downloads),
         "the bundled engine, the default models folder and the downloads folder");
}

void a_later_start_keeps_everything() {
  Install install;
  const auto folder = secure_frontend::data_folder_layout(install.root / "data");
  (void)secure_frontend::ensure_data_folder(folder, install.engine);
  const std::string config = secure_frontend_test::read_file(folder.config_file);
  const std::string token = secure_frontend_test::read_file(folder.secret / "token");
  const auto again = secure_frontend::ensure_data_folder(folder, install.engine);
  const auto* done = std::get_if<secure_frontend::FirstStartResult>(&again);
  expect(done != nullptr && !done->created, "a later start uses the folder as it is");
  expect(secure_frontend_test::read_file(folder.config_file) == config && secure_frontend_test::read_file(folder.secret / "token") == token,
         "the config and the key are never rewritten");
  // a data folder that is a file is refused, and nothing is written there
  Install other;
  secure_frontend_test::write_file(other.root / "data", "not a folder");
  const auto refused = secure_frontend::ensure_data_folder(secure_frontend::data_folder_layout(other.root / "data"), other.engine);
  expect(std::holds_alternative<secure_frontend::FirstStartError>(refused), "a data path that is a file is refused");
}

void card_less_server_config_comes_from_the_main_config() {
  Install install;
  const auto folder = secure_frontend::data_folder_layout(install.root / "data");
  (void)secure_frontend::ensure_data_folder(folder, install.engine);
  const auto draft = secure_frontend::read_config_file(folder.config_file);
  const auto& main = std::get<FrontendConfigDraft>(draft);
  const auto server = secure_frontend::card_less_server_config(main);
  const auto* config = std::get_if<secure_frontend::ValidatedConfig>(&server);
  std::string why;
  if (const auto* issues = std::get_if<secure_frontend::ConfigErrors>(&server)) {
    for (const auto& issue : *issues) why += secure_frontend::format_issue(issue) + "; ";
  }
  expect(config != nullptr, "the first start's config serves with no card: " + why);
  if (config != nullptr) {
    expect(config->get().listener.web_ui && config->get().listener.login_link && config->get().models.control,
           "its server settings are the main config's");
  }
  FrontendConfigDraft without = main;
  without.entries.erase("models.default_engine");
  const auto refused = secure_frontend::card_less_server_config(without);
  const auto* issues = std::get_if<secure_frontend::ConfigErrors>(&refused);
  expect(issues != nullptr && !issues->empty() && issues->front().key_path == "models.default_engine",
         "with no card and no models.default_engine there is nothing to serve");
}

void install_folder_is_made_owner_only() {
  Install install;
  // as a copy into a folder others may write leaves it: the install folder and the engine's own folder
#if defined(_WIN32)
  expect(secure_frontend_test::grant_everyone_write(install.root) &&
             secure_frontend_test::grant_everyone_write(install.root / "engine"),
         "the test widens the install folder and the engine's folder");
#else
  ::chmod(install.root.c_str(), 0777);
  ::chmod((install.root / "engine").c_str(), 0777);
#endif
  expect(!std::holds_alternative<secure_frontend::CanonicalFile>(secure_frontend::open_executable(install.engine)),
         "an engine in a folder others may write is refused");
  const auto fixed = secure_frontend::secure_install_folder(install.root);
  const auto* changed = std::get_if<bool>(&fixed);
  expect(changed != nullptr && *changed, "the first start makes the install folder owner-only");
  expect(std::holds_alternative<secure_frontend::CanonicalFile>(secure_frontend::open_executable(install.engine)),
         "and the engine is accepted afterwards");
  const auto again = secure_frontend::secure_install_folder(install.root);
  const auto* unchanged = std::get_if<bool>(&again);
  expect(unchanged != nullptr && !*unchanged, "an owner-only folder is left as it is");
}

void login_link_is_used_once() {
  using Clock = secure_frontend::LoginLinks::Clock;
  secure_frontend::LoginLinks links;
  const auto now = Clock::now();
  const auto code = links.issue(now);
  expect(code.has_value() && code->size() == 43U, "a 43-character code (32 random bytes, base64url)");
  if (!code) return;
  expect(!links.claim("", now) && !links.claim(code->substr(1), now), "a wrong code is refused");
  expect(links.claim(*code, now + std::chrono::minutes(1)), "the code logs in once");
  expect(!links.claim(*code, now + std::chrono::minutes(1)), "and never again");
  const auto second = links.issue(now);
  const auto third = links.issue(now);
  expect(second && third && *second != *third && !links.claim(*second, now) && links.claim(*third, now),
         "a new code replaces the one before it");
  const auto late = links.issue(now);
  expect(late && !links.claim(*late, now + secure_frontend::LoginLinks::kLifetime + std::chrono::seconds(1)),
         "a code expires after ten minutes");
  const auto guessed = links.issue(now);
  for (int i = 0; i < secure_frontend::LoginLinks::kMaximumAttempts; ++i) (void)links.claim("wrong", now);
  expect(guessed && !links.claim(*guessed, now), "too many wrong tries spend the code");
  expect(secure_frontend::login_link_url("127.0.0.1:8080", "abc") == "http://127.0.0.1:8080/#login=abc",
         "the link carries the code after #, so it never reaches a request line or a log");
}

void data_dir_flag_is_parsed() {
  const char* with[] = {"strata-unified-server", "--data-dir", "D:/SecureStrata/data"};
  const auto parsed = secure_frontend::parse_server_command_line(3, with);
  const auto* invocation = std::get_if<secure_frontend::ServerInvocation>(&parsed);
  expect(invocation != nullptr && invocation->data_dir.has_value() &&
             secure_frontend::path_to_utf8(*invocation->data_dir) == "D:/SecureStrata/data",
         "--data-dir names the data folder");
  const char* both[] = {"strata-unified-server", "--data-dir", "D:/a", "--config", "D:/a/config.json"};
  const auto refused = secure_frontend::parse_server_command_line(5, both);
  expect(std::holds_alternative<secure_frontend::CliError>(refused), "--data-dir and --config exclude each other");
}

// Owner 2026-10-28 ("ensure the syntax for configuring and starting it are correct"): `strata-unified-server --port 9090`
// failed with "roots.models: is required" because options alone skipped data/. Options now adjust the data folder's
// config for that start; only --config and a bring-up start that names its model by flags leave the data folder out.
void options_alone_start_from_the_data_folder() {
  const auto uses = [](std::vector<const char*> argv) {
    argv.insert(argv.begin(), "strata-unified-server");
    const auto parsed = secure_frontend::parse_server_command_line(static_cast<int>(argv.size()), argv.data());
    const auto* invocation = std::get_if<secure_frontend::ServerInvocation>(&parsed);
    expect(invocation != nullptr, "the command line parses");
    return invocation != nullptr && secure_frontend::uses_data_folder(*invocation);
  };
  expect(uses({}), "no option: the data folder");
  expect(uses({"--port", "9090"}), "--port alone: the data folder on that port");
  expect(uses({"--host", "127.0.0.1", "--port", "9090", "--token-env", "STRATA_DEV_KEY", "--gpu", "0"}),
         "listener, key and GPU options: the data folder");
  expect(uses({"--check", "--port", "9090"}), "--check with an option reads the data folder too");
  expect(uses({"--data-dir", "D:/a", "--port", "1"}), "--data-dir with options: that data folder");
  expect(!uses({"--config", "D:/a/config.json", "--port", "1"}), "--config: that file, not the data folder");
  expect(!uses({"--expert", "--port", "1"}), "--expert is a bring-up start");
  for (const char* flag : {"--models-root", "--engine", "--model-id", "--profile", "--tokenizer-pack"}) {
    expect(!uses({flag, "x"}), std::string{flag} + " names the model by flags: a bring-up start");
  }
}

}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"config_writer_renders_decimals", config_writer_renders_decimals},
      {"options_alone_start_from_the_data_folder", options_alone_start_from_the_data_folder},
      {"first_start_creates_the_data_folder", first_start_creates_the_data_folder},
      {"a_later_start_keeps_everything", a_later_start_keeps_everything},
      {"card_less_server_config_comes_from_the_main_config", card_less_server_config_comes_from_the_main_config},
      {"install_folder_is_made_owner_only", install_folder_is_made_owner_only},
      {"login_link_is_used_once", login_link_is_used_once},
      {"data_dir_flag_is_parsed", data_dir_flag_is_parsed},
  }, failures);
}
