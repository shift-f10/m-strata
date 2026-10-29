#include "secure_frontend/acquire_manifest.hpp"
#include "secure_frontend/acquire_url.hpp"
#include "secure_frontend/catalog.hpp"
#include "secure_frontend/engine_version.hpp"
#include "secure_frontend/manifest_json.hpp"
#include "secure_frontend/manifest_trust.hpp"
#include "secure_frontend/sha256.hpp"
#include "secure_frontend/test_hooks.hpp"
#include "support/test_support.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

int failures = 0;

void expect(const bool condition, const std::string& message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
  }
}

bool problem_is(const std::variant<secure_frontend::HttpsUrl, secure_frontend::UrlProblem>& result,
                const secure_frontend::UrlProblem expected) {
  const auto* problem = std::get_if<secure_frontend::UrlProblem>(&result);
  return problem != nullptr && *problem == expected;
}

void source_url_grammar_matrix() {
  using P = secure_frontend::UrlProblem;
  const auto ok = secure_frontend::parse_source_url(
      "https://huggingface.co/ISTA-DASLab/Qwen3.8-Flash-Next-GSQ-RCO-GGUF/resolve/ed59f92082b1e93c0e96d60a8b11aab089b52f09/"
      "IQ2_XS/Qwen3.8-Flash-Next-GSQ-RCO-IQ2_XS-00001-of-00002.gguf");
  expect(std::holds_alternative<secure_frontend::HttpsUrl>(ok), "Hugging Face resolve URL accepted");
  if (const auto* url = std::get_if<secure_frontend::HttpsUrl>(&ok)) {
    expect(url->host == "huggingface.co", "host split");
    expect(secure_frontend::render_url(*url).starts_with("https://huggingface.co/ISTA-DASLab/"), "round trip");
  }
  expect(std::holds_alternative<secure_frontend::HttpsUrl>(secure_frontend::parse_source_url("https://h.example:443/a%20b")),
         "explicit :443 and %XX accepted");
  expect(problem_is(secure_frontend::parse_source_url("http://huggingface.co/a"), P::not_https), "http rejected");
  expect(problem_is(secure_frontend::parse_source_url("HTTPS://huggingface.co/a"), P::not_https), "scheme is exact");
  expect(problem_is(secure_frontend::parse_source_url("https://huggingface.co:8443/a"), P::port_not_allowed), "port 8443");
  expect(problem_is(secure_frontend::parse_source_url("https://huggingface.co:/a"), P::port_not_allowed), "empty port");
  expect(problem_is(secure_frontend::parse_source_url("https://user@huggingface.co/a"), P::userinfo), "userinfo");
  expect(problem_is(secure_frontend::parse_source_url("https://huggingface.co/a?x=1"), P::query_not_allowed), "query");
  expect(problem_is(secure_frontend::parse_source_url("https://huggingface.co/a#f"), P::fragment), "fragment");
  for (const auto* bad : {"https://h.example/a[1]", "https://h.example/a{b}", "https://h.example/a b",
                          "https://h.example/a\"b", "https://h.example/a\\b", "https://h.example/%zz",
                          "https://h.example/a%2"}) {
    expect(problem_is(secure_frontend::parse_source_url(bad), P::bad_character), std::string{"bad character: "} + bad);
  }
  expect(problem_is(secure_frontend::parse_source_url("https://h.example"), P::missing_path), "no path");
  expect(problem_is(secure_frontend::parse_source_url("https://h.example/"), P::missing_path), "root path only");
  expect(problem_is(secure_frontend::parse_source_url("https://H.example/a"), P::bad_host), "manifest hosts are lowercase");
  expect(problem_is(secure_frontend::parse_source_url("https://127.0.0.1/a"), P::bad_host), "IPv4 literal");
  expect(problem_is(secure_frontend::parse_source_url("https://[::1]/a"), P::bad_host), "IPv6 literal");
  expect(problem_is(secure_frontend::parse_source_url("https://localhost/a"), P::bad_host), "single label");
  expect(problem_is(secure_frontend::parse_source_url("https://h.example/" + std::string(2100, 'a')), P::too_long),
         "longer than 2048 bytes");
}

void dns_names_and_wildcards() {
  expect(secure_frontend::is_valid_dns_name("huggingface.co"), "two labels");
  expect(secure_frontend::is_valid_dns_name("us.aws.cdn.hf.co"), "five labels");
  for (const auto* bad : {"", "co", "-a.co", "a-.co", "a..co", "a.co.", "A.co", "a_b.co", "1.2.3.4", "a.123"}) {
    expect(!secure_frontend::is_valid_dns_name(bad), std::string{"invalid dns name: "} + bad);
  }
  expect(secure_frontend::is_valid_redirect_pattern("*.aws.cdn.hf.co"), "wildcard over 4 labels");
  expect(secure_frontend::is_valid_redirect_pattern("cas-bridge.xethub.hf.co"), "plain host");
  expect(!secure_frontend::is_valid_redirect_pattern("*.co"), "wildcard base needs two labels");
  expect(!secure_frontend::is_valid_redirect_pattern("*"), "bare star");
  expect(!secure_frontend::is_valid_redirect_pattern("a.*.co"), "star only leftmost");
  expect(secure_frontend::host_matches("*.aws.cdn.hf.co", "us.aws.cdn.hf.co"), "one extra label matches");
  expect(!secure_frontend::host_matches("*.aws.cdn.hf.co", "aws.cdn.hf.co"), "zero extra labels do not match");
  expect(!secure_frontend::host_matches("*.aws.cdn.hf.co", "a.b.aws.cdn.hf.co"), "two extra labels do not match");
  expect(!secure_frontend::host_matches("*.aws.cdn.hf.co", "evilaws.cdn.hf.co"), "label boundary");
  expect(secure_frontend::host_matches("huggingface.co", "huggingface.co"), "exact");
  expect(!secure_frontend::host_matches("huggingface.co", "x.huggingface.co"), "exact means exact");
}

void proxy_grammar() {
  const auto http = secure_frontend::parse_proxy("http://proxy.corp:3128");
  expect(std::holds_alternative<secure_frontend::ProxySetting>(http), "http proxy with single-label-free name");
  const auto single = secure_frontend::parse_proxy("http://proxy:8080");
  expect(std::holds_alternative<secure_frontend::ProxySetting>(single), "single-label proxy name");
  expect(std::holds_alternative<secure_frontend::ProxySetting>(secure_frontend::parse_proxy("https://10.0.0.5:443")),
         "IPv4 literal proxy (may be private)");
  expect(std::holds_alternative<secure_frontend::ProxySetting>(secure_frontend::parse_proxy("http://[fd00::1]:3128")),
         "IPv6 literal proxy");
  if (const auto* p = std::get_if<secure_frontend::ProxySetting>(&http)) {
    expect(secure_frontend::render_proxy(*p) == "http://proxy.corp:3128", "render");
  }
  for (const auto* bad : {"proxy:3128", "socks5://p.example:1080", "http://u:p@p.example:3128", "http://p.example",
                          "http://p.example:0", "http://p.example:65536", "http://p.example:3128/path",
                          "http://p.example:03128"}) {
    expect(std::holds_alternative<secure_frontend::UrlProblem>(secure_frontend::parse_proxy(bad)),
           std::string{"proxy rejected: "} + bad);
  }
}

using secure_frontend::AcquireManifest;
using secure_frontend::AcquireReason;
using secure_frontend::FileRole;
using secure_frontend::ManifestProfile;

std::string hex64(const char c) { return std::string(64, c); }

secure_frontend::ManifestFile file(const std::string& path, const FileRole role) {
  return secure_frontend::ManifestFile{path, role, 10, hex64('a'), {"https://fixture.example/" + path}, std::nullopt};
}

AcquireManifest valid_qwen_manifest() {
  AcquireManifest m;
  m.name = "qwen3.8-flash-next";
  m.tag = "iq2_xs";
  m.version = "2026.10.14.1";
  m.sequence = 1;
  m.profile = ManifestProfile::qwen;
  m.family = secure_frontend::ManifestFamily::strata;
  m.min_version = *secure_frontend::parse_engine_version("0.1.43");
  m.license = {"Apache-2.0", "https://fixture.example/license", true, "Notice."};
  m.hosts = {"fixture.example"};
  m.redirect_hosts = {"*.cdn.fixture.example"};
  m.files = {file("weights/m-00001-of-00002.gguf", FileRole::weights), file("weights/m-00002-of-00002.gguf", FileRole::weights),
             file("pack/index.txt", FileRole::pack), file("pack/dense.bin", FileRole::pack),
             file("pack/native_experts.txt", FileRole::pack), file("pack/conversions.json", FileRole::pack),
             file("pack/tokenizer/vocab.json", FileRole::tokenizer), file("pack/tokenizer/merges.txt", FileRole::tokenizer),
             file("pack/tokenizer/token_type.json", FileRole::tokenizer),
             file("pack/tokenizer/tokenizer.json", FileRole::tokenizer)};
  m.launch = {"weights/m-00001-of-00002.gguf", "weights/m-00002-of-00002.gguf", "pack", "pack/tokenizer", std::nullopt};
  return m;
}

secure_frontend::ManifestRules rules() { return secure_frontend::ManifestRules{}; }

void expect_issue(const AcquireManifest& m, const std::string& key, const std::string& why,
                  const AcquireReason reason = AcquireReason::manifest_invalid) {
  const auto issue = secure_frontend::validate_manifest(m, rules());
  expect(issue.has_value() && issue->reason == reason && issue->key_path == key,
         why + " (got " + (issue ? issue->key_path : std::string{"no issue"}) + ")");
}

void valid_qwen_manifest_accepted() {
  const auto issue = secure_frontend::validate_manifest(valid_qwen_manifest(), rules());
  expect(!issue.has_value(), "a complete qwen manifest is valid: " + (issue ? issue->key_path : std::string{}));
}

void manifest_field_rules_matrix() {
  struct Mutation final {
    std::string key;
    std::string why;
    std::function<void(AcquireManifest&)> change;
  };
  const std::vector<Mutation> mutations{
      {"name", "uppercase name", [](auto& m) { m.name = "Qwen"; }},
      {"name", "name starting with '.'", [](auto& m) { m.name = ".hidden"; }},
      {"name", "device name", [](auto& m) { m.name = "con"; }},
      {"name", "device name with extension", [](auto& m) { m.name = "aux.json"; }},
      {"name", "trailing dot", [](auto& m) { m.name = "model."; }},
      {"name", "129 characters", [](auto& m) { m.name = std::string(129, 'a'); }},
      {"tag", "tag default reserved", [](auto& m) { m.tag = "default"; }},
      {"tag", "colon in tag", [](auto& m) { m.tag = "a:b"; }},
      {"version", "bad version label", [](auto& m) { m.version = "2026.13.01.1"; }},
      {"version", "N above 999", [](auto& m) { m.version = "2026.10.14.1000"; }},
      {"sequence", "sequence 0", [](auto& m) { m.sequence = 0; }},
      {"sequence", "sequence above 2^31-1", [](auto& m) { m.sequence = 0x80000000U; }},
      {"engine.family", "family must follow profile", [](auto& m) { m.family = secure_frontend::ManifestFamily::glm; }},
      {"license.id", "license id grammar", [](auto& m) { m.license.id = "Apache 2.0"; }},
      {"license.url", "license url https", [](auto& m) { m.license.url = "http://x.example/l"; }},
      {"license.notice", "notice over 512", [](auto& m) { m.license.notice = std::string(513, 'n'); }},
      {"license.notice", "control byte in notice", [](auto& m) { m.license.notice = "a\x1b[31mb"; }},
      {"hosts", "no hosts", [](auto& m) { m.hosts.clear(); }},
      {"hosts[0]", "IP literal host", [](auto& m) { m.hosts = {"1.2.3.4"}; }},
      {"hosts", "17 hosts", [](auto& m) { m.hosts.assign(17, "a.example"); }},
      {"redirect_hosts[0]", "single-label wildcard base", [](auto& m) { m.redirect_hosts = {"*.co"}; }},
      {"files", "no files", [](auto& m) { m.files.clear(); }},
      {"files[0].path", "dot-dot", [](auto& m) { m.files[0].path = "weights/../x"; }},
      {"files[0].path", "absolute", [](auto& m) { m.files[0].path = "/etc/passwd"; }},
      {"files[0].path", "backslash", [](auto& m) { m.files[0].path = "weights\\x"; }},
      {"files[0].path", "hidden component", [](auto& m) { m.files[0].path = "weights/.x"; }},
      {"files[0].path", "control byte", [](auto& m) { m.files[0].path = "weights/a\nb"; }},
      {"files[0].path", "P1: reserved device component", [](auto& m) { m.files[0].path = "weights/con.txt"; }},
      {"files[0].path", "P1: device name, mixed case, with extension", [](auto& m) { m.files[0].path = "weights/Lpt1.gguf"; }},
      {"files[0].path", "P1: trailing dot", [](auto& m) { m.files[0].path = "weights/x."; }},
      {"files[0].path", "P1: trailing space", [](auto& m) { m.files[0].path = "weights/x "; }},
      {"files[0].path", "P1: 8.3 short-name alias", [](auto& m) { m.files[0].path = "weights/PROGRA~1"; }},
      {"files[0].path", "P1: reserved character", [](auto& m) { m.files[0].path = "weights/a*b"; }},
      {"files[0].path", "P1: component over 255 bytes", [](auto& m) { m.files[0].path = "weights/" + std::string(256, 'a'); }},
      {"files[1].path", "case-fold collision", [](auto& m) { m.files[1].path = "WEIGHTS/M-00001-OF-00002.GGUF"; }},
      {"files[0].size", "size 0", [](auto& m) { m.files[0].size = 0; }},
      {"files[0].size", "size above 2^40", [](auto& m) { m.files[0].size = (std::uint64_t{1} << 40U) + 1U; }},
      {"files[0].sha256", "uppercase hex", [](auto& m) { m.files[0].sha256 = std::string(64, 'A'); }},
      {"files[0].sha256", "short hex", [](auto& m) { m.files[0].sha256 = std::string(63, 'a'); }},
      {"files[0].sources[0]", "http source", [](auto& m) { m.files[0].sources = {"http://fixture.example/a"}; }},
      {"files[0].sources[0]", "[ in source URL", [](auto& m) { m.files[0].sources = {"https://fixture.example/a[1]"}; }},
      {"files[0].sources[0]", "source host not in hosts", [](auto& m) { m.files[0].sources = {"https://other.example/a"}; }},
      // owner 2026-10-29: a window of its source (Qwen3.8's MTP tensors) has a source and lies inside 2^62
      {"files[0].source_offset", "a window with no source", [](auto& m) {
         m.files[0].source_offset = 5U;
         m.files[0].sources.clear();
       }},
      {"files[0].source_offset", "a window past 2^62", [](auto& m) { m.files[0].source_offset = std::uint64_t{1} << 62U; }},
      {"files[0].sources", "9 sources", [](auto& m) { m.files[0].sources.assign(9, "https://fixture.example/a"); }},
      {"files[0].license.url", "file license validated",
       [](auto& m) { m.files[0].license = secure_frontend::ManifestLicense{"MIT", "ftp://x", true, ""}; }},
      {"files", "incomplete shard set", [](auto& m) { m.files.erase(m.files.begin() + 1); m.launch.ple_gguf.reset(); }},
      {"launch.native", "launch names an unlisted file", [](auto& m) { m.launch.native = "weights/other.gguf"; }},
      {"launch.native", "native must be shard 00001", [](auto& m) { m.launch.native = "weights/m-00002-of-00002.gguf"; }},
      {"launch.pack", "pack must be a directory prefix", [](auto& m) { m.launch.pack = "pack/index.txt"; }},
      {"launch.tokenizer_pack", "tokenizer_pack must be <pack>/tokenizer", [](auto& m) { m.launch.tokenizer_pack = "pack"; }},
      {"launch.pack", "qwen requires pack", [](auto& m) { m.launch.pack.reset(); }},
      // Gemma 4 plan 1 Task 2: the gemma4 family belongs to the gemma4 profile only.
      {"engine.family", "family gemma4 on a qwen profile", [](auto& m) { m.family = secure_frontend::ManifestFamily::gemma4; }},
  };
  for (const auto& mutation : mutations) {
    auto m = valid_qwen_manifest();
    mutation.change(m);
    expect_issue(m, mutation.key, mutation.why);
  }
}

void reserved_manifest_names_rejected() {
  for (const auto* reserved : {"manifest.json", "MANIFEST", "Strata-Bundle", "SHA256SUMS", "missing", "manifest.json/x"}) {
    auto m = valid_qwen_manifest();
    m.files.push_back(file(reserved, FileRole::pack));
    expect_issue(m, "files[10].path", std::string{"reserved top-level name: "} + reserved);
  }
  auto nested = valid_qwen_manifest();
  nested.files.push_back(file("pack/MISSING", FileRole::pack));
  expect(!secure_frontend::validate_manifest(nested, rules()).has_value(), "reserved names only at the top level");
}

void model_name_index_json_rejected() {
  auto m = valid_qwen_manifest();
  m.name = "index.json";
  expect_issue(m, "name", "index.json is a file in roots.models");
  expect(!secure_frontend::parse_model_reference("INDEX.JSON:x").has_value(), "references are case-folded first");
}

void part_suffix_path_rejected() {
  for (const auto* path : {"pack/dense.bin.part", "pack/X.PART", "pack.part/index.txt"}) {
    auto m = valid_qwen_manifest();
    m.files.push_back(file(path, FileRole::pack));
    expect_issue(m, "files[10].path", std::string{".part is the in-progress suffix: "} + path);
  }
}

// A P1-valid path of exactly `length` bytes: 200-byte components below "x/".
std::string path_of_length(const std::size_t length) {
  std::string path = "x/";
  while (length - path.size() > 201U) path += std::string(200, 'a') + "/";
  return path + std::string(length - path.size(), 'b');
}

// Controller ruling A: the longest stored form (.staging/<key>/<path>.part,
// <name>/<tag>/v<sequence>-<12 hex>/<path>, files/<path>) must fit P1's 1024 bytes.
void stored_path_length_fits_p1() {
  // Short name/tag: the staging form (+47 bytes) is the longest, so 977 fits and 978 does not.
  for (const auto& [length, accepted] : std::vector<std::pair<std::size_t, bool>>{{1024, false}, {978, false}, {977, true}}) {
    auto m = valid_qwen_manifest();
    m.name = "q";
    m.tag = "t";
    m.files.push_back(file(path_of_length(length), FileRole::pack));
    if (accepted) {
      const auto issue = secure_frontend::validate_manifest(m, rules());
      expect(!issue.has_value(), "staging form just fits at " + std::to_string(length) + " bytes");
    } else {
      expect_issue(m, "files[10].path", "staging form overflows P1 at " + std::to_string(length) + " bytes");
    }
  }
  // Longest name, tag and sequence: <128>/<128>/v2147483647-<12 hex>/ is 283 bytes, so 741 fits and 742 does not.
  for (const auto& [length, accepted] : std::vector<std::pair<std::size_t, bool>>{{1024, false}, {742, false}, {741, true}}) {
    auto m = valid_qwen_manifest();
    m.name = std::string(128, 'n');
    m.tag = std::string(128, 't');
    m.sequence = 0x7FFFFFFFU;
    m.files.push_back(file(path_of_length(length), FileRole::pack));
    if (accepted) {
      const auto issue = secure_frontend::validate_manifest(m, rules());
      expect(!issue.has_value(), "version-dir form just fits at " + std::to_string(length) + " bytes");
    } else {
      expect_issue(m, "files[10].path", "version-dir form overflows P1 at " + std::to_string(length) + " bytes");
    }
  }
}

void file_and_directory_prefix_collision_rejected() {
  auto file_is_dir = valid_qwen_manifest();
  file_is_dir.files.push_back(file("pack/tokenizer", FileRole::pack));
  expect_issue(file_is_dir, "files[10].path", "a file path that is another file's directory");
  auto folded = valid_qwen_manifest();
  folded.files.push_back(file("PACK", FileRole::pack));
  expect_issue(folded, "files[10].path", "case-folded file/directory collision");
  // Final review I3 (Review Focus 2): directory components that differ only in case are one
  // directory on Windows; the second spelling is manifest_invalid, never a publish-time collision.
  auto spelled = valid_qwen_manifest();
  spelled.files.push_back(file("Pack/x", FileRole::pack));
  spelled.files.push_back(file("pack/y", FileRole::pack));
  expect_issue(spelled, "files[10].path", "Pack/x beside pack/y");
  auto nested = valid_qwen_manifest();
  nested.files.push_back(file("extra/Sub/a", FileRole::pack));
  nested.files.push_back(file("extra/sub/b", FileRole::pack));
  expect_issue(nested, "files[11].path", "a deeper directory spelled two ways");
  auto same = valid_qwen_manifest();
  same.files.push_back(file("extra/Sub/a", FileRole::pack));
  same.files.push_back(file("extra/Sub/b", FileRole::pack));
  expect(!secure_frontend::validate_manifest(same, rules()).has_value(), "one spelling of a mixed-case directory is fine");
}

void shard_set_must_be_complete_in_one_directory() {
  auto split = valid_qwen_manifest();
  split.files[1].path = "other/m-00002-of-00002.gguf";
  split.launch.ple_gguf = "other/m-00002-of-00002.gguf";
  expect_issue(split, "files", "shards must share one directory");
  auto three = valid_qwen_manifest();
  three.files[1].path = "weights/m-00003-of-00002.gguf";
  three.launch.ple_gguf = "weights/m-00003-of-00002.gguf";
  expect_issue(three, "files", "shard number above the count");
}

void launch_keys_follow_profile() {
  auto glm = valid_qwen_manifest();
  glm.profile = ManifestProfile::glm53;
  glm.family = secure_frontend::ManifestFamily::glm;
  expect_issue(glm, "launch.ple_gguf", "glm53 has only launch.native in schema 1");
  glm.launch.pack.reset();
  glm.launch.tokenizer_pack.reset();
  glm.launch.ple_gguf.reset();
  expect(!secure_frontend::validate_manifest(glm, rules()).has_value(), "glm53 with native only is valid");
  // Gemma 4 plan 1 Task 2: the gemma4 case is its own (it never falls through to qwen's).
  auto gemma = valid_qwen_manifest();
  gemma.profile = ManifestProfile::gemma4;
  gemma.family = secure_frontend::ManifestFamily::gemma4;
  expect_issue(gemma, "launch.ple_gguf", "a qwen-shaped launch on gemma4 is refused (no ple_gguf)");
  gemma.launch.ple_gguf.reset();
  expect_issue(gemma, "launch.pack", "a qwen-shaped launch on gemma4 is refused (no pack)");
  gemma.launch.pack.reset();
  expect(!secure_frontend::validate_manifest(gemma, rules()).has_value(),
         "gemma4 with native and tokenizer_pack is valid (the qwen pack files may still be listed)");
}

void wildcard_redirect_hosts_validated() {
  auto m = valid_qwen_manifest();
  m.redirect_hosts = {"*.aws.cdn.hf.co", "cas-bridge.xethub.hf.co"};
  expect(!secure_frontend::validate_manifest(m, rules()).has_value(), "valid redirect hosts");
  m.redirect_hosts.assign(17, "a.example");
  expect_issue(m, "redirect_hosts", "17 redirect hosts");
}

void total_size_above_budget_is_total_budget_exceeded() {
  auto m = valid_qwen_manifest();
  secure_frontend::ManifestRules small_rules;
  small_rules.max_total_bytes = 50;
  const auto issue = secure_frontend::validate_manifest(m, small_rules);
  expect(issue && issue->reason == AcquireReason::total_budget_exceeded && issue->key_path == "files",
         "total of 100 bytes exceeds a 50-byte budget");
}

AcquireManifest raw_manifest() {
  AcquireManifest m = valid_qwen_manifest();
  m.profile = ManifestProfile::raw;
  m.family = secure_frontend::ManifestFamily::none;
  m.min_version = *secure_frontend::parse_engine_version("0.0.0");
  m.files = {file("vocab.gguf", FileRole::weights)};
  m.launch = {};
  return m;
}

void raw_profile_rejected_in_release_build() {
  expect(!secure_frontend::kTestHooksEnabled, "this case runs only in the hooks-OFF configuration");
  expect_issue(raw_manifest(), "profile", "raw is rejected as manifest_invalid in release builds");
}

#if defined(SECURE_FRONTEND_TEST_HOOKS)
void raw_profile_accepted_with_test_hooks() {
  expect(!secure_frontend::validate_manifest(raw_manifest(), rules()).has_value(), "raw accepted in hooks builds");
  auto launched = raw_manifest();
  launched.launch.native = "vocab.gguf";
  expect_issue(launched, "launch.native", "raw has no launch keys");
}
#endif

void engine_version_parsed_with_fourth_component() {
  const auto four = secure_frontend::parse_engine_version("0.1.44.1");
  const auto three = secure_frontend::parse_engine_version("0.1.44");
  expect(four.has_value() && four->count == 4 && four->parts[3] == 1, "four components");
  expect(three && four && secure_frontend::compare_versions(*four, *three) > 0, "0.1.44.1 > 0.1.44");
  expect(three && secure_frontend::compare_versions(*three, *secure_frontend::parse_engine_version("0.1.44.0")) == 0,
         "a missing fourth part is 0");
  expect(four && secure_frontend::render_engine_version(*four) == "0.1.44.1", "render");
  for (const auto* bad : {"0.1", "0.1.44.1.2", "v0.1.44", "0.01.44", "0.1.10000", "0.1.-1", "0.1.44 "}) {
    expect(!secure_frontend::parse_engine_version(bad).has_value(), std::string{"rejected: "} + bad);
  }
}

// ---- SP4 Task 14: manifest JSON, catalog, trust rules, engine version ----

const std::string kBaseManifest = R"({
  "format": "strata-model-manifest", "schema_version": 1, "name": "m", "tag": "t", "version": "2026.10.14.1",
  "sequence": 1, "profile": "qwen", "engine": {"family": "strata", "min_version": "0.1.43"},
  "license": {"id": "Apache-2.0", "url": "https://h.example/l", "bundle_allowed": true, "notice": "n"},
  "hosts": ["h.example"], "redirect_hosts": ["*.cdn.h.example"],
  "files": [
    {"path": "weights/m-00001-of-00002.gguf", "role": "weights", "size": 1, "sha256": "SHA1", "sources": ["https://h.example/a"]},
    {"path": "weights/m-00002-of-00002.gguf", "role": "weights", "size": 1, "sha256": "SHA2", "sources": []},
    {"path": "pack/index.txt", "role": "pack", "size": 1, "sha256": "SHA3", "sources": []},
    {"path": "pack/tokenizer/vocab.json", "role": "tokenizer", "size": 1, "sha256": "SHA4", "sources": []}
  ],
  "launch": {"native": "weights/m-00001-of-00002.gguf", "pack": "pack", "tokenizer_pack": "pack/tokenizer"}
}
)";

std::string base_manifest() {
  std::string text = kBaseManifest;
  for (const auto& [mark, digit] : {std::pair{"SHA1", 'a'}, std::pair{"SHA2", 'b'}, std::pair{"SHA3", 'c'}, std::pair{"SHA4", 'd'}}) {
    text.replace(text.find(mark), 4, std::string(64, digit));
  }
  return text;
}

std::string replaced(std::string text, const std::string& from, const std::string& to) {
  const auto at = text.find(from);
  if (at == std::string::npos) throw std::runtime_error("fixture text not found: " + from);
  return text.replace(at, from.size(), to);
}

bool rejected(const std::string& text, const secure_frontend::AcquireReason reason = secure_frontend::AcquireReason::manifest_invalid) {
  const auto result = secure_frontend::parse_manifest(text, secure_frontend::ManifestRules{});
  const auto* issue = std::get_if<secure_frontend::ManifestIssue>(&result);
  return issue != nullptr && issue->reason == reason;
}

// F-31: the reason alone can be satisfied by an unrelated rule; also pin the key.
void expect_rejected_at(const std::string& text, const std::string& key, const std::string& why,
                        const secure_frontend::AcquireReason reason = secure_frontend::AcquireReason::manifest_invalid) {
  const auto result = secure_frontend::parse_manifest(text, secure_frontend::ManifestRules{});
  const auto* issue = std::get_if<secure_frontend::ManifestIssue>(&result);
  expect(issue != nullptr && issue->reason == reason && issue->key_path == key,
         "manifest rejected at " + key + ": " + why + " (got " + (issue ? issue->key_path : std::string{"no issue"}) + ")");
}

// SP5 M1 Task 11 (D-9: mmproj enters schema 1 as a manifest artifact role): the role parses,
// at most ONE entry, and the optional launch.mmproj must name a listed mmproj file.
// "vision"/"draft" stay reserved-and-rejected (the role we use is mmproj, like the pin's
// --mmproj flag spelling, serve/server.py:2557).
void manifest_accepts_mmproj_role_and_rejects_duplicates() {
  const std::string entry =
      "{\"path\": \"mmproj/v.gguf\", \"role\": \"mmproj\", \"size\": 1, \"sha256\": \"" + hex64('e') + "\", \"sources\": []}, ";
  const auto with_mmproj = [&](const std::string& launch) {
    std::string text =
        replaced(base_manifest(), "    {\"path\": \"pack/tokenizer/vocab.json\"", entry + "    {\"path\": \"pack/tokenizer/vocab.json\"");
    return replaced(text, "\"tokenizer_pack\": \"pack/tokenizer\"", "\"tokenizer_pack\": \"pack/tokenizer\", " + launch);
  };
  const auto parsed = secure_frontend::parse_manifest(with_mmproj("\"mmproj\": \"mmproj/v.gguf\""), secure_frontend::ManifestRules{});
  const auto* doc = std::get_if<secure_frontend::ParsedManifest>(&parsed);
  expect(doc != nullptr && doc->manifest.launch.mmproj == "mmproj/v.gguf",
         "one mmproj entry + launch.mmproj is valid schema 1 (Ruling T11-bb)");
  const std::string doubled = replaced(with_mmproj("\"mmproj\": \"mmproj/v.gguf\""), "{\"path\": \"pack/index.txt\"",
                                       replaced(entry, "mmproj/v.gguf", "mmproj/w.gguf") + "{\"path\": \"pack/index.txt\"");
  expect_rejected_at(doubled, "files", "a second mmproj entry is rejected");
  expect_rejected_at(with_mmproj("\"mmproj\": \"weights/m-00001-of-00002.gguf\""), "launch.mmproj",
                     "launch.mmproj must name a listed mmproj file, not the weights");
  expect_rejected_at(with_mmproj("\"mmproj\": \"mmproj/absent.gguf\""), "launch.mmproj", "launch.mmproj must name a listed file");

  auto m = valid_qwen_manifest();
  m.files.push_back(file("mmproj/v.gguf", FileRole::mmproj));
  m.launch.mmproj = "mmproj/v.gguf";
  expect(!secure_frontend::validate_manifest(m, rules()).has_value(), "qwen with mmproj is valid");
  auto glm = m;
  glm.profile = ManifestProfile::glm53;
  glm.family = secure_frontend::ManifestFamily::glm;
  glm.launch.pack.reset();
  glm.launch.tokenizer_pack.reset();
  glm.launch.ple_gguf.reset();
  expect_issue(glm, "launch.mmproj", "glm53 keeps only launch.native in schema 1 (Maya vision is the M2 kit, D-10)");
}

// ---- Gemma 4 plan 1 Task 2: the gemma4 profile and family (spec §6.1, §7) ----

AcquireManifest valid_gemma4_manifest() {
  AcquireManifest m;
  m.name = "gemma-4-26b-a4b-it";
  m.tag = "ud-q8_k_xl";
  m.version = "2026.10.22.1";
  m.sequence = 1;
  m.profile = ManifestProfile::gemma4;
  m.family = secure_frontend::ManifestFamily::gemma4;
  m.min_version = *secure_frontend::parse_engine_version("0.0.1");
  m.license = {"Apache-2.0", "https://fixture.example/license", true, "Gemma terms."};
  m.hosts = {"fixture.example"};
  m.files = {file("weights/gemma-4-26b-a4b-it-ud-q8_k_xl.gguf", FileRole::weights),
             file("tokenizer/vocab.json", FileRole::tokenizer), file("tokenizer/merges.json", FileRole::tokenizer),
             file("tokenizer/token_type.json", FileRole::tokenizer), file("tokenizer/tokenizer.json", FileRole::tokenizer)};
  m.launch.native = "weights/gemma-4-26b-a4b-it-ud-q8_k_xl.gguf";
  m.launch.tokenizer_pack = "tokenizer";
  return m;
}

const std::string kGemma4Manifest = R"({
  "format": "strata-model-manifest", "schema_version": 1, "name": "g", "tag": "t", "version": "2026.10.22.1",
  "sequence": 1, "profile": "gemma4", "engine": {"family": "gemma4", "min_version": "0.0.1"},
  "license": {"id": "Apache-2.0", "url": "https://h.example/l", "bundle_allowed": true},
  "hosts": ["h.example"],
  "files": [
    {"path": "weights/g.gguf", "role": "weights", "size": 1, "sha256": "SHA1", "sources": []},
    {"path": "tokenizer/vocab.json", "role": "tokenizer", "size": 1, "sha256": "SHA2", "sources": []}
  ],
  "launch": {"native": "weights/g.gguf", "tokenizer_pack": "tokenizer"}
}
)";

std::string gemma4_manifest_text() {
  std::string text = kGemma4Manifest;
  for (const auto& [mark, digit] : {std::pair{"SHA1", 'a'}, std::pair{"SHA2", 'b'}}) {
    text.replace(text.find(mark), 4, std::string(64, digit));
  }
  return text;
}

void manifest_accepts_gemma4_profile_with_gemma4_family() {
  const auto issue = secure_frontend::validate_manifest(valid_gemma4_manifest(), rules());
  expect(!issue.has_value(), "a gemma4 manifest (native + tokenizer_pack) is valid: " + (issue ? issue->key_path : std::string{}));
  expect(secure_frontend::profile_name(ManifestProfile::gemma4) == "gemma4", "profile name gemma4");
  expect(secure_frontend::family_name(secure_frontend::ManifestFamily::gemma4) == "gemma4", "family name gemma4");
  const auto parsed = secure_frontend::parse_manifest(gemma4_manifest_text(), rules());
  const auto* doc = std::get_if<secure_frontend::ParsedManifest>(&parsed);
  expect(doc != nullptr, "the gemma4 JSON manifest parses");
  if (doc != nullptr) {
    expect(doc->manifest.profile == ManifestProfile::gemma4 && doc->manifest.family == secure_frontend::ManifestFamily::gemma4,
           "JSON profile gemma4 with engine.family gemma4");
    expect(secure_frontend::profile_name(doc->manifest.profile) == "gemma4" &&
               secure_frontend::family_name(doc->manifest.family) == "gemma4",
           "the parsed values render back to their JSON names");
    expect(doc->manifest.launch.native == "weights/g.gguf" && doc->manifest.launch.tokenizer_pack == "tokenizer" &&
               !doc->manifest.launch.pack && !doc->manifest.launch.ple_gguf && !doc->manifest.launch.mmproj,
           "the gemma4 launch keys parse");
  }
  // A family that does not follow the profile is refused by name, both ways round.
  const std::string text = gemma4_manifest_text();
  for (const char* family : {"strata", "glm", "none"}) {
    expect_rejected_at(replaced(text, "\"family\": \"gemma4\"", std::string{"\"family\": \""} + family + "\""), "engine.family",
                       std::string{"gemma4 with family "} + family, AcquireReason::manifest_invalid);
  }
  expect_rejected_at(replaced(base_manifest(), "\"family\": \"strata\"", "\"family\": \"gemma4\""), "engine.family",
                     "qwen with family gemma4", AcquireReason::manifest_invalid);
  expect_rejected_at(replaced(text, "\"family\": \"gemma4\"", "\"family\": \"gemma\""), "engine.family",
                     "an unknown family is refused", AcquireReason::manifest_invalid);
  expect_rejected_at(replaced(text, "\"profile\": \"gemma4\"", "\"profile\": \"gemma\""), "profile",
                     "an unknown profile is refused", AcquireReason::manifest_invalid);
  for (const auto family : {secure_frontend::ManifestFamily::strata, secure_frontend::ManifestFamily::glm,
                            secure_frontend::ManifestFamily::none}) {
    auto m = valid_gemma4_manifest();
    m.family = family;
    expect_issue(m, "engine.family", "gemma4 needs family gemma4, not " + std::string{secure_frontend::family_name(family)});
  }
  auto glm = valid_gemma4_manifest();
  glm.profile = ManifestProfile::glm53;
  expect_issue(glm, "engine.family", "glm53 with family gemma4");
}

// D2: text only. gemma4 launches from native + tokenizer_pack; mmproj, ple_gguf and pack are refused.
void gemma4_launch_keys_are_text_only() {
  auto ple = valid_gemma4_manifest();
  ple.files.push_back(file("weights/ple.gguf", FileRole::weights));
  ple.launch.ple_gguf = "weights/ple.gguf";
  expect_issue(ple, "launch.ple_gguf", "gemma4 refuses launch.ple_gguf");
  auto pack = valid_gemma4_manifest();
  pack.files.push_back(file("pack/index.txt", FileRole::pack));
  pack.launch.pack = "pack";
  expect_issue(pack, "launch.pack", "gemma4 refuses launch.pack");
  auto vision = valid_gemma4_manifest();
  vision.files.push_back(file("mmproj/v.gguf", FileRole::mmproj));
  vision.launch.mmproj = "mmproj/v.gguf";
  expect_issue(vision, "launch.mmproj", "gemma4 refuses launch.mmproj (text only, D2)");
  auto no_tokenizer = valid_gemma4_manifest();
  no_tokenizer.launch.tokenizer_pack.reset();
  expect_issue(no_tokenizer, "launch.tokenizer_pack", "gemma4 requires launch.tokenizer_pack");
  auto file_tokenizer = valid_gemma4_manifest();
  file_tokenizer.launch.tokenizer_pack = "tokenizer/vocab.json";
  expect_issue(file_tokenizer, "launch.tokenizer_pack", "launch.tokenizer_pack must be a directory of listed files");
  auto unlisted = valid_gemma4_manifest();
  unlisted.launch.tokenizer_pack = "other";
  expect_issue(unlisted, "launch.tokenizer_pack", "launch.tokenizer_pack must name listed files");
  auto no_native = valid_gemma4_manifest();
  no_native.launch.native.reset();
  expect_issue(no_native, "launch.native", "gemma4 requires launch.native");
  auto tokenizer_native = valid_gemma4_manifest();
  tokenizer_native.launch.native = "tokenizer/vocab.json";
  expect_issue(tokenizer_native, "launch.native", "launch.native must name a weights file");
  auto sharded = valid_gemma4_manifest();
  sharded.files[0].path = "weights/g-00001-of-00002.gguf";
  sharded.files.push_back(file("weights/g-00002-of-00002.gguf", FileRole::weights));
  sharded.launch.native = "weights/g-00002-of-00002.gguf";
  expect_issue(sharded, "launch.native", "gemma4 launch.native must be shard 00001");
  sharded.launch.native = "weights/g-00001-of-00002.gguf";
  expect(!secure_frontend::validate_manifest(sharded, rules()).has_value(), "a sharded gemma4 weights set is valid");
}

// Owner 2026-10-29 (Qwen3.8's MTP draft layer): the mtp profile lists only the draft layer's tensors - weights at
// tensors/<name>.bin, each a window of its checkpoint shard - with no launch keys and the family none (never launched;
// strata-prepare mtp builds the layer from them).
void an_mtp_manifest_lists_only_windows_of_tensors() {
  AcquireManifest m;
  m.name = "qwen3.8-flash-next-mtp";
  m.tag = "download";
  m.version = "2026.10.29.1";
  m.sequence = 1;
  m.profile = ManifestProfile::mtp;
  m.family = secure_frontend::ManifestFamily::none;
  m.license = {"Apache-2.0", "https://fixture.example/license", false, "Notice."};
  m.hosts = {"fixture.example"};
  m.redirect_hosts = {"*.cdn.fixture.example"};
  const auto tensor = [](const std::string& name) {
    auto f = secure_frontend::ManifestFile{"tensors/" + name + ".bin", FileRole::weights, 10, hex64('a'),
                                           {"https://fixture.example/model-00001-of-00002.safetensors"}, std::nullopt};
    f.source_offset = 4096U;
    return f;
  };
  m.files = {tensor("mtp.fc_hidden.weight"), tensor("mtp.layers.0.mlp.experts.down_proj")};
  const auto issue = secure_frontend::validate_manifest(m, rules());
  expect(!issue.has_value(), "the draft layer's tensors are a valid mtp manifest: " + (issue ? issue->key_path : std::string{}));
  const auto with = [&m](const std::function<void(AcquireManifest&)>& change) {
    AcquireManifest changed = m;
    change(changed);
    return changed;
  };
  expect_issue(with([](auto& x) { x.files[0].role = FileRole::pack; }), "files[0].role", "mtp lists only weights");
  expect_issue(with([](auto& x) { x.files[0].path = "weights/mtp.fc_hidden.weight.bin"; }), "files[0].path", "only tensors/");
  expect_issue(with([](auto& x) { x.files[0].path = "tensors/sub/x.bin"; }), "files[0].path", "no folder below tensors/");
  expect_issue(with([](auto& x) { x.files[0].path = "tensors/x.gguf"; }), "files[0].path", "only .bin");
  expect_issue(with([](auto& x) { x.files[1].source_offset.reset(); }), "files[1].source_offset", "only windows of their sources");
  expect_issue(with([](auto& x) { x.launch.native = "tensors/mtp.fc_hidden.weight.bin"; }), "launch", "no launch keys");
  expect_issue(with([](auto& x) { x.family = secure_frontend::ManifestFamily::strata; }), "engine.family", "the family none");
  // the JSON names it
  const std::string text = replaced(replaced(base_manifest(), "\"profile\": \"qwen\"", "\"profile\": \"mtp\""), "\"family\": \"strata\"",
                                    "\"family\": \"none\"");
  const auto parsed = secure_frontend::parse_manifest(text, secure_frontend::ManifestRules{});
  const auto* problem = std::get_if<secure_frontend::ManifestIssue>(&parsed);
  expect(problem == nullptr || problem->key_path != "profile", "the profile is named mtp in the JSON");
}

void base_manifest_parses() {
  const auto result = secure_frontend::parse_manifest(base_manifest(), secure_frontend::ManifestRules{});
  const auto* parsed = std::get_if<secure_frontend::ParsedManifest>(&result);
  expect(parsed != nullptr, "the base manifest parses");
  if (parsed != nullptr) {
    expect(parsed->manifest.files.size() == 4 && parsed->manifest.files[1].sources.empty(), "sourceless file kept");
    expect(parsed->bytes == base_manifest(), "exact bytes kept");
    expect(!parsed->manifest.files[0].source_offset.has_value(), "a whole source: no window");
  }
  // owner 2026-10-29: a window of its source (bytes 1234.. of it)
  const std::string base = base_manifest();
  const std::string windowed = replaced(base, "\"size\": 1, \"sha256\": \"" + std::string(64, 'a'),
                                        "\"source_offset\": 1234, \"size\": 1, \"sha256\": \"" + std::string(64, 'a'));
  const auto window = secure_frontend::parse_manifest(windowed, secure_frontend::ManifestRules{});
  const auto* with = std::get_if<secure_frontend::ParsedManifest>(&window);
  expect(with != nullptr && with->manifest.files[0].source_offset == std::optional<std::uint64_t>{1234U}, "a window parses");
}

// Spec §10.3 DL-03 matrix.
void dl03_invalid_manifest_matrix() {
  const std::string base = base_manifest();
  struct Case final {
    std::string why;
    std::string text;
    std::string key;
  };
  const std::vector<Case> cases{
      {"unknown key", replaced(base, "\"sequence\": 1,", "\"sequence\": 1, \"extra\": 1,"), "extra"},
      {"duplicate key", replaced(base, "\"sequence\": 1,", "\"sequence\": 1, \"sequence\": 2,"), "manifest"},
      {"bad hex", replaced(base, std::string(64, 'a'), std::string(63, 'a') + "g"), "files[0].sha256"},
      {"size 0", replaced(base, "\"size\": 1, \"sha256\": \"" + std::string(64, 'a'), "\"size\": 0, \"sha256\": \"" + std::string(64, 'a')),
       "files[0].size"},
      {"..", replaced(base, "pack/index.txt\"", "pack/../index.txt\""), "files[2].path"},
      {"absolute", replaced(base, "\"pack/index.txt\"", "\"/pack/index.txt\""), "files[2].path"},
      {"case-fold collision", replaced(base, "pack/tokenizer/vocab.json\", \"role", "PACK/INDEX.TXT\", \"role"), "files[3].path"},
      {"control byte", replaced(base, "\"notice\": \"n\"", "\"notice\": \"n\\u0007\""), "license.notice"},
      {"[ in URL", replaced(base, "https://h.example/a", "https://h.example/a[1]"), "files[0].sources[0]"},
      {"role draft", replaced(base, "\"role\": \"pack\"", "\"role\": \"draft\""), "files[2].role"},
      {"role vision", replaced(base, "\"role\": \"pack\"", "\"role\": \"vision\""), "files[2].role"},
      {"incomplete shard set", replaced(base, "m-00002-of-00002.gguf", "m-00002-of-00003.gguf"), "files"},
      {"launch names an unlisted file",
       replaced(base, "\"native\": \"weights/m-00001-of-00002.gguf\"", "\"native\": \"weights/x.gguf\""), "launch.native"},
      {"single-label wildcard base", replaced(base, "*.cdn.h.example", "*.example"), "redirect_hosts[0]"},
      {"fraction", replaced(base, "\"sequence\": 1,", "\"sequence\": 1.0,"), "manifest"},
      {"string where integer", replaced(base, "\"sequence\": 1,", "\"sequence\": \"1\","), "sequence"},
      {"BOM", "\xEF\xBB\xBF" + base, "manifest"},
      {"P1 reserved device component", replaced(base, "pack/index.txt\"", "pack/con.txt\""), "files[2].path"},
      {"P1 trailing dot", replaced(base, "pack/index.txt\"", "pack/index.\""), "files[2].path"},
      {"negative window", replaced(base, "\"size\": 1, \"sha256\": \"" + std::string(64, 'a'),
                                     "\"source_offset\": -1, \"size\": 1, \"sha256\": \"" + std::string(64, 'a')),
       "files[0].source_offset"},
      {"string window", replaced(base, "\"size\": 1, \"sha256\": \"" + std::string(64, 'a'),
                                   "\"source_offset\": \"5\", \"size\": 1, \"sha256\": \"" + std::string(64, 'a')),
       "files[0].source_offset"},
  };
  for (const auto& c : cases) {
    expect(rejected(c.text), "manifest_invalid: " + c.why);
    expect_rejected_at(c.text, c.key, c.why);
  }
  expect_rejected_at(replaced(base, "\"schema_version\": 1", "\"schema_version\": 2"), "schema_version", "schema_version 2",
                     secure_frontend::AcquireReason::manifest_unsupported_version);
  expect(rejected(std::string(secure_frontend::kManifestMaxBytes + 1, ' ')), "larger than 1 MiB");
}

void dl03_http_url_rejected() {
  expect(rejected(replaced(base_manifest(), "https://h.example/a", "http://h.example/a")), "an http source is manifest_invalid");
}

void manifest_identity_is_sha256_of_exact_bytes() {
  const std::string text = base_manifest();
  const auto parsed = std::get<secure_frontend::ParsedManifest>(secure_frontend::parse_manifest(text, {}));
  expect(parsed.sha256 == secure_frontend::to_hex(secure_frontend::sha256(text)), "digest of the exact bytes");
  const auto spaced = std::get<secure_frontend::ParsedManifest>(secure_frontend::parse_manifest(text + " ", {}));
  expect(spaced.sha256 != parsed.sha256, "nothing is re-serialized: one more byte is another manifest");
  const auto upper = replaced(text, "\"name\": \"m\"", "\"name\": \"M\"");
  const auto lowered = std::get<secure_frontend::ParsedManifest>(secure_frontend::parse_manifest(upper, {}));
  expect(lowered.manifest.name == "m", "name ASCII-lowercased on input");
}

std::string read_text(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return std::string{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

// Spec §2.5: committed CATALOG.sha256 must list exactly the embedded manifests.
void dl05_catalog_pins_match_committed_list() {
  const auto& args = secure_frontend_test::extra_arguments();
  const std::filesystem::path production = args.at(0);
  const std::filesystem::path fixtures = args.at(1);
  std::string embedded_production;
  std::string embedded_fixtures;
  for (const auto& entry : secure_frontend::catalog_entries()) {
    expect(secure_frontend::to_hex(secure_frontend::sha256(entry.bytes)) == entry.sha256, "embedded digest recomputes");
    const auto parsed = secure_frontend::parse_manifest(entry.bytes, {});
    const auto* manifest = std::get_if<secure_frontend::ParsedManifest>(&parsed);
    expect(manifest != nullptr && manifest->manifest.name == entry.name && manifest->manifest.tag == entry.tag &&
               manifest->manifest.sequence == entry.sequence,
           "catalog entry parses and matches its path");
    (entry.fixture ? embedded_fixtures : embedded_production) +=
        std::string{entry.sha256} + "  " + std::string{entry.name} + "/" + std::string{entry.tag} + ".json\n";
  }
  expect(embedded_production == read_text(production), "catalog/CATALOG.sha256 matches the embedded production catalog");
  if (secure_frontend::kTestHooksEnabled) {
    expect(embedded_fixtures == read_text(fixtures), "tests/fixtures/catalog/CATALOG.sha256 matches");
  } else {
    expect(embedded_fixtures.empty(), "release builds embed no fixture");
  }
}

void engine_version_read_from_upstream_version_file() {
  const auto& args = secure_frontend_test::extra_arguments();
  expect(secure_frontend::built_engine_version_text() == args.at(0), "kEngineVersion is upstream.version without its leading v");
  expect(secure_frontend::render_engine_version(secure_frontend::built_engine_version()) == args.at(0), "parsed");
}

void trust_rules_catalog_digest_or_typed_digest() {
  using secure_frontend::AcquireReason;
  const std::string text = base_manifest();
  const std::string digest = secure_frontend::to_hex(secure_frontend::sha256(text));
  const auto untrusted = secure_frontend::trust_manifest_bytes(text, std::nullopt, {});
  expect(std::holds_alternative<secure_frontend::AcquireError>(untrusted) &&
             std::get<secure_frontend::AcquireError>(untrusted).reason == AcquireReason::manifest_untrusted, "no trust");
  const auto wrong = secure_frontend::trust_manifest_bytes(text, std::string(64, '0'), {});
  expect(std::get<secure_frontend::AcquireError>(wrong).reason == AcquireReason::manifest_untrusted, "wrong typed digest");
  const auto typed = secure_frontend::trust_manifest_bytes(text, digest, {});
  expect(std::holds_alternative<secure_frontend::TrustedManifest>(typed) &&
             std::get<secure_frontend::TrustedManifest>(typed).origin == secure_frontend::ModelOrigin::third_party,
         "--trust-manifest with the exact digest");
  expect(secure_frontend::parse_trust_argument("sha256:" + digest) == digest, "argument form");
  for (const auto* bad : {"sha256:ABC", "sha512:00", "", "sha256:"}) expect(!secure_frontend::parse_trust_argument(bad), bad);
  // Task 8 carry: a trusted digest never lets a P1-invalid path through.
  const std::string device = replaced(text, "pack/index.txt\"", "pack/con.txt\"");
  const auto device_trusted = secure_frontend::trust_manifest_bytes(device, secure_frontend::to_hex(secure_frontend::sha256(device)), {});
  expect(std::holds_alternative<secure_frontend::AcquireError>(device_trusted) &&
             std::get<secure_frontend::AcquireError>(device_trusted).reason == AcquireReason::manifest_invalid,
         "a typed digest does not bypass the P1 path grammar");
#if defined(SECURE_FRONTEND_TEST_HOOKS)
  const auto* fixture = secure_frontend::find_catalog_entry("fixture-qwen", "default");
  expect(fixture != nullptr && fixture->tag == "t1", "alias default -> t1");
  const auto catalog = secure_frontend::trust_manifest_bytes(fixture->bytes, std::nullopt, {});
  expect(std::get<secure_frontend::TrustedManifest>(catalog).origin == secure_frontend::ModelOrigin::catalog, "catalog digest");
  const std::string shadow = replaced(replaced(text, "\"name\": \"m\"", "\"name\": \"fixture-qwen\""), "\"tag\": \"t\"", "\"tag\": \"t1\"");
  const auto shadowed = secure_frontend::trust_manifest_bytes(shadow, secure_frontend::to_hex(secure_frontend::sha256(shadow)), {});
  expect(std::get<secure_frontend::AcquireError>(shadowed).reason == AcquireReason::manifest_shadows_catalog, "shadowing");
#endif
}

#if defined(SECURE_FRONTEND_TEST_HOOKS)
// Final review M1 (§2.5 rule 2): a typed digest must equal the file's digest, even when the
// bytes are a catalog manifest (a typo or the wrong file never goes unnoticed).
void typed_digest_must_match_catalog_bytes_too() {
  using secure_frontend::AcquireReason;
  const auto* fixture = secure_frontend::find_catalog_entry("fixture-qwen", "t1");
  expect(fixture != nullptr, "catalog fixture");
  const auto wrong = secure_frontend::trust_manifest_bytes(fixture->bytes, std::string(64, '0'), {});
  expect(std::holds_alternative<secure_frontend::AcquireError>(wrong) &&
             std::get<secure_frontend::AcquireError>(wrong).reason == AcquireReason::manifest_untrusted,
         "a typed digest that differs from catalog bytes: manifest_untrusted");
  const auto exact = secure_frontend::trust_manifest_bytes(fixture->bytes, secure_frontend::to_hex(secure_frontend::sha256(fixture->bytes)), {});
  expect(std::holds_alternative<secure_frontend::TrustedManifest>(exact) &&
             std::get<secure_frontend::TrustedManifest>(exact).origin == secure_frontend::ModelOrigin::catalog,
         "the exact digest of catalog bytes keeps catalog trust");
}
#endif

}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"source_url_grammar_matrix", source_url_grammar_matrix},
      {"dns_names_and_wildcards", dns_names_and_wildcards},
      {"proxy_grammar", proxy_grammar},
      {"valid_qwen_manifest_accepted", valid_qwen_manifest_accepted},
      {"manifest_field_rules_matrix", manifest_field_rules_matrix},
      {"reserved_manifest_names_rejected", reserved_manifest_names_rejected},
      {"model_name_index_json_rejected", model_name_index_json_rejected},
      {"part_suffix_path_rejected", part_suffix_path_rejected},
      {"stored_path_length_fits_p1", stored_path_length_fits_p1},
      {"file_and_directory_prefix_collision_rejected", file_and_directory_prefix_collision_rejected},
      {"shard_set_must_be_complete_in_one_directory", shard_set_must_be_complete_in_one_directory},
      {"launch_keys_follow_profile", launch_keys_follow_profile},
      {"manifest_accepts_mmproj_role_and_rejects_duplicates", manifest_accepts_mmproj_role_and_rejects_duplicates},
      {"manifest_accepts_gemma4_profile_with_gemma4_family", manifest_accepts_gemma4_profile_with_gemma4_family},
      {"gemma4_launch_keys_are_text_only", gemma4_launch_keys_are_text_only},
      {"wildcard_redirect_hosts_validated", wildcard_redirect_hosts_validated},
      {"total_size_above_budget_is_total_budget_exceeded", total_size_above_budget_is_total_budget_exceeded},
      {"raw_profile_rejected_in_release_build", raw_profile_rejected_in_release_build},
#if defined(SECURE_FRONTEND_TEST_HOOKS)
      {"raw_profile_accepted_with_test_hooks", raw_profile_accepted_with_test_hooks},
#endif
      {"engine_version_parsed_with_fourth_component", engine_version_parsed_with_fourth_component},
      {"base_manifest_parses", base_manifest_parses},
      {"an_mtp_manifest_lists_only_windows_of_tensors", an_mtp_manifest_lists_only_windows_of_tensors},
      {"dl03_invalid_manifest_matrix", dl03_invalid_manifest_matrix},
      {"dl03_http_url_rejected", dl03_http_url_rejected},
      {"manifest_identity_is_sha256_of_exact_bytes", manifest_identity_is_sha256_of_exact_bytes},
      {"dl05_catalog_pins_match_committed_list", dl05_catalog_pins_match_committed_list},
      {"engine_version_read_from_upstream_version_file", engine_version_read_from_upstream_version_file},
      {"trust_rules_catalog_digest_or_typed_digest", trust_rules_catalog_digest_or_typed_digest},
#if defined(SECURE_FRONTEND_TEST_HOOKS)
      {"typed_digest_must_match_catalog_bytes_too", typed_digest_must_match_catalog_bytes_too},
#endif
  }, failures);
}
