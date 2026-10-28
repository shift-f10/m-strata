#pragma once

// Model downloads (spec docs/superpowers/specs/2026-10-23-model-downloads-design.md; plan Task 1).
// Owner rule O4: "make sure there is a safety in the download that only downloads from huggingface
// and a gguf". Every download is an SP4 manifest whose sources are Hugging Face file URLs at a
// commit and whose files end in .gguf; this module is the one place those URLs and manifests are
// made, and the only place a pasted link is read. Pure: no network, no filesystem.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace secure_frontend {

// The one host a source may name, and the hosts Hugging Face redirects a file download to (its
// LFS CDN and its Xet bridge). A pattern matches exactly one extra leftmost label (SP4's rule).
inline constexpr std::string_view kHfHost = "huggingface.co";
// So each level is named; the box run of 2026-10-23 was sent to us.aws.cdn.hf.co.
inline constexpr std::array<std::string_view, 5> kHfRedirectPatterns{"*.huggingface.co", "*.hf.co", "*.xethub.hf.co", "*.cdn.hf.co",
                                                                     "*.aws.cdn.hf.co"};

// "owner/name": each part 1-96 characters of [A-Za-z0-9._-], not starting with '.' or '-'.
[[nodiscard]] bool is_hf_repo(std::string_view repo) noexcept;
// A 40-character lowercase hex commit.
[[nodiscard]] bool is_hf_commit(std::string_view revision) noexcept;
// A branch, tag or commit as it may appear in a link: 1-96 of [A-Za-z0-9._-], not starting with '.'.
[[nodiscard]] bool is_hf_revision(std::string_view revision) noexcept;
// A file path inside a repository: '/'-separated segments of [A-Za-z0-9._-] (no "." or ".."
// segment, none empty), at most 512 bytes, ending in ".gguf" (any case).
[[nodiscard]] bool is_hf_gguf_path(std::string_view path) noexcept;

// A pasted link: https://huggingface.co/<owner>/<repo>/(resolve|blob)/<revision>/<path>.gguf,
// optionally followed by "?download=true" (the site's own download links); nothing else. The
// reason is fixed text for the page.
struct HfFileLink final {
  std::string repo;
  std::string revision;
  std::string path;
};
[[nodiscard]] std::variant<HfFileLink, std::string_view> parse_hf_link(std::string_view text);

// https://huggingface.co/<repo>/resolve/<commit>/<path> (a manifest source).
[[nodiscard]] std::string hf_file_url(std::string_view repo, std::string_view commit, std::string_view path);
// The API URLs the metadata lookup reads (strata hf-info): the revision's commit, and a folder's
// listing at a commit (sizes and LFS SHA-256s).
[[nodiscard]] std::string hf_revision_api_url(std::string_view repo, std::string_view revision);
[[nodiscard]] std::string hf_tree_api_url(std::string_view repo, std::string_view commit, std::string_view folder);

// A split GGUF's shard names: "<stem>-0000k-of-0000n.gguf" gives the n names in order; any other
// name gives itself alone.
[[nodiscard]] std::vector<std::string> gguf_shard_set(std::string_view path);

struct HfFile final {
  std::string path;          // in the repository (and below the version directory)
  std::uint64_t size{0};
  std::string sha256;        // 64 lowercase hex
  bool mmproj{false};        // the vision encoder file (role mmproj)
};
struct HfDownload final {
  std::string name;          // the SP4 model name (is_valid_model_name)
  std::string tag;
  std::string profile;       // "gguf" (downloaded to be prepared here) or "glm53"
  std::string repo;
  std::string commit;        // 40 hex
  std::vector<HfFile> files;
  std::string license_id{"LicenseRef-see-model-card"};
  std::string license_url;   // https://huggingface.co/<repo>
};
// The SP4 manifest (strata-model-manifest, schema 1) for the download, or a fixed reason when a
// rule fails: the repo, the commit, every path a .gguf, sizes non-zero, SHA-256s well formed.
// glm53 names its first shard as launch.native; gguf has no launch keys.
[[nodiscard]] std::variant<std::string, std::string_view> hf_manifest_json(const HfDownload& download, std::string_view version);

}  // namespace secure_frontend
