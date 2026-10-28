#pragma once

#include "secure_frontend/engine_version.hpp"

#include <cstdint>
#include <span>
#include <string_view>

namespace secure_frontend {

// Spec §2.5 rule 1: manifests compiled into the strata binary. `fixture`
// entries exist only in SECURE_FRONTEND_TEST_HOOKS builds.
struct CatalogEntry final {
  std::string_view name;
  std::string_view tag;
  std::uint32_t sequence;
  std::string_view sha256;
  std::string_view bytes;
  bool fixture;
};

struct CatalogAlias final {
  std::string_view name;
  std::string_view alias;  // "default"
  std::string_view tag;
};

[[nodiscard]] std::span<const CatalogEntry> catalog_entries() noexcept;  // generated
[[nodiscard]] std::span<const CatalogAlias> catalog_aliases() noexcept;  // generated
[[nodiscard]] const CatalogEntry* find_catalog_entry(std::string_view name, std::string_view tag) noexcept;
[[nodiscard]] const CatalogEntry* find_catalog_by_digest(std::string_view sha256) noexcept;

// kEngineVersion (spec §2.3): the committed upstream.version, checked at configure time against a reachable submodule tag.
[[nodiscard]] std::string_view built_engine_version_text() noexcept;
[[nodiscard]] const EngineVersion& built_engine_version() noexcept;

}  // namespace secure_frontend
