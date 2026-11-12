// The shipped samplers implement top-k only in 1..64. Reject unsupported values
// rather than silently changing a requested full-vocabulary/wider distribution.
// This matches the managed frontend's explicit sampling limits. It does not
// change the selected experts, the model logits, or supported sampling values.
#pragma once

#include <charconv>
#include <string_view>
#include <system_error>

namespace multiengine {

inline bool sampling_arguments_supported(int argc, const char* const* argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg.starts_with("--top-k=")) return false; // family CLIs require a separate value
        if (arg != "--top-k") continue;
        if (++i >= argc) return false;
        const std::string_view value(argv[i]);
        int k = 0;
        const auto result = std::from_chars(value.data(), value.data() + value.size(), k);
        if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || k < 1 || k > 64)
            return false;
    }
    return true;
}

} // namespace multiengine
