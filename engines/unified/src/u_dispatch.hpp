// strata-unified-engine: the family dispatch. One fixed table maps the GGUF
// `general.architecture` string to a FamilyId. Nothing else selects a family; an unknown
// architecture fails closed (the engine trusts only the arch key, which the geometry guard
// then re-checks against that family's expected shape).
#pragma once

#include <optional>
#include <string_view>

namespace u {

// The families this engine dispatches to (design §3.1). The order is stable; it is not a
// wire value.
enum class FamilyId { glm5next, ds4, raon1, qwen35moe, qwen4exp, gemma4, flint };

// The `general.architecture` string for a family, e.g. "raon1".
[[nodiscard]] std::string_view arch_of(FamilyId family);

// A short, stable name for logs and the INFO record (same as the arch string here).
[[nodiscard]] std::string_view name_of(FamilyId family);

// Maps an architecture string to its family via the fixed table; nullopt (fail closed) for
// any string not in the table.
[[nodiscard]] std::optional<FamilyId> family_for_arch(std::string_view arch);

}  // namespace u
