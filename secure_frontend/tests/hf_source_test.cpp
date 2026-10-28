// Model downloads (spec 2026-10-23-model-downloads-design.md, plan Task 1). Owner rule O4: "make
// sure there is a safety in the download that only downloads from huggingface and a gguf". The
// link grammar, the source and API URLs, split-shard sets, and the SP4 manifest a download becomes
// (parsed back by SP4's own reader with the release rules).
#include "secure_frontend/acquire_manifest.hpp"
#include "secure_frontend/acquire_url.hpp"
#include "secure_frontend/hf_source.hpp"
#include "secure_frontend/manifest_json.hpp"
#include "support/test_support.hpp"

#include <iostream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

int failures = 0;
void expect(const bool value, const std::string_view message) {
  if (!value) {
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
  }
}

using Strings = std::vector<std::string>;
constexpr std::string_view kCommit = "0123456789abcdef0123456789abcdef01234567";
const std::string kSha(64, 'a');

void links_only_huggingface_gguf() {
  struct Accepted {
    std::string_view url;
    std::string_view repo;
    std::string_view revision;
    std::string_view path;
  };
  for (const Accepted& a : std::vector<Accepted>{
           {"https://huggingface.co/ISTA-DASLab/Qwen3.8-Flash-Next-GSQ-RCO-GGUF/resolve/main/IQ3_S/model-00001-of-00002.gguf",
            "ISTA-DASLab/Qwen3.8-Flash-Next-GSQ-RCO-GGUF", "main", "IQ3_S/model-00001-of-00002.gguf"},
           {"https://huggingface.co/unsloth/Qwen3.6-35B-A3B-MTP-GGUF/blob/0123456789abcdef0123456789abcdef01234567/Q4_K_M.GGUF",
            "unsloth/Qwen3.6-35B-A3B-MTP-GGUF", "0123456789abcdef0123456789abcdef01234567", "Q4_K_M.GGUF"},
           {"https://huggingface.co/a/b/resolve/v1.0/x.gguf?download=true", "a/b", "v1.0", "x.gguf"},
           // The revision's text also in the owner or repo name: the path still starts after it.
           {"https://huggingface.co/main/main/resolve/main/sub/main.gguf", "main/main", "main", "sub/main.gguf"}}) {
    const auto parsed = secure_frontend::parse_hf_link(a.url);
    const auto* link = std::get_if<secure_frontend::HfFileLink>(&parsed);
    expect(link != nullptr && link->repo == a.repo && link->revision == a.revision && link->path == a.path,
           std::string{"accepted: "} + std::string{a.url});
  }
  for (const char* refused : {
           "http://huggingface.co/a/b/resolve/main/x.gguf",                 // not https
           "https://hf.co/a/b/resolve/main/x.gguf",                         // another host
           "https://cdn-lfs.huggingface.co/a/b/resolve/main/x.gguf",        // a CDN host is never a source
           "https://huggingface.co.evil.example/a/b/resolve/main/x.gguf",   // a look-alike
           "https://user@huggingface.co/a/b/resolve/main/x.gguf",           // userinfo
           "https://huggingface.co:8443/a/b/resolve/main/x.gguf",           // a port
           "https://huggingface.co/a/b/resolve/main/x.safetensors",         // not a gguf (O4)
           "https://huggingface.co/a/b/resolve/main/x.gguf.part",
           "https://huggingface.co/a/b/resolve/main/x.bin",
           "https://huggingface.co/a/b/resolve/main/../x.gguf",             // traversal
           "https://huggingface.co/a/b/resolve/main/%2e%2e/x.gguf",          // encoded traversal (no % at all)
           "https://huggingface.co/a/b/resolve/main/sub//x.gguf",
           "https://huggingface.co/a/b/tree/main/x.gguf",                   // a folder view
           "https://huggingface.co/a/b/resolve/main/",                      // no file
           "https://huggingface.co/a/b",                                    // a repository page
           "https://huggingface.co/a/b/resolve/main/x.gguf?raw=1",          // only ?download=true
           "https://huggingface.co/a/b/resolve/main/x.gguf#frag",
           "https://huggingface.co/a/b/resolve/main/x y.gguf",
           "https://huggingface.co/a/b/resolve/main/sub\\x.gguf",
           "https://huggingface.co/datasets/a/b/resolve/main/x.gguf",       // a dataset, not a model
           "https://huggingface.co/.a/b/resolve/main/x.gguf",
           "https://huggingface.co/a/b/resolve/.main/x.gguf",
           " https://huggingface.co/a/b/resolve/main/x.gguf",
           ""}) {
    const auto parsed = secure_frontend::parse_hf_link(refused);
    const auto* reason = std::get_if<std::string_view>(&parsed);
    expect(reason != nullptr && !reason->empty(), std::string{"refused with a reason: "} + refused);
  }
  expect(secure_frontend::is_hf_gguf_path("a/b.GgUf") && !secure_frontend::is_hf_gguf_path("a/b.gguf/") &&
             !secure_frontend::is_hf_gguf_path(".gguf") && !secure_frontend::is_hf_gguf_path("./a.gguf"),
         "the .gguf rule is the path's own");
  expect(secure_frontend::is_hf_commit(kCommit) && !secure_frontend::is_hf_commit("main") &&
             !secure_frontend::is_hf_commit("0123456789ABCDEF0123456789ABCDEF01234567"),
         "a commit is 40 lowercase hex");
  expect(secure_frontend::is_hf_repo("ISTA-DASLab/Qwen3.8-Flash-Next-GSQ-RCO-GGUF") && !secure_frontend::is_hf_repo("a") &&
             !secure_frontend::is_hf_repo("a/b/c") && !secure_frontend::is_hf_repo("a/-b"),
         "a repository is owner/name");
}

void urls_and_shard_sets() {
  expect(secure_frontend::hf_file_url("a/b", kCommit, "IQ3_S/x-00001-of-00002.gguf") ==
             "https://huggingface.co/a/b/resolve/" + std::string{kCommit} + "/IQ3_S/x-00001-of-00002.gguf",
         "a source is the resolve URL at the commit");
  expect(secure_frontend::hf_revision_api_url("a/b", "main") == "https://huggingface.co/api/models/a/b/revision/main",
         "the revision lookup");
  expect(secure_frontend::hf_tree_api_url("a/b", kCommit, "IQ3_S") ==
             "https://huggingface.co/api/models/a/b/tree/" + std::string{kCommit} + "/IQ3_S",
         "a folder listing at the commit");
  expect(secure_frontend::hf_tree_api_url("a/b", kCommit, "") == "https://huggingface.co/api/models/a/b/tree/" + std::string{kCommit},
         "the top folder");
  expect(secure_frontend::gguf_shard_set("IQ3_S/x-00002-of-00003.gguf") ==
             Strings{"IQ3_S/x-00001-of-00003.gguf", "IQ3_S/x-00002-of-00003.gguf", "IQ3_S/x-00003-of-00003.gguf"},
         "a split model's shards");
  expect(secure_frontend::gguf_shard_set("x.gguf") == Strings{"x.gguf"}, "a single file is itself");
  expect(secure_frontend::gguf_shard_set("x-00001-of-00001.gguf") == Strings{"x-00001-of-00001.gguf"}, "one of one");
}

secure_frontend::HfDownload sample(const std::string& profile) {
  secure_frontend::HfDownload d;
  d.name = "qwen3.8-flash-next";
  d.tag = "iq2_xs";
  d.profile = profile;
  d.repo = "ISTA-DASLab/Qwen3.8-Flash-Next-GSQ-RCO-GGUF";
  d.commit = std::string{kCommit};
  d.files = {{"IQ2_XS/m-00001-of-00002.gguf", 1000, kSha, false},
             {"IQ2_XS/m-00002-of-00002.gguf", 2000, std::string(64, 'b'), false},
             {"mmproj-F16.gguf", 300, std::string(64, 'c'), true}};
  d.license_url = "https://huggingface.co/" + d.repo;
  return d;
}

// The manifest SP4 reads back with the release rules (no raw profile): Hugging Face as the one host,
// its download hosts as the redirect patterns, every source the resolve URL at the commit.
void manifests_are_huggingface_gguf_only() {
  secure_frontend::ManifestRules rules;
  rules.allow_raw_profile = false;
  for (const std::string profile : {"gguf", "glm53"}) {
    const auto text = secure_frontend::hf_manifest_json(sample(profile), "2026.10.23.1");
    const auto* json = std::get_if<std::string>(&text);
    expect(json != nullptr, "a manifest for " + profile);
    if (json == nullptr) continue;
    const auto parsed = secure_frontend::parse_manifest(*json, rules);
    const auto* manifest = std::get_if<secure_frontend::ParsedManifest>(&parsed);
    expect(manifest != nullptr, "SP4 reads the " + profile + " manifest: " +
                                    (manifest ? std::string{} : std::get<secure_frontend::ManifestIssue>(parsed).key_path + " " +
                                                                    std::string{std::get<secure_frontend::ManifestIssue>(parsed).message}));
    if (manifest == nullptr) continue;
    const auto& m = manifest->manifest;
    expect(m.hosts == Strings{"huggingface.co"} &&
               m.redirect_hosts == Strings{"*.huggingface.co", "*.hf.co", "*.xethub.hf.co", "*.cdn.hf.co", "*.aws.cdn.hf.co"},
           "Hugging Face alone, and its download hosts");
    expect(m.files.size() == 3U && m.files[0].sources ==
                                       Strings{"https://huggingface.co/ISTA-DASLab/Qwen3.8-Flash-Next-GSQ-RCO-GGUF/resolve/" +
                                               std::string{kCommit} + "/IQ2_XS/m-00001-of-00002.gguf"},
           "the source is the resolve URL at the commit");
    expect(m.files[2].role == secure_frontend::FileRole::mmproj, "the mmproj keeps its role");
    if (profile == "glm53") {
      expect(m.launch.native == std::optional<std::string>{"IQ2_XS/m-00001-of-00002.gguf"}, "glm53 names its first shard");
    } else {
      expect(m.profile == secure_frontend::ManifestProfile::gguf && !m.launch.native.has_value() && !m.launch.pack.has_value(),
             "gguf has no launch keys");
    }
  }
  // The rules, each refused with a reason.
  const auto refused = [](secure_frontend::HfDownload d, const std::string& what) {
    expect(std::holds_alternative<std::string_view>(secure_frontend::hf_manifest_json(d, "2026.10.23.1")), "refused: " + what);
  };
  auto d = sample("gguf");
  d.files[0].path = "IQ2_XS/m.safetensors";
  refused(d, "a file that is not a gguf");
  d = sample("gguf");
  d.files[1].sha256 = "xyz";
  refused(d, "a malformed SHA-256");
  d = sample("gguf");
  d.files[0].size = 0;
  refused(d, "an empty file");
  d = sample("gguf");
  d.commit = "main";
  refused(d, "a branch instead of a commit");
  d = sample("gguf");
  d.repo = "not a repo";
  refused(d, "a bad repository");
  d = sample("gguf");
  d.profile = "qwen";
  refused(d, "a profile other than gguf or glm53");
  d = sample("gguf");
  d.files.clear();
  refused(d, "no files");
  // SP4 itself refuses a gguf manifest with launch keys or a non-gguf file, and qwen still needs a pack.
  const auto text = std::get<std::string>(secure_frontend::hf_manifest_json(sample("gguf"), "2026.10.23.1"));
  std::string with_launch = text;
  const auto brace = with_launch.rfind('}');
  with_launch.insert(brace, ",\"launch\":{\"native\":\"IQ2_XS/m-00001-of-00002.gguf\"}");
  secure_frontend::ManifestRules release;
  release.allow_raw_profile = false;
  expect(std::holds_alternative<secure_frontend::ManifestIssue>(secure_frontend::parse_manifest(with_launch, release)),
         "SP4: a gguf manifest has no launch keys");
  std::string with_bin = text;
  for (std::size_t at = with_bin.find("m-00002-of-00002.gguf"); at != std::string::npos; at = with_bin.find("m-00002-of-00002.gguf", at)) {
    with_bin.replace(at, std::string_view{"m-00002-of-00002.gguf"}.size(), "m-00002-of-00002.bin");
  }
  expect(std::holds_alternative<secure_frontend::ManifestIssue>(secure_frontend::parse_manifest(with_bin, release)),
         "SP4: every file of a gguf manifest is a .gguf (O4 in the schema)");
}

}  // namespace

// Box run 2026-10-23: Hugging Face answered the resolve URL of a real GGUF with a 302 to
// us.aws.cdn.hf.co, three labels under hf.co, which SP4's one-label wildcard (`*.hf.co`) does not
// admit (redirect_not_allowed). The manifests name Hugging Face's CDN levels too; nothing else.
void cdn_redirects_are_admitted() {
  const auto text = secure_frontend::hf_manifest_json(sample("gguf"), "2026.10.23.1");
  const auto* json = std::get_if<std::string>(&text);
  expect(json != nullptr, "a manifest");
  if (json == nullptr) return;
  secure_frontend::ManifestRules rules;
  rules.allow_raw_profile = false;
  const auto parsed = secure_frontend::parse_manifest(*json, rules);
  const auto* manifest = std::get_if<secure_frontend::ParsedManifest>(&parsed);
  expect(manifest != nullptr, "SP4 reads it");
  if (manifest == nullptr) return;
  Strings allowed = manifest->manifest.hosts;
  allowed.insert(allowed.end(), manifest->manifest.redirect_hosts.begin(), manifest->manifest.redirect_hosts.end());
  for (const std::string host : {"us.aws.cdn.hf.co", "eu.aws.cdn.hf.co", "cas-bridge.xethub.hf.co", "cdn-lfs.hf.co", "cdn-lfs-us-1.hf.co",
                                 "cdn-lfs.huggingface.co", "huggingface.co"}) {
    expect(secure_frontend::host_in(allowed, host), host + " is admitted");
  }
  for (const std::string host : {"hf.co.example.com", "example.com", "a.b.us.aws.cdn.hf.co", "huggingface.co.example.net", "aws.cdn.hf.co.evil"}) {
    expect(!secure_frontend::host_in(allowed, host), host + " is not");
  }
}

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"links_only_huggingface_gguf", links_only_huggingface_gguf},
      {"urls_and_shard_sets", urls_and_shard_sets},
      {"manifests_are_huggingface_gguf_only", manifests_are_huggingface_gguf_only},
      {"cdn_redirects_are_admitted", cdn_redirects_are_admitted},
  }, failures);
}
