#pragma once
// SP4 Task 22: run_pull over the scripted transport, a fake probe and a fake confirmer.
#include "secure_frontend/acquire_pull.hpp"
#include "secure_frontend/frontend_config.hpp"
#include "secure_frontend/model_index_store.hpp"
#include "support/acquire_fixtures.hpp"
#include "support/scripted_transport.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace secure_frontend_test::pull {

namespace fx = secure_frontend_test::acquire;

class FakeProbe final : public secure_frontend::DownloaderProbe {
 public:
  int calls{0};
  std::optional<secure_frontend::AcquireError> result;
  std::string label{"full"};
  std::optional<secure_frontend::AcquireError> probe() override {
    ++calls;
    return result;
  }
  std::string confinement() const override { return label; }
};

// Task 27 (ruling I13): thin forwarders, so a CliServices factory can hand the CLI an owned
// unique_ptr while the test keeps the recording object.
class ForwardTransport final : public secure_frontend::HopTransport {
 public:
  explicit ForwardTransport(secure_frontend::HopTransport& target) : target_(target) {}
  secure_frontend::HopExit run(const secure_frontend::TransferRequest& request,
                               const std::function<bool(std::string_view)>& on_bytes) override {
    return target_.run(request, on_bytes);
  }

 private:
  secure_frontend::HopTransport& target_;
};

class ForwardProbe final : public secure_frontend::DownloaderProbe {
 public:
  explicit ForwardProbe(secure_frontend::DownloaderProbe& target) : target_(target) {}
  std::optional<secure_frontend::AcquireError> probe() override { return target_.probe(); }
  std::string confinement() const override { return target_.confinement(); }

 private:
  secure_frontend::DownloaderProbe& target_;
};

class InterruptingProgress final : public secure_frontend::ProgressSink {
 public:
  InterruptingProgress(std::atomic<bool>& flag, const bool& armed, const bool& mid_file)
      : flag_(flag), armed_(armed), mid_file_(mid_file) {}
  void notice(const std::string_view line) override {
    if (armed_ && line == "verifying") flag_.store(true);
  }
  // Final fix F1: the flag is raised after the first chunk of a file that has more to come.
  void file_bytes(const std::string_view, const std::uint64_t done, const std::uint64_t total) override {
    if (mid_file_ && done < total) flag_.store(true);
  }

 private:
  std::atomic<bool>& flag_;
  const bool& armed_;
  const bool& mid_file_;
};

class FakeConfirmer final : public secure_frontend::Confirmer {
 public:
  bool interactive{false};
  std::deque<bool> answers;
  std::vector<std::string> questions;
  std::vector<std::string> lines;
  void say(const std::string_view line) override { lines.emplace_back(line); }
  bool confirm(const std::string_view question, const bool yes_applies) override {
    questions.emplace_back(question);
    if (yes_applies) return true;
    if (!interactive || answers.empty()) return false;
    const bool answer = answers.front();
    answers.pop_front();
    return answer;
  }
  [[nodiscard]] std::string text() const {
    std::string all;
    for (const auto& line : lines) all += line + "\n";
    return all;
  }
};

// A response for <path> requested with --range <from>-: 206 that delivers no body
// byte and exits 18 (curl "partial file"), so the .part stays at <from> bytes.
inline std::string ranged_drop(const fx::PayloadFile& file, const std::uint64_t from) {
  return "response https://fixture.example/fx/" + file.path + " range " + std::to_string(from) +
         "\nstatus 206 Partial\nheader Content-Range: bytes " + std::to_string(from) + "-" + std::to_string(file.bytes.size() - 1) +
         "/" + std::to_string(file.bytes.size()) + "\nbody-text x\ntruncate-at 0\nexit 18\nend\n";
}

// A response for <path> requested with --range <from>-: 206 with the remaining bytes.
inline std::string ranged_rest(const fx::PayloadFile& file, const std::uint64_t from) {
  return "response https://fixture.example/fx/" + file.path + " range " + std::to_string(from) +
         "\nstatus 206 Partial\nheader Content-Range: bytes " + std::to_string(from) + "-" + std::to_string(file.bytes.size() - 1) +
         "/" + std::to_string(file.bytes.size()) + "\nbody-text " + file.bytes.substr(from) + "\nend\n";
}

struct PullHarness final {
  static secure_frontend::CanonicalRoot make_models(const std::filesystem::path& scratch_dir) {
    secure_frontend_test::make_private_directory(scratch_dir / "models");
    return fx::models_root(scratch_dir / "models");
  }

  PullHarness()
      : scratch(secure_frontend_test::make_scratch_directory()), models(make_models(scratch)),
        settings{secure_frontend::default_downloader_path(), std::nullopt, false, std::uint64_t{1} << 40U,
                 std::chrono::seconds{172800}, std::chrono::seconds{120}, std::chrono::seconds{30},
                 std::uint32_t{1}} {
    resolver.answers["fixture.example"] = {secure_frontend::make_ipv4(93, 184, 216, 34)};
  }

  // Serves every payload file at https://fixture.example/fx/<path>; `overrides` responses win.
  void serve(const std::vector<fx::PayloadFile>& files, const std::string& overrides = "") {
    std::string scenario = overrides;
    std::map<std::string, std::string> fixtures;
    for (const auto& f : files) {
      scenario += "response https://fixture.example/fx/" + f.path + "\nstatus 200 OK\nheader Content-Length: " +
                  std::to_string(f.bytes.size()) + "\nbody-file " + f.path + "\nend\n";
      fixtures[f.path] = f.bytes;
    }
    auto parsed = secure_frontend_test::curl::parse_scenario(scenario);
    if (!parsed) throw std::logic_error("PullHarness: scenario does not parse");
    transport = std::make_unique<secure_frontend_test::ScriptedTransport>(std::move(*parsed), fixtures);
  }

  secure_frontend::PullRequest third_party(const std::string& manifest) {
    const auto path = scratch / ("manifest-" + std::to_string(++manifests) + ".json");
    secure_frontend_test::write_file(path, manifest);
    secure_frontend::PullRequest request;
    request.manifest_file = path;
    request.trust_digest = secure_frontend::to_hex(secure_frontend::sha256(manifest));
    request.yes = true;
    return request;
  }

  secure_frontend::PullOutcome pull(const secure_frontend::PullRequest& request,
                                    const secure_frontend::StoreEnvironment* override_env = nullptr) {
    secure_frontend::PullServices services{*transport, probe, resolver, clock, override_env ? *override_env : env, loaded, ui, progress,
                                           interrupted};
    return secure_frontend::run_pull(request, models, settings, services);
  }

  secure_frontend::ModelIndex index() const {
    return std::get<secure_frontend::ModelIndex>(secure_frontend::read_model_index(models));
  }

  std::filesystem::path scratch;
  secure_frontend::CanonicalRoot models;
  secure_frontend::StoreEnvironment env;
  secure_frontend_test::ManualClock clock;
  secure_frontend_test::ScriptedResolver resolver;
  std::unique_ptr<secure_frontend_test::ScriptedTransport> transport;
  FakeProbe probe;
  FakeConfirmer ui;
  fx::FakeLoaded loaded;
  std::atomic<bool> interrupted{false};
  bool progress_interrupt{false};  // dl04_interrupt_during_verification: interrupt at the "verifying" notice
  bool mid_file_interrupt{false};  // final fix F1: interrupt after the first chunk of a multi-chunk file
  InterruptingProgress progress{interrupted, progress_interrupt, mid_file_interrupt};
  secure_frontend::PullSettings settings;
  int manifests{0};
};

}  // namespace secure_frontend_test::pull
