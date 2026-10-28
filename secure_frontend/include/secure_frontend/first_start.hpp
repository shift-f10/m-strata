#pragma once

#include "secure_frontend/frontend_config.hpp"

#include <filesystem>
#include <string>
#include <variant>
#include <vector>

namespace secure_frontend {

// GUI-first start-up (docs/superpowers/specs/2026-10-27-gui-first-install-design.md §3, D2): started without --config,
// the server keeps its config, API key, cards and state in a data folder, by default <the executable's folder>/data,
// so moving the install folder moves the whole install.
struct DataFolder final {
  std::filesystem::path root;
  std::filesystem::path config_file;   // root/config.json: an ordinary config file (editable; --config reads others)
  std::filesystem::path secret;        // root/secret: the API key (file "token")
  std::filesystem::path state;         // root/state: roots.state (with cards/, the cards the page and downloads write)
  std::filesystem::path downloads;     // root/downloads: models.downloads_root (the default download folder)
  std::filesystem::path models;        // root/models: models.default_root
  std::filesystem::path logs;          // root/logs
  std::filesystem::path cache;         // root/cache
};
[[nodiscard]] DataFolder data_folder_layout(const std::filesystem::path& root);

// The folder this executable is in (empty when the system does not say).
[[nodiscard]] std::filesystem::path executable_directory();
// The engine an install folder ships (make.py): <install>/engine/multiengine(.exe).
[[nodiscard]] std::filesystem::path bundled_engine(const std::filesystem::path& install_dir);

// The first start's config (§3): launcher mode with no card yet, the listener on 127.0.0.1:8080 with the page, the
// monitor and the one-time login link, the key in secret/token, model control and downloads on, and `engine` as the
// engine of the cards the page writes.
[[nodiscard]] FrontendConfigDraft first_start_config(const DataFolder& folder, const std::filesystem::path& engine);

struct FirstStartResult final {
  bool created{false};
};
// `message` is fixed text naming what failed; it never contains a path.
struct FirstStartError final {
  std::string message;
};

// The data folder ready to serve: with root/config.json present, nothing is touched (created false). Otherwise every
// folder is created owner-only, a new API key is written to secret/token and config.json is written (created true).
// Nothing is ever overwritten.
[[nodiscard]] std::variant<FirstStartResult, FirstStartError> ensure_data_folder(const DataFolder& folder,
                                                                                const std::filesystem::path& engine);

// The install folder made owner-only when it is not (a copied folder keeps its destination's inherited permissions,
// and the server refuses an engine others may write): Windows, a protected DACL for the current user that everything
// below inherits; POSIX, no group or other write on the tree. true when it changed something, false when it was
// owner-only already.
[[nodiscard]] std::variant<bool, FirstStartError> secure_install_folder(const std::filesystem::path& install_dir);

}  // namespace secure_frontend
