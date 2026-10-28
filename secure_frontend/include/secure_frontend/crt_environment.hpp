#pragma once

#include <cstdlib>

namespace secure_frontend {

// The C runtime's own environment entry for `name` — a pointer into the CRT's
// copy, not a duplicate — or nullptr. MSVC deprecates getenv/_wgetenv (C4996) in
// favour of _dupenv_s/_wdupenv_s, which return a fresh copy: wrong where the
// caller must wipe the original in place (token_reader.cpp, CFG-05), and noise
// for a plain read. The deprecation is silenced here, in one place, and nowhere
// else (session log 2026-10-21, issue 9).
inline char* crt_getenv(const char* name) noexcept {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
  return std::getenv(name);
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}

#if defined(_WIN32)
// The wide twin, for the CRT's wide environment copy (Windows only).
inline wchar_t* crt_wgetenv(const wchar_t* name) noexcept {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
  return _wgetenv(name);
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}
#endif

}  // namespace secure_frontend
