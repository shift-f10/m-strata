#include "secure_frontend/model_inventory.hpp"

#include "secure_frontend/canonical_path.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <variant>

namespace secure_frontend {
namespace {

[[nodiscard]] bool is_ascii_alphanumeric(const char value) {
  return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
         (value >= '0' && value <= '9');
}

[[nodiscard]] bool contains_ascii_control(const std::string_view value) {
  return std::any_of(value.begin(), value.end(), [](const unsigned char character) {
    return character <= 0x1FU || character == 0x7FU;
  });
}

[[nodiscard]] std::string ascii_lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](const unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  return text;
}

// A file name ending in ".gguf" (any case) with a non-empty stem; the same
// rule std::filesystem::path::extension() gives for portable names.
[[nodiscard]] bool has_gguf_name(const std::string_view name) {
  constexpr std::string_view kExtension{".gguf"};
  return name.size() > kExtension.size() &&
         ascii_lower(std::string{name.substr(name.size() - kExtension.size())}) == kExtension;
}

// Entry names come from the file system, not from configuration; a
// non-portable one may hold control bytes or invalid UTF-8, so only printable
// ASCII is shown verbatim.
[[nodiscard]] std::string printable_name(const std::string_view name) {
  std::string shown;
  for (const unsigned char character : name) {
    shown.push_back(character >= 0x20U && character < 0x7FU ? static_cast<char>(character) : '?');
  }
  return shown;
}

}  // namespace

ModelIdValidation validate_model_id(const std::string_view id) {
  if (id.empty() || id.size() > 128U || !is_ascii_alphanumeric(id.front()) ||
      contains_ascii_control(id)) {
    return ModelInventoryError{ModelInventoryErrorCode::invalid_model_id,
                               "model IDs must start with an ASCII letter or digit and be at most 128 characters"};
  }
  for (const char character : id) {
    if (!(is_ascii_alphanumeric(character) || character == '.' || character == '_' || character == '-')) {
      return ModelInventoryError{ModelInventoryErrorCode::invalid_model_id,
                                 "model IDs may contain only ASCII letters, digits, dot, underscore, and hyphen"};
    }
  }
  if (id.back() == '.' || is_windows_reserved_component(id)) {
    return ModelInventoryError{ModelInventoryErrorCode::invalid_model_id,
                               "model ID is not portable on Windows"};
  }
  return std::string{id};
}

ArtifactPathValidation validate_local_artifact_path(const std::string_view path) {
  const auto parsed = parse_relative_path(path);
  if (const auto* valid = std::get_if<RelativePath>(&parsed)) return valid->generic();
  return ModelInventoryError{ModelInventoryErrorCode::invalid_artifact_path,
                             "artifact paths must be portable relative paths below the configured model root"};
}

ModelInventoryResult enumerate_local_models(const CanonicalRoot& model_root) {
  std::vector<ModelManifest> models;
  std::vector<std::optional<RelativePath>> pending{std::nullopt};
  while (!pending.empty()) {
    const auto directory = std::move(pending.back());
    pending.pop_back();
    auto listed = list_directory(model_root, directory);
    if (const auto* failure = std::get_if<PathError>(&listed)) {
      return ModelInventoryError{ModelInventoryErrorCode::inventory_read_failed,
                                 "unable to list " + (directory ? directory->generic() : std::string{"the model root"}) +
                                     " (" + failure->message + ")"};
    }
    for (const auto& entry : std::get<std::vector<DirectoryEntry>>(listed)) {
      // SP4 §4.9 delta a: roots.models/.staging, .lock and every other dot entry at the root
      // belong to acquisition, never to the inventory.
      if (!directory.has_value() && !entry.name.empty() && entry.name.front() == '.') continue;
      const std::string relative_text = directory ? directory->generic() + "/" + entry.name : entry.name;
      const auto relative = parse_relative_path(relative_text);
      const auto* path = std::get_if<RelativePath>(&relative);
      const std::string shown = path != nullptr ? path->generic() : printable_name(relative_text);
      if (entry.type == EntryType::symlink) {
        return ModelInventoryError{ModelInventoryErrorCode::symbolic_link_not_allowed,
                                   "symbolic link or junction in the model inventory: " + shown +
                                       "; point roots.models at the other disk instead (the root itself may be a link)"};
      }
      const bool is_directory = entry.type == EntryType::directory;
      const bool model_file = !is_directory && has_gguf_name(entry.name);
      if (path == nullptr) {
        // Controller ruling: a directory (which must be descended) or a model
        // file with a non-portable name fails; any other non-portable file is
        // ignored.
        if (!is_directory && !model_file) continue;
        return ModelInventoryError{ModelInventoryErrorCode::invalid_artifact_path,
                                   std::string{is_directory ? "directory" : "model file"} + " name is not portable: " +
                                       shown + " (" + std::get<PathError>(relative).message + "); rename it"};
      }
      if (entry.type == EntryType::directory) {
        pending.push_back(*path);
        continue;
      }
      if (entry.type != EntryType::regular_file || !model_file) continue;
      const std::string id = entry.name.substr(0U, entry.name.size() - 5U);  // the stem: name without ".gguf"
      const auto validated = validate_model_id(id);
      if (const auto* invalid = std::get_if<ModelInventoryError>(&validated)) {
        return ModelInventoryError{invalid->code, shown + ": " + invalid->message + "; rename the file"};
      }
      models.push_back(ModelManifest{.id = id, .relative_artifact_path = path->generic(), .artifact_size_bytes = entry.size});
    }
  }
  // IDs differing only in ASCII case are duplicates: a Windows client (and a
  // case-insensitive file system) cannot tell them apart.
  std::sort(models.begin(), models.end(), [](const ModelManifest& left, const ModelManifest& right) {
    const auto left_folded = ascii_lower(left.id);
    const auto right_folded = ascii_lower(right.id);
    return left_folded != right_folded ? left_folded < right_folded : left.id < right.id;
  });
  for (std::size_t index = 1U; index < models.size(); ++index) {
    if (ascii_lower(models[index - 1U].id) == ascii_lower(models[index].id)) {
      return ModelInventoryError{ModelInventoryErrorCode::duplicate_model_id,
                                 "model files " + models[index - 1U].relative_artifact_path + " and " +
                                     models[index].relative_artifact_path +
                                     " resolve to the same model ID (IDs are compared ignoring case); rename or remove one"};
    }
  }
  std::sort(models.begin(), models.end(), [](const ModelManifest& left, const ModelManifest& right) {
    return left.id < right.id;
  });
  return models;
}

ModelInventoryResult enumerate_local_models(const std::filesystem::path& model_root) {
  auto root = open_root(model_root);
  if (const auto* failure = std::get_if<PathError>(&root)) {
    return ModelInventoryError{failure->code == PathErrorCode::not_directory ? ModelInventoryErrorCode::model_root_not_directory
                                                                             : ModelInventoryErrorCode::model_root_unavailable,
                               "model root: " + failure->message};
  }
  return enumerate_local_models(std::get<CanonicalRoot>(root));
}

}  // namespace secure_frontend
