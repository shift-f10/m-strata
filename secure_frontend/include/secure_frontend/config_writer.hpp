#pragma once

// The page's settings routes (email alerts, web search, model folders): the server's config file rewritten with some of
// its rows changed - read, changed, rendered and written owner-only over itself in its own folder. The folder must be
// private: the page never writes a config file other users of this PC could change. The caller serializes its writes.

#include "secure_frontend/frontend_config.hpp"

#include <filesystem>
#include <functional>
#include <optional>
#include <string_view>

namespace secure_frontend {

enum class ConfigUpdateFailure {
  unreadable,   // the file does not read as a config
  not_written,  // its folder is not private (or the write failed)
};

[[nodiscard]] std::optional<ConfigUpdateFailure> update_config_file(const std::filesystem::path& file,
                                                                    const std::function<void(FrontendConfigDraft&)>& change);

// The page's fixed reason for each.
[[nodiscard]] std::string_view config_update_reason(ConfigUpdateFailure failure) noexcept;

}  // namespace secure_frontend
