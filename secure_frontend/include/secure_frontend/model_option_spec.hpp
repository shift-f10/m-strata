#pragma once

// A model card's option as a family declares it (model_options.hpp reads and writes them; family_descriptor.hpp holds
// each family's table): plain data, so the descriptors sit under the config layer.

#include <cstdint>
#include <span>
#include <string_view>

namespace secure_frontend {

// integer_or_auto: the string "auto" or an integer. devices: "auto" or GPU numbers 0-63 separated by commas, none
// repeated ("0,1"), at most `maximum` of them (multi-GPU, 2026-10-28).
enum class ModelOptionKind { boolean, integer, decimal, choice, integer_or_auto, devices };

// One option of an engine profile with the engine's hard bounds.
struct ModelOptionSpec final {
  std::string_view name;   // the card key and the API name: [a-z_]+
  ModelOptionKind kind;
  std::string_view label;  // the page's label
  std::string_view help;   // one fixed sentence for the page
  std::int64_t minimum{0};   // integer and integer_or_auto
  std::int64_t maximum{0};
  double minimum_decimal{0.0};  // decimal (inclusive)
  double maximum_decimal{0.0};
  std::span<const std::string_view> choices{};  // choice
  // Integer kinds (owner request 2026-10-23, the page's dropdowns): the only values offered,
  // ascending; the card's min/max trim the list. Empty: any integer within the bounds.
  std::span<const std::int64_t> values{};
  std::string_view zero_label{};  // the label of 0 among `values` ("Off"); empty: "0"
  std::string_view at_most{};     // another option this one may not exceed (KV streaming: the context)
};

}  // namespace secure_frontend
