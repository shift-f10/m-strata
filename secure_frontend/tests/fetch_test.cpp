#include "secure_frontend/acquire_url.hpp"
#include "secure_frontend/address_policy.hpp"
#include "secure_frontend/confinement_plan.hpp"
#include "secure_frontend/download_pieces.hpp"
#include "secure_frontend/download_response.hpp"
#include "secure_frontend/downloader_command.hpp"
#include "secure_frontend/fetch_schedule.hpp"
#include "secure_frontend/model_index_model.hpp"
#include "fetch_cases.hpp"
#include "support/curl_scenario.hpp"
#include "support/expect.hpp"
#include "support/fake_curl_support.hpp"
#include "support/test_support.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#else
#include "secure_frontend/win32_prelude.hpp"
#endif

namespace {

using secure_frontend_test::expect;

bool location_problem(const std::string_view value, const secure_frontend::UrlProblem expected) {
  const auto result = secure_frontend::parse_location(value, "huggingface.co");
  const auto* problem = std::get_if<secure_frontend::UrlProblem>(&result);
  return problem != nullptr && *problem == expected;
}

// Spec §9 "URL globbing makes curl fetch many URLs": [ ] { } never pass a Location.
void dl03_location_with_glob_characters_rejected() {
  using P = secure_frontend::UrlProblem;
  for (const auto* bad : {"https://us.aws.cdn.hf.co/a[1-9]", "https://us.aws.cdn.hf.co/{a,b}",
                          "https://us.aws.cdn.hf.co/a?x=[1]", "https://us.aws.cdn.hf.co/a?x={y}",
                          "https://us.aws.cdn.hf.co/a b", "https://us.aws.cdn.hf.co/a?x=\"y\"",
                          "https://us.aws.cdn.hf.co/a\\b"}) {
    expect(location_problem(bad, P::bad_character), std::string{"Location rejected: "} + bad);
  }
}

void location_resolution_and_host_normalization() {
  using P = secure_frontend::UrlProblem;
  const auto relative = secure_frontend::parse_location("/api/x", "huggingface.co");
  const auto* url = std::get_if<secure_frontend::HttpsUrl>(&relative);
  expect(url != nullptr && url->host == "huggingface.co" && url->target == "/api/x", "absolute path uses current host");
  const auto upper = secure_frontend::parse_location("https://US.AWS.CDN.HF.CO/x", "huggingface.co");
  const auto* lowered = std::get_if<secure_frontend::HttpsUrl>(&upper);
  expect(lowered != nullptr && lowered->host == "us.aws.cdn.hf.co", "Location host is lowercased");
  expect(location_problem("https://us.aws.cdn.hf.co./x", P::bad_host), "trailing dot rejected");
  expect(location_problem("http://us.aws.cdn.hf.co/x", P::not_https), "http Location");
  expect(location_problem("//us.aws.cdn.hf.co/x", P::not_https), "network-path reference");
  expect(location_problem("x/y", P::not_https), "relative path");
  expect(location_problem("https://us.aws.cdn.hf.co:444/x", P::port_not_allowed), "port 444");
  expect(location_problem("https://a@us.aws.cdn.hf.co/x", P::userinfo), "userinfo");
  expect(location_problem("https://us.aws.cdn.hf.co/x#f", P::fragment), "fragment");
  expect(location_problem("https://us.aws.cdn.hf.co/" + std::string(8200, 'a'), P::too_long), "over 8 KiB");
  const auto bare = secure_frontend::parse_location("https://us.aws.cdn.hf.co", "huggingface.co");
  const auto* bare_url = std::get_if<secure_frontend::HttpsUrl>(&bare);
  expect(bare_url != nullptr && bare_url->target == "/", "no path means /");
}

void location_query_characters() {
  const auto signed_url = secure_frontend::parse_location(
      "https://us.aws.cdn.hf.co/xet-bridge-us/abc?X-Xet-Signed-Range=bytes%3D0-1&Expires=1&Policy=a,b;c:d@e!f$g'h(i)j*k+l",
      "huggingface.co");
  expect(std::holds_alternative<secure_frontend::HttpsUrl>(signed_url), "signed CDN query characters accepted");
  expect(location_problem("https://us.aws.cdn.hf.co/a?b?c", secure_frontend::UrlProblem::bad_character),
         "second ? rejected");
  expect(location_problem("https://us.aws.cdn.hf.co/a=b", secure_frontend::UrlProblem::bad_character),
         "query-only characters are not path characters");
}

using secure_frontend::make_ipv4;
using secure_frontend::make_ipv6;

void address_policy_denies_special_ranges() {
  const std::vector<secure_frontend::ResolvedAddress> denied{
      make_ipv4(0, 1, 2, 3),        make_ipv4(10, 0, 0, 1),     make_ipv4(100, 64, 0, 1),   make_ipv4(100, 127, 255, 255),
      make_ipv4(127, 0, 0, 1),      make_ipv4(169, 254, 1, 1),  make_ipv4(172, 16, 0, 1),   make_ipv4(172, 31, 255, 255),
      make_ipv4(192, 0, 0, 8),      make_ipv4(192, 0, 2, 1),    make_ipv4(192, 168, 1, 1),  make_ipv4(198, 18, 0, 1),
      make_ipv4(198, 19, 255, 255), make_ipv4(198, 51, 100, 7), make_ipv4(203, 0, 113, 9),  make_ipv4(224, 0, 0, 1),
      make_ipv4(239, 1, 1, 1),      make_ipv4(240, 0, 0, 1),    make_ipv4(255, 255, 255, 255),
      make_ipv6({0, 0, 0, 0, 0, 0, 0, 0}),           make_ipv6({0, 0, 0, 0, 0, 0, 0, 1}),
      make_ipv6({0, 0, 0, 0, 0, 0, 0x0808, 0x0808}), make_ipv6({0, 0, 0, 0, 0, 0xffff, 0x0808, 0x0808}),
      make_ipv6({0x64, 0xff9b, 0, 0, 0, 0, 0x0808, 0x0808}), make_ipv6({0x64, 0xff9b, 1, 0, 0, 0, 0, 1}),
      make_ipv6({0x100, 0, 0, 0, 0, 0, 0, 1}),       make_ipv6({0x2001, 0, 1, 0, 0, 0, 0, 1}),
      make_ipv6({0x2001, 0xdb8, 0, 0, 0, 0, 0, 1}),  make_ipv6({0x2002, 0x0808, 0x0808, 0, 0, 0, 0, 1}),
      make_ipv6({0xfc00, 0, 0, 0, 0, 0, 0, 1}),      make_ipv6({0xfdff, 0, 0, 0, 0, 0, 0, 1}),
      make_ipv6({0xfe80, 0, 0, 0, 0, 0, 0, 1}),      make_ipv6({0xfebf, 0, 0, 0, 0, 0, 0, 1}),
      make_ipv6({0xff02, 0, 0, 0, 0, 0, 0, 1})};
  for (std::size_t i = 0; i < denied.size(); ++i) {
    expect(secure_frontend::is_denied_address(denied[i]), "denied #" + std::to_string(i) + " " +
                                                              secure_frontend::format_address(denied[i]));
  }
  const std::vector<secure_frontend::ResolvedAddress> allowed{
      make_ipv4(1, 1, 1, 1),        make_ipv4(9, 255, 255, 255), make_ipv4(11, 0, 0, 0),     make_ipv4(100, 63, 255, 255),
      make_ipv4(100, 128, 0, 0),    make_ipv4(172, 15, 255, 255), make_ipv4(172, 32, 0, 0),   make_ipv4(192, 0, 1, 1),
      make_ipv4(198, 17, 255, 255), make_ipv4(198, 20, 0, 0),     make_ipv4(223, 255, 255, 255),
      make_ipv6({0x2001, 0x4860, 0, 0, 0, 0, 0, 0x8888}), make_ipv6({0x2a00, 0x1450, 0, 0, 0, 0, 0, 1}),
      make_ipv6({0xfbff, 0, 0, 0, 0, 0, 0, 1}),           make_ipv6({0xfec0, 0, 0, 0, 0, 0, 0, 1}),
      make_ipv6({0x64, 0xff9b, 2, 0, 0, 0, 0, 1}),        make_ipv6({0x100, 0, 0, 1, 0, 0, 0, 1})};
  for (std::size_t i = 0; i < allowed.size(); ++i) {
    expect(!secure_frontend::is_denied_address(allowed[i]), "allowed #" + std::to_string(i) + " " +
                                                                secure_frontend::format_address(allowed[i]));
  }
}

// fec0::/10 (deprecated site-local) is allowed: spec §3.5's IPv6 list does not deny it (only fc00::/7 and fe80::/10).
void address_policy_any_denied_answer_rejects_hop() {
  const std::vector answers{make_ipv4(93, 184, 216, 34), make_ipv4(192, 168, 1, 1)};
  const auto verdict = secure_frontend::choose_pinned_address(answers, false);
  expect(verdict.kind == secure_frontend::AddressVerdictKind::denied, "a mixed public/private answer is denied");
  expect(secure_frontend::choose_pinned_address({}, false).kind == secure_frontend::AddressVerdictKind::no_usable_address,
         "no answers");
}

void ipv6_only_answer_fails_closed() {
  const std::vector answers{make_ipv6({0x2a00, 0x1450, 0, 0, 0, 0, 0, 1})};
  expect(secure_frontend::choose_pinned_address(answers, false).kind == secure_frontend::AddressVerdictKind::no_usable_address,
         "IPv6-only hops fail closed until the [addr] --resolve form is verified");
  const std::vector mixed{make_ipv6({0x2a00, 0x1450, 0, 0, 0, 0, 0, 1}), make_ipv4(1, 1, 1, 1)};
  const auto verdict = secure_frontend::choose_pinned_address(mixed, false);
  expect(verdict.kind == secure_frontend::AddressVerdictKind::pinned && !verdict.pinned.ipv6, "first IPv4 is pinned");
}

void allow_private_addresses_pins_first_ipv4() {
  const std::vector answers{make_ipv4(10, 0, 0, 7), make_ipv4(1, 1, 1, 1)};
  const auto verdict = secure_frontend::choose_pinned_address(answers, true);
  expect(verdict.kind == secure_frontend::AddressVerdictKind::pinned &&
             secure_frontend::format_address(verdict.pinned) == "10.0.0.7",
         "pull.allow_private_addresses pins the first address");
}

void resolve_argument_brackets_ipv6() {
  expect(secure_frontend::resolve_argument("huggingface.co", make_ipv4(1, 2, 3, 4)) == "huggingface.co:443:1.2.3.4", "IPv4");
  expect(secure_frontend::resolve_argument("h.example", make_ipv6({0x2a00, 0x1450, 0, 0, 0, 0, 0, 0x1f})) ==
             "h.example:443:[2a00:1450:0:0:0:0:0:1f]",
         "IPv6 in brackets, uncompressed");
}

void transfer_argv_exact_order() {
  secure_frontend::TransferRequest request;
  request.url = "https://huggingface.co/r/resolve/abc/f.gguf";
  request.host = "huggingface.co";
  request.pin = secure_frontend::make_ipv4(1, 2, 3, 4);
  request.range_from = 1000;
  request.connect_timeout_s = 30;
  request.stall_timeout_s = 120;
  request.max_time_s = 172800;
  const std::vector<std::string> expected{
      "/usr/bin/curl", "-q", "--config", "-", "--silent", "--include", "--suppress-connect-headers", "--globoff",
      "--proto", "=https", "--max-redirs", "0", "--tlsv1.2", "--connect-timeout", "30", "--speed-limit", "1024",
      "--speed-time", "120", "--max-time", "172800", "--user-agent", "strata", "--header", "Accept-Encoding: identity",
      "--noproxy", "*", "--resolve", "huggingface.co:443:1.2.3.4", "--range", "1000-"};
  expect(secure_frontend::build_transfer_argv("/usr/bin/curl", request) == expected, "argv is exactly spec §3.3");
  for (const auto& argument : secure_frontend::build_transfer_argv("/usr/bin/curl", request)) {
    expect(argument.find("resolve/abc") == std::string::npos, "no URL in argv");
  }
  secure_frontend::TransferRequest proxied = request;
  proxied.proxy = secure_frontend::ProxySetting{false, "proxy.corp", 3128};
  proxied.pin.reset();
  proxied.range_from.reset();
  const auto with_proxy = secure_frontend::build_transfer_argv("/usr/bin/curl", proxied);
  expect(with_proxy[with_proxy.size() - 2] == "--proxy" && with_proxy.back() == "http://proxy.corp:3128",
         "--proxy replaces --noproxy and no --resolve with a proxy");
}

void transfer_argv_bounded_range() {
  secure_frontend::TransferRequest request;
  request.url = "https://huggingface.co/r/resolve/abc/f.gguf";
  request.host = "huggingface.co";
  request.pin = secure_frontend::make_ipv4(1, 2, 3, 4);
  request.range_from = 1000;
  request.range_to = 1999;
  const auto argv = secure_frontend::build_transfer_argv("/usr/bin/curl", request);
  expect(argv[argv.size() - 2] == "--range" && argv.back() == "1000-1999", "a bounded piece renders lo-hi: " + argv.back());
  secure_frontend::TransferRequest open = request;
  open.range_to.reset();
  const auto open_argv = secure_frontend::build_transfer_argv("/usr/bin/curl", open);
  expect(open_argv.back() == "1000-", "range_from alone stays open-ended");
}

void piece_map_roundtrip_and_resume() {
  // 70 MiB over 32 MiB pieces: 3 pieces, the last short (70 - 64 = 6 MiB).
  const std::uint64_t total = (std::uint64_t{70} << 20U);
  auto map = secure_frontend::piece_map_new(total, "\"abc123\"");
  expect(map.done.size() == 3U && !secure_frontend::all_done(map) && secure_frontend::done_bytes(map) == 0U, "3 pieces, none done");
  expect(secure_frontend::piece_span(map, 2).offset == (std::uint64_t{64} << 20U) &&
             secure_frontend::piece_span(map, 2).length == (std::uint64_t{6} << 20U),
         "the last piece is short");
  map.done[0] = true;
  map.done[2] = true;
  expect(secure_frontend::done_bytes(map) == (std::uint64_t{32} << 20U) + (std::uint64_t{6} << 20U) && !secure_frontend::all_done(map),
         "done_bytes counts full and short pieces; not all done");
  const auto text = secure_frontend::serialize_piece_map(map);
  const auto back = secure_frontend::parse_piece_map(text);
  expect(back && back->total == total && back->piece_bytes == (std::uint64_t{32} << 20U) && back->etag == "\"abc123\"" &&
             back->done == map.done,
         "serialize -> parse round-trips the total, piece size, ETag (quotes and all) and the bitmap");
  map.done[1] = true;
  expect(secure_frontend::all_done(map) && secure_frontend::done_bytes(map) == total, "all pieces done");
  // Malformed sidecars parse to nullopt (a corrupt file restarts from zero).
  expect(!secure_frontend::parse_piece_map("garbage"), "not the magic");
  expect(!secure_frontend::parse_piece_map("STRATA-CHUNKS 1 total=10 piece=0 etag=61 bits=00"), "a zero piece size");
  expect(!secure_frontend::parse_piece_map("STRATA-CHUNKS 1 total=100 piece=32 etag=61 bits=0000"), "a bitmap of the wrong length (4 pieces need 1 byte)");
  expect(!secure_frontend::parse_piece_map("STRATA-CHUNKS 1 total=10 piece=32 etag=6 bits=01"), "an odd-length etag hex");
  expect(!secure_frontend::parse_piece_map("STRATA-CHUNKS 1 total=10 piece=32 etag=61 bits=01 extra=1"), "an unknown field");
  const auto empty = secure_frontend::parse_piece_map("STRATA-CHUNKS 1 total=0 piece=32 etag= bits=");
  expect(empty && empty->done.empty(), "a zero-byte file has no pieces");
}

void config_line_is_single_quoted_url() {
  secure_frontend::TransferRequest request;
  request.url = "https://huggingface.co/a/b.gguf";
  expect(secure_frontend::build_config_stdin(request) == "url = \"https://huggingface.co/a/b.gguf\"\n", "one config line");
}

void probe_argv_is_version_only() {
  expect((secure_frontend::build_probe_argv("C:\\Windows\\System32\\curl.exe") ==
          std::vector<std::string>{"C:\\Windows\\System32\\curl.exe", "-q", "--version"}),
         "probe argv is exactly <curl> -q --version");
}

void probe_output_parsing_and_floor() {
  using secure_frontend::DownloaderPlatform;
  const auto ubuntu = secure_frontend::parse_probe_output(
      "curl 7.81.0 (x86_64-pc-linux-gnu) libcurl/7.81.0 OpenSSL/3.0.2 zlib/1.2.11 brotli/1.0.9 zstd/1.4.8\n"
      "Release-Date: 2022-01-05\nProtocols: dict file ftp ftps http https\n");
  expect(ubuntu.has_value() && ubuntu->major == 7 && ubuntu->minor == 81 && ubuntu->patch == 0, "version parsed");
  expect(ubuntu.has_value() && ubuntu->tls_backend == "OpenSSL", "TLS backend parsed");
  expect(ubuntu && secure_frontend::meets_version_floor(*ubuntu, DownloaderPlatform::linux_like), "7.81.0 meets the Linux floor");
  expect(ubuntu && !secure_frontend::meets_version_floor(*ubuntu, DownloaderPlatform::windows), "7.81.0 is below the Windows floor");
  const auto windows = secure_frontend::parse_probe_output("curl 8.4.0 (Windows) libcurl/8.4.0 Schannel WinIDN\r\n");
  expect(windows.has_value() && windows->tls_backend == "Schannel", "Schannel parsed");
  expect(windows && secure_frontend::meets_version_floor(*windows, DownloaderPlatform::windows), "8.4.0 meets the Windows floor");
  const auto old = secure_frontend::parse_probe_output("curl 7.80.0 (x86_64-pc-linux-gnu) libcurl/7.80.0 OpenSSL/1.1.1\n");
  expect(old && !secure_frontend::meets_version_floor(*old, DownloaderPlatform::linux_like), "7.80.0 below the Linux floor");
  expect(!secure_frontend::parse_probe_output("wget 1.21\n").has_value(), "not curl");
  expect(!secure_frontend::parse_probe_output("curl 8.5.0 (x) libcurl/8.5.0 zlib/1.3\n").has_value(), "no TLS backend");
  const auto dev = secure_frontend::parse_probe_output("curl 8.11.1-DEV (x) libcurl/8.11.1-DEV rustls-ffi/0.14.0\n");
  expect(dev.has_value() && dev->minor == 11 && dev->tls_backend == "rustls-ffi", "suffixes tolerated");
}

void curl_exit_codes_map_to_reasons() {
  using R = secure_frontend::AcquireReason;
  const auto map = [](const int code) { return secure_frontend::map_curl_exit(code, false); };
  expect(!map(0).has_value(), "0 is decided by the status table");
  expect(map(5) == R::dns_failed && map(6) == R::dns_failed, "5, 6");
  expect(map(7) == R::connect_failed, "7");
  for (const int code : {18, 52, 55, 56, 92}) expect(map(code) == R::size_short, "size_short " + std::to_string(code));
  expect(map(28) == R::stalled, "28 stalled");
  expect(secure_frontend::map_curl_exit(28, true) == R::time_budget_exceeded, "28 with the run budget used up");
  expect(map(35) == R::tls_failed, "35");
  for (const int code : {60, 58, 59, 77, 83, 90, 91}) expect(map(code) == R::tls_untrusted, "tls_untrusted " + std::to_string(code));
  for (const int code : {1, 2, 3, 4, 43, 48}) expect(map(code) == R::downloader_unsupported, "unsupported " + std::to_string(code));
  for (const int code : {8, 22, 47, 99}) expect(map(code) == R::transport_failed, "other " + std::to_string(code));
}

using secure_frontend::HeadAction;
using secure_frontend::ResponseHeadParser;

secure_frontend::ResponseHead head_of(const std::string_view bytes) {
  ResponseHeadParser parser;
  const auto fed = parser.feed(bytes);
  expect(fed.state == ResponseHeadParser::State::head_ready, "head parses");
  return parser.head();
}

void informational_header_blocks_skipped() {
  ResponseHeadParser parser;
  const std::string bytes =
      "HTTP/1.1 100 Continue\r\n\r\nHTTP/2 103\r\nlink: </x>; rel=preload\r\n\r\n"
      "HTTP/2 200 \r\ncontent-length: 5\r\nX-Linked-Etag: \"abc\"\r\n\r\nhello";
  const auto fed = parser.feed(bytes);
  expect(fed.state == ResponseHeadParser::State::head_ready, "1xx blocks skipped, final head ready");
  expect(parser.head().status == 200, "final status");
  expect(bytes.substr(fed.consumed) == "hello", "consumed stops exactly at the body");
  expect(secure_frontend::header_values(parser.head(), "x-linked-etag").size() == 1, "names lowercased");
  ResponseHeadParser too_many;
  std::string five;
  for (int i = 0; i < 5; ++i) five += "HTTP/1.1 103 Early Hints\r\n\r\n";
  expect(too_many.feed(five + "HTTP/1.1 200 OK\r\n\r\n").state == ResponseHeadParser::State::malformed,
         "a fifth 1xx block is response_malformed");
}

void header_block_limits_enforced() {
  ResponseHeadParser big;
  expect(big.feed("HTTP/1.1 200 OK\r\nx: " + std::string(16400, 'a') + "\r\n\r\n").state ==
             ResponseHeadParser::State::malformed,
         "block over 16 KiB");
  std::string lines = "HTTP/1.1 200 OK\r\n";
  for (int i = 0; i < 64; ++i) lines += "x" + std::to_string(i) + ": v\r\n";
  ResponseHeadParser many;
  expect(many.feed(lines + "\r\n").state == ResponseHeadParser::State::malformed, "65 lines");
  for (const auto* bad : {"HTTP/1.1 20 OK\r\n\r\n", "HTTP/3 200\r\n\r\n", "ICY 200 OK\r\n\r\n",
                          "HTTP/1.1 200 OK\r\n folded: x\r\n\r\n", "HTTP/1.1 200 OK\r\nno colon\r\n\r\n",
                          "HTTP/1.1 200 OK\r\nbad name: x\r\n\r\n"}) {
    ResponseHeadParser parser;
    expect(parser.feed(bad).state == ResponseHeadParser::State::malformed, std::string{"malformed head: "} + bad);
  }
  // Bare LF never terminates a block: it may stay need_more, but is never a ready head.
  ResponseHeadParser bare_lf;
  expect(bare_lf.feed("HTTP/1.1 200 OK\nx: y\n\n").state != ResponseHeadParser::State::head_ready, "bare LF block");
}

void split_feed_reports_exact_body_offset() {
  const std::string bytes = "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 4-9/10\r\n\r\nbody!!";
  for (std::size_t split = 1; split < bytes.size(); ++split) {
    ResponseHeadParser parser;
    const auto first = parser.feed(std::string_view{bytes}.substr(0, split));
    std::size_t header_bytes = first.consumed;
    if (first.state == ResponseHeadParser::State::need_more) {
      const auto second = parser.feed(std::string_view{bytes}.substr(split));
      expect(second.state == ResponseHeadParser::State::head_ready, "ready after second feed");
      header_bytes += second.consumed;
    }
    expect(bytes.substr(header_bytes) == "body!!", "split at " + std::to_string(split));
  }
}

void status_table_decisions() {
  const secure_frontend::HopExpectation fresh{10, std::nullopt};
  const secure_frontend::HopExpectation resume{10, 4};
  expect(secure_frontend::classify_head(head_of("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n"), fresh).action ==
             HeadAction::write_body, "200 without range");
  const auto announced = secure_frontend::classify_head(head_of("HTTP/1.1 200 OK\r\nContent-Length: 11\r\n\r\n"), fresh);
  expect(announced.action == HeadAction::fail && announced.reason == secure_frontend::AcquireReason::size_mismatch_announced,
         "Content-Length must equal the remaining size");
  expect(secure_frontend::classify_head(head_of("HTTP/1.1 200 OK\r\n\r\n"), resume).action == HeadAction::restart_from_zero,
         "200 to a range request restarts from byte 0");
  expect(secure_frontend::classify_head(head_of("HTTP/1.1 206 X\r\nContent-Range: bytes 4-9/10\r\n\r\n"), resume).action ==
             HeadAction::write_body, "exact Content-Range");
  for (const auto* range : {"bytes 0-9/10", "bytes 4-8/10", "bytes 4-9/11", "bytes 4-9/*", "bytes  4-9/10"}) {
    const auto decision = secure_frontend::classify_head(
        head_of(std::string{"HTTP/1.1 206 X\r\nContent-Range: "} + range + "\r\n\r\n"), resume);
    expect(decision.reason == secure_frontend::AcquireReason::range_mismatch, std::string{"range_mismatch: "} + range);
  }
  expect(secure_frontend::classify_head(head_of("HTTP/1.1 206 X\r\nContent-Range: bytes 0-9/10\r\n\r\n"), fresh).reason ==
             secure_frontend::AcquireReason::range_mismatch, "206 without a range request");
  const auto redirect = secure_frontend::classify_head(head_of("HTTP/1.1 302 Found\r\nLocation: /x\r\n\r\n"), fresh);
  expect(redirect.action == HeadAction::redirect && redirect.location == "/x", "302 with one Location");
  expect(secure_frontend::classify_head(head_of("HTTP/1.1 307 X\r\nLocation: /a\r\nLocation: /b\r\n\r\n"), fresh).reason ==
             secure_frontend::AcquireReason::response_malformed, "two Location headers");
  expect(secure_frontend::classify_head(head_of("HTTP/1.1 416 X\r\n\r\n"), resume).reason ==
             secure_frontend::AcquireReason::range_mismatch, "416");
  const auto busy = secure_frontend::classify_head(head_of("HTTP/1.1 503 X\r\n\r\n"), fresh);
  expect(busy.reason == secure_frontend::AcquireReason::http_status && busy.transient && busy.status == 503, "5xx transient");
  expect(secure_frontend::classify_head(head_of("HTTP/1.1 429 X\r\n\r\n"), fresh).transient, "429 transient");
  const auto missing = secure_frontend::classify_head(head_of("HTTP/1.1 404 X\r\n\r\n"), fresh);
  expect(missing.reason == secure_frontend::AcquireReason::http_status && !missing.transient && missing.status == 404,
         "404 is http_status, next source");
}

using namespace std::chrono_literals;
using Kind = secure_frontend::SourceSchedule::Step::Kind;

void retry_backoff_follows_schedule() {
  secure_frontend::SourceSchedule schedule(2);
  const std::vector<std::chrono::seconds> waits{2s, 4s, 8s, 16s};
  for (const auto wait : waits) {
    const auto step = schedule.record_failure(secure_frontend::AcquireReason::connect_failed, true);
    expect(step.kind == Kind::retry_same_source && step.wait == wait && step.source == 0,
           "retry after " + std::to_string(wait.count()) + " s");
  }
  expect(schedule.attempt() == 5, "fifth attempt on source 0");
  const auto move = schedule.record_failure(secure_frontend::AcquireReason::connect_failed, true);
  expect(move.kind == Kind::next_source && move.wait == 32s && move.source == 1, "after 5 attempts: 32 s, next source");
  expect(schedule.attempt() == 1, "attempts restart on the new source");
}

void non_transient_moves_to_next_source() {
  secure_frontend::SourceSchedule schedule(3);
  const auto step = schedule.record_failure(secure_frontend::AcquireReason::tls_untrusted, false);
  expect(step.kind == Kind::next_source && step.wait == 0s && step.source == 1, "tls_untrusted is not retried");
  expect(secure_frontend::is_transient(secure_frontend::AcquireReason::stalled), "stalled retried");
  expect(secure_frontend::is_transient(secure_frontend::AcquireReason::range_mismatch), "range_mismatch restarts on same source");
  expect(!secure_frontend::is_transient(secure_frontend::AcquireReason::transport_failed), "unknown curl exit: next source");
  expect(!secure_frontend::is_transient(secure_frontend::AcquireReason::http_status), "http_status needs the head's transient flag");
}

void exhausted_class_follows_last_source_reason() {
  secure_frontend::SourceSchedule schedule(2);
  (void)schedule.record_failure(secure_frontend::AcquireReason::tls_untrusted, false);
  const auto last = schedule.record_failure(secure_frontend::AcquireReason::digest_mismatch, false);
  expect(last.kind == Kind::exhausted && last.wait == 0s, "no more sources");
  expect(schedule.last_reason() == secure_frontend::AcquireReason::digest_mismatch, "last reason kept");
  expect(secure_frontend::exit_class(*schedule.last_reason()) == secure_frontend::ExitCode::verification,
         "class follows the last source: 4");
}

void run_budget_remaining_seconds() {
  const auto start = std::chrono::steady_clock::time_point{} + 1000s;
  const secure_frontend::RunBudget budget(start, 60s);
  expect(budget.remaining_seconds(start) == 60U, "full budget");
  expect(budget.remaining_seconds(start + 59500ms) == 1U, "rounded up to whole seconds");
  expect(!budget.remaining_seconds(start + 60s).has_value(), "used up at the deadline");
}

void staging_cwd_not_treated_as_key() {
  expect(!secure_frontend::is_staging_key_name(".cwd"), "the downloader's cwd is never a staging key");
  expect(secure_frontend::is_staging_key_name(std::string(32, 'f')), "32 lowercase hex");
  expect(!secure_frontend::is_staging_key_name(std::string(32, 'F')), "uppercase");
  expect(!secure_frontend::is_staging_key_name(std::string(31, 'f')), "31 characters");
}

void landlock_rights_per_abi() {
  namespace la = secure_frontend::landlock_access;
  const auto abi1 = secure_frontend::landlock_rights_for_abi(1);
  expect(abi1.handled_fs == 0x1FFFU, "ABI 1 handles bits 0-12");
  expect(abi1.handled_net == 0, "no network rights before ABI 4");
  expect(abi1.directory_rule == (la::kFsExecute | la::kFsReadFile | la::kFsReadDir), "read and execute below system dirs");
  expect(abi1.file_rule == la::kFsReadFile, "READ_FILE on /dev/null and /dev/urandom");
  expect(secure_frontend::landlock_rights_for_abi(2).handled_fs == 0x3FFFU, "ABI 2 adds REFER");
  expect(secure_frontend::landlock_rights_for_abi(3).handled_fs == 0x7FFFU, "ABI 3 adds TRUNCATE");
  const auto abi4 = secure_frontend::landlock_rights_for_abi(4);
  expect(abi4.handled_fs == 0x7FFFU && abi4.handled_net == (la::kNetBindTcp | la::kNetConnectTcp), "ABI 4 adds TCP rights");
  expect(secure_frontend::landlock_rights_for_abi(5).handled_fs == 0xFFFFU, "ABI 5 adds IOCTL_DEV");
  expect(secure_frontend::landlock_rights_for_abi(7).handled_fs == 0xFFFFU, "later ABIs use the ABI 5 set (note 14)");
  expect((abi1.directory_rule & la::kFsWriteFile) == 0 && (abi4.directory_rule & la::kFsMakeReg) == 0, "no write right anywhere");
}

void confinement_plan_detects_readable_roots() {
  const std::set<std::string> existing{"/usr", "/lib", "/bin", "/etc", "/home/u/tools"};
  const auto exists = [&](const std::string_view d) { return existing.contains(std::string{d}); };
  const std::vector<secure_frontend::ProtectedPath> clean{
      {"config file", "/home/u/.config/strata/config.json"}, {"roots.models", "/home/u/.local/share/strata/models"}};
  const auto plan = secure_frontend::plan_linux_confinement("/usr/bin/curl", clean, exists);
  expect((plan.read_dirs == std::vector<std::string>{"/usr", "/lib", "/bin", "/etc"}), "existing system dirs; /usr/bin covered by /usr");
  expect(!plan.readable_key.has_value(), "nothing protected is readable");
  const std::vector<secure_frontend::ProtectedPath> etc_config{{"config file", "/etc/strata/config.json"}};
  expect(secure_frontend::plan_linux_confinement("/usr/bin/curl", etc_config, exists).readable_key == "config file",
         "a config under /etc is readable");
  const std::vector<secure_frontend::ProtectedPath> home_root{{"roots.models", "/home/u/tools/models"}};
  const auto home = secure_frontend::plan_linux_confinement("/home/u/tools/curl", home_root, exists);
  expect(home.readable_key == "roots.models", "a downloader parent covering a root reduces confinement");
  expect(home.read_dirs.back() == "/home/u/tools", "the downloader's parent is read-allowed");
  expect(!secure_frontend::path_is_beneath("/usr", "/usrlocal/x") && secure_frontend::path_is_beneath("/usr", "/usr"),
         "separator-boundary prefix test");
}

void confinement_label_texts() {
  expect(secure_frontend::confinement_label(true, std::nullopt) == "full", "full");
  expect(secure_frontend::confinement_label(false, std::nullopt) == "reduced:no_landlock", "no Landlock");
  expect(secure_frontend::confinement_label(true, std::string{"roots.secret"}) == "reduced:readable:roots.secret", "readable");
  expect(secure_frontend::confinement_label(false, std::string{"roots.secret"}) == "reduced:no_landlock",
         "missing Landlock is the stronger cause");
}

void crt_fd_block_layout() {
  namespace cf = secure_frontend::crt_flags;
  const std::array<std::uint64_t, 5> handles{0x11, 0x22, 0x33, secure_frontend::kInvalidHandleBits, 0x55};
  const std::array<std::uint8_t, 5> flags{cf::kOpen | cf::kPipe, cf::kOpen | cf::kPipe, cf::kOpen | cf::kDevice, 0, cf::kOpen};
  const auto block = secure_frontend::build_crt_fd_block(handles, flags, 8);
  expect(block.size() == 49, "4 + 5 * (1 + 8) = 49 bytes on x64");
  expect(block[0] == 5 && block[1] == 0 && block[2] == 0 && block[3] == 0, "little-endian int count n = 5");
  expect(block[4] == 0x09 && block[5] == 0x09 && block[6] == 0x41 && block[7] == 0 && block[8] == 0x01, "flag bytes, packed");
  expect(block[9] == 0x11 && block[17] == 0x22 && block[25] == 0x33, "handles follow the flags, no padding");
  bool all_ff = true;
  for (std::size_t i = 33; i < 41; ++i) all_ff = all_ff && block[i] == 0xFF;
  expect(all_ff, "entry 3 is INVALID_HANDLE_VALUE");
  expect(block[41] == 0x55, "entry 4 is the record handle");
  expect(secure_frontend::build_crt_fd_block(handles, flags, 4).size() == 29, "x86 layout: 4 + 5 * 5");
}


std::filesystem::path built_fake() { return secure_frontend_test::extra_arguments().at(0); }

const char* kScenario =
    "probe 8.4.0 OpenSSL/3.0.13\n"
    "response https://fixture.example/f.bin\n"
    "status 103 Early Hints\n"
    "status 200 OK\n"
    "header Content-Length: 5\n"
    "body-file f.bin\n"
    "end\n"
    "response https://fixture.example/f.bin range 2\n"
    "status 206 Partial Content\n"
    "header Content-Range: bytes 2-4/5\n"
    "body-text llo\n"
    "end\n";

void fake_curl_scenario_parses_and_renders() {
  const auto scenario = secure_frontend_test::curl::parse_scenario(kScenario);
  expect(scenario.has_value() && scenario->responses.size() == 2, "two responses");
  if (!scenario) return;
  const auto* first = secure_frontend_test::curl::select(*scenario, "https://fixture.example/f.bin", std::nullopt);
  expect(first != nullptr && secure_frontend_test::curl::render_head(*first) ==
                                 "HTTP/1.1 103 Early Hints\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n",
         "head render");
  const auto* resumed = secure_frontend_test::curl::select(*scenario, "https://fixture.example/f.bin", 2);
  expect(resumed != nullptr && secure_frontend_test::curl::render_body(*resumed, "") == "llo", "selected by range");
  expect(secure_frontend_test::curl::select(*scenario, "https://fixture.example/f.bin", 3) == nullptr, "no match");
  expect(!secure_frontend_test::curl::parse_scenario("response x\nstatus\nend\n").has_value(), "bad status line");
  // Task 27: delay-ms <N> (the fake sleeps after the head, before the body).
  const auto delayed = secure_frontend_test::curl::parse_scenario("response x\nstatus 200 OK\ndelay-ms 250\nend\n");
  expect(delayed.has_value() && delayed->responses.size() == 1 && delayed->responses[0].delay_ms == 250, "delay-ms parsed");
  expect(!secure_frontend_test::curl::parse_scenario("response x\ndelay-ms soon\nend\n").has_value(), "delay-ms needs a number");
}

void fake_curl_argv_grammar_matches_builder() {
  secure_frontend::TransferRequest request;
  request.url = "https://fixture.example/f.bin";
  request.host = "fixture.example";
  request.pin = secure_frontend::make_ipv4(93, 184, 216, 34);
  request.range_from = 2;
  auto argv = secure_frontend::build_transfer_argv("/x/curl", request);
  const std::vector<std::string> args(argv.begin() + 1, argv.end());
  const auto parsed = secure_frontend_test::curl::parse_curl_argv(args);
  expect(parsed && !parsed->probe && parsed->range == 2U && parsed->resolve == "fixture.example:443:93.184.216.34",
         "the fake accepts exactly what the builder emits");
  auto with_l = args;
  with_l.insert(with_l.begin(), "-L");
  expect(!secure_frontend_test::curl::parse_curl_argv(with_l).has_value(), "-L is outside the grammar");
  const std::vector<std::string> probe{"-q", "--version"};
  const auto parsed_probe = secure_frontend_test::curl::parse_curl_argv(probe);
  expect(parsed_probe && parsed_probe->probe, "probe argv");
  expect(secure_frontend_test::curl::parse_config_line("url = \"https://fixture.example/f.bin\"\n") == "https://fixture.example/f.bin",
         "config line");
  expect(!secure_frontend_test::curl::parse_config_line("url = \"a\"\nurl = \"b\"\n").has_value(), "exactly one line");
}

#if !defined(_WIN32)
void fake_curl_replays_scenario_and_records() {
  const auto scratch = secure_frontend_test::make_scratch_directory();
  const auto fake = secure_frontend_test::install_fake_curl(scratch, built_fake(), kScenario, {{"f.bin", "hello"}});
  const auto record_path = scratch / "downloader-record.log";
  const int record = ::open(record_path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  expect(record >= 0, "record file created by the parent");
  secure_frontend::TransferRequest request;
  request.url = "https://fixture.example/f.bin";
  request.host = "fixture.example";
  const auto result = secure_frontend_test::spawn_with_fd4(secure_frontend::build_transfer_argv(fake.executable.string(), request),
                                                           secure_frontend::build_config_stdin(request), record);
  ::close(record);
  expect(result.exit_code == 0, "exit 0");
  expect(result.stdout_bytes == "HTTP/1.1 103 Early Hints\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello", "replayed bytes");
  const auto lines = secure_frontend_test::read_record_lines(record_path);
  expect(lines.size() == 1 && lines[0].find("\"kind\":\"transfer\"") != std::string::npos, "one transfer record");
  expect(!lines.empty() && lines[0].find("\"env\":[]") != std::string::npos, "empty environment recorded");
  expect(!lines.empty() && lines[0].find("url = \\\"https://fixture.example/f.bin\\\"\\n") != std::string::npos, "stdin recorded");
}

void fake_curl_rejects_argv_outside_grammar() {
  const auto scratch = secure_frontend_test::make_scratch_directory();
  const auto fake = secure_frontend_test::install_fake_curl(scratch, built_fake(), kScenario);
  const auto record_path = scratch / "downloader-record.log";
  const int record = ::open(record_path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  // 1 MiB the child never reads: the parent must see EPIPE, not die of SIGPIPE.
  const std::string unread(std::size_t{1} << 20U, 'x');
  const auto result = secure_frontend_test::spawn_with_fd4({fake.executable.string(), "-L", "https://x.example/"}, unread, record);
  ::close(record);
  expect(result.exit_code == 98, "exit 98 outside the grammar");
  const auto lines = secure_frontend_test::read_record_lines(record_path);
  expect(lines.size() == 1 && lines[0].find("\"kind\":\"invalid\"") != std::string::npos, "invalid recorded");
}

void fake_curl_answers_probe() {
  const auto scratch = secure_frontend_test::make_scratch_directory();
  const auto fake = secure_frontend_test::install_fake_curl(scratch, built_fake(), "probe 7.80.0 GnuTLS/3.7.3\n");
  const auto result = secure_frontend_test::spawn_with_fd4(secure_frontend::build_probe_argv(fake.executable.string()), "", -1);
  expect(result.exit_code == 0 && result.stdout_bytes == "curl 7.80.0 (fake) libcurl/7.80.0 GnuTLS/3.7.3\n", "probe text");
}
#else
void fake_curl_records_on_crt_entry_4_windows() {
  const auto scratch = secure_frontend_test::make_scratch_directory();
  const auto fake = secure_frontend_test::install_fake_curl(scratch, built_fake(), kScenario, {{"f.bin", "hello"}});
  const auto record_path = scratch / "downloader-record.log";
  SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
  HANDLE record = CreateFileW(record_path.wstring().c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, &inherit, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  expect(record != INVALID_HANDLE_VALUE, "record file created by the parent");
  secure_frontend::TransferRequest request;
  request.url = "https://fixture.example/f.bin";
  request.host = "fixture.example";
  const auto result = secure_frontend_test::spawn_with_crt_entry4(
      secure_frontend::build_transfer_argv(fake.executable.string(), request), secure_frontend::build_config_stdin(request), record);
  CloseHandle(record);
  expect(result.exit_code == 0, "exit 0");
  const auto lines = secure_frontend_test::read_record_lines(record_path);
  expect(lines.size() == 1 && lines[0].find("\"kind\":\"transfer\"") != std::string::npos, "record written to CRT entry 4");
}
#endif

}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"dl03_location_with_glob_characters_rejected", dl03_location_with_glob_characters_rejected},
      {"location_resolution_and_host_normalization", location_resolution_and_host_normalization},
      {"location_query_characters", location_query_characters},
      {"address_policy_denies_special_ranges", address_policy_denies_special_ranges},
      {"address_policy_any_denied_answer_rejects_hop", address_policy_any_denied_answer_rejects_hop},
      {"ipv6_only_answer_fails_closed", ipv6_only_answer_fails_closed},
      {"allow_private_addresses_pins_first_ipv4", allow_private_addresses_pins_first_ipv4},
      {"resolve_argument_brackets_ipv6", resolve_argument_brackets_ipv6},
      {"transfer_argv_exact_order", transfer_argv_exact_order},
      {"transfer_argv_bounded_range", transfer_argv_bounded_range},
      {"piece_map_roundtrip_and_resume", piece_map_roundtrip_and_resume},
      {"config_line_is_single_quoted_url", config_line_is_single_quoted_url},
      {"probe_argv_is_version_only", probe_argv_is_version_only},
      {"probe_output_parsing_and_floor", probe_output_parsing_and_floor},
      {"curl_exit_codes_map_to_reasons", curl_exit_codes_map_to_reasons},
      {"informational_header_blocks_skipped", informational_header_blocks_skipped},
      {"header_block_limits_enforced", header_block_limits_enforced},
      {"split_feed_reports_exact_body_offset", split_feed_reports_exact_body_offset},
      {"status_table_decisions", status_table_decisions},
      {"retry_backoff_follows_schedule", retry_backoff_follows_schedule},
      {"non_transient_moves_to_next_source", non_transient_moves_to_next_source},
      {"exhausted_class_follows_last_source_reason", exhausted_class_follows_last_source_reason},
      {"run_budget_remaining_seconds", run_budget_remaining_seconds},
      {"staging_cwd_not_treated_as_key", staging_cwd_not_treated_as_key},
      {"landlock_rights_per_abi", landlock_rights_per_abi},
      {"confinement_plan_detects_readable_roots", confinement_plan_detects_readable_roots},
      {"confinement_label_texts", confinement_label_texts},
      {"crt_fd_block_layout", crt_fd_block_layout},
      {"fake_curl_scenario_parses_and_renders", fake_curl_scenario_parses_and_renders},
      {"fake_curl_argv_grammar_matches_builder", fake_curl_argv_grammar_matches_builder},
#if !defined(_WIN32)
      {"fake_curl_replays_scenario_and_records", fake_curl_replays_scenario_and_records},
      {"fake_curl_rejects_argv_outside_grammar", fake_curl_rejects_argv_outside_grammar},
      {"fake_curl_answers_probe", fake_curl_answers_probe},
#else
      {"fake_curl_records_on_crt_entry_4_windows", fake_curl_records_on_crt_entry_4_windows},
#endif
      {"dl03_truncated_body_rejected", fetch_cases::dl03_truncated_body_rejected},
      {"dl_parallel_assembles_pieces", fetch_cases::dl_parallel_assembles_pieces},
      {"dl_parallel_falls_back_without_ranges", fetch_cases::dl_parallel_falls_back_without_ranges},
      {"dl_parallel_resumes_from_sidecar", fetch_cases::dl_parallel_resumes_from_sidecar},
      {"a_window_of_its_source_is_fetched_by_its_range", fetch_cases::a_window_of_its_source_is_fetched_by_its_range},
      {"dl_parallel_fetches_a_window", fetch_cases::dl_parallel_fetches_a_window},
      {"dl03_oversize_body_rejected_mid_stream", fetch_cases::dl03_oversize_body_rejected_mid_stream},
      {"dl03_redirect_to_http_rejected", fetch_cases::dl03_redirect_to_http_rejected},
      {"dl03_redirect_off_allowlist_rejected", fetch_cases::dl03_redirect_off_allowlist_rejected},
      {"dl03_redirect_limit", fetch_cases::dl03_redirect_limit},
      {"redirect_query_only_in_config_line", fetch_cases::redirect_query_only_in_config_line},
      {"dl03_private_address_rejected", fetch_cases::dl03_private_address_rejected},
      {"dl03_mixed_public_private_answer_rejected", fetch_cases::dl03_mixed_public_private_answer_rejected},
      {"dl03_range_ignored_restarts_from_zero", fetch_cases::dl03_range_ignored_restarts_from_zero},
      {"dl03_content_range_mismatch_rejected", fetch_cases::dl03_content_range_mismatch_rejected},
      {"dl03_content_encoding_gzip_fails_digest", fetch_cases::dl03_content_encoding_gzip_fails_digest},
      {"dl03_stalled_source_times_out", fetch_cases::dl03_stalled_source_times_out},
      {"time_budget_exceeded_is_resumable", fetch_cases::time_budget_exceeded_is_resumable},
      {"tls_untrusted_moves_to_next_source", fetch_cases::tls_untrusted_moves_to_next_source},
      {"digest_mismatch_after_resume_refetches_once", fetch_cases::digest_mismatch_after_resume_refetches_once},
      {"free_space_rechecked_while_writing", fetch_cases::free_space_rechecked_while_writing},
      {"ipv6_only_hop_is_address_not_allowed", fetch_cases::ipv6_only_hop_is_address_not_allowed},
      {"range_ignored_twice_is_range_mismatch_then_restarts", fetch_cases::range_ignored_twice_is_range_mismatch_then_restarts},
      {"proxy_hop_skips_address_policy", fetch_cases::proxy_hop_skips_address_policy},
      {"dl03_wrong_digest_rejected_prior_version_intact", fetch_cases::dl03_wrong_digest_rejected_prior_version_intact},
      {"confinement_reduced_written_to_log_record", fetch_cases::confinement_reduced_written_to_log_record},
#if !defined(_WIN32)
      {"doctor_downloader_probes_on_fresh_store", fetch_cases::doctor_downloader_probes_on_fresh_store},
#if defined(SECURE_FRONTEND_TEST_HOOKS)
      {"confinement_reduced_when_root_beneath_allowed_dir", fetch_cases::confinement_reduced_when_root_beneath_allowed_dir},
#endif
#endif
#if !defined(_WIN32)
      {"release_spawn_has_no_fd4", fetch_cases::release_spawn_has_no_fd4},
#endif
#if defined(SECURE_FRONTEND_TEST_HOOKS)
      {"test_cacert_reaches_argv_in_hook_builds", fetch_cases::test_cacert_reaches_argv_in_hook_builds},
#endif
#if !defined(_WIN32)
      {"downloader_spawn_report_classified", fetch_cases::downloader_spawn_report_classified},
#if defined(SECURE_FRONTEND_TEST_HOOKS)
      {"priv04_downloader_argv_exact", fetch_cases::priv04_downloader_argv_exact},
      {"priv04_downloader_env_is_empty", fetch_cases::priv04_downloader_env_is_empty},
      {"redirect_url_absent_from_downloader_argv", fetch_cases::redirect_url_absent_from_downloader_argv},
      {"dl03_downloader_untrusted_path_rejected", fetch_cases::dl03_downloader_untrusted_path_rejected},
      {"downloader_below_floor_exits_9", fetch_cases::downloader_below_floor_exits_9},
      {"dl03_downloader_cannot_write_files", fetch_cases::dl03_downloader_cannot_write_files},
      {"dl03_downloader_cannot_read_home", fetch_cases::dl03_downloader_cannot_read_home},
      {"pre_exec_failure_fails_closed_exit_9", fetch_cases::pre_exec_failure_fails_closed_exit_9},
      {"probe_interrupted_exits_8", fetch_cases::probe_interrupted_exits_8},
      {"ruleset_failure_fails_closed_exit_9", fetch_cases::ruleset_failure_fails_closed_exit_9},
      {"record_fd_does_not_clobber_report_pipe", fetch_cases::record_fd_does_not_clobber_report_pipe},
      {"confinement_reduced_is_announced_without_landlock", fetch_cases::confinement_reduced_is_announced_without_landlock},
#endif
#endif
#if defined(_WIN32)
      {"downloader_transfer_through_job_windows", fetch_cases::downloader_transfer_through_job_windows},
      {"dl03_default_windows_curl_path_accepted_windows", fetch_cases::dl03_default_windows_curl_path_accepted_windows},
      {"dl03_downloader_path_normalised_windows", fetch_cases::dl03_downloader_path_normalised_windows},
      {"dl03_downloader_unc_path_rejected_windows", fetch_cases::dl03_downloader_unc_path_rejected_windows},
#if defined(SECURE_FRONTEND_TEST_HOOKS)
      {"dl03_downloader_runs_at_low_integrity_windows", fetch_cases::dl03_downloader_runs_at_low_integrity_windows},
      {"priv04_downloader_env_is_systemroot_only_windows", fetch_cases::priv04_downloader_env_is_systemroot_only_windows},
#endif
#endif
  }, secure_frontend_test::failures);
}
