#pragma once

// Spec G4 / plan Global Constraint "Elevation in tests (no override flag)":
// strata-unified-server refuses an elevated token, and GitHub's Windows runners
// (and any elevated developer shell, and wine) are elevated. So EVERY test
// launch of the server goes through create_non_elevated_process(): a
// LUA-restricted copy of the caller's own token (CreateProcessAsUserW needs no
// extra privilege for a restricted copy of the caller's token), falling back to
// a Safer NORMALUSER token. If neither is non-elevated the launch fails with a
// fixed message: STOP and ask the owner. There is no test-only override flag in
// the server. Users: the SP2 server harness (server_harness.cpp), SP1's
// ChildProcess (process_runner.hpp) and non_elevated_run (the CLI checks).

#if defined(_WIN32)

#include "secure_frontend/win32_prelude.hpp"
#include <winsafer.h>

#include <iostream>
#include <string>
#include <string_view>

namespace secure_frontend_test {

inline bool token_elevated(HANDLE token) {
  TOKEN_ELEVATION elevation{};
  DWORD size = sizeof(elevation);
  return GetTokenInformation(token, TokenElevation, &elevation, size, &size) && elevation.TokenIsElevated != 0;
}

inline HANDLE lua_restricted_token() {
  HANDLE own = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY, &own)) return nullptr;
  HANDLE restricted = nullptr;
  if (!CreateRestrictedToken(own, LUA_TOKEN | DISABLE_MAX_PRIVILEGE, 0, nullptr, 0, nullptr, 0, nullptr, &restricted)) restricted = nullptr;
  CloseHandle(own);
  return restricted;
}

// Fallback when the LUA copy still reports elevation: a Safer NORMALUSER token.
inline HANDLE safer_normal_user_token() {
  SAFER_LEVEL_HANDLE level = nullptr;
  if (!SaferCreateLevel(SAFER_SCOPEID_USER, SAFER_LEVELID_NORMALUSER, SAFER_LEVEL_OPEN, &level, nullptr)) return nullptr;
  HANDLE token = nullptr;
  if (!SaferComputeTokenFromLevel(level, nullptr, &token, 0, nullptr)) token = nullptr;
  (void)SaferCloseLevel(level);
  return token;
}

// A non-elevated primary token, or nullptr.
inline HANDLE non_elevated_token() {
  HANDLE token = lua_restricted_token();
  if (token != nullptr && !token_elevated(token)) return token;
  if (token != nullptr) CloseHandle(token);
  token = safer_normal_user_token();
  if (token != nullptr && !token_elevated(token)) return token;
  if (token != nullptr) CloseHandle(token);
  return nullptr;
}

// CreateProcessAsUserW(non-elevated token, ..., flags | CREATE_SUSPENDED). On
// success the child's own token has been checked non-elevated and its primary
// thread is STILL SUSPENDED: the caller assigns a Job if it wants one, then
// calls ResumeThread. On failure nothing is left running; a token problem is
// reported on stderr as "<who>: ..." with the fixed STOP text.
inline bool create_non_elevated_process(const std::wstring& executable, std::wstring& command_line, const DWORD flags,
                                        void* environment, STARTUPINFOW& startup, PROCESS_INFORMATION& info,
                                        const std::string_view who) {
  HANDLE token = non_elevated_token();
  if (token == nullptr) {
    std::cerr << who
              << ": no non-elevated token for strata-unified-server (LUA and Safer NORMALUSER both elevated); "
                 "STOP and ask the owner -- there is no test-only override\n";
    return false;
  }
  const bool created = CreateProcessAsUserW(token, executable.c_str(), command_line.data(), nullptr, nullptr, TRUE,
                                            flags | CREATE_SUSPENDED, environment, nullptr, &startup, &info) != 0;
  const DWORD create_error = created ? ERROR_SUCCESS : GetLastError();
  CloseHandle(token);
  if (!created) {
    std::cerr << who << ": CreateProcessAsUserW failed (Win32 error " << create_error << ")\n";
    return false;
  }
  HANDLE child_token = nullptr;
  const bool elevated = OpenProcessToken(info.hProcess, TOKEN_QUERY, &child_token) == 0 || token_elevated(child_token);
  if (child_token != nullptr) CloseHandle(child_token);
  if (elevated) {
    std::cerr << who << ": the server token is elevated; STOP and ask the owner\n";
    (void)TerminateProcess(info.hProcess, 1);
    (void)WaitForSingleObject(info.hProcess, 5000);
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    info = PROCESS_INFORMATION{};
    return false;
  }
  return true;
}

}  // namespace secure_frontend_test

#endif  // defined(_WIN32)
