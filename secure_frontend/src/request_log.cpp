#include "secure_frontend/request_log.hpp"

#include "secure_frontend/api_constants.hpp"

#include <algorithm>
#include <cstdio>

namespace secure_frontend {
namespace {
std::string_view route_name(const RouteId route) {
  switch (route) {
    case RouteId::health: return "health";
    case RouteId::models: return "models";
    case RouteId::model: return "model";
    case RouteId::chat: return "chat";
    case RouteId::other: return "other";
    case RouteId::web: return "web";
    case RouteId::metrics: return "metrics";
    case RouteId::strata_models: return "strata_models";
    case RouteId::strata_start: return "strata_start";
    case RouteId::strata_stop: return "strata_stop";
    case RouteId::strata_downloads: return "strata_downloads";
    case RouteId::strata_download_start: return "strata_download_start";
    case RouteId::strata_download_pause: return "strata_download_pause";
    case RouteId::strata_download_cancel: return "strata_download_cancel";
    case RouteId::strata_download_link: return "strata_download_link";
    case RouteId::strata_calibrate: return "strata_calibrate";
    case RouteId::strata_calibrate_cancel: return "strata_calibrate_cancel";
    case RouteId::strata_calibrate_auto: return "strata_calibrate_auto";
    case RouteId::strata_calibrate_report: return "strata_calibrate_report";
    case RouteId::strata_login_link: return "strata_login_link";
    case RouteId::strata_alerts: return "strata_alerts";
    case RouteId::strata_alerts_settings: return "strata_alerts_settings";
    case RouteId::strata_alerts_password: return "strata_alerts_password";
    case RouteId::strata_alerts_test: return "strata_alerts_test";
    case RouteId::strata_search: return "strata_search";
    case RouteId::strata_search_settings: return "strata_search_settings";
    case RouteId::strata_search_test: return "strata_search_test";
    case RouteId::strata_config: return "strata_config";
    case RouteId::strata_config_settings: return "strata_config_settings";
    case RouteId::strata_key_rotate: return "strata_key_rotate";
    case RouteId::strata_folders: return "strata_folders";
    case RouteId::strata_folder_private: return "strata_folder_private";
    case RouteId::strata_folder_new: return "strata_folder_new";
    case RouteId::strata_card_add: return "strata_card_add";
    case RouteId::strata_card_folder: return "strata_card_folder";
    case RouteId::strata_card_mtp: return "strata_card_mtp";
    case RouteId::strata_card_delete_begin: return "strata_card_delete_begin";
    case RouteId::strata_card_delete: return "strata_card_delete";
    case RouteId::strata_card_remove: return "strata_card_remove";
    case RouteId::strata_folder_sharing: return "strata_folder_sharing";
    case RouteId::strata_about: return "strata_about";
  }
  return "other";
}
}  // namespace

std::string format_request_line(const std::uint64_t sequence, const RequestLogRecord& r) {
  std::string line = "req " + std::to_string(sequence) + " ";
  line += r.streamed && r.route == RouteId::chat ? std::string_view{"chat_stream"} : route_name(r.route);
  line += " " + std::to_string(r.status) + " ";
  line += r.code.empty() ? std::string_view{"-"} : r.code;
  line += " " + std::to_string(r.elapsed.count()) + " " + std::to_string(r.bytes_out) + " ";
  line += r.peer == AddressScope::loopback ? "loopback" : "remote";
  return line;
}

RequestLogRecord log_record_for(const FinishedConnection& f) {
  std::string_view code = f.result.code;
  if (f.result.outcome == ConnectionOutcome::evicted) code = "evicted";
  else if (f.result.outcome == ConnectionOutcome::client_gone && f.result.status != 0 && code.empty()) code = "write_stall";
  else if (f.result.outcome == ConnectionOutcome::client_gone && code.empty()) code = "client_gone";
  return {f.result.route, f.result.status, code, f.elapsed, f.result.bytes_out, f.peer, f.result.streamed};
}

RequestLogger::RequestLogger(const bool enabled, Sink sink, std::function<Clock::time_point()> now)
    : enabled_(enabled), sink_(std::move(sink)), now_(std::move(now)), tokens_(static_cast<double>(api_constants::kLogBurst)),
      last_refill_(now_()), last_report_(now_()) {}

void RequestLogger::record(const RequestLogRecord& record) {
  if (!enabled_ || !sink_) return;
  std::lock_guard lock(mutex_);
  const Clock::time_point now = now_();
  const double elapsed = std::chrono::duration<double>(now - last_refill_).count();
  tokens_ = std::min(static_cast<double>(api_constants::kLogBurst), tokens_ + elapsed * static_cast<double>(api_constants::kLogRatePerSecond));
  last_refill_ = now;
  if (tokens_ < 1.0) { ++dropped_; return; }
  if (dropped_ > 0U && now - last_report_ >= std::chrono::seconds(1)) {
    sink_("log: " + std::to_string(dropped_) + " request lines suppressed");
    dropped_ = 0U;
    last_report_ = now;
  }
  tokens_ -= 1.0;
  sink_(format_request_line(++sequence_, record));
}

RequestLogger::Sink stderr_sink() {
  return [](const std::string_view line) {
    char buffer[256];
    const std::size_t n = std::min(line.size(), sizeof(buffer) - 1U);
    std::copy_n(line.data(), n, buffer);
    buffer[n] = '\n';
    (void)std::fwrite(buffer, 1, n + 1U, stderr);   // a failed write is dropped, never retried
    (void)std::fflush(stderr);
  };
}

}  // namespace secure_frontend
