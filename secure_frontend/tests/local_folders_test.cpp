// GUI-first start-up (docs/superpowers/specs/2026-10-27-gui-first-install-design.md §4.2, §4.3, §6): the folders the
// page may pick on this PC - the local drives with their free space, one folder's sub-folders and GGUF files - and a
// model folder made private (owner-only) so the server trusts it.
#include "secure_frontend/canonical_path.hpp"
#include "secure_frontend/local_folders.hpp"
#include "support/test_support.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <variant>
#include <vector>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace {

int failures = 0;
void expect(const bool condition, const std::string& message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
  }
}

void volumes_are_the_local_drives() {
  const auto volumes = secure_frontend::local_volumes();
  expect(!volumes.empty(), "at least one local drive");
  const auto scratch = secure_frontend_test::make_scratch_directory();
  const std::string scratch_root = secure_frontend::path_to_utf8(scratch.root_path());
  bool found = false;
  for (const auto& volume : volumes) {
    expect(!volume.path.empty() && volume.total_bytes > 0U && volume.free_bytes <= volume.total_bytes,
           "a drive has a path, a size and its free space: " + volume.path);
#if defined(_WIN32)
    if (volume.path.size() == 3U && scratch_root.size() >= 2U &&
        std::tolower(static_cast<unsigned char>(volume.path[0])) == std::tolower(static_cast<unsigned char>(scratch_root[0]))) {
      found = true;
    }
#else
    if (volume.path == "/") found = true;
#endif
  }
  expect(found, "the scratch folder's drive is listed");
  const auto free = secure_frontend::free_space(scratch);
  expect(free.has_value() && *free > 0U, "a folder's volume reports its free space");
}

void a_folder_lists_its_folders_and_gguf_files() {
  const auto root = secure_frontend_test::make_scratch_directory();
  secure_frontend_test::make_private_directory(root / "zeta");
  secure_frontend_test::make_private_directory(root / "Alpha");
  secure_frontend_test::make_private_directory(root / ".hidden");
  secure_frontend_test::write_file(root / "model-Q8_0.gguf", "GGUF1234");
  secure_frontend_test::write_file(root / "SHOUT.GGUF", "GGUF");
  secure_frontend_test::write_file(root / "notes.txt", "mine");
  const auto listed = secure_frontend::list_local_folder(secure_frontend::path_to_utf8(root));
  const auto* listing = std::get_if<secure_frontend::FolderListing>(&listed);
  expect(listing != nullptr, "the folder lists");
  if (listing == nullptr) return;
  std::string names;
  for (const auto& entry : listing->entries) names += (entry.folder ? "[" : "") + entry.name + (entry.folder ? "] " : " ");
  expect(names == "[Alpha] [zeta] model-Q8_0.gguf SHOUT.GGUF ", "folders first, then GGUF files, by name; no hidden entry: " + names);
  const auto model = std::find_if(listing->entries.begin(), listing->entries.end(),
                                  [](const secure_frontend::FolderEntry& entry) { return entry.name == "model-Q8_0.gguf"; });
  expect(model != listing->entries.end() && model->bytes == 8U, "a file's size");
  expect(listing->parent == secure_frontend::path_to_utf8(root.lexically_normal().parent_path()) && listing->free_bytes > 0U &&
             !listing->truncated,
         "its parent and its volume's free space");

  for (const std::string& bad : std::vector<std::string>{"", "relative/folder", "..", secure_frontend::path_to_utf8(root / ".." / "x")}) {
    expect(std::holds_alternative<secure_frontend::FolderError>(secure_frontend::list_local_folder(bad)), "refused: \"" + bad + "\"");
  }
#if defined(_WIN32)
  for (const std::string& bad : std::vector<std::string>{"\\\\server\\share\\models", "\\\\?\\C:\\", "\\\\.\\PhysicalDrive0", "C:models"}) {
    expect(std::holds_alternative<secure_frontend::FolderError>(secure_frontend::list_local_folder(bad)),
           "a network, device or drive-relative path is refused: " + bad);
  }
#endif
  const auto missing = secure_frontend::list_local_folder(secure_frontend::path_to_utf8(root / "nope"));
  const auto* error = std::get_if<secure_frontend::FolderError>(&missing);
  expect(error != nullptr && error->message.find("nope") == std::string::npos, "a missing folder is refused without its path");
}

// A copy onto a drive whose root grants Authenticated Users write (the owner's F:) is not a trusted model folder;
// the page offers to make it private: only that folder and its files, and only a flat folder of model files.
void a_model_folder_is_made_private() {
  const auto root = secure_frontend_test::make_scratch_directory();
  const auto folder = root / "flint";
  secure_frontend_test::make_private_directory(folder);
  secure_frontend_test::write_file(folder / "flint-Q8_0.gguf", "GGUF");
#if defined(_WIN32)
  expect(secure_frontend_test::grant_everyone_write(folder), "the test widens the folder");
#else
  ::chmod(folder.c_str(), 0777);
#endif
  expect(!std::holds_alternative<secure_frontend::CanonicalRoot>(secure_frontend::open_root(folder)),
         "a folder others may write is not trusted");
  const auto made = secure_frontend::make_model_folder_private(folder);
  expect(!made.has_value(), "it is made private: " + (made ? made->message : std::string{}));
  auto trusted = secure_frontend::open_root(folder);
  expect(std::holds_alternative<secure_frontend::CanonicalRoot>(trusted), "and trusted afterwards");
  const auto relative = secure_frontend::parse_relative_path("flint-Q8_0.gguf");
  expect(std::holds_alternative<secure_frontend::CanonicalRoot>(trusted) &&
             std::holds_alternative<secure_frontend::OpenedFile>(secure_frontend::open_beneath(
                 std::get<secure_frontend::CanonicalRoot>(trusted), std::get<secure_frontend::RelativePath>(relative))),
         "its model file opens");

  const auto nested = root / "nested";
  secure_frontend_test::make_private_directory(nested);
  secure_frontend_test::make_private_directory(nested / "inner");
  secure_frontend_test::write_file(nested / "m.gguf", "GGUF");
  expect(secure_frontend::make_model_folder_private(nested).has_value(), "a folder with folders in it is refused");
  const auto empty = root / "empty";
  secure_frontend_test::make_private_directory(empty);
  secure_frontend_test::write_file(empty / "notes.txt", "mine");
  expect(secure_frontend::make_model_folder_private(empty).has_value(), "a folder with no GGUF file is refused");
  expect(secure_frontend::make_model_folder_private(folder.root_path()).has_value(), "a drive's root is refused");
  expect(secure_frontend::make_model_folder_private("relative").has_value(), "a relative path is refused");
}

// The owner's raon1 folders (2026-10-27): a model with the pack its tools built beside it (pack/, as Maya's setup and
// the server put a GLM-5.3 or raon1 pack; maya-ds4/ for DeepSeek, maya-mm3/ for MiniMax) and Hugging Face's download
// cache (.cache/) is still
// a model folder; the whole tree is made private. Any other folder in it is still refused.
void a_model_folder_with_its_pack_is_made_private() {
  const auto root = secure_frontend_test::make_scratch_directory();
  const auto folder = root / "raon1_q8";
  secure_frontend_test::make_private_directory(folder);
  secure_frontend_test::write_file(folder / "raon-1-mini-preview-Q8_0.gguf", "GGUF");
  for (const char* const inner : {"pack", "pack/tokenizer", "pack/slots", "pack-other", "maya-ds4", "maya-mm3", "maya-mm3-other", ".cache",
                                  ".cache/huggingface"}) {
    secure_frontend_test::make_private_directory(folder / inner);
  }
  secure_frontend_test::write_file(folder / "pack" / "index.txt", "index");
  secure_frontend_test::write_file(folder / "pack" / "tokenizer" / "vocab.json", "{}");
  secure_frontend_test::write_file(folder / ".cache" / "huggingface" / "CACHEDIR.TAG", "tag");
  // every folder others may write, as on the owner's drives (their ACL inherited from the drive's root)
  for (const char* const inner : {"", "pack", "pack/tokenizer", "pack/slots", "pack-other", "maya-ds4", "maya-mm3", "maya-mm3-other", ".cache",
                                  ".cache/huggingface"}) {
#if defined(_WIN32)
    expect(secure_frontend_test::grant_everyone_write(folder / inner), std::string{"the test widens "} + inner);
#else
    ::chmod((folder / inner).c_str(), 0777);
#endif
  }
  expect(!std::holds_alternative<secure_frontend::CanonicalRoot>(secure_frontend::open_root(folder / "pack")),
         "a pack others may write is not trusted");
  const auto made = secure_frontend::make_model_folder_private(folder);
  expect(!made.has_value(), "a model folder with its pack is made private: " + (made ? made->message : std::string{}));
  for (const char* const inner : {"", "pack", "pack/tokenizer", ".cache/huggingface"}) {
    expect(std::holds_alternative<secure_frontend::CanonicalRoot>(secure_frontend::open_root(folder / inner)),
           std::string{"and trusted afterwards: "} + inner);
  }

  const auto other = root / "other";
  secure_frontend_test::make_private_directory(other);
  secure_frontend_test::make_private_directory(other / "pack");
  secure_frontend_test::make_private_directory(other / "Documents");
  secure_frontend_test::write_file(other / "m.gguf", "GGUF");
  const auto refused = secure_frontend::make_model_folder_private(other);
  expect(refused.has_value() && refused->message.find("folder") != std::string::npos, "any other folder in it is refused");
  const auto lookalike = root / "lookalike";
  secure_frontend_test::make_private_directory(lookalike);
  secure_frontend_test::make_private_directory(lookalike / "package");
  secure_frontend_test::write_file(lookalike / "m.gguf", "GGUF");
  expect(secure_frontend::make_model_folder_private(lookalike).has_value(), "a folder only named like a pack is refused");
#if !defined(_WIN32)
  const auto linked = root / "linked";
  secure_frontend_test::make_private_directory(linked);
  secure_frontend_test::make_private_directory(linked / "pack");
  secure_frontend_test::write_file(linked / "m.gguf", "GGUF");
  std::filesystem::create_directory_symlink(root / "other", linked / "pack" / "away");
  expect(secure_frontend::make_model_folder_private(linked).has_value(), "a link inside its pack is refused");
#endif
}

// The owner, pointing his Qwen3.8 card at its MTP draft layer (2026-10-29): "when I select the mtp I get the error that
// the folder was not made private" - upstream's draft layer holds no .gguf file. Its runtime folder (rt: dense.bin,
// experts.bin, draft_vocab.bin and dense.txt) and the folder holding it as upstream's setup and the server's downloads lay
// it out (rt/ beside the fetched tensors/, with upstream's notes and its intermediate mtp-q2_0.gguf) are made private;
// a folder whose rt lacks the layer, or with any other folder beside it, is still refused.
void an_mtp_draft_layer_folder_is_made_private() {
  const auto root = secure_frontend_test::make_scratch_directory();
  const auto widen = [](const std::filesystem::path& folder) {
#if defined(_WIN32)
    expect(secure_frontend_test::grant_everyone_write(folder), "the test widens the folder");
#else
    ::chmod(folder.c_str(), 0777);
#endif
  };
  const auto layer = [](const std::filesystem::path& rt) {
    secure_frontend_test::make_private_directory(rt);
    for (const char* file : {"dense.bin", "experts.bin", "draft_vocab.bin", "dense.txt"}) secure_frontend_test::write_file(rt / file, "x");
  };
  const auto rt = root / "rt-only" / "rt";
  secure_frontend_test::make_private_directory(root / "rt-only");
  layer(rt);
  widen(rt);
  const auto made_rt = secure_frontend::make_model_folder_private(rt);
  expect(!made_rt.has_value(), "the runtime folder is made private: " + (made_rt ? made_rt->message : std::string{}));
  expect(std::holds_alternative<secure_frontend::CanonicalRoot>(secure_frontend::open_root(rt)), "and trusted afterwards");

  const auto mtp = root / "mtp";
  secure_frontend_test::make_private_directory(mtp);
  layer(mtp / "rt");
  secure_frontend_test::make_private_directory(mtp / "tensors");
  secure_frontend_test::write_file(mtp / "tensors" / "mtp.fc.weight.bin", "x");
  for (const char* file : {"mtp-manifest.json", "mtp-inventory.json", "mtp-q2_0.gguf"}) secure_frontend_test::write_file(mtp / file, "x");
  widen(mtp);
  const auto made_mtp = secure_frontend::make_model_folder_private(mtp);
  expect(!made_mtp.has_value(), "the folder holding it is made private: " + (made_mtp ? made_mtp->message : std::string{}));
  expect(std::holds_alternative<secure_frontend::CanonicalRoot>(secure_frontend::open_root(mtp)) &&
             std::holds_alternative<secure_frontend::CanonicalRoot>(secure_frontend::open_root(mtp / "rt")),
         "both trusted afterwards");

  const auto partial = root / "partial";
  secure_frontend_test::make_private_directory(partial);
  secure_frontend_test::make_private_directory(partial / "rt");
  secure_frontend_test::write_file(partial / "rt" / "dense.bin", "x");
  expect(secure_frontend::make_model_folder_private(partial).has_value(), "an rt without the layer is still refused");
  expect(secure_frontend::make_model_folder_private(partial / "rt").has_value(), "and so is that rt alone");
  const auto extra = root / "extra";
  secure_frontend_test::make_private_directory(extra);
  layer(extra / "rt");
  secure_frontend_test::make_private_directory(extra / "photos");
  expect(secure_frontend::make_model_folder_private(extra).has_value(), "another folder beside the layer is still refused");
}

// §4.3: a download's destination on a drive whose root others may write (the owner's F:) - a new folder there is
// owner-only from the start (others cannot rename or delete it), and only a new or already-private one is answered.
void a_private_folder_is_made_in_a_shared_one() {
  const auto shared = secure_frontend_test::make_scratch_directory() / "drive";
  secure_frontend_test::make_private_directory(shared);
#if defined(_WIN32)
  expect(secure_frontend_test::grant_everyone_write(shared), "the test widens the drive folder");
#else
  ::chmod(shared.c_str(), 01777);   // world-writable with the sticky bit, as /tmp
#endif
  const auto made = secure_frontend::make_private_folder(shared, "strata models");
  const auto* path = std::get_if<std::filesystem::path>(&made);
  expect(path != nullptr && std::holds_alternative<secure_frontend::CanonicalRoot>(secure_frontend::open_root(shared / "strata models")),
         "a new owner-only folder: " + (path == nullptr ? std::get<secure_frontend::FolderError>(made).message : std::string{}));
  expect(std::holds_alternative<std::filesystem::path>(secure_frontend::make_private_folder(shared, "strata models")),
         "the same folder again is answered");
  secure_frontend_test::make_private_directory(shared / "open");
#if defined(_WIN32)
  (void)secure_frontend_test::grant_everyone_write(shared / "open");
#else
  ::chmod((shared / "open").c_str(), 0777);
#endif
  expect(std::holds_alternative<secure_frontend::FolderError>(secure_frontend::make_private_folder(shared, "open")),
         "an existing folder others may write is not taken");
  for (const std::string& bad : std::vector<std::string>{"", "..", ".hidden", "a/b", "a\\b", "trailing.", "x:y", std::string(65, 'a')}) {
    expect(std::holds_alternative<secure_frontend::FolderError>(secure_frontend::make_private_folder(shared, bad)), "a bad name: " + bad);
  }
  expect(std::holds_alternative<secure_frontend::FolderError>(secure_frontend::make_private_folder(shared / "absent", "x")),
         "the folder it goes in must be there");
#if !defined(_WIN32)
  const auto open_parent = secure_frontend_test::make_scratch_directory() / "open-parent";
  secure_frontend_test::make_private_directory(open_parent);
  ::chmod(open_parent.c_str(), 0777);   // anyone may rename what is in it
  expect(std::holds_alternative<secure_frontend::FolderError>(secure_frontend::make_private_folder(open_parent, "x")),
         "not inside a folder anyone may rename entries of");
#endif
}

}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"volumes_are_the_local_drives", volumes_are_the_local_drives},
      {"a_folder_lists_its_folders_and_gguf_files", a_folder_lists_its_folders_and_gguf_files},
      {"a_model_folder_is_made_private", a_model_folder_is_made_private},
      {"a_model_folder_with_its_pack_is_made_private", a_model_folder_with_its_pack_is_made_private},
      {"an_mtp_draft_layer_folder_is_made_private", an_mtp_draft_layer_folder_is_made_private},
      {"a_private_folder_is_made_in_a_shared_one", a_private_folder_is_made_in_a_shared_one},
  }, failures);
}
