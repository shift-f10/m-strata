#pragma once

#include <string_view>

namespace secure_frontend {

// SP4 §10.5. The shipped binary is always the hooks-OFF build; the marker lets
// privacy.release_binary_lacks_hook_marker and secure_distribution_check.py
// --release prove that.
#if defined(SECURE_FRONTEND_TEST_HOOKS)
inline constexpr bool kTestHooksEnabled = true;
inline constexpr std::string_view kTestHooksMarker = "SECURE_FRONTEND_TEST_HOOKS_COMPILED_IN";
#else
inline constexpr bool kTestHooksEnabled = false;
#endif

}  // namespace secure_frontend
