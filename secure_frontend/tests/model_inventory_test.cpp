#include "secure_frontend/model_inventory.hpp"
#include "support/test_support.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>
#include <variant>

namespace {

int failures = 0;

void expect(const bool condition, const std::string& message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
  }
}

template <typename Result>
bool has_error(const Result& result, const secure_frontend::ModelInventoryErrorCode expected) {
  const auto* error = std::get_if<secure_frontend::ModelInventoryError>(&result);
  return error != nullptr && error->code == expected;
}

std::filesystem::path unique_temp_directory() {
  return secure_frontend_test::make_scratch_directory();
}

// Creates each missing directory one level at a time, owner-only (0700 /
// protected DACL), below an existing scratch root.
void make_private_directories(const std::filesystem::path& directory) {
  if (directory.empty() || std::filesystem::exists(directory)) return;
  make_private_directories(directory.parent_path());
  secure_frontend_test::make_private_directory(directory);
}

void write_file(const std::filesystem::path& path, const std::string_view contents) {
  make_private_directories(path.parent_path());
  std::ofstream stream(path, std::ios::binary);
  stream << contents;
}

void validates_portable_model_ids() {
  for (const auto id : {"Qwen3-30B-A3B-Q4_K_M", "llama.3.3", "Model1"}) {
    const auto result = secure_frontend::validate_model_id(id);
    expect(std::holds_alternative<std::string>(result), "expected valid model ID: " + std::string{id});
  }
  for (const auto id : {"", ".hidden", "model/other", "model\\other", "model:ads", "model ",
                        "NUL", "COM1", "model.", "model\nnext"}) {
    expect(has_error(secure_frontend::validate_model_id(id),
                     secure_frontend::ModelInventoryErrorCode::invalid_model_id),
           "expected invalid model ID: " + std::string{id});
  }
}

void validates_portable_artifact_paths() {
  const auto valid = secure_frontend::validate_local_artifact_path("qwen/Qwen3-30B-Q4_K_M.gguf");
  expect(std::holds_alternative<std::string>(valid), "valid nested artifact path should be accepted");
  for (const auto path : {"", "../model.gguf", "models/../model.gguf", "/models/model.gguf",
                          "C:\\models\\model.gguf", "\\\\server\\share\\model.gguf",
                          "model.gguf:ads", "models//model.gguf", "NUL/model.gguf",
                          "models./model.gguf", "models /model.gguf"}) {
    expect(has_error(secure_frontend::validate_local_artifact_path(path),
                     secure_frontend::ModelInventoryErrorCode::invalid_artifact_path),
           "expected invalid artifact path: " + std::string{path});
  }
}

void inventories_only_local_gguf_artifacts() {
  const auto root = unique_temp_directory();
  write_file(root / "zeta.gguf", "abc");
  write_file(root / "nested" / "Alpha-Q4_K_M.gguf", "abcdef");
  write_file(root / "notes.txt", "not a model");
  const auto result = secure_frontend::enumerate_local_models(root);
  const auto* models = std::get_if<std::vector<secure_frontend::ModelManifest>>(&result);
  expect(models != nullptr, "regular local GGUF artifacts should inventory without networking");
  if (models != nullptr) {
    expect(models->size() == 2U, "only GGUF artifacts should be returned");
    expect((*models)[0].id == "Alpha-Q4_K_M" && (*models)[0].object == "model" &&
               (*models)[0].owned_by == "local" &&
               (*models)[0].relative_artifact_path == "nested/Alpha-Q4_K_M.gguf" &&
               (*models)[0].artifact_size_bytes == 6U,
           "manifest should have stable local /v1/models-compatible metadata");
    expect((*models)[1].id == "zeta", "inventory should sort models by stable ID");
  }
  std::filesystem::remove_all(root);
}

void rejects_duplicate_ids() {
  const auto duplicate_root = unique_temp_directory();
  write_file(duplicate_root / "same.gguf", "a");
  write_file(duplicate_root / "nested" / "same.gguf", "b");
  expect(has_error(secure_frontend::enumerate_local_models(duplicate_root),
                   secure_frontend::ModelInventoryErrorCode::duplicate_model_id),
         "duplicate filename-derived IDs must fail closed");
  std::filesystem::remove_all(duplicate_root);
}

// Spec §4.9 delta a.
void dl04_staging_ignored_by_inventory_scan() {
  const auto root = unique_temp_directory();
  write_file(root / "real.gguf", "a");
  write_file(root / ".staging" / "0123456789abcdef0123456789abcdef" / "weights" / "partial.gguf.part", "b");
  write_file(root / ".staging" / "0123456789abcdef0123456789abcdef" / "weights" / "done.gguf", "c");
  write_file(root / ".lock", "");
  const auto result = secure_frontend::enumerate_local_models(root);
  const auto* models = std::get_if<std::vector<secure_frontend::ModelManifest>>(&result);
  expect(models != nullptr && models->size() == 1 && models->front().id == "real",
         "staging, the lock and every other dot entry at the root are skipped");
  std::filesystem::remove_all(root);
}

#if !defined(_WIN32)
void cfg03_inventory_rejects_symlink_entry() {
  const auto root = unique_temp_directory();
  write_file(root / "real.gguf", "a");
  std::filesystem::create_symlink(root / "real.gguf", root / "linked.gguf");
  expect(has_error(secure_frontend::enumerate_local_models(root),
                   secure_frontend::ModelInventoryErrorCode::symbolic_link_not_allowed),
         "inventory must reject, not follow, symbolic links");
  std::filesystem::remove_all(root);  // removes the link, not its target
}

// Review Focus 1: a model kept on another disk via a symlinked file.
void inventory_symlink_error_names_entry() {
  const auto root = unique_temp_directory();
  const auto other_disk = unique_temp_directory();
  write_file(other_disk / "x.gguf", "big");
  secure_frontend_test::make_private_directory(root / "big");
  std::filesystem::create_symlink(other_disk / "x.gguf", root / "big" / "x.gguf");
  const auto result = secure_frontend::enumerate_local_models(root);
  const auto* error = std::get_if<secure_frontend::ModelInventoryError>(&result);
  expect(error != nullptr && error->code == secure_frontend::ModelInventoryErrorCode::symbolic_link_not_allowed,
         "the symlinked file fails the inventory");
  if (error != nullptr) {
    expect(error->message.find("big/x.gguf") != std::string::npos, "the error names the entry: " + error->message);
    expect(error->message.find("roots.models") != std::string::npos, "and says what to do instead: " + error->message);
  }
  std::filesystem::remove_all(root);
  std::filesystem::remove_all(other_disk);
}

// Review Focus 5: two names a Windows client cannot tell apart.
void inventory_duplicate_detection_is_case_insensitive() {
  const auto root = unique_temp_directory();
  write_file(root / "Qwen.gguf", "a");
  write_file(root / "qwen.gguf", "b");
  expect(has_error(secure_frontend::enumerate_local_models(root), secure_frontend::ModelInventoryErrorCode::duplicate_model_id),
         "IDs differing only in case are duplicates");
  std::filesystem::remove_all(root);
}

// Controller ruling: an entry whose name fails the relative-path grammar is
// ignored unless it is a .gguf model file; a non-portable .gguf name fails the
// inventory and the error names the entry.
void inventory_ignores_non_portable_non_model_names() {
  const auto root = unique_temp_directory();
  write_file(root / "good.gguf", "a");
  write_file(root / "README~1", "8.3 alias look-alike");
  write_file(root / "notes:txt", "colon");
  const auto result = secure_frontend::enumerate_local_models(root);
  const auto* models = std::get_if<std::vector<secure_frontend::ModelManifest>>(&result);
  expect(models != nullptr && models->size() == 1U && (*models)[0].id == "good",
         "non-model entries with non-portable names are ignored");
  write_file(root / "bad:name.gguf", "b");
  const auto failed = secure_frontend::enumerate_local_models(root);
  const auto* error = std::get_if<secure_frontend::ModelInventoryError>(&failed);
  expect(error != nullptr && error->code == secure_frontend::ModelInventoryErrorCode::invalid_artifact_path,
         "a model file with a non-portable name fails the inventory");
  if (error != nullptr) {
    expect(error->message.find("bad:name.gguf") != std::string::npos, "the error names the entry: " + error->message);
    expect(error->message.find("rename") != std::string::npos, "and says what to do: " + error->message);
  }
  std::filesystem::remove_all(root);
}

// Fix round 1 ruling: a directory must be descended, so a non-portable
// directory name fails the inventory instead of hiding the models below it.
void inventory_rejects_non_portable_directory() {
  const auto root = unique_temp_directory();
  write_file(root / "bad:dir" / "x.gguf", "a");
  const auto result = secure_frontend::enumerate_local_models(root);
  const auto* error = std::get_if<secure_frontend::ModelInventoryError>(&result);
  expect(error != nullptr && error->code == secure_frontend::ModelInventoryErrorCode::invalid_artifact_path,
         "a non-portable directory fails the inventory");
  if (error != nullptr) {
    expect(error->message.find("bad:dir") != std::string::npos, "the error names the directory: " + error->message);
    expect(error->message.find("rename") != std::string::npos, "and says what to do: " + error->message);
  }
  std::filesystem::remove_all(root);
}
#else
void cfg03_inventory_rejects_junction_entry() {
  const auto root = unique_temp_directory();
  const auto outside = unique_temp_directory();
  write_file(outside / "far.gguf", "a");
  expect(secure_frontend_test::create_junction(root / "j", outside), "junction created");
  expect(has_error(secure_frontend::enumerate_local_models(root),
                   secure_frontend::ModelInventoryErrorCode::symbolic_link_not_allowed),
         "a junction inside the models root fails the inventory");
  std::error_code ignored;
  std::filesystem::remove(root / "j", ignored);  // the junction itself first, never its target's contents
  std::filesystem::remove_all(root, ignored);
  std::filesystem::remove_all(outside, ignored);
}
#endif

}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"validates_portable_model_ids", validates_portable_model_ids},
      {"validates_portable_artifact_paths", validates_portable_artifact_paths},
      {"inventories_only_local_gguf_artifacts", inventories_only_local_gguf_artifacts},
      {"rejects_duplicate_ids", rejects_duplicate_ids},
      {"dl04_staging_ignored_by_inventory_scan", dl04_staging_ignored_by_inventory_scan},
#if !defined(_WIN32)
      {"cfg03_inventory_rejects_symlink_entry", cfg03_inventory_rejects_symlink_entry},
      {"inventory_symlink_error_names_entry", inventory_symlink_error_names_entry},
      {"inventory_duplicate_detection_is_case_insensitive", inventory_duplicate_detection_is_case_insensitive},
      {"inventory_ignores_non_portable_non_model_names", inventory_ignores_non_portable_non_model_names},
      {"inventory_rejects_non_portable_directory", inventory_rejects_non_portable_directory},
#else
      {"cfg03_inventory_rejects_junction_entry", cfg03_inventory_rejects_junction_entry},
#endif
  }, failures);
}
