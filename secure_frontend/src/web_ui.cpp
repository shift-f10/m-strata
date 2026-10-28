#include "secure_frontend/web_ui.hpp"

namespace secure_frontend {

const WebAsset* find_web_asset(const std::string_view target) noexcept {
  for (const auto& asset : web_ui_assets()) {
    if (asset.target == target) return &asset;
  }
  return nullptr;
}

}  // namespace secure_frontend
