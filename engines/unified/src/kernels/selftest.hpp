// strata-unified-engine: GPU kernel self-checks (the GPU box only; STRATA_KERNEL_SELFTEST=1 runs them and exits).
// Host-visible declarations only -- no CUDA headers -- so u_main.cpp can call them.
#pragma once

#include <string>

namespace u::k {

// The multi-column kernels' contract for the speculative verify window: every column of an NC-column call is
// BITWISE the single-column call on that column, and the result agrees with the reference kernel within float
// tolerance. Appends one line per checked shape to `report`; false on any mismatch.
[[nodiscard]] bool selftest_kernels(std::string& report);

}  // namespace u::k
