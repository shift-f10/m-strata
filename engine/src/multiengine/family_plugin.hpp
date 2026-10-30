// src/multiengine/family_plugin.hpp - the launcher's engine family plugins (the owner, 2026-10-30: "our engine should
// have a published way so that we or another user can use those guidelines and create a library that we can just add
// later on"; docs/superpowers/specs/2026-10-30-engine-family-plugins-design.md; the ABI: sdk/include/strata/family_abi.h).
//
//   multiengine --family <id> --family-sha256 <hex> <the family's own arguments ...>
//
// loads <engine folder>/../plugins/<id>/multiengine_<id>.dll (libmultiengine_<id>.so; the install's one plugins
// folder, which every plugin kind shares) only when its bytes have that SHA-256 - hashed through a handle that keeps
// the file from changing until it is loaded (Windows: no write sharing; Linux: the loaded file checked to be the open
// one hashed, by device and inode) - and its strata_family_info() says this ABI major and that id; then runs
// strata_family_main() with argv[0] and the family's own arguments.
#pragma once

#include <string>
#include <variant>

namespace multiengine {

struct PluginRequest {
    std::string id;       // [a-z][a-z0-9_]{0,31}
    std::string sha256;   // 64 lowercase hex digits
    int first = 0;        // argv's index of the family's first own argument
};

// argv[1] is --family: the request, or why it is not one (a plain-text reason). Anything else: std::monostate.
std::variant<std::monostate, PluginRequest, std::string> parse_plugin_request(int argc, char** argv);

// A family id: a lowercase letter, then up to 31 lowercase letters, digits or '_' (a folder and file name, never a path).
bool valid_family_id(const std::string& id);

// Checks and runs the plugin, `steps` (the launcher's process steps) applied to the family's own arguments first.
// The plugin's exit code, or 1 after saying on stderr why it was refused.
int run_plugin(const PluginRequest& request, int argc, char** argv, void (*steps)(int, char**));

}  // namespace multiengine
