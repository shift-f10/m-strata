// parity_table.hpp — SP5 M1: every option the pinned upstream Strata v0.2.2
// reads has exactly one dispositioned row (spec §3). The row's citation names
// the governing pinned-source read site (`serve/<file>.py:<line>`) and, for
// rejected rows, the spec §4 / C-4 rule that excludes it.
//
// Kinds, bounds, and defaults of typed values are copied from the pin in the
// wiring tasks (C-1); this table carries disposition, destination, citation.

#ifndef SECURE_FRONTEND_PARITY_TABLE_HPP
#define SECURE_FRONTEND_PARITY_TABLE_HPP

#include <cstddef>
#include <span>
#include <string_view>

namespace secure_frontend::parity {

enum class ParityDisposition { typed, rejected, mapped_away, out_of_scope };

enum class ParityDestination {
  frontend_behavior,  // changes what the frontend itself does
  config_only,        // stored typed config, read by frontend logic
  engine_arg,         // composed into the engine's argv
  engine_env,         // typed child-env entry (SP2 allowlist)
  manifest_artifact,  // reaches the engine as an SP4 manifest artifact role
  request_rule,       // shapes request validation/admission
  none,
};

struct ParityRow final {
  std::string_view key;
  ParityDisposition disposition;
  ParityDestination destination;
  std::string_view citation;
  // SP5-P2 Task 1 (C-R, ledger ruling R-T1-e): for a GLM row named by Maya's
  // documented `MAYA_*` spelling, the engine's own name whose row carries the
  // behavior (env_alias.hpp maps MAYA_X to STRATA_X at engine start); empty
  // for every other row.
  std::string_view target{};
};

// The generated inventory of the pin (secure_frontend/tests/parity_keys_upstream.hpp).
[[nodiscard]] std::span<const std::string_view> upstream_config_keys() noexcept;
[[nodiscard]] std::span<const std::string_view> upstream_env_knobs() noexcept;

// Every row of the table, in table order (meta-tests scan this for duplicates
// and for rows that name no inventory key).
[[nodiscard]] std::span<const ParityRow> strata_parity_rows() noexcept;

// The one row for `key`, or nullptr when the table has a gap (or `key` names no
// option of the pin and no extra rule row).
[[nodiscard]] const ParityRow* strata_parity_row(std::string_view key) noexcept;

// ---- GLM profile (SP5 M2): the vendored Project Maya v1.1.21 reference ------
// The same discipline against `engines/maya-ref/` (D-10): every option the
// vendored source reads has exactly one dispositioned row, cited file:line.

// The generated inventory of the vendored Maya reference
// (secure_frontend/tests/parity_keys_maya.hpp).
[[nodiscard]] std::span<const std::string_view> maya_config_keys() noexcept;
[[nodiscard]] std::span<const std::string_view> maya_env_knobs() noexcept;

// Every row of the GLM table, in table order (meta-tests scan this for
// duplicates and for rows that name no inventory key).
[[nodiscard]] std::span<const ParityRow> maya_parity_rows() noexcept;

// The one row for `key` in the GLM table, or nullptr when the table has a gap.
[[nodiscard]] const ParityRow* maya_parity_row(std::string_view key) noexcept;

} // namespace secure_frontend::parity

#endif // SECURE_FRONTEND_PARITY_TABLE_HPP
