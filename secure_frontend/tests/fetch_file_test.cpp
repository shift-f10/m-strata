#include "fetch_cases.hpp"
#include "secure_frontend/fetch_file.hpp"
#include "secure_frontend/sha256.hpp"
#include "support/expect.hpp"
#include "support/scripted_transport.hpp"
#include "support/test_support.hpp"


#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace fetch_cases {
namespace {

using secure_frontend::AcquireReason;
using secure_frontend::must_rel;
using secure_frontend_test::expect;

const std::string kBody = "0123456789";  // the file: 10 bytes
const std::string kUrl = "https://fixture.example/f.bin";

class SpaceEnvironment final : public secure_frontend::StoreEnvironment {
 public:
  std::uint64_t free{std::uint64_t{1} << 40U};
  std::variant<std::uint64_t, secure_frontend::AcquireError> free_bytes(const secure_frontend::CanonicalRoot&) const override { return free; }
};

struct Harness final {
  explicit Harness(const std::string& scenario_text, std::vector<std::string> sources = {"https://fixture.example/f.bin"},
                   const std::chrono::seconds budget_total = std::chrono::seconds{172800})
      : scratch(secure_frontend_test::make_scratch_directory()),
        root(std::get<secure_frontend::CanonicalRoot>(secure_frontend::open_root(scratch))),
        transport(*secure_frontend_test::curl::parse_scenario(scenario_text), {{"f.bin", kBody}}),
        budget(clock.now(), budget_total) {
    manifest.hosts = {"fixture.example", "mirror.example"};
    manifest.redirect_hosts = {"*.cdn.fixture.example"};
    manifest.files = {secure_frontend::ManifestFile{"f.bin", secure_frontend::FileRole::weights, kBody.size(),
                                                    secure_frontend::to_hex(secure_frontend::sha256(kBody)), std::move(sources), {}}};
    resolver.answers["fixture.example"] = {secure_frontend::make_ipv4(93, 184, 216, 34)};
    resolver.answers["mirror.example"] = {secure_frontend::make_ipv4(93, 184, 216, 35)};
    resolver.answers["us.cdn.fixture.example"] = {secure_frontend::make_ipv4(93, 184, 216, 36)};
    (void)secure_frontend::ensure_directory(root, must_rel("k"));
  }

  // Seeds the .part with `existing` bytes (a previous run), then fetches.
  std::optional<secure_frontend::AcquireError> run(const std::string& existing = "") {
    if (!existing.empty()) (void)secure_frontend::write_new_file(root, must_rel("k/f.bin.part"), existing);
    auto opened = secure_frontend::open_part_file(root, must_rel("k/f.bin.part"),
                                                  existing.empty() ? secure_frontend::PartOpen::create_new : secure_frontend::PartOpen::open_existing, env);
    auto& file = std::get<secure_frontend::PartFile>(opened);
    secure_frontend::Sha256 hasher;
    hasher.update(existing);
    secure_frontend::FetchTarget target{must_rel("k/f.bin.part"), must_rel("k/f.bin"), file, hasher, !existing.empty()};
    secure_frontend::FetchContext context{transport, resolver, clock, budget, settings, interrupted, progress, env, root};
    return secure_frontend::fetch_file(context, manifest, 0, target);
  }

  std::string final_bytes() const { return secure_frontend_test::read_file(root.join(must_rel("k/f.bin"))); }
  std::string part_bytes() const { return secure_frontend_test::read_file(root.join(must_rel("k/f.bin.part"))); }
  bool part_exists() const { return secure_frontend::exists_within(root, must_rel("k/f.bin.part")); }

  std::filesystem::path scratch;
  secure_frontend::CanonicalRoot root;
  SpaceEnvironment env;
  secure_frontend::AcquireManifest manifest;
  secure_frontend_test::ManualClock clock;
  secure_frontend_test::ScriptedResolver resolver;
  secure_frontend_test::ScriptedTransport transport;
  secure_frontend::RunBudget budget;
  secure_frontend::FetchSettings settings;
  std::atomic<bool> interrupted{false};
  secure_frontend::ProgressSink progress;
};

std::string ok_response(const std::string& url = kUrl) {
  return "response " + url + "\nstatus 200 OK\nheader Content-Length: 10\nbody-file f.bin\nend\n";
}

class RecheckEnvironment final : public secure_frontend::StoreEnvironment {
 public:
  std::variant<std::uint64_t, secure_frontend::AcquireError> free_bytes(const secure_frontend::CanonicalRoot&) const override { return std::uint64_t{0}; }
  std::uint64_t recheck_interval_bytes() const override { return 4; }
};

const std::vector<std::chrono::seconds> kFourBackoffs{std::chrono::seconds{2}, std::chrono::seconds{4}, std::chrono::seconds{8},
                                                      std::chrono::seconds{16}};

}  // namespace

// Parallel ranged downloads (spec 2026-10-23-parallel-downloader-design.md). The 10-byte file over
// 4-byte pieces is three pieces [0-3][4-7][8-9]; with connections > 1 they are fetched over several
// confined children and written at their offsets, then the whole file is hashed and published.
void dl_parallel_assembles_pieces() {
  const std::string scenario =
      "response " + kUrl + " range 0\nstatus 206 Partial\nheader ETag: \"v1\"\nheader Content-Range: bytes 0-3/10\nbody-text 0123\nend\n" +
      "response " + kUrl + " range 4\nstatus 206 Partial\nheader ETag: \"v1\"\nheader Content-Range: bytes 4-7/10\nbody-text 4567\nend\n" +
      "response " + kUrl + " range 8\nstatus 206 Partial\nheader ETag: \"v1\"\nheader Content-Range: bytes 8-9/10\nbody-text 89\nend\n";
  Harness h(scenario);
  h.settings.connections = 3;
  h.settings.piece_bytes = 4;
  h.settings.min_parallel_bytes = 0;
  h.settings.pieces_per_run = 1;
  expect(!h.run().has_value(), "the parallel download succeeds");
  expect(h.final_bytes() == kBody, "the pieces assemble into the whole file: " + h.final_bytes());
  expect(!h.part_exists(), "the .part is renamed to the final name");
  expect(!secure_frontend::exists_within(h.root, must_rel("k/f.bin.part.chunks")), "the .part.chunks sidecar is removed on success");
}

// A server that answers the range probe with 200 (ignores ranges) falls back to one stream.
void dl_parallel_falls_back_without_ranges() {
  const std::string scenario =
      "response " + kUrl + " range 0\nstatus 200 OK\nheader Content-Length: 10\nbody-file f.bin\nend\n" +
      "response " + kUrl + "\nstatus 200 OK\nheader Content-Length: 10\nbody-file f.bin\nend\n";
  Harness h(scenario);
  h.settings.connections = 3;
  h.settings.piece_bytes = 4;
  h.settings.min_parallel_bytes = 0;
  expect(!h.run().has_value(), "the download still succeeds over one stream");
  expect(h.final_bytes() == kBody, "the whole file arrives over one stream");
}

// Resume: a sidecar marking piece 0 done, and the seeded bytes on disk, so piece 0 is not fetched
// again (its scripted body is poison that would fail the hash if it were written).
void dl_parallel_resumes_from_sidecar() {
  const std::string scenario =
      "response " + kUrl + " range 0\nstatus 206 Partial\nheader ETag: \"v1\"\nheader Content-Range: bytes 0-3/10\nbody-text XXXX\nend\n" +
      "response " + kUrl + " range 4\nstatus 206 Partial\nheader ETag: \"v1\"\nheader Content-Range: bytes 4-7/10\nbody-text 4567\nend\n" +
      "response " + kUrl + " range 8\nstatus 206 Partial\nheader ETag: \"v1\"\nheader Content-Range: bytes 8-9/10\nbody-text 89\nend\n";
  Harness h(scenario);
  h.settings.connections = 3;
  h.settings.piece_bytes = 4;
  h.settings.min_parallel_bytes = 0;
  h.settings.pieces_per_run = 1;
  secure_frontend::PieceMap map = secure_frontend::piece_map_new(10, "\"v1\"", 4);
  map.done[0] = true;  // piece 0 ("0123") is already on disk
  (void)secure_frontend::write_new_file(h.root, must_rel("k/f.bin.part.chunks"), secure_frontend::serialize_piece_map(map));
  expect(!h.run("0123").has_value(), "the resumed parallel download succeeds");
  expect(h.final_bytes() == kBody, "piece 0 was kept (not refetched as poison): " + h.final_bytes());
}


// Owner 2026-10-29 (Qwen3.8's MTP draft layer, upstream's tools/mtp_fetch.py): a file that is a window of its source -
// bytes [offset, offset + size) of a checkpoint shard, its tensor - is fetched with that bounded range and taken only
// from a 206 of exactly that range (any total); a resume stays inside the window; a 200 (the whole shard, its header
// first: #327) or another range is refused, never restarted from byte 0.
void a_window_of_its_source_is_fetched_by_its_range() {
  Harness h("response " + kUrl + " range 100\nstatus 206 Partial\nheader Content-Range: bytes 100-109/5000\n"
            "header Content-Length: 10\nbody-text 0123456789\nend\n");
  h.manifest.files[0].source_offset = 100;
  expect(!h.run().has_value(), "the window arrives");
  expect(h.final_bytes() == kBody, "its bytes, verified: " + h.final_bytes());
  expect(h.transport.argvs.size() == 1U && std::find(h.transport.argvs[0].begin(), h.transport.argvs[0].end(), "100-109") != h.transport.argvs[0].end(),
         "one bounded request for bytes 100-109");
  Harness resumed("response " + kUrl + " range 103\nstatus 206 Partial\nheader Content-Range: bytes 103-109/5000\nbody-text 3456789\nend\n");
  resumed.manifest.files[0].source_offset = 100;
  expect(!resumed.run("012").has_value() && resumed.final_bytes() == kBody, "a resume asks for the rest of the window only");
  // every try asks for the window again (the answers are not consumed): never for the whole source
  const auto always_the_window = [](const Harness& harness) {
    return !harness.transport.argvs.empty() &&
           std::all_of(harness.transport.argvs.begin(), harness.transport.argvs.end(),
                       [](const std::vector<std::string>& argv) { return std::find(argv.begin(), argv.end(), "100-109") != argv.end(); });
  };
  Harness whole("response " + kUrl + " range 100\nstatus 200 OK\nbody-text 0123456789\nend\n");
  whole.manifest.files[0].source_offset = 100;
  const auto refused = whole.run();
  expect(refused && refused->reason == AcquireReason::range_mismatch && always_the_window(whole),
         "a 200 (the whole source) is refused, not restarted from 0");
  Harness other("response " + kUrl + " range 100\nstatus 206 Partial\nheader Content-Range: bytes 0-9/5000\nbody-text 0123456789\nend\n");
  other.manifest.files[0].source_offset = 100;
  const auto wrong = other.run();
  expect(wrong && wrong->reason == AcquireReason::range_mismatch && always_the_window(other), "another range is refused");
}

// The same window over parallel pieces: the probe asks for its first byte, each piece for its bytes at the window's
// offset, each answer the exact range of the source's total.
void dl_parallel_fetches_a_window() {
  // the probe and the first piece both start at byte 100: one answer serves them (the probe reads only its head)
  const std::string scenario =
      "response " + kUrl + " range 100\nstatus 206 Partial\nheader ETag: \"v1\"\nheader Content-Range: bytes 100-103/5000\nbody-text 0123\nend\n" +
      "response " + kUrl + " range 104\nstatus 206 Partial\nheader ETag: \"v1\"\nheader Content-Range: bytes 104-107/5000\nbody-text 4567\nend\n" +
      "response " + kUrl + " range 108\nstatus 206 Partial\nheader ETag: \"v1\"\nheader Content-Range: bytes 108-109/5000\nbody-text 89\nend\n";
  Harness h(scenario);
  h.manifest.files[0].source_offset = 100;
  h.settings.connections = 3;
  h.settings.piece_bytes = 4;
  h.settings.min_parallel_bytes = 0;
  h.settings.pieces_per_run = 1;
  const auto failure = h.run();
  expect(!failure.has_value(), "the window arrives in pieces: " + (failure ? failure->detail : std::string{}));
  expect(h.final_bytes() == kBody, "the pieces assemble into the window: " + h.final_bytes());
}

// F-49: every retry resumes at byte 3 and is truncated again, so the run really ends on
// the truncation path (size_short), not on an unscripted request.
void dl03_truncated_body_rejected() {
  Harness h("response " + kUrl + "\nstatus 200 OK\nbody-file f.bin\ntruncate-at 3\nexit 18\nend\n" +
            "response " + kUrl + " range 3\nstatus 206 Partial\nheader Content-Range: bytes 3-9/10\nbody-text 3456789\n"
            "truncate-at 0\nexit 18\nend\n");
  const auto failure = h.run();
  expect(failure.has_value() && failure->reason == AcquireReason::size_short, "a truncated body is size_short");
  expect(failure.has_value() && secure_frontend::exit_class(failure->reason) == secure_frontend::ExitCode::transport,
         "a truncated body is never accepted; the run ends with a transport class");
  expect(h.transport.argvs.size() == 5 && h.clock.sleeps == kFourBackoffs, "5 attempts on the one source, back-off 2/4/8/16");
  expect(h.transport.argvs.size() == 5 && h.transport.argvs[4].back() == "3-", "retries resume from the bytes on disk");
  expect(!secure_frontend::exists_within(h.root, must_rel("k/f.bin")), "nothing renamed to the final name");
  expect(h.part_exists() && h.part_bytes() == "012", "partial bytes kept for a resume");
}

void dl03_oversize_body_rejected_mid_stream() {
  Harness h("response " + kUrl + "\nstatus 200 OK\nbody-file f.bin\nexceed-by 5\nend\n");
  const auto failure = h.run();
  expect(failure && failure->reason == AcquireReason::size_exceeded, "one byte past size kills the transfer");
  expect(failure && secure_frontend::exit_class(failure->reason) == secure_frontend::ExitCode::verification, "class 4");
  expect(!h.part_exists(), "the file's partial bytes are deleted");
}

void dl03_redirect_to_http_rejected() {
  Harness h("response " + kUrl + "\nstatus 302 Found\nheader Location: http://fixture.example/f.bin\nend\n");
  const auto failure = h.run();
  expect(failure && failure->reason == AcquireReason::redirect_not_allowed && h.transport.argvs.size() == 1, "never followed");
}

void dl03_redirect_off_allowlist_rejected() {
  Harness h("response " + kUrl + "\nstatus 302 Found\nheader Location: https://evil.example/f.bin\nend\n");
  const auto failure = h.run();
  expect(failure && failure->reason == AcquireReason::redirect_not_allowed && h.transport.argvs.size() == 1, "host outside the allowlist");
  expect(failure && failure->detail.find("evil") == std::string::npos && failure->detail.find("/f.bin") == std::string::npos,
         "no Location value in the error");
}

void dl03_redirect_limit() {
  std::string scenario;
  for (int i = 0; i < 5; ++i) {
    const std::string from = i == 0 ? kUrl : "https://us.cdn.fixture.example/" + std::to_string(i);
    scenario += "response " + from + "\nstatus 302 Found\nheader Location: https://us.cdn.fixture.example/" + std::to_string(i + 1) + "\nend\n";
  }
  Harness h(scenario);
  const auto failure = h.run();
  expect(failure && failure->reason == AcquireReason::redirect_limit, "a fourth redirect is redirect_limit");
  expect(h.transport.argvs.size() == 4, "initial request + 3 followed hops");
}

void redirect_query_only_in_config_line() {
  Harness h("response " + kUrl + "\nstatus 302 Found\nheader Location: https://us.cdn.fixture.example/x?sig=SENTINEL-QUERY\nend\n" +
            ok_response("https://us.cdn.fixture.example/x?sig=SENTINEL-QUERY"));
  expect(!h.run().has_value(), "redirected download verifies");
  expect(h.final_bytes() == kBody, "published bytes");
  for (const auto& argv : h.transport.argvs) {
    for (const auto& arg : argv) expect(arg.find("SENTINEL") == std::string::npos, "the signed query never reaches argv");
  }
  expect(h.transport.configs.size() == 2 && h.transport.configs[1].find("SENTINEL-QUERY") != std::string::npos, "only the stdin config");
  // Task 5 carry: the pinned host is the host of the URL in the same hop's config line.
  expect(h.transport.configs.size() == 2 && h.transport.configs[0] == "url = \"" + kUrl + "\"\n" &&
             h.transport.argvs[0].back() == "fixture.example:443:93.184.216.34",
         "the first hop pins the source host");
  expect(h.transport.configs.size() == 2 && h.transport.configs[1].starts_with("url = \"https://us.cdn.fixture.example/") &&
             h.transport.argvs[1].back() == "us.cdn.fixture.example:443:93.184.216.36",
         "the next hop re-runs DNS policy and is pinned");
  expect((h.resolver.queries == std::vector<std::string>{"fixture.example", "us.cdn.fixture.example"}), "one resolution per hop");
}

void dl03_private_address_rejected() {
  Harness h(ok_response());
  h.resolver.answers["fixture.example"] = {secure_frontend::make_ipv4(10, 0, 0, 1)};
  const auto failure = h.run();
  expect(failure && failure->reason == AcquireReason::address_not_allowed && h.transport.argvs.empty(), "never spawned");
  expect(failure && failure->detail.find("10.0.0.1") == std::string::npos, "no IP address in the error");
}

void dl03_mixed_public_private_answer_rejected() {
  Harness h(ok_response());
  h.resolver.answers["fixture.example"] = {secure_frontend::make_ipv4(93, 184, 216, 34), secure_frontend::make_ipv4(127, 0, 0, 1)};
  const auto failure = h.run();
  expect(failure && failure->reason == AcquireReason::address_not_allowed && h.transport.argvs.empty(), "any denied answer");
  Harness allowed(ok_response());
  allowed.settings.allow_private_addresses = true;
  allowed.resolver.answers["fixture.example"] = {secure_frontend::make_ipv4(10, 0, 0, 1)};
  expect(!allowed.run().has_value(), "pull.allow_private_addresses = true");
}

void dl03_range_ignored_restarts_from_zero() {
  Harness h("response " + kUrl + " range 3\nstatus 200 OK\nbody-file f.bin\nend\n");
  expect(!h.run("012").has_value(), "200 to a range request: restart from byte 0, once");
  expect(h.final_bytes() == kBody, "no spliced bytes");
}

void dl03_content_range_mismatch_rejected() {
  Harness h("response " + kUrl + " range 3\nstatus 206 Partial\nheader Content-Range: bytes 0-9/10\nbody-file f.bin\nend\n" + ok_response());
  expect(!h.run("012").has_value(), "the mismatched 206 is rejected; the file restarts from 0");
  expect(h.transport.argvs.size() == 2 && h.transport.argvs[1].back() != "3-", "second request has no --range");
  expect(h.final_bytes() == kBody, "verified");
}

void dl03_content_encoding_gzip_fails_digest() {
  Harness h("response " + kUrl + "\nstatus 200 OK\nheader Content-Encoding: gzip\nbody-text gzipped!!!\nend\n");
  const auto failure = h.run();
  expect(failure && failure->reason == AcquireReason::digest_mismatch, "raw bytes are hashed; encoded bytes fail the digest");
  expect(!h.part_exists(), "deleted");
}

void dl03_stalled_source_times_out() {
  Harness h(ok_response());
  h.transport.forced_exits = {28, 28, 28, 28, 28};
  const auto failure = h.run();
  expect(failure && failure->reason == AcquireReason::stalled, "curl 28 below 1024 B/s: stalled");
  expect(h.clock.sleeps == kFourBackoffs, "back-off 2, 4, 8, 16 between the 5 attempts");
}

void time_budget_exceeded_is_resumable() {
  Harness h(ok_response(), {"https://fixture.example/f.bin"}, std::chrono::seconds{10});
  h.transport.forced_exits = {7, 7, 7, 7, 7};
  const auto failure = h.run();
  expect(failure && failure->reason == AcquireReason::time_budget_exceeded, "pull.max_duration_s used up");
  expect(failure && secure_frontend::reason_info(failure->reason).resumable &&
             secure_frontend::exit_class(failure->reason) == secure_frontend::ExitCode::transport,
         "resumable, class 3");
  expect(h.transport.argvs.size() == 3, "no hop starts once the budget is gone");
}

void tls_untrusted_moves_to_next_source() {
  Harness h(ok_response("https://mirror.example/f.bin"), {"https://fixture.example/f.bin", "https://mirror.example/f.bin"});
  h.transport.forced_exits = {60};
  expect(!h.run().has_value(), "the mirror serves the file");
  expect(h.clock.sleeps.empty() || h.clock.sleeps.front() == std::chrono::seconds{0}, "no retry on the same source");
  expect(h.transport.argvs.size() == 2, "exactly one attempt per source");
}

void digest_mismatch_after_resume_refetches_once() {
  Harness h("response " + kUrl + " range 3\nstatus 206 Partial\nheader Content-Range: bytes 3-9/10\nbody-text 3456789\nend\n" + ok_response());
  expect(!h.run("XYZ").has_value(), "stale staged bytes: mismatch after resume, refetched once from 0");
  expect(h.transport.argvs.size() == 2 && h.final_bytes() == kBody, "second fetch from byte 0 verifies");
}

void free_space_rechecked_while_writing() {
  Harness h(ok_response());
  const RecheckEnvironment low;
  auto opened = secure_frontend::open_part_file(h.root, must_rel("k/f.bin.part"), secure_frontend::PartOpen::create_new, low);
  auto& file = std::get<secure_frontend::PartFile>(opened);
  secure_frontend::Sha256 hasher;
  secure_frontend::FetchTarget target{must_rel("k/f.bin.part"), must_rel("k/f.bin"), file, hasher, false};
  secure_frontend::FetchContext context{h.transport, h.resolver, h.clock, h.budget, h.settings, h.interrupted, h.progress, low, h.root};
  const auto failure = secure_frontend::fetch_file(context, h.manifest, 0, target);
  expect(failure && failure->reason == AcquireReason::disk_space && h.part_exists(), "ENOSPC-class failure keeps the .part (resumable)");
}

// F-13 / spec §3.5: until the IPv6 --resolve form is verified, an IPv6-only hop fails closed.
void ipv6_only_hop_is_address_not_allowed() {
  Harness h(ok_response());
  h.resolver.answers["fixture.example"] = {secure_frontend::make_ipv6({0x2606, 0x2800, 0x220, 0x1, 0x248, 0x1893, 0x25c8, 0x1946})};
  const auto failure = h.run();
  expect(failure && failure->reason == AcquireReason::address_not_allowed && h.transport.argvs.empty(), "never spawned");
}

// Spec §3.4: the 200-to-a-range restart happens once per file; a second ignored range
// is range_mismatch, which restarts the file from 0 on the next attempt (a retry slot).
void range_ignored_twice_is_range_mismatch_then_restarts() {
  Harness h("response " + kUrl + " range 3\nstatus 200 OK\nbody-file f.bin\ntruncate-at 5\nexit 18\nend\n" +
            "response " + kUrl + " range 5\nstatus 200 OK\nbody-file f.bin\nend\n" + ok_response());
  expect(!h.run("012").has_value(), "verified after the restart");
  expect(h.final_bytes() == kBody, "no spliced bytes");
  expect(h.transport.argvs.size() == 3 && h.transport.argvs[2].back() != "5-", "third request is from byte 0");
  expect((h.clock.sleeps == std::vector<std::chrono::seconds>{std::chrono::seconds{2}, std::chrono::seconds{4}}),
         "size_short and range_mismatch each take a retry slot on the same source");
}

// Spec §3.5: with pull.proxy the proxy resolves the target; no --resolve, no local DNS.
void proxy_hop_skips_address_policy() {
  Harness h(ok_response());
  h.settings.proxy = secure_frontend::ProxySetting{false, "proxy.internal", 3128};
  h.resolver.answers.clear();
  expect(!h.run().has_value(), "fetched through the proxy");
  expect(h.resolver.queries.empty(), "no local resolution");
  expect(h.transport.argvs.size() == 1 && h.transport.argvs[0].back() == "http://proxy.internal:3128", "--proxy, no --resolve");
}

#if defined(SECURE_FRONTEND_TEST_HOOKS)
// F-51: FetchSettings::test_cacert reaches curl's argv in hook builds.
void test_cacert_reaches_argv_in_hook_builds() {
  Harness h(ok_response());
  h.settings.test_cacert = "/fixture/ca.pem";
  expect(!h.run().has_value(), "fetched");
  expect(h.transport.argvs.size() == 1 && h.transport.argvs[0].back() == "/fixture/ca.pem" &&
             h.transport.argvs[0][h.transport.argvs[0].size() - 2] == "--cacert",
         "--cacert from FetchSettings");
}
#endif

}  // namespace fetch_cases
