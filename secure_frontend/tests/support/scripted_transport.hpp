#pragma once
// In-process HopTransport replaying the fake downloader's scenario format (Task 12),
// plus a scripted resolver and a manual clock (spec §10.1 seams). Never opens a socket.
#include "secure_frontend/fetch_file.hpp"
#include "support/curl_scenario.hpp"

#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace secure_frontend_test {

class ScriptedTransport final : public secure_frontend::HopTransport {
 public:
  ScriptedTransport(curl::Scenario scenario, std::map<std::string, std::string> fixtures)
      : scenario_(std::move(scenario)), fixtures_(std::move(fixtures)) {}
  std::vector<std::vector<std::string>> argvs;
  std::vector<std::string> configs;
  std::deque<int> forced_exits;  // consumed first: that exit status, no output
  std::function<void()> after_first_chunk;  // called once, after the first chunk is delivered (interrupt tests)
  secure_frontend::HopExit run(const secure_frontend::TransferRequest& request,
                               const std::function<bool(std::string_view)>& on_bytes) override {
    // Parallel downloads call run() from several threads at once; the shared recording and the
    // scenario/fixture lookup are guarded, then the response bytes stream outside the lock so the
    // connections really overlap.
    std::string bytes;
    int exit_code = 0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      argvs.push_back(secure_frontend::build_transfer_argv("/fake/curl", request));
      configs.push_back(secure_frontend::build_config_stdin(request));
      check_hop(argvs.back(), configs.back(), request);
      if (!forced_exits.empty()) {
        const int code = forced_exits.front();
        forced_exits.pop_front();
        return {code, std::nullopt, false};
      }
      const auto* response = curl::select(scenario_, request.url, request.range_from);
      // F-49: an unscripted request is a test bug, never an implicit 404.
      if (response == nullptr) {
        throw std::logic_error("ScriptedTransport: no scripted response for request " + std::to_string(argvs.size()) +
                               (request.range_from ? " (range " + std::to_string(*request.range_from) + ")" : std::string{}));
      }
      bytes = curl::render_head(*response) + curl::render_body(*response, fixtures_[response->body_file]);
      exit_code = response->exit_code;
    }
    for (std::size_t i = 0; i < bytes.size(); i += 7) {
      if (!on_bytes(std::string_view{bytes}.substr(i, 7))) return {std::nullopt, std::nullopt, true};
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (after_first_chunk) std::exchange(after_first_chunk, nullptr)();
      }
    }
    return {exit_code, std::nullopt, false};
  }

 private:
  // Every hop: argv inside the §3.3 grammar, and the --resolve pin names exactly the
  // host of the URL curl is told to fetch (Task 5 carry: otherwise curl does its own DNS).
  static void check_hop(const std::vector<std::string>& argv, const std::string& config,
                        const secure_frontend::TransferRequest& request) {
    const auto parsed = curl::parse_curl_argv(std::span<const std::string>{argv}.subspan(1));
    if (!parsed) throw std::logic_error("ScriptedTransport: argv outside the spec §3.3 grammar");
    const auto url = curl::parse_config_line(config);
    if (!url || !url->starts_with("https://")) throw std::logic_error("ScriptedTransport: config line is not one https URL");
    const auto host_end = url->find_first_of(":/", 8);
    const std::string url_host = url->substr(8, host_end == std::string::npos ? std::string::npos : host_end - 8);
    if (url_host != request.host) throw std::logic_error("ScriptedTransport: TransferRequest.host differs from the URL host");
    if (!request.proxy && !parsed->resolve) throw std::logic_error("ScriptedTransport: hop without a proxy is not pinned");
    if (parsed->resolve && !parsed->resolve->starts_with(url_host + ":443:")) {
      throw std::logic_error("ScriptedTransport: --resolve pins a different host than the URL");
    }
  }

  curl::Scenario scenario_;
  std::map<std::string, std::string> fixtures_;
  std::mutex mutex_;
};

class ScriptedResolver final : public secure_frontend::Resolver {
 public:
  std::map<std::string, std::vector<secure_frontend::ResolvedAddress>, std::less<>> answers;
  std::vector<std::string> queries;
  // Parallel downloads resolve from several threads at once; `answers` is set up before the run and
  // only read here, but recording a query mutates `queries`, so guard it.
  std::optional<std::vector<secure_frontend::ResolvedAddress>> resolve(const std::string_view host) override {
    { std::lock_guard<std::mutex> lock(mutex_); queries.emplace_back(host); }
    const auto it = answers.find(host);
    if (it == answers.end()) return std::nullopt;
    return it->second;
  }

 private:
  std::mutex mutex_;
};

class ManualClock final : public secure_frontend::AcquireClock {
 public:
  std::chrono::steady_clock::time_point now_value{std::chrono::seconds{1000}};
  std::vector<std::chrono::seconds> sleeps;
  std::chrono::steady_clock::time_point now() override { return now_value; }
  void sleep_for(const std::chrono::seconds duration) override {
    sleeps.push_back(duration);
    now_value += duration;
  }
};

}  // namespace secure_frontend_test
