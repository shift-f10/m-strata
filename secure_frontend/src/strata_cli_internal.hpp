#pragma once
// Shared between strata_cli.cpp (local verbs, Task 23) and strata_cli_network.cpp
// (network verbs, Task 27), so neither re-defines the argument parser or the
// error/usage printers (F-57). Not installed; not part of the public interface.

#include "secure_frontend/strata_cli.hpp"

#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace secure_frontend {

// Network verbs (pull, run, update --models, doctor), in strata_cli_network.cpp (Task 27).
int run_network_verb(const std::string& verb, const FrontendConfig& config, const std::filesystem::path& config_file,
                     std::span<const std::string> rest, CliStreams& io, CliServices& services);

namespace strata_cli_detail {

extern const std::string_view kUsage;

struct Args final {
  std::vector<std::string> positional;
  std::set<std::string> flags;
  std::map<std::string, std::string> options;
};

// Flags are --name; options are --name <value>. Any other "--…" argument, or an option
// without its value, is a usage error (nullopt).
[[nodiscard]] std::optional<Args> parse_args(std::span<const std::string> args, const std::set<std::string>& flags,
                                             const std::set<std::string>& options);

// "strata: <problem>" and the usage text on err; exit 1.
int usage(CliStreams& io, std::string_view problem);

// format_error on err (fixed reason + §8.1 detail) plus fixed-text hints; returns the §8.3 exit code.
int report(CliStreams& io, const AcquireError& error);

// --trust-manifest sha256:<hex>; `bad` is set (and a message printed) when it is malformed.
[[nodiscard]] std::optional<std::string> trust_option(const Args& args, CliStreams& io, bool& bad);

[[nodiscard]] std::unique_ptr<LoadedVersions> loaded_for(const FrontendConfig& config, CliServices& services);
[[nodiscard]] const StoreEnvironment& store_for(const CliServices& services);
[[nodiscard]] ManifestRules rules_for(const FrontendConfig& config);

// name[:tag] → its index entry (find_installed over read_model_index); a malformed reference is usage_error.
[[nodiscard]] std::variant<IndexEntry, AcquireError> find_installed_text(const CanonicalRoot& models, std::string_view text);

}  // namespace strata_cli_detail
}  // namespace secure_frontend
