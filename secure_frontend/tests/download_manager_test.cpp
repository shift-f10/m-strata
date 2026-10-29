// Model downloads (plan 2026-10-23-model-downloads Task 4): the server's download manager over a
// scripted `strata` child and a fake disk. The catalog's cards and the free-space refusal (O2),
// the pull's argv and the manifest it is given (O4 through hf_source), progress, failure and
// cancel, Maya's "main" pins resolved and checked, and a pasted link (O3) checked then started.
#include "secure_frontend/canonical_path.hpp"
#include "secure_frontend/card_builder.hpp"
#include "secure_frontend/download_manager.hpp"
#include "secure_frontend/gguf_metadata.hpp"
#include "secure_frontend/engine_arguments.hpp"
#include "secure_frontend/json.hpp"
#include "secure_frontend/manifest_json.hpp"
#include "secure_frontend/model_cards.hpp"
#include "secure_frontend/model_options.hpp"
#include "secure_frontend/sha256.hpp"
#include "support/gguf_builder.hpp"
#include "support/test_support.hpp"
#include "support/test_tokenizer.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

int failures = 0;
void expect(const bool value, const std::string_view message) {
  if (!value) {
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
  }
}

using secure_frontend::DownloadManager;

constexpr std::string_view kCommit = "0123456789abcdef0123456789abcdef01234567";
const std::string kShaA(64, 'a');
const std::string kShaB(64, 'b');

struct Script final {
  std::vector<std::string> lines;
  int exit_code{0};
  bool hang{false};  // after its lines, never ends until killed
};

struct Recorded final {
  secure_frontend::EngineCommand command;
};

class FakeChild final : public secure_frontend::LineChild {
 public:
  FakeChild(Script script, std::shared_ptr<std::atomic<bool>> killed) : script_(std::move(script)), killed_(std::move(killed)) {}
  std::optional<std::string> next_line(const std::chrono::milliseconds timeout) override {
    // "#sleep <ms>": the child is quiet that long (a rate needs a second between readings).
    while (next_ < script_.lines.size() && script_.lines[next_].rfind("#sleep ", 0) == 0U) {
      std::this_thread::sleep_for(std::chrono::milliseconds(std::stoi(script_.lines[next_++].substr(7))));
    }
    if (next_ < script_.lines.size()) return script_.lines[next_++];
    std::this_thread::sleep_for(std::min(timeout, std::chrono::milliseconds(5)));
    return std::nullopt;
  }
  bool ended() override { return next_ >= script_.lines.size() && (!script_.hang || killed_->load()); }
  int exit_code() override { return killed_->load() ? -1 : script_.exit_code; }
  void kill() override { killed_->store(true); }

 private:
  Script script_;
  std::size_t next_{0};
  std::shared_ptr<std::atomic<bool>> killed_;
};

// Hands out scripts by verb (hf-info or pull) and records every command.
struct Spawner final {
  std::mutex mutex;
  std::deque<Script> hf;
  std::deque<Script> pulls;
  std::vector<Recorded> recorded;
  std::shared_ptr<std::atomic<bool>> killed = std::make_shared<std::atomic<bool>>(false);
  std::map<std::filesystem::path, std::uint64_t> free_by_path;   // a folder's own free space (else the default)
  std::optional<std::filesystem::path> installed;                // where an installed download's files are
  std::function<void(const secure_frontend::EngineCommand&)> on_spawn;   // a child's side effects (the files it writes)
  secure_frontend::DownloadSeams seams(std::uint64_t free_bytes) {
    secure_frontend::DownloadSeams s;
    s.spawn = [this](const secure_frontend::EngineCommand& command) -> std::unique_ptr<secure_frontend::LineChild> {
      std::lock_guard lock(mutex);
      recorded.push_back(Recorded{command});
      const auto has_argument = [&command](const std::string_view a) {
        return std::find(command.arguments.begin(), command.arguments.end(), a) != command.arguments.end();
      };
      const bool info = has_argument("hf-info");
      auto& queue = info ? hf : pulls;
      if (queue.empty()) return nullptr;
      Script script = queue.front();
      queue.pop_front();
      // `strata rm <id>:download` that succeeds: the store no longer lists the download
      if (has_argument("rm") && !has_argument("--partials") && script.exit_code == 0) installed.reset();
      if (on_spawn) on_spawn(command);
      return std::make_unique<FakeChild>(std::move(script), killed);
    };
    s.disk = [this, free_bytes](const std::filesystem::path& path) {
      const auto found = free_by_path.find(path);
      const std::uint64_t free = found == free_by_path.end() ? free_bytes : found->second;
      return std::optional<secure_frontend::DiskSpace>{secure_frontend::DiskSpace{free, free * 2U}};
    };
    s.locate = [this](const std::filesystem::path&, const std::string_view) { return installed; };
    return s;
  }
};

std::string catalog_text() {
  return "{\"schema_version\":1,\"models\":["
         "{\"id\":\"qwen-small\",\"name\":\"Qwen small\",\"source\":\"strata\",\"family\":\"qwen\",\"engine\":\"qwen\","
         "\"about\":\"a test model\",\"repo\":\"acme/Qwen-GGUF\",\"revision\":\"" +
         std::string{kCommit} +
         "\",\"files\":[{\"path\":\"Q/m-00001-of-00002.gguf\",\"size\":1000,\"sha256\":\"" + kShaA +
         "\"},{\"path\":\"Q/m-00002-of-00002.gguf\",\"size\":2000,\"sha256\":\"" + kShaB +
         "\"}],\"prepare\":\"iq_pack\",\"pack_args\":[],\"expert_profile\":\"expert-profile.bin\",\"mmproj\":{\"path\":"
         "\"mmproj-BF16.gguf\",\"size\":300,\"sha256\":\"" + kShaB + "\",\"repo\":\"acme/Vision-GGUF\",\"revision\":\"" +
         std::string{kCommit} + "\"}},"
         "{\"id\":\"glm-small\",\"name\":\"GLM small\",\"source\":\"maya\",\"family\":\"glm53\",\"engine\":\"glm53\","
         "\"about\":\"\",\"repo\":\"acme/GLM-GGUF\",\"revision\":\"main\",\"files\":[{\"path\":\"S/g-00001-of-00001.gguf\",\"size\":null,"
         "\"sha256\":\"" + kShaA + "\"}],\"prepare\":\"unsupported\",\"pack_args\":[],\"expert_profile\":null,\"mmproj\":null},"
         "{\"id\":\"qwen36-small\",\"name\":\"Qwen3.6 small\",\"source\":\"strata\",\"family\":\"qwen36\",\"engine\":\"qwen\","
         "\"about\":\"gguf-direct\",\"repo\":\"acme/Qwen36-GGUF\",\"revision\":\"" + std::string{kCommit} +
         "\",\"files\":[{\"path\":\"Q36/m.gguf\",\"size\":500,\"sha256\":\"" + kShaA +
         "\"}],\"prepare\":\"none\",\"pack_args\":[],\"expert_profile\":null,\"mmproj\":{\"path\":\"mmproj-F16.gguf\","
         "\"size\":200,\"sha256\":\"" + kShaB + "\",\"repo\":\"acme/Qwen36-GGUF\",\"revision\":\"" + std::string{kCommit} + "\"}},"
         "{\"id\":\"glm-maya\",\"name\":\"GLM Maya\",\"source\":\"maya\",\"family\":\"glm53\",\"engine\":\"glm53\","
         "\"about\":\"packed by Maya's step\",\"repo\":\"acme/GLM-GGUF\",\"revision\":\"" + std::string{kCommit} +
         "\",\"files\":[{\"path\":\"S/g-00001-of-00002.gguf\",\"size\":1000,\"sha256\":\"" + kShaA +
         "\"},{\"path\":\"S/g-00002-of-00002.gguf\",\"size\":2000,\"sha256\":\"" + kShaB +
         "\"}],\"prepare\":\"maya_pack\",\"pack_args\":[],\"expert_profile\":null,\"mmproj\":null},"
         "{\"id\":\"ds-maya\",\"name\":\"DeepSeek Maya\",\"source\":\"maya\",\"family\":\"deepseek\",\"engine\":\"deepseek\","
         "\"about\":\"packed by Maya's DeepSeek step\",\"repo\":\"acme/DeepSeek-GGUF\",\"revision\":\"" + std::string{kCommit} +
         "\",\"files\":[{\"path\":\"D/d-00001-of-00002.gguf\",\"size\":1000,\"sha256\":\"" + kShaA +
         "\"},{\"path\":\"D/d-00002-of-00002.gguf\",\"size\":2000,\"sha256\":\"" + kShaB +
         "\"}],\"prepare\":\"maya_pack\",\"pack_args\":[],\"expert_profile\":null,\"mmproj\":null},"
         "{\"id\":\"mm-maya\",\"name\":\"MiniMax Maya\",\"source\":\"maya\",\"family\":\"minimax\",\"engine\":\"minimax\","
         "\"about\":\"packed by Maya's MiniMax step\",\"repo\":\"acme/MiniMax-GGUF\",\"revision\":\"" + std::string{kCommit} +
         "\",\"files\":[{\"path\":\"M/m-IQ1_S-00001-of-00002.gguf\",\"size\":1000,\"sha256\":\"" + kShaA +
         "\"},{\"path\":\"M/m-IQ1_S-00002-of-00002.gguf\",\"size\":2000,\"sha256\":\"" + kShaB +
         "\"},{\"path\":\"M/m-maya-indexers.gguf\",\"size\":100,\"sha256\":\"" + kShaA +
         "\"}],\"prepare\":\"maya_pack\",\"pack_args\":[],\"expert_profile\":null,\"mmproj\":null},"
         // our own pins (owner 2026-10-28): Gemma 4 with its MTP draft file (here from a repository of its own), and
         // raon-1-mini, packed first
         "{\"id\":\"gemma-small\",\"name\":\"Gemma small\",\"source\":\"m-strata\",\"family\":\"gemma4\",\"engine\":\"gemma4\","
         "\"about\":\"its card from the GGUF\",\"repo\":\"acme/Gemma-GGUF\",\"revision\":\"" + std::string{kCommit} +
         "\",\"files\":[{\"path\":\"gemma-small-Q8_0.gguf\",\"size\":1000,\"sha256\":\"" + kShaA +
         "\"}],\"prepare\":\"none\",\"pack_args\":[],\"expert_profile\":null,\"mmproj\":null,\"mtp\":{\"path\":"
         "\"MTP/mtp-gemma-small-Q8_0.gguf\",\"size\":50,\"sha256\":\"" + kShaB + "\",\"repo\":\"acme/Gemma-MTP-GGUF\",\"revision\":\"" +
         std::string{kCommit} + "\"}},"
         "{\"id\":\"raon-small\",\"name\":\"Raon small\",\"source\":\"m-strata\",\"family\":\"raon1\",\"engine\":\"raon1\","
         "\"about\":\"packed first\",\"repo\":\"acme/Raon-GGUF\",\"revision\":\"" + std::string{kCommit} +
         "\",\"files\":[{\"path\":\"raon-small-Q8_0.gguf\",\"size\":2000,\"sha256\":\"" + kShaA +
         "\"}],\"prepare\":\"maya_pack\",\"pack_args\":[],\"expert_profile\":null,\"mmproj\":null,\"mtp\":null}]}";
}

// One catalog entry of the given engine and source, its "mtp" member as written (JSON).
std::string catalog_entry(const std::string& id, const std::string& engine, const std::string& source, const std::string& mtp,
                          const std::string& revision = std::string{kCommit}) {
  return "{\"id\":\"" + id + "\",\"name\":\"N " + id + "\",\"source\":\"" + source + "\",\"family\":\"" + engine + "\",\"engine\":\"" +
         engine + "\",\"about\":\"x\",\"repo\":\"acme/G-GGUF\",\"revision\":\"" + revision + "\",\"files\":[{\"path\":\"g.gguf\",\"size\":" +
         (revision == "main" ? std::string{"null"} : std::string{"1000"}) + ",\"sha256\":\"" + kShaA +
         "\"}],\"prepare\":\"none\",\"pack_args\":[],\"expert_profile\":null,\"mmproj\":null,\"mtp\":" + mtp + "}";
}

struct Fixture final {
  std::filesystem::path dir = secure_frontend_test::make_scratch_directory();
  Spawner spawner;
  std::unique_ptr<DownloadManager> manager;
  bool with_prepare = false;  // set before start(): configures the prepare step + a card template
  bool with_gguf_engine = false;  // set before start(): a downloaded Gemma 4 / flint GGUF becomes a card on this engine
  std::vector<std::string> carded;  // the cards on_card was told about
  std::vector<std::string> events;  // the alerts raised (spec 2026-10-27 §5a)
  std::vector<secure_frontend::ModelCard> held;   // the launcher's cards, as the list reads them (owner 2026-10-28)
  void start(const std::uint64_t free_bytes) {
    for (const char* name : {"downloads", "work", "cards"}) secure_frontend_test::make_private_directory(dir / name);
    open(free_bytes);
  }
  // A later server over the same folders.
  void restart(const std::uint64_t free_bytes) {
    manager.reset();
    open(free_bytes);
  }
  void open(const std::uint64_t free_bytes) {
    secure_frontend::DownloadSettings settings;
    settings.catalog = secure_frontend::parse_download_catalog(catalog_text());
    settings.strata_executable = dir / "strata.exe";
    settings.config_file = dir / "config.json";
    settings.downloads_root = dir / "downloads";
    settings.work_dir = dir / "work";
    settings.cards_dir = dir / "cards";
    settings.child_environment = {{"SystemRoot", "C:\\Windows"}};
    settings.margin_bytes = 100;
    settings.log = [](std::string_view) {};
    if (with_prepare) {
      settings.prepare_python = dir / "python.exe";
      settings.prepare_tools = dir / "tools";
      settings.prepare_script = dir / "prepare.py";
      secure_frontend::DownloadCardTemplate tmpl;
      tmpl.engine_executable = secure_frontend::path_to_utf8(dir / "engine.exe");
      settings.card_template = tmpl;
    }
    if (with_gguf_engine) settings.gguf_engine = dir / "multiengine.exe";
    settings.on_card = [this](const std::string& name, const std::filesystem::path&) {
      carded.push_back(name);
      return true;
    };
    settings.on_event = [this](const std::string_view event, const std::string& subject, const std::string&) {
      events.push_back(std::string{event} + ": " + subject);
    };
    settings.cards = [this] { return held; };
    settings.seams = spawner.seams(free_bytes);
    manager = std::make_unique<DownloadManager>(std::move(settings));
  }
};

std::vector<std::string> pull_lines(const std::string& name) {
  return {"MANIFEST " + name + ":download " + std::string(64, 'c'), "FILE Q/m-00001-of-00002.gguf 1000",
          "BYTES Q/m-00001-of-00002.gguf 500 1000", "BYTES Q/m-00001-of-00002.gguf 1000 1000", "FINISHED Q/m-00001-of-00002.gguf ok",
          "FILE Q/m-00002-of-00002.gguf 2000", "BYTES Q/m-00002-of-00002.gguf 2000 2000", "FINISHED Q/m-00002-of-00002.gguf ok",
          "RESULT ok"};
}

bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

void catalog_cards_and_the_disk_check() {
  Fixture f;
  f.start(1000);
  const std::string listed = f.manager->list_json();
  expect(has(listed, "\"id\":\"qwen-small\"") && has(listed, "\"id\":\"glm-small\"") && has(listed, "\"free_bytes\":1000"),
         "the cards and the free space are listed: " + listed);
  expect(has(listed, "\"size_bytes\":3000") && has(listed, "\"needed_bytes\":3100") && has(listed, "\"fits\":false"),
         "a card that does not fit says so (size, needed with the margin): " + listed);
  const auto refused = f.manager->start("qwen-small");
  expect(refused && refused->status == 507 && refused->code == "insufficient_storage", "a start that does not fit is refused (O2)");
  const auto unknown = f.manager->start("nothing");
  expect(unknown && unknown->status == 404, "an unknown card");
  expect(f.spawner.recorded.empty(), "a refusal spawns nothing");
  expect(has(listed, "the GLM-5.3 engine is not built"), "a GLM card says it is download only");
  // owner 2026-10-27 ("0.0 GB · 3 files"): Maya pins no sizes (they are read from Hugging Face at the start), so a card
  // says whether its size is known rather than showing 0 bytes
  const auto size_known = [&listed](const std::string& id) {
    const auto at = listed.find("{\"id\":\"" + id + "\"");
    const auto flag = at == std::string::npos ? std::string::npos : listed.find("\"size_known\":", at);
    const auto next = at == std::string::npos ? std::string::npos : listed.find("{\"id\":", at + 1U);
    if (flag == std::string::npos || (next != std::string::npos && flag > next)) return std::string{"absent"};
    return listed.substr(flag + 13U, listed.compare(flag + 13U, 4U, "true") == 0 ? 4U : 5U);
  };
  expect(size_known("glm-small") == "false" && size_known("qwen-small") == "true" && size_known("glm-maya") == "true",
         "a card says whether its size is known: glm-small " + size_known("glm-small") + ", qwen-small " + size_known("qwen-small"));
}

void pull_runs_strata_and_tracks_progress() {
  Fixture f;
  f.spawner.pulls.push_back(Script{pull_lines("qwen-small")});
  f.start(1U << 30U);
  expect(!f.manager->start("qwen-small").has_value(), "the start is accepted");
  expect(f.manager->wait_idle(std::chrono::seconds(10)), "the download ends");
  const std::string listed = f.manager->list_json();
  expect(has(listed, "\"id\":\"qwen-small\"") && has(listed, "\"state\":\"installed\"") && has(listed, "\"bytes\":3000"),
         "the card is installed (no prepare step configured): " + listed);
  expect(f.spawner.recorded.size() == 1U, "one child");
  if (f.spawner.recorded.size() != 1U) return;
  const auto& command = f.spawner.recorded[0].command;
  const auto manifest_path = f.dir / "work" / "qwen-small.manifest.json";
  std::ifstream in(manifest_path, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  const std::string digest = secure_frontend::to_hex(secure_frontend::sha256(text.str()));
  const std::vector<std::string> expected{"--config",
                                          secure_frontend::path_to_utf8(f.dir / "config.json"),
                                          "pull",
                                          "--manifest",
                                          secure_frontend::path_to_utf8(manifest_path),
                                          "--trust-manifest",
                                          "sha256:" + digest,
                                          "--yes",
                                          "--progress-lines"};
  expect(command.executable == f.dir / "strata.exe" && command.arguments == expected && !command.insert_serve &&
             command.environment.size() == 1U && command.working_directory == f.dir / "work",
         "strata pull with the written manifest, trusted by its digest, a closed environment");
  secure_frontend::ManifestRules release;
  release.allow_raw_profile = false;
  const auto parsed = secure_frontend::parse_manifest(text.str(), release);
  const auto* manifest = std::get_if<secure_frontend::ParsedManifest>(&parsed);
  expect(manifest != nullptr && manifest->manifest.profile == secure_frontend::ManifestProfile::gguf &&
             manifest->manifest.hosts == std::vector<std::string>{"huggingface.co"} &&
             manifest->manifest.files.size() == 2U &&
             manifest->manifest.files[0].sources ==
                 std::vector<std::string>{"https://huggingface.co/acme/Qwen-GGUF/resolve/" + std::string{kCommit} +
                                          "/Q/m-00001-of-00002.gguf"},
         "the manifest: gguf, Hugging Face only, the pinned commit");
}

void failures_and_cancel() {
  Fixture f;
  f.spawner.pulls.push_back(Script{{"MANIFEST qwen-small:download x", "FILE Q/m-00001-of-00002.gguf 1000", "RESULT digest_mismatch"}, 3});
  f.spawner.pulls.push_back(Script{{"MANIFEST qwen-small:download x", "FILE Q/m-00001-of-00002.gguf 1000",
                                    "BYTES Q/m-00001-of-00002.gguf 10 1000"},
                                   0, true});
  f.start(1U << 30U);
  expect(!f.manager->start("qwen-small").has_value() && f.manager->wait_idle(std::chrono::seconds(10)), "the first attempt runs");
  const std::string failed = f.manager->list_json();
  expect(has(failed, "\"state\":\"failed\"") && has(failed, "SHA-256"), "a digest mismatch fails with a reason: " + failed);
  expect(f.events == std::vector<std::string>{"download_failed: Qwen small did not download"}, "an alert says it failed");
  expect(!f.manager->start("qwen-small").has_value(), "a failed card starts again");
  for (int i = 0; i < 200 && !has(f.manager->list_json(), "\"state\":\"downloading\""); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  expect(has(f.manager->list_json(), "\"active\":{\"id\":\"qwen-small\""), "the active transfer is reported");
  expect(!f.manager->cancel("qwen-small").has_value(), "a cancel is accepted");
  expect(f.manager->wait_idle(std::chrono::seconds(10)), "the cancel ends the transfer");
  expect(has(f.manager->list_json(), "\"state\":\"cancelled\"") && f.spawner.killed->load(), "cancelled, the child killed");
}

void maya_main_is_resolved_and_checked() {
  Fixture f;
  f.spawner.hf.push_back(Script{{"{\"repo\":\"acme/GLM-GGUF\",\"commit\":\"" + std::string{kCommit} +
                                 "\",\"files\":[{\"path\":\"S/g-00001-of-00001.gguf\",\"size\":4000,\"sha256\":\"" + kShaA + "\"}]}"}});
  f.spawner.pulls.push_back(Script{{"RESULT ok"}});
  f.spawner.hf.push_back(Script{{"{\"repo\":\"acme/GLM-GGUF\",\"commit\":\"" + std::string{kCommit} +
                                 "\",\"files\":[{\"path\":\"S/g-00001-of-00001.gguf\",\"size\":4000,\"sha256\":\"" + kShaB + "\"}]}"}});
  f.start(1U << 30U);
  expect(!f.manager->start("glm-small").has_value() && f.manager->wait_idle(std::chrono::seconds(10)), "a Maya card runs");
  expect(f.spawner.recorded.size() == 2U && f.spawner.recorded[0].command.arguments.back() ==
                                                "https://huggingface.co/acme/GLM-GGUF/resolve/main/S/g-00001-of-00001.gguf",
         "main is looked up first through strata hf-info");
  std::ifstream in(f.dir / "work" / "glm-small.manifest.json", std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  expect(has(text.str(), "/resolve/" + std::string{kCommit} + "/S/g-00001-of-00001.gguf") && has(text.str(), "\"size\":4000") &&
             has(text.str(), "\"profile\":\"glm53\""),
         "the manifest pins the resolved commit and size: " + text.str());
  expect(has(f.manager->list_json(), "\"state\":\"installed\""), "installed (GLM: download only)");
  // A second start whose lookup reports another SHA-256 refuses before any pull.
  Fixture g;
  g.spawner.hf.push_back(Script{{"{\"repo\":\"acme/GLM-GGUF\",\"commit\":\"" + std::string{kCommit} +
                                 "\",\"files\":[{\"path\":\"S/g-00001-of-00001.gguf\",\"size\":4000,\"sha256\":\"" + kShaB + "\"}]}"}});
  g.start(1U << 30U);
  expect(!g.manager->start("glm-small").has_value() && g.manager->wait_idle(std::chrono::seconds(10)), "the second runs");
  const std::string listed = g.manager->list_json();
  expect(has(listed, "\"state\":\"failed\"") && has(listed, "no longer match") && g.spawner.recorded.size() == 1U,
         "a changed file is refused before any pull: " + listed);
}

// O2 for Maya's cards: their sizes are known only once "main" is resolved at start, so the free
// space is checked again then, before any pull. A paused Maya download keeps the commit it
// resolved: the resume pulls the same manifest (so the same partial files), with no new lookup.
void maya_sizes_meet_the_free_space_and_resume_keeps_the_commit() {
  const std::string answer = "{\"repo\":\"acme/GLM-GGUF\",\"commit\":\"" + std::string{kCommit} +
                             "\",\"files\":[{\"path\":\"S/g-00001-of-00001.gguf\",\"size\":4000,\"sha256\":\"" + kShaA + "\"}]}";
  Fixture f;
  f.spawner.hf.push_back(Script{{answer}});
  f.start(3000);
  expect(!f.manager->start("glm-small").has_value() && f.manager->wait_idle(std::chrono::seconds(10)), "the start is accepted (no size yet)");
  const std::string refused = f.manager->list_json();
  expect(has(refused, "\"state\":\"failed\"") && has(refused, "Insufficient disk space") && f.spawner.recorded.size() == 1U,
         "the resolved size does not fit: failed before any pull: " + refused);

  Fixture g;
  g.spawner.hf.push_back(Script{{answer}});
  g.spawner.pulls.push_back(Script{{"MANIFEST glm-small:download x", "FILE S/g-00001-of-00001.gguf 4000", "BYTES S/g-00001-of-00001.gguf 900 4000"},
                                   0, true});
  g.spawner.pulls.push_back(Script{{"RESULT ok"}});
  g.start(1U << 30U);
  expect(!g.manager->start("glm-small").has_value(), "a Maya download starts");
  for (int i = 0; i < 200 && !has(g.manager->list_json(), "\"bytes\":900"); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  expect(!g.manager->pause("glm-small").has_value() && g.manager->wait_idle(std::chrono::seconds(10)), "it pauses");
  g.spawner.killed->store(false);  // the fake's kill flag is shared by its children
  expect(!g.manager->start("glm-small").has_value() && g.manager->wait_idle(std::chrono::seconds(10)), "it resumes");
  expect(has(g.manager->list_json(), "\"state\":\"installed\""), "the resumed download completes: " + g.manager->list_json());
  expect(g.spawner.recorded.size() == 3U && g.spawner.recorded[1].command.arguments == g.spawner.recorded[2].command.arguments,
         "one lookup, then the same pull twice (" + std::to_string(g.spawner.recorded.size()) + " children)");
}

// After a Resume the rate counts from the bytes already kept, not from zero (else the first
// MB/s and ETA after a resume would count the whole kept part as one second's transfer).
void a_resume_counts_its_rate_from_the_kept_bytes() {
  Fixture f;
  f.spawner.pulls.push_back(Script{{"MANIFEST qwen-small:download x", "FILE Q/m-00001-of-00002.gguf 1000",
                                    "BYTES Q/m-00001-of-00002.gguf 900 1000"},
                                   0, true});
  f.spawner.pulls.push_back(Script{{"MANIFEST qwen-small:download x", "FILE Q/m-00001-of-00002.gguf 1000",
                                    "BYTES Q/m-00001-of-00002.gguf 900 1000", "#sleep 1100", "BYTES Q/m-00001-of-00002.gguf 950 1000"},
                                   0, true});
  f.start(1U << 30U);
  expect(!f.manager->start("qwen-small").has_value(), "the download starts");
  for (int i = 0; i < 200 && !has(f.manager->list_json(), "\"bytes\":900"); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  expect(!f.manager->pause("qwen-small").has_value() && f.manager->wait_idle(std::chrono::seconds(10)), "it pauses at 900 bytes");
  f.spawner.killed->store(false);
  expect(!f.manager->start("qwen-small").has_value(), "it resumes");
  std::string listed;
  for (int i = 0; i < 300 && !has(listed = f.manager->list_json(), "\"bytes\":950"); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  const auto at = listed.find("\"rate_bps\":");
  const double rate = at == std::string::npos ? -1.0 : std::stod(listed.substr(at + 11U));
  expect(rate > 0.0 && rate < 200.0, "about 50 bytes in 1.1 s, not the kept 950: " + listed.substr(0, 300));
  expect(!f.manager->cancel("qwen-small").has_value() && f.manager->wait_idle(std::chrono::seconds(10)), "a cancel ends it");
}

// Owner 2026-10-28 ("did not even say why it failed"): his Gemma 4 download failed with "the download failed (ok)" - the
// downloader said RESULT ok and both files were in the store, published, but it ended with another exit code. The store
// has the last word: every file there at its pinned size, the download is done; not, the failure names the exit code.
// Every reason the downloader gives is said in words, and so is an end without one.
void a_pull_that_said_ok_is_checked_in_the_store() {
  const auto run = [](const Script& script, const std::optional<std::size_t> stored_bytes) {
    Fixture f;
    f.spawner.pulls.push_back(script);
    const auto installed = f.dir / "installed";
    if (stored_bytes) {
      secure_frontend_test::make_private_directory(installed);
      secure_frontend_test::make_private_directory(installed / "Q36");
      secure_frontend_test::write_file(installed / "Q36" / "m.gguf", std::string(*stored_bytes, 'x'));
    }
    f.start(1U << 30U);
    if (stored_bytes) {
      std::lock_guard lock(f.spawner.mutex);
      f.spawner.installed = installed;
    }
    expect(!f.manager->start("qwen36-small").has_value() && f.manager->wait_idle(std::chrono::seconds(10)), "it runs");
    const std::string listed = f.manager->list_json();
    const auto at = listed.find("{\"id\":\"qwen36-small\"");
    return at == std::string::npos ? std::string{} : listed.substr(at, listed.find("{\"id\":", at + 1U) - at);
  };
  const std::vector<std::string> pulled{"MANIFEST qwen36-small:download " + std::string(64, 'c'), "FILE Q36/m.gguf 500",
                                        "BYTES Q36/m.gguf 500 500", "FINISHED Q36/m.gguf ok", "RESULT ok"};
  const auto kept = run(Script{pulled, 1}, 500U);
  expect(!has(kept, "\"state\":\"failed\"") && (has(kept, "\"state\":\"installed\"") || has(kept, "\"state\":\"ready\"")),
         "said ok, ended with 1, every file in the store: the download is done: " + kept);
  const auto short_file = run(Script{pulled, 1}, 499U);
  expect(has(short_file, "\"state\":\"failed\"") && has(short_file, "exit code 1") && !has(short_file, "(ok)"),
         "a file short in the store: failed, and the exit code is said: " + short_file);
  const auto stalled = run(Script{{"FILE Q36/m.gguf 500", "RESULT stalled"}, 3}, std::nullopt);
  expect(has(stalled, "\"state\":\"failed\"") && has(stalled, "stalled") && has(stalled, "Download again"),
         "a reason is said in words: " + stalled);
  const auto no_result = run(Script{{"FILE Q36/m.gguf 500"}, 9}, std::nullopt);
  expect(has(no_result, "\"state\":\"failed\"") && has(no_result, "exit code 9"), "an end without a result names its exit code: " + no_result);
}

void pasted_links() {
  Fixture f;
  f.spawner.hf.push_back(Script{{"{\"repo\":\"someone/Model-GGUF\",\"commit\":\"" + std::string{kCommit} +
                                 "\",\"files\":[{\"path\":\"model-Q4.gguf\",\"size\":5000,\"sha256\":\"" + kShaA + "\"}]}"}});
  f.spawner.pulls.push_back(Script{{"RESULT ok"}});
  f.start(1U << 30U);
  const auto refused = f.manager->check_link("https://example.com/someone/Model-GGUF/resolve/main/model-Q4.gguf");
  expect(refused.refusal && refused.refusal->status == 400 && !refused.reason.empty() && f.spawner.recorded.empty(),
         "a link off Hugging Face is refused in the server, before any child (O4)");
  const auto not_gguf = f.manager->check_link("https://huggingface.co/someone/Model/resolve/main/model.safetensors");
  expect(not_gguf.refusal && not_gguf.refusal->status == 400 && f.spawner.recorded.empty(), "a file that is not a .gguf (O4)");
  const std::string link = "https://huggingface.co/someone/Model-GGUF/resolve/main/model-Q4.gguf";
  expect(f.manager->start_link(link).has_value(), "a link must be checked before it starts");
  const auto checked = f.manager->check_link(link);
  expect(checked.confirm_json && has(*checked.confirm_json, "\"commit\":\"" + std::string{kCommit} + "\"") &&
             has(*checked.confirm_json, "\"size_bytes\":5000") && has(*checked.confirm_json, "\"fits\":true"),
         "the confirmation: commit, files, size, fits");
  expect(!f.manager->start_link(link).has_value() && f.manager->wait_idle(std::chrono::seconds(10)), "the confirmed link downloads");
  const std::string listed = f.manager->list_json();
  expect(has(listed, "\"source\":\"custom\"") && has(listed, "\"state\":\"installed\""), "a custom card, installed: " + listed);
  // It survives a restart (custom.json under the work folder).
  f.manager.reset();
  secure_frontend::DownloadSettings again;
  again.catalog = secure_frontend::parse_download_catalog(catalog_text());
  again.strata_executable = f.dir / "strata.exe";
  again.config_file = f.dir / "config.json";
  again.downloads_root = f.dir / "downloads";
  again.work_dir = f.dir / "work";
  again.cards_dir = f.dir / "cards";
  again.log = [](std::string_view) {};
  again.seams = f.spawner.seams(1U << 30U);
  DownloadManager restarted(std::move(again));
  expect(has(restarted.list_json(), "\"id\":\"hf-model-gguf-model-q4\""), "the custom card is back after a restart");
}


// Owner: "Have a download pause and resume functionality". Pause stops the transfer and keeps
// the partial files (SP4's pull resumes from them); Resume is a start of the same manifest (the
// same digest, so the same staging); Cancel stops and discards the partials (`strata rm
// --partials <id>:download`).
// Owner 2026-10-23: "why does qwen 3.6 35B says download only ... it is the same engine as qwen 3.8
// flash next". Qwen3.6 (family qwen36) reads the GGUF directly (OWN_PATH); with the prepare step
// configured it is runnable, and its note says a card is made (only the tokenizer is prepared).
void qwen36_is_runnable_with_a_tokenizer_note() {
  Fixture f;
  f.with_prepare = true;
  f.start(1U << 30U);
  const std::string listed = f.manager->list_json();
  const auto at = listed.find("\"id\":\"qwen36-small\"");
  expect(at != std::string::npos, "the Qwen3.6 card is listed");
  const std::string card = at == std::string::npos ? std::string{} : listed.substr(at, listed.find('}', at) - at);
  expect(has(card, "\"runnable\":true"), "Qwen3.6 is runnable (same engine, GGUF-direct): " + card);
  expect(has(card, "its tokenizer is prepared"), "its note says a card is made from the tokenizer: " + card);
  // The iq_pack card still says a pack is built, and the GLM card is still download-only.
  const auto qw = listed.find("\"id\":\"qwen-small\"");
  expect(has(listed.substr(qw, listed.find('}', qw) - qw), "\"runnable\":true") &&
             has(listed.substr(qw, listed.find('}', qw) - qw), "the pack is built"),
         "the iq_pack card is unchanged");
  const auto glm = listed.find("\"id\":\"glm-small\"");
  expect(has(listed.substr(glm, listed.find('}', glm) - glm), "\"runnable\":false"), "the GLM card is still download only");
}

// Owner 2026-10-23: "for custom download from huggingface, the downloads must be matched to a
// current settings we have available like qwen 3.8 flash next, swift, gemma4, qwen3.6 etc." A
// pasted GGUF is matched to a family so a runnable card is made; an unknown family and more than
// five extra part links are refused, and the picker's families are advertised in the list.
void a_pasted_gguf_is_matched_to_a_profile() {
  Fixture f;
  f.spawner.hf.push_back(Script{{"{\"repo\":\"someone/Qwen36-Finetune-GGUF\",\"commit\":\"" + std::string{kCommit} +
                                 "\",\"files\":[{\"path\":\"ft-Q4.gguf\",\"size\":5000,\"sha256\":\"" + kShaA + "\"}]}"}});
  f.start(1U << 30U);
  const std::string link = "https://huggingface.co/someone/Qwen36-Finetune-GGUF/resolve/main/ft-Q4.gguf";
  const auto matched = f.manager->check_link(link, "qwen36", {});
  expect(matched.confirm_json && has(*matched.confirm_json, "\"family\":\"qwen36\"") &&
             has(*matched.confirm_json, "\"runnable\":true") && has(*matched.confirm_json, "\"complete\":true") &&
             has(*matched.confirm_json, "\"parts\":1"),
         "a single GGUF matched to qwen36 confirms runnable and complete: " + matched.confirm_json.value_or("<none>"));
  const auto unknown = f.manager->check_link(link, "not-a-family", {});
  expect(unknown.refusal && unknown.refusal->status == 400, "an unknown family is refused");
  const auto too_many = f.manager->check_link(link, "qwen", {"a", "b", "c", "d", "e", "f"});
  expect(too_many.refusal && too_many.refusal->status == 400 && has(too_many.reason, "five"),
         "more than five extra part links are refused");
  const std::string listed = f.manager->list_json();
  expect(has(listed, "\"profiles\":[") && has(listed, "\"key\":\"qwen\"") && has(listed, "\"key\":\"swift\"") &&
             has(listed, "\"key\":\"qwen36\""),
         "the matchable families are advertised: " + listed);
  expect(has(listed, "\"key\":\"gemma4\",\"display\":\"Gemma 4\",\"engine\":\"gemma4\",\"runnable\":false"),
         "Gemma 4 is listed but not runnable (no tokenizer from a bare GGUF)");
}

std::string json_of(const std::filesystem::path& path) {
  std::string out;
  secure_frontend::append_json_string(&out, secure_frontend::path_to_utf8(path));
  return out;
}

// GUI-first start-up (spec 2026-10-27 §4.3): a download goes to the drive or folder the page chose - its free space is
// checked there first, strata pulls into it (--store), and the choice is kept for a resume after a restart.
void a_download_goes_to_the_chosen_folder() {
  Fixture f;
  f.spawner.pulls.push_back(Script{pull_lines("qwen-small")});
  for (const char* name : {"elsewhere", "small"}) secure_frontend_test::make_private_directory(f.dir / name);
  f.spawner.free_by_path[f.dir / "small"] = 10;
  f.start(1U << 30U);
  const auto tight = f.manager->start("qwen-small", f.dir / "small");
  expect(tight && tight->status == 507 && tight->code == "insufficient_storage", "the chosen folder's own drive is checked");
  const auto missing = f.manager->start("qwen-small", f.dir / "nope");
  expect(missing && missing->status == 422 && missing->code == "folder_refused", "a folder that is not there is refused");
  expect(f.spawner.recorded.empty(), "a refusal spawns nothing");
  expect(!f.manager->start("qwen-small", f.dir / "elsewhere").has_value(), "a start into a folder with room");
  expect(f.manager->wait_idle(std::chrono::seconds(10)), "the download ends");
  expect(f.spawner.recorded.size() == 1U, "one child");
  if (f.spawner.recorded.size() != 1U) return;
  const auto& arguments = f.spawner.recorded[0].command.arguments;
  expect(arguments.size() > 5U && arguments[0] == "--config" && arguments[2] == "--store" &&
             arguments[3] == secure_frontend::path_to_utf8(f.dir / "elsewhere") && arguments[4] == "pull",
         "strata pulls into the chosen folder (--store)");
  const std::string listed = f.manager->list_json();
  expect(has(listed, "\"folder\":" + json_of(f.dir / "elsewhere")) && has(listed, "\"file\":\"m-00001-of-00002\""),
         "the list shows each download's folder and model file: " + listed);
  expect(has(listed, ",\"folder\":" + json_of(f.dir / "downloads") + ",\"free_bytes\":"),
         "the disk line names the default folder (models.downloads_root): " + listed);
  f.restart(1U << 30U);
  expect(has(f.manager->list_json(), "\"folder\":" + json_of(f.dir / "elsewhere")), "the folder is kept for a later server");
}

// §4.3, §4.3a: a Gemma 4 or flint GGUF from a pasted link becomes a card the moment it is verified - the card read from
// its header, its source the download (the Download list then hides it) - so the model is ready to start.
void a_downloaded_gguf_becomes_a_card() {
  Fixture f;
  f.with_gguf_engine = true;
  f.spawner.hf.push_back(Script{{"{\"repo\":\"someone/Flint-GGUF\",\"commit\":\"" + std::string{kCommit} +
                                 "\",\"files\":[{\"path\":\"flint-Q8_0.gguf\",\"size\":5000,\"sha256\":\"" + kShaA + "\"}]}"}});
  f.spawner.pulls.push_back(
      Script{{"FILE flint-Q8_0.gguf 5000", "BYTES flint-Q8_0.gguf 5000 5000", "FINISHED flint-Q8_0.gguf ok", "RESULT ok"}});
  const auto store = f.dir / "elsewhere";
  const auto installed = store / "v1";
  secure_frontend_test::make_private_directory(store);
  secure_frontend_test::make_private_directory(installed);
  secure_frontend_test::write_file(installed / "flint-Q8_0.gguf",
                                   test_support::gguf_with_tokenizer(test_support::byte_tokenizer(), "gpt2", "flint")
                                       .u32("flint.context_length", 262144U)
                                       .bytes());
  f.spawner.installed = installed;
  f.start(1U << 30U);
  const std::string link = "https://huggingface.co/someone/Flint-GGUF/resolve/main/flint-Q8_0.gguf";
  const auto matched = f.manager->check_link(link, "flint", {}, store);
  expect(matched.confirm_json && has(*matched.confirm_json, "\"runnable\":true"),
         "a flint GGUF is runnable: " + matched.confirm_json.value_or("<none>"));
  expect(!f.manager->start_link(link, store).has_value() && f.manager->wait_idle(std::chrono::seconds(10)), "it downloads");
  const std::string listed = f.manager->list_json();
  expect(has(listed, "\"state\":\"ready\""), "ready - a card was made: " + listed);
  expect(f.carded == std::vector<std::string>{"flint-Q8_0"}, "the launcher was given the card");
  expect(f.events.size() == 1U && f.events[0].rfind("download_finished: ", 0) == 0U, "an alert says it finished");
  const auto read = secure_frontend::read_model_card(f.dir / "cards" / "hf-flint-gguf-flint-q8_0.json");
  const auto* card = std::get_if<secure_frontend::ModelCard>(&read);
  expect(card != nullptr && card->profile == secure_frontend::EngineProfile::flint && card->source == "download:hf-flint-gguf-flint-q8_0" &&
             std::get<std::string>(card->config.entries.at("roots.models").value) == secure_frontend::path_to_utf8(installed),
         "the card: flint, from that download, its folder the installed version");
  Fixture g;
  g.start(1U << 30U);
  expect(has(g.manager->list_json(), "\"key\":\"flint\",\"display\":\"flint\",\"engine\":\"flint\",\"runnable\":false"),
         "without models.default_engine a flint GGUF stays download only");
}

// Owner 2026-10-27 ("Qwen 3.6 from the GGUF first"): with an engine for new cards (models.default_engine) a Qwen3.6
// download becomes a card straight from its GGUF, like Gemma 4 and flint - no prepare step runs, even where one is
// configured.
void a_downloaded_qwen36_becomes_a_card() {
  Fixture f;
  f.with_gguf_engine = true;
  f.with_prepare = true;   // configured, and still not used
  f.spawner.pulls.push_back(Script{{"MANIFEST qwen36-small:download " + std::string(64, 'c'), "FILE Q36/m.gguf 500",
                                    "BYTES Q36/m.gguf 500 500", "FINISHED Q36/m.gguf ok", "RESULT ok"}});
  const auto installed = f.dir / "installed";
  secure_frontend_test::make_private_directory(installed);
  secure_frontend_test::make_private_directory(installed / "Q36");  // the store keeps the catalog's subfolder
  secure_frontend_test::write_file(installed / "Q36" / "m.gguf", test_support::gguf_with_tokenizer(test_support::byte_tokenizer(), "gpt2", "qwen35moe")
                                                             .u32("qwen35moe.context_length", 262144U)
                                                             .u32("qwen35moe.nextn_predict_layers", 1U)
                                                             .bytes());
  f.start(1U << 30U);
  const std::string before = f.manager->list_json();
  const auto at = before.find("\"id\":\"qwen36-small\"");
  const std::string entry = at == std::string::npos ? std::string{} : before.substr(at, before.find('}', at) - at);
  expect(has(entry, "\"runnable\":true") && has(entry, "a card is made from the GGUF"), "runnable, from the GGUF: " + entry);
  {
    std::lock_guard lock(f.spawner.mutex);
    f.spawner.installed = installed;   // where the store puts it
  }
  expect(!f.manager->start("qwen36-small").has_value() && f.manager->wait_idle(std::chrono::seconds(10)), "it downloads");
  expect(f.spawner.recorded.size() == 1U, "one child, the pull: no prepare step");
  expect(f.carded == std::vector<std::string>{"m"}, "the launcher was given the card");
  const auto read = secure_frontend::read_model_card(f.dir / "cards" / "qwen36-small.json");
  const auto* card = std::get_if<secure_frontend::ModelCard>(&read);
  expect(card != nullptr && card->profile == secure_frontend::EngineProfile::qwen && card->source == "download:qwen36-small" &&
             !card->config.entries.contains("roots.tokenizer_pack") &&
             std::get<std::string>(card->config.entries.at("roots.models").value) == secure_frontend::path_to_utf8(installed / "Q36"),
         "the card: qwen, from that download, its tokenizer from the GGUF, the file's own folder");
}

// Owner 2026-10-27 ("executables to handle what needs to be done so we don't have any other dependencies"): with an
// engine for new cards and strata-prepare beside it, a Qwen3.8 download (iq_pack) is packed natively - `strata-prepare
// download`, no Python, even where Python is configured - into the download's own store, and its card is written: the
// pack, the PLE shard (two shards at most, as upstream's setup), the bundled expert profile, the tokenizer from the
// GGUF. Deleting the model takes the pack with it.
void a_downloaded_qwen38_is_packed_natively() {
  Fixture f;
  f.with_gguf_engine = true;
  f.with_prepare = true;   // configured, and still not used
#if defined(_WIN32)
  const auto tool = f.dir / "strata-prepare.exe";
#else
  const auto tool = f.dir / "strata-prepare";
#endif
  secure_frontend_test::write_file(tool, "tool");
  secure_frontend_test::make_private_directory(f.dir / "profiles");
  secure_frontend_test::write_file(f.dir / "profiles" / "expert-profile.bin", "profile");
  f.spawner.pulls.push_back(Script{pull_lines("qwen-small")});
  f.spawner.pulls.push_back(Script{{"STEP pack", "STEP ple", "DONE"}});
  const auto installed = f.dir / "installed";
  secure_frontend_test::make_private_directory(installed);
  const auto shards = installed / "Q";  // the store keeps the catalog's subfolder (pull_lines: Q/m-...)
  secure_frontend_test::make_private_directory(shards);
  f.spawner.on_spawn = [&](const secure_frontend::EngineCommand& command) {
    if (command.executable != tool) return;
    const auto& a = command.arguments;
    const auto out = std::find(a.begin(), a.end(), "--out");
    if (out == a.end() || out + 1 == a.end()) return;
    const std::filesystem::path pack = secure_frontend::path_from_utf8(*(out + 1));
    std::filesystem::create_directories(pack);
    secure_frontend_test::write_file(pack / "prepared.json",
                                     "{\"pack\": " + json_of(pack) + ", \"tokenizer\": null, \"ple\": " +
                                         json_of(shards / "m-00002-of-00002.gguf") + ", \"shards\": 2}\n");
  };
  f.start(1U << 30U);
  const std::string before = f.manager->list_json();
  const auto at = before.find("\"id\":\"qwen-small\"");
  const std::string entry = at == std::string::npos ? std::string{} : before.substr(at, before.find('}', at) - at);
  expect(has(entry, "\"runnable\":true") && has(entry, "the pack is built"), "runnable, packed natively: " + entry);
  {
    std::lock_guard lock(f.spawner.mutex);
    f.spawner.installed = installed;
  }
  expect(!f.manager->start("qwen-small").has_value() && f.manager->wait_idle(std::chrono::seconds(10)), "it downloads");
  const std::string listed = f.manager->list_json();
  expect(has(listed, "\"state\":\"ready\""), "ready - a card was made: " + listed);
  expect(f.spawner.recorded.size() == 2U, "two children: the pull and strata-prepare");
  if (f.spawner.recorded.size() != 2U) return;
  const auto& prepare = f.spawner.recorded[1].command;
  const auto pack = f.dir / "downloads" / "packs" / "qwen-small";
  expect(prepare.executable == tool &&
             prepare.arguments == std::vector<std::string>{"download", "--gguf", secure_frontend::path_to_utf8(shards / "m-00001-of-00002.gguf"),
                                                           "--out", secure_frontend::path_to_utf8(pack)},
         "strata-prepare download, the first shard, the pack in the download's store");
  const auto read = secure_frontend::read_model_card(f.dir / "cards" / "qwen-small.json");
  const auto* card = std::get_if<secure_frontend::ModelCard>(&read);
  expect(card != nullptr, "the card reads");
  if (card == nullptr) return;
  const auto& rows = card->config.entries;
  const auto args = std::get<std::vector<std::string>>(rows.at("engine.args").value);
  expect(card->profile == secure_frontend::EngineProfile::qwen && card->source == "download:qwen-small" &&
             !rows.contains("roots.tokenizer_pack") &&
             std::get<std::string>(rows.at("engine.executable").value) == secure_frontend::path_to_utf8(f.dir / "multiengine.exe") &&
             std::get<std::string>(rows.at("roots.models").value) == secure_frontend::path_to_utf8(shards) &&
             std::get<std::string>(rows.at("engine.model_id").value) == "m-00001-of-00002",
         "the card: qwen, from that download, the engine for new cards, the shards' folder, the tokenizer from the GGUF");
  expect(args == std::vector<std::string>{"--pack", secure_frontend::path_to_utf8(pack), "--ple-gguf",
                                          secure_frontend::path_to_utf8(shards / "m-00002-of-00002.gguf"), "--expert-profile",
                                          secure_frontend::path_to_utf8(f.dir / "profiles" / "expert-profile.bin")},
         "its engine arguments: the pack, the PLE shard, the bundled expert profile");
  // the delete takes the pack too
  f.spawner.pulls.push_back(Script{{"removed qwen-small:download"}});
  expect(!f.manager->remove_installed("qwen-small").has_value() && !std::filesystem::exists(pack), "removed, the pack with it");
}

// Owner 2026-10-27 ("Yes continue with the others": GLM after Qwen): with an engine for new cards and strata-prepare
// beside it, a GLM-5.3 download (prepare maya_pack) is packed as Maya's setup packs it - `strata-prepare download
// --maya`, the pack in a folder beside the shards, as the engine reads them (<pack>/..). The store's own version folder
// is never written: <store>/packs/<id> holds hard links to its shards (no copy) and the pack in pack/. The card: profile
// glm53, the pack as roots.models with Maya's model name as its marker file, the pack's tokenizer, Maya's GLM options
// (contexts 8K-128K, default 32K; temperature 1.0, top_p 0.95). Deleting the model takes that folder with it.
void a_downloaded_glm_is_packed_with_mayas_step() {
  Fixture f;
  f.with_gguf_engine = true;
#if defined(_WIN32)
  const auto tool = f.dir / "strata-prepare.exe";
#else
  const auto tool = f.dir / "strata-prepare";
#endif
  f.spawner.pulls.push_back(Script{{"MANIFEST glm-maya:download " + std::string(64, 'c'), "FILE S/g-00001-of-00002.gguf 1000",
                                    "FINISHED S/g-00001-of-00002.gguf ok", "FILE S/g-00002-of-00002.gguf 2000",
                                    "FINISHED S/g-00002-of-00002.gguf ok", "RESULT ok"}});
  f.spawner.pulls.push_back(Script{{"STEP pack", "STEP sources", "DONE"}});
  const auto installed = f.dir / "installed";
  secure_frontend_test::make_private_directory(installed);
  secure_frontend_test::make_private_directory(installed / "S");  // the store keeps the catalog's subfolder
  secure_frontend_test::write_file(installed / "S" / "g-00001-of-00002.gguf", "shard one");
  secure_frontend_test::write_file(installed / "S" / "g-00002-of-00002.gguf", "shard two");
  const auto folder = f.dir / "downloads" / "packs" / "glm-maya";
  bool linked = false;
  f.spawner.on_spawn = [&](const secure_frontend::EngineCommand& command) {
    if (command.executable != tool) return;
    std::error_code ec;
    linked = std::filesystem::equivalent(folder / "g-00001-of-00002.gguf", installed / "S" / "g-00001-of-00002.gguf", ec) &&
             std::filesystem::equivalent(folder / "g-00002-of-00002.gguf", installed / "S" / "g-00002-of-00002.gguf", ec) &&
             std::filesystem::hard_link_count(installed / "S" / "g-00001-of-00002.gguf", ec) == 2U;
    const auto pack = folder / "pack";
    std::filesystem::create_directories(pack / "tokenizer");
    secure_frontend_test::write_file(pack / "prepared.json", "{\"pack\": " + json_of(pack) + ", \"tokenizer\": " + json_of(pack / "tokenizer") +
                                                                 ", \"ple\": null, \"shards\": 2}\n");
  };
  f.start(1U << 30U);
  const auto entry_of = [&f]() {
    const std::string listed = f.manager->list_json();
    const auto at = listed.find("\"id\":\"glm-maya\"");
    return at == std::string::npos ? std::string{} : listed.substr(at, listed.find('}', at) - at);
  };
  expect(has(entry_of(), "\"runnable\":false") && has(entry_of(), "download only"), "without strata-prepare: download only: " + entry_of());
  secure_frontend_test::write_file(tool, "tool");
  expect(has(entry_of(), "\"runnable\":true") && has(entry_of(), "the GLM pack is built"), "runnable, packed by Maya's step: " + entry_of());
  {
    std::lock_guard lock(f.spawner.mutex);
    f.spawner.installed = installed;
  }
  expect(!f.manager->start("glm-maya").has_value() && f.manager->wait_idle(std::chrono::seconds(10)), "it downloads");
  expect(has(entry_of(), "\"state\":\"ready\""), "ready - a card was made: " + entry_of());
  expect(f.spawner.recorded.size() == 2U, "two children: the pull and strata-prepare");
  if (f.spawner.recorded.size() != 2U) return;
  const auto& prepare = f.spawner.recorded[1].command;
  expect(prepare.executable == tool &&
             prepare.arguments == std::vector<std::string>{"download", "--maya", "--gguf",
                                                           secure_frontend::path_to_utf8(folder / "g-00001-of-00002.gguf"), "--out",
                                                           secure_frontend::path_to_utf8(folder / "pack")},
         "strata-prepare download --maya: the linked first shard, the pack beside the shards");
  expect(linked, "the shards are hard links to the store's files when the pack is made");
  expect(std::filesystem::exists(folder / "pack" / "glm-5.3-flash.gguf"), "the pack's marker: Maya's model name, the card's model file");
  const auto read = secure_frontend::read_model_card(f.dir / "cards" / "glm-maya.json");
  const auto* card = std::get_if<secure_frontend::ModelCard>(&read);
  expect(card != nullptr, "the card reads");
  if (card == nullptr) {
    for (const auto& issue : std::get<secure_frontend::ConfigErrors>(read)) std::cerr << "  " << issue.key_path << ": " << issue.reason << '\n';
    return;
  }
  const auto& rows = card->config.entries;
  expect(card->profile == secure_frontend::EngineProfile::glm53 && card->source == "download:glm-maya" && !rows.contains("engine.args") &&
             std::get<std::string>(rows.at("engine.executable").value) == secure_frontend::path_to_utf8(f.dir / "multiengine.exe") &&
             std::get<std::string>(rows.at("roots.models").value) == secure_frontend::path_to_utf8(folder / "pack") &&
             std::get<std::string>(rows.at("roots.tokenizer_pack").value) == secure_frontend::path_to_utf8(folder / "pack" / "tokenizer") &&
             std::get<std::string>(rows.at("engine.model_id").value) == "glm-5.3-flash",
         "the card: glm53, from that download, the engine for new cards, the pack and its tokenizer, Maya's model name");
  const auto option = [card](const std::string_view name) -> const secure_frontend::ModelOptionLimits* {
    for (const auto& limit : card->options) {
      if (limit.spec != nullptr && limit.spec->name == name) return &limit;
    }
    return nullptr;
  };
  const auto* context = option("max_context");
  const auto* temperature = option("temperature");
  expect(context != nullptr && std::get<std::int64_t>(context->default_value) == 32768 && context->minimum == 8192 && context->maximum == 131072 &&
             option("kv") != nullptr && temperature != nullptr && std::get<double>(temperature->default_value) == 1.0 &&
             option("top_p") != nullptr,
         "Maya's GLM options: contexts 8K-128K (32K), the KV format, temperature 1.0 and top_p 0.95");
  f.spawner.pulls.push_back(Script{{"removed glm-maya:download"}});
  expect(!f.manager->remove_installed("glm-maya").has_value() && !std::filesystem::exists(folder) &&
             std::filesystem::exists(installed / "S" / "g-00001-of-00002.gguf"),
         "removed: the links and the pack go, the store's own files are the store's to remove");
}

// DeepSeek-V4.1 (2026-10-27, spec 2026-10-27-strata-prepare-design §7.2): a DeepSeek download is packed by Maya's DeepSeek
// step (maya.py ds4_pack_step; strata-prepare download --maya on its architecture), whose ds4.txt names the model's folder:
// the pack is <store>/packs/<id> and reads the store's version folder in place (no links), Maya's shipped profile beside
// the engine handed to it to seed. Then the card: deepseek on the pack, Maya's model name its marker.
void a_downloaded_deepseek_is_packed_with_mayas_step() {
  Fixture f;
  f.with_gguf_engine = true;
#if defined(_WIN32)
  const auto tool = f.dir / "strata-prepare.exe";
#else
  const auto tool = f.dir / "strata-prepare";
#endif
  secure_frontend_test::write_file(tool, "tool");
  secure_frontend_test::make_private_directory(f.dir / "profiles");
  secure_frontend_test::write_file(f.dir / "profiles" / "expert-profile-ds4.bin", "profile");
  f.spawner.pulls.push_back(Script{{"MANIFEST ds-maya:download " + std::string(64, 'c'), "FILE D/d-00001-of-00002.gguf 1000",
                                    "FINISHED D/d-00001-of-00002.gguf ok", "FILE D/d-00002-of-00002.gguf 2000",
                                    "FINISHED D/d-00002-of-00002.gguf ok", "RESULT ok"}});
  f.spawner.pulls.push_back(Script{{"STEP pack", "STEP sources", "DONE"}});
  const auto installed = f.dir / "installed";
  secure_frontend_test::make_private_directory(installed);
  secure_frontend_test::make_private_directory(installed / "D");  // the store keeps the catalog's subfolder
  secure_frontend_test::write_file(installed / "D" / "d-00001-of-00002.gguf", "shard one");
  secure_frontend_test::write_file(installed / "D" / "d-00002-of-00002.gguf", "shard two");
  const auto pack = f.dir / "downloads" / "packs" / "ds-maya";
  f.spawner.on_spawn = [&](const secure_frontend::EngineCommand& command) {
    if (command.executable != tool) return;
    std::filesystem::create_directories(pack / "tokenizer");
    secure_frontend_test::write_file(pack / "prepared.json", "{\"pack\": " + json_of(pack) + ", \"tokenizer\": " + json_of(pack / "tokenizer") +
                                                                 ", \"ple\": null, \"shards\": 2}\n");
  };
  f.start(1U << 30U);
  const auto entry_of = [&f]() {
    const std::string listed = f.manager->list_json();
    const auto at = listed.find("\"id\":\"ds-maya\"");
    return at == std::string::npos ? std::string{} : listed.substr(at, listed.find('}', at) - at);
  };
  expect(has(entry_of(), "\"runnable\":true") && has(entry_of(), "the DeepSeek pack is built"), "runnable, packed by Maya's step: " + entry_of());
  {
    std::lock_guard lock(f.spawner.mutex);
    f.spawner.installed = installed;
  }
  expect(!f.manager->start("ds-maya").has_value() && f.manager->wait_idle(std::chrono::seconds(10)), "it downloads");
  expect(has(entry_of(), "\"state\":\"ready\""), "ready - a card was made: " + entry_of());
  expect(f.spawner.recorded.size() == 2U, "two children: the pull and strata-prepare");
  if (f.spawner.recorded.size() != 2U) return;
  expect(f.spawner.recorded[1].command.arguments ==
             std::vector<std::string>{"download", "--maya", "--gguf", secure_frontend::path_to_utf8(installed / "D" / "d-00001-of-00002.gguf"),
                                      "--out", secure_frontend::path_to_utf8(pack), "--expert-profile",
                                      secure_frontend::path_to_utf8(f.dir / "profiles" / "expert-profile-ds4.bin")},
         "strata-prepare download --maya: the store's first shard in place, the pack in <store>/packs/<id>, the profile to seed");
  expect(!std::filesystem::exists(pack / "d-00001-of-00002.gguf") && std::filesystem::exists(pack / "deepseek-v4.1-flash.gguf"),
         "no links beside the pack; its marker is Maya's model name");
  const auto read = secure_frontend::read_model_card(f.dir / "cards" / "ds-maya.json");
  const auto* card = std::get_if<secure_frontend::ModelCard>(&read);
  expect(card != nullptr, "the card reads");
  if (card == nullptr) {
    for (const auto& issue : std::get<secure_frontend::ConfigErrors>(read)) std::cerr << "  " << issue.key_path << ": " << issue.reason << '\n';
    return;
  }
  const auto& rows = card->config.entries;
  expect(card->profile == secure_frontend::EngineProfile::deepseek && card->source == "download:ds-maya" &&
             std::get<std::string>(rows.at("roots.models").value) == secure_frontend::path_to_utf8(pack) &&
             std::get<std::string>(rows.at("roots.tokenizer_pack").value) == secure_frontend::path_to_utf8(pack / "tokenizer") &&
             std::get<std::string>(rows.at("engine.model_id").value) == "deepseek-v4.1-flash",
         "the card: deepseek, from that download, the pack and its tokenizer, Maya's model name");
  f.spawner.pulls.push_back(Script{{"removed ds-maya:download"}});
  expect(!f.manager->remove_installed("ds-maya").has_value() && !std::filesystem::exists(pack) &&
             std::filesystem::exists(installed / "D" / "d-00001-of-00002.gguf"),
         "removed: the pack goes, the store's own files are the store's to remove");
}

// MiniMax-M3.1-Flash (owner 2026-10-28): a MiniMax download is packed by Maya's MiniMax step (maya.py mm3_pack_step;
// strata-prepare download --maya on its architecture) with Maya's chat template the install ships beside the engine
// (engine/templates/minimax-m3.1): the pack is <store>/packs/<id>, reading the store's version folder in place (mm3.txt
// names it). Then the card: minimax on the pack, Maya's model name its marker. The entry says which licence it comes
// under, and without the shipped template it is not offered as runnable.
void a_downloaded_minimax_is_packed_with_mayas_step() {
  Fixture f;
  f.with_gguf_engine = true;
#if defined(_WIN32)
  const auto tool = f.dir / "strata-prepare.exe";
#else
  const auto tool = f.dir / "strata-prepare";
#endif
  secure_frontend_test::write_file(tool, "tool");
  f.spawner.pulls.push_back(Script{{"MANIFEST mm-maya:download " + std::string(64, 'c'), "FILE M/m-IQ1_S-00001-of-00002.gguf 1000",
                                    "FINISHED M/m-IQ1_S-00001-of-00002.gguf ok", "FILE M/m-IQ1_S-00002-of-00002.gguf 2000",
                                    "FINISHED M/m-IQ1_S-00002-of-00002.gguf ok", "FILE M/m-maya-indexers.gguf 100",
                                    "FINISHED M/m-maya-indexers.gguf ok", "RESULT ok"}});
  f.spawner.pulls.push_back(Script{{"STEP pack", "STEP sources", "DONE"}});
  const auto installed = f.dir / "installed";
  secure_frontend_test::make_private_directory(installed);
  secure_frontend_test::make_private_directory(installed / "M");  // the store keeps the catalog's subfolder
  secure_frontend_test::write_file(installed / "M" / "m-IQ1_S-00001-of-00002.gguf", "shard one");
  secure_frontend_test::write_file(installed / "M" / "m-IQ1_S-00002-of-00002.gguf", "shard two");
  secure_frontend_test::write_file(installed / "M" / "m-maya-indexers.gguf", "indexers");
  const auto pack = f.dir / "downloads" / "packs" / "mm-maya";
  f.spawner.on_spawn = [&](const secure_frontend::EngineCommand& command) {
    if (command.executable != tool) return;
    std::filesystem::create_directories(pack / "tokenizer");
    secure_frontend_test::write_file(pack / "prepared.json", "{\"pack\": " + json_of(pack) + ", \"tokenizer\": " + json_of(pack / "tokenizer") +
                                                                 ", \"ple\": null, \"shards\": 2}\n");
  };
  f.start(1U << 30U);
  const auto entry_of = [&f]() {
    const std::string listed = f.manager->list_json();
    const auto at = listed.find("\"id\":\"mm-maya\"");
    return at == std::string::npos ? std::string{} : listed.substr(at, listed.find('}', at) - at);
  };
  expect(has(entry_of(), "\"runnable\":false"), "without the shipped template it is not runnable here: " + entry_of());
  const auto templates = f.dir / "templates" / "minimax-m3.1";
  secure_frontend_test::make_private_directory(f.dir / "templates");
  secure_frontend_test::make_private_directory(templates);
  secure_frontend_test::write_file(templates / "chat_template.jinja", "]~!b[");
  expect(has(entry_of(), "\"runnable\":true") && has(entry_of(), "the MiniMax pack is built") && has(entry_of(), "MiniMax Community License"),
         "runnable once the template is there, packed by Maya's step, its licence named: " + entry_of());
  {
    std::lock_guard lock(f.spawner.mutex);
    f.spawner.installed = installed;
  }
  expect(!f.manager->start("mm-maya").has_value() && f.manager->wait_idle(std::chrono::seconds(10)), "it downloads");
  expect(has(entry_of(), "\"state\":\"ready\""), "ready - a card was made: " + entry_of());
  expect(f.spawner.recorded.size() == 2U, "two children: the pull and strata-prepare");
  if (f.spawner.recorded.size() != 2U) return;
  expect(f.spawner.recorded[1].command.arguments ==
             std::vector<std::string>{"download", "--maya", "--gguf", secure_frontend::path_to_utf8(installed / "M" / "m-IQ1_S-00001-of-00002.gguf"),
                                      "--out", secure_frontend::path_to_utf8(pack), "--template",
                                      secure_frontend::path_to_utf8(templates / "chat_template.jinja")},
         "strata-prepare download --maya: the store's first shard in place, the pack in <store>/packs/<id>, Maya's template");
  expect(std::filesystem::exists(pack / "minimax-m3.1-flash.gguf"), "its marker is Maya's model name");
  const auto read = secure_frontend::read_model_card(f.dir / "cards" / "mm-maya.json");
  const auto* card = std::get_if<secure_frontend::ModelCard>(&read);
  expect(card != nullptr, "the card reads");
  if (card == nullptr) return;
  const auto& rows = card->config.entries;
  expect(card->profile == secure_frontend::EngineProfile::minimax && card->source == "download:mm-maya" &&
             std::get<std::string>(rows.at("roots.models").value) == secure_frontend::path_to_utf8(pack) &&
             std::get<std::string>(rows.at("roots.tokenizer_pack").value) == secure_frontend::path_to_utf8(pack / "tokenizer") &&
             std::get<std::string>(rows.at("engine.model_id").value) == "minimax-m3.1-flash",
         "the card: minimax, from that download, the pack and its tokenizer, Maya's model name");
}

// §4.5: the danger zone's delete of a downloaded model also takes it out of its store (`strata rm <id>:download` there:
// the index row and the version folder), so the Download list offers it again instead of calling it installed.
void a_downloaded_model_can_be_removed() {
  Fixture f;
  f.spawner.pulls.push_back(Script{pull_lines("qwen-small")});
  const auto store = f.dir / "elsewhere";
  secure_frontend_test::make_private_directory(store);
  f.start(1U << 30U);
  expect(!f.manager->start("qwen-small", store).has_value() && f.manager->wait_idle(std::chrono::seconds(10)), "it downloads");
  {
    std::lock_guard lock(f.spawner.mutex);
    f.spawner.installed = store / "v1";   // the store lists it now
  }
  expect(has(f.manager->list_json(), "\"id\":\"qwen-small\"") && has(f.manager->list_json(), "\"state\":\"installed\""),
         "installed: " + f.manager->list_json());
  const auto unknown = f.manager->remove_installed("nobody");
  expect(unknown && unknown->status == 404, "an unknown download is not found");

  f.spawner.pulls.push_back(Script{{"error: the store is locked"}, 1});
  const auto failed = f.manager->remove_installed("qwen-small");
  expect(failed && failed->status == 500 && has(f.manager->list_json(), "\"state\":\"installed\""),
         "a strata rm that leaves it in the store is an error, and it stays installed");

  f.spawner.pulls.push_back(Script{{"removed qwen-small:download"}});
  expect(!f.manager->remove_installed("qwen-small").has_value(), "a remove is accepted");
  const auto& arguments = f.spawner.recorded.back().command.arguments;
  expect(arguments == std::vector<std::string>{"--config", secure_frontend::path_to_utf8(f.dir / "config.json"), "--store",
                                               secure_frontend::path_to_utf8(store), "rm", "qwen-small:download"},
         "strata rm in the download's own store");
  const std::string listed = f.manager->list_json();
  expect(!has(listed, "\"state\":\"installed\"") && f.events.size() == 1U,
         "downloadable again, with no alert: " + listed);
}

// Owner 2026-10-23: "check if the model is a multipart gguf ... paste the other links so that they
// are all downloaded at the same location, max 5 additional links." The shards assemble into one
// card; an incomplete set names the missing shards and does not start. A matched custom card loaded
// from custom.json is runnable in the list.
void a_multipart_paste_assembles_its_shards() {
  Fixture f;
  const std::string base = "https://huggingface.co/someone/Big-GGUF/resolve/main/";
  const auto hf = [](const std::string& path, const std::string& sha, const int size) {
    return Script{{"{\"repo\":\"someone/Big-GGUF\",\"commit\":\"" + std::string{kCommit} + "\",\"files\":[{\"path\":\"" + path +
                   "\",\"size\":" + std::to_string(size) + ",\"sha256\":\"" + sha + "\"}]}"}};
  };
  f.spawner.hf.push_back(hf("ms-00001-of-00003.gguf", kShaA, 1000));
  f.spawner.hf.push_back(hf("ms-00002-of-00003.gguf", kShaB, 2000));
  f.spawner.hf.push_back(hf("ms-00003-of-00003.gguf", kShaA, 3000));
  f.start(1U << 30U);
  const auto whole = f.manager->check_link(base + "ms-00001-of-00003.gguf", "qwen",
                                           {base + "ms-00002-of-00003.gguf", base + "ms-00003-of-00003.gguf"});
  expect(whole.confirm_json && has(*whole.confirm_json, "\"parts\":3") && has(*whole.confirm_json, "\"have\":3") &&
             has(*whole.confirm_json, "\"complete\":true") && has(*whole.confirm_json, "\"size_bytes\":6000"),
         "three shards assemble into one card: " + whole.confirm_json.value_or("<none>"));
  f.spawner.hf.push_back(hf("ms-00001-of-00003.gguf", kShaA, 1000));
  const auto partial = f.manager->check_link(base + "ms-00001-of-00003.gguf", "qwen", {});
  expect(partial.confirm_json && has(*partial.confirm_json, "\"have\":1") && has(*partial.confirm_json, "\"complete\":false") &&
             has(*partial.confirm_json, "ms-00002-of-00003.gguf") && has(*partial.confirm_json, "ms-00003-of-00003.gguf"),
         "an incomplete paste names the missing shards: " + partial.confirm_json.value_or("<none>"));
  expect(f.manager->start_link(base + "ms-00001-of-00003.gguf").has_value(), "an incomplete multipart paste does not start");

  // A matched custom card loaded from custom.json is runnable (the prepare step is configured).
  const auto work2 = f.dir / "work2";
  secure_frontend_test::make_private_directory(work2);
  {
    std::ofstream seed(work2 / "custom.json", std::ios::binary);
    seed << "{\"schema_version\":1,\"models\":[{\"id\":\"hf-seed\",\"name\":\"Seed\",\"source\":\"custom\",\"family\":\"qwen\","
            "\"engine\":\"qwen\",\"about\":\"x\",\"repo\":\"a/b\",\"revision\":\""
         << kCommit << "\",\"prepare\":\"none\",\"files\":[{\"path\":\"s.gguf\",\"size\":10,\"sha256\":\"" << kShaA << "\"}]}]}";
  }
  secure_frontend::DownloadSettings seeded;
  seeded.catalog = secure_frontend::parse_download_catalog(catalog_text());
  seeded.strata_executable = f.dir / "strata.exe";
  seeded.config_file = f.dir / "config.json";
  seeded.downloads_root = f.dir / "downloads";
  seeded.work_dir = work2;
  seeded.cards_dir = f.dir / "cards";
  seeded.prepare_python = f.dir / "python.exe";
  seeded.prepare_tools = f.dir / "tools";
  seeded.prepare_script = f.dir / "prepare.py";
  secure_frontend::DownloadCardTemplate tmpl;
  tmpl.engine_executable = secure_frontend::path_to_utf8(f.dir / "engine.exe");
  seeded.card_template = tmpl;
  seeded.log = [](std::string_view) {};
  seeded.seams = f.spawner.seams(1U << 30U);
  DownloadManager seeded_mgr(std::move(seeded));
  const std::string slist = seeded_mgr.list_json();
  const auto at = slist.find("\"id\":\"hf-seed\"");
  expect(at != std::string::npos && has(slist.substr(at, slist.find('}', at) - at), "\"runnable\":true"),
         "a profile-matched custom card loads runnable: " + slist);
}

void pause_resume_and_cancel_discards() {
  Fixture f;
  f.spawner.pulls.push_back(Script{{"MANIFEST qwen-small:download x", "FILE Q/m-00001-of-00002.gguf 1000",
                                    "BYTES Q/m-00001-of-00002.gguf 400 1000"},
                                   0, true});
  f.spawner.pulls.push_back(Script{pull_lines("qwen-small")});
  f.start(1U << 30U);
  expect(!f.manager->start("qwen-small").has_value(), "the download starts");
  for (int i = 0; i < 200 && !has(f.manager->list_json(), "\"bytes\":400"); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  expect(!f.manager->pause("qwen-small").has_value() && f.manager->wait_idle(std::chrono::seconds(10)), "a pause is accepted");
  const std::string paused = f.manager->list_json();
  expect(has(paused, "\"state\":\"paused\"") && has(paused, "\"bytes\":400") && f.spawner.killed->load(),
         "paused: the child stopped, the bytes kept: " + paused);
  f.spawner.killed->store(false);
  expect(!f.manager->start("qwen-small").has_value() && f.manager->wait_idle(std::chrono::seconds(10)), "a resume is a start");
  expect(has(f.manager->list_json(), "\"state\":\"installed\""), "the resumed download completes");
  expect(f.spawner.recorded.size() == 2U && f.spawner.recorded[0].command.arguments == f.spawner.recorded[1].command.arguments,
         "the resume pulls the same manifest (the same digest, so the same partial files)");
  const auto refused = f.manager->pause("qwen-small");
  expect(refused && refused->status == 409, "nothing to pause once installed");

  Fixture g;
  g.spawner.pulls.push_back(Script{{"MANIFEST qwen-small:download x", "FILE Q/m-00001-of-00002.gguf 1000"}, 0, true});
  g.spawner.pulls.push_back(Script{{"removed"}});  // the rm --partials child
  g.start(1U << 30U);
  expect(!g.manager->start("qwen-small").has_value(), "a second download starts");
  for (int i = 0; i < 200 && !has(g.manager->list_json(), "\"state\":\"downloading\""); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  expect(!g.manager->cancel("qwen-small").has_value() && g.manager->wait_idle(std::chrono::seconds(10)), "a cancel is accepted");
  const std::string cancelled = g.manager->list_json();
  expect(has(cancelled, "\"state\":\"cancelled\"") && has(cancelled, "\"bytes\":0"), "cancelled: nothing kept: " + cancelled);
  const bool discarded = g.spawner.recorded.size() == 2U && g.spawner.recorded[1].command.arguments ==
                                                               std::vector<std::string>{"--config",
                                                                                        secure_frontend::path_to_utf8(g.dir / "config.json"),
                                                                                        "rm", "--partials", "qwen-small:download"};
  expect(discarded, "the partial files are discarded by strata rm --partials");
}


// The download routes' JSON (the plan's contract): the list, a start by id or by a confirmed
// link, pause, cancel and the link check, each refusal an ApiError row.
// Vision for cards (spec 2026-10-28 point 2; the owner: "a checkbox to also download vision files"): a qwen entry with a
// pinned picture encoder lists its size (vision_bytes; 0 where there is none, and GLM's stay text only here), a start
// that asks for the files where there are none is refused, and a start with them pulls the encoder as the manifest's
// mmproj (its own repository and revision), counts it in the size, and the card reads pictures with it (vision.mmproj,
// the Pictures option, on the GPU by default). Without the box ticked nothing changes.
void the_vision_files_come_with_the_download_when_asked() {
  Fixture f;
  f.with_gguf_engine = true;
#if defined(_WIN32)
  const auto tool = f.dir / "strata-prepare.exe";
#else
  const auto tool = f.dir / "strata-prepare";
#endif
  secure_frontend_test::write_file(tool, "tool");
  secure_frontend_test::make_private_directory(f.dir / "profiles");
  secure_frontend_test::write_file(f.dir / "profiles" / "expert-profile.bin", "profile");
  auto lines = pull_lines("qwen-small");
  lines.insert(lines.end() - 1, {"FILE mmproj-BF16.gguf 300", "BYTES mmproj-BF16.gguf 300 300", "FINISHED mmproj-BF16.gguf ok"});
  f.spawner.pulls.push_back(Script{lines});
  f.spawner.pulls.push_back(Script{{"STEP pack", "DONE"}});
  const auto installed = f.dir / "installed";
  secure_frontend_test::make_private_directory(installed);
  secure_frontend_test::make_private_directory(installed / "Q");
  f.spawner.on_spawn = [&](const secure_frontend::EngineCommand& command) {
    if (command.executable != tool) return;
    const auto& a = command.arguments;
    const auto out = std::find(a.begin(), a.end(), "--out");
    if (out == a.end() || out + 1 == a.end()) return;
    const std::filesystem::path pack = secure_frontend::path_from_utf8(*(out + 1));
    std::filesystem::create_directories(pack);
    secure_frontend_test::write_file(pack / "prepared.json", "{\"pack\": " + json_of(pack) + ", \"tokenizer\": null, \"ple\": null, \"shards\": 2}\n");
  };
  f.start(1U << 30U);
  const auto entry_of = [](const std::string& listed, const std::string& id) {
    const auto at = listed.find("{\"id\":\"" + id + "\"");
    return at == std::string::npos ? std::string{} : listed.substr(at, listed.find('}', at) - at);
  };
  const std::string before = f.manager->list_json();
  expect(has(entry_of(before, "qwen-small"), "\"vision_bytes\":300") && has(entry_of(before, "qwen-small"), "\"vision\":false"),
         "a qwen entry with a picture encoder lists its size, not chosen: " + entry_of(before, "qwen-small"));
  expect(has(entry_of(before, "glm-small"), "\"vision_bytes\":0"), "an entry with none lists 0: " + entry_of(before, "glm-small"));
  const auto refused = f.manager->start("glm-small", std::nullopt, true);
  expect(refused && refused->status == 400 && refused->code == "invalid_request", "vision files where there are none are refused");
  {
    std::lock_guard lock(f.spawner.mutex);
    f.spawner.installed = installed;
  }
  expect(!f.manager->start("qwen-small", std::nullopt, true).has_value() && f.manager->wait_idle(std::chrono::seconds(10)), "it downloads");
  const std::string listed = f.manager->list_json();
  expect(has(entry_of(listed, "qwen-small"), "\"state\":\"ready\"") && has(entry_of(listed, "qwen-small"), "\"size_bytes\":3300") &&
             has(entry_of(listed, "qwen-small"), "\"vision\":true"),
         "ready, the encoder counted in its size: " + entry_of(listed, "qwen-small"));
  std::ifstream in(f.dir / "work" / "qwen-small.manifest.json", std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  secure_frontend::ManifestRules release;
  release.allow_raw_profile = false;
  const auto parsed = secure_frontend::parse_manifest(text.str(), release);
  const auto* manifest = std::get_if<secure_frontend::ParsedManifest>(&parsed);
  expect(manifest != nullptr && manifest->manifest.files.size() == 3U, "the manifest lists the encoder too: " + text.str());
  if (manifest != nullptr && manifest->manifest.files.size() == 3U) {
    const auto& encoder = manifest->manifest.files[2];
    expect(encoder.role == secure_frontend::FileRole::mmproj && encoder.path == "mmproj-BF16.gguf" && encoder.size == 300U &&
               encoder.sources == std::vector<std::string>{"https://huggingface.co/acme/Vision-GGUF/resolve/" + std::string{kCommit} +
                                                           "/mmproj-BF16.gguf"},
           "the encoder: role mmproj, from its own repository and revision");
  }
  const auto read = secure_frontend::read_model_card(f.dir / "cards" / "qwen-small.json");
  const auto* card = std::get_if<secure_frontend::ModelCard>(&read);
  expect(card != nullptr && card->config.entries.contains("vision.mmproj") &&
             std::get<std::string>(card->config.entries.at("vision.mmproj").value) == secure_frontend::path_to_utf8(installed / "mmproj-BF16.gguf"),
         "the card names its encoder file");
  const bool pictures = card != nullptr && std::any_of(card->options.begin(), card->options.end(), [](const secure_frontend::ModelOptionLimits& o) {
                          return o.spec != nullptr && o.spec->name == "vision" && std::get_if<std::string>(&o.default_value) != nullptr &&
                                 *std::get_if<std::string>(&o.default_value) == "gpu";
                        });
  expect(pictures, "the card lists the Pictures option, on the GPU by default");
  // Without the box: the Qwen3.6 entry's two files are not asked for, its card reads no pictures.
  Fixture plain;
  plain.with_gguf_engine = true;
  plain.spawner.pulls.push_back(Script{{"MANIFEST qwen36-small:download " + std::string(64, 'c'), "FILE Q36/m.gguf 500",
                                        "BYTES Q36/m.gguf 500 500", "FINISHED Q36/m.gguf ok", "RESULT ok"}});
  const auto q36 = plain.dir / "installed";
  secure_frontend_test::make_private_directory(q36);
  secure_frontend_test::make_private_directory(q36 / "Q36");
  secure_frontend_test::write_file(q36 / "Q36" / "m.gguf", test_support::gguf_with_tokenizer(test_support::byte_tokenizer(), "gpt2", "qwen35moe")
                                                         .u32("qwen35moe.context_length", 262144U)
                                                         .bytes());
  plain.start(1U << 30U);
  {
    std::lock_guard lock(plain.spawner.mutex);
    plain.spawner.installed = q36;
  }
  expect(!plain.manager->start("qwen36-small").has_value() && plain.manager->wait_idle(std::chrono::seconds(10)), "Qwen3.6 downloads");
  std::ifstream plain_in(plain.dir / "work" / "qwen36-small.manifest.json", std::ios::binary);
  std::ostringstream plain_text;
  plain_text << plain_in.rdbuf();
  expect(!has(plain_text.str(), "mmproj"), "without the box the manifest has no encoder");
  const auto plain_read = secure_frontend::read_model_card(plain.dir / "cards" / "qwen36-small.json");
  const auto* plain_card = std::get_if<secure_frontend::ModelCard>(&plain_read);
  expect(plain_card != nullptr && !plain_card->config.entries.contains("vision.mmproj"), "and its card reads no pictures");
  // With it, the GGUF-made card names the encoder too.
  Fixture seen;
  seen.with_gguf_engine = true;
  seen.spawner.pulls.push_back(Script{{"MANIFEST qwen36-small:download " + std::string(64, 'c'), "FILE Q36/m.gguf 500", "FINISHED Q36/m.gguf ok",
                                       "FILE mmproj-F16.gguf 200", "FINISHED mmproj-F16.gguf ok", "RESULT ok"}});
  const auto seen_dir = seen.dir / "installed";
  secure_frontend_test::make_private_directory(seen_dir);
  secure_frontend_test::make_private_directory(seen_dir / "Q36");
  secure_frontend_test::write_file(seen_dir / "Q36" / "m.gguf", secure_frontend_test::read_file(q36 / "Q36" / "m.gguf"));
  seen.start(1U << 30U);
  {
    std::lock_guard lock(seen.spawner.mutex);
    seen.spawner.installed = seen_dir;
  }
  expect(!seen.manager->start("qwen36-small", std::nullopt, true).has_value() && seen.manager->wait_idle(std::chrono::seconds(10)),
         "Qwen3.6 with its encoder downloads");
  const auto seen_read = secure_frontend::read_model_card(seen.dir / "cards" / "qwen36-small.json");
  const auto* seen_card = std::get_if<secure_frontend::ModelCard>(&seen_read);
  expect(seen_card != nullptr && seen_card->config.entries.contains("vision.mmproj") &&
             std::get<std::string>(seen_card->config.entries.at("vision.mmproj").value) == secure_frontend::path_to_utf8(seen_dir / "mmproj-F16.gguf"),
         "the Qwen3.6 card names its encoder file");
}

// Owner 2026-10-28 ("there are no download cards for ... flint, raon1, gemma4"; "gemma4 has an mtp"): the embedded
// catalog's own pins (source m-strata) are read whole - flint and Gemma 4 on their engines, raon-1-mini packed first,
// Gemma 4's MTP draft file pinned by its commit, size and SHA-256. An MTP file is Gemma 4's only, always fetched, so it
// must be pinned to a commit with its size; an unknown source or engine is refused.
void the_catalog_reads_our_pins_and_the_mtp_file() {
  const auto catalog = secure_frontend::parse_download_catalog(secure_frontend::embedded_download_catalog());
  expect(catalog.dropped == 0U, "the embedded catalog is read whole: " + std::to_string(catalog.dropped) + " dropped");
  const auto find = [&catalog](const std::string_view id) -> const secure_frontend::DownloadEntry* {
    for (const auto& entry : catalog.entries) {
      if (entry.id == id) return &entry;
    }
    return nullptr;
  };
  const auto* flint = find("flint-q8_0");
  const auto* raon = find("raon-1-mini-preview-q8_0");
  const auto* gemma = find("gemma-4-26b-a4b-it-q8_0");
  expect(flint != nullptr && flint->source == "m-strata" && flint->engine == "flint" && flint->prepare == "none" && !flint->mtp &&
             flint->files.size() == 1U && flint->files[0].size == std::optional<std::uint64_t>{42301076928ULL},
         "flint Q8_0: our pin, the flint engine, its card from the GGUF");
  expect(raon != nullptr && raon->source == "m-strata" && raon->engine == "raon1" && raon->prepare == "maya_pack" && !raon->mtp,
         "raon-1-mini Q8_0: our pin, the raon1 engine, packed first");
  expect(gemma != nullptr && gemma->source == "m-strata" && gemma->engine == "gemma4" && gemma->mtp.has_value() &&
             gemma->mtp->path == "MTP/mtp-gemma-4-26B-A4B-it-Q8_0.gguf" &&
             gemma->mtp->size == std::optional<std::uint64_t>{461766816ULL} &&
             gemma->mtp->sha256 == "6326fb9f5e487aa8dcdd313a091e3c67724cb2a666ec3b7d2895b5b26d93ed1b" &&
             gemma->mtp->repo == "unsloth/gemma-4-26B-A4B-it-GGUF" && gemma->mtp->revision == gemma->revision,
         "Gemma 4 26B-A4B Q8_0 with its MTP draft file, from its repository and commit");
  const auto read = [](const std::string& entry) {
    return secure_frontend::parse_download_catalog("{\"schema_version\":1,\"models\":[" + entry + "]}");
  };
  const std::string mtp = "{\"path\":\"MTP/mtp-g.gguf\",\"size\":50,\"sha256\":\"" + kShaB + "\"}";
  const auto kept = read(catalog_entry("g", "gemma4", "m-strata", mtp));
  expect(kept.entries.size() == 1U && kept.entries[0].mtp && kept.entries[0].mtp->repo.empty() && kept.entries[0].mtp->size == std::optional<std::uint64_t>{50U},
         "an MTP file without its own repository and revision is the entry's");
  expect(read(catalog_entry("g", "flint", "m-strata", mtp)).dropped == 1U, "an MTP file on an engine that runs none: refused");
  expect(read(catalog_entry("g", "gemma4", "m-strata", "{\"path\":\"MTP/mtp-g.gguf\",\"size\":null,\"sha256\":\"" + kShaB + "\"}")).dropped == 1U,
         "an MTP file without its size: refused (it is always fetched)");
  expect(read(catalog_entry("g", "gemma4", "m-strata", "{\"path\":\"MTP/mtp-g.gguf\",\"size\":50,\"sha256\":\"" + kShaB +
                                                           "\",\"revision\":\"main\"}")).dropped == 1U,
         "an MTP file at main: refused (pinned to a commit)");
  expect(read(catalog_entry("g", "gemma4", "m-strata", mtp, "main")).dropped == 1U, "an MTP file of an entry at main: refused");
  expect(read(catalog_entry("g", "gemma4", "m-strata", "{\"path\":\"MTP/mtp-g.bin\",\"size\":50,\"sha256\":\"" + kShaB + "\"}")).dropped == 1U,
         "an MTP file that is not a .gguf (O4): refused");
  expect(read(catalog_entry("g", "gemma4", "m-strata", "{\"path\":\"g.gguf\",\"size\":50,\"sha256\":\"" + kShaB + "\"}")).dropped == 1U,
         "an MTP file that is the model's own file: refused");
  expect(read(catalog_entry("g", "gemma4", "m-strata", "{\"path\":\"MTP/mtp-g-00001-of-00002.gguf\",\"size\":50,\"sha256\":\"" + kShaB + "\"}"))
                 .dropped == 1U,
         "an MTP file named as a shard: refused");
  expect(read(catalog_entry("g", "gemma4", "elsewhere", "null")).dropped == 1U, "an unknown source: refused");
  expect(read(catalog_entry("g", "llama", "m-strata", "null")).dropped == 1U, "an unknown engine: refused");
  for (const auto* engine : {"gemma4", "flint", "raon1"}) {
    expect(read(catalog_entry("g", engine, "custom", "null")).entries.size() == 1U,
           std::string{"a custom paste matched to "} + engine + " reads back (custom.json after a restart)");
  }
}

// Owner 2026-10-28 ("gemma4 has an mtp ... make sure we work that mtp in our gemma engine"): a Gemma 4 download brings
// its MTP draft file - counted in the size and the files, pulled with the model as a weights file from its own pin - and
// the card made from the GGUF runs it: engine.args --mtp <the stored MTP file>, the MTP option on by default.
// A card added from a folder whose file is named like a download's (the page names a card's file after the card; on
// Windows "Gemma-Small.json" is this very file) is never written over by that download's card: the download says why.
void a_download_never_writes_over_a_folder_card() {
  Fixture f;
  f.with_gguf_engine = true;
  f.spawner.pulls.push_back(Script{{"MANIFEST gemma-small:download " + std::string(64, 'c'), "FILE gemma-small-Q8_0.gguf 1000",
                                    "FINISHED gemma-small-Q8_0.gguf ok", "FILE MTP/mtp-gemma-small-Q8_0.gguf 50",
                                    "FINISHED MTP/mtp-gemma-small-Q8_0.gguf ok", "RESULT ok"}});
  const auto installed = f.dir / "installed";
  secure_frontend_test::make_private_directory(installed);
  secure_frontend_test::make_private_directory(installed / "MTP");
  secure_frontend_test::write_file(installed / "gemma-small-Q8_0.gguf",
                                   test_support::gguf_with_tokenizer(test_support::spm_tokenizer(), "gemma4", "gemma4")
                                       .u32("gemma4.context_length", 262144U)
                                       .bytes());
  secure_frontend_test::write_file(installed / "MTP" / "mtp-gemma-small-Q8_0.gguf", "GGUF");
  for (const char* name : {"downloads", "work", "cards"}) secure_frontend_test::make_private_directory(f.dir / name);
  const std::string folder_card =
      R"({"schema_version":1,"name":"gemma-small","source":"folder","options":{"temperature":{"default":1.0}}})";
  secure_frontend_test::write_file(f.dir / "cards" / "gemma-small.json", folder_card);
  f.start(1U << 30U);
  {
    std::lock_guard lock(f.spawner.mutex);
    f.spawner.installed = installed;
  }
  expect(!f.manager->start("gemma-small").has_value() && f.manager->wait_idle(std::chrono::seconds(10)), "it downloads");
  expect(secure_frontend_test::read_file(f.dir / "cards" / "gemma-small.json") == folder_card, "the folder's card is as it was");
  const std::string listed = f.manager->list_json();
  const auto at = listed.find("\"id\":\"gemma-small\"");
  const std::string entry = at == std::string::npos ? std::string{} : listed.substr(at, listed.find("{\"id\":", at + 1U) - at);
  expect(has(entry, "\"state\":\"failed\"") && has(entry, "already has a card") && has(entry, "gemma-small.json"),
         "the download says its card was not written over the folder's: " + entry);
}

void a_downloaded_gemma4_brings_its_mtp_file() {
  Fixture f;
  f.with_gguf_engine = true;
  f.spawner.pulls.push_back(Script{{"MANIFEST gemma-small:download " + std::string(64, 'c'), "FILE gemma-small-Q8_0.gguf 1000",
                                    "FINISHED gemma-small-Q8_0.gguf ok", "FILE MTP/mtp-gemma-small-Q8_0.gguf 50",
                                    "FINISHED MTP/mtp-gemma-small-Q8_0.gguf ok", "RESULT ok"}});
  const auto installed = f.dir / "installed";
  secure_frontend_test::make_private_directory(installed);
  secure_frontend_test::make_private_directory(installed / "MTP");   // the store keeps the catalog's subfolder
  secure_frontend_test::write_file(installed / "gemma-small-Q8_0.gguf",
                                   test_support::gguf_with_tokenizer(test_support::spm_tokenizer(), "gemma4", "gemma4")
                                       .u32("gemma4.context_length", 262144U)
                                       .bytes());
  secure_frontend_test::write_file(installed / "MTP" / "mtp-gemma-small-Q8_0.gguf", "GGUF");
  f.start(1U << 30U);
  const auto entry_of = [&f]() {
    const std::string listed = f.manager->list_json();
    const auto at = listed.find("\"id\":\"gemma-small\"");
    return at == std::string::npos ? std::string{} : listed.substr(at, listed.find('}', at) - at);
  };
  expect(has(entry_of(), "\"size_bytes\":1050") && has(entry_of(), "\"files\":2,") && has(entry_of(), "\"runnable\":true") &&
             has(entry_of(), "MTP"),
         "the MTP file counts in its size and files, and the note names it: " + entry_of());
  {
    std::lock_guard lock(f.spawner.mutex);
    f.spawner.installed = installed;
  }
  expect(!f.manager->start("gemma-small").has_value() && f.manager->wait_idle(std::chrono::seconds(10)), "it downloads");
  expect(has(entry_of(), "\"state\":\"ready\""), "ready - a card was made: " + entry_of());
  expect(f.spawner.recorded.size() == 1U, "one child, the pull: nothing to prepare");
  std::ifstream in(f.dir / "work" / "gemma-small.manifest.json", std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  secure_frontend::ManifestRules release;
  release.allow_raw_profile = false;
  const auto parsed = secure_frontend::parse_manifest(text.str(), release);
  const auto* manifest = std::get_if<secure_frontend::ParsedManifest>(&parsed);
  expect(manifest != nullptr && manifest->manifest.files.size() == 2U, "the manifest lists the model and its MTP file: " + text.str());
  if (manifest != nullptr && manifest->manifest.files.size() == 2U) {
    const auto& mtp = manifest->manifest.files[1];
    expect(mtp.role == secure_frontend::FileRole::weights && mtp.path == "MTP/mtp-gemma-small-Q8_0.gguf" && mtp.size == 50U &&
               mtp.sha256 == kShaB &&
               mtp.sources == std::vector<std::string>{"https://huggingface.co/acme/Gemma-MTP-GGUF/resolve/" + std::string{kCommit} +
                                                       "/MTP/mtp-gemma-small-Q8_0.gguf"},
           "the MTP file: a weights file at its own pin");
  }
  const auto read = secure_frontend::read_model_card(f.dir / "cards" / "gemma-small.json");
  const auto* card = std::get_if<secure_frontend::ModelCard>(&read);
  expect(card != nullptr, "the card reads");
  if (card == nullptr) {
    for (const auto& issue : std::get<secure_frontend::ConfigErrors>(read)) std::cerr << "  " << issue.key_path << ": " << issue.reason << '\n';
    return;
  }
  const auto& rows = card->config.entries;
  const auto args = rows.contains("engine.args") ? std::get<std::vector<std::string>>(rows.at("engine.args").value) : std::vector<std::string>{};
  const std::string stored = secure_frontend::path_to_utf8((installed / "MTP" / "mtp-gemma-small-Q8_0.gguf").make_preferred());
  expect(card->profile == secure_frontend::EngineProfile::gemma4 && card->source == "download:gemma-small" &&
             args == std::vector<std::string>{"--mtp", stored},
         "the card: gemma4, from that download, drafting with the stored MTP file");
  expect(!secure_frontend::validate_engine_user_arguments(secure_frontend::EngineArgumentsProfile::strata_gemma4, args).has_value(),
         "--mtp <file> passes the Gemma engine's allowlist");
  const bool mtp_on = std::any_of(card->options.begin(), card->options.end(), [](const secure_frontend::ModelOptionLimits& o) {
    return o.spec != nullptr && o.spec->name == "mtp" && std::get_if<bool>(&o.default_value) != nullptr && *std::get_if<bool>(&o.default_value);
  });
  expect(mtp_on, "the card lists the MTP option, on by default");
}

// raon-1-mini (owner 2026-10-28, our pin): packed as its folder card is - strata-prepare download --maya on the GGUF
// hard-linked beside the pack (<store>/packs/<id>/, the pack in pack/), as GLM-5.3's - then its card: raon1 on the pack,
// the served model name its marker. Deleting it takes that folder with it.
void a_downloaded_raon1_is_packed_like_glm() {
  Fixture f;
  f.with_gguf_engine = true;
#if defined(_WIN32)
  const auto tool = f.dir / "strata-prepare.exe";
#else
  const auto tool = f.dir / "strata-prepare";
#endif
  f.spawner.pulls.push_back(Script{{"MANIFEST raon-small:download " + std::string(64, 'c'), "FILE raon-small-Q8_0.gguf 2000",
                                    "FINISHED raon-small-Q8_0.gguf ok", "RESULT ok"}});
  f.spawner.pulls.push_back(Script{{"STEP pack", "DONE"}});
  const auto installed = f.dir / "installed";
  secure_frontend_test::make_private_directory(installed);
  secure_frontend_test::write_file(installed / "raon-small-Q8_0.gguf", "raon");
  const auto folder = f.dir / "downloads" / "packs" / "raon-small";
  bool linked = false;
  f.spawner.on_spawn = [&](const secure_frontend::EngineCommand& command) {
    if (command.executable != tool) return;
    std::error_code ec;
    linked = std::filesystem::equivalent(folder / "raon-small-Q8_0.gguf", installed / "raon-small-Q8_0.gguf", ec);
    std::filesystem::create_directories(folder / "pack" / "tokenizer");
  };
  f.start(1U << 30U);
  const auto entry_of = [&f]() {
    const std::string listed = f.manager->list_json();
    const auto at = listed.find("\"id\":\"raon-small\"");
    return at == std::string::npos ? std::string{} : listed.substr(at, listed.find('}', at) - at);
  };
  expect(has(entry_of(), "\"runnable\":false") && has(entry_of(), "download only"), "without strata-prepare: download only: " + entry_of());
  secure_frontend_test::write_file(tool, "tool");
  expect(has(entry_of(), "\"runnable\":true") && has(entry_of(), "the raon-1-mini pack is built"), "runnable, packed first: " + entry_of());
  {
    std::lock_guard lock(f.spawner.mutex);
    f.spawner.installed = installed;
  }
  expect(!f.manager->start("raon-small").has_value() && f.manager->wait_idle(std::chrono::seconds(10)), "it downloads");
  expect(has(entry_of(), "\"state\":\"ready\""), "ready - a card was made: " + entry_of());
  expect(f.spawner.recorded.size() == 2U, "two children: the pull and strata-prepare");
  if (f.spawner.recorded.size() != 2U) return;
  expect(f.spawner.recorded[1].command.executable == tool &&
             f.spawner.recorded[1].command.arguments ==
                 std::vector<std::string>{"download", "--maya", "--gguf", secure_frontend::path_to_utf8(folder / "raon-small-Q8_0.gguf"), "--out",
                                          secure_frontend::path_to_utf8(folder / "pack")},
         "strata-prepare download --maya: the linked GGUF, the pack beside it");
  expect(linked, "the GGUF is a hard link to the store's file when the pack is made");
  expect(std::filesystem::exists(folder / "pack" / "raon1-mini.gguf"), "the pack's marker: the served model name");
  const auto read = secure_frontend::read_model_card(f.dir / "cards" / "raon-small.json");
  const auto* card = std::get_if<secure_frontend::ModelCard>(&read);
  expect(card != nullptr, "the card reads");
  if (card == nullptr) return;
  const auto& rows = card->config.entries;
  expect(card->profile == secure_frontend::EngineProfile::raon1 && card->source == "download:raon-small" &&
             std::get<std::string>(rows.at("roots.models").value) == secure_frontend::path_to_utf8(folder / "pack") &&
             std::get<std::string>(rows.at("roots.tokenizer_pack").value) == secure_frontend::path_to_utf8(folder / "pack" / "tokenizer") &&
             std::get<std::string>(rows.at("engine.model_id").value) == "raon1-mini",
         "the card: raon1, from that download, the pack and its tokenizer, the served model name");
  f.spawner.pulls.push_back(Script{{"removed raon-small:download"}});
  expect(!f.manager->remove_installed("raon-small").has_value() && !std::filesystem::exists(folder) &&
             std::filesystem::exists(installed / "raon-small-Q8_0.gguf"),
         "removed: the link and the pack go, the store's own file is the store's to remove");
}

void download_routes_answer_the_contract() {
  using secure_frontend::RouteId;
  Fixture f;
  f.spawner.pulls.push_back(Script{{"MANIFEST qwen-small:download x", "FILE Q/m-00001-of-00002.gguf 1000"}, 0, true});
  f.spawner.hf.push_back(Script{{"{\"repo\":\"someone/Model-GGUF\",\"commit\":\"" + std::string{kCommit} +
                                 "\",\"files\":[{\"path\":\"model-Q4.gguf\",\"size\":5000,\"sha256\":\"" + kShaA + "\"}]}"}});
  f.start(1U << 30U);
  auto& m = *f.manager;
  const auto answer = [&m](const RouteId route, const std::string& body) {
    return secure_frontend::download_route_answer(m, route, body);
  };
  const auto list = answer(RouteId::strata_downloads, "");
  expect(list.status == 200 && has(list.body, "\"object\":\"list\"") && has(list.body, "\"id\":\"qwen-small\""), "the list: " + list.body);
  expect(list.header_value("Content-Type") == std::optional<std::string_view>{"application/json; charset=utf-8"}, "served as JSON");
  const auto bad_json = answer(RouteId::strata_download_start, "{");
  expect(bad_json.status == 400 && has(bad_json.body, "\"code\":\"invalid_json\""), "a start body is JSON: " + bad_json.body);
  const auto no_id = answer(RouteId::strata_download_start, "{\"name\":\"qwen-small\"}");
  expect(no_id.status == 400 && has(no_id.body, "\"code\":\"invalid_request\""), "a start names an id or a url: " + no_id.body);
  const auto unknown = answer(RouteId::strata_download_start, "{\"id\":\"nope\"}");
  expect(unknown.status == 404 && has(unknown.body, "\"code\":\"model_not_found\""), "an unknown id: " + unknown.body);
  const auto started = answer(RouteId::strata_download_start, "{\"id\":\"qwen-small\"}");
  expect(started.status == 202 && has(started.body, "\"state\":\"queued\"") && has(started.body, "\"id\":\"qwen-small\""),
         "a start: 202 queued: " + started.body);
  const auto not_boolean = answer(RouteId::strata_download_start, "{\"id\":\"qwen-small\",\"vision\":\"yes\"}");
  expect(not_boolean.status == 400 && has(not_boolean.body, "\"code\":\"invalid_request\""), "vision is a boolean: " + not_boolean.body);
  const auto no_files = answer(RouteId::strata_download_start, "{\"id\":\"glm-small\",\"vision\":true}");
  expect(no_files.status == 400 && has(no_files.body, "\"code\":\"invalid_request\""), "no vision files to take: " + no_files.body);
  const auto twice = answer(RouteId::strata_download_start, "{\"id\":\"qwen-small\",\"vision\":false}");
  expect(twice.status == 409 && has(twice.body, "\"code\":\"download_in_progress\""), "already downloading: " + twice.body);
  for (int i = 0; i < 200 && !has(m.list_json(), "\"state\":\"downloading\""); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  const auto paused = answer(RouteId::strata_download_pause, "{\"id\":\"qwen-small\"}");
  expect(paused.status == 202 && has(paused.body, "\"state\":\"paused\""), "a pause: 202: " + paused.body);
  expect(m.wait_idle(std::chrono::seconds(10)) && has(m.list_json(), "\"state\":\"paused\""), "the card is paused");
  const auto again = answer(RouteId::strata_download_pause, "{\"id\":\"qwen-small\"}");
  expect(again.status == 409 && has(again.body, "\"code\":\"download_not_active\""), "nothing to pause: " + again.body);
  const auto cancelled = answer(RouteId::strata_download_cancel, "{\"id\":\"qwen-small\"}");
  expect(cancelled.status == 202 && has(cancelled.body, "\"state\":\"cancelled\"") && has(m.list_json(), "\"state\":\"cancelled\""),
         "a cancel of a paused download: 202: " + cancelled.body);
  // Box run 2026-10-23: a cancel of a card that is not running said "cancelled" and changed nothing.
  const auto nothing = answer(RouteId::strata_download_cancel, "{\"id\":\"qwen-small\"}");
  expect(nothing.status == 409 && has(nothing.body, "\"code\":\"download_not_active\""), "nothing to cancel: " + nothing.body);
  const auto off_host = answer(RouteId::strata_download_link, "{\"url\":\"https://example.com/a/b/resolve/main/m.gguf\"}");
  expect(off_host.status == 400 && has(off_host.body, "\"code\":\"invalid_request\"") && has(off_host.body, "\"reason\":\""),
         "a link off Hugging Face: 400 with the fixed reason (O4): " + off_host.body);
  const std::string link = "https://huggingface.co/someone/Model-GGUF/resolve/main/model-Q4.gguf";
  const auto unconfirmed = answer(RouteId::strata_download_start, "{\"url\":\"" + link + "\"}");
  expect(unconfirmed.status == 400, "a link starts only with confirm true: " + unconfirmed.body);
  const auto checked = answer(RouteId::strata_download_link, "{\"url\":\"" + link + "\"}");
  expect(checked.status == 200 && has(checked.body, "\"confirm\":{") && has(checked.body, "\"size_bytes\":5000"), "the link check: " + checked.body);
  f.spawner.pulls.push_back(Script{{"RESULT ok"}});
  const auto custom = answer(RouteId::strata_download_start, "{\"url\":\"" + link + "\",\"confirm\":true}");
  expect(custom.status == 202 && has(custom.body, "\"id\":\"hf-model-gguf-model-q4\""), "the confirmed link starts: " + custom.body);
  expect(m.wait_idle(std::chrono::seconds(10)), "the custom download ends");
  const auto wrong_route = answer(RouteId::health, "");
  expect(wrong_route.status == 500, "another route is an internal error");
}

// Owner 2026-10-28: "when we point it to a local folder with the model, if it match the card of the model that is
// downloadable, we should change the banner from available to Locally Provided vs Downloaded. That way the user knows
// what they already have in the system." Each entry of the list says what this PC has of it (one rule for the page and
// strata-tui): "downloaded" - a card from this download, or the download ready or installed; "local" - another card
// runs the same model, its file there: its first model file's name (a Maya pack card's is the shard its pack was built
// from, beside the pack), and its size where the catalog pins one (another quant of that name is not it); null
// otherwise. have_card names the card. Nothing is hashed.
void the_list_says_what_is_already_on_this_pc() {
  Fixture f;
  const auto local = f.dir / "local";
  secure_frontend_test::make_private_directory(local);
  const auto card_of = [&f](const std::string& json) {
    auto parsed = secure_frontend::parse_model_card(json, f.dir);
    expect(std::holds_alternative<secure_frontend::ModelCard>(parsed), "a valid card");
    if (auto* card = std::get_if<secure_frontend::ModelCard>(&parsed)) f.held.push_back(std::move(*card));
  };
  const auto sized = [](const std::filesystem::path& file, const std::size_t bytes) {
    secure_frontend_test::make_private_directory(file.parent_path());
    secure_frontend_test::write_file(file, std::string(bytes, 'x'));
  };
  const auto engine = f.dir / "multiengine.exe";
  // Gemma 4 from its folder: the catalog's file, its pinned size
  secure_frontend::GgufMetadata gemma;
  gemma.version = 3U;
  gemma.values["general.architecture"] = std::string{"gemma4"};
  sized(local / "gemma" / "gemma-small-Q8_0.gguf", 1000U);
  const auto made = secure_frontend::card_for_gguf(gemma, {.folder = local / "gemma", .file = "gemma-small-Q8_0.gguf", .engine = engine,
                                                           .source = "folder"});
  if (const auto* text = std::get_if<secure_frontend::CardText>(&made)) card_of(text->json);
  // raon-1-mini and GLM-5.3 on their packs (native_experts.txt names the shard), the shards beside them
  using secure_frontend::PackedFamily;
  sized(local / "raon" / "raon-small-Q8_0.gguf", 2000U);
  secure_frontend_test::make_private_directory(local / "raon" / "pack");
  secure_frontend_test::write_file(local / "raon" / "pack" / "native_experts.txt", "# ... absolute offsets in raon-small-Q8_0.gguf, or ...)\n");
  card_of(secure_frontend::card_for_pack({.family = PackedFamily::raon1, .name = "my-raon", .engine = engine, .source = "folder",
                                          .pack = local / "raon" / "pack"}).json);
  sized(local / "glm" / "g-00001-of-00002.gguf", 1000U);
  secure_frontend_test::make_private_directory(local / "glm" / "pack");
  secure_frontend_test::write_file(local / "glm" / "pack" / "native_experts.txt", "# ... absolute offsets in g-00001-of-00002.gguf, or ...)\n");
  card_of(secure_frontend::card_for_pack({.family = PackedFamily::glm53, .name = "my-glm", .engine = engine, .source = "folder",
                                          .pack = local / "glm" / "pack"}).json);
  // DeepSeek-V4.1: the same name, another size - not this entry's file (another revision: the owner's raon-1-mini Q8_0 is
  // 64 bytes larger than KRAFTON's current one), so "other": that card named, the download still offered
  sized(local / "ds" / "d-00001-of-00002.gguf", 999U);
  secure_frontend_test::make_private_directory(local / "ds" / "maya-ds4");
  secure_frontend_test::write_file(local / "ds" / "maya-ds4" / "sources.json", "{\"files\": [{\"name\": \"d-00001-of-00002.gguf\", \"size\": 999}]}");
  card_of(secure_frontend::card_for_pack({.family = PackedFamily::deepseek, .name = "my-ds", .engine = engine, .source = "folder",
                                          .pack = local / "ds" / "maya-ds4"}).json);
  // MiniMax-M3.1: sources.json names the shard (its indexers file after it)
  sized(local / "mm" / "m-IQ1_S-00001-of-00002.gguf", 1000U);
  secure_frontend_test::make_private_directory(local / "mm" / "maya-mm3");
  secure_frontend_test::write_file(local / "mm" / "maya-mm3" / "sources.json",
                                   "{\"files\": [{\"name\": \"m-IQ1_S-00001-of-00002.gguf\"}, {\"name\": \"m-maya-indexers.gguf\"}]}");
  card_of(secure_frontend::card_for_pack({.family = PackedFamily::minimax, .name = "my-mm", .engine = engine, .source = "folder",
                                          .pack = local / "mm" / "maya-mm3"}).json);
  // GLM-5.3 whose catalog pins no size: its shard not there (deleted, or a drive not plugged in) - not on this PC
  secure_frontend_test::make_private_directory(local / "glm1");
  secure_frontend_test::make_private_directory(local / "glm1" / "pack");
  secure_frontend_test::write_file(local / "glm1" / "pack" / "native_experts.txt", "# ... absolute offsets in g-00001-of-00001.gguf, or ...)\n");
  card_of(secure_frontend::card_for_pack({.family = PackedFamily::glm53, .name = "my-glm1", .engine = engine, .source = "folder",
                                          .pack = local / "glm1" / "pack"}).json);
  // a Qwen3.8 card from this download (source download:qwen-small), its files in the store
  card_of(secure_frontend::card_for_pack({.family = PackedFamily::qwen38, .name = "Qwen-small", .engine = engine,
                                          .source = "download:qwen-small", .pack = f.dir / "packs" / "q", .folder = f.dir / "store" / "q",
                                          .model_id = "m-00001-of-00002"}).json);
  expect(f.held.size() == 7U, "seven cards are held");
  // Qwen3.6's download made its card earlier (its card file in the cards folder: ready; a card from before cards named
  // their source), which the launcher did not take
  for (const char* name : {"downloads", "work", "cards"}) secure_frontend_test::make_private_directory(f.dir / name);
  secure_frontend_test::write_file(f.dir / "cards" / "qwen36-small.json", "{}");
  // a card added from a folder whose file is named like a download's (the page names a card's file after the card; on
  // Windows "Gemma-Small.json" is this very file): not that download's card, so the download is not ready
  secure_frontend_test::write_file(f.dir / "cards" / "gemma-small.json",
                                   R"({"schema_version":1,"name":"gemma-small","source":"folder","options":{"temperature":{"default":1.0},"min_p":{"default":0.05}}})");
  f.start(1U << 30U);
  std::string listed = f.manager->list_json();
  const auto entry = [&listed](const std::string& id) {
    const auto at = listed.find("{\"id\":\"" + id + "\"");
    if (at == std::string::npos) return std::string{"absent"};
    const auto next = listed.find("{\"id\":", at + 1U);
    return listed.substr(at, next == std::string::npos ? std::string::npos : next - at);
  };
  const auto says = [&entry](const std::string& id, const std::string& have, const std::string& card) {
    const std::string text = entry(id);
    const std::string want = ",\"have\":" + have + ",\"have_card\":" + card + "}";
    expect(has(text, want), id + " says " + want + ": " + text);
  };
  says("gemma-small", "\"local\"", "\"gemma-small-Q8_0\"");
  says("raon-small", "\"local\"", "\"my-raon\"");
  says("glm-maya", "\"local\"", "\"my-glm\"");
  says("ds-maya", "\"other\"", "\"my-ds\"");
  says("mm-maya", "\"local\"", "\"my-mm\"");
  says("qwen-small", "\"downloaded\"", "\"Qwen-small\"");
  says("qwen36-small", "\"downloaded\"", "null");
  says("glm-small", "null", "null");
  expect(has(entry("gemma-small"), "\"state\":\"available\"") && has(entry("qwen36-small"), "\"state\":\"ready\""),
         "the states are as before: " + entry("gemma-small"));
  // its shard back in place: on this PC by its name (the catalog pins no size to compare)
  secure_frontend_test::write_file(local / "glm1" / "g-00001-of-00001.gguf", "GGUF");
  listed = f.manager->list_json();
  says("glm-small", "\"local\"", "\"my-glm1\"");
  // a card that goes is no longer counted
  f.held.erase(f.held.begin());
  listed = f.manager->list_json();
  says("gemma-small", "null", "null");
}

}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"catalog_cards_and_the_disk_check", catalog_cards_and_the_disk_check},
      {"pull_runs_strata_and_tracks_progress", pull_runs_strata_and_tracks_progress},
      {"failures_and_cancel", failures_and_cancel},
      {"a_pull_that_said_ok_is_checked_in_the_store", a_pull_that_said_ok_is_checked_in_the_store},
      {"maya_main_is_resolved_and_checked", maya_main_is_resolved_and_checked},
      {"pasted_links", pasted_links},
      {"a_resume_counts_its_rate_from_the_kept_bytes", a_resume_counts_its_rate_from_the_kept_bytes},
      {"maya_sizes_meet_the_free_space_and_resume_keeps_the_commit", maya_sizes_meet_the_free_space_and_resume_keeps_the_commit},
      {"pause_resume_and_cancel_discards", pause_resume_and_cancel_discards},
      {"qwen36_is_runnable_with_a_tokenizer_note", qwen36_is_runnable_with_a_tokenizer_note},
      {"a_pasted_gguf_is_matched_to_a_profile", a_pasted_gguf_is_matched_to_a_profile},
      {"a_download_goes_to_the_chosen_folder", a_download_goes_to_the_chosen_folder},
      {"a_downloaded_model_can_be_removed", a_downloaded_model_can_be_removed},
      {"a_downloaded_qwen36_becomes_a_card", a_downloaded_qwen36_becomes_a_card},
      {"a_downloaded_qwen38_is_packed_natively", a_downloaded_qwen38_is_packed_natively},
      {"a_downloaded_glm_is_packed_with_mayas_step", a_downloaded_glm_is_packed_with_mayas_step},
      {"a_downloaded_deepseek_is_packed_with_mayas_step", a_downloaded_deepseek_is_packed_with_mayas_step},
      {"a_downloaded_minimax_is_packed_with_mayas_step", a_downloaded_minimax_is_packed_with_mayas_step},
      {"a_downloaded_gguf_becomes_a_card", a_downloaded_gguf_becomes_a_card},
      {"a_multipart_paste_assembles_its_shards", a_multipart_paste_assembles_its_shards},
      {"download_routes_answer_the_contract", download_routes_answer_the_contract},
      {"the_vision_files_come_with_the_download_when_asked", the_vision_files_come_with_the_download_when_asked},
      {"the_catalog_reads_our_pins_and_the_mtp_file", the_catalog_reads_our_pins_and_the_mtp_file},
      {"a_downloaded_gemma4_brings_its_mtp_file", a_downloaded_gemma4_brings_its_mtp_file},
      {"a_download_never_writes_over_a_folder_card", a_download_never_writes_over_a_folder_card},
      {"a_downloaded_raon1_is_packed_like_glm", a_downloaded_raon1_is_packed_like_glm},
      {"the_list_says_what_is_already_on_this_pc", the_list_says_what_is_already_on_this_pc},
  }, failures);
}
