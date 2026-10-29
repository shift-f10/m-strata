#pragma once

#include "secure_frontend/engine_supervisor.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace test_support {

// The status a fake child shows after the supervisor's kill.
inline secure_frontend::ExitStatus fake_kill_status() {
#if defined(_WIN32)
  return {false, secure_frontend::kSupervisorKillExitCode};
#else
  return {true, 9U};
#endif
}

// Control block of one fake engine generation; the test keeps a copy.
struct FakeChild final {
  std::mutex mutex;
  std::condition_variable readable;
  std::deque<secure_frontend::EngineReadResult> reads;
  std::vector<std::string> writes;
  std::optional<secure_frontend::ExitStatus> exit;
  bool reapable{true};
  // The first this-many try_reap() calls see no status yet, like a child whose
  // stdout closed just before its exit status became reapable.
  int unreapable_polls{0};
  int kills{0};
  std::thread::id spawned_on{};    // the thread that ran the factory (the supervisor thread)
  std::thread::id destroyed_on{};  // the thread that destroyed this generation's process
  std::optional<std::uint64_t> resident{};  // what resident_bytes() answers (the Monitor's engine memory)

  void push_line(std::string line) {
    std::lock_guard lock(mutex);
    reads.push_back({secure_frontend::EngineReadKind::line, std::move(line), {}});
    readable.notify_all();
  }
  // The child ends: stdout reaches EOF and `status` becomes reapable.
  void end(const secure_frontend::ExitStatus status) {
    std::lock_guard lock(mutex);
    if (!exit) exit = status;
    reads.push_back({secure_frontend::EngineReadKind::closed, {}, {}});
    readable.notify_all();
  }
};

class FakeChildTransport final : public secure_frontend::EngineLineTransport {
 public:
  explicit FakeChildTransport(std::shared_ptr<FakeChild> child) : child_(std::move(child)) {}
  bool write_line(const std::string_view line, secure_frontend::MonotonicClock::time_point, std::string&) override {
    std::lock_guard lock(child_->mutex);
    if (child_->exit) return false;
    child_->writes.emplace_back(line);
    return true;
  }
  secure_frontend::EngineReadResult read_line(const std::chrono::milliseconds timeout) override {
    std::unique_lock lock(child_->mutex);
    if (timeout.count() <= 0) return {secure_frontend::EngineReadKind::timeout, {}, {}};
    if (!child_->readable.wait_for(lock, timeout, [this] { return !child_->reads.empty(); })) {
      return {secure_frontend::EngineReadKind::timeout, {}, {}};
    }
    auto result = child_->reads.front();
    if (result.kind != secure_frontend::EngineReadKind::closed) child_->reads.pop_front();  // EOF stays
    return result;
  }

 private:
  std::shared_ptr<FakeChild> child_;
};

class FakeSupervisedProcess final : public secure_frontend::SupervisedProcess {
 public:
  explicit FakeSupervisedProcess(std::shared_ptr<FakeChild> child) : child_(child), transport_(std::move(child)) {}
  ~FakeSupervisedProcess() override {
    std::lock_guard lock(child_->mutex);
    child_->destroyed_on = std::this_thread::get_id();
  }
  FakeSupervisedProcess(const FakeSupervisedProcess&) = delete;
  FakeSupervisedProcess& operator=(const FakeSupervisedProcess&) = delete;
  secure_frontend::EngineLineTransport& transport() noexcept override { return transport_; }
  std::optional<secure_frontend::ExitStatus> try_reap() override {
    std::lock_guard lock(child_->mutex);
    if (child_->unreapable_polls > 0) {
      --child_->unreapable_polls;
      return std::nullopt;
    }
    if (!child_->exit || !child_->reapable) return std::nullopt;
    return child_->exit;
  }
  std::intptr_t monitor_handle() const noexcept override { return secure_frontend::kNoMonitorHandle; }
  // Like the real engine: EOF on stdin is a clean exit.
  void close_stdin() noexcept override { end_with({false, 0U}); }
  void signal_group_terminate() noexcept override {}
  void kill_group() noexcept override {
    std::lock_guard lock(child_->mutex);
    ++child_->kills;
    if (!child_->exit) child_->exit = fake_kill_status();
    child_->reads.push_back({secure_frontend::EngineReadKind::closed, {}, {}});
    child_->readable.notify_all();
  }
  void release_resources(std::chrono::milliseconds) noexcept override {}
  long pid() const noexcept override { return -1; }
  std::optional<std::uint64_t> resident_bytes() const noexcept override {
    std::lock_guard lock(child_->mutex);
    return child_->resident;
  }

 private:
  void end_with(const secure_frontend::ExitStatus status) noexcept {
    std::lock_guard lock(child_->mutex);
    if (!child_->exit) child_->exit = status;
    child_->reads.push_back({secure_frontend::EngineReadKind::closed, {}, {}});
    child_->readable.notify_all();
  }
  std::shared_ptr<FakeChild> child_;
  FakeChildTransport transport_;
};

// Generation n gets a fresh FakeChild; `script` runs at spawn time on the supervisor thread.
class FakeFactory final {
 public:
  using Script = std::function<void(FakeChild& child, std::uint64_t generation)>;
  explicit FakeFactory(Script script) : state_(std::make_shared<State>()) { state_->script = std::move(script); }
  [[nodiscard]] secure_frontend::ProcessFactory factory() const {
    auto state = state_;
    return [state](const std::uint64_t generation) -> secure_frontend::SpawnResult {
      auto child = std::make_shared<FakeChild>();
      child->spawned_on = std::this_thread::get_id();
      if (state->script) state->script(*child, generation);
      {
        std::lock_guard lock(state->mutex);
        state->children.push_back(child);
      }
      return std::unique_ptr<secure_frontend::SupervisedProcess>(std::make_unique<FakeSupervisedProcess>(child));
    };
  }
  [[nodiscard]] std::shared_ptr<FakeChild> child(const std::uint64_t generation) const {
    std::lock_guard lock(state_->mutex);
    if (generation == 0U || generation > state_->children.size()) return nullptr;
    return state_->children[generation - 1U];
  }
  [[nodiscard]] std::uint64_t spawned() const {
    std::lock_guard lock(state_->mutex);
    return state_->children.size();
  }

 private:
  struct State {
    std::mutex mutex;
    std::vector<std::shared_ptr<FakeChild>> children;
    Script script;
  };
  std::shared_ptr<State> state_;
};

// Backoff delays are recorded and return at once; now() moves only by those
// delays and by advance(). Makes the §3.3 numbers exactly testable.
class ManualClock final : public secure_frontend::SupervisorClock {
 public:
  [[nodiscard]] time_point now() const override {
    std::lock_guard lock(mutex_);
    return now_;
  }
  [[nodiscard]] bool sleep_backoff(const std::chrono::milliseconds delay, secure_frontend::SupervisorWake&) override {
    std::function<void()> hook;
    {
      std::lock_guard lock(mutex_);
      delays_.push_back(delay);
      now_ += delay;
      hook = on_sleep_;
    }
    if (hook) hook();
    return true;
  }
  void advance(const std::chrono::milliseconds by) {
    std::lock_guard lock(mutex_);
    now_ += by;
  }
  [[nodiscard]] std::vector<std::chrono::milliseconds> delays() const {
    std::lock_guard lock(mutex_);
    return delays_;
  }
  void on_sleep(std::function<void()> hook) {
    std::lock_guard lock(mutex_);
    on_sleep_ = std::move(hook);
  }

 private:
  mutable std::mutex mutex_;
  time_point now_{};
  std::vector<std::chrono::milliseconds> delays_;
  std::function<void()> on_sleep_;
};

// Thread-safe capture of supervisor log lines.
class LogCapture final {
 public:
  [[nodiscard]] secure_frontend::SupervisorLog sink() const {
    auto state = state_;
    return [state](const std::string_view line) {
      std::lock_guard lock(state->mutex);
      state->lines.emplace_back(line);
    };
  }
  [[nodiscard]] bool contains(const std::string_view text) const {
    std::lock_guard lock(state_->mutex);
    return std::any_of(state_->lines.begin(), state_->lines.end(), [text](const std::string& line) { return line == text; });
  }
  [[nodiscard]] bool wait_for(const std::string_view text, const std::chrono::milliseconds timeout) const {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!contains(text)) {
      if (std::chrono::steady_clock::now() >= deadline) return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
  }

 private:
  struct State {
    std::mutex mutex;
    std::vector<std::string> lines;
  };
  std::shared_ptr<State> state_ = std::make_shared<State>();
};

inline bool wait_generation_ready(const secure_frontend::EngineSupervisor& supervisor, const std::uint64_t generation,
                                  const std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (supervisor.generations_started() == generation && supervisor.snapshot().state == secure_frontend::SupervisorState::ready) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return false;
}

}  // namespace test_support
