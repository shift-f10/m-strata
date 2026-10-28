#include "secure_frontend/fetch_file.hpp"

#include "secure_frontend/acquire_url.hpp"
#include "secure_frontend/download_response.hpp"

#include <mutex>
#include <span>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace secure_frontend {
namespace {

// Per manifest file, across every attempt and source of this run.
struct FileState final {
  bool restarted_after_ignored_range{false};  // §3.4: a 200 to a range restarts from 0 once per file
  bool refetched_after_mismatch{false};       // §3.7: one refetch from 0 after a mismatch on resumed bytes
  std::uint64_t written_since_check{0};       // §4.6 free-space re-check counter
};

struct AttemptResult final {
  std::optional<AcquireError> error;
  bool status_transient{false};  // 429 / 5xx (§3.4); the reason table decides the rest
  bool done{false};
  bool retry_now{false};
};

AttemptResult fail(const AcquireReason reason, std::string detail, const bool status_transient = false) {
  return AttemptResult{AcquireError{reason, std::move(detail)}, status_transient, false, false};
}

AttemptResult fail_with(AcquireError error) { return AttemptResult{std::move(error), false, false, false}; }

std::optional<AcquireError> reset(FetchTarget& target) {
  if (auto failure = target.file.truncate_to_zero()) return failure;
  target.hasher = Sha256{};
  return std::nullopt;
}

// §3.7: the bytes on disk are complete; verify, then publish into the staging name.
// `resumed`: the bytes did not all arrive in one from-zero transfer of this attempt.
AttemptResult verify_and_finish(FetchContext& ctx, const ManifestFile& file, FetchTarget& target, FileState& state, const bool resumed) {
  if (to_hex(Sha256{target.hasher}.finish()) != file.sha256) {
    if (resumed && !state.refetched_after_mismatch) {
      state.refetched_after_mismatch = true;
      if (auto failure = reset(target)) return fail_with(std::move(*failure));
      return AttemptResult{std::nullopt, false, false, true};
    }
    return fail(AcquireReason::digest_mismatch, file.path);
  }
  if (auto failure = target.file.sync()) return fail_with(std::move(*failure));
  target.file.close();
  if (auto failure = rename_within(ctx.root, target.part, target.final_path, ctx.store)) return fail_with(std::move(*failure));
  return AttemptResult{std::nullopt, false, true, false};
}

// Parallel ranged download (spec 2026-10-23-parallel-downloader-design.md). A large file is
// fetched as `piece_bytes` pieces over `connections` confined children: first one probe resolves
// the final CDN URL, the object's ETag and total and confirms range support, then N workers fetch
// the missing pieces, each the same confined curl with `--range lo-hi` to that URL, writing at the
// piece's offset. The whole file is hashed in one pass at the end (pieces arrive out of order, so
// the streaming hash of the one-stream path cannot be used). A `.part.chunks` sidecar records the
// finished pieces, so a stopped download resumes only what is missing.

std::optional<std::string> header_value(const ResponseHead& head, const std::string_view name) {
  const auto values = header_values(head, name);
  if (values.empty()) return std::nullopt;
  return std::string{values.front()};
}

// "bytes <lo>-<hi>/<total>" -> total; nullopt on anything else.
std::optional<std::uint64_t> content_range_total(const std::string_view value) {
  const auto slash = value.rfind('/');
  if (slash == std::string_view::npos) return std::nullopt;
  const std::string_view total = value.substr(slash + 1);
  if (total.empty() || total.size() > 20U) return std::nullopt;
  std::uint64_t parsed = 0;
  for (const char c : total) {
    if (c < '0' || c > '9') return std::nullopt;
    parsed = parsed * 10U + static_cast<std::uint64_t>(c - '0');
  }
  return parsed;
}

// §3.5 for one hop: resolve the host and pin a permitted address into `request`.
std::optional<AcquireReason> pin_request(FetchContext& ctx, const HttpsUrl& url, TransferRequest& request) {
  if (ctx.settings.proxy) return std::nullopt;  // parallel never runs with a proxy, but keep it total
  const auto answers = ctx.resolver.resolve(url.host);
  if (!answers || answers->empty()) return AcquireReason::dns_failed;
  const auto verdict = choose_pinned_address(*answers, ctx.settings.allow_private_addresses);
  if (verdict.kind != AddressVerdictKind::pinned) return AcquireReason::address_not_allowed;
  request.pin = verdict.pinned;
  return std::nullopt;
}

// A resolved parallel source: the final URL (after our redirects), the object ETag and total.
struct ResolvedSource final {
  HttpsUrl url;
  std::string etag;
  std::uint64_t total{0};
};

// The one-byte probe: follow redirects to the final URL and read its 206 head (ETag, total, range
// support). nullopt on anything that means "not parallelizable here" (no ETag, a 200 to the range,
// a wrong total) — the caller then uses the single stream. An AcquireError is a hard transport/host
// failure that should end the attempt.
std::variant<ResolvedSource, AcquireError, std::monostate> resolve_parallel_source(
    FetchContext& ctx, const ManifestFile& file, HttpsUrl current, const std::vector<std::string>& redirect_allowed) {
  for (int hop = 0;; ++hop) {
    if (ctx.interrupted.load()) return AcquireError{AcquireReason::interrupted, file.path};
    const auto remaining = ctx.budget.remaining_seconds(ctx.clock.now());
    if (!remaining) return AcquireError{AcquireReason::time_budget_exceeded, "pull.max_duration_s"};
    TransferRequest request = hop_request(current, ctx.settings, *remaining);
    request.range_from = 0;
    request.range_to = 0;  // one byte: a cheap 206 that carries Content-Range and ETag
    if (const auto reason = pin_request(ctx, current, request)) return AcquireError{*reason, current.host};

    ResponseHeadParser parser;
    std::optional<HeadDecision> decision;
    const auto on_bytes = [&](std::string_view bytes) -> bool {
      if (decision) return false;  // head seen; the one byte of body is not needed
      const auto fed = parser.feed(bytes);
      if (fed.state == ResponseHeadParser::State::need_more) return true;
      if (fed.state == ResponseHeadParser::State::malformed) return false;
      decision = classify_head(parser.head(), HopExpectation{file.size, request.range_from});
      return false;  // stop after the head either way
    };
    const HopExit exit = ctx.transport.run(request, on_bytes);
    if (exit.failure) return *exit.failure;
    if (ctx.interrupted.load()) return AcquireError{AcquireReason::interrupted, file.path};
    if (!decision) return std::monostate{};  // no usable head: fall back to one stream
    if (decision->action == HeadAction::redirect) {
      auto location = parse_location(decision->location, current.host);
      if (!std::holds_alternative<HttpsUrl>(location)) return std::monostate{};
      auto next = std::get<HttpsUrl>(std::move(location));
      if (!host_in(redirect_allowed, next.host)) return std::monostate{};
      if (hop + 1 > kMaxRedirectHops) return std::monostate{};
      current = std::move(next);
      continue;
    }
    // The final response. A 206 with an ETag and the right total is parallelizable; anything else
    // (a 200 that ignored the range, no ETag, a wrong total) is not — use one stream.
    if (parser.head().status != 206) return std::monostate{};
    const auto etag = header_value(parser.head(), "etag");
    const auto range = header_value(parser.head(), "content-range");
    if (!etag || etag->empty() || !range) return std::monostate{};
    const auto total = content_range_total(*range);
    if (!total || *total != file.size) return std::monostate{};
    return ResolvedSource{std::move(current), *etag, *total};
  }
}

// Fetch one run of contiguous pieces [first_piece, last_piece] (one request, range lo-hi) from the
// resolved URL, writing each byte at its offset under file_mutex. Returns the AcquireReason on
// failure (so the worker can retry or give up), or nullopt on success.
std::optional<AcquireReason> fetch_piece_run(FetchContext& ctx, const ResolvedSource& src, const ManifestFile& file,
                                             FetchTarget& target, std::mutex& file_mutex, const PieceMap& map,
                                             const std::uint64_t first_piece, const std::uint64_t last_piece) {
  const auto remaining = ctx.budget.remaining_seconds(ctx.clock.now());
  if (!remaining) return AcquireReason::time_budget_exceeded;
  const std::uint64_t lo = piece_span(map, first_piece).offset;
  const PieceSpan last = piece_span(map, last_piece);
  const std::uint64_t hi = last.offset + last.length - 1U;  // inclusive
  TransferRequest request = hop_request(src.url, ctx.settings, *remaining);
  request.range_from = lo;
  request.range_to = hi;
  if (const auto reason = pin_request(ctx, src.url, request)) return reason;

  ResponseHeadParser parser;
  bool head_seen = false;
  std::uint64_t written = 0;
  std::optional<AcquireReason> body_error;
  const auto on_bytes = [&](std::string_view bytes) -> bool {
    if (ctx.interrupted.load()) { body_error = AcquireReason::interrupted; return false; }
    if (!head_seen) {
      const auto fed = parser.feed(bytes);
      if (fed.state == ResponseHeadParser::State::malformed) { body_error = AcquireReason::response_malformed; return false; }
      if (fed.state == ResponseHeadParser::State::need_more) return true;
      head_seen = true;
      bytes.remove_prefix(fed.consumed);
      // A bounded piece is its own 206 (not the open-ended resume classify_head expects): the exact
      // Content-Range for [lo,hi] of this object, and the ETag unchanged (else it changed under us).
      const ResponseHead& head = parser.head();
      if (head.status != 206) { body_error = AcquireReason::range_mismatch; return false; }
      const auto etag = header_value(head, "etag");
      if (!etag || *etag != src.etag) { body_error = AcquireReason::range_mismatch; return false; }
      const auto ranges = header_values(head, "content-range");
      const std::string expected = "bytes " + std::to_string(lo) + "-" + std::to_string(hi) + "/" + std::to_string(file.size);
      if (ranges.size() != 1 || ranges.front() != expected) { body_error = AcquireReason::range_mismatch; return false; }
    }
    if (bytes.empty()) return true;
    if (lo + written + bytes.size() > hi + 1U) { body_error = AcquireReason::size_exceeded; return false; }
    {
      std::lock_guard lock(file_mutex);
      if (auto failure = target.file.write_at(lo + written, bytes)) { body_error = failure->reason; return false; }
    }
    written += bytes.size();
    return true;
  };
  const HopExit exit = ctx.transport.run(request, on_bytes);
  if (exit.failure) return exit.failure->reason;
  if (body_error) return body_error;
  if (!head_seen) return AcquireReason::response_malformed;
  if (written != (hi + 1U - lo)) return AcquireReason::size_short;
  if (exit.curl_exit.value_or(-1) != 0) return AcquireReason::transport_failed;
  return std::nullopt;
}

// The parallel attempt. nullopt → "not parallelizable, use the single stream"; otherwise an
// AttemptResult like run_attempt's.
std::optional<AttemptResult> run_parallel(FetchContext& ctx, const AcquireManifest& manifest, const ManifestFile& file,
                                          const std::string& source, const std::vector<std::string>& redirect_allowed,
                                          FetchTarget& target, FileState& state) {
  (void)state;
  auto parsed_source = parse_source_url(source);
  if (!std::holds_alternative<HttpsUrl>(parsed_source)) return fail(AcquireReason::manifest_invalid, file.path);
  HttpsUrl start = std::get<HttpsUrl>(std::move(parsed_source));
  if (!host_in(manifest.hosts, start.host)) return fail(AcquireReason::host_not_allowed, start.host);

  auto resolved = resolve_parallel_source(ctx, file, start, redirect_allowed);
  if (auto* error = std::get_if<AcquireError>(&resolved)) return fail_with(std::move(*error));
  if (std::holds_alternative<std::monostate>(resolved)) return std::nullopt;  // one stream
  const ResolvedSource src = std::get<ResolvedSource>(std::move(resolved));

  const RelativePath sidecar = must_rel(target.part.generic() + ".chunks");
  // Resume from the sidecar only when the object is unchanged; else start fresh.
  PieceMap map = piece_map_new(file.size, src.etag, ctx.settings.piece_bytes);
  if (auto existing = read_small_file(ctx.root, sidecar, 1U << 20U); std::holds_alternative<std::string>(existing)) {
    if (auto loaded = parse_piece_map(std::get<std::string>(existing));
        loaded && loaded->total == file.size && loaded->piece_bytes == ctx.settings.piece_bytes && loaded->etag == src.etag) {
      map = std::move(*loaded);
    }
  }
  if (map.done.empty() || all_done(map) == false) {
    if (auto failure = target.file.preallocate(file.size)) return fail_with(std::move(*failure));
  }

  const std::uint64_t count = static_cast<std::uint64_t>(map.done.size());
  std::mutex file_mutex;
  std::mutex map_mutex;
  std::uint64_t next_piece = 0;
  bool failed = false;
  AcquireReason fail_reason = AcquireReason::transport_failed;
  bool fail_transient = false;

  const auto worker = [&] {
    for (;;) {
      std::uint64_t first = 0;
      std::uint64_t last = 0;
      {
        std::lock_guard lock(map_mutex);
        if (failed) return;
        while (next_piece < count && map.done[static_cast<std::size_t>(next_piece)]) ++next_piece;
        if (next_piece >= count) return;
        first = next_piece;
        last = first;
        // A run of up to kDownloadRun contiguous undone pieces (fewer round trips, out of slow start).
        while (last + 1U < count && (last + 1U - first) < ctx.settings.pieces_per_run && !map.done[static_cast<std::size_t>(last + 1U)]) ++last;
        next_piece = last + 1U;
      }
      std::optional<AcquireReason> error;
      for (std::uint32_t attempt = 0; attempt < kDownloadRunTries; ++attempt) {
        error = fetch_piece_run(ctx, src, file, target, file_mutex, map, first, last);
        if (!error) break;
        if (is_run_fatal(*error) || *error == AcquireReason::interrupted || *error == AcquireReason::range_mismatch) break;
      }
      if (error) {
        std::lock_guard lock(map_mutex);
        if (!failed) { failed = true; fail_reason = *error; fail_transient = is_transient(*error); }
        return;
      }
      std::string sidecar_text;
      {
        std::lock_guard lock(map_mutex);
        for (std::uint64_t p = first; p <= last; ++p) map.done[static_cast<std::size_t>(p)] = true;
        sidecar_text = serialize_piece_map(map);
      }
      (void)replace_small_file(ctx.root, sidecar, sidecar_text);
      {
        std::lock_guard lock(map_mutex);
        ctx.progress.file_bytes(file.path, done_bytes(map), file.size);
      }
    }
  };

  const std::uint32_t worker_count = std::min<std::uint32_t>(ctx.settings.connections, static_cast<std::uint32_t>(count == 0 ? 1 : count));
  std::vector<std::thread> workers;
  workers.reserve(worker_count);
  for (std::uint32_t i = 0; i < worker_count; ++i) workers.emplace_back(worker);
  for (auto& thread : workers) thread.join();

  if (failed) return fail(fail_reason, file.path, fail_transient);
  if (ctx.interrupted.load()) return fail(AcquireReason::interrupted, file.path);

  // The whole file is on disk. Sync and close the handle first, then hash the file in one pass
  // (hash_into opens it read-only; Windows will not share a handle still open for writing), and
  // publish. A mismatch discards everything.
  if (auto failure = target.file.sync()) return fail_with(std::move(*failure));
  target.file.close();
  Sha256 hasher;
  const auto hashed = hash_into(ctx.root, target.part, hasher, file.size, &ctx.interrupted);
  if (std::holds_alternative<AcquireError>(hashed)) return fail_with(std::get<AcquireError>(hashed));
  if (to_hex(hasher.finish()) != file.sha256) {
    (void)remove_file_within(ctx.root, target.part);
    (void)remove_file_within(ctx.root, sidecar);
    return fail(AcquireReason::digest_mismatch, file.path);
  }
  (void)remove_file_within(ctx.root, sidecar);
  if (auto failure = rename_within(ctx.root, target.part, target.final_path, ctx.store)) return fail_with(std::move(*failure));
  return AttemptResult{std::nullopt, false, true, false};
}

[[nodiscard]] bool parallel_applicable(const FetchSettings& settings, const ManifestFile& file) {
  return settings.connections > 1U && !settings.proxy && file.size >= settings.min_parallel_bytes &&
         piece_count(file.size, settings.piece_bytes) >= 2U;
}

// One attempt: the manifest source URL plus at most kMaxRedirectHops of our own redirects.
AttemptResult run_attempt(FetchContext& ctx, const AcquireManifest& manifest, const ManifestFile& file, const std::string& source,
                          const std::vector<std::string>& redirect_allowed, FetchTarget& target, FileState& state) {
  auto parsed_source = parse_source_url(source);
  if (!std::holds_alternative<HttpsUrl>(parsed_source)) return fail(AcquireReason::manifest_invalid, file.path);
  HttpsUrl current = std::get<HttpsUrl>(std::move(parsed_source));
  if (!host_in(manifest.hosts, current.host)) return fail(AcquireReason::host_not_allowed, current.host);
  if (target.file.size() > file.size) {
    if (auto failure = reset(target)) return fail_with(std::move(*failure));
  }
  // §3.7 "L == size is hashed": nothing left to transfer (a --range would only earn a 416).
  if (target.file.size() == file.size) return verify_and_finish(ctx, file, target, state, true);
  for (int hop = 0;; ++hop) {
    if (ctx.interrupted.load()) return fail(AcquireReason::interrupted, file.path);
    const auto remaining = ctx.budget.remaining_seconds(ctx.clock.now());
    if (!remaining) return fail(AcquireReason::time_budget_exceeded, "pull.max_duration_s");
    TransferRequest request = hop_request(current, ctx.settings, *remaining);
    if (!ctx.settings.proxy) {
      // §3.5: resolve and check before every hop; any denied answer rejects it.
      const auto answers = ctx.resolver.resolve(current.host);
      if (!answers || answers->empty()) return fail(AcquireReason::dns_failed, current.host);
      const auto verdict = choose_pinned_address(*answers, ctx.settings.allow_private_addresses);
      // denied, and (F-13) an IPv6-only answer while the IPv6 pin is unverified.
      if (verdict.kind != AddressVerdictKind::pinned) return fail(AcquireReason::address_not_allowed, current.host);
      request.pin = verdict.pinned;
    }  // else: the proxy resolves the target; §3.5 cannot see it (§13.2)
    if (target.file.size() > 0) request.range_from = target.file.size();

    ResponseHeadParser parser;
    std::optional<HeadDecision> decision;
    std::optional<AcquireError> body_error;
    bool writing = false;
    bool resumed = false;
    const auto on_bytes = [&](std::string_view bytes) -> bool {
      if (ctx.interrupted.load()) {
        body_error = AcquireError{AcquireReason::interrupted, file.path};
        return false;
      }
      if (!decision) {
        const auto fed = parser.feed(bytes);
        if (fed.state == ResponseHeadParser::State::malformed) {
          body_error = AcquireError{AcquireReason::response_malformed, current.host};
          return false;
        }
        if (fed.state == ResponseHeadParser::State::need_more) return true;
        decision = classify_head(parser.head(), HopExpectation{file.size, request.range_from});
        bytes.remove_prefix(fed.consumed);
        if (decision->action == HeadAction::restart_from_zero) {
          if (state.restarted_after_ignored_range) {
            body_error = AcquireError{AcquireReason::range_mismatch, current.host};
            return false;
          }
          state.restarted_after_ignored_range = true;
          if (auto failure = reset(target)) {
            body_error = std::move(failure);
            return false;
          }
          writing = true;
        } else if (decision->action == HeadAction::write_body) {
          writing = true;
          resumed = request.range_from.has_value();  // 206: appended to bytes already on disk
        } else {
          return false;  // redirect or failure: the body is not needed
        }
      }
      if (!writing || bytes.empty()) return true;
      if (target.file.size() + bytes.size() > file.size) {
        body_error = AcquireError{AcquireReason::size_exceeded, file.path};
        return false;
      }
      if (auto failure = target.file.append(bytes)) {
        body_error = std::move(failure);
        return false;
      }
      target.hasher.update(bytes);
      state.written_since_check += bytes.size();
      if (state.written_since_check >= ctx.store.recheck_interval_bytes()) {
        state.written_since_check = 0;
        const auto space = ctx.store.free_bytes(ctx.root);
        if (!std::holds_alternative<std::uint64_t>(space) || std::get<std::uint64_t>(space) < kFreeSpaceMarginBytes) {
          body_error = AcquireError{AcquireReason::disk_space, file.path};
          return false;
        }
      }
      ctx.progress.file_bytes(file.path, target.file.size(), file.size);
      return true;
    };
    const HopExit exit = ctx.transport.run(request, on_bytes);
    if (exit.failure) return fail_with(*exit.failure);
    if (body_error) return fail_with(std::move(*body_error));
    if (ctx.interrupted.load()) return fail(AcquireReason::interrupted, file.path);
    const bool budget_gone = !ctx.budget.remaining_seconds(ctx.clock.now());
    if (!decision) {
      // No complete header block: the exit code says why (0 = curl ended without a response).
      const AcquireReason reason = !exit.curl_exit     ? AcquireReason::transport_failed
                                   : *exit.curl_exit == 0 ? AcquireReason::response_malformed
                                                          : map_curl_exit(*exit.curl_exit, budget_gone).value_or(AcquireReason::transport_failed);
      return fail(reason, current.host);
    }
    if (decision->action == HeadAction::redirect) {
      // §3.4: Location grammar (<= 8 KiB) and hosts ∪ redirect_hosts; the value is never printed.
      auto location = parse_location(decision->location, current.host);
      if (!std::holds_alternative<HttpsUrl>(location)) return fail(AcquireReason::redirect_not_allowed, current.host);
      auto next = std::get<HttpsUrl>(std::move(location));
      if (!host_in(redirect_allowed, next.host)) return fail(AcquireReason::redirect_not_allowed, current.host);
      if (hop + 1 > kMaxRedirectHops) return fail(AcquireReason::redirect_limit, current.host);
      current = std::move(next);
      continue;
    }
    if (decision->action == HeadAction::fail) {
      std::string detail = current.host;
      if (decision->reason == AcquireReason::http_status) detail += " HTTP " + std::to_string(decision->status);
      return fail(decision->reason, std::move(detail), decision->transient);
    }
    if (exit.curl_exit.value_or(-1) != 0) {
      const AcquireReason reason = exit.curl_exit ? map_curl_exit(*exit.curl_exit, budget_gone).value_or(AcquireReason::transport_failed)
                                                  : AcquireReason::transport_failed;
      return fail(reason, current.host);
    }
    if (target.file.size() < file.size) return fail(AcquireReason::size_short, current.host);
    return verify_and_finish(ctx, file, target, state, resumed);
  }
}

std::string source_host(const std::string& source) {
  const auto parsed = parse_source_url(source);
  const auto* url = std::get_if<HttpsUrl>(&parsed);
  return url == nullptr ? std::string{} : url->host;
}

}  // namespace

bool is_run_fatal(const AcquireReason reason) noexcept {
  switch (reason) {
    case AcquireReason::interrupted:
    case AcquireReason::disk_space:
    case AcquireReason::time_budget_exceeded:
    case AcquireReason::publish_failed:
    case AcquireReason::downloader_missing:
    case AcquireReason::downloader_version:
    case AcquireReason::downloader_untrusted_path:
    case AcquireReason::downloader_unsupported:
    case AcquireReason::downloader_confinement_failed:
      return true;
    default:
      return false;
  }
}

TransferRequest hop_request(const HttpsUrl& url, const FetchSettings& settings, const std::uint64_t max_time_s) {
  TransferRequest request;
  request.url = render_url(url);
  request.host = url.host;
  request.connect_timeout_s = settings.connect_timeout_s;
  request.stall_timeout_s = settings.stall_timeout_s;
  request.max_time_s = max_time_s;
  request.proxy = settings.proxy;
#if defined(SECURE_FRONTEND_TEST_HOOKS)
  request.test_cacert = settings.test_cacert;
#endif
  return request;
}

std::optional<AcquireError> fetch_file(FetchContext& ctx, const AcquireManifest& manifest, const std::size_t file_index, FetchTarget& target) {
  const auto& file = manifest.files[file_index];
  if (file.sources.empty()) return AcquireError{AcquireReason::manifest_requires_bundle, file.path};
  std::vector<std::string> redirect_allowed = manifest.hosts;
  redirect_allowed.insert(redirect_allowed.end(), manifest.redirect_hosts.begin(), manifest.redirect_hosts.end());
  SourceSchedule schedule(file.sources.size());
  FileState state;
  ctx.progress.file_started(file.path, file.size);
  while (true) {
    const std::string& source = file.sources[schedule.source()];
    const std::string host = source_host(source);
    if (ctx.interrupted.load()) {
      ctx.progress.file_finished(file.path, file.size, file.sha256, host, reason_name(AcquireReason::interrupted));
      return AcquireError{AcquireReason::interrupted, file.path};
    }
    // §3.6: each attempt starts again at the manifest's source URL. With models.download_connections
    // the parallel path runs first; it returns nullopt (use one stream) when the file is small or the
    // server does not honour ranges with a stable ETag.
    AttemptResult result;
    if (parallel_applicable(ctx.settings, file)) {
      auto parallel = run_parallel(ctx, manifest, file, source, redirect_allowed, target, state);
      result = parallel.has_value() ? std::move(*parallel) : run_attempt(ctx, manifest, file, source, redirect_allowed, target, state);
    } else {
      result = run_attempt(ctx, manifest, file, source, redirect_allowed, target, state);
    }
    if (result.done) {
      ctx.progress.file_finished(file.path, file.size, file.sha256, host, "ok");
      return std::nullopt;
    }
    if (result.retry_now) continue;  // §3.7: refetch once from byte 0, same source, no back-off
    const AcquireError error = std::move(*result.error);
    if (is_run_fatal(error.reason)) {
      ctx.progress.file_finished(file.path, file.size, file.sha256, host, reason_name(error.reason));
      return AcquireError{error.reason, file.path + ": " + error.detail};
    }
    const auto same = reason_info(error.reason).same_source;
    if (same == SameSource::restart_file || same == SameSource::next_source_file_deleted) {
      if (auto failure = reset(target)) return failure;
    }
    // Task 7 carry: the reason table decides, plus 429/5xx from the status line.
    const auto step = schedule.record_failure(error.reason, is_transient(error.reason) || result.status_transient);
    if (step.kind == SourceSchedule::Step::Kind::exhausted) {
      if (exit_class(error.reason) == ExitCode::verification) {
        target.file.close();
        (void)remove_file_within(ctx.root, target.part);
      }
      ctx.progress.file_finished(file.path, file.size, file.sha256, host, reason_name(error.reason));
      return AcquireError{error.reason, file.path + ": " + error.detail};
    }
    if (step.wait.count() > 0) ctx.clock.sleep_for(step.wait);
  }
}

}  // namespace secure_frontend
