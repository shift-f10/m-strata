#include "secure_frontend/catalog.hpp"

#include "secure_frontend/engine_version_generated.hpp"

namespace secure_frontend {

const CatalogEntry* find_catalog_entry(const std::string_view name, std::string_view tag) noexcept {
  if (tag == "default") {
    const CatalogAlias* alias = nullptr;
    for (const auto& a : catalog_aliases()) {
      if (a.name == name && a.alias == "default") alias = &a;
    }
    if (alias == nullptr) return nullptr;
    tag = alias->tag;
  }
  for (const auto& entry : catalog_entries()) {
    if (entry.name == name && entry.tag == tag) return &entry;
  }
  return nullptr;
}

const CatalogEntry* find_catalog_by_digest(const std::string_view sha256) noexcept {
  for (const auto& entry : catalog_entries()) {
    if (entry.sha256 == sha256) return &entry;
  }
  return nullptr;
}

std::string_view built_engine_version_text() noexcept { return generated::kEngineVersionText; }

const EngineVersion& built_engine_version() noexcept {
  // Validated by CMake's regex at configure time, so the parse cannot fail.
  static const EngineVersion version = parse_engine_version(generated::kEngineVersionText).value_or(EngineVersion{});
  return version;
}

}  // namespace secure_frontend
