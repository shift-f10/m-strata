#include "secure_frontend/config_writer.hpp"

#include "secure_frontend/canonical_path.hpp"
#include "secure_frontend/config_file.hpp"
#include "secure_frontend/token_store.hpp"

#include <variant>

namespace secure_frontend {

std::optional<ConfigUpdateFailure> update_config_file(const std::filesystem::path& file,
                                                      const std::function<void(FrontendConfigDraft&)>& change) {
  auto draft = read_config_file(file);
  if (!std::holds_alternative<FrontendConfigDraft>(draft)) return ConfigUpdateFailure::unreadable;
  change(std::get<FrontendConfigDraft>(draft));
  auto folder = open_root(file.parent_path(), RootPolicy::existing);
  const auto name = parse_relative_path(path_to_utf8(file.filename()));
  if (!std::holds_alternative<CanonicalRoot>(folder) || !std::holds_alternative<RelativePath>(name) ||
      write_private_file(std::get<CanonicalRoot>(folder), std::get<RelativePath>(name),
                         render_config_document(std::get<FrontendConfigDraft>(draft)), TokenStoreMode::replace)) {
    return ConfigUpdateFailure::not_written;
  }
  return std::nullopt;
}

std::string_view config_update_reason(const ConfigUpdateFailure failure) noexcept {
  switch (failure) {
    case ConfigUpdateFailure::unreadable: return "the config file could not be read";
    case ConfigUpdateFailure::not_written: return "the config file's folder is not private, so the page does not write it; edit it by hand";
  }
  return "the config file could not be written";  // unreachable
}

}  // namespace secure_frontend
