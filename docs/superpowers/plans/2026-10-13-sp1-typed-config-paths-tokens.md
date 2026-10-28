# SP1 Typed Configuration, Canonical Paths and Tokens Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give `secure_frontend` one immutable, schema-validated configuration model, one canonical-path module with handle-based root containment on Linux and Windows, and CSPRNG tokens stored owner-only (Linux) or DPAPI-wrapped under a private DACL (Windows), and land the `NET-01..03`, `AUTH-01..04` and `CFG-01..06` acceptance tests by ID while fixing defects (a)–(e) and the hostname defect.

**Architecture:** Seven new libraries inside `secure_frontend/`: `secure_frontend_json` (one strict bounded JSON reader shared by the chat schema, the tokenizer pack and the config file), `secure_frontend_typed_config` (schema table, draft → `validate()` → immutable `FrontendConfig`), `secure_frontend_paths` (`RelativePath` grammar, `CanonicalRoot`, `open_beneath` with `openat2` + fallback on Linux and per-component reparse/final-path checks on Windows), `secure_frontend_secrets` (`SecretBytes`, CSPRNG, base64url, fixed-window `TokenVerifier`), `secure_frontend_token_store` (atomic owner-only / DPAPI writes), `secure_frontend_subsystem_limits` (config → listener/parser/chat limits) and `secure_frontend_server_cli` (flag→key mapping, `--config`, `--check`, `init`). Existing modules call these instead of their private checks; `strata-secure-server` validates everything before it binds or spawns.

**Tech Stack:** C++20 (g++ 11.4 on the dev host, clang in CI), CMake 4.4.3 + Ninja 1.13.2 from `.venv-dev/`, llvm-mingw 20261006 (clang 23.1.3) cross builds, wine 6.0.3 (smoke only), glibc 2.35 / Linux 5.15 (`openat2`, `getrandom`, `explicit_bzero`, `renameat2`), Win32 (`bcrypt`, `crypt32`, `advapi32`), GitHub Actions `windows-msys2-clang` as the authoritative Windows runtime.

**Spec:** `docs/superpowers/specs/2026-10-13-sp1-typed-config-paths-tokens-design.md` (APPROVED; open questions 1–5 resolved per their recommendations). Sibling: `docs/superpowers/specs/2026-10-13-product-experience-and-upstream-tracking-design.md` (§3 paths, §4 config model, §11). Fact base: `docs/superpowers/research/sp1-config-paths-tokens.md`.

## Global Constraints

- **Execution environment.** Tasks 1 and 3–24 run in the git worktree `/opt/projects/ai/.worktrees/sp1` on branch `sp1-typed-config-paths-tokens` (spec §8). The worktree and branch already exist; do not create or switch branches. `.venv-dev` and `.toolchains` are symlinks in the worktree and show as untracked (`?? .toolchains`, `?? .venv-dev`); they are never added. Every git command acts on the checkout it is run from: `git -C "$(git rev-parse --show-toplevel)" …` (from `secure_frontend/`, `git -C .. …` is equivalent). Nothing in this plan names `/opt/projects/ai` as a git directory.
- **Task 2 runs first, on `main`.** Task 2 is a shared pre-task consumed by SP2 (see Review Focus 6). It is executed in the main checkout `/opt/projects/ai` on branch `main` before any other SP1 task, and its commit lands on `main`. Then the SP1 worktree rebases onto it: `git -C /opt/projects/ai/.worktrees/sp1 rebase main` (the branch has no commits yet, so this is a fast-forward), and Task 1 and Tasks 3–24 follow in the worktree in number order. Task 1 therefore registers its new cases with Task 2's helper.
- Every shell starts with `cd /opt/projects/ai/.worktrees/sp1/secure_frontend && export PATH="$(git rev-parse --show-toplevel)/.venv-dev/bin:$PATH"` (Task 2: `cd /opt/projects/ai/secure_frontend` instead) and works in `secure_frontend/` unless a step says otherwise. Python tests run from the worktree root as `"$(git rev-parse --show-toplevel)/.venv-dev/bin/pytest" -q`.
- wine in the worktree: every direct `wine …` call and every `ctest --test-dir out/build/windows-llvm-mingw-cross …` call is prefixed with `WINEPREFIX=$PWD/out/wineprefix WINEDLLOVERRIDES=mscoree,mshtml=` (run from `secure_frontend/`). The `windows-llvm-mingw-cross` test preset sets `WINEPREFIX` itself; direct calls do not, and without the prefix wine would create or reuse `~/.wine`.
- Linux build and test: `cmake --preset linux-gcc && cmake --build --preset linux-gcc && ctest --preset linux-gcc` (20/20 before Task 2; per-case, 111/111, after Task 2 and before Task 1).
- Windows cross build: `cmake --preset windows-llvm-mingw-cross && cmake --build --preset windows-llvm-mingw-cross` (with `-Werror`), then `ctest --preset windows-llvm-mingw-cross` under wine (19/19 before Task 2; 102/102 after Task 2 and before Task 1). Task 2 makes that preset exclude the CTest label `windows-runtime` (spec §3.10).
- wine is a smoke test. It does not implement Job Objects, DACLs or DPAPI faithfully; DPAPI under wine may or may not round-trip. For every Windows-only step the RED/GREEN evidence is: cross-compile with `-Werror`, run under wine and **record** the result (pass, fail or skip) in the commit message, and mark the case "pending MSYS2" in the handoff. The `windows-msys2-clang` CI job is authoritative; nothing is claimed as Windows-runtime-verified beyond its logs (spec §9.6).
- No third-party dependencies. Standard library and OS APIs only. New Windows link libraries go into the `if(WIN32)` link block of `secure_frontend/CMakeLists.txt`: `bcrypt` (`BCryptGenRandom`), `crypt32` (DPAPI), `advapi32` (already there for the token reader; the paths library needs it too), `iphlpapi` (test-only, `GetExtendedTcpTable`).
- No change under `upstream/` (the submodule) or our root `CMakeLists.txt` (`secure_frontend/src` and `secure_frontend/include` are this plan's own tree). SP1 touches only `secure_frontend/`, `docs/`, `scripts/dev/`. If a step seems to need anything else: stop and report. No `rm -rf`. Install nothing outside `.venv-dev/` and `.toolchains/`.
- TDD per task: the regression test is written first and fails (RED) before the fix; the commit message names the item IDs and the test names, and says which tests were acceptance tests that passed on first run ("none unless it fails" rows).
- Warnings are errors on every target via `secure_frontend_set_warnings(<target>)` (`-Wall -Wextra -Wpedantic -Werror`; `/W4 /WX`).
- Test style: framework-free `main()` programs with `int failures` and `void expect(bool, const std::string&)`; from Task 2 on every binary dispatches `--case <name>` through `secure_frontend_test::run_cases` and is registered with `secure_frontend_add_cases(<binary> <case>... [ARGS ...] [LABELS ...] [TIMEOUT s])`, which creates one CTest test per case named `<binary>.<case>`. Every later task registers its cases with that helper. Tests that need real Windows semantics carry `LABELS ${SECURE_FRONTEND_WINDOWS_RUNTIME}` (`windows-runtime` on Windows, empty elsewhere).
- Scratch files: tests create directories only through `secure_frontend_test::make_scratch_directory()` (mkdtemp 0700 under `<build>/test-tmp` on POSIX) and `make_private_directory()`; files through `write_file()` (mode 0600 by default). The dev host umask is `0002`, so `std::filesystem::create_directories` makes `0775` directories that the root checks of Task 9 reject.
- In commit-message templates, `<record>` means "paste the wine (or skip) result lines this task's run printed" and `<passed|skipped>` means "write the actual outcome"; nothing else in a template is to be filled in.
- Errors and logs never contain a token, a prompt, an option value, or a configured value; config issues name the key path and a fixed reason (spec §3.3).
- `--check` has no side effects: it opens and inspects, and never creates, writes, renames or deletes anything. `validate()` accepts a logs/cache root that is absent but creatable (its parent opens as a private root); only `serve` (the default command) and `init` create those directories (Task 14). The tests that run `--check` assert the scratch tree is byte-for-byte unchanged afterwards.
- Windows environment scrub (spec §3.6) — accepted deviation, owner-ruled: Windows has no `/proc/<pid>/environ`, and the Win32 environment block cannot be overwritten in place, so Task 20 wipes the CRT copies in place (the narrow one through `getenv`'s pointer, the wide one through `_wgetenv`'s), removes the variable from the CRT with `_putenv_s(name, "")`, and removes the Win32 block entry with `SetEnvironmentVariableW(name, nullptr)`; the entry is removed, not overwritten in place. Task 24 records this in the handoff.
- Commits: local only, never pushed. One commit per task, test and fix together. Before Task 2 (the first task executed) set the trailers once to the exact lines from the harness's attribution reminder in the executing session, e.g.
  `export SP1_TRAILERS=$'Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>\nClaude-Session: <session link>'`
  (copy the session line verbatim from the reminder), and commit with
  `git -C "$(git rev-parse --show-toplevel)" -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit -m "<subject>" -m "<body>" -m "$SP1_TRAILERS"`.
- Scope reconciliation with the product-experience spec: **the `strata` CLI binary (`serve`, `version`, `key show/rotate`, `list`, the banner, `doctor`) is a product-spec deliverable scheduled after SP1/SP2; SP1 exposes the mechanics on `strata-secure-server`** (`--config`, `--check`, `init --config FILE [--force]`, spec §3.9). The product spec owns the config file name/location (`config.json`, XDG / `%APPDATA%`); SP1 never picks a default location.
- Config file format (spec §3.1, product §4): strict JSON, UTF-8 without BOM, no comments, no trailing commas, duplicate keys rejected, nesting depth ≤ 4, file ≤ 64 KiB, integers only where a number is expected. `schema_version` (integer, must be `1`) is required in a file (product §4). Key paths follow the SP1 data model (§4): `listener.*`, `limits.http.*`, `limits.chat.*`, `limits.engine.*`, `limits.concurrency.*`, `roots.*`, `engine.*`; the token source is the single key `token.source` with value `"file:<relative path>"` or `"env:<NAME>"` (product §4 form, SP1 §3.2 semantics). The only environment variable the frontend reads is the one named by `token.source = env:…`.
- Owner decisions (spec §10, resolved): strict JSON; operator tokens shorter than 32 bytes are rejected; the Windows reader accepts a plaintext token under the same private-DACL rules; `localhost` (ASCII case-insensitive) maps to `127.0.0.1` with no resolver call and every other hostname is rejected; symlinks below the models root stay rejected.
- Defaults (spec §4) except one forced change: `limits.chat.max_body_bytes` defaults to **49152** (= 65536 − 16384), because the spec's own cross-field rule `max_body_bytes ≤ max_request_bytes − max_header_bytes` rejects its listed default 262144 with the listed `max_request_bytes` 65536. Chat bodies above ~48 KiB were already impossible (the listener caps a whole request at 64 KiB), so nothing a user can do today stops working.

## Review Focus

1. **Models kept on another disk through a symlinked file** (`models/x.gguf -> /mnt/big/x.gguf`): startup must fail with an error that names the entry and says why and what to do (point `roots.models` at the other disk), not a bare "inventory failed" — pinned in Task 13 (`inventory_symlink_error_names_entry`).
2. **Windows roots under a redirected or synced folder** (OneDrive Known Folder Move, a roaming profile on a share): the handle's final path becomes `\\?\UNC\…`; the error must say the root resolves to a network share or redirected folder and to choose a local folder — pinned in Task 9 (`cfg04_final_path_on_network_share_is_actionable`, a portable test of `classify_final_path`).
3. **A token file edited in Notepad** (UTF-8 BOM, a trailing space, the token pasted with quotes, an extra blank line): each must be rejected with a reason that names the problem, and exactly one LF or CRLF must still be accepted — pinned in Task 18 (`auth02_notepad_edits_rejected_with_reason`).
4. **Non-ASCII user names and paths longer than 260 characters** (`C:\Users\José\…`): roots and relative paths must round-trip UTF-8 ↔ UTF-16 and open through the internal `\\?\` prefix (never accepted as input) — pinned in Task 11 (`paths_handle_non_ascii_and_long_names`, portable; `windows-runtime` on Windows).
5. **Case-folding collisions** (`Qwen.gguf` and `qwen.gguf` on a case-sensitive file system, ext4 casefold or NTFS per-directory case sensitivity): the inventory must fail with `duplicate_model_id` rather than serve two models a Windows client cannot tell apart — pinned in Task 13 (`inventory_duplicate_detection_is_case_insensitive`).
6. **Task 2 is also a SHARED PRE-TASK consumed by SP2.** Its helper signature `secure_frontend_add_cases(<binary> <case>... [ARGS …] [LABELS …] [TIMEOUT n])`, the test naming `<binary>.<case>`, the `windows-runtime` label (`${SECURE_FRONTEND_WINDOWS_RUNTIME}`) and the wine test preset's label exclusion are frozen once landed on `main`; SP2's plan calls them with exactly these names. Task 2 is executed on `main` BEFORE the rest of SP1 (its commit lands on `main`; SP1's worktree rebases onto it — Global Constraints). Later SP1 tasks only add cases and libraries; none of them renames or re-signatures these four things.

---

## File map

| File | Responsibility | Task |
| --- | --- | --- |
| `include/secure_frontend/config.hpp`, `src/config.cpp` | `IpAddress`, `parse_listener_address`, listener `validate_config` (bytes, not strings) | 1 |
| `tests/support/test_support.hpp` | `--case` runner, scratch directories, file helpers (+ Windows DACL/junction helpers from Task 11) | 2, 11 |
| `CMakeLists.txt`, `CMakePresets.json` | `secure_frontend_add_test_executable`, `secure_frontend_add_cases`, `windows-runtime` label, wine filter, new libraries | 2 → all |
| `include/secure_frontend/json.hpp`, `src/json.cpp` | strict bounded JSON reader + string escaper | 3 |
| `include/secure_frontend/config_defaults.hpp` | the one place every tuning default lives | 4, 7 |
| `include/secure_frontend/frontend_config.hpp`, `src/frontend_config.cpp` | schema table, draft, `validate()`, `FrontendConfig`, `describe()` | 4 → 14, 24 |
| `include/secure_frontend/config_file.hpp`, `src/config_file.cpp` | JSON text → draft, file read (trusted from Task 12), merge, render | 4, 5, 12, 21 |
| `include/secure_frontend/server_cli.hpp`, `src/server_cli.cpp` | command line → invocation + draft, usage, `load_server_config` | 5, 21 |
| `src/secure_server_main.cpp` | validate first, bind before spawn, derived limits, verifier, `--check`, `init` | 5–7, 9, 13, 19, 21 |
| `tests/support/process_runner.hpp`, `http_client.hpp`, `config_json.hpp`, `server_fixture.hpp`, `tests/fake_engine_main.cpp` | end-to-end harness | 4, 5 |
| `tests/server_cli_test.cpp`, `tests/server_e2e_test.cpp` | CLI and process-level acceptance tests | 5 → 23 |
| `include/secure_frontend/subsystem_limits.hpp`, `src/subsystem_limits.cpp` | config → `HttpSocketServerLimits`, `ChatExecutionLimits`, `ChatSchemaLimits` | 7 |
| `include/secure_frontend/canonical_path.hpp`, `src/relative_path.cpp`, `src/canonical_path_posix.cpp`, `src/canonical_path_windows.cpp`, `src/canonical_path_internal.hpp`, `src/windows_security.hpp`, `src/windows_security.cpp` | the canonical-path module | 8–14 |
| `include/secure_frontend/secret_bytes.hpp`, `src/secret_bytes.cpp`, `include/secure_frontend/token_generator.hpp`, `src/token_generator.cpp`, `include/secure_frontend/token_verifier.hpp`, `src/token_verifier.cpp` | secrets | 15, 19 |
| `include/secure_frontend/token_store.hpp`, `src/token_store.cpp` | atomic private writes, token store, DPAPI | 16, 17 |
| `include/secure_frontend/frontend_init.hpp`, `src/frontend_init.cpp` | `init` mechanics | 21 |

---

### Task 1: Listener addresses classified from bytes; hostnames rejected (N1, N2)

**Files:**
- Modify: `secure_frontend/include/secure_frontend/config.hpp:1-49` (add `IpAddress`, `parse_listener_address`)
- Replace: `secure_frontend/src/config.cpp` (whole file, 123 lines)
- Modify: `secure_frontend/src/http_socket_server.cpp:257-299` (bind the canonical text the validator produced)
- Modify: `secure_frontend/CMakeLists.txt` (the `secure_frontend_add_cases(secure_frontend_config_tests …)` call Task 2 added)
- Test: `secure_frontend/tests/config_test.cpp` (new cases before `}  // namespace`; the `run_cases` list in `main` that Task 2 wrote; the `service.internal` block of `rejects_invalid_hosts_and_blank_remote_keys` rewritten)

**Order:** executed after Task 2 (which lands on `main` first; Global Constraints), so `config_test.cpp` already has `#include "support/test_support.hpp"` and the `run_cases` `main()`, and line numbers below are after Task 2.

**Interfaces:**
- Consumes: nothing new.
- Produces (used by Tasks 4, 6, 23):
```cpp
namespace secure_frontend {
enum class IpFamily { v4, v6 };
enum class AddressScope { loopback, wildcard, remote };
class IpAddress final {
 public:
  [[nodiscard]] IpFamily family() const noexcept;
  [[nodiscard]] const std::array<unsigned char, 16>& bytes() const noexcept;  // IPv4 uses bytes 0-3
  [[nodiscard]] AddressScope scope() const noexcept;
  [[nodiscard]] const std::string& canonical_text() const noexcept;           // inet_ntop form; bound and logged
};
[[nodiscard]] std::optional<IpAddress> parse_listener_address(std::string_view text);
}
```
  `validate_config()` keeps its signature; `Config::host` now always holds `canonical_text()`. `is_valid_host()` keeps its signature and means "parse_listener_address succeeds".

- [ ] **Step 1: Write the failing tests**

In `tests/config_test.cpp`, replace lines 119-124 (at HEAD 7ddcd755 lines 118-123, shifted by Task 2's include: the `service.internal` half of `rejects_invalid_hosts_and_blank_remote_keys`, which a DNS name can no longer reach) with:

```cpp
  const auto result = secure_frontend::validate_config(
      {.host = std::string{"192.0.2.10"}, .port = std::nullopt, .api_key = std::string{" \t\n"},
       .allow_remote = true});
  const auto* error = error_from(result);
  expect(error != nullptr && error->code == secure_frontend::ConfigErrorCode::api_key_required,
         "blank API key must not permit remote binding");
```

Insert before `}  // namespace` (line 127 after Task 2):

```cpp
// NET-03 / defect (a): every spelling of the unspecified address is a wildcard bind.
const char* const kWildcardSpellings[] = {"0.0.0.0", "::", "::0", "0::0", "0:0:0:0:0:0:0:0", "::0.0.0.0",
                                          "::ffff:0.0.0.0", "0000:0000:0000:0000:0000:0000:0000:0000"};

void net03_wildcard_spellings_need_wildcard_opt_in() {
  for (const char* host : kWildcardSpellings) {
    const auto without = secure_frontend::validate_config(
        {.host = std::string{host}, .port = 8080, .api_key = std::string{"k"}});
    const auto* error = error_from(without);
    expect(error != nullptr && error->code == secure_frontend::ConfigErrorCode::remote_not_enabled,
           std::string{"wildcard without any opt-in must be rejected: "} + host);
    const auto with_both = secure_frontend::validate_config(
        {.host = std::string{host}, .port = 8080, .api_key = std::string{"k"}, .allow_remote = true,
         .allow_wildcard_bind = true});
    const auto* config = config_from(with_both);
    expect(config != nullptr && !config->is_loopback, std::string{"wildcard with both opt-ins is accepted: "} + host);
    if (config != nullptr) {
      expect(config->host == "0.0.0.0" || config->host == "::",
             std::string{"a wildcard binds its canonical text, got "} + config->host);
    }
  }
}

void net03_remote_opt_in_alone_rejects_wildcard() {
  for (const char* host : kWildcardSpellings) {
    const auto result = secure_frontend::validate_config(
        {.host = std::string{host}, .port = 8080, .api_key = std::string{"k"}, .allow_remote = true});
    const auto* error = error_from(result);
    expect(error != nullptr && error->code == secure_frontend::ConfigErrorCode::wildcard_bind_not_enabled,
           std::string{"--allow-remote alone must not allow a wildcard bind: "} + host);
  }
}

void net01_loopback_spellings_classified_from_bytes() {
  const std::pair<const char*, const char*> cases[] = {
      {"127.0.0.1", "127.0.0.1"}, {"127.1.2.3", "127.1.2.3"}, {"::1", "::1"},
      {"0:0:0:0:0:0:0:1", "::1"}, {"0000::0001", "::1"}, {"::ffff:127.0.0.1", "127.0.0.1"}};
  for (const auto& [host, canonical] : cases) {
    const auto result = secure_frontend::validate_config(
        {.host = std::string{host}, .port = 8080, .api_key = std::string{"k"}});
    const auto* config = config_from(result);
    expect(config != nullptr && config->is_loopback, std::string{"loopback spelling must need no opt-in: "} + host);
    if (config != nullptr) expect(config->host == canonical, std::string{"canonical loopback text for "} + host);
  }
}

void net02_hostname_rejected_before_bind() {
  for (const char* host : {"example.com", "service.internal", "my-pc", "localhost.", "localhost.localdomain",
                           "ip6-localhost", "LOCALHOST.example"}) {
    const auto result = secure_frontend::validate_config(
        {.host = std::string{host}, .port = 8080, .api_key = std::string{"k"}, .allow_remote = true});
    const auto* error = error_from(result);
    expect(error != nullptr && error->code == secure_frontend::ConfigErrorCode::invalid_host,
           std::string{"hostnames are rejected at validation, not at bind(): "} + host);
  }
}

void net01_localhost_maps_to_ipv4_loopback() {
  for (const char* host : {"localhost", "LocalHost", "LOCALHOST"}) {
    const auto result = secure_frontend::validate_config(
        {.host = std::string{host}, .port = 8080, .api_key = std::string{"k"}});
    const auto* config = config_from(result);
    expect(config != nullptr && config->is_loopback && config->host == "127.0.0.1",
           std::string{"localhost maps to 127.0.0.1 without a resolver: "} + host);
  }
}
```

Add `#include <utility>` to the includes, and in `main()` append five entries to the `run_cases` list after `{"rejects_invalid_hosts_and_blank_remote_keys", rejects_invalid_hosts_and_blank_remote_keys},`:

```cpp
      {"net03_wildcard_spellings_need_wildcard_opt_in", net03_wildcard_spellings_need_wildcard_opt_in},
      {"net03_remote_opt_in_alone_rejects_wildcard", net03_remote_opt_in_alone_rejects_wildcard},
      {"net01_loopback_spellings_classified_from_bytes", net01_loopback_spellings_classified_from_bytes},
      {"net02_hostname_rejected_before_bind", net02_hostname_rejected_before_bind},
      {"net01_localhost_maps_to_ipv4_loopback", net01_localhost_maps_to_ipv4_loopback},
```

In `CMakeLists.txt`, replace the `secure_frontend_config_tests` registration Task 2 wrote with:

```cmake
    secure_frontend_add_cases(secure_frontend_config_tests
        defaults_to_loopback requires_key_for_loopback requires_explicit_opt_in_for_remote_host
        requires_key_after_remote_opt_in accepts_remote_host_with_opt_in_and_key
        requires_separate_opt_in_for_wildcard_host rejects_invalid_ports rejects_invalid_hosts_and_blank_remote_keys
        net03_wildcard_spellings_need_wildcard_opt_in net03_remote_opt_in_alone_rejects_wildcard
        net01_loopback_spellings_classified_from_bytes net02_hostname_rejected_before_bind
        net01_localhost_maps_to_ipv4_loopback)
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --preset linux-gcc && cmake --build --preset linux-gcc && ctest --preset linux-gcc -R secure_frontend_config_tests --output-on-failure`
Expected: the five new tests `secure_frontend_config_tests.net0*` FAIL with lines such as `FAILED: --allow-remote alone must not allow a wildcard bind: ::0`, `FAILED: loopback spelling must need no opt-in: 0:0:0:0:0:0:0:1`, `FAILED: hostnames are rejected at validation, not at bind(): example.com`, `FAILED: localhost maps to 127.0.0.1 without a resolver: localhost`.

- [ ] **Step 3: Implement**

In `include/secure_frontend/config.hpp` add `#include <array>` and, after `kDefaultPort` (line 12):

```cpp
enum class IpFamily { v4, v6 };
enum class AddressScope { loopback, wildcard, remote };

// A listener address parsed once from text into bytes. Scope and canonical
// text are derived from the bytes, never from the spelling (NET-01..03).
// Built only by parse_listener_address.
class IpAddress final {
 public:
  [[nodiscard]] IpFamily family() const noexcept { return family_; }
  [[nodiscard]] const std::array<unsigned char, 16>& bytes() const noexcept { return bytes_; }
  [[nodiscard]] AddressScope scope() const noexcept { return scope_; }
  // inet_ntop text of the bytes: what is bound and logged. IPv4-mapped IPv6
  // input (::ffff:a.b.c.d) is canonicalized to its IPv4 form.
  [[nodiscard]] const std::string& canonical_text() const noexcept { return text_; }

 private:
  friend std::optional<IpAddress> parse_listener_address(std::string_view text);
  IpAddress() = default;
  IpFamily family_{IpFamily::v4};
  std::array<unsigned char, 16> bytes_{};
  AddressScope scope_{AddressScope::remote};
  std::string text_;
};

// Accepts an IPv4 or IPv6 literal, or exactly "localhost" (ASCII
// case-insensitive), which maps to 127.0.0.1 with no resolver call. Every
// other hostname is rejected: the listener never resolves names.
[[nodiscard]] std::optional<IpAddress> parse_listener_address(std::string_view text);
```

Replace `src/config.cpp` with:

```cpp
#include "secure_frontend/config.hpp"

#include <algorithm>
#include <cstring>
#include <string_view>

#ifdef _WIN32
#include "secure_frontend/win32_prelude.hpp"
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

namespace secure_frontend {
namespace {

[[nodiscard]] bool is_blank(const std::string_view value) {
  return std::all_of(value.begin(), value.end(), [](const unsigned char character) {
    return character == ' ' || character == '\t' || character == '\n' || character == '\r' || character == '\f' ||
           character == '\v';
  });
}

[[nodiscard]] bool is_localhost(const std::string_view text) {
  constexpr std::string_view kLocalhost{"localhost"};
  if (text.size() != kLocalhost.size()) return false;
  for (std::size_t index = 0; index < text.size(); ++index) {
    const char c = text[index];
    const char lowered = c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
    if (lowered != kLocalhost[index]) return false;
  }
  return true;
}

}  // namespace

std::optional<IpAddress> parse_listener_address(const std::string_view text) {
  if (text.empty() || text.size() > 64U || text.find('\0') != std::string_view::npos) return std::nullopt;
  const std::string candidate = is_localhost(text) ? std::string{"127.0.0.1"} : std::string{text};
  IpAddress address;
  in_addr v4{};
  in6_addr v6{};
  if (inet_pton(AF_INET, candidate.c_str(), &v4) == 1) {
    address.family_ = IpFamily::v4;
    std::memcpy(address.bytes_.data(), &v4, 4U);
  } else if (inet_pton(AF_INET6, candidate.c_str(), &v6) == 1) {
    unsigned char raw[16];
    std::memcpy(raw, &v6, 16U);
    const bool v4_mapped = std::all_of(raw, raw + 10, [](const unsigned char b) { return b == 0U; }) &&
                           raw[10] == 0xffU && raw[11] == 0xffU;
    if (v4_mapped) {
      address.family_ = IpFamily::v4;
      std::memcpy(address.bytes_.data(), raw + 12, 4U);
    } else {
      address.family_ = IpFamily::v6;
      std::memcpy(address.bytes_.data(), raw, 16U);
    }
  } else {
    return std::nullopt;
  }
  const auto& b = address.bytes_;
  if (address.family_ == IpFamily::v4) {
    const bool unspecified = b[0] == 0U && b[1] == 0U && b[2] == 0U && b[3] == 0U;
    address.scope_ = b[0] == 127U ? AddressScope::loopback : unspecified ? AddressScope::wildcard : AddressScope::remote;
  } else {
    const bool leading_zero = std::all_of(b.begin(), b.begin() + 15, [](const unsigned char x) { return x == 0U; });
    address.scope_ = leading_zero && b[15] == 0U   ? AddressScope::wildcard   // IN6_IS_ADDR_UNSPECIFIED
                     : leading_zero && b[15] == 1U ? AddressScope::loopback   // IN6_IS_ADDR_LOOPBACK
                                                   : AddressScope::remote;
  }
  char buffer[INET6_ADDRSTRLEN] = {};
  const int family = address.family_ == IpFamily::v4 ? AF_INET : AF_INET6;
  if (inet_ntop(family, address.bytes_.data(), buffer, sizeof(buffer)) == nullptr) return std::nullopt;
  address.text_ = buffer;
  return address;
}

bool is_valid_host(const std::string_view host) { return parse_listener_address(host).has_value(); }

ConfigValidation validate_config(const ConfigInput& input) {
  const std::string host_text = input.host.value_or(std::string{kDefaultHost});
  const std::uint32_t requested_port = input.port.value_or(kDefaultPort);

  const auto address = parse_listener_address(host_text);
  if (!address.has_value()) {
    return ConfigError{ConfigErrorCode::invalid_host, "host must be an IP literal or localhost"};
  }
  if (requested_port == 0 || requested_port > 65535) {
    return ConfigError{ConfigErrorCode::invalid_port, "port must be in the TCP range 1-65535"};
  }
  const bool loopback = address->scope() == AddressScope::loopback;
  if (!loopback && !input.allow_remote) {
    return ConfigError{ConfigErrorCode::remote_not_enabled,
                       "a non-loopback bind requires explicit remote-listening opt-in"};
  }
  if (address->scope() == AddressScope::wildcard && !input.allow_wildcard_bind) {
    return ConfigError{ConfigErrorCode::wildcard_bind_not_enabled,
                       "a wildcard bind requires explicit wildcard-listening opt-in"};
  }
  const bool has_api_key = input.api_key.has_value() && !input.api_key->empty() && !is_blank(*input.api_key);
  if (!has_api_key) {
    return ConfigError{ConfigErrorCode::api_key_required, "a nonempty API key is required for every listener"};
  }
  // The authentication layer, not the long-lived validated configuration,
  // owns the secret. Only evidence that validation saw a non-blank key is kept.
  return Config{address->canonical_text(), static_cast<std::uint16_t>(requested_port), has_api_key, loopback,
                input.allow_remote, input.allow_wildcard_bind};
}

}  // namespace secure_frontend
```

In `src/http_socket_server.cpp`, replace lines 270-273 (the `if (!std::holds_alternative<Config>(validate_config(verification_input)) …) { … }` check, verified at HEAD 7ddcd755) and the two `inet_pton` calls at lines 288 and 293 so that the address bound is the one the validator canonicalized:

```cpp
  const ConfigValidation verification = validate_config(verification_input);
  const auto* verified = std::get_if<Config>(&verification);
  if (verified == nullptr || !validator || !dispatcher) {
    if (error != nullptr) *error = "invalid listener configuration";
    return std::nullopt;
  }
```

```cpp
  if (inet_pton(AF_INET, verified->host.c_str(), &address4->sin_addr) == 1) {
```

```cpp
  } else if (inet_pton(AF_INET6, verified->host.c_str(), &address6->sin6_addr) == 1) {
```

- [ ] **Step 4: Run to verify it passes**

Run: `cmake --build --preset linux-gcc && ctest --preset linux-gcc | tail -3`
Expected: `100% tests passed, 0 tests failed out of 116` (Task 2's 111 + the five `net0*` cases).
Run: `cmake --build --preset windows-llvm-mingw-cross 2>&1 | tail -1 && ctest --preset windows-llvm-mingw-cross 2>&1 | tail -3`
Expected: build succeeds with `-Werror`; `100% tests passed, 0 tests failed out of 107` (Task 2's 102 + five).

- [ ] **Step 5: Commit**

```bash
git -C "$(git rev-parse --show-toplevel)" add secure_frontend/include/secure_frontend/config.hpp secure_frontend/src/config.cpp \
  secure_frontend/src/http_socket_server.cpp secure_frontend/tests/config_test.cpp secure_frontend/CMakeLists.txt
git -C "$(git rev-parse --show-toplevel)" -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "config: classify listener addresses from bytes; reject hostnames (SP1 N1, N2; defect a)" \
  -m "Regression tests (failed before the fix): net03_wildcard_spellings_need_wildcard_opt_in,
net03_remote_opt_in_alone_rejects_wildcard, net01_loopback_spellings_classified_from_bytes,
net02_hostname_rejected_before_bind, net01_localhost_maps_to_ipv4_loopback. ::0, 0::0, the long
zero form, ::0.0.0.0 and ::ffff:0.0.0.0 now need --allow-wildcard-bind; localhost maps to 127.0.0.1
without DNS; the canonical text is what bind() uses." \
  -m "$SP1_TRAILERS"
```

---

### Task 2: One CTest test per case, `windows-runtime` label, scratch roots under the build tree (spec §3.10)

**Files:**
- Create: `secure_frontend/tests/support/test_support.hpp`
- Modify: `secure_frontend/CMakeLists.txt:31` (add helpers after `secure_frontend_set_warnings`), `:273-407` (test block replaced)
- Modify: `secure_frontend/CMakePresets.json:115-124` (wine test preset excludes `windows-runtime`)
- Modify: `main()` of all 18 files in `secure_frontend/tests/`; `token_reader_test.cpp:29-48`, `model_inventory_test.cpp:27-32`, `tokenizer_pack_test.cpp:16-21` (scratch directories)

**Where and when (binding):** this task is the SHARED PRE-TASK consumed by SP2 (Review Focus 6). It runs FIRST, in the main checkout: `cd /opt/projects/ai/secure_frontend && export PATH="$(git rev-parse --show-toplevel)/.venv-dev/bin:$PATH"`, on branch `main` at 7ddcd755, so its commit lands on `main`. It registers only the cases that exist on `main` (Task 1's five `net0*` cases are added by Task 1). After the commit: `git -C /opt/projects/ai/.worktrees/sp1 rebase main`, and every later task runs in the worktree. The names `secure_frontend_add_cases`, `secure_frontend_add_test_executable`, `<binary>.<case>`, `SECURE_FRONTEND_WINDOWS_RUNTIME`/`windows-runtime` and the preset's `filter.exclude.label` are frozen from this commit on.

**Interfaces:**
- Produces (every later task):
```cpp
namespace secure_frontend_test {
struct Case final { std::string_view name; std::function<void()> run; };
std::vector<std::string>& extra_arguments();                      // args after `--case <name>`
std::filesystem::path make_scratch_directory();                   // fresh 0700 dir; removed by run_cases
void make_private_directory(const std::filesystem::path& path);   // mkdir 0700 (POSIX) / CreateDirectoryW + protected DACL (user, SYSTEM, Administrators)
void write_file(const std::filesystem::path& path, std::string_view bytes, unsigned mode = 0600);
std::string read_file(const std::filesystem::path& path);
int run_cases(int argc, char** argv, std::initializer_list<Case> cases, const int& failures);
}
```
```cmake
secure_frontend_add_test_executable(<name> <source> <libraries>...)
secure_frontend_add_cases(<binary> <case>... [ARGS <arg>...] [LABELS <label>...] [TIMEOUT <seconds>])
# SECURE_FRONTEND_WINDOWS_RUNTIME = "windows-runtime" on WIN32, "" elsewhere
```

- [ ] **Step 1: Write the failing check**

Run: `ctest --preset linux-gcc -N | grep -c 'secure_frontend_config_tests\.'`
Expected: `0` (exit status 1) — cases are not individually registered yet.

- [ ] **Step 2: Create `tests/support/test_support.hpp`**

```cpp
#pragma once
// Shared by every secure_frontend test binary (SP1 §3.10): a framework-free
// case runner -- `<binary> --case <name> [args...]` runs one case, no
// arguments runs all, `--list` prints the names -- plus owner-only scratch
// directories below the build tree.
#include <cerrno>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"
#include <sddl.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace secure_frontend_test {

struct Case final {
  std::string_view name;
  std::function<void()> run;
};

// Arguments after `--case <name>` (the ARGS of secure_frontend_add_cases).
inline std::vector<std::string>& extra_arguments() {
  static std::vector<std::string> arguments;
  return arguments;
}

inline std::vector<std::filesystem::path>& scratch_registry() {
  static std::vector<std::filesystem::path> directories;
  return directories;
}

#if defined(_WIN32)
// CreateDirectoryW with an explicit, protected DACL: full control for the
// current user, LocalSystem and Administrators, nothing inherited. %TEMP% is
// not trusted to be private -- on GitHub's setup-msys2 runners TEMP lives under
// D:\a\_temp, whose ACL can grant Authenticated Users write, and an inherited
// ACE like that would make every open_root(scratch) report shared_writable.
inline bool create_private_directory_w(const std::wstring& path) {
  HANDLE token = nullptr;
  if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == 0) return false;
  DWORD size = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &size);
  std::vector<unsigned char> buffer(size);
  const bool have_user = size != 0 && GetTokenInformation(token, TokenUser, buffer.data(), size, &size) != 0;
  CloseHandle(token);
  if (!have_user) return false;
  wchar_t* sid_text = nullptr;
  if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &sid_text) == 0) return false;
  // D:P = protected DACL; OICI = inherited by files and subdirectories; FA = full access.
  const std::wstring sddl = std::wstring{L"D:P(A;OICI;FA;;;"} + sid_text + L")(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)";
  LocalFree(sid_text);
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr) == 0) {
    return false;
  }
  SECURITY_ATTRIBUTES attributes{static_cast<DWORD>(sizeof(SECURITY_ATTRIBUTES)), descriptor, FALSE};
  const bool created = CreateDirectoryW(path.c_str(), &attributes) != 0;
  LocalFree(descriptor);
  return created;
}
#endif

// A fresh, empty directory only the current user can use. POSIX: mkdtemp
// (mode 0700) below $SECURE_FRONTEND_TEST_TMP, which secure_frontend_add_cases
// points at <build>/test-tmp. Windows: below %TEMP%, created with an explicit
// protected DACL (user, SYSTEM, Administrators) instead of inheriting %TEMP%'s.
inline std::filesystem::path make_scratch_directory() {
#if defined(_WIN32)
  const std::filesystem::path base = std::filesystem::temp_directory_path();
  static unsigned counter = 0;
  for (int attempt = 0; attempt < 1000; ++attempt) {
    const auto candidate = base / ("sf-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(++counter) +
                                   "-" + std::to_string(GetTickCount64()));
    if (create_private_directory_w(L"\\\\?\\" + std::filesystem::absolute(candidate).wstring())) {
      scratch_registry().push_back(candidate);
      return candidate;
    }
  }
  std::cerr << "unable to create a scratch directory\n";
  std::exit(EXIT_FAILURE);
#else
  const char* configured = std::getenv("SECURE_FRONTEND_TEST_TMP");
  const std::filesystem::path base = configured != nullptr && *configured != '\0'
                                         ? std::filesystem::path{configured}
                                         : std::filesystem::temp_directory_path();
  std::error_code ignored;
  std::filesystem::create_directories(base, ignored);
  std::string pattern = (base / "sf-XXXXXX").string();
  if (mkdtemp(pattern.data()) == nullptr) {
    std::cerr << "mkdtemp failed below the test scratch root\n";
    std::exit(EXIT_FAILURE);
  }
  scratch_registry().emplace_back(pattern);
  return std::filesystem::path{pattern};
#endif
}

// mkdir with mode 0700 (the umask cannot widen it), or CreateDirectoryW with
// the same explicit protected DACL as make_scratch_directory (never inherited).
inline void make_private_directory(const std::filesystem::path& path) {
#if defined(_WIN32)
  const std::wstring extended = L"\\\\?\\" + std::filesystem::absolute(path).wstring();
  if (!create_private_directory_w(extended) && GetLastError() != ERROR_ALREADY_EXISTS) {
    std::cerr << "CreateDirectoryW failed\n";
    std::exit(EXIT_FAILURE);
  }
#else
  if (mkdir(path.c_str(), 0700) != 0 && errno != EEXIST) {
    std::cerr << "mkdir failed\n";
    std::exit(EXIT_FAILURE);
  }
#endif
}

// Writes bytes (replacing the file) and, on POSIX, sets `mode` exactly.
inline void write_file(const std::filesystem::path& path, const std::string_view bytes, const unsigned mode = 0600) {
#if defined(_WIN32)
  (void)mode;
  const std::wstring extended = L"\\\\?\\" + std::filesystem::absolute(path).wstring();
  HANDLE file = CreateFileW(extended.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) { std::cerr << "CreateFileW failed\n"; std::exit(EXIT_FAILURE); }
  DWORD written = 0;
  const bool ok = bytes.empty() || (WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) != 0 &&
                                    written == bytes.size());
  CloseHandle(file);
  if (!ok) { std::cerr << "WriteFile failed\n"; std::exit(EXIT_FAILURE); }
#else
  {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }
  if (chmod(path.c_str(), static_cast<mode_t>(mode)) != 0) { std::cerr << "chmod failed\n"; std::exit(EXIT_FAILURE); }
#endif
}

inline std::string read_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return std::string{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

inline void remove_scratch_directories() {
  for (const auto& directory : scratch_registry()) {
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
  }
  scratch_registry().clear();
}

// `failures` is the calling file's own counter (files whose expect() throws pass a zero).
inline int run_cases(const int argc, char** argv, const std::initializer_list<Case> cases, const int& failures) {
  std::vector<const Case*> selected;
  if (argc >= 2 && std::string_view{argv[1]} == "--list") {
    for (const auto& c : cases) std::cout << c.name << '\n';
    return EXIT_SUCCESS;
  }
  if (argc >= 3 && std::string_view{argv[1]} == "--case") {
    for (int index = 3; index < argc; ++index) extra_arguments().emplace_back(argv[index]);
    for (const auto& c : cases) if (c.name == std::string_view{argv[2]}) selected.push_back(&c);
    if (selected.empty()) { std::cerr << "unknown case: " << argv[2] << '\n'; return EXIT_FAILURE; }
  } else if (argc == 1) {
    for (const auto& c : cases) selected.push_back(&c);
  } else {
    std::cerr << "usage: " << argv[0] << " [--list | --case <name> [args...]]\n";
    return EXIT_FAILURE;
  }
  for (const Case* c : selected) {
    try {
      c->run();
    } catch (const std::exception& error) {
      std::cerr << "FAILED: " << c->name << ": " << error.what() << '\n';
      remove_scratch_directories();
      return EXIT_FAILURE;
    }
  }
  remove_scratch_directories();
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

}  // namespace secure_frontend_test
```

- [ ] **Step 3: CMake helpers and the test block**

In `CMakeLists.txt`, after `endfunction()` of `secure_frontend_set_warnings` (line 31), add:

```cmake
# --- Test helpers (SP1 §3.10) -------------------------------------------------
# Every test binary is a framework-free main() that takes `--case <name>`; each
# case is its own CTest test named <binary>.<case>, so acceptance IDs appear in
# `ctest -N`. Cases that need real Windows semantics (DACLs, DPAPI, junctions,
# Job Objects) carry ${SECURE_FRONTEND_WINDOWS_RUNTIME}; the wine preset
# excludes that label and the windows-msys2-clang CI job runs everything.
set(SECURE_FRONTEND_TEST_TMP "${CMAKE_CURRENT_BINARY_DIR}/test-tmp")
if(WIN32)
    set(SECURE_FRONTEND_WINDOWS_RUNTIME windows-runtime)
else()
    set(SECURE_FRONTEND_WINDOWS_RUNTIME "")
endif()

function(secure_frontend_add_test_executable name source)
    add_executable(${name} ${source})
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_link_libraries(${name} PRIVATE ${ARGN})
    if(WIN32)
        # test_support.hpp builds private-DACL scratch directories (ConvertSidToStringSidW,
        # ConvertStringSecurityDescriptorToSecurityDescriptorW, OpenProcessToken).
        target_link_libraries(${name} PRIVATE advapi32)
    endif()
    target_compile_features(${name} PRIVATE cxx_std_20)
    secure_frontend_set_warnings(${name})
endfunction()

# secure_frontend_add_cases(<binary> <case>... [ARGS <arg>...] [LABELS <label>...] [TIMEOUT <seconds>])
function(secure_frontend_add_cases binary)
    cmake_parse_arguments(PARSE_ARGV 1 _sfac "" "TIMEOUT" "ARGS;LABELS")
    file(MAKE_DIRECTORY "${SECURE_FRONTEND_TEST_TMP}")
    foreach(_case IN LISTS _sfac_UNPARSED_ARGUMENTS)
        set(_test "${binary}.${_case}")
        add_test(NAME ${_test} COMMAND ${binary} --case ${_case} ${_sfac_ARGS})
        set_tests_properties(${_test} PROPERTIES ENVIRONMENT "SECURE_FRONTEND_TEST_TMP=${SECURE_FRONTEND_TEST_TMP}")
        if(_sfac_LABELS)
            set_tests_properties(${_test} PROPERTIES LABELS "${_sfac_LABELS}")
        endif()
        if(_sfac_TIMEOUT)
            set_tests_properties(${_test} PROPERTIES TIMEOUT ${_sfac_TIMEOUT})
        endif()
    endforeach()
endfunction()
```

Replace lines 273-407 (the whole `if(STRATA_BUILD_SECURE_FRONTEND_TESTS)` block) with:

```cmake
if(STRATA_BUILD_SECURE_FRONTEND_TESTS)
    enable_testing()

    secure_frontend_add_test_executable(secure_frontend_config_tests tests/config_test.cpp secure_frontend_config)
    secure_frontend_add_cases(secure_frontend_config_tests
        defaults_to_loopback requires_key_for_loopback requires_explicit_opt_in_for_remote_host
        requires_key_after_remote_opt_in accepts_remote_host_with_opt_in_and_key
        requires_separate_opt_in_for_wildcard_host rejects_invalid_ports rejects_invalid_hosts_and_blank_remote_keys)

    secure_frontend_add_test_executable(secure_frontend_secret_source_tests tests/secret_source_test.cpp secure_frontend_config)
    secure_frontend_add_cases(secure_frontend_secret_source_tests
        accepts_environment_variable_name_without_retaining_a_secret accepts_protected_file_path_metadata
        rejects_unsafe_environment_variable_names rejects_blank_or_control_containing_file_paths
        rejects_paths_that_escape_or_trigger_platform_special_handling)

    if(UNIX)
        # POSIX-only until SP1 Task 18 un-gates it for the windows-msys2-clang job.
        secure_frontend_add_test_executable(secure_frontend_token_reader_tests tests/token_reader_test.cpp
            secure_frontend_token_reader)
        secure_frontend_add_cases(secure_frontend_token_reader_tests
            reads_an_owner_only_regular_file_below_the_root reads_the_named_environment_variable
            rejects_control_characters_in_resolved_tokens rejects_a_group_or_world_accessible_file
            rejects_a_symlink_that_escapes_the_root rejects_a_non_regular_file)
    endif()

    secure_frontend_add_test_executable(secure_frontend_engine_stdio_process_tests tests/engine_stdio_process_test.cpp
        secure_frontend_engine_stdio_process)
    set(_stdio_cases test_round_trip test_invalid_record read_line_rejects_oversize_on_all_platforms
        read_line_waits_for_a_record_split_across_writes)
    if(WIN32)
        list(APPEND _stdio_cases job_object_terminates_grandchild)
    else()
        list(APPEND _stdio_cases child_inherits_only_stdio_descriptors child_receives_exact_argv_and_env
            terminate_reaps_killed_child)
    endif()
    secure_frontend_add_cases(secure_frontend_engine_stdio_process_tests ${_stdio_cases} TIMEOUT 300)

    secure_frontend_add_test_executable(secure_frontend_http_policy_tests tests/http_policy_test.cpp secure_frontend_http_policy)
    secure_frontend_add_cases(secure_frontend_http_policy_tests
        accepts_exact_host_without_browser_origin rejects_ambiguous_or_untrusted_host requires_an_exact_same_origin
        rejects_duplicate_or_malformed_origin permits_only_configured_cors_origin_when_enabled)

    secure_frontend_add_test_executable(secure_frontend_http_request_handler_tests tests/http_request_handler_test.cpp
        secure_frontend_http_request_handler)
    secure_frontend_add_cases(secure_frontend_http_request_handler_tests
        accepts_authorized_request passes_the_bounded_raw_body_to_an_authorized_dispatcher
        rejects_malformed_and_ambiguous_messages requires_one_well_formed_bearer_token
        fails_closed_before_calling_validator_for_untrusted_origin emits_cors_only_for_policy_approved_origin
        handles_policy_approved_preflight_without_exposing_an_endpoint constant_time_helper_matches_only_equal_tokens)

    secure_frontend_add_test_executable(secure_frontend_http_socket_server_tests tests/http_socket_server_test.cpp
        secure_frontend_http_socket_server)
    secure_frontend_add_cases(secure_frontend_http_socket_server_tests
        serves_a_loopback_request_without_outbound_connections socket_server_survives_client_early_close
        socket_server_rejects_slow_request_past_deadline socket_server_send_times_out_on_stalled_reader
        socket_server_rejects_oversize_request_over_tcp socket_server_accept_waits_without_timeout
        socket_server_ipv6_bind_is_v6only socket_server_response_is_complete
        socket_server_survives_reset_before_accept socket_server_responds_after_client_half_close
        TIMEOUT 300)

    secure_frontend_add_test_executable(secure_frontend_api_router_tests tests/api_router_test.cpp secure_frontend_api_router)
    secure_frontend_add_cases(secure_frontend_api_router_tests
        health_samples_status_before_state_once_each health_includes_engine_state_when_provided
        health_is_503_with_engine_state_when_unavailable dispatches_exact_health_route
        dispatches_models_with_json_escaping rejects_non_exact_routes
        fails_closed_when_provider_throws_or_returns_invalid_model)

    secure_frontend_add_test_executable(secure_frontend_engine_process_tests tests/engine_process_test.cpp
        secure_frontend_engine_process)
    secure_frontend_add_cases(secure_frontend_engine_process_tests
        starts_without_a_shell_and_uses_only_allowlisted_environment rejects_empty_executable
        rejects_malformed_allowlist_entry rejects_embedded_nul_in_exec_boundary termination_cleans_up_a_process_group)

    secure_frontend_add_test_executable(secure_frontend_engine_protocol_client_tests tests/engine_protocol_client_test.cpp
        secure_frontend_engine_protocol_client)
    secure_frontend_add_cases(secure_frontend_engine_protocol_client_tests
        runs_the_solo_protocol stop_and_quit_are_explicit refuses_stop_when_not_advertised
        rejects_malformed_and_unexpected_engine_output bounds_and_engine_errors_fail_closed
        prefill_progress_is_glm_only_and_strictly_bounded generation_total_deadline_is_enforced
        gen_line_fits_max_prompt_tokens derived_line_cap_is_clamped_to_protocol_max)

    secure_frontend_add_test_executable(secure_frontend_model_inventory_tests tests/model_inventory_test.cpp
        secure_frontend_model_inventory)
    secure_frontend_add_cases(secure_frontend_model_inventory_tests
        validates_portable_model_ids validates_portable_artifact_paths inventories_only_local_gguf_artifacts
        rejects_duplicate_ids_and_symlinks)

    secure_frontend_add_test_executable(secure_frontend_engine_arguments_tests tests/engine_arguments_test.cpp
        secure_frontend_engine_arguments)
    secure_frontend_add_cases(secure_frontend_engine_arguments_tests
        engine_receives_model_path_from_inventory engine_args_reject_owned_and_network_flags
        strata_profile_passes_pack_and_weight_flags_through rejects_invalid_relative_artifact_path
        glm_profile_passes_arguments_through_without_native)

    secure_frontend_add_test_executable(secure_frontend_chat_schema_tests tests/chat_schema_test.cpp secure_frontend_chat_schema)
    secure_frontend_add_cases(secure_frontend_chat_schema_tests
        parses_minimal_text_request rejects_unsupported_and_unsafe_features rejects_duplicate_and_malformed_json
        renders_escaped_minimal_response)

    secure_frontend_add_test_executable(secure_frontend_tokenizer_pack_tests tests/tokenizer_pack_test.cpp
        secure_frontend_tokenizer_pack)
    set(_tokenizer_cases loads_validates_and_decodes_offline_pack rejects_ambiguous_or_incompatible_pack_assets
        rejects_malformed_json_and_special_literals_for_decode encoding_fails_closed_for_special_invalid_and_missing_tokens)
    if(NOT WIN32)
        list(APPEND _tokenizer_cases rejects_symlinked_assets)
    endif()
    secure_frontend_add_cases(secure_frontend_tokenizer_pack_tests ${_tokenizer_cases})

    secure_frontend_add_test_executable(secure_frontend_qwen_prompt_renderer_tests tests/qwen_prompt_renderer_test.cpp
        secure_frontend_qwen_prompt_renderer)
    secure_frontend_add_cases(secure_frontend_qwen_prompt_renderer_tests
        renders_fixed_qwen_chatml_prompt accepts_systemless_conversation rejects_unsafe_or_unsupported_conversations)

    secure_frontend_add_test_executable(secure_frontend_glm_profile_tests tests/glm_profile_test.cpp
        secure_frontend_chat_generation_plan)
    secure_frontend_add_cases(secure_frontend_glm_profile_tests
        renders_exact_fixed_profile rejects_boundary_injection_and_invalid_order creates_plan_only_with_control_tokens)

    secure_frontend_add_test_executable(secure_frontend_chat_generation_plan_tests tests/chat_generation_plan_test.cpp
        secure_frontend_chat_generation_plan)
    secure_frontend_add_cases(secure_frontend_chat_generation_plan_tests
        composes_schema_prompt_and_tokenizer defaults_output_limit_and_rejects_unsafe_inputs
        rejects_missing_or_non_control_framing_tokens does_not_parse_arbitrary_special_literals)

    secure_frontend_add_test_executable(secure_frontend_chat_execution_tests tests/chat_execution_test.cpp
        secure_frontend_chat_execution)
    secure_frontend_add_cases(secure_frontend_chat_execution_tests
        executes_a_bounded_non_streaming_completion rejects_context_and_output_limits_before_generation
        fails_closed_on_invalid_engine_output executes_glm53_only_when_explicitly_selected
        chat_recovers_after_generation_timeout chat_marks_engine_failed_when_drain_fails
        chat_rejects_nonfinite_done_and_stays_ready health_reflects_protocol_state
        health_is_failed_after_engine_closed_mid_generation health_is_failed_when_engine_closes_before_ready
        chat_applies_engine_context_rule_with_eight_token_margin)

    add_test(NAME strata_secure_server_help COMMAND strata-secure-server --help)
    # The GLM runner takes positional arguments: glm53 without any --engine-arg is a usage error.
    add_test(NAME strata_secure_server_glm_requires_engine_arg
        COMMAND strata-secure-server --profile glm53 --models-root models --tokenizer-pack tok
                --engine engine --model-id glm-local --token-env STRATA_UNUSED_TOKEN)
    set_tests_properties(strata_secure_server_glm_requires_engine_arg PROPERTIES
        PASS_REGULAR_EXPRESSION "glm53 requires at least one --engine-arg")
endif()
```

In `CMakePresets.json`, replace the `windows-llvm-mingw-cross` test preset (lines 115-124) with:

```json
    {
      "name": "windows-llvm-mingw-cross",
      "configurePreset": "windows-llvm-mingw-cross",
      "output": {
        "outputOnFailure": true
      },
      "filter": {
        "exclude": {
          "label": "windows-runtime"
        }
      },
      "environment": {
        "WINEPREFIX": "${sourceDir}/out/wineprefix"
      }
    },
```

- [ ] **Step 4: Convert every test `main()`**

Add `#include "support/test_support.hpp"` to each of the 18 files in `tests/` and replace its `main()` as follows (the case functions are unchanged).

`tests/config_test.cpp`:
```cpp
int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"defaults_to_loopback", defaults_to_loopback},
      {"requires_key_for_loopback", requires_key_for_loopback},
      {"requires_explicit_opt_in_for_remote_host", requires_explicit_opt_in_for_remote_host},
      {"requires_key_after_remote_opt_in", requires_key_after_remote_opt_in},
      {"accepts_remote_host_with_opt_in_and_key", accepts_remote_host_with_opt_in_and_key},
      {"requires_separate_opt_in_for_wildcard_host", requires_separate_opt_in_for_wildcard_host},
      {"rejects_invalid_ports", rejects_invalid_ports},
      {"rejects_invalid_hosts_and_blank_remote_keys", rejects_invalid_hosts_and_blank_remote_keys},
  }, failures);
}
```

`tests/secret_source_test.cpp`:
```cpp
int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"accepts_environment_variable_name_without_retaining_a_secret", accepts_environment_variable_name_without_retaining_a_secret},
      {"accepts_protected_file_path_metadata", accepts_protected_file_path_metadata},
      {"rejects_unsafe_environment_variable_names", rejects_unsafe_environment_variable_names},
      {"rejects_blank_or_control_containing_file_paths", rejects_blank_or_control_containing_file_paths},
      {"rejects_paths_that_escape_or_trigger_platform_special_handling", rejects_paths_that_escape_or_trigger_platform_special_handling},
  }, failures);
}
```

`tests/token_reader_test.cpp` — also replace `class TemporaryDirectory` (lines 29-48) with:
```cpp
class TemporaryDirectory final {
 public:
  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

 private:
  std::filesystem::path path_ = secure_frontend_test::make_scratch_directory();
};
```
and `main()` with:
```cpp
int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"reads_an_owner_only_regular_file_below_the_root", reads_an_owner_only_regular_file_below_the_root},
      {"reads_the_named_environment_variable", reads_the_named_environment_variable},
      {"rejects_control_characters_in_resolved_tokens", rejects_control_characters_in_resolved_tokens},
      {"rejects_a_group_or_world_accessible_file", rejects_a_group_or_world_accessible_file},
      {"rejects_a_symlink_that_escapes_the_root", rejects_a_symlink_that_escapes_the_root},
      {"rejects_a_non_regular_file", rejects_a_non_regular_file},
  }, failures);
}
```
Keep the file's existing includes unchanged (`-Wunused` does not apply to includes, so `<algorithm>` and `<array>` stay even where no longer used).

`tests/engine_stdio_process_test.cpp` (its `expect()` throws, so the counter is a constant zero):
```cpp
int main(const int argc, char* argv[]) {
  g_argc = argc;
  g_argv = argv;
  for (int index = 1; index < argc; ++index) {
    if (std::string(argv[index]) == "--fake-engine") return fake_engine();
    if (std::string(argv[index]) == "--fake-sleeper") {
      std::this_thread::sleep_for(std::chrono::seconds(30));
      return 0;
    }
  }
  static const int no_counted_failures = 0;
  return secure_frontend_test::run_cases(argc, argv, {
      {"test_round_trip", [] { test_round_trip(current_executable()); }},
      {"test_invalid_record", [] { test_invalid_record(); }},
      {"read_line_rejects_oversize_on_all_platforms", [] { read_line_rejects_oversize_on_all_platforms(current_executable()); }},
      {"read_line_waits_for_a_record_split_across_writes", [] { read_line_waits_for_a_record_split_across_writes(current_executable()); }},
#if defined(_WIN32)
      {"job_object_terminates_grandchild", [] { job_object_terminates_grandchild(current_executable()); }},
#else
      {"child_inherits_only_stdio_descriptors", [] { child_inherits_only_stdio_descriptors(current_executable()); }},
      {"child_receives_exact_argv_and_env", [] { child_receives_exact_argv_and_env(current_executable()); }},
      {"terminate_reaps_killed_child", [] { terminate_reaps_killed_child(current_executable()); }},
#endif
  }, no_counted_failures);
}
```

`tests/engine_process_test.cpp` (keep the two re-exec branches first):
```cpp
int main(int argc, char** argv) {
  if (argc >= 2 && std::string(argv[1]) == "--fake-engine") return run_fake_engine(argc, argv);
  if (argc >= 2 && std::string(argv[1]) == "--fake-sleeper") {
    std::this_thread::sleep_for(std::chrono::seconds(30));
    return 0;
  }
  return secure_frontend_test::run_cases(argc, argv, {
      {"starts_without_a_shell_and_uses_only_allowlisted_environment", starts_without_a_shell_and_uses_only_allowlisted_environment},
      {"rejects_empty_executable", rejects_empty_executable},
      {"rejects_malformed_allowlist_entry", rejects_malformed_allowlist_entry},
      {"rejects_embedded_nul_in_exec_boundary", rejects_embedded_nul_in_exec_boundary},
      {"termination_cleans_up_a_process_group", termination_cleans_up_a_process_group},
  }, failures);
}
```

`tests/http_socket_server_test.cpp`:
```cpp
int main(int argc, char** argv) {
  secure_frontend::install_process_defaults();
  return secure_frontend_test::run_cases(argc, argv, {
      {"serves_a_loopback_request_without_outbound_connections", serves_a_loopback_request_without_outbound_connections},
      {"socket_server_survives_client_early_close", socket_server_survives_client_early_close},
      {"socket_server_rejects_slow_request_past_deadline", socket_server_rejects_slow_request_past_deadline},
      {"socket_server_send_times_out_on_stalled_reader", socket_server_send_times_out_on_stalled_reader},
      {"socket_server_rejects_oversize_request_over_tcp", socket_server_rejects_oversize_request_over_tcp},
      {"socket_server_accept_waits_without_timeout", socket_server_accept_waits_without_timeout},
      {"socket_server_ipv6_bind_is_v6only", socket_server_ipv6_bind_is_v6only},
      {"socket_server_response_is_complete", socket_server_response_is_complete},
      {"socket_server_survives_reset_before_accept", socket_server_survives_reset_before_accept},
      {"socket_server_responds_after_client_half_close", socket_server_responds_after_client_half_close},
  }, failures);
}
```

`tests/model_inventory_test.cpp` — replace `unique_temp_directory()` (lines 27-32) body with `return secure_frontend_test::make_scratch_directory();` and:
```cpp
int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"validates_portable_model_ids", validates_portable_model_ids},
      {"validates_portable_artifact_paths", validates_portable_artifact_paths},
      {"inventories_only_local_gguf_artifacts", inventories_only_local_gguf_artifacts},
      {"rejects_duplicate_ids_and_symlinks", rejects_duplicate_ids_and_symlinks},
  }, failures);
}
```

`tests/tokenizer_pack_test.cpp` — replace `temporary_pack()` (lines 16-21) body with `return secure_frontend_test::make_scratch_directory();` and:
```cpp
int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"loads_validates_and_decodes_offline_pack", loads_validates_and_decodes_offline_pack},
      {"rejects_ambiguous_or_incompatible_pack_assets", rejects_ambiguous_or_incompatible_pack_assets},
      {"rejects_malformed_json_and_special_literals_for_decode", rejects_malformed_json_and_special_literals_for_decode},
      {"encoding_fails_closed_for_special_invalid_and_missing_tokens", encoding_fails_closed_for_special_invalid_and_missing_tokens},
#ifndef _WIN32
      {"rejects_symlinked_assets", rejects_symlinked_assets},
#endif
  }, failures);
}
```

`tests/api_router_test.cpp`:
```cpp
int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"health_samples_status_before_state_once_each", health_samples_status_before_state_once_each},
      {"health_includes_engine_state_when_provided", health_includes_engine_state_when_provided},
      {"health_is_503_with_engine_state_when_unavailable", health_is_503_with_engine_state_when_unavailable},
      {"dispatches_exact_health_route", dispatches_exact_health_route},
      {"dispatches_models_with_json_escaping", dispatches_models_with_json_escaping},
      {"rejects_non_exact_routes", rejects_non_exact_routes},
      {"fails_closed_when_provider_throws_or_returns_invalid_model", fails_closed_when_provider_throws_or_returns_invalid_model},
  }, failures);
}
```

`tests/chat_execution_test.cpp`:
```cpp
int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"executes_a_bounded_non_streaming_completion", executes_a_bounded_non_streaming_completion},
      {"rejects_context_and_output_limits_before_generation", rejects_context_and_output_limits_before_generation},
      {"fails_closed_on_invalid_engine_output", fails_closed_on_invalid_engine_output},
      {"executes_glm53_only_when_explicitly_selected", executes_glm53_only_when_explicitly_selected},
      {"chat_recovers_after_generation_timeout", chat_recovers_after_generation_timeout},
      {"chat_marks_engine_failed_when_drain_fails", chat_marks_engine_failed_when_drain_fails},
      {"chat_rejects_nonfinite_done_and_stays_ready", chat_rejects_nonfinite_done_and_stays_ready},
      {"health_reflects_protocol_state", health_reflects_protocol_state},
      {"health_is_failed_after_engine_closed_mid_generation", health_is_failed_after_engine_closed_mid_generation},
      {"health_is_failed_when_engine_closes_before_ready", health_is_failed_when_engine_closes_before_ready},
      {"chat_applies_engine_context_rule_with_eight_token_margin", chat_applies_engine_context_rule_with_eight_token_margin},
  }, failures);
}
```

`tests/chat_generation_plan_test.cpp`:
```cpp
int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"composes_schema_prompt_and_tokenizer", composes_schema_prompt_and_tokenizer},
      {"defaults_output_limit_and_rejects_unsafe_inputs", defaults_output_limit_and_rejects_unsafe_inputs},
      {"rejects_missing_or_non_control_framing_tokens", rejects_missing_or_non_control_framing_tokens},
      {"does_not_parse_arbitrary_special_literals", does_not_parse_arbitrary_special_literals},
  }, failures);
}
```

`tests/chat_schema_test.cpp`:
```cpp
int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"parses_minimal_text_request", parses_minimal_text_request},
      {"rejects_unsupported_and_unsafe_features", rejects_unsupported_and_unsafe_features},
      {"rejects_duplicate_and_malformed_json", rejects_duplicate_and_malformed_json},
      {"renders_escaped_minimal_response", renders_escaped_minimal_response},
  }, failures);
}
```

`tests/engine_arguments_test.cpp`:
```cpp
int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"engine_receives_model_path_from_inventory", engine_receives_model_path_from_inventory},
      {"engine_args_reject_owned_and_network_flags", engine_args_reject_owned_and_network_flags},
      {"strata_profile_passes_pack_and_weight_flags_through", strata_profile_passes_pack_and_weight_flags_through},
      {"rejects_invalid_relative_artifact_path", rejects_invalid_relative_artifact_path},
      {"glm_profile_passes_arguments_through_without_native", glm_profile_passes_arguments_through_without_native},
  }, failures);
}
```

`tests/engine_protocol_client_test.cpp`:
```cpp
int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"runs_the_solo_protocol", runs_the_solo_protocol},
      {"stop_and_quit_are_explicit", stop_and_quit_are_explicit},
      {"refuses_stop_when_not_advertised", refuses_stop_when_not_advertised},
      {"rejects_malformed_and_unexpected_engine_output", rejects_malformed_and_unexpected_engine_output},
      {"bounds_and_engine_errors_fail_closed", bounds_and_engine_errors_fail_closed},
      {"prefill_progress_is_glm_only_and_strictly_bounded", prefill_progress_is_glm_only_and_strictly_bounded},
      {"generation_total_deadline_is_enforced", generation_total_deadline_is_enforced},
      {"gen_line_fits_max_prompt_tokens", gen_line_fits_max_prompt_tokens},
      {"derived_line_cap_is_clamped_to_protocol_max", derived_line_cap_is_clamped_to_protocol_max},
  }, failures);
}
```

`tests/glm_profile_test.cpp`:
```cpp
int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"renders_exact_fixed_profile", renders_exact_fixed_profile},
      {"rejects_boundary_injection_and_invalid_order", rejects_boundary_injection_and_invalid_order},
      {"creates_plan_only_with_control_tokens", creates_plan_only_with_control_tokens},
  }, failures);
}
```

`tests/http_policy_test.cpp`:
```cpp
int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"accepts_exact_host_without_browser_origin", accepts_exact_host_without_browser_origin},
      {"rejects_ambiguous_or_untrusted_host", rejects_ambiguous_or_untrusted_host},
      {"requires_an_exact_same_origin", requires_an_exact_same_origin},
      {"rejects_duplicate_or_malformed_origin", rejects_duplicate_or_malformed_origin},
      {"permits_only_configured_cors_origin_when_enabled", permits_only_configured_cors_origin_when_enabled},
  }, failures);
}
```

`tests/http_request_handler_test.cpp`:
```cpp
int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"accepts_authorized_request", accepts_authorized_request},
      {"passes_the_bounded_raw_body_to_an_authorized_dispatcher", passes_the_bounded_raw_body_to_an_authorized_dispatcher},
      {"rejects_malformed_and_ambiguous_messages", rejects_malformed_and_ambiguous_messages},
      {"requires_one_well_formed_bearer_token", requires_one_well_formed_bearer_token},
      {"fails_closed_before_calling_validator_for_untrusted_origin", fails_closed_before_calling_validator_for_untrusted_origin},
      {"emits_cors_only_for_policy_approved_origin", emits_cors_only_for_policy_approved_origin},
      {"handles_policy_approved_preflight_without_exposing_an_endpoint", handles_policy_approved_preflight_without_exposing_an_endpoint},
      {"constant_time_helper_matches_only_equal_tokens", constant_time_helper_matches_only_equal_tokens},
  }, failures);
}
```

`tests/qwen_prompt_renderer_test.cpp`:
```cpp
int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"renders_fixed_qwen_chatml_prompt", renders_fixed_qwen_chatml_prompt},
      {"accepts_systemless_conversation", accepts_systemless_conversation},
      {"rejects_unsafe_or_unsupported_conversations", rejects_unsafe_or_unsupported_conversations},
  }, failures);
}
```

A name registered in CMake but missing from `main` fails with `unknown case: <name>`, so Step 5 catches any mismatch between the two lists.

- [ ] **Step 5: Run to verify it passes**

Run: `cmake --preset linux-gcc && cmake --build --preset linux-gcc && ctest --preset linux-gcc -N | tail -1 && ctest --preset linux-gcc | tail -3`
Expected: `Total Tests: 111` (109 cases + `strata_secure_server_help` + `strata_secure_server_glm_requires_engine_arg`); `100% tests passed, 0 tests failed out of 111`.
Run: `ctest --preset linux-gcc -N | grep -c 'secure_frontend_config_tests\.'` → `8`.
Run: `find out/build/linux-gcc/test-tmp -mindepth 1 | wc -l` → `0` (every scratch directory removed).
Run: `cmake --preset windows-llvm-mingw-cross && cmake --build --preset windows-llvm-mingw-cross && ctest --preset windows-llvm-mingw-cross | tail -3`
Expected: `100% tests passed, 0 tests failed out of 102` (no token_reader cases, five stdio cases, four tokenizer cases). The scratch directories now carry an explicit protected DACL; wine accepts `CreateDirectoryW` with a security descriptor, so nothing here depends on wine modelling ACLs.
Run: `git -C "$(git rev-parse --show-toplevel)" status --short` → only `M` lines under `secure_frontend/` and `?? secure_frontend/tests/support/`.

- [ ] **Step 6: Commit**

```bash
git -C "$(git rev-parse --show-toplevel)" add secure_frontend/CMakeLists.txt secure_frontend/CMakePresets.json secure_frontend/tests
git -C "$(git rev-parse --show-toplevel)" -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "tests: one CTest test per case via --case; windows-runtime label; scratch roots under the build tree (SP1 §3.10)" \
  -m "secure_frontend_add_cases(<binary> <case>...) registers <binary>.<case>; the wine preset excludes
the windows-runtime label. Fixtures use mkdtemp (0700) below <build>/test-tmp instead of fixed /tmp names.
Windows scratch directories get an explicit protected DACL (user, SYSTEM, Administrators). Shared
pre-task for SP2: helper names, <binary>.<case>, the windows-runtime label and the preset exclusion are frozen.
Linux 111/111, wine 102/102." \
  -m "$SP1_TRAILERS"
```

Then move the SP1 worktree onto it (the branch has no commits yet, so this fast-forwards):

```bash
git -C /opt/projects/ai/.worktrees/sp1 rebase main
git -C /opt/projects/ai/.worktrees/sp1 log --oneline -1   # → the Task 2 commit
```

---
### Task 3: One strict, bounded JSON reader shared by the chat schema and the tokenizer pack (C1)

**Files:**
- Create: `secure_frontend/include/secure_frontend/json.hpp`, `secure_frontend/src/json.cpp`, `secure_frontend/tests/json_test.cpp`
- Modify: `secure_frontend/src/chat_schema.cpp:94-110` (delete `append_utf8`), `:112-299` (delete `Parser`), `:301-324` (delete `append_json_string`), `:328-330` (rewrite `parse_chat_completion_request`)
- Modify: `secure_frontend/src/tokenizer_pack.cpp:67-158` (delete `class Json`), `:258-261` (use `parse_json`)
- Modify: `secure_frontend/CMakeLists.txt` (new library `secure_frontend_json`; chat schema and tokenizer pack link it; warnings list; test binary)

**Interfaces:**
- Produces (Tasks 4, 5, 12, 21 and the test support):
```cpp
namespace secure_frontend {
enum class JsonKind { null, boolean, integer, string, array, object };
class JsonValue;
struct JsonMember;
class JsonValue final {
 public:
  JsonValue() = default;                                       // null
  static JsonValue make_boolean(bool value);
  static JsonValue make_integer(std::int64_t value);
  static JsonValue make_string(std::string value);
  static JsonValue make_array(std::vector<JsonValue> items);
  static JsonValue make_object(std::vector<JsonMember> members);
  [[nodiscard]] JsonKind kind() const noexcept;
  [[nodiscard]] const bool* if_boolean() const noexcept;
  [[nodiscard]] const std::int64_t* if_integer() const noexcept;
  [[nodiscard]] const std::string* if_string() const noexcept;
  [[nodiscard]] const std::vector<JsonValue>* if_array() const noexcept;
  [[nodiscard]] const std::vector<JsonMember>* if_object() const noexcept;
};
struct JsonMember final { std::string key; JsonValue value; };
struct JsonLimits final { std::size_t max_bytes; std::size_t max_depth; std::size_t max_container_entries; std::size_t max_string_bytes; };
enum class JsonErrorCode { too_large, byte_order_mark, syntax, depth_exceeded, too_many_entries, string_too_long,
                           invalid_string, duplicate_key, non_integer_number, integer_out_of_range, trailing_data };
struct JsonError final { JsonErrorCode code; std::size_t offset; };
using JsonParseResult = std::variant<JsonValue, JsonError>;
[[nodiscard]] JsonParseResult parse_json(std::string_view text, const JsonLimits& limits);
[[nodiscard]] std::string_view describe_json_error(JsonErrorCode code) noexcept;
void append_json_string(std::string* output, std::string_view value);   // quoted, escaped
}
```

- [ ] **Step 1: Write the failing tests** (`tests/json_test.cpp`)

```cpp
#include "secure_frontend/json.hpp"
#include "support/test_support.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <variant>

namespace {
int failures = 0;
void expect(const bool condition, const std::string& message) {
  if (!condition) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
}
constexpr secure_frontend::JsonLimits kLimits{.max_bytes = 4096, .max_depth = 4, .max_container_entries = 64,
                                              .max_string_bytes = 256};

bool fails_with(const std::string& text, const secure_frontend::JsonErrorCode code,
                const secure_frontend::JsonLimits& limits = kLimits) {
  const auto result = secure_frontend::parse_json(text, limits);
  const auto* error = std::get_if<secure_frontend::JsonError>(&result);
  return error != nullptr && error->code == code;
}

void json_accepts_strict_document() {
  const auto result = secure_frontend::parse_json(
      R"( {"a": 1, "b": [true, false, null], "c": {"d": "xé\n"}, "e": -9223372036854775808} )", kLimits);
  const auto* value = std::get_if<secure_frontend::JsonValue>(&result);
  expect(value != nullptr && value->if_object() != nullptr, "a strict document must parse");
  if (value == nullptr || value->if_object() == nullptr) return;
  const auto& members = *value->if_object();
  expect(members.size() == 4U && members[0].key == "a" && *members[0].value.if_integer() == 1,
         "members keep their order and integer values");
  expect(members[2].value.if_object() != nullptr &&
             *(*members[2].value.if_object())[0].value.if_string() == "x\xC3\xA9\n",
         "escapes decode to UTF-8");
  expect(*members[3].value.if_integer() == INT64_MIN, "the int64 minimum is representable");
}

void json_rejects_duplicate_keys() {
  expect(fails_with(R"({"a":1,"a":2})", secure_frontend::JsonErrorCode::duplicate_key), "top-level duplicate");
  expect(fails_with(R"({"o":{"k":1,"x":0,"k":1}})", secure_frontend::JsonErrorCode::duplicate_key), "nested duplicate");
  expect(fails_with(R"({"a":1,"\u0061":2})", secure_frontend::JsonErrorCode::duplicate_key),
         "duplicates compare decoded keys");
}

void json_rejects_depth_over_limit() {
  expect(std::holds_alternative<secure_frontend::JsonValue>(secure_frontend::parse_json("[[[[1]]]]", kLimits)),
         "depth 4 is allowed");
  expect(fails_with("[[[[[1]]]]]", secure_frontend::JsonErrorCode::depth_exceeded), "depth 5 is rejected");
  expect(fails_with(R"({"a":{"b":{"c":{"d":{}}}}})", secure_frontend::JsonErrorCode::depth_exceeded),
         "object nesting counts the same way");
}

void json_rejects_non_integer_numbers() {
  for (const char* text : {"1.0", "8080.0", "1e3", "1E3", "-0.5", "[2.5]"}) {
    expect(fails_with(text, secure_frontend::JsonErrorCode::non_integer_number), std::string{"non-integer: "} + text);
  }
  for (const char* text : {"+1", "01", "-", ".5"}) {
    expect(fails_with(text, secure_frontend::JsonErrorCode::syntax), std::string{"malformed number: "} + text);
  }
  // The reader takes "0" as a complete number and then finds "x10" after the document.
  expect(fails_with("0x10", secure_frontend::JsonErrorCode::trailing_data), "hex is not a JSON number: 0x10");
  expect(fails_with("9223372036854775808", secure_frontend::JsonErrorCode::integer_out_of_range), "int64 overflow");
}

void json_rejects_bom_trailing_data_and_comments() {
  expect(fails_with("\xEF\xBB\xBF{}", secure_frontend::JsonErrorCode::byte_order_mark), "UTF-8 BOM");
  expect(fails_with("{} {}", secure_frontend::JsonErrorCode::trailing_data), "two documents");
  expect(fails_with(R"({"a":1,})", secure_frontend::JsonErrorCode::syntax), "trailing comma");
  expect(fails_with("{\"a\":1 // c\n}", secure_frontend::JsonErrorCode::syntax), "comment");
  expect(fails_with("{\"a\":\"\x01\"}", secure_frontend::JsonErrorCode::invalid_string), "raw control byte");
  expect(fails_with(R"({"a":"\ud800"})", secure_frontend::JsonErrorCode::invalid_string), "lone surrogate");
  expect(fails_with("{\"a\":\"\xFF\"}", secure_frontend::JsonErrorCode::invalid_string), "invalid UTF-8");
}

void json_enforces_size_entry_and_string_limits() {
  const secure_frontend::JsonLimits tiny{.max_bytes = 8, .max_depth = 4, .max_container_entries = 2, .max_string_bytes = 3};
  expect(fails_with("[1,2,3,4,5]", secure_frontend::JsonErrorCode::too_large, tiny), "document size");
  expect(fails_with("[1,2,3]", secure_frontend::JsonErrorCode::too_many_entries, tiny), "entry count");
  expect(fails_with(R"(["abcd"])", secure_frontend::JsonErrorCode::string_too_long, tiny), "string length");
}

void json_string_escaper_round_trips() {
  std::string out;
  secure_frontend::append_json_string(&out, std::string{"q\"b\\n\n\x01"});
  expect(out == "\"q\\\"b\\\\n\\n\\u0001\"", "escaper output: " + out);
  const auto parsed = secure_frontend::parse_json(out, kLimits);
  const auto* value = std::get_if<secure_frontend::JsonValue>(&parsed);
  expect(value != nullptr && value->if_string() != nullptr && *value->if_string() == std::string{"q\"b\\n\n\x01"},
         "escaped text parses back to the original");
}
}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"json_accepts_strict_document", json_accepts_strict_document},
      {"json_rejects_duplicate_keys", json_rejects_duplicate_keys},
      {"json_rejects_depth_over_limit", json_rejects_depth_over_limit},
      {"json_rejects_non_integer_numbers", json_rejects_non_integer_numbers},
      {"json_rejects_bom_trailing_data_and_comments", json_rejects_bom_trailing_data_and_comments},
      {"json_enforces_size_entry_and_string_limits", json_enforces_size_entry_and_string_limits},
      {"json_string_escaper_round_trips", json_string_escaper_round_trips},
  }, failures);
}
```

Add `#include <cstdint>` for `INT64_MIN`. CMake (inside the test block, after the chat-schema test):

```cmake
    secure_frontend_add_test_executable(secure_frontend_json_tests tests/json_test.cpp secure_frontend_json)
    secure_frontend_add_cases(secure_frontend_json_tests
        json_accepts_strict_document json_rejects_duplicate_keys json_rejects_depth_over_limit
        json_rejects_non_integer_numbers json_rejects_bom_trailing_data_and_comments
        json_enforces_size_entry_and_string_limits json_string_escaper_round_trips)
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --preset linux-gcc && cmake --build --preset linux-gcc 2>&1 | grep -m1 error`
Expected: `fatal error: secure_frontend/json.hpp: No such file or directory`.

- [ ] **Step 3: Implement**

`include/secure_frontend/json.hpp`:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace secure_frontend {

enum class JsonKind { null, boolean, integer, string, array, object };

struct JsonMember;

// An immutable JSON value produced by parse_json. Numbers are integers only:
// a fraction or exponent is a parse error, never a silent conversion.
class JsonValue final {
 public:
  JsonValue() = default;
  static JsonValue make_boolean(bool value);
  static JsonValue make_integer(std::int64_t value);
  static JsonValue make_string(std::string value);
  static JsonValue make_array(std::vector<JsonValue> items);
  static JsonValue make_object(std::vector<JsonMember> members);

  [[nodiscard]] JsonKind kind() const noexcept { return kind_; }
  [[nodiscard]] const bool* if_boolean() const noexcept { return kind_ == JsonKind::boolean ? &boolean_ : nullptr; }
  [[nodiscard]] const std::int64_t* if_integer() const noexcept { return kind_ == JsonKind::integer ? &integer_ : nullptr; }
  [[nodiscard]] const std::string* if_string() const noexcept { return kind_ == JsonKind::string ? &string_ : nullptr; }
  [[nodiscard]] const std::vector<JsonValue>* if_array() const noexcept { return kind_ == JsonKind::array ? &array_ : nullptr; }
  [[nodiscard]] const std::vector<JsonMember>* if_object() const noexcept { return kind_ == JsonKind::object ? &object_ : nullptr; }

 private:
  JsonKind kind_{JsonKind::null};
  bool boolean_{false};
  std::int64_t integer_{0};
  std::string string_;
  std::vector<JsonValue> array_;
  std::vector<JsonMember> object_;  // document order; keys are unique
};

struct JsonMember final {
  std::string key;
  JsonValue value;
};

// Every bound is enforced while parsing; nothing is allocated past it.
// Depth counts containers: a top-level object or array is depth 1.
struct JsonLimits final {
  std::size_t max_bytes;
  std::size_t max_depth;
  std::size_t max_container_entries;
  std::size_t max_string_bytes;
};

enum class JsonErrorCode {
  too_large, byte_order_mark, syntax, depth_exceeded, too_many_entries, string_too_long,
  invalid_string, duplicate_key, non_integer_number, integer_out_of_range, trailing_data,
};

// `offset` is a byte offset into the input; errors never carry input text.
struct JsonError final {
  JsonErrorCode code;
  std::size_t offset;
};

using JsonParseResult = std::variant<JsonValue, JsonError>;

// Strict RFC 8259 subset: UTF-8 without BOM, no comments, no trailing commas,
// duplicate object keys rejected (compared after unescaping), integers only
// (int64), strings must be valid UTF-8 with no raw control bytes and no lone
// surrogates, exactly one value plus whitespace.
[[nodiscard]] JsonParseResult parse_json(std::string_view text, const JsonLimits& limits);
[[nodiscard]] std::string_view describe_json_error(JsonErrorCode code) noexcept;

// Appends `value` as a quoted JSON string ("\"", "\\", \b \f \n \r \t, other
// bytes < 0x20 as \u00XX; everything else, including UTF-8, verbatim).
void append_json_string(std::string* output, std::string_view value);

}  // namespace secure_frontend
```

`src/json.cpp`:

```cpp
#include "secure_frontend/json.hpp"

#include <algorithm>
#include <limits>
#include <numeric>
#include <utility>

namespace secure_frontend {

JsonValue JsonValue::make_boolean(const bool value) { JsonValue v; v.kind_ = JsonKind::boolean; v.boolean_ = value; return v; }
JsonValue JsonValue::make_integer(const std::int64_t value) { JsonValue v; v.kind_ = JsonKind::integer; v.integer_ = value; return v; }
JsonValue JsonValue::make_string(std::string value) { JsonValue v; v.kind_ = JsonKind::string; v.string_ = std::move(value); return v; }
JsonValue JsonValue::make_array(std::vector<JsonValue> items) { JsonValue v; v.kind_ = JsonKind::array; v.array_ = std::move(items); return v; }
JsonValue JsonValue::make_object(std::vector<JsonMember> members) { JsonValue v; v.kind_ = JsonKind::object; v.object_ = std::move(members); return v; }

namespace {

bool is_continuation(const unsigned char byte) { return (byte & 0xc0U) == 0x80U; }

bool valid_utf8(const std::string_view value) {
  for (std::size_t i = 0; i < value.size();) {
    const auto b = static_cast<unsigned char>(value[i]);
    const auto at = [&](const std::size_t k) { return static_cast<unsigned char>(value[i + k]); };
    const std::size_t left = value.size() - i;
    if (b <= 0x7fU) { i += 1U; continue; }
    if (b >= 0xc2U && b <= 0xdfU && left >= 2U && is_continuation(at(1))) { i += 2U; continue; }
    if (b == 0xe0U && left >= 3U && at(1) >= 0xa0U && at(1) <= 0xbfU && is_continuation(at(2))) { i += 3U; continue; }
    if (((b >= 0xe1U && b <= 0xecU) || b == 0xeeU || b == 0xefU) && left >= 3U && is_continuation(at(1)) &&
        is_continuation(at(2))) { i += 3U; continue; }
    if (b == 0xedU && left >= 3U && at(1) >= 0x80U && at(1) <= 0x9fU && is_continuation(at(2))) { i += 3U; continue; }
    if (b == 0xf0U && left >= 4U && at(1) >= 0x90U && at(1) <= 0xbfU && is_continuation(at(2)) && is_continuation(at(3))) { i += 4U; continue; }
    if (b >= 0xf1U && b <= 0xf3U && left >= 4U && is_continuation(at(1)) && is_continuation(at(2)) && is_continuation(at(3))) { i += 4U; continue; }
    if (b == 0xf4U && left >= 4U && at(1) >= 0x80U && at(1) <= 0x8fU && is_continuation(at(2)) && is_continuation(at(3))) { i += 4U; continue; }
    return false;
  }
  return true;
}

void append_utf8(std::string* out, const std::uint32_t point) {
  if (point <= 0x7fU) {
    out->push_back(static_cast<char>(point));
  } else if (point <= 0x7ffU) {
    out->push_back(static_cast<char>(0xc0U | (point >> 6U)));
    out->push_back(static_cast<char>(0x80U | (point & 0x3fU)));
  } else if (point <= 0xffffU) {
    out->push_back(static_cast<char>(0xe0U | (point >> 12U)));
    out->push_back(static_cast<char>(0x80U | ((point >> 6U) & 0x3fU)));
    out->push_back(static_cast<char>(0x80U | (point & 0x3fU)));
  } else {
    out->push_back(static_cast<char>(0xf0U | (point >> 18U)));
    out->push_back(static_cast<char>(0x80U | ((point >> 12U) & 0x3fU)));
    out->push_back(static_cast<char>(0x80U | ((point >> 6U) & 0x3fU)));
    out->push_back(static_cast<char>(0x80U | (point & 0x3fU)));
  }
}

class Parser final {
 public:
  Parser(const std::string_view text, const JsonLimits& limits) : text_(text), limits_(limits) {}

  JsonParseResult run() {
    if (text_.size() > limits_.max_bytes) return JsonError{JsonErrorCode::too_large, 0U};
    if (text_.size() >= 3U && static_cast<unsigned char>(text_[0]) == 0xefU &&
        static_cast<unsigned char>(text_[1]) == 0xbbU && static_cast<unsigned char>(text_[2]) == 0xbfU) {
      return JsonError{JsonErrorCode::byte_order_mark, 0U};
    }
    JsonValue value;
    if (!parse_value(0U, value)) return error_;
    skip();
    if (position_ != text_.size()) return JsonError{JsonErrorCode::trailing_data, position_};
    return value;
  }

 private:
  bool fail(const JsonErrorCode code, const std::size_t offset) {
    error_ = JsonError{code, offset};
    return false;
  }
  bool fail(const JsonErrorCode code) { return fail(code, position_); }
  bool at_end() const { return position_ >= text_.size(); }
  void skip() {
    while (!at_end() && (text_[position_] == ' ' || text_[position_] == '\t' || text_[position_] == '\n' ||
                         text_[position_] == '\r')) ++position_;
  }
  bool literal(const std::string_view word) {
    if (text_.substr(position_, word.size()) != word) return false;
    position_ += word.size();
    return true;
  }

  bool parse_value(const std::size_t depth, JsonValue& out) {
    skip();
    if (at_end()) return fail(JsonErrorCode::syntax);
    const char c = text_[position_];
    if (c == '{' || c == '[') {
      if (depth + 1U > limits_.max_depth) return fail(JsonErrorCode::depth_exceeded);
      return c == '{' ? parse_object(depth + 1U, out) : parse_array(depth + 1U, out);
    }
    if (c == '"') {
      std::string text;
      if (!parse_string(text)) return false;
      out = JsonValue::make_string(std::move(text));
      return true;
    }
    if (c == '-' || (c >= '0' && c <= '9')) return parse_number(out);
    if (literal("true")) { out = JsonValue::make_boolean(true); return true; }
    if (literal("false")) { out = JsonValue::make_boolean(false); return true; }
    if (literal("null")) { out = JsonValue{}; return true; }
    return fail(JsonErrorCode::syntax);
  }

  bool parse_number(JsonValue& out) {
    const std::size_t start = position_;
    const bool negative = text_[position_] == '-';
    if (negative) ++position_;
    if (at_end() || text_[position_] < '0' || text_[position_] > '9') return fail(JsonErrorCode::syntax, start);
    const std::size_t digits_start = position_;
    if (text_[position_] == '0') {
      ++position_;
    } else {
      while (!at_end() && text_[position_] >= '0' && text_[position_] <= '9') ++position_;
    }
    if (!at_end() && (text_[position_] == '.' || text_[position_] == 'e' || text_[position_] == 'E')) {
      return fail(JsonErrorCode::non_integer_number, start);
    }
    if (!at_end() && text_[position_] >= '0' && text_[position_] <= '9') return fail(JsonErrorCode::syntax, start);
    std::uint64_t magnitude = 0U;
    for (std::size_t i = digits_start; i < position_; ++i) {
      const auto digit = static_cast<std::uint64_t>(text_[i] - '0');
      if (magnitude > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
        return fail(JsonErrorCode::integer_out_of_range, start);
      }
      magnitude = magnitude * 10U + digit;
    }
    constexpr std::uint64_t kMaxPositive = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (magnitude > kMaxPositive + (negative ? 1U : 0U)) return fail(JsonErrorCode::integer_out_of_range, start);
    std::int64_t value = 0;
    if (negative) {
      value = magnitude == kMaxPositive + 1U ? std::numeric_limits<std::int64_t>::min()
                                             : -static_cast<std::int64_t>(magnitude);
    } else {
      value = static_cast<std::int64_t>(magnitude);
    }
    out = JsonValue::make_integer(value);
    return true;
  }

  bool hex4(std::uint32_t& out) {
    if (text_.size() - position_ < 4U) return fail(JsonErrorCode::invalid_string);
    out = 0U;
    for (int n = 0; n < 4; ++n) {
      const char c = text_[position_++];
      std::uint32_t digit = 0U;
      if (c >= '0' && c <= '9') digit = static_cast<std::uint32_t>(c - '0');
      else if (c >= 'a' && c <= 'f') digit = static_cast<std::uint32_t>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') digit = static_cast<std::uint32_t>(c - 'A' + 10);
      else return fail(JsonErrorCode::invalid_string);
      out = (out << 4U) | digit;
    }
    return true;
  }

  bool parse_string(std::string& out) {
    const std::size_t start = position_;
    ++position_;  // opening quote
    while (!at_end()) {
      const auto byte = static_cast<unsigned char>(text_[position_++]);
      if (byte == '"') return valid_utf8(out) ? true : fail(JsonErrorCode::invalid_string, start);
      if (byte < 0x20U) return fail(JsonErrorCode::invalid_string);
      if (byte != '\\') {
        out.push_back(static_cast<char>(byte));
      } else {
        if (at_end()) return fail(JsonErrorCode::invalid_string);
        switch (text_[position_++]) {
          case '"': out.push_back('"'); break;
          case '\\': out.push_back('\\'); break;
          case '/': out.push_back('/'); break;
          case 'b': out.push_back('\b'); break;
          case 'f': out.push_back('\f'); break;
          case 'n': out.push_back('\n'); break;
          case 'r': out.push_back('\r'); break;
          case 't': out.push_back('\t'); break;
          case 'u': {
            std::uint32_t point = 0U;
            if (!hex4(point)) return false;
            if (point >= 0xd800U && point <= 0xdbffU) {
              std::uint32_t low = 0U;
              if (text_.substr(position_, 2U) != "\\u") return fail(JsonErrorCode::invalid_string);
              position_ += 2U;
              if (!hex4(low)) return false;
              if (low < 0xdc00U || low > 0xdfffU) return fail(JsonErrorCode::invalid_string);
              point = 0x10000U + ((point - 0xd800U) << 10U) + (low - 0xdc00U);
            } else if (point >= 0xdc00U && point <= 0xdfffU) {
              return fail(JsonErrorCode::invalid_string);
            }
            append_utf8(&out, point);
            break;
          }
          default: return fail(JsonErrorCode::invalid_string);
        }
      }
      if (out.size() > limits_.max_string_bytes) return fail(JsonErrorCode::string_too_long, start);
    }
    return fail(JsonErrorCode::invalid_string, start);
  }

  bool parse_array(const std::size_t depth, JsonValue& out) {
    ++position_;
    std::vector<JsonValue> items;
    skip();
    if (!at_end() && text_[position_] == ']') { ++position_; out = JsonValue::make_array({}); return true; }
    while (true) {
      if (items.size() == limits_.max_container_entries) return fail(JsonErrorCode::too_many_entries);
      JsonValue item;
      if (!parse_value(depth, item)) return false;
      items.push_back(std::move(item));
      skip();
      if (at_end()) return fail(JsonErrorCode::syntax);
      if (text_[position_] == ']') { ++position_; break; }
      if (text_[position_] != ',') return fail(JsonErrorCode::syntax);
      ++position_;
    }
    out = JsonValue::make_array(std::move(items));
    return true;
  }

  bool parse_object(const std::size_t depth, JsonValue& out) {
    ++position_;
    std::vector<JsonMember> members;
    std::vector<std::size_t> key_offsets;
    skip();
    if (!at_end() && text_[position_] == '}') { ++position_; out = JsonValue::make_object({}); return true; }
    while (true) {
      if (members.size() == limits_.max_container_entries) return fail(JsonErrorCode::too_many_entries);
      skip();
      if (at_end() || text_[position_] != '"') return fail(JsonErrorCode::syntax);
      key_offsets.push_back(position_);
      JsonMember member;
      if (!parse_string(member.key)) return false;
      skip();
      if (at_end() || text_[position_] != ':') return fail(JsonErrorCode::syntax);
      ++position_;
      if (!parse_value(depth, member.value)) return false;
      members.push_back(std::move(member));
      skip();
      if (at_end()) return fail(JsonErrorCode::syntax);
      if (text_[position_] == '}') { ++position_; break; }
      if (text_[position_] != ',') return fail(JsonErrorCode::syntax);
      ++position_;
    }
    std::vector<std::size_t> order(members.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::sort(order.begin(), order.end(), [&](const std::size_t a, const std::size_t b) {
      return members[a].key < members[b].key || (members[a].key == members[b].key && a < b);
    });
    for (std::size_t i = 1U; i < order.size(); ++i) {
      if (members[order[i]].key == members[order[i - 1U]].key) {
        return fail(JsonErrorCode::duplicate_key, key_offsets[order[i]]);
      }
    }
    out = JsonValue::make_object(std::move(members));
    return true;
  }

  std::string_view text_;
  const JsonLimits& limits_;
  std::size_t position_{0U};
  JsonError error_{JsonErrorCode::syntax, 0U};
};

}  // namespace

JsonParseResult parse_json(const std::string_view text, const JsonLimits& limits) { return Parser(text, limits).run(); }

std::string_view describe_json_error(const JsonErrorCode code) noexcept {
  switch (code) {
    case JsonErrorCode::too_large: return "is larger than the size limit";
    case JsonErrorCode::byte_order_mark: return "starts with a UTF-8 byte order mark";
    case JsonErrorCode::syntax: return "is not valid JSON";
    case JsonErrorCode::depth_exceeded: return "is nested too deeply";
    case JsonErrorCode::too_many_entries: return "has too many entries in one object or array";
    case JsonErrorCode::string_too_long: return "has a string longer than the limit";
    case JsonErrorCode::invalid_string: return "has an invalid string (control byte, bad escape or invalid UTF-8)";
    case JsonErrorCode::duplicate_key: return "has a duplicate key";
    case JsonErrorCode::non_integer_number: return "has a number that is not an integer";
    case JsonErrorCode::integer_out_of_range: return "has an integer outside the 64-bit range";
    case JsonErrorCode::trailing_data: return "has data after the document";
  }
  return "is not valid JSON";
}

void append_json_string(std::string* const output, const std::string_view value) {
  static constexpr char kHex[] = "0123456789abcdef";
  output->push_back('"');
  for (const unsigned char byte : value) {
    switch (byte) {
      case '"': output->append("\\\""); break;
      case '\\': output->append("\\\\"); break;
      case '\b': output->append("\\b"); break;
      case '\f': output->append("\\f"); break;
      case '\n': output->append("\\n"); break;
      case '\r': output->append("\\r"); break;
      case '\t': output->append("\\t"); break;
      default:
        if (byte < 0x20U) {
          output->append("\\u00");
          output->push_back(kHex[(byte >> 4U) & 0x0fU]);
          output->push_back(kHex[byte & 0x0fU]);
        } else {
          output->push_back(static_cast<char>(byte));
        }
    }
  }
  output->push_back('"');
}

}  // namespace secure_frontend
```

`src/chat_schema.cpp`: add `#include "secure_frontend/json.hpp"` and `#include <variant>`; delete lines 94-110 (`append_utf8`), 112-299 (`class Parser`) and 301-324 (`append_json_string`, now in the JSON library); replace lines 328-330 with:

```cpp
std::optional<ChatCompletionRequest> parse_chat_completion_request(std::string_view json) {
  if (json.empty() || json.size() > kMaximumRequestBytes) return std::nullopt;
  // Depth 3: request object -> messages array -> message object. Deeper
  // input (content arrays, tool objects) fails closed at the reader.
  const JsonLimits limits{.max_bytes = kMaximumRequestBytes, .max_depth = 3U,
                          .max_container_entries = kMaximumMessages, .max_string_bytes = kMaximumRequestBytes};
  const auto parsed = parse_json(json, limits);
  const auto* root = std::get_if<JsonValue>(&parsed);
  const auto* members = root != nullptr ? root->if_object() : nullptr;
  if (members == nullptr) return std::nullopt;
  ChatCompletionRequest request;
  bool model_seen = false;
  bool messages_seen = false;
  for (const auto& member : *members) {
    if (member.key == "model") {
      const auto* value = member.value.if_string();
      if (value == nullptr || !valid_model(*value)) return std::nullopt;
      request.model = *value;
      model_seen = true;
    } else if (member.key == "messages") {
      const auto* items = member.value.if_array();
      if (items == nullptr || items->size() > kMaximumMessages) return std::nullopt;
      for (const auto& item : *items) {
        const auto* fields = item.if_object();
        if (fields == nullptr) return std::nullopt;
        ChatMessage message;
        bool role_seen = false;
        bool content_seen = false;
        for (const auto& field : *fields) {
          const auto* text = field.value.if_string();
          if (text == nullptr) return std::nullopt;
          if (field.key == "role" && (*text == "system" || *text == "user" || *text == "assistant")) {
            message.role = *text;
            role_seen = true;
          } else if (field.key == "content" && valid_content(*text)) {
            message.content = *text;
            content_seen = true;
          } else {
            return std::nullopt;
          }
        }
        if (!role_seen || !content_seen) return std::nullopt;
        request.messages.push_back(std::move(message));
      }
      messages_seen = true;
    } else if (member.key == "max_tokens") {
      const auto* value = member.value.if_integer();
      if (value == nullptr || *value < 0 || static_cast<std::uint64_t>(*value) > kMaximumTokens) return std::nullopt;
      request.max_tokens = static_cast<std::size_t>(*value);
    } else if (member.key == "stream") {
      const auto* value = member.value.if_boolean();
      if (value == nullptr || *value) return std::nullopt;
    } else {
      return std::nullopt;
    }
  }
  if (!model_seen || !messages_seen || request.messages.empty()) return std::nullopt;
  return request;
}
```

`render_chat_completion_response` keeps calling `append_json_string(&response, …)`, now the library's.

`src/tokenizer_pack.cpp`: add `#include "secure_frontend/json.hpp"` and `#include <variant>`; delete lines 67-158 (`class Json`); add before `}  // namespace` that closes the anonymous namespace:

```cpp
// One extra entry so an oversized vocabulary still reaches the
// invalid_vocabulary check below with its own error code.
constexpr JsonLimits kPackJsonLimits{.max_bytes = kMaximumFileBytes, .max_depth = 1U,
                                     .max_container_entries = kMaximumVocabularyEntries + 1U,
                                     .max_string_bytes = kMaximumTokenBytes};

std::optional<std::map<std::string, std::size_t>> vocabulary_from(const JsonParseResult& parsed) {
  const auto* root = std::get_if<JsonValue>(&parsed);
  const auto* members = root != nullptr ? root->if_object() : nullptr;
  if (members == nullptr) return std::nullopt;
  std::map<std::string, std::size_t> result;
  for (const auto& member : *members) {
    const auto* id = member.value.if_integer();
    if (id == nullptr || *id < 0) return std::nullopt;
    result.emplace(member.key, static_cast<std::size_t>(*id));
  }
  return result;
}

std::optional<std::vector<int>> token_types_from(const JsonParseResult& parsed) {
  const auto* root = std::get_if<JsonValue>(&parsed);
  const auto* items = root != nullptr ? root->if_array() : nullptr;
  if (items == nullptr || items->size() > kMaximumVocabularyEntries) return std::nullopt;
  std::vector<int> result;
  result.reserve(items->size());
  for (const auto& item : *items) {
    const auto* value = item.if_integer();
    if (value == nullptr || *value < std::numeric_limits<int>::min() || *value > std::numeric_limits<int>::max()) {
      return std::nullopt;
    }
    result.push_back(static_cast<int>(*value));
  }
  return result;
}
```

and replace lines 258-259 with:

```cpp
  const auto vocabulary = vocabulary_from(parse_json(std::get<std::string>(vocab_text), kPackJsonLimits));
  const auto types = token_types_from(parse_json(std::get<std::string>(types_text), kPackJsonLimits));
```

CMake: add `add_library(secure_frontend_json src/json.cpp)` next to the other `add_library` calls, with

```cmake
target_compile_features(secure_frontend_json PUBLIC cxx_std_20)
target_include_directories(secure_frontend_json PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)
target_link_libraries(secure_frontend_chat_schema PUBLIC secure_frontend_json)
target_link_libraries(secure_frontend_tokenizer_pack PUBLIC secure_frontend_json)
```

and `secure_frontend_json` in the `foreach(_target …)` warnings list (line 261).

- [ ] **Step 4: Run to verify it passes**

Run: `cmake --build --preset linux-gcc && ctest --preset linux-gcc | tail -3`
Expected: `100% tests passed, 0 tests failed out of 123` — the seven `secure_frontend_json_tests.*` cases plus the unchanged chat-schema, chat-generation-plan, GLM-profile and tokenizer-pack cases.
Run: `cmake --build --preset windows-llvm-mingw-cross | tail -1 && ctest --preset windows-llvm-mingw-cross | tail -1`
Expected: build ok; `0 tests failed out of 114`.

- [ ] **Step 5: Commit**

```bash
git -C "$(git rev-parse --show-toplevel)" add secure_frontend
git -C "$(git rev-parse --show-toplevel)" -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "json: one strict bounded reader for chat schema and tokenizer pack (SP1 C1)" \
  -m "Regression tests: json_rejects_duplicate_keys, json_rejects_depth_over_limit,
json_rejects_non_integer_numbers (+ BOM/trailing/size cases). Chat-schema and tokenizer-pack
suites pass unchanged on the shared reader." \
  -m "$SP1_TRAILERS"
```

---

### Task 4: Typed configuration model, schema table and `validate()` (C2)

**Files:**
- Create: `secure_frontend/include/secure_frontend/config_defaults.hpp`
- Create: `secure_frontend/include/secure_frontend/frontend_config.hpp`, `secure_frontend/src/frontend_config.cpp`
- Create: `secure_frontend/include/secure_frontend/config_file.hpp`, `secure_frontend/src/config_file.cpp`
- Create: `secure_frontend/tests/support/config_json.hpp`, `secure_frontend/tests/typed_config_test.cpp`
- Modify: `secure_frontend/include/secure_frontend/engine_arguments.hpp:42-45`, `secure_frontend/src/engine_arguments.cpp:19-50` (extract `validate_engine_user_arguments`)
- Modify: `secure_frontend/src/secret_source.cpp:43-54` (environment names capped at 128 characters, spec §4)
- Modify: `secure_frontend/CMakeLists.txt` (library `secure_frontend_typed_config`, test binary)

**Interfaces:**
- Consumes: `IpAddress`/`parse_listener_address` (Task 1); `parse_json`, `append_json_string`, `JsonLimits` (Task 3); `validate_model_id` (`model_inventory.hpp`); `make_environment_secret_source`, `make_protected_file_secret_source` (`secret_source.hpp`).
- Produces (Tasks 5–24):
```cpp
namespace secure_frontend {
enum class ConfigValueType { boolean, integer, string, string_array };
struct ConfigKey final {
  std::string_view path; ConfigValueType type; bool required; std::int64_t minimum; std::int64_t maximum;
  std::int64_t default_integer; bool default_boolean; std::string_view default_string;
  std::string_view flag; std::string_view description;
};
[[nodiscard]] std::span<const ConfigKey> config_schema() noexcept;
[[nodiscard]] const ConfigKey* find_config_key(std::string_view path) noexcept;
[[nodiscard]] bool is_config_section(std::string_view path) noexcept;
[[nodiscard]] std::string_view type_reason(ConfigValueType type) noexcept;

enum class ConfigSource { file, command_line };
using ConfigDraftValue = std::variant<bool, std::int64_t, std::string, std::vector<std::string>>;
struct ConfigDraftEntry final { ConfigDraftValue value; ConfigSource source; };
struct FrontendConfigDraft final {
  std::map<std::string, ConfigDraftEntry, std::less<>> entries;   // absent key == "not set"
  std::optional<std::filesystem::path> config_directory;          // set when a file was read
  std::vector<std::string> overridden_by_command_line;
};
enum class ConfigIssueCode { malformed_document, unknown_key, duplicate_key, wrong_type, missing_required,
  out_of_range, invalid_value, conflicting_values, remote_not_enabled, wildcard_bind_not_enabled,
  file_unavailable, file_untrusted };
struct ConfigIssue final { std::string key_path; ConfigIssueCode code; std::string reason; };
using ConfigErrors = std::vector<ConfigIssue>;
[[nodiscard]] std::string format_issue(const ConfigIssue& issue);   // "<key>: <reason>"

enum class EngineProfile { qwen, glm53 };
class ModelId final { public: [[nodiscard]] const std::string& value() const noexcept; };
[[nodiscard]] std::optional<ModelId> make_model_id(std::string_view text);
struct ListenerConfig final { IpAddress host; std::uint16_t port; bool allow_remote; bool allow_wildcard_bind; std::uint32_t backlog; };
struct HttpLimits final { std::chrono::milliseconds request_deadline; std::uint32_t max_request_bytes; std::uint32_t max_header_bytes; std::uint32_t max_headers; std::uint32_t max_request_line_bytes; };
struct ChatLimits final { std::uint32_t max_body_bytes; std::uint32_t max_messages; std::uint32_t max_message_bytes; std::uint32_t max_prompt_tokens; std::uint32_t max_completion_tokens; std::uint32_t default_max_tokens; std::uint32_t max_generated_bytes; };
struct EngineLimits final { std::chrono::seconds ready_timeout; std::chrono::seconds generation_timeout; std::chrono::seconds stop_drain_timeout; };
struct ConcurrencyLimits final { std::uint32_t max_concurrent_requests; };
struct Limits final { HttpLimits http; ChatLimits chat; EngineLimits engine; ConcurrencyLimits concurrency; };
struct Roots final { std::filesystem::path models; std::filesystem::path tokenizer_pack; std::optional<std::filesystem::path> secret; std::optional<std::filesystem::path> logs; std::optional<std::filesystem::path> cache; };   // Task 9: CanonicalRoot
struct EngineLaunch final { EngineProfile profile; std::filesystem::path executable; ModelId model_id; std::vector<std::string> args; };  // Task 9: CanonicalFile
class FrontendConfig final { public: const ListenerConfig listener; const Limits limits; const Roots roots; const SecretSource token; const EngineLaunch engine; };
class ValidatedConfig final { public: [[nodiscard]] const FrontendConfig& get() const noexcept; [[nodiscard]] std::shared_ptr<const FrontendConfig> shared() const noexcept; };
using ConfigValidationResult = std::variant<ValidatedConfig, ConfigErrors>;
[[nodiscard]] ConfigValidationResult validate(const FrontendConfigDraft& draft);
[[nodiscard]] std::string describe(const FrontendConfig& config);   // "key = value\n" per schema key, never a token
[[nodiscard]] Config listener_config(const FrontendConfig& config);
// config_file.hpp
inline constexpr std::size_t kMaxConfigFileBytes = 64U * 1024U;
[[nodiscard]] std::variant<FrontendConfigDraft, ConfigErrors> parse_config_text(std::string_view text, const std::filesystem::path& config_directory);
// engine_arguments.hpp
[[nodiscard]] std::optional<EngineArgumentsError> validate_engine_user_arguments(EngineArgumentsProfile profile, const std::vector<std::string>& user_arguments);
}
namespace secure_frontend::config_defaults { /* one inline constexpr per tuning default, see Step 3 */ }
```
  The draft is the spec's "`FrontendConfigDraft` (all fields `std::optional`)" expressed as one map keyed by schema path, so every rule can be table-driven over every key.

- [ ] **Step 1: Test support for config documents** (`tests/support/config_json.hpp`)

```cpp
#pragma once
#include "secure_frontend/json.hpp"
#include "support/test_support.hpp"

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace secure_frontend_test {

inline std::string json_literal(const std::string& text) {
  std::string out;
  secure_frontend::append_json_string(&out, text);
  return out;
}

// Renders {"a":{"b":1}} from {{"a.b","1"}} (values are JSON literals). The key
// path named by `duplicate` is written twice, for duplicate-key tests.
inline std::string render_config_json(const std::map<std::string, std::string>& literals,
                                      const std::string& duplicate = {}) {
  struct Node {
    std::vector<std::string> names;
    std::vector<Node> children;
    std::string literal;
  };
  Node root;
  for (const auto& [path, literal] : literals) {
    Node* node = &root;
    std::size_t start = 0;
    while (true) {
      const auto dot = path.find('.', start);
      const std::string name = path.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
      std::size_t index = 0;
      while (index < node->names.size() && node->names[index] != name) ++index;
      if (index == node->names.size()) {
        node->names.push_back(name);
        node->children.emplace_back();
      }
      node = &node->children[index];
      if (dot == std::string::npos) break;
      start = dot + 1;
    }
    node->literal = literal;
  }
  std::function<void(const Node&, const std::string&, std::string&)> emit =
      [&](const Node& node, const std::string& prefix, std::string& out) {
        out += '{';
        for (std::size_t i = 0; i < node.names.size(); ++i) {
          const std::string path = prefix.empty() ? node.names[i] : prefix + "." + node.names[i];
          for (int copy = 0; copy < (path == duplicate ? 2 : 1); ++copy) {
            if (out.back() != '{') out += ',';
            out += json_literal(node.names[i]) + ':';
            if (node.children[i].names.empty()) out += node.children[i].literal;
            else emit(node.children[i], path, out);
          }
        }
        out += '}';
      };
  std::string out;
  emit(root, "", out);
  return out;
}

// A valid minimal configuration over real, owner-only directories: models,
// tokenizer pack, secret root and an engine file (mode 0700).
struct ConfigFixture final {
  std::filesystem::path dir = make_scratch_directory();
  std::map<std::string, std::string> literals;

  ConfigFixture() {
    for (const char* name : {"models", "tok", "secret", "bin"}) make_private_directory(dir / name);
    write_file(dir / "bin" / "engine", "not a real engine\n", 0700);
    literals = {{"schema_version", "1"},
                {"roots.models", json_literal((dir / "models").string())},
                {"roots.tokenizer_pack", json_literal((dir / "tok").string())},
                {"roots.secret", json_literal((dir / "secret").string())},
                {"engine.executable", json_literal((dir / "bin" / "engine").string())},
                {"engine.model_id", json_literal("qwen-local")}};
  }
  [[nodiscard]] std::string document(const std::string& duplicate = {}) const {
    return render_config_json(literals, duplicate);
  }
};

}  // namespace secure_frontend_test
```

- [ ] **Step 2: Write the failing tests** (`tests/typed_config_test.cpp`)

```cpp
#include "secure_frontend/config_file.hpp"
#include "secure_frontend/frontend_config.hpp"
#include "support/config_json.hpp"
#include "support/test_support.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <variant>
#include <vector>

namespace {
int failures = 0;
void expect(const bool condition, const std::string& message) {
  if (!condition) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
}
using secure_frontend::ConfigIssueCode;
using secure_frontend::ConfigValueType;
using secure_frontend_test::ConfigFixture;
using secure_frontend_test::render_config_json;

secure_frontend::ConfigValidationResult validate_text(const std::string& text, const std::filesystem::path& dir) {
  auto draft = secure_frontend::parse_config_text(text, dir);
  if (auto* errors = std::get_if<secure_frontend::ConfigErrors>(&draft)) return *errors;
  return secure_frontend::validate(std::get<secure_frontend::FrontendConfigDraft>(draft));
}

bool has_issue(const secure_frontend::ConfigValidationResult& result, const std::string& key, const ConfigIssueCode code) {
  const auto* errors = std::get_if<secure_frontend::ConfigErrors>(&result);
  return errors != nullptr && std::any_of(errors->begin(), errors->end(), [&](const auto& issue) {
           return issue.key_path == key && issue.code == code;
         });
}

std::string issues_of(const secure_frontend::ConfigValidationResult& result) {
  std::string text;
  if (const auto* errors = std::get_if<secure_frontend::ConfigErrors>(&result)) {
    for (const auto& issue : *errors) text += secure_frontend::format_issue(issue) + "; ";
  }
  return text;
}

std::string valid_sample(const secure_frontend::ConfigKey& key) {
  switch (key.type) {
    case ConfigValueType::integer: return std::to_string(key.minimum);
    case ConfigValueType::boolean: return "false";
    case ConfigValueType::string_array: return "[]";
    case ConfigValueType::string: return key.default_string.empty() ? "\"x\"" : "\"" + std::string{key.default_string} + "\"";
  }
  return "null";
}

std::vector<std::string> wrong_type_samples(const ConfigValueType type) {
  switch (type) {
    case ConfigValueType::integer: return {"\"8080\"", "true", "[1]", "null"};
    case ConfigValueType::boolean: return {"1", "\"true\"", "null"};
    case ConfigValueType::string: return {"1", "true", "[\"x\"]", "{}"};
    case ConfigValueType::string_array: return {"\"x\"", "[1]", "{}", "true"};
  }
  return {};
}

void typed_config_accepts_minimal_file_and_applies_defaults() {
  ConfigFixture f;
  const auto result = validate_text(f.document(), f.dir);
  const auto* validated = std::get_if<secure_frontend::ValidatedConfig>(&result);
  expect(validated != nullptr, "the minimal config must validate: " + issues_of(result));
  if (validated == nullptr) return;
  const auto& c = validated->get();
  expect(c.listener.host.canonical_text() == "127.0.0.1" && c.listener.port == 8080U && !c.listener.allow_remote &&
             !c.listener.allow_wildcard_bind && c.listener.backlog == 16U, "listener defaults");
  expect(c.limits.http.request_deadline == std::chrono::milliseconds(30000) && c.limits.http.max_request_bytes == 65536U &&
             c.limits.http.max_header_bytes == 16384U && c.limits.http.max_headers == 64U &&
             c.limits.http.max_request_line_bytes == 4096U, "http limit defaults");
  expect(c.limits.chat.max_body_bytes == 49152U && c.limits.chat.max_messages == 128U &&
             c.limits.chat.max_completion_tokens == 4096U && c.limits.chat.default_max_tokens == 256U, "chat defaults");
  expect(c.limits.engine.ready_timeout == std::chrono::seconds(900), "ready_timeout defaults to 900 s");
  expect(c.limits.concurrency.max_concurrent_requests == 1U, "one request at a time until SP3");
  expect(c.token.kind() == secure_frontend::SecretSourceKind::protected_file && c.token.location() == "token",
         "token.source defaults to file:token");
  expect(c.engine.profile == secure_frontend::EngineProfile::qwen && c.engine.model_id.value() == "qwen-local",
         "engine defaults");
}

void cfg01_unknown_key_rejected() {
  ConfigFixture f;
  std::vector<std::string> unknown{"bogus", "listener.bogus", "limits.bogus", "limits.http.bogus", "limits.chat.bogus",
                                   "limits.engine.bogus", "limits.concurrency.bogus", "roots.bogus", "token.bogus",
                                   "engine.bogus"};
  for (const auto& key : secure_frontend::config_schema()) unknown.push_back(std::string{key.path} + "_typo");
  for (const auto& path : unknown) {
    auto literals = f.literals;
    literals[path] = "1";
    const auto result = validate_text(render_config_json(literals), f.dir);
    expect(has_issue(result, path, ConfigIssueCode::unknown_key), "unknown key must be rejected by name: " + path);
  }
}

void cfg01_duplicate_key_rejected() {
  ConfigFixture f;
  for (const auto& key : secure_frontend::config_schema()) {
    auto literals = f.literals;
    const std::string path{key.path};
    if (!literals.contains(path)) literals[path] = valid_sample(key);
    const auto result = validate_text(render_config_json(literals, path), f.dir);
    expect(has_issue(result, "config file", ConfigIssueCode::duplicate_key), "duplicate must be rejected: " + path);
  }
}

void cfg01_type_confusion_rejected() {
  ConfigFixture f;
  for (const auto& key : secure_frontend::config_schema()) {
    const std::string path{key.path};
    for (const auto& sample : wrong_type_samples(key.type)) {
      auto literals = f.literals;
      literals[path] = sample;
      const auto result = validate_text(render_config_json(literals), f.dir);
      expect(has_issue(result, path, ConfigIssueCode::wrong_type), "type confusion " + sample + " for " + path);
    }
    if (key.type == ConfigValueType::integer) {
      for (const char* number : {"8080.0", "1e3"}) {
        auto literals = f.literals;
        literals[path] = number;
        const auto result = validate_text(render_config_json(literals), f.dir);
        expect(has_issue(result, "config file", ConfigIssueCode::malformed_document),
               std::string{"a float is never coerced: "} + number + " for " + path);
      }
    }
  }
}

void cfg01_missing_required_rejected() {
  ConfigFixture f;
  std::vector<std::string> required{"schema_version"};
  for (const auto& key : secure_frontend::config_schema()) if (key.required) required.emplace_back(key.path);
  expect(required.size() == 5U, "schema_version, roots.models, roots.tokenizer_pack, engine.executable, engine.model_id");
  for (const auto& path : required) {
    auto literals = f.literals;
    literals.erase(path);
    const auto result = validate_text(render_config_json(literals), f.dir);
    expect(has_issue(result, path, ConfigIssueCode::missing_required), "missing required key: " + path);
  }
}

void cfg01_out_of_range_rejected() {
  ConfigFixture f;
  for (const auto& key : secure_frontend::config_schema()) {
    if (key.type != ConfigValueType::integer) continue;
    for (const auto value : {key.minimum - 1, key.maximum + 1}) {
      auto literals = f.literals;
      literals[std::string{key.path}] = std::to_string(value);
      const auto result = validate_text(render_config_json(literals), f.dir);
      expect(has_issue(result, std::string{key.path}, ConfigIssueCode::out_of_range),
             "out of range " + std::to_string(value) + " for " + std::string{key.path});
    }
  }
}

void typed_config_cross_field_rules() {
  ConfigFixture f;
  const auto with = [&](std::map<std::string, std::string> extra) {
    auto literals = f.literals;
    for (auto& [k, v] : extra) literals[k] = v;
    return validate_text(render_config_json(literals), f.dir);
  };
  expect(has_issue(with({{"limits.http.max_request_bytes", "8192"}, {"limits.http.max_header_bytes", "16384"}}),
                   "limits.http.max_header_bytes", ConfigIssueCode::conflicting_values), "header > request");
  expect(has_issue(with({{"limits.http.max_header_bytes", "2048"}, {"limits.http.max_request_line_bytes", "4096"}}),
                   "limits.http.max_request_line_bytes", ConfigIssueCode::conflicting_values), "line > header");
  expect(has_issue(with({{"limits.chat.max_body_bytes", "262144"}}), "limits.chat.max_body_bytes",
                   ConfigIssueCode::conflicting_values), "body > request - header");
  expect(has_issue(with({{"limits.chat.default_max_tokens", "5000"}}), "limits.chat.default_max_tokens",
                   ConfigIssueCode::conflicting_values), "default_max_tokens > max_completion_tokens");
  expect(has_issue(with({{"engine.profile", "\"glm53\""}}), "engine.args", ConfigIssueCode::missing_required),
         "glm53 needs at least one engine argument");
  expect(has_issue(with({{"engine.args", "[\"--native\",\"x\"]"}}), "engine.args", ConfigIssueCode::invalid_value),
         "the SP0 deny-list applies to engine.args");
  expect(has_issue(with({{"listener.host", "\"192.0.2.10\""}}), "listener.host", ConfigIssueCode::remote_not_enabled),
         "remote needs listener.allow_remote");
  expect(has_issue(with({{"listener.host", "\"::0\""}, {"listener.allow_remote", "true"}}), "listener.host",
                   ConfigIssueCode::wildcard_bind_not_enabled), "wildcard needs its own opt-in");
  expect(has_issue(with({{"token.source", "\"cmd:echo\""}}), "token.source", ConfigIssueCode::invalid_value),
         "token.source is env:NAME or file:PATH");
  expect(has_issue(with({{"roots.models", "\"relative/models\""}}), "roots.models", ConfigIssueCode::invalid_value),
         "roots must be absolute");
}

void typed_config_errors_never_contain_values() {
  ConfigFixture f;
  auto literals = f.literals;
  const std::string secret_like = "sk-PASTED-SECRET-0123456789";
  literals["listener.host"] = secure_frontend_test::json_literal(secret_like);
  literals["token.source"] = secure_frontend_test::json_literal("env:" + secret_like + "!");
  literals["engine.model_id"] = secure_frontend_test::json_literal(secret_like + "/x");
  const auto text = issues_of(validate_text(render_config_json(literals), f.dir));
  expect(!text.empty() && text.find(secret_like) == std::string::npos, "issues name keys, never values: " + text);
}
}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"typed_config_accepts_minimal_file_and_applies_defaults", typed_config_accepts_minimal_file_and_applies_defaults},
      {"cfg01_unknown_key_rejected", cfg01_unknown_key_rejected},
      {"cfg01_duplicate_key_rejected", cfg01_duplicate_key_rejected},
      {"cfg01_type_confusion_rejected", cfg01_type_confusion_rejected},
      {"cfg01_missing_required_rejected", cfg01_missing_required_rejected},
      {"cfg01_out_of_range_rejected", cfg01_out_of_range_rejected},
      {"typed_config_cross_field_rules", typed_config_cross_field_rules},
      {"typed_config_errors_never_contain_values", typed_config_errors_never_contain_values},
  }, failures);
}
```

CMake test registration:

```cmake
    secure_frontend_add_test_executable(secure_frontend_typed_config_tests tests/typed_config_test.cpp
        secure_frontend_typed_config)
    secure_frontend_add_cases(secure_frontend_typed_config_tests
        typed_config_accepts_minimal_file_and_applies_defaults cfg01_unknown_key_rejected cfg01_duplicate_key_rejected
        cfg01_type_confusion_rejected cfg01_missing_required_rejected cfg01_out_of_range_rejected
        typed_config_cross_field_rules typed_config_errors_never_contain_values)
```

- [ ] **Step 3: Run to verify it fails**

Run: `cmake --preset linux-gcc && cmake --build --preset linux-gcc 2>&1 | grep -m1 error`
Expected: `fatal error: secure_frontend/config_file.hpp: No such file or directory`.

- [ ] **Step 4: Implement**

`include/secure_frontend/config_defaults.hpp`:

```cpp
#pragma once
#include <cstdint>

// The one place every tunable default lives (SP1 §4). The schema table and
// every subsystem struct default read these; no other file may hardcode a
// tuning limit. Internal format/protocol constants are listed in
// docs/security/engine-protocol-limits.md and are not here.
namespace secure_frontend::config_defaults {
inline constexpr std::int64_t kSchemaVersion = 1;
inline constexpr std::int64_t kListenerPort = 8080;
inline constexpr std::int64_t kListenerBacklog = 16;
inline constexpr std::int64_t kRequestDeadlineMs = 30'000;
inline constexpr std::int64_t kMaxRequestBytes = 65'536;
inline constexpr std::int64_t kMaxHeaderBytes = 16'384;
inline constexpr std::int64_t kMaxHeaders = 64;
inline constexpr std::int64_t kMaxRequestLineBytes = 4'096;
inline constexpr std::int64_t kChatMaxBodyBytes = kMaxRequestBytes - kMaxHeaderBytes;  // 49152, see the plan's Global Constraints
inline constexpr std::int64_t kChatMaxMessages = 128;
inline constexpr std::int64_t kChatMaxMessageBytes = 65'536;
inline constexpr std::int64_t kChatMaxPromptTokens = 32'768;
inline constexpr std::int64_t kChatMaxCompletionTokens = 4'096;
inline constexpr std::int64_t kChatDefaultMaxTokens = 256;
inline constexpr std::int64_t kChatMaxGeneratedBytes = 1'048'576;
inline constexpr std::int64_t kEngineReadyTimeoutS = 900;   // upstream STRATA_ENGINE_READY_S
inline constexpr std::int64_t kEngineGenerationTimeoutS = 120;
inline constexpr std::int64_t kEngineStopDrainTimeoutS = 5;
inline constexpr std::int64_t kMaxConcurrentRequests = 1;
}  // namespace secure_frontend::config_defaults
```

`include/secure_frontend/frontend_config.hpp`:

```cpp
#pragma once

#include "secure_frontend/config.hpp"
#include "secure_frontend/secret_source.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace secure_frontend {

enum class ConfigValueType { boolean, integer, string, string_array };

// One row per configuration key: the single source of truth for the JSON
// reader, the command line, --check output and the README key table.
struct ConfigKey final {
  std::string_view path;
  ConfigValueType type;
  bool required;
  std::int64_t minimum;          // integer keys
  std::int64_t maximum;
  std::int64_t default_integer;
  bool default_boolean;
  std::string_view default_string;  // empty = no default
  std::string_view flag;            // strata-secure-server option(s) for the README; empty = file only
  std::string_view description;
};

[[nodiscard]] std::span<const ConfigKey> config_schema() noexcept;
[[nodiscard]] const ConfigKey* find_config_key(std::string_view path) noexcept;
// True for an object path that prefixes at least one key ("limits", "limits.http").
[[nodiscard]] bool is_config_section(std::string_view path) noexcept;
[[nodiscard]] std::string_view type_reason(ConfigValueType type) noexcept;

enum class ConfigSource { file, command_line };
using ConfigDraftValue = std::variant<bool, std::int64_t, std::string, std::vector<std::string>>;
struct ConfigDraftEntry final {
  ConfigDraftValue value;
  ConfigSource source;
};

// Parsing produces this mutable draft. A key absent from `entries` is "not
// set"; values are stored exactly as written (no coercion) for validate().
struct FrontendConfigDraft final {
  std::map<std::string, ConfigDraftEntry, std::less<>> entries;
  std::optional<std::filesystem::path> config_directory;
  std::vector<std::string> overridden_by_command_line;
};

enum class ConfigIssueCode {
  malformed_document, unknown_key, duplicate_key, wrong_type, missing_required, out_of_range, invalid_value,
  conflicting_values, remote_not_enabled, wildcard_bind_not_enabled, file_unavailable, file_untrusted,
};

// key_path names a key ("limits.http.max_request_bytes") or "config file";
// reason is fixed text. Neither ever contains a configured value.
struct ConfigIssue final {
  std::string key_path;
  ConfigIssueCode code;
  std::string reason;
};
using ConfigErrors = std::vector<ConfigIssue>;
[[nodiscard]] std::string format_issue(const ConfigIssue& issue);

enum class EngineProfile { qwen, glm53 };

class ModelId final {
 public:
  [[nodiscard]] const std::string& value() const noexcept { return value_; }

 private:
  friend std::optional<ModelId> make_model_id(std::string_view text);
  explicit ModelId(std::string value) : value_(std::move(value)) {}
  std::string value_;
};
[[nodiscard]] std::optional<ModelId> make_model_id(std::string_view text);

struct ListenerConfig final {
  IpAddress host;
  std::uint16_t port;
  bool allow_remote;
  bool allow_wildcard_bind;
  std::uint32_t backlog;
};
struct HttpLimits final {
  std::chrono::milliseconds request_deadline;
  std::uint32_t max_request_bytes;
  std::uint32_t max_header_bytes;
  std::uint32_t max_headers;
  std::uint32_t max_request_line_bytes;
};
struct ChatLimits final {
  std::uint32_t max_body_bytes;
  std::uint32_t max_messages;
  std::uint32_t max_message_bytes;
  std::uint32_t max_prompt_tokens;
  std::uint32_t max_completion_tokens;
  std::uint32_t default_max_tokens;
  std::uint32_t max_generated_bytes;
};
struct EngineLimits final {
  std::chrono::seconds ready_timeout;
  std::chrono::seconds generation_timeout;
  std::chrono::seconds stop_drain_timeout;
};
struct ConcurrencyLimits final {
  std::uint32_t max_concurrent_requests;
};
struct Limits final {
  HttpLimits http;
  ChatLimits chat;
  EngineLimits engine;
  ConcurrencyLimits concurrency;
};
struct Roots final {
  std::filesystem::path models;
  std::filesystem::path tokenizer_pack;
  std::optional<std::filesystem::path> secret;
  std::optional<std::filesystem::path> logs;
  std::optional<std::filesystem::path> cache;
};
struct EngineLaunch final {
  EngineProfile profile;
  std::filesystem::path executable;
  ModelId model_id;
  std::vector<std::string> args;
};

class ValidatedConfig;
class FrontendConfig final {
 public:
  const ListenerConfig listener;
  const Limits limits;
  const Roots roots;
  const SecretSource token;  // metadata only: env name or relative file path (CFG-06)
  const EngineLaunch engine;

 private:
  friend std::variant<ValidatedConfig, std::vector<ConfigIssue>> validate(const FrontendConfigDraft& draft);
  FrontendConfig(ListenerConfig listener_value, Limits limits_value, Roots roots_value, SecretSource token_value,
                 EngineLaunch engine_value)
      : listener(std::move(listener_value)), limits(limits_value), roots(std::move(roots_value)),
        token(std::move(token_value)), engine(std::move(engine_value)) {}
};

// Immutable after validation; subsystems derive their limits from it once.
class ValidatedConfig final {
 public:
  [[nodiscard]] const FrontendConfig& get() const noexcept { return *config_; }
  [[nodiscard]] std::shared_ptr<const FrontendConfig> shared() const noexcept { return config_; }

 private:
  friend std::variant<ValidatedConfig, std::vector<ConfigIssue>> validate(const FrontendConfigDraft& draft);
  explicit ValidatedConfig(std::shared_ptr<const FrontendConfig> config) : config_(std::move(config)) {}
  std::shared_ptr<const FrontendConfig> config_;
};

using ConfigValidationResult = std::variant<ValidatedConfig, ConfigErrors>;

// Collects every issue (all keys, all rules) before returning.
[[nodiscard]] ConfigValidationResult validate(const FrontendConfigDraft& draft);
// "key = value" per schema key in schema order. Holds no secret by construction.
[[nodiscard]] std::string describe(const FrontendConfig& config);
// The listener primitive HttpSocketServer::bind takes.
[[nodiscard]] Config listener_config(const FrontendConfig& config);

}  // namespace secure_frontend
```

`src/frontend_config.cpp`:

```cpp
#include "secure_frontend/frontend_config.hpp"

#include "secure_frontend/config_defaults.hpp"
#include "secure_frontend/engine_arguments.hpp"
#include "secure_frontend/json.hpp"
#include "secure_frontend/model_inventory.hpp"

#include <algorithm>
#include <array>

namespace secure_frontend {
namespace {
namespace d = config_defaults;
using T = ConfigValueType;

constexpr std::array<ConfigKey, 32> kSchema{{
    {"schema_version", T::integer, false, 1, 1, d::kSchemaVersion, false, "", "",
     "Config format version; must be 1. Required in a config file."},
    {"listener.host", T::string, false, 0, 0, 0, false, "127.0.0.1", "--host",
     "Bind address: an IPv4/IPv6 literal or localhost. Other hostnames are rejected (never resolved)."},
    {"listener.port", T::integer, false, 1, 65535, d::kListenerPort, false, "", "--port", "TCP port."},
    {"listener.allow_remote", T::boolean, false, 0, 0, 0, false, "", "--allow-remote",
     "Required for every non-loopback address."},
    {"listener.allow_wildcard_bind", T::boolean, false, 0, 0, 0, false, "", "--allow-wildcard-bind",
     "Also required for 0.0.0.0, :: and every other spelling of the unspecified address."},
    {"listener.backlog", T::integer, false, 1, 4096, d::kListenerBacklog, false, "", "", "listen() backlog."},
    {"limits.http.request_deadline_ms", T::integer, false, 1, 3'600'000, d::kRequestDeadlineMs, false, "", "",
     "Total time to read one request, header and body."},
    {"limits.http.max_request_bytes", T::integer, false, 4096, 16'777'216, d::kMaxRequestBytes, false, "", "",
     "Largest request (header and body) the listener and parser accept."},
    {"limits.http.max_header_bytes", T::integer, false, 1024, 65'536, d::kMaxHeaderBytes, false, "", "",
     "Largest header block; at most limits.http.max_request_bytes."},
    {"limits.http.max_headers", T::integer, false, 8, 256, d::kMaxHeaders, false, "", "", "Most header fields."},
    {"limits.http.max_request_line_bytes", T::integer, false, 256, 16'384, d::kMaxRequestLineBytes, false, "", "",
     "Longest request line; at most limits.http.max_header_bytes."},
    {"limits.chat.max_body_bytes", T::integer, false, 1, 16'777'216, d::kChatMaxBodyBytes, false, "", "",
     "Largest chat request body; at most max_request_bytes minus max_header_bytes."},
    {"limits.chat.max_messages", T::integer, false, 1, 1024, d::kChatMaxMessages, false, "", "",
     "Most messages in one chat request."},
    {"limits.chat.max_message_bytes", T::integer, false, 1, 16'777'216, d::kChatMaxMessageBytes, false, "", "",
     "Largest single message content."},
    {"limits.chat.max_prompt_tokens", T::integer, false, 1, 1'048'576, d::kChatMaxPromptTokens, false, "", "",
     "Largest tokenized prompt sent to the engine."},
    {"limits.chat.max_completion_tokens", T::integer, false, 1, 16'384, d::kChatMaxCompletionTokens, false, "", "",
     "Largest max_tokens a request may ask for."},
    {"limits.chat.default_max_tokens", T::integer, false, 1, 16'384, d::kChatDefaultMaxTokens, false, "", "",
     "max_tokens when a request omits it; at most limits.chat.max_completion_tokens."},
    {"limits.chat.max_generated_bytes", T::integer, false, 1, 16'777'216, d::kChatMaxGeneratedBytes, false, "", "",
     "Largest generated text returned."},
    {"limits.engine.ready_timeout_s", T::integer, false, 1, 86'400, d::kEngineReadyTimeoutS, false, "", "",
     "How long the first request waits for the engine's READY (upstream STRATA_ENGINE_READY_S)."},
    {"limits.engine.generation_timeout_s", T::integer, false, 1, 86'400, d::kEngineGenerationTimeoutS, false, "", "",
     "Total time for one generation before STOP."},
    {"limits.engine.stop_drain_timeout_s", T::integer, false, 1, 600, d::kEngineStopDrainTimeoutS, false, "", "",
     "Time to drain to DONE after STOP before the engine is marked failed."},
    {"limits.concurrency.max_concurrent_requests", T::integer, false, 1, 1, d::kMaxConcurrentRequests, false, "", "",
     "Requests served at once; only 1 until SP3."},
    {"roots.models", T::string, true, 0, 0, 0, false, "", "--models-root",
     "Absolute directory of .gguf files. The root itself may be a symlink; nothing below it may be."},
    {"roots.tokenizer_pack", T::string, true, 0, 0, 0, false, "", "--tokenizer-pack",
     "Absolute directory with vocab.json, merges.txt, token_type.json."},
    {"roots.secret", T::string, false, 0, 0, 0, false, "", "--secret-root",
     "Absolute directory holding the token file. Default: the config file's directory."},
    {"roots.logs", T::string, false, 0, 0, 0, false, "", "", "Absolute log directory (created 0700; unused in SP1)."},
    {"roots.cache", T::string, false, 0, 0, 0, false, "", "", "Absolute cache directory (created 0700; unused in SP1)."},
    {"token.source", T::string, false, 0, 0, 0, false, "file:token", "--token-file, --token-env",
     "file:<relative path below roots.secret> or env:<NAME>. Metadata only; the token is never in the config."},
    {"engine.profile", T::string, false, 0, 0, 0, false, "qwen", "--profile", "qwen or glm53."},
    {"engine.executable", T::string, true, 0, 0, 0, false, "", "--engine",
     "Absolute path of the engine; a regular file not writable by group or others."},
    {"engine.model_id", T::string, true, 0, 0, 0, false, "", "--model-id", "Model ID from the local inventory."},
    {"engine.args", T::string_array, false, 0, 0, 0, false, "", "--engine-arg (repeatable)",
     "Extra engine arguments; frontend-owned and unsupported flags are rejected."},
}};

void add(ConfigErrors& errors, std::string key, const ConfigIssueCode code, std::string reason) {
  errors.push_back(ConfigIssue{std::move(key), code, std::move(reason)});
}

// Reads one key with its schema type, range and default; records issues.
class DraftReader final {
 public:
  DraftReader(const FrontendConfigDraft& draft, ConfigErrors& errors) : draft_(draft), errors_(errors) {}

  std::int64_t integer(const std::string_view path) {
    const ConfigKey& key = *find_config_key(path);
    const auto* entry = lookup(key);
    if (entry == nullptr) return key.default_integer;
    const auto* value = std::get_if<std::int64_t>(&entry->value);
    if (value == nullptr) return wrong_type(key), key.default_integer;
    if (*value < key.minimum || *value > key.maximum) {
      add(errors_, std::string{path}, ConfigIssueCode::out_of_range,
          "must be between " + std::to_string(key.minimum) + " and " + std::to_string(key.maximum));
      return key.default_integer;
    }
    return *value;
  }
  bool boolean(const std::string_view path) {
    const ConfigKey& key = *find_config_key(path);
    const auto* entry = lookup(key);
    if (entry == nullptr) return key.default_boolean;
    const auto* value = std::get_if<bool>(&entry->value);
    if (value == nullptr) return wrong_type(key), key.default_boolean;
    return *value;
  }
  std::optional<std::string> string(const std::string_view path) {
    const ConfigKey& key = *find_config_key(path);
    const auto* entry = lookup(key);
    if (entry == nullptr) {
      if (key.default_string.empty()) return std::nullopt;
      return std::string{key.default_string};
    }
    const auto* value = std::get_if<std::string>(&entry->value);
    if (value == nullptr) return wrong_type(key), std::nullopt;
    return *value;
  }
  std::vector<std::string> strings(const std::string_view path) {
    const ConfigKey& key = *find_config_key(path);
    const auto* entry = lookup(key);
    if (entry == nullptr) return {};
    const auto* value = std::get_if<std::vector<std::string>>(&entry->value);
    if (value == nullptr) return wrong_type(key), std::vector<std::string>{};
    return *value;
  }
  bool present(const std::string_view path) const { return draft_.entries.find(path) != draft_.entries.end(); }

 private:
  const ConfigDraftEntry* lookup(const ConfigKey& key) {
    const auto it = draft_.entries.find(key.path);
    if (it != draft_.entries.end()) return &it->second;
    if (key.required) add(errors_, std::string{key.path}, ConfigIssueCode::missing_required, "is required");
    return nullptr;
  }
  void wrong_type(const ConfigKey& key) {
    add(errors_, std::string{key.path}, ConfigIssueCode::wrong_type, std::string{type_reason(key.type)});
  }
  const FrontendConfigDraft& draft_;
  ConfigErrors& errors_;
};

std::optional<std::filesystem::path> absolute_root(DraftReader& read, ConfigErrors& errors, const std::string_view path) {
  const auto text = read.string(path);
  if (!text.has_value()) return std::nullopt;
  const std::filesystem::path value{*text};
  if (text->empty() || text->find('\0') != std::string::npos || !value.is_absolute()) {
    add(errors, std::string{path}, ConfigIssueCode::invalid_value, "must be an absolute path");
    return std::nullopt;
  }
  return value;
}

std::optional<SecretSource> token_source(const std::string& text) {
  if (text.rfind("env:", 0) == 0) {
    auto result = make_environment_secret_source(std::string_view{text}.substr(4));
    if (auto* source = std::get_if<SecretSource>(&result)) return std::move(*source);
    return std::nullopt;
  }
  if (text.rfind("file:", 0) == 0) {
    auto result = make_protected_file_secret_source(std::string_view{text}.substr(5));
    if (auto* source = std::get_if<SecretSource>(&result)) return std::move(*source);
  }
  return std::nullopt;
}

std::string quoted(const std::string_view text) {
  std::string out;
  append_json_string(&out, text);
  return out;
}

}  // namespace

std::span<const ConfigKey> config_schema() noexcept { return kSchema; }

const ConfigKey* find_config_key(const std::string_view path) noexcept {
  for (const auto& key : kSchema) if (key.path == path) return &key;
  return nullptr;
}

bool is_config_section(const std::string_view path) noexcept {
  return std::any_of(kSchema.begin(), kSchema.end(), [path](const ConfigKey& key) {
    return key.path.size() > path.size() && key.path.substr(0, path.size()) == path && key.path[path.size()] == '.';
  });
}

std::string_view type_reason(const ConfigValueType type) noexcept {
  switch (type) {
    case ConfigValueType::boolean: return "must be true or false";
    case ConfigValueType::integer: return "must be an integer";
    case ConfigValueType::string: return "must be a string";
    case ConfigValueType::string_array: return "must be an array of strings";
  }
  return "has the wrong type";
}

std::string format_issue(const ConfigIssue& issue) { return issue.key_path + ": " + issue.reason; }

std::optional<ModelId> make_model_id(const std::string_view text) {
  if (!std::holds_alternative<std::string>(validate_model_id(text))) return std::nullopt;
  return ModelId{std::string{text}};
}

ConfigValidationResult validate(const FrontendConfigDraft& draft) {
  ConfigErrors errors;
  for (const auto& [path, entry] : draft.entries) {
    if (find_config_key(path) == nullptr) add(errors, path, ConfigIssueCode::unknown_key, "unknown key");
  }
  DraftReader read(draft, errors);
  if (draft.config_directory.has_value() && !read.present("schema_version")) {
    add(errors, "schema_version", ConfigIssueCode::missing_required, "is required in a config file");
  }
  (void)read.integer("schema_version");

  // listener
  const std::string host_text = read.string("listener.host").value_or("127.0.0.1");
  const auto port = read.integer("listener.port");
  const bool allow_remote = read.boolean("listener.allow_remote");
  const bool allow_wildcard = read.boolean("listener.allow_wildcard_bind");
  const auto backlog = read.integer("listener.backlog");
  const auto host = parse_listener_address(host_text);
  if (!host.has_value()) {
    add(errors, "listener.host", ConfigIssueCode::invalid_value,
        "must be an IP literal or localhost; hostnames are never resolved");
  } else if (host->scope() != AddressScope::loopback && !allow_remote) {
    add(errors, "listener.host", ConfigIssueCode::remote_not_enabled,
        "a non-loopback address needs listener.allow_remote (--allow-remote)");
  } else if (host->scope() == AddressScope::wildcard && !allow_wildcard) {
    add(errors, "listener.host", ConfigIssueCode::wildcard_bind_not_enabled,
        "a wildcard address also needs listener.allow_wildcard_bind (--allow-wildcard-bind)");
  }

  // limits
  const auto deadline_ms = read.integer("limits.http.request_deadline_ms");
  const auto max_request = read.integer("limits.http.max_request_bytes");
  const auto max_header = read.integer("limits.http.max_header_bytes");
  const auto max_headers = read.integer("limits.http.max_headers");
  const auto max_line = read.integer("limits.http.max_request_line_bytes");
  const auto max_body = read.integer("limits.chat.max_body_bytes");
  const auto max_messages = read.integer("limits.chat.max_messages");
  const auto max_message = read.integer("limits.chat.max_message_bytes");
  const auto max_prompt = read.integer("limits.chat.max_prompt_tokens");
  const auto max_completion = read.integer("limits.chat.max_completion_tokens");
  const auto default_max = read.integer("limits.chat.default_max_tokens");
  const auto max_generated = read.integer("limits.chat.max_generated_bytes");
  const auto ready_s = read.integer("limits.engine.ready_timeout_s");
  const auto generation_s = read.integer("limits.engine.generation_timeout_s");
  const auto drain_s = read.integer("limits.engine.stop_drain_timeout_s");
  const auto concurrent = read.integer("limits.concurrency.max_concurrent_requests");
  if (max_header > max_request) {
    add(errors, "limits.http.max_header_bytes", ConfigIssueCode::conflicting_values,
        "must not exceed limits.http.max_request_bytes");
  }
  if (max_line > max_header) {
    add(errors, "limits.http.max_request_line_bytes", ConfigIssueCode::conflicting_values,
        "must not exceed limits.http.max_header_bytes");
  }
  if (max_body > max_request - max_header) {
    add(errors, "limits.chat.max_body_bytes", ConfigIssueCode::conflicting_values,
        "must not exceed limits.http.max_request_bytes minus limits.http.max_header_bytes");
  }
  if (default_max > max_completion) {
    add(errors, "limits.chat.default_max_tokens", ConfigIssueCode::conflicting_values,
        "must not exceed limits.chat.max_completion_tokens");
  }

  // roots
  const auto models = absolute_root(read, errors, "roots.models");
  const auto tokenizer = absolute_root(read, errors, "roots.tokenizer_pack");
  auto secret = absolute_root(read, errors, "roots.secret");
  if (!read.present("roots.secret") && draft.config_directory.has_value()) secret = *draft.config_directory;
  const auto logs = absolute_root(read, errors, "roots.logs");
  const auto cache = absolute_root(read, errors, "roots.cache");

  // token
  const std::string source_text = read.string("token.source").value_or("file:token");
  auto token = token_source(source_text);
  if (!token.has_value()) {
    add(errors, "token.source", ConfigIssueCode::invalid_value,
        "must be env:NAME ([A-Za-z_][A-Za-z0-9_]{0,127}) or file:RELATIVE-PATH below roots.secret");
  } else if (token->kind() == SecretSourceKind::protected_file && !secret.has_value() &&
             !read.present("roots.secret")) {
    add(errors, "roots.secret", ConfigIssueCode::missing_required, "is required when token.source is file:…");
  }

  // engine
  const std::string profile_text = read.string("engine.profile").value_or("qwen");
  EngineProfile profile = EngineProfile::qwen;
  if (profile_text == "glm53") profile = EngineProfile::glm53;
  else if (profile_text != "qwen") add(errors, "engine.profile", ConfigIssueCode::invalid_value, "must be qwen or glm53");
  const auto executable = absolute_root(read, errors, "engine.executable");
  const auto model_text = read.string("engine.model_id");
  std::optional<ModelId> model_id = model_text ? make_model_id(*model_text) : std::nullopt;
  if (model_text.has_value() && !model_id.has_value()) {
    add(errors, "engine.model_id", ConfigIssueCode::invalid_value,
        "must be 1-128 characters of [A-Za-z0-9._-], starting with a letter or digit");
  }
  const auto args = read.strings("engine.args");
  const auto arguments_profile = profile == EngineProfile::glm53 ? EngineArgumentsProfile::lighttransport_glm53
                                                                 : EngineArgumentsProfile::strata_0143;
  if (const auto rejected = validate_engine_user_arguments(arguments_profile, args)) {
    add(errors, "engine.args", ConfigIssueCode::invalid_value, rejected->message);
  }
  if (profile == EngineProfile::glm53 && args.empty()) {
    add(errors, "engine.args", ConfigIssueCode::missing_required, "--profile glm53 requires at least one --engine-arg");
  }

  if (!errors.empty() || !host || !models || !tokenizer || !token || !executable || !model_id) {
    if (errors.empty()) add(errors, "config file", ConfigIssueCode::invalid_value, "is incomplete");
    return errors;
  }
  const auto u32 = [](const std::int64_t v) { return static_cast<std::uint32_t>(v); };
  ListenerConfig listener{*host, static_cast<std::uint16_t>(port), allow_remote, allow_wildcard, u32(backlog)};
  const Limits limits{
      HttpLimits{std::chrono::milliseconds(deadline_ms), u32(max_request), u32(max_header), u32(max_headers), u32(max_line)},
      ChatLimits{u32(max_body), u32(max_messages), u32(max_message), u32(max_prompt), u32(max_completion), u32(default_max),
                 u32(max_generated)},
      EngineLimits{std::chrono::seconds(ready_s), std::chrono::seconds(generation_s), std::chrono::seconds(drain_s)},
      ConcurrencyLimits{u32(concurrent)}};
  Roots roots{*models, *tokenizer, secret, logs, cache};
  EngineLaunch engine{profile, *executable, std::move(*model_id), args};
  return ValidatedConfig{std::shared_ptr<const FrontendConfig>(
      new FrontendConfig(std::move(listener), limits, std::move(roots), std::move(*token), std::move(engine)))};
}

std::string describe(const FrontendConfig& c) {
  std::string out;
  const auto line = [&out](const std::string_view key, const std::string& value) {
    out.append(key).append(" = ").append(value).push_back('\n');
  };
  const auto path_or_unset = [](const std::optional<std::filesystem::path>& p) {
    return p.has_value() ? quoted(p->string()) : std::string{"(unset)"};
  };
  line("schema_version", "1");
  line("listener.host", quoted(c.listener.host.canonical_text()));
  line("listener.port", std::to_string(c.listener.port));
  line("listener.allow_remote", c.listener.allow_remote ? "true" : "false");
  line("listener.allow_wildcard_bind", c.listener.allow_wildcard_bind ? "true" : "false");
  line("listener.backlog", std::to_string(c.listener.backlog));
  line("limits.http.request_deadline_ms", std::to_string(c.limits.http.request_deadline.count()));
  line("limits.http.max_request_bytes", std::to_string(c.limits.http.max_request_bytes));
  line("limits.http.max_header_bytes", std::to_string(c.limits.http.max_header_bytes));
  line("limits.http.max_headers", std::to_string(c.limits.http.max_headers));
  line("limits.http.max_request_line_bytes", std::to_string(c.limits.http.max_request_line_bytes));
  line("limits.chat.max_body_bytes", std::to_string(c.limits.chat.max_body_bytes));
  line("limits.chat.max_messages", std::to_string(c.limits.chat.max_messages));
  line("limits.chat.max_message_bytes", std::to_string(c.limits.chat.max_message_bytes));
  line("limits.chat.max_prompt_tokens", std::to_string(c.limits.chat.max_prompt_tokens));
  line("limits.chat.max_completion_tokens", std::to_string(c.limits.chat.max_completion_tokens));
  line("limits.chat.default_max_tokens", std::to_string(c.limits.chat.default_max_tokens));
  line("limits.chat.max_generated_bytes", std::to_string(c.limits.chat.max_generated_bytes));
  line("limits.engine.ready_timeout_s", std::to_string(c.limits.engine.ready_timeout.count()));
  line("limits.engine.generation_timeout_s", std::to_string(c.limits.engine.generation_timeout.count()));
  line("limits.engine.stop_drain_timeout_s", std::to_string(c.limits.engine.stop_drain_timeout.count()));
  line("limits.concurrency.max_concurrent_requests", std::to_string(c.limits.concurrency.max_concurrent_requests));
  line("roots.models", quoted(c.roots.models.string()));
  line("roots.tokenizer_pack", quoted(c.roots.tokenizer_pack.string()));
  line("roots.secret", path_or_unset(c.roots.secret));
  line("roots.logs", path_or_unset(c.roots.logs));
  line("roots.cache", path_or_unset(c.roots.cache));
  line("token.source", quoted((c.token.kind() == SecretSourceKind::environment_variable ? "env:" : "file:") +
                              c.token.location()));
  line("engine.profile", c.engine.profile == EngineProfile::glm53 ? "\"glm53\"" : "\"qwen\"");
  line("engine.executable", quoted(c.engine.executable.string()));
  line("engine.model_id", quoted(c.engine.model_id.value()));
  std::string args = "[";
  for (const auto& arg : c.engine.args) args += (args.size() > 1U ? "," : "") + quoted(arg);
  line("engine.args", args + "]");
  return out;
}

Config listener_config(const FrontendConfig& c) {
  return Config{c.listener.host.canonical_text(), c.listener.port, true,
                c.listener.host.scope() == AddressScope::loopback, c.listener.allow_remote,
                c.listener.allow_wildcard_bind};
}

}  // namespace secure_frontend
```

`include/secure_frontend/config_file.hpp`:

```cpp
#pragma once

#include "secure_frontend/frontend_config.hpp"

#include <cstddef>
#include <filesystem>
#include <string_view>
#include <variant>

namespace secure_frontend {

inline constexpr std::size_t kMaxConfigFileBytes = 64U * 1024U;

// Converts config file text into a draft without touching the filesystem.
// Every JSON or shape problem becomes a ConfigIssue; values are never echoed.
[[nodiscard]] std::variant<FrontendConfigDraft, ConfigErrors> parse_config_text(
    std::string_view text, const std::filesystem::path& config_directory);

}  // namespace secure_frontend
```

`src/config_file.cpp`:

```cpp
#include "secure_frontend/config_file.hpp"

#include "secure_frontend/json.hpp"

#include <optional>
#include <string>

namespace secure_frontend {
namespace {

constexpr JsonLimits kConfigJsonLimits{.max_bytes = kMaxConfigFileBytes, .max_depth = 4U,
                                       .max_container_entries = 256U, .max_string_bytes = 4096U};

std::optional<ConfigDraftValue> draft_value(const JsonValue& value) {
  if (const auto* b = value.if_boolean()) return ConfigDraftValue{*b};
  if (const auto* i = value.if_integer()) return ConfigDraftValue{*i};
  if (const auto* s = value.if_string()) return ConfigDraftValue{*s};
  if (const auto* items = value.if_array()) {
    std::vector<std::string> strings;
    for (const auto& item : *items) {
      const auto* s = item.if_string();
      if (s == nullptr) return std::nullopt;
      strings.push_back(*s);
    }
    return ConfigDraftValue{std::move(strings)};
  }
  return std::nullopt;
}

void collect(const std::vector<JsonMember>& members, const std::string& prefix, FrontendConfigDraft& draft,
             ConfigErrors& errors) {
  for (const auto& member : members) {
    const std::string path = prefix.empty() ? member.key : prefix + "." + member.key;
    if (const ConfigKey* key = find_config_key(path)) {
      if (auto value = draft_value(member.value)) {
        draft.entries[path] = ConfigDraftEntry{std::move(*value), ConfigSource::file};
      } else {
        errors.push_back(ConfigIssue{path, ConfigIssueCode::wrong_type, std::string{type_reason(key->type)}});
      }
    } else if (is_config_section(path)) {
      if (const auto* inner = member.value.if_object()) collect(*inner, path, draft, errors);
      else errors.push_back(ConfigIssue{path, ConfigIssueCode::wrong_type, "must be an object"});
    } else {
      errors.push_back(ConfigIssue{path, ConfigIssueCode::unknown_key, "unknown key"});
    }
  }
}

}  // namespace

std::variant<FrontendConfigDraft, ConfigErrors> parse_config_text(const std::string_view text,
                                                                  const std::filesystem::path& config_directory) {
  const auto parsed = parse_json(text, kConfigJsonLimits);
  if (const auto* error = std::get_if<JsonError>(&parsed)) {
    const auto code = error->code == JsonErrorCode::duplicate_key ? ConfigIssueCode::duplicate_key
                                                                  : ConfigIssueCode::malformed_document;
    return ConfigErrors{ConfigIssue{"config file", code,
                                    std::string{describe_json_error(error->code)} + " (byte " +
                                        std::to_string(error->offset) + ")"}};
  }
  const auto* members = std::get<JsonValue>(parsed).if_object();
  if (members == nullptr) {
    return ConfigErrors{ConfigIssue{"config file", ConfigIssueCode::malformed_document, "must be a JSON object"}};
  }
  FrontendConfigDraft draft;
  draft.config_directory = config_directory;
  ConfigErrors errors;
  collect(*members, "", draft, errors);
  if (!errors.empty()) return errors;
  return draft;
}

}  // namespace secure_frontend
```

`engine_arguments`: declare in `engine_arguments.hpp` after line 40:

```cpp
// Checks only the operator's pass-through arguments (no model needed): NUL,
// frontend-owned flags and unsupported modes for `profile`. build_engine_arguments
// calls it; the config validator calls it so a bad engine.args fails before startup.
[[nodiscard]] std::optional<EngineArgumentsError> validate_engine_user_arguments(
    EngineArgumentsProfile profile, const std::vector<std::string>& user_arguments);
```

(`#include <optional>`), and in `engine_arguments.cpp` replace lines 19-50 with:

```cpp
std::optional<EngineArgumentsError> validate_engine_user_arguments(const EngineArgumentsProfile profile,
                                                                   const std::vector<std::string>& user_arguments) {
  for (const auto& argument : user_arguments) {
    if (argument.find('\0') != std::string::npos) {
      return EngineArgumentsError{EngineArgumentsErrorCode::invalid_value, "engine argument contains NUL"};
    }
    const auto name = flag_name(argument);
    if (profile == EngineArgumentsProfile::lighttransport_glm53) {
      if (name == "--serve") {
        return EngineArgumentsError{EngineArgumentsErrorCode::owned_flag, "engine argument is owned by the frontend"};
      }
      continue;
    }
    for (const auto owned : kOwnedFlags) {
      if (name == owned) {
        return EngineArgumentsError{EngineArgumentsErrorCode::owned_flag, "engine argument is owned by the frontend"};
      }
    }
    for (const auto mode : kUnsupportedModes) {
      if (name == mode) {
        return EngineArgumentsError{EngineArgumentsErrorCode::unsupported_mode,
                                    "engine mode is not supported by the frontend"};
      }
    }
  }
  return std::nullopt;
}

EngineArgumentsResult build_engine_arguments(const EngineArgumentsProfile profile, const ModelManifest& selected,
                                             const std::filesystem::path& models_root,
                                             const std::vector<std::string>& user_arguments) {
  if (auto rejected = validate_engine_user_arguments(profile, user_arguments)) return *rejected;
  std::vector<std::string> arguments = user_arguments;
  if (profile == EngineArgumentsProfile::lighttransport_glm53) return arguments;
```

(the remaining lines — artifact-path validation and `--native` — stay as they are).

`src/secret_source.cpp`: in `is_valid_environment_variable_name` (line 43), change the first condition to
`if (name.empty() || name.size() > 128U || !(is_ascii_letter(name.front()) || name.front() == '_')) {`.

CMake:

```cmake
add_library(secure_frontend_typed_config src/frontend_config.cpp src/config_file.cpp)
target_compile_features(secure_frontend_typed_config PUBLIC cxx_std_20)
target_include_directories(secure_frontend_typed_config PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)
target_link_libraries(secure_frontend_typed_config PUBLIC secure_frontend_config secure_frontend_json
    secure_frontend_model_inventory secure_frontend_engine_arguments)
```

plus `secure_frontend_typed_config` in the warnings `foreach`.

- [ ] **Step 5: Run to verify it passes**

Run: `cmake --preset linux-gcc && cmake --build --preset linux-gcc && ctest --preset linux-gcc -R 'typed_config|engine_arguments|secret_source' --output-on-failure | tail -3 && ctest --preset linux-gcc | tail -1`
Expected: every `secure_frontend_typed_config_tests.*` case passes (the eight names above); full run `0 tests failed`.
Run: `cmake --build --preset windows-llvm-mingw-cross | tail -1 && ctest --preset windows-llvm-mingw-cross | tail -1` → build ok, `0 tests failed`.

- [ ] **Step 6: Commit**

```bash
git -C "$(git rev-parse --show-toplevel)" add secure_frontend
git -C "$(git rev-parse --show-toplevel)" -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "config: typed FrontendConfig, schema table and validate() collecting every issue (SP1 C2)" \
  -m "Regression tests (table-driven over every key): cfg01_unknown_key_rejected, cfg01_duplicate_key_rejected,
cfg01_type_confusion_rejected, cfg01_missing_required_rejected, cfg01_out_of_range_rejected, plus
typed_config_cross_field_rules and typed_config_errors_never_contain_values. ready_timeout defaults to 900 s
in the model; limits.chat.max_body_bytes defaults to 49152 to satisfy the spec's own cross-field rule." \
  -m "$SP1_TRAILERS"
```

---
### Task 5: `--config`, `--check`, one validator for file and flags, no value echo (C6, C8) — with the end-to-end harness

**Files:**
- Create: `secure_frontend/include/secure_frontend/server_cli.hpp`, `secure_frontend/src/server_cli.cpp`
- Modify: `secure_frontend/include/secure_frontend/config_file.hpp`, `secure_frontend/src/config_file.cpp` (add `read_config_file`, `merge_drafts`)
- Replace: `secure_frontend/src/secure_server_main.cpp` (whole file, 356 lines)
- Create: `secure_frontend/tests/support/process_runner.hpp`, `secure_frontend/tests/support/http_client.hpp`, `secure_frontend/tests/support/server_fixture.hpp`, `secure_frontend/tests/fake_engine_main.cpp`, `secure_frontend/tests/server_cli_test.cpp`
- Modify: `secure_frontend/CMakeLists.txt` (library `secure_frontend_server_cli`; `strata-secure-server` links it; fake engine; test binary)

**Interfaces:**
- Consumes: `FrontendConfigDraft`, `validate`, `describe`, `format_issue`, `listener_config`, `parse_config_text`, `kMaxConfigFileBytes` (Task 4).
- Produces:
```cpp
namespace secure_frontend {
enum class ServerCommand { serve, check, init, help };
struct ServerInvocation final {
  ServerCommand command{ServerCommand::serve};
  std::optional<std::filesystem::path> config_file;
  bool force{false};
  FrontendConfigDraft flags;               // entries with ConfigSource::command_line
};
struct CliError final { std::string message; };   // never contains an option value
[[nodiscard]] std::variant<ServerInvocation, CliError> parse_server_command_line(int argc, const char* const* argv);
[[nodiscard]] std::string safe_option_name(std::string_view argument);   // up to '=', <= 64 printable bytes
[[nodiscard]] std::string server_usage();
[[nodiscard]] ConfigValidationResult load_server_config(const ServerInvocation& invocation, std::vector<std::string>* overridden);
// config_file.hpp
[[nodiscard]] std::variant<FrontendConfigDraft, ConfigErrors> read_config_file(const std::filesystem::path& file);
[[nodiscard]] FrontendConfigDraft merge_drafts(FrontendConfigDraft file, const FrontendConfigDraft& flags);
}
namespace secure_frontend_test {   // tests/support
struct ProcessSpec final { std::filesystem::path executable; std::vector<std::string> arguments; std::vector<std::pair<std::string, std::string>> environment; };
struct ProcessResult final { int exit_code; std::string out; std::string err; };
class ChildProcess final { public: bool start(const ProcessSpec&); std::optional<int> wait_for(std::chrono::milliseconds); void terminate(); long pid() const noexcept; std::string stdout_text() const; std::string stderr_text() const; };
ProcessResult run_to_completion(const ProcessSpec& spec, std::chrono::milliseconds timeout = std::chrono::seconds(60));
std::string http_exchange(std::uint16_t port, const std::string& request, std::chrono::milliseconds timeout = std::chrono::seconds(30));
bool can_connect(std::uint16_t port);
std::uint16_t pick_free_port();
inline constexpr const char* kTestTokenVariable = "SF_TEST_TOKEN";
inline constexpr std::string_view kTestToken = "sf-e2e-token-0123456789abcdefghijklmnopqrstuv";
class ServerFixture final {   // extra_arguments(): [0] strata-secure-server, [1] fake engine
 public:
  std::filesystem::path dir, models, tokenizer, secret, engine, config, server_executable; std::uint16_t port;
  std::vector<std::string> base_flags() const; std::map<std::string, std::string> base_literals() const;
  std::vector<std::pair<std::string, std::string>> environment() const;
  void write_config(const std::map<std::string, std::string>& literals) const;
  ProcessResult run(const std::vector<std::string>& arguments) const;
  bool start(ChildProcess& child, const std::vector<std::string>& arguments) const;
  std::string request(const std::string& method, const std::string& target, const std::string& authorization, const std::string& body = {}) const;
  std::string bearer() const;
};
bool contains(const std::string& haystack, std::string_view needle);
std::vector<std::string> snapshot_tree(const std::filesystem::path& base);   // type|size|mtime per entry; equal = untouched
}
```
  The fake engine (`secure_frontend_fake_engine`) understands `--marker=PATH` (creates PATH at start), `--dump=PATH` (writes `argv:<arg>` and `env:<k=v>` lines), `--ready-delay-ms=N`; it prints `READY 4096 stop`, answers `GEN n …` with `n` lines `T 0` and `DONE n`, `STOP` with `DONE 0 cancel`, and exits on stdin EOF.

- [ ] **Step 1: Write the harness** (test-only code; nothing to fail yet)

`tests/support/process_runner.hpp`:

```cpp
#pragma once
#include "support/test_support.hpp"

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"
#include <cwchar>
#else
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace secure_frontend_test {

struct ProcessSpec final {
  std::filesystem::path executable;
  std::vector<std::string> arguments;
  std::vector<std::pair<std::string, std::string>> environment;  // the child's whole environment
};

struct ProcessResult final {
  int exit_code;  // -1: killed at the timeout; -2: could not start
  std::string out;
  std::string err;
};

inline bool contains(const std::string& haystack, const std::string_view needle) {
  return haystack.find(needle) != std::string::npos;
}

// stdin is the null device; stdout/stderr go to files in a scratch directory
// (files, not pipes, so a chatty child can never block on a full pipe).
class ChildProcess final {
 public:
  ChildProcess() = default;
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;
  ~ChildProcess() {
    terminate();
#if defined(_WIN32)
    if (process_ != nullptr) CloseHandle(process_);
#endif
  }

  bool start(const ProcessSpec& spec) {
    output_ = make_scratch_directory();
#if defined(_WIN32)
    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);
    HANDLE out = CreateFileW((output_ / "stdout").c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE err = CreateFileW((output_ / "stderr").c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    std::wstring command = quote(spec.executable.wstring());
    for (const auto& argument : spec.arguments) command += L" " + quote(widen(argument));
    std::vector<std::wstring> entries;
    bool has_system_root = false;
    for (const auto& [key, value] : spec.environment) {
      entries.push_back(widen(key) + L"=" + widen(value));
      if (_wcsicmp(widen(key).c_str(), L"SystemRoot") == 0) has_system_root = true;
    }
    wchar_t system_root[MAX_PATH] = {};
    if (!has_system_root && GetEnvironmentVariableW(L"SystemRoot", system_root, MAX_PATH) > 0) {
      entries.push_back(std::wstring{L"SystemRoot="} + system_root);  // Winsock needs it
    }
    std::sort(entries.begin(), entries.end());
    std::wstring block;
    for (const auto& entry : entries) { block += entry; block.push_back(L'\0'); }
    if (entries.empty()) block.push_back(L'\0');
    block.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = in;
    startup.hStdOutput = out;
    startup.hStdError = err;
    PROCESS_INFORMATION info{};
    const BOOL ok = CreateProcessW(spec.executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                                   CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW, block.data(), nullptr, &startup, &info);
    CloseHandle(in);
    CloseHandle(out);
    CloseHandle(err);
    if (ok == 0) return false;
    CloseHandle(info.hThread);
    process_ = info.hProcess;
    return true;
#else
    const std::string out = (output_ / "stdout").string();
    const std::string err = (output_ / "stderr").string();
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, 1, out.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    posix_spawn_file_actions_addopen(&actions, 2, err.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    std::vector<std::string> argv_text{spec.executable.string()};
    argv_text.insert(argv_text.end(), spec.arguments.begin(), spec.arguments.end());
    std::vector<char*> argv;
    for (auto& argument : argv_text) argv.push_back(argument.data());
    argv.push_back(nullptr);
    std::vector<std::string> env_text;
    for (const auto& [key, value] : spec.environment) env_text.push_back(key + "=" + value);
    std::vector<char*> envp;
    for (auto& entry : env_text) envp.push_back(entry.data());
    envp.push_back(nullptr);
    const int rc = posix_spawn(&pid_, argv_text.front().c_str(), &actions, nullptr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    if (rc != 0) pid_ = -1;
    return rc == 0;
#endif
  }

  std::optional<int> wait_for(const std::chrono::milliseconds timeout) {
    if (exit_code_.has_value()) return exit_code_;
#if defined(_WIN32)
    if (process_ == nullptr) return std::nullopt;
    if (WaitForSingleObject(process_, static_cast<DWORD>(timeout.count())) != WAIT_OBJECT_0) return std::nullopt;
    DWORD code = 0;
    GetExitCodeProcess(process_, &code);
    exit_code_ = static_cast<int>(code);
    return exit_code_;
#else
    if (pid_ <= 0) return std::nullopt;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
      int status = 0;
      const pid_t reaped = waitpid(pid_, &status, WNOHANG);
      if (reaped == pid_) {
        exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
        return exit_code_;
      }
      if (reaped < 0 || std::chrono::steady_clock::now() >= deadline) return std::nullopt;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
#endif
  }

  void terminate() {
    if (exit_code_.has_value()) return;
#if defined(_WIN32)
    if (process_ == nullptr) return;
    TerminateProcess(process_, 1);
    (void)wait_for(std::chrono::seconds(5));
#else
    if (pid_ <= 0) return;
    (void)kill(pid_, SIGTERM);
    if (!wait_for(std::chrono::seconds(5)).has_value()) {
      (void)kill(pid_, SIGKILL);
      (void)wait_for(std::chrono::seconds(5));
    }
#endif
  }

  [[nodiscard]] long pid() const noexcept {
#if defined(_WIN32)
    return process_ != nullptr ? static_cast<long>(GetProcessId(process_)) : -1L;
#else
    return static_cast<long>(pid_);
#endif
  }
  [[nodiscard]] std::string stdout_text() const { return read_file(output_ / "stdout"); }
  [[nodiscard]] std::string stderr_text() const { return read_file(output_ / "stderr"); }

 private:
#if defined(_WIN32)
  static std::wstring widen(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
  }
  // CommandLineToArgvW quoting rules.
  static std::wstring quote(const std::wstring& argument) {
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) return argument;
    std::wstring out = L"\"";
    for (auto it = argument.begin();; ++it) {
      std::size_t backslashes = 0;
      while (it != argument.end() && *it == L'\\') { ++it; ++backslashes; }
      if (it == argument.end()) { out.append(backslashes * 2, L'\\'); break; }
      if (*it == L'"') { out.append(backslashes * 2 + 1, L'\\'); out.push_back(L'"'); }
      else { out.append(backslashes, L'\\'); out.push_back(*it); }
    }
    out.push_back(L'"');
    return out;
  }
  HANDLE process_{nullptr};
#else
  pid_t pid_{-1};
#endif
  std::filesystem::path output_;
  std::optional<int> exit_code_;
};

inline ProcessResult run_to_completion(const ProcessSpec& spec,
                                       const std::chrono::milliseconds timeout = std::chrono::seconds(60)) {
  ChildProcess child;
  if (!child.start(spec)) return ProcessResult{-2, {}, "spawn failed"};
  const auto code = child.wait_for(timeout);
  if (!code.has_value()) {
    child.terminate();
    return ProcessResult{-1, child.stdout_text(), child.stderr_text()};
  }
  return ProcessResult{*code, child.stdout_text(), child.stderr_text()};
}

}  // namespace secure_frontend_test
```

`tests/support/http_client.hpp`:

```cpp
#pragma once

#include <chrono>
#include <cstdint>
#include <string>

#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace secure_frontend_test {

#if defined(_WIN32)
using TestSocket = SOCKET;
inline constexpr TestSocket kNoSocket = INVALID_SOCKET;
inline void close_test_socket(const TestSocket s) { closesocket(s); }
inline bool winsock_ready() {
  static const bool started = [] { WSADATA data{}; return WSAStartup(MAKEWORD(2, 2), &data) == 0; }();
  return started;
}
#else
using TestSocket = int;
inline constexpr TestSocket kNoSocket = -1;
inline void close_test_socket(const TestSocket s) { close(s); }
inline bool winsock_ready() { return true; }
#endif

inline TestSocket connect_loopback(const std::uint16_t port, const std::chrono::milliseconds timeout) {
  if (!winsock_ready()) return kNoSocket;
  const TestSocket s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == kNoSocket) return kNoSocket;
#if defined(_WIN32)
  const DWORD ms = static_cast<DWORD>(timeout.count());
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
#else
  const timeval tv{static_cast<time_t>(timeout.count() / 1000), static_cast<suseconds_t>((timeout.count() % 1000) * 1000)};
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
  if (connect(s, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
    close_test_socket(s);
    return kNoSocket;
  }
  return s;
}

inline bool can_connect(const std::uint16_t port) {
  const TestSocket s = connect_loopback(port, std::chrono::seconds(1));
  if (s == kNoSocket) return false;
  close_test_socket(s);
  return true;
}

// Sends one request to 127.0.0.1:<port> and reads to EOF; empty if the connect fails.
inline std::string http_exchange(const std::uint16_t port, const std::string& request,
                                 const std::chrono::milliseconds timeout = std::chrono::seconds(30)) {
  const TestSocket s = connect_loopback(port, timeout);
  if (s == kNoSocket) return {};
  std::size_t sent = 0;
  while (sent < request.size()) {
#if defined(_WIN32)
    const int n = send(s, request.data() + sent, static_cast<int>(request.size() - sent), 0);
#else
    const ssize_t n = send(s, request.data() + sent, request.size() - sent, MSG_NOSIGNAL);
#endif
    if (n <= 0) break;
    sent += static_cast<std::size_t>(n);
  }
  std::string response;
  char chunk[4096];
  for (;;) {
#if defined(_WIN32)
    const int n = recv(s, chunk, static_cast<int>(sizeof(chunk)), 0);
#else
    const ssize_t n = recv(s, chunk, sizeof(chunk), 0);
#endif
    if (n <= 0) break;
    response.append(chunk, static_cast<std::size_t>(n));
  }
  close_test_socket(s);
  return response;
}

// Binds 127.0.0.1:0 to learn a free port, then releases it.
inline std::uint16_t pick_free_port() {
  if (!winsock_ready()) return 0;
  const TestSocket s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
  std::uint16_t port = 0;
  if (bind(s, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0) {
#if defined(_WIN32)
    int length = sizeof(address);
#else
    socklen_t length = sizeof(address);
#endif
    if (getsockname(s, reinterpret_cast<sockaddr*>(&address), &length) == 0) port = ntohs(address.sin_port);
  }
  close_test_socket(s);
  return port;
}

}  // namespace secure_frontend_test
```

`tests/support/server_fixture.hpp`:

```cpp
#pragma once
#include "support/config_json.hpp"
#include "support/http_client.hpp"
#include "support/process_runner.hpp"
#include "support/test_support.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace secure_frontend_test {

// Every entry below `base` with its type, size and modification time, sorted.
// Two equal snapshots mean nothing was created, removed, renamed or written
// (a directory's mtime changes when an entry is added or removed). Used to
// prove that --check has no side effects.
inline std::vector<std::string> snapshot_tree(const std::filesystem::path& base) {
  std::vector<std::string> rows;
  std::error_code error;
  for (auto it = std::filesystem::recursive_directory_iterator(base, error);
       !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
    const auto relative = it->path().lexically_relative(base).generic_u8string();
    std::string row(reinterpret_cast<const char*>(relative.data()), relative.size());
    std::error_code ignored;
    const auto type = it->symlink_status(ignored).type();
    row += '|' + std::to_string(static_cast<int>(type));
    if (type == std::filesystem::file_type::regular) row += '|' + std::to_string(it->file_size(ignored));
    row += '|' + std::to_string(std::filesystem::last_write_time(it->path(), ignored).time_since_epoch().count());
    rows.push_back(std::move(row));
  }
  std::sort(rows.begin(), rows.end());
  return rows;
}

inline constexpr const char* kTestTokenVariable = "SF_TEST_TOKEN";
inline constexpr std::string_view kTestToken = "sf-e2e-token-0123456789abcdefghijklmnopqrstuv";  // 46 bytes

// Printable ASCII, the GPT-2 byte symbols for space and newline, and the two
// Qwen ChatML control tokens: enough to encode any ASCII prompt.
inline void write_test_tokenizer_pack(const std::filesystem::path& root) {
  std::string vocab = "{";
  std::string types = "[";
  std::size_t id = 0;
  const auto add = [&](const std::string& token, const int type) {
    if (id > 0) { vocab += ','; types += ','; }
    vocab += json_literal(token) + ':' + std::to_string(id++);
    types += std::to_string(type);
  };
  for (int byte = 33; byte <= 126; ++byte) add(std::string(1, static_cast<char>(byte)), 1);
  add("\xC4\xA0", 1);  // U+0120, the byte-level symbol for ' '
  add("\xC4\x8A", 1);  // U+010A, the byte-level symbol for '\n'
  add("<|im_start|>", 3);
  add("<|im_end|>", 3);
  write_file(root / "vocab.json", vocab + "}");
  write_file(root / "token_type.json", types + "]");
  write_file(root / "merges.txt", "");
}

// A complete local installation in a scratch directory: models/ with
// qwen-local.gguf, a tokenizer pack, an empty secret root, a private copy of
// the fake engine, and a free loopback port. ARGS: [0] strata-secure-server, [1] fake engine.
class ServerFixture final {
 public:
  std::filesystem::path dir = make_scratch_directory();
  std::filesystem::path models = dir / "models";
  std::filesystem::path tokenizer = dir / "tok";
  std::filesystem::path secret = dir / "secret";
  std::filesystem::path engine;
  std::filesystem::path config = dir / "config.json";
  std::filesystem::path server_executable;
  std::uint16_t port = pick_free_port();

  ServerFixture() {
    const auto& args = extra_arguments();
    if (args.size() < 2U) {
      std::cerr << "ServerFixture needs ARGS <strata-secure-server> <fake engine>\n";
      std::exit(EXIT_FAILURE);
    }
    server_executable = args[0];
    for (const auto& directory : {models, tokenizer, secret, dir / "bin"}) make_private_directory(directory);
    write_file(models / "qwen-local.gguf", "GGUF");
#if defined(_WIN32)
    engine = dir / "bin" / "strata.exe";
#else
    engine = dir / "bin" / "strata";
#endif
    std::filesystem::copy_file(args[1], engine, std::filesystem::copy_options::overwrite_existing);
#if !defined(_WIN32)
    chmod(engine.c_str(), 0700);
#endif
    write_test_tokenizer_pack(tokenizer);
  }

  [[nodiscard]] std::vector<std::string> base_flags() const {
    return {"--models-root", models.string(), "--tokenizer-pack", tokenizer.string(), "--engine", engine.string(),
            "--model-id", "qwen-local", "--token-env", kTestTokenVariable, "--port", std::to_string(port)};
  }
  [[nodiscard]] std::map<std::string, std::string> base_literals() const {
    return {{"schema_version", "1"},
            {"listener.port", std::to_string(port)},
            {"roots.models", json_literal(models.string())},
            {"roots.tokenizer_pack", json_literal(tokenizer.string())},
            {"engine.executable", json_literal(engine.string())},
            {"engine.model_id", json_literal("qwen-local")},
            {"token.source", json_literal(std::string{"env:"} + kTestTokenVariable)}};
  }
  [[nodiscard]] std::vector<std::pair<std::string, std::string>> environment() const {
    return {{kTestTokenVariable, std::string{kTestToken}}};
  }
  void write_config(const std::map<std::string, std::string>& literals) const {
    write_file(config, render_config_json(literals));
  }
  [[nodiscard]] ProcessResult run(const std::vector<std::string>& arguments) const {
    return run_to_completion(ProcessSpec{server_executable, arguments, environment()});
  }
  bool start(ChildProcess& child, const std::vector<std::string>& arguments) const {
    if (!child.start(ProcessSpec{server_executable, arguments, environment()})) return false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline) {
      if (child.wait_for(std::chrono::milliseconds(0)).has_value()) return false;
      if (can_connect(port)) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
  }
  [[nodiscard]] std::string request(const std::string& method, const std::string& target,
                                    const std::string& authorization, const std::string& body = {}) const {
    std::string text = method + " " + target + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\n";
    if (!authorization.empty()) text += "Authorization: " + authorization + "\r\n";
    if (!body.empty()) {
      text += "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
    }
    return http_exchange(port, text + "\r\n" + body);
  }
  [[nodiscard]] std::string bearer() const { return "Bearer " + std::string{kTestToken}; }
};

}  // namespace secure_frontend_test
```

`tests/fake_engine_main.cpp`:

```cpp
// A stand-in for the Strata --serve engine used by the end-to-end tests. It
// never touches a model. Options (passed by the server via engine.args):
//   --marker=PATH        create PATH as soon as the process starts
//   --dump=PATH          write "argv:<arg>" and "env:<k=v>" lines to PATH
//   --ready-delay-ms=N   wait N ms before printing READY
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

int main(int argc, char** argv) {
#if defined(_WIN32)
  _setmode(_fileno(stdout), _O_BINARY);
  char** environment = _environ;
#else
  char** environment = environ;
#endif
  std::string marker;
  std::string dump;
  long ready_delay_ms = 0;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument.rfind("--marker=", 0) == 0) marker = argument.substr(9);
    else if (argument.rfind("--dump=", 0) == 0) dump = argument.substr(7);
    else if (argument.rfind("--ready-delay-ms=", 0) == 0) ready_delay_ms = std::strtol(argument.c_str() + 17, nullptr, 10);
  }
  if (!marker.empty()) std::ofstream(marker) << "started\n";
  if (!dump.empty()) {
    std::ofstream out(dump, std::ios::binary);
    for (int index = 0; index < argc; ++index) out << "argv:" << argv[index] << '\n';
    for (char** entry = environment; entry != nullptr && *entry != nullptr; ++entry) out << "env:" << *entry << '\n';
  }
  if (ready_delay_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ready_delay_ms));
  std::cout << "READY 4096 stop\n" << std::flush;
  std::string line;
  while (std::getline(std::cin, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.rfind("GEN ", 0) == 0) {
      const unsigned long count = std::strtoul(line.c_str() + 4, nullptr, 10);
      for (unsigned long i = 0; i < count; ++i) std::cout << "T 0\n";
      std::cout << "DONE " << count << '\n' << std::flush;
    } else if (line == "STOP") {
      std::cout << "DONE 0 cancel\n" << std::flush;
    }
  }
  return 0;
}
```

CMake (test block):

```cmake
    add_executable(secure_frontend_fake_engine tests/fake_engine_main.cpp)
    target_compile_features(secure_frontend_fake_engine PRIVATE cxx_std_20)
    secure_frontend_set_warnings(secure_frontend_fake_engine)
    set(SECURE_FRONTEND_SERVER_ARGS ARGS $<TARGET_FILE:strata-secure-server> $<TARGET_FILE:secure_frontend_fake_engine>)

    secure_frontend_add_test_executable(secure_frontend_server_cli_tests tests/server_cli_test.cpp
        secure_frontend_typed_config)
    add_dependencies(secure_frontend_server_cli_tests strata-secure-server secure_frontend_fake_engine)
    secure_frontend_add_cases(secure_frontend_server_cli_tests
        cli_config_file_and_flags_share_validator cli_flag_overrides_file_and_is_reported cli_check_prints_no_token
        cli_unknown_option_does_not_echo_value
        ${SECURE_FRONTEND_SERVER_ARGS} TIMEOUT 120)
```

- [ ] **Step 2: Write the failing tests** (`tests/server_cli_test.cpp`)

```cpp
#include "support/server_fixture.hpp"
#include "support/test_support.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {
int failures = 0;
void expect(const bool condition, const std::string& message) {
  if (!condition) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
}
using secure_frontend_test::contains;
using secure_frontend_test::ServerFixture;

std::vector<std::string> with(std::vector<std::string> base, const std::vector<std::string>& extra) {
  base.insert(base.end(), extra.begin(), extra.end());
  return base;
}

std::vector<std::string> without_port(std::vector<std::string> flags) {
  for (std::size_t i = 0; i + 1 < flags.size(); ++i) {
    if (flags[i] == "--port") { flags.erase(flags.begin() + static_cast<long>(i), flags.begin() + static_cast<long>(i) + 2); break; }
  }
  return flags;
}

void cli_config_file_and_flags_share_validator() {
  ServerFixture f;
  auto literals = f.base_literals();
  literals["listener.port"] = "0";
  f.write_config(literals);
  const auto from_file = f.run({"--check", "--config", f.config.string()});
  const auto from_flag = f.run(with(without_port(f.base_flags()), {"--check", "--port", "0"}));
  const std::string expected = "listener.port: must be between 1 and 65535";
  expect(from_file.exit_code != 0 && contains(from_file.err, expected), "file value is validated: " + from_file.err);
  expect(from_flag.exit_code != 0 && contains(from_flag.err, expected), "flag value is validated: " + from_flag.err);
}

void cli_flag_overrides_file_and_is_reported() {
  ServerFixture f;
  f.write_config(f.base_literals());
  const std::string other_port = std::to_string(f.port == 65535 ? 65534 : f.port + 1);
  const auto result = f.run({"--check", "--config", f.config.string(), "--port", other_port});
  expect(result.exit_code == 0, "a valid file plus a flag passes --check: " + result.err);
  expect(contains(result.out, "listener.port = " + other_port), "the flag wins: " + result.out);
  expect(contains(result.out, "listener.port: the command line overrides the config file"),
         "the override is reported, never silent: " + result.out);
}

void cli_check_prints_no_token() {
  ServerFixture f;
  f.write_config(f.base_literals());
  const auto before = secure_frontend_test::snapshot_tree(f.dir);
  const auto result = f.run({"--check", "--config", f.config.string()});
  expect(result.exit_code == 0, "--check on a valid config exits 0: " + result.err);
  expect(secure_frontend_test::snapshot_tree(f.dir) == before, "--check has no side effects below the fixture");
  expect(contains(result.out, "token.source = \"env:SF_TEST_TOKEN\""), "the token source is shown as metadata");
  expect(contains(result.out, "token: available (value not shown)"), "--check confirms the token is readable");
  expect(!contains(result.out, secure_frontend_test::kTestToken) && !contains(result.err, secure_frontend_test::kTestToken),
         "--check never prints the token");
}

void cli_unknown_option_does_not_echo_value() {
  ServerFixture f;
  const std::string secret = "sk-PASTED-SECRET-0123456789";
  const auto equals_form = f.run({"--token=" + secret});
  expect(equals_form.exit_code != 0 && contains(equals_form.err, "unknown option: --token"), "flag name is echoed");
  expect(!contains(equals_form.err, secret), "the value after = is never echoed");
  const auto bare = f.run(with(f.base_flags(), {secret}));
  expect(bare.exit_code != 0 && !contains(bare.err, secret), "a stray argument is never echoed: " + bare.err);
  const auto long_flag = f.run({"--" + std::string(200, 'x')});
  expect(long_flag.exit_code != 0 && !contains(long_flag.err, std::string(65, 'x')), "at most 64 bytes are echoed");
  const auto missing = f.run({"--port"});
  expect(missing.exit_code != 0 && contains(missing.err, "missing value for --port"), "known flag without value");
}
}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"cli_config_file_and_flags_share_validator", cli_config_file_and_flags_share_validator},
      {"cli_flag_overrides_file_and_is_reported", cli_flag_overrides_file_and_is_reported},
      {"cli_check_prints_no_token", cli_check_prints_no_token},
      {"cli_unknown_option_does_not_echo_value", cli_unknown_option_does_not_echo_value},
  }, failures);
}
```

- [ ] **Step 3: Run to verify it fails**

Run: `cmake --preset linux-gcc && cmake --build --preset linux-gcc && ctest --preset linux-gcc -R server_cli --output-on-failure`
Expected: 4 tests FAIL — `--check` and `--config` are `unknown option`s today; `FAILED: the value after = is never echoed` (the current message is `unknown option: --token=sk-PASTED-…`).

- [ ] **Step 4: Implement**

`config_file.hpp` — add:

```cpp
// Reads a config file (bounded at kMaxConfigFileBytes) and parses it; the
// file's directory becomes the draft's config_directory. Task 12 replaces the
// read with a trusted, handle-based open.
[[nodiscard]] std::variant<FrontendConfigDraft, ConfigErrors> read_config_file(const std::filesystem::path& file);

// Overlays command-line entries on a file draft: flags win, and every key set
// in both is listed in overridden_by_command_line.
[[nodiscard]] FrontendConfigDraft merge_drafts(FrontendConfigDraft file, const FrontendConfigDraft& flags);
```

`config_file.cpp` — add `#include <fstream>` and:

```cpp
std::variant<FrontendConfigDraft, ConfigErrors> read_config_file(const std::filesystem::path& file) {
  std::error_code error;
  const auto absolute = std::filesystem::absolute(file, error);
  if (error) return ConfigErrors{ConfigIssue{"config file", ConfigIssueCode::file_unavailable, "cannot be resolved"}};
  std::ifstream input(absolute, std::ios::binary);
  if (!input) return ConfigErrors{ConfigIssue{"config file", ConfigIssueCode::file_unavailable, "cannot be opened"}};
  std::string text(kMaxConfigFileBytes + 1U, '\0');
  input.read(text.data(), static_cast<std::streamsize>(text.size()));
  text.resize(static_cast<std::size_t>(input.gcount()));
  if (text.size() > kMaxConfigFileBytes) {
    return ConfigErrors{ConfigIssue{"config file", ConfigIssueCode::malformed_document, "is larger than 64 KiB"}};
  }
  return parse_config_text(text, absolute.parent_path());
}

FrontendConfigDraft merge_drafts(FrontendConfigDraft file, const FrontendConfigDraft& flags) {
  for (const auto& [key, entry] : flags.entries) {
    if (file.entries.find(key) != file.entries.end()) file.overridden_by_command_line.push_back(key);
    file.entries[key] = entry;
  }
  return file;
}
```

`include/secure_frontend/server_cli.hpp`:

```cpp
#pragma once

#include "secure_frontend/frontend_config.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace secure_frontend {

enum class ServerCommand { serve, check, init, help };

struct ServerInvocation final {
  ServerCommand command{ServerCommand::serve};
  std::optional<std::filesystem::path> config_file;
  bool force{false};
  FrontendConfigDraft flags;  // every entry has ConfigSource::command_line
};

// Command-line problems. The message names a flag at most, never a value.
struct CliError final {
  std::string message;
};

// Each existing option sets exactly one configuration key (README table).
// Values are stored uncoerced except that an all-digit value for an integer
// key becomes an integer; validate() judges everything else.
[[nodiscard]] std::variant<ServerInvocation, CliError> parse_server_command_line(int argc, const char* const* argv);

// The option name up to '=', at most 64 printable ASCII bytes ('?' for others).
[[nodiscard]] std::string safe_option_name(std::string_view argument);

[[nodiscard]] std::string server_usage();

// defaults < config file < flags, then validate(). `overridden` receives the
// keys set in both the file and on the command line.
[[nodiscard]] ConfigValidationResult load_server_config(const ServerInvocation& invocation,
                                                        std::vector<std::string>* overridden);

}  // namespace secure_frontend
```

`src/server_cli.cpp`:

```cpp
#include "secure_frontend/server_cli.hpp"

#include "secure_frontend/config_file.hpp"

#include <array>

namespace secure_frontend {
namespace {

enum class FlagKind { boolean, integer, string, append, token_env, token_file };
struct FlagSpec final {
  std::string_view flag;
  std::string_view key;
  FlagKind kind;
};

constexpr std::array<FlagSpec, 13> kFlags{{
    {"--host", "listener.host", FlagKind::string},
    {"--port", "listener.port", FlagKind::integer},
    {"--allow-remote", "listener.allow_remote", FlagKind::boolean},
    {"--allow-wildcard-bind", "listener.allow_wildcard_bind", FlagKind::boolean},
    {"--profile", "engine.profile", FlagKind::string},
    {"--models-root", "roots.models", FlagKind::string},
    {"--tokenizer-pack", "roots.tokenizer_pack", FlagKind::string},
    {"--engine", "engine.executable", FlagKind::string},
    {"--model-id", "engine.model_id", FlagKind::string},
    {"--engine-arg", "engine.args", FlagKind::append},
    {"--token-env", "token.source", FlagKind::token_env},
    {"--token-file", "token.source", FlagKind::token_file},
    {"--secret-root", "roots.secret", FlagKind::string},
}};

const FlagSpec* find_flag(const std::string_view argument) {
  for (const auto& spec : kFlags) if (spec.flag == argument) return &spec;
  return nullptr;
}

bool all_digits(const std::string& value) {
  return !value.empty() && value.size() <= 18U &&
         value.find_first_not_of("0123456789") == std::string::npos;
}

}  // namespace

std::string safe_option_name(const std::string_view argument) {
  const std::string_view name = argument.substr(0, argument.find('='));
  std::string out;
  for (const char c : name) {
    if (out.size() == 64U) { out += "..."; break; }
    const auto byte = static_cast<unsigned char>(c);
    out.push_back(byte >= 0x21U && byte <= 0x7eU ? c : '?');
  }
  return out;
}

std::variant<ServerInvocation, CliError> parse_server_command_line(const int argc, const char* const* argv) {
  ServerInvocation invocation;
  auto& entries = invocation.flags.entries;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument{argv[index]};
    if (argument == "--help") { invocation.command = ServerCommand::help; return invocation; }
    if (argument == "--check") {
      if (invocation.command == ServerCommand::check) return CliError{"duplicate --check"};
      invocation.command = ServerCommand::check;
      continue;
    }
    if (argument == "--config") {
      if (index + 1 >= argc) return CliError{"missing value for --config"};
      if (invocation.config_file.has_value()) return CliError{"duplicate --config"};
      const std::string value{argv[++index]};
      if (value.empty()) return CliError{"empty value for --config"};
      invocation.config_file = std::filesystem::path{value};
      continue;
    }
    const FlagSpec* spec = find_flag(argument);
    if (spec == nullptr) {
      const auto equals = argument.find('=');
      if (equals != std::string_view::npos && find_flag(argument.substr(0, equals)) != nullptr) {
        return CliError{"write " + safe_option_name(argument) + " VALUE as two arguments"};
      }
      if (argument.rfind("--", 0) == 0) return CliError{"unknown option: " + safe_option_name(argument)};
      return CliError{"unexpected argument (not shown); every value must follow its option"};
    }
    const std::string key{spec->key};
    if (spec->kind == FlagKind::boolean) {
      if (entries.find(key) != entries.end()) return CliError{"duplicate " + std::string{spec->flag}};
      entries[key] = ConfigDraftEntry{true, ConfigSource::command_line};
      continue;
    }
    if (index + 1 >= argc) return CliError{"missing value for " + std::string{spec->flag}};
    const std::string value{argv[++index]};
    if (value.empty()) return CliError{"empty value for " + std::string{spec->flag}};
    switch (spec->kind) {
      case FlagKind::append: {
        auto [it, inserted] = entries.try_emplace(key, ConfigDraftEntry{std::vector<std::string>{}, ConfigSource::command_line});
        (void)inserted;
        std::get<std::vector<std::string>>(it->second.value).push_back(value);
        break;
      }
      case FlagKind::token_env:
      case FlagKind::token_file:
        if (entries.find(key) != entries.end()) return CliError{"supply at most one of --token-env or --token-file"};
        entries[key] = ConfigDraftEntry{(spec->kind == FlagKind::token_env ? "env:" : "file:") + value,
                                        ConfigSource::command_line};
        break;
      case FlagKind::integer:
        if (entries.find(key) != entries.end()) return CliError{"duplicate " + std::string{spec->flag}};
        if (all_digits(value)) entries[key] = ConfigDraftEntry{static_cast<std::int64_t>(std::stoll(value)), ConfigSource::command_line};
        else entries[key] = ConfigDraftEntry{value, ConfigSource::command_line};
        break;
      case FlagKind::string:
      case FlagKind::boolean:
        if (entries.find(key) != entries.end()) return CliError{"duplicate " + std::string{spec->flag}};
        entries[key] = ConfigDraftEntry{value, ConfigSource::command_line};
        break;
    }
  }
  return invocation;
}

std::string server_usage() {
  std::string text =
      "Usage: strata-secure-server [--config FILE] [options]\n"
      "       strata-secure-server --check [--config FILE] [options]\n"
      "\n"
      "Configuration comes from built-in defaults, then FILE (strict JSON; see the\n"
      "README's key table), then the options below. An option wins over the file and\n"
      "--check reports every key where that happened.\n"
      "\n"
      "  --config FILE            Read configuration keys from FILE\n"
      "  --check                  Validate everything, print the effective configuration\n"
      "                           (never the token) and exit\n"
      "  --help                   Print this help\n"
      "\n"
      "Options (each sets one configuration key):\n";
  for (const auto& spec : kFlags) {
    std::string row = "  " + std::string{spec.flag};
    if (spec.kind != FlagKind::boolean) row += " VALUE";
    row.resize(std::max<std::size_t>(row.size() + 1U, 27U), ' ');
    row += std::string{spec.key};
    if (spec.kind == FlagKind::token_env) row += " = env:VALUE";
    if (spec.kind == FlagKind::token_file) row += " = file:VALUE";
    if (spec.kind == FlagKind::append) row += " (repeatable)";
    text += row + "\n";
  }
  text +=
      "\n"
      "The server starts exactly one local --serve child with no shell and an empty\n"
      "environment. It serves authenticated GET /health, GET /v1/models and\n"
      "non-streaming text-only POST /v1/chat/completions. Streaming, tools, downloads,\n"
      "remote URLs, telemetry and MCP are disabled. With the qwen profile the frontend\n"
      "supplies --native from engine.model_id; the operator passes --pack and other\n"
      "weight flags with --engine-arg.\n";
  return text;
}

ConfigValidationResult load_server_config(const ServerInvocation& invocation, std::vector<std::string>* overridden) {
  FrontendConfigDraft draft = invocation.flags;
  if (invocation.config_file.has_value()) {
    auto file = read_config_file(*invocation.config_file);
    if (auto* errors = std::get_if<ConfigErrors>(&file)) return *errors;
    draft = merge_drafts(std::move(std::get<FrontendConfigDraft>(file)), invocation.flags);
  }
  if (overridden != nullptr) *overridden = draft.overridden_by_command_line;
  return validate(draft);
}

}  // namespace secure_frontend
```

(`#include <algorithm>` for `std::max`.)

Replace `src/secure_server_main.cpp` with:

```cpp
#include "secure_frontend/api_router.hpp"
#include "secure_frontend/chat_execution.hpp"
#include "secure_frontend/chat_schema.hpp"
#include "secure_frontend/config.hpp"
#include "secure_frontend/engine_arguments.hpp"
#include "secure_frontend/engine_stdio_process.hpp"
#include "secure_frontend/frontend_config.hpp"
#include "secure_frontend/http_request_handler.hpp"
#include "secure_frontend/http_socket_server.hpp"
#include "secure_frontend/model_inventory.hpp"
#include "secure_frontend/process_defaults.hpp"
#include "secure_frontend/server_cli.hpp"
#include "secure_frontend/token_reader.hpp"
#include "secure_frontend/tokenizer_pack.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

using secure_frontend::EngineProfile;
using secure_frontend::FrontendConfig;

std::string host_header(const secure_frontend::Config& config) {
  const bool ipv6 = config.host.find(':') != std::string::npos;
  return (ipv6 ? "[" + config.host + "]" : config.host) + ":" + std::to_string(config.port);
}

class TokenVerifier final {
 public:
  explicit TokenVerifier(std::string token) : token_(std::move(token)) {}
  ~TokenVerifier() {
    volatile char* bytes = token_.empty() ? nullptr : token_.data();
    for (std::size_t index = 0; bytes != nullptr && index < token_.size(); ++index) bytes[index] = '\0';
  }
  [[nodiscard]] bool matches(const std::string_view candidate) const {
    return secure_frontend::constant_time_token_equals(candidate, token_);
  }

 private:
  std::string token_;
};

secure_frontend::HttpResponse json_error(const int status, const char* message) {
  return {.status = status,
          .headers = {{"Content-Type", "application/json; charset=utf-8"}, {"Cache-Control", "no-store"}},
          .body = std::string{"{\"error\":\""} + message + "\"}\n"};
}

secure_frontend::TokenReadResult read_configured_token(const FrontendConfig& config) {
  return secure_frontend::read_token(config.token, config.roots.secret.value_or(std::filesystem::path{"."}));
}

int check(const FrontendConfig& config, const std::vector<std::string>& overridden) {
  std::cout << "Configuration is valid.\n" << secure_frontend::describe(config);
  for (const auto& key : overridden) std::cout << key << ": the command line overrides the config file\n";
  const auto token = read_configured_token(config);
  if (const auto* error = std::get_if<secure_frontend::TokenReadError>(&token)) {
    std::cout << "token: unavailable: " << error->message << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "token: available (value not shown)\n";
  return EXIT_SUCCESS;
}

int run(const FrontendConfig& config) {
  auto token_result = read_configured_token(config);
  auto* token = std::get_if<std::string>(&token_result);
  if (token == nullptr) { std::cerr << "Unable to read API token.\n"; return EXIT_FAILURE; }
  auto verifier = std::make_shared<TokenVerifier>(std::move(*token));

  const secure_frontend::Config listener = secure_frontend::listener_config(config);
  const auto tokenizer_result = secure_frontend::load_local_tokenizer_pack(config.roots.tokenizer_pack);
  const auto* tokenizer = std::get_if<secure_frontend::TokenizerPack>(&tokenizer_result);
  if (tokenizer == nullptr) { std::cerr << "Unable to load local tokenizer pack.\n"; return EXIT_FAILURE; }
  const auto models_result = secure_frontend::enumerate_local_models(config.roots.models);
  const auto* manifests = std::get_if<std::vector<secure_frontend::ModelManifest>>(&models_result);
  if (manifests == nullptr) { std::cerr << "Unable to inventory local models.\n"; return EXIT_FAILURE; }
  const std::string& model_id = config.engine.model_id.value();
  const auto selected = std::find_if(manifests->begin(), manifests->end(),
                                     [&model_id](const auto& model) { return model.id == model_id; });
  if (selected == manifests->end()) { std::cerr << "Selected model is not present in roots.models.\n"; return EXIT_FAILURE; }
  const bool glm = config.engine.profile == EngineProfile::glm53;
  const auto arguments_profile = glm ? secure_frontend::EngineArgumentsProfile::lighttransport_glm53
                                     : secure_frontend::EngineArgumentsProfile::strata_0143;
  auto engine_arguments_result =
      secure_frontend::build_engine_arguments(arguments_profile, *selected, config.roots.models, config.engine.args);
  auto* engine_arguments = std::get_if<std::vector<std::string>>(&engine_arguments_result);
  if (engine_arguments == nullptr) { std::cerr << "Engine arguments rejected.\n"; return EXIT_FAILURE; }
  std::vector<secure_frontend::ApiModel> models;
  models.reserve(manifests->size());
  for (const auto& manifest : *manifests) models.push_back({.id = manifest.id, .owned_by = manifest.owned_by});

  secure_frontend::EngineStdioProcess engine;
  const secure_frontend::EngineCommand command{.executable = config.engine.executable,
                                               .arguments = std::move(*engine_arguments), .environment = {}};
  if (engine.start(command).has_value()) { std::cerr << "Unable to start local engine.\n"; return EXIT_FAILURE; }
  const auto protocol_flavor = glm ? secure_frontend::EngineProtocolFlavor::lighttransport_glm53
                                   : secure_frontend::EngineProtocolFlavor::strata_0143;
  secure_frontend::EngineProtocolClient protocol(engine, {}, protocol_flavor);
  const auto chat_profile = glm ? secure_frontend::NativeChatProfile::glm53 : secure_frontend::NativeChatProfile::qwen;
  secure_frontend::ChatExecutionService chat(protocol, {}, chat_profile);

  const std::string authority = host_header(listener);
  secure_frontend::HttpPolicy policy{.allowed_hosts = {authority}, .same_origin_origins = {"http://" + authority},
                                     .cors_allowed_origins = {}, .cors_enabled = false};
  const auto engine_state = [&engine, &chat]() -> std::string_view {
    if (!engine.running()) return "failed";
    switch (chat.health()) {
      case secure_frontend::EngineHealth::starting: return "starting";
      case secure_frontend::EngineHealth::ready: return "ready";
      case secure_frontend::EngineHealth::generating: return "generating";
      case secure_frontend::EngineHealth::failed: return "failed";
    }
    return "failed";
  };
  std::string_view health_snapshot{"failed"};
  const secure_frontend::ApiRouter router(
      [&engine_state, &health_snapshot] { health_snapshot = engine_state(); return health_snapshot != "failed"; },
      [models] { return models; }, [&health_snapshot] { return health_snapshot; });
  auto server = secure_frontend::HttpSocketServer::bind(
      listener, std::move(policy), [verifier](const std::string_view supplied) { return verifier->matches(supplied); },
      [&router, &chat, &engine, &tokenizer, &model_id](const std::string_view method, const std::string_view target,
                                                       const std::string_view body) {
        if (method == "GET") return router.dispatch(method, target);
        if (method != "POST" || target != "/v1/chat/completions") return json_error(404, "Not Found");
        if (!engine.running()) return json_error(503, "Service Unavailable");
        const auto request = secure_frontend::parse_chat_completion_request(body);
        if (!request || request->model != model_id) return json_error(400, "Bad Request");
        const auto result = chat.execute(*request, *tokenizer);
        const auto* completion = std::get_if<secure_frontend::ChatExecutionResult>(&result);
        if (completion == nullptr) return json_error(503, "Service Unavailable");
        const auto response = secure_frontend::render_chat_completion_response(*request, completion->text);
        if (!response) return json_error(503, "Service Unavailable");
        return secure_frontend::HttpResponse{
            .status = 200,
            .headers = {{"Content-Type", "application/json; charset=utf-8"}, {"Cache-Control", "no-store"}},
            .body = *response};
      },
      nullptr);
  if (!server) { std::cerr << "Unable to bind secure listener.\n"; return EXIT_FAILURE; }
  std::cout << "Secure server listening on http://" << authority << " (authenticated; Ctrl-C to stop)\n";
  std::cout.flush();
  for (;;) {
    std::string error;
    if (!server->serve_one(&error) && server->listener_failed()) {
      std::cerr << "listener stopped: " << error << '\n';
      return EXIT_FAILURE;
    }
  }
}

}  // namespace

int main(int argc, char* argv[]) {
  secure_frontend::install_process_defaults();
  const auto parsed = secure_frontend::parse_server_command_line(argc, argv);
  if (const auto* error = std::get_if<secure_frontend::CliError>(&parsed)) {
    std::cerr << "Error: " << error->message << "\nRun strata-secure-server --help for usage.\n";
    return EXIT_FAILURE;
  }
  const auto& invocation = std::get<secure_frontend::ServerInvocation>(parsed);
  if (invocation.command == secure_frontend::ServerCommand::help) {
    std::cout << secure_frontend::server_usage();
    return EXIT_SUCCESS;
  }
  std::vector<std::string> overridden;
  const auto loaded = secure_frontend::load_server_config(invocation, &overridden);
  if (const auto* issues = std::get_if<secure_frontend::ConfigErrors>(&loaded)) {
    for (const auto& issue : *issues) std::cerr << "Configuration error: " << secure_frontend::format_issue(issue) << '\n';
    return EXIT_FAILURE;
  }
  const auto config = std::get<secure_frontend::ValidatedConfig>(loaded).shared();
  if (invocation.command == secure_frontend::ServerCommand::check) return check(*config, overridden);
  return run(*config);
}
```

CMake: add the library and link it into the server:

```cmake
add_library(secure_frontend_server_cli src/server_cli.cpp)
target_compile_features(secure_frontend_server_cli PUBLIC cxx_std_20)
target_include_directories(secure_frontend_server_cli PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)
target_link_libraries(secure_frontend_server_cli PUBLIC secure_frontend_typed_config)
target_link_libraries(strata-secure-server PRIVATE secure_frontend_server_cli secure_frontend_typed_config)
```

and `secure_frontend_server_cli` in the warnings `foreach`.

- [ ] **Step 5: Run to verify it passes**

Run: `cmake --build --preset linux-gcc && ctest --preset linux-gcc | tail -3`
Expected: the four `secure_frontend_server_cli_tests.*` cases pass; `strata_secure_server_glm_requires_engine_arg` still passes (its output now reads `Configuration error: engine.args: --profile glm53 requires at least one --engine-arg` next to the relative-path errors); `0 tests failed`.
Run: `cmake --build --preset windows-llvm-mingw-cross | tail -1 && ctest --preset windows-llvm-mingw-cross | tail -3`
Expected: build ok; the four CLI cases run unlabelled under wine in this task (wine's `CreateProcessW` starts the PE child, and before Task 9 nothing they do depends on DACLs); record their result in the commit body. Task 9 labels them `windows-runtime` when `validate()` starts opening roots.

- [ ] **Step 6: Commit**

```bash
git -C "$(git rev-parse --show-toplevel)" add secure_frontend
git -C "$(git rev-parse --show-toplevel)" -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "server: --config and --check through one validator; never echo option values (SP1 C6, C8)" \
  -m "Regression tests (failed before): cli_config_file_and_flags_share_validator,
cli_flag_overrides_file_and_is_reported, cli_check_prints_no_token, cli_unknown_option_does_not_echo_value.
Adds the end-to-end harness (process runner, HTTP client, server fixture, fake engine). wine: <record>." \
  -m "$SP1_TRAILERS"
```

---

### Task 6: Nothing starts on an invalid or unbindable listener; process-level NET tests (C3, N3, N4)

**Files:**
- Modify: `secure_frontend/src/secure_server_main.cpp` (`run()`: engine object created before `bind`, `engine.start` moved after a successful bind)
- Create: `secure_frontend/tests/server_e2e_test.cpp`
- Modify: `secure_frontend/CMakeLists.txt` (test binary; `iphlpapi` on WIN32)

**Interfaces:**
- Consumes: the Task 5 harness (`ServerFixture`, `ChildProcess`, `run_to_completion`, `contains`).
- Produces: `secure_frontend_server_e2e_tests` — later tasks (7, 9, 20, 22, 23) add cases to this file and to its `secure_frontend_add_cases` call.

- [ ] **Step 1: Write the failing tests** (`tests/server_e2e_test.cpp`)

```cpp
#include "support/server_fixture.hpp"
#include "support/test_support.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <iphlpapi.h>
#endif

namespace {
int failures = 0;
void expect(const bool condition, const std::string& message) {
  if (!condition) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
}
using secure_frontend_test::contains;
using secure_frontend_test::ServerFixture;

std::vector<std::string> with(std::vector<std::string> base, const std::vector<std::string>& extra) {
  base.insert(base.end(), extra.begin(), extra.end());
  return base;
}

// Every listening TCP socket on `port`: {"v4", "<hex addr>"} / {"v6", ...}.
std::vector<std::pair<std::string, std::string>> listeners_on_port(const std::uint16_t port, const long pid) {
  std::vector<std::pair<std::string, std::string>> found;
#if defined(_WIN32)
  for (const ULONG family : {static_cast<ULONG>(AF_INET), static_cast<ULONG>(AF_INET6)}) {
    DWORD size = 0;
    GetExtendedTcpTable(nullptr, &size, FALSE, family, TCP_TABLE_OWNER_PID_LISTENER, 0);
    std::vector<unsigned char> buffer(size);
    if (GetExtendedTcpTable(buffer.data(), &size, FALSE, family, TCP_TABLE_OWNER_PID_LISTENER, 0) != NO_ERROR) continue;
    if (family == AF_INET) {
      const auto* table = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(buffer.data());
      for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const auto& row = table->table[i];
        if (static_cast<long>(row.dwOwningPid) == pid || ntohs(static_cast<u_short>(row.dwLocalPort)) == port) {
          found.emplace_back("v4", row.dwLocalAddr == htonl(INADDR_LOOPBACK) ? "loopback" : "other");
        }
      }
    } else {
      const auto* table = reinterpret_cast<const MIB_TCP6TABLE_OWNER_PID*>(buffer.data());
      for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const auto& row = table->table[i];
        if (static_cast<long>(row.dwOwningPid) == pid || ntohs(static_cast<u_short>(row.dwLocalPort)) == port) {
          found.emplace_back("v6", "other");
        }
      }
    }
  }
#else
  (void)pid;
  char port_hex[8];
  std::snprintf(port_hex, sizeof(port_hex), "%04X", port);
  for (const auto& [file, family] : {std::pair{"/proc/net/tcp", "v4"}, std::pair{"/proc/net/tcp6", "v6"}}) {
    std::ifstream table(file);
    std::string line;
    std::getline(table, line);  // header
    while (std::getline(table, line)) {
      std::istringstream fields(line);
      std::string slot, local, remote, state;
      fields >> slot >> local >> remote >> state;
      const auto colon = local.rfind(':');
      if (state != "0A" || colon == std::string::npos || local.substr(colon + 1) != port_hex) continue;
      const std::string address = local.substr(0, colon);
      found.emplace_back(family, address == "0100007F" ? "loopback" : "other");
    }
  }
#endif
  return found;
}

void cfg01_invalid_config_fails_before_listener_and_child() {
  ServerFixture f;
  const auto marker = f.dir / "engine-started";
  // (a) an invalid key value
  auto literals = f.base_literals();
  literals["limits.http.max_headers"] = "1";
  literals["engine.args"] = "[" + secure_frontend_test::json_literal("--marker=" + marker.string()) + "]";
  f.write_config(literals);
  const auto invalid = f.run({"--config", f.config.string()});
  expect(invalid.exit_code != 0 && contains(invalid.err, "limits.http.max_headers"), "invalid config exits: " + invalid.err);
  expect(!std::filesystem::exists(marker), "the engine must not start on an invalid config");
  expect(!secure_frontend_test::can_connect(f.port), "no listener remains");
  // (b) a valid config whose address cannot be bound (TEST-NET-1 is never local)
  const auto unbindable = f.run(with(f.base_flags(), {"--host", "192.0.2.10", "--allow-remote", "--engine-arg",
                                                      "--marker=" + marker.string()}));
  expect(unbindable.exit_code != 0 && contains(unbindable.err, "Unable to bind"), "unbindable address exits");
  expect(!std::filesystem::exists(marker), "the engine must not start when the listener cannot bind");
}

void net01_default_listener_is_loopback_only() {
  ServerFixture f;
  secure_frontend_test::ChildProcess server;
  expect(f.start(server, f.base_flags()), "server starts with defaults: " + server.stderr_text());
  const auto listeners = listeners_on_port(f.port, server.pid());
  expect(!listeners.empty(), "the server listens");
  for (const auto& [family, scope] : listeners) {
    expect(family == "v4" && scope == "loopback", "only 127.0.0.1 listens by default, found " + family + "/" + scope);
  }
}

void net02_remote_addresses_need_opt_in() {
  ServerFixture f;
  const auto marker = f.dir / "engine-started";
  for (const char* host : {"0.0.0.0", "::", "172.16.1.10", "10.0.0.1", "203.0.113.5", "2001:db8::1", "fd00::1"}) {
    const auto result = f.run(with(f.base_flags(), {"--host", host, "--engine-arg", "--marker=" + marker.string()}));
    expect(result.exit_code != 0 && contains(result.err, "listener.host: a non-loopback address needs listener.allow_remote"),
           std::string{"remote address without opt-in must fail: "} + host);
  }
  expect(!std::filesystem::exists(marker), "no engine was started for any of them");
}

void net02_error_is_non_secret_and_exits_before_bind() {
  ServerFixture f;
  const auto marker = f.dir / "engine-started";
  const auto result = f.run(with(f.base_flags(), {"--host", "203.0.113.5", "--engine-arg", "--marker=" + marker.string()}));
  expect(result.exit_code != 0, "startup fails");
  expect(contains(result.err, "listener.host"), "the error names the key");
  expect(!contains(result.err, secure_frontend_test::kTestToken) && !contains(result.out, secure_frontend_test::kTestToken),
         "the error never contains the token");
  expect(!contains(result.err, "203.0.113.5"), "the error does not echo the value");
  expect(!std::filesystem::exists(marker), "the engine was not launched");
}
}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"cfg01_invalid_config_fails_before_listener_and_child", cfg01_invalid_config_fails_before_listener_and_child},
      {"net01_default_listener_is_loopback_only", net01_default_listener_is_loopback_only},
      {"net02_remote_addresses_need_opt_in", net02_remote_addresses_need_opt_in},
      {"net02_error_is_non_secret_and_exits_before_bind", net02_error_is_non_secret_and_exits_before_bind},
  }, failures);
}
```

Add `#include <cstdio>` for `std::snprintf`. CMake:

```cmake
    secure_frontend_add_test_executable(secure_frontend_server_e2e_tests tests/server_e2e_test.cpp
        secure_frontend_typed_config)
    add_dependencies(secure_frontend_server_e2e_tests strata-secure-server secure_frontend_fake_engine)
    if(WIN32)
        target_link_libraries(secure_frontend_server_e2e_tests PRIVATE iphlpapi ws2_32)
    endif()
    secure_frontend_add_cases(secure_frontend_server_e2e_tests
        cfg01_invalid_config_fails_before_listener_and_child net02_remote_addresses_need_opt_in
        net02_error_is_non_secret_and_exits_before_bind
        ${SECURE_FRONTEND_SERVER_ARGS} TIMEOUT 120)
    # GetExtendedTcpTable is not faithful under wine.
    secure_frontend_add_cases(secure_frontend_server_e2e_tests net01_default_listener_is_loopback_only
        ${SECURE_FRONTEND_SERVER_ARGS} LABELS ${SECURE_FRONTEND_WINDOWS_RUNTIME} TIMEOUT 120)
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build --preset linux-gcc && ctest --preset linux-gcc -R server_e2e --output-on-failure`
Expected: `cfg01_invalid_config_fails_before_listener_and_child` FAILS with `FAILED: the engine must not start when the listener cannot bind` (today the engine starts before `bind()`). The three NET cases pass on first run (acceptance tests of Tasks 1 and 4; record that in the commit).

- [ ] **Step 3: Implement**

In `run()` of `secure_server_main.cpp`, delete the line
`if (engine.start(command).has_value()) { std::cerr << "Unable to start local engine.\n"; return EXIT_FAILURE; }`
that follows the `EngineCommand` declaration, and insert after
`if (!server) { std::cerr << "Unable to bind secure listener.\n"; return EXIT_FAILURE; }`:

```cpp
  // Bound first, spawned second: a listener that cannot bind never leaves an
  // engine child behind (CFG-01).
  if (engine.start(command).has_value()) { std::cerr << "Unable to start local engine.\n"; return EXIT_FAILURE; }
```

`EngineProtocolClient` and `ChatExecutionService` hold only a reference to `engine` and do no I/O in their constructors, so they stay where they are.

- [ ] **Step 4: Run to verify it passes**

Run: `cmake --build --preset linux-gcc && ctest --preset linux-gcc | tail -3` → `0 tests failed`.
Run: `cmake --build --preset windows-llvm-mingw-cross | tail -1 && ctest --preset windows-llvm-mingw-cross | tail -1` → build ok; record the wine result (`net01_default_listener_is_loopback_only` is excluded there).

- [ ] **Step 5: Commit**

```bash
git -C "$(git rev-parse --show-toplevel)" add secure_frontend
git -C "$(git rev-parse --show-toplevel)" -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "server: bind before spawning the engine; process-level NET-01/NET-02 tests (SP1 C3, N3, N4)" \
  -m "Regression test (failed before): cfg01_invalid_config_fails_before_listener_and_child (unbindable
192.0.2.10 started the engine). Acceptance tests that passed on first run: net01_default_listener_is_loopback_only,
net02_remote_addresses_need_opt_in, net02_error_is_non_secret_and_exits_before_bind." \
  -m "$SP1_TRAILERS"
```

---

### Task 7: Subsystems take their limits from the configuration; parser honors them; ready timeout 900 s (C4, C5)

**Files:**
- Create: `secure_frontend/include/secure_frontend/subsystem_limits.hpp`, `secure_frontend/src/subsystem_limits.cpp`, `secure_frontend/tests/subsystem_limits_test.cpp`
- Modify: `secure_frontend/include/secure_frontend/http_request_handler.hpp:44-47` (`HttpParserLimits` parameter), `secure_frontend/src/http_request_handler.cpp:10-12, 80-87, 114, 179-183`
- Modify: `secure_frontend/include/secure_frontend/http_socket_server.hpp:15-21`, `secure_frontend/src/http_socket_server.cpp:327, 411` (verified at HEAD 7ddcd755; Task 1 does not move these lines)
- Modify: `secure_frontend/include/secure_frontend/chat_schema.hpp:3-35`, `secure_frontend/src/chat_schema.cpp:12-17`, `valid_content` and the two public functions (as Task 3 left them)
- Modify: `secure_frontend/include/secure_frontend/chat_execution.hpp:24-31`, `secure_frontend/src/chat_execution.cpp:33-34`
- Modify: `secure_frontend/include/secure_frontend/chat_generation_plan.hpp:40-48`, `secure_frontend/src/chat_generation_plan.cpp:11, 49, 72`
- Modify: `secure_frontend/src/secure_server_main.cpp` (`run()` uses the derived limits)
- Modify: `secure_frontend/tests/http_socket_server_test.cpp` (fixture dispatcher gains `POST /sink`; new case), `secure_frontend/tests/server_e2e_test.cpp` (new case)
- Modify: `secure_frontend/CMakeLists.txt`

**Interfaces:**
- Consumes: `FrontendConfig`, `config_defaults` (Task 4).
- Produces:
```cpp
namespace secure_frontend {
struct HttpParserLimits final { std::size_t max_request_bytes; std::size_t max_header_bytes; std::size_t max_headers; std::size_t max_request_line_bytes; };   // defaults from config_defaults
[[nodiscard]] HttpResponse handle_http_request(std::string_view request, const HttpPolicy&, const BearerTokenValidator&,
                                               const RequestDispatcher&, const HttpParserLimits& limits = {});
struct HttpSocketServerLimits final { std::chrono::milliseconds request_deadline; std::size_t max_request_bytes;
  std::size_t max_header_bytes; std::size_t max_headers; std::size_t max_request_line_bytes; int backlog; };
struct ChatSchemaLimits final { std::size_t max_body_bytes; std::size_t max_messages; std::size_t max_message_bytes;
  std::size_t max_completion_tokens; std::size_t max_generated_bytes; };
[[nodiscard]] std::optional<ChatCompletionRequest> parse_chat_completion_request(std::string_view json, const ChatSchemaLimits& limits = {});
[[nodiscard]] std::optional<std::string> render_chat_completion_response(const ChatCompletionRequest&, std::string_view generated_text, const ChatSchemaLimits& limits = {});
struct ChatExecutionLimits final { std::size_t maximum_prompt_tokens; std::size_t maximum_output_tokens; std::size_t default_max_tokens;
  std::chrono::milliseconds ready_timeout /* 900 s */; std::chrono::milliseconds generation_timeout; std::chrono::milliseconds stop_drain_timeout; };
[[nodiscard]] ChatGenerationPlanResult make_qwen_chat_generation_plan(const ChatCompletionRequest&, const TokenizerPack&, std::size_t maximum_prompt_tokens = 32768U, std::size_t default_max_tokens = 256U);
[[nodiscard]] ChatGenerationPlanResult make_glm53_chat_generation_plan(const ChatCompletionRequest&, const TokenizerPack&, std::size_t maximum_prompt_tokens = 32768U, std::size_t default_max_tokens = 256U);
// subsystem_limits.hpp
[[nodiscard]] HttpSocketServerLimits http_socket_server_limits(const FrontendConfig& config);
[[nodiscard]] ChatSchemaLimits chat_schema_limits(const FrontendConfig& config);
[[nodiscard]] ChatExecutionLimits chat_execution_limits(const FrontendConfig& config);
}
```
  All struct defaults come from `config_defaults` (`#include "secure_frontend/config_defaults.hpp"`); the literal `256U` / `32768U` parameter defaults are written as `static_cast<std::size_t>(config_defaults::kChatDefaultMaxTokens)` / `kChatMaxPromptTokens`.

- [ ] **Step 1: Write the failing tests**

`tests/http_socket_server_test.cpp`: in the fixture dispatcher (the lambda at lines 68-76) name the body parameter and add a branch before the 404:

```cpp
          [](std::string_view method, std::string_view target, std::string_view body) {
            if (method == "POST" && target == "/sink") {
              return secure_frontend::HttpResponse{200, {{"Content-Type", "text/plain"}}, std::to_string(body.size())};
            }
```

and add the case (before `}  // namespace`):

```cpp
// C4 / defect (d): the parser must honor a configured max_request_bytes.
std::string post_sink(Fixture& f, const std::size_t body_bytes) {
  std::string response;
  bool served = false;
  std::thread worker([&] { served = f.server->serve_one(&f.error); });
  const NativeSocket client = f.connect_client();
  if (client == kBad) { f.unblock_accept(); worker.join(); return {}; }
  std::string request = "POST /sink HTTP/1.1\r\nHost: " + f.authority() +
                        "\r\nAuthorization: Bearer test-token\r\nContent-Length: " + std::to_string(body_bytes) + "\r\n\r\n";
  request.append(body_bytes, 'b');
  (void)send_bytes(client, request);
  char chunk[1024];
  for (;;) { const int n = recv_bytes(client, chunk, sizeof(chunk)); if (n <= 0) break; response.append(chunk, static_cast<std::size_t>(n)); }
  close_socket(client);
  worker.join();
  (void)served;
  return response;
}

void parser_honors_configured_max_request_bytes() {
  secure_frontend::HttpSocketServerLimits raised;
  raised.max_request_bytes = 256U * 1024U;
  Fixture big(raised);
  if (!big.server) { expect(false, "bind failed: " + big.error); return; }
  const auto accepted = post_sink(big, 128U * 1024U);
  expect(accepted.rfind("HTTP/1.1 200 OK", 0) == 0 && accepted.find("\r\n\r\n131072") != std::string::npos,
         "a 128 KiB body must pass a 256 KiB limit, got: " + accepted.substr(0, 24));
  Fixture standard;
  if (!standard.server) { expect(false, "bind failed: " + standard.error); return; }
  const auto rejected = post_sink(standard, 128U * 1024U);
  expect(rejected.rfind("HTTP/1.1 413", 0) == 0, "the default limit still rejects it: " + rejected.substr(0, 24));
}
```

Add `{"parser_honors_configured_max_request_bytes", parser_honors_configured_max_request_bytes},` to its `run_cases` list and the name to its `secure_frontend_add_cases` call.

`tests/subsystem_limits_test.cpp`:

```cpp
#include "secure_frontend/chat_execution.hpp"
#include "secure_frontend/config_file.hpp"
#include "secure_frontend/subsystem_limits.hpp"
#include "support/config_json.hpp"
#include "support/test_support.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <variant>

namespace {
int failures = 0;
void expect(const bool condition, const std::string& message) {
  if (!condition) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
}

std::shared_ptr<const secure_frontend::FrontendConfig> config_with(const std::map<std::string, std::string>& extra) {
  secure_frontend_test::ConfigFixture f;
  auto literals = f.literals;
  for (const auto& [k, v] : extra) literals[k] = v;
  auto draft = secure_frontend::parse_config_text(secure_frontend_test::render_config_json(literals), f.dir);
  auto result = secure_frontend::validate(std::get<secure_frontend::FrontendConfigDraft>(draft));
  return std::get<secure_frontend::ValidatedConfig>(result).shared();
}

void ready_timeout_defaults_to_900_seconds() {
  expect(secure_frontend::ChatExecutionLimits{}.ready_timeout == std::chrono::seconds(900),
         "ChatExecutionLimits{} waits 900 s for READY (upstream STRATA_ENGINE_READY_S)");
  expect(secure_frontend::chat_execution_limits(*config_with({})).ready_timeout == std::chrono::seconds(900),
         "the default config derives 900 s");
}

void subsystem_limits_follow_config() {
  const auto config = config_with({{"limits.http.max_request_bytes", "262144"},
                                   {"limits.http.max_header_bytes", "8192"},
                                   {"limits.http.max_headers", "32"},
                                   {"limits.http.request_deadline_ms", "1500"},
                                   {"listener.backlog", "64"},
                                   {"limits.chat.max_body_bytes", "131072"},
                                   {"limits.chat.default_max_tokens", "64"},
                                   {"limits.engine.ready_timeout_s", "7"}});
  const auto http = secure_frontend::http_socket_server_limits(*config);
  expect(http.max_request_bytes == 262144U && http.max_header_bytes == 8192U && http.max_headers == 32U &&
             http.request_deadline == std::chrono::milliseconds(1500) && http.backlog == 64,
         "listener and parser limits come from the config");
  const auto schema = secure_frontend::chat_schema_limits(*config);
  expect(schema.max_body_bytes == 131072U, "chat body limit comes from the config");
  const auto chat = secure_frontend::chat_execution_limits(*config);
  expect(chat.ready_timeout == std::chrono::seconds(7) && chat.default_max_tokens == 64U, "chat execution limits");
}
}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"ready_timeout_defaults_to_900_seconds", ready_timeout_defaults_to_900_seconds},
      {"subsystem_limits_follow_config", subsystem_limits_follow_config},
  }, failures);
}
```

`tests/server_e2e_test.cpp` — add:

```cpp
void ready_timeout_is_configurable() {
  const std::string chat = R"({"model":"qwen-local","messages":[{"role":"user","content":"Hi"}],"max_tokens":2})";
  for (const auto& [timeout_s, expected] : {std::pair{"1", "HTTP/1.1 503"}, std::pair{"5", "HTTP/1.1 200"}}) {
    ServerFixture f;
    auto literals = f.base_literals();
    literals["limits.engine.ready_timeout_s"] = timeout_s;
    literals["engine.args"] = "[\"--ready-delay-ms=2000\"]";
    f.write_config(literals);
    secure_frontend_test::ChildProcess server;
    expect(f.start(server, {"--config", f.config.string()}), "server starts: " + server.stderr_text());
    const auto response = f.request("POST", "/v1/chat/completions", f.bearer(), chat);
    expect(response.rfind(expected, 0) == 0,
           std::string{"ready_timeout_s="} + timeout_s + " against a 2 s READY must give " + expected + ", got " +
               response.substr(0, 16));
  }
}
```

Add `{"ready_timeout_is_configurable", ready_timeout_is_configurable},` to its `run_cases` list and `ready_timeout_is_configurable` to the first (unlabelled) `secure_frontend_add_cases(secure_frontend_server_e2e_tests …)` call.

- [ ] **Step 2: Run to verify it fails**

`subsystem_limits_test.cpp` is registered in Step 3 (it cannot compile before the header exists), so this run exercises the two behavioral tests.
Run: `cmake --build --preset linux-gcc && ctest --preset linux-gcc -R 'parser_honors|ready_timeout_is_configurable' --output-on-failure`
Expected: FAIL — `FAILED: a 128 KiB body must pass a 256 KiB limit, got: HTTP/1.1 400 Bad Req` (defect d: the parser's own 64 KiB cap) and `FAILED: ready_timeout_s=1 against a 2 s READY must give HTTP/1.1 503, got HTTP/1.1 200` (the server ignores the key and waits the hardcoded 30 s).

- [ ] **Step 3: Implement**

Register the new test binary (it now compiles):

```cmake
    secure_frontend_add_test_executable(secure_frontend_subsystem_limits_tests tests/subsystem_limits_test.cpp
        secure_frontend_subsystem_limits)
    secure_frontend_add_cases(secure_frontend_subsystem_limits_tests
        ready_timeout_defaults_to_900_seconds subsystem_limits_follow_config)
```

`http_request_handler.hpp` — add `#include "secure_frontend/config_defaults.hpp"` and `#include <cstddef>`, before `handle_http_request`:

```cpp
// Parser bounds; the listener passes the configured values (SP1 C4).
struct HttpParserLimits final {
  std::size_t max_request_bytes{static_cast<std::size_t>(config_defaults::kMaxRequestBytes)};
  std::size_t max_header_bytes{static_cast<std::size_t>(config_defaults::kMaxHeaderBytes)};
  std::size_t max_headers{static_cast<std::size_t>(config_defaults::kMaxHeaders)};
  std::size_t max_request_line_bytes{static_cast<std::size_t>(config_defaults::kMaxRequestLineBytes)};
};
```

and replace the `handle_http_request` declaration (lines 44-47) with:

```cpp
[[nodiscard]] HttpResponse handle_http_request(std::string_view request,
                                                const HttpPolicy& policy,
                                                const BearerTokenValidator& validate_bearer_token,
                                                const RequestDispatcher& dispatch_request,
                                                const HttpParserLimits& limits = {});
```

`http_request_handler.cpp`: delete lines 10-12 (`kMaximumRequestBytes`, `kMaximumHeaderBytes`, `kMaximumHeaders`). Replace lines 80-87 (the head of `parse_request`, up to the request-line length check) with:

```cpp
bool parse_request(std::string_view request, ParsedRequest* parsed, const HttpParserLimits& limits) {
  if (request.empty() || request.size() > limits.max_request_bytes) return false;
  const std::size_t headers_end = request.find("\r\n\r\n");
  if (headers_end == std::string_view::npos || headers_end + 4U > limits.max_header_bytes) return false;
  if (request.find('\n') != std::string_view::npos && request.find("\r\n") == std::string_view::npos) return false;
  const std::string_view header_block = request.substr(0, headers_end);
  const std::size_t line_end = header_block.find("\r\n");
  if (line_end == std::string_view::npos || line_end > limits.max_request_line_bytes) return false;
```

replace line 114 with:

```cpp
    if (++header_count > limits.max_headers) return false;
```

and replace lines 179-183 (the head of `handle_http_request`) with:

```cpp
HttpResponse handle_http_request(std::string_view request, const HttpPolicy& policy,
                                 const BearerTokenValidator& validate_bearer_token,
                                 const RequestDispatcher& dispatch_request, const HttpParserLimits& limits) {
  ParsedRequest parsed;
  if (!parse_request(request, &parsed, limits)) return bad_request();
```

`http_socket_server.hpp` lines 15-21 (the comment and the struct):

```cpp
// Listener and parser bounds. request_deadline covers reading the whole
// request (header and body) from accept(); the response send gets the
// remaining budget, but never less than one second. Derived from
// FrontendConfig by http_socket_server_limits().
struct HttpSocketServerLimits final {
  std::chrono::milliseconds request_deadline{config_defaults::kRequestDeadlineMs};  // header+body, total
  std::size_t max_request_bytes{static_cast<std::size_t>(config_defaults::kMaxRequestBytes)};
  std::size_t max_header_bytes{static_cast<std::size_t>(config_defaults::kMaxHeaderBytes)};
  std::size_t max_headers{static_cast<std::size_t>(config_defaults::kMaxHeaders)};
  std::size_t max_request_line_bytes{static_cast<std::size_t>(config_defaults::kMaxRequestLineBytes)};
  int backlog{static_cast<int>(config_defaults::kListenerBacklog)};
};
```

and add `#include "secure_frontend/config_defaults.hpp"` after `#include "secure_frontend/http_request_handler.hpp"`.

`http_socket_server.cpp` (`<algorithm>` is already included for `std::clamp`): replace line 327 with:

```cpp
      listen(state->socket, std::clamp(limits.backlog, 1, 4096)) != 0) {
```

and line 411 with:

```cpp
          handle_http_request(request, state_->policy, state_->validator, state_->dispatcher,
                              HttpParserLimits{state_->limits.max_request_bytes, state_->limits.max_header_bytes,
                                               state_->limits.max_headers, state_->limits.max_request_line_bytes}));
```

`chat_schema.hpp` — add `#include "secure_frontend/config_defaults.hpp"` above `#include <cstddef>`, and before the functions:

```cpp
// Chat request/response bounds; derived from FrontendConfig by chat_schema_limits().
struct ChatSchemaLimits final {
  std::size_t max_body_bytes{static_cast<std::size_t>(config_defaults::kChatMaxBodyBytes)};
  std::size_t max_messages{static_cast<std::size_t>(config_defaults::kChatMaxMessages)};
  std::size_t max_message_bytes{static_cast<std::size_t>(config_defaults::kChatMaxMessageBytes)};
  std::size_t max_completion_tokens{static_cast<std::size_t>(config_defaults::kChatMaxCompletionTokens)};
  std::size_t max_generated_bytes{static_cast<std::size_t>(config_defaults::kChatMaxGeneratedBytes)};
};
```

and replace the two declarations (lines 28-35) with:

```cpp
[[nodiscard]] std::optional<ChatCompletionRequest> parse_chat_completion_request(
    std::string_view json, const ChatSchemaLimits& limits = {});

// Produces the non-streaming response form only.  The generated text is JSON
// escaped and bounded; nullopt means either caller-owned request fields or the
// generated text are unsafe to serialize.
[[nodiscard]] std::optional<std::string> render_chat_completion_response(
    const ChatCompletionRequest& request, std::string_view generated_text, const ChatSchemaLimits& limits = {});
```

`chat_schema.cpp`: replace lines 12-17 (the six constants) with the one internal field-format bound that stays:

```cpp
constexpr std::size_t kMaximumModelBytes = 256U;  // model-id format bound, not a tuning limit
```

Replace `valid_content` with:

```cpp
bool valid_content(std::string_view value, const std::size_t max_message_bytes) {
  if (value.empty() || value.size() > max_message_bytes || !valid_utf8(value) || contains_remote_scheme(value)) return false;
  for (const unsigned char byte : value) {
    if (byte < 0x20U && byte != '\n' && byte != '\r' && byte != '\t') return false;
  }
  return true;
}
```

Replace `parse_chat_completion_request` (Task 3's version) with:

```cpp
std::optional<ChatCompletionRequest> parse_chat_completion_request(std::string_view json,
                                                                   const ChatSchemaLimits& limits) {
  if (json.empty() || json.size() > limits.max_body_bytes) return std::nullopt;
  // Depth 3: request object -> messages array -> message object. Deeper
  // input (content arrays, tool objects) fails closed at the reader.
  const JsonLimits json_limits{.max_bytes = limits.max_body_bytes, .max_depth = 3U,
                               .max_container_entries = limits.max_messages,
                               .max_string_bytes = limits.max_body_bytes};
  const auto parsed = parse_json(json, json_limits);
  const auto* root = std::get_if<JsonValue>(&parsed);
  const auto* members = root != nullptr ? root->if_object() : nullptr;
  if (members == nullptr) return std::nullopt;
  ChatCompletionRequest request;
  bool model_seen = false;
  bool messages_seen = false;
  for (const auto& member : *members) {
    if (member.key == "model") {
      const auto* value = member.value.if_string();
      if (value == nullptr || !valid_model(*value)) return std::nullopt;
      request.model = *value;
      model_seen = true;
    } else if (member.key == "messages") {
      const auto* items = member.value.if_array();
      if (items == nullptr || items->size() > limits.max_messages) return std::nullopt;
      for (const auto& item : *items) {
        const auto* fields = item.if_object();
        if (fields == nullptr) return std::nullopt;
        ChatMessage message;
        bool role_seen = false;
        bool content_seen = false;
        for (const auto& field : *fields) {
          const auto* text = field.value.if_string();
          if (text == nullptr) return std::nullopt;
          if (field.key == "role" && (*text == "system" || *text == "user" || *text == "assistant")) {
            message.role = *text;
            role_seen = true;
          } else if (field.key == "content" && valid_content(*text, limits.max_message_bytes)) {
            message.content = *text;
            content_seen = true;
          } else {
            return std::nullopt;
          }
        }
        if (!role_seen || !content_seen) return std::nullopt;
        request.messages.push_back(std::move(message));
      }
      messages_seen = true;
    } else if (member.key == "max_tokens") {
      const auto* value = member.value.if_integer();
      if (value == nullptr || *value < 0 || static_cast<std::uint64_t>(*value) > limits.max_completion_tokens) {
        return std::nullopt;
      }
      request.max_tokens = static_cast<std::size_t>(*value);
    } else if (member.key == "stream") {
      const auto* value = member.value.if_boolean();
      if (value == nullptr || *value) return std::nullopt;
    } else {
      return std::nullopt;
    }
  }
  if (!model_seen || !messages_seen || request.messages.empty()) return std::nullopt;
  return request;
}
```

and replace the first lines of `render_chat_completion_response` (signature and bound check) with:

```cpp
std::optional<std::string> render_chat_completion_response(const ChatCompletionRequest& request,
                                                           std::string_view generated_text,
                                                           const ChatSchemaLimits& limits) {
  if (!valid_model(request.model) || generated_text.size() > limits.max_generated_bytes ||
      !valid_utf8(generated_text)) {
    return std::nullopt;
  }
```

(the rest of the function is unchanged).

`chat_execution.hpp` lines 24-31:

```cpp
struct ChatExecutionLimits final {
  std::size_t maximum_prompt_tokens{static_cast<std::size_t>(config_defaults::kChatMaxPromptTokens)};
  std::size_t maximum_output_tokens{static_cast<std::size_t>(config_defaults::kChatMaxCompletionTokens)};
  std::size_t default_max_tokens{static_cast<std::size_t>(config_defaults::kChatDefaultMaxTokens)};
  std::chrono::milliseconds ready_timeout{std::chrono::seconds(config_defaults::kEngineReadyTimeoutS)};
  std::chrono::milliseconds generation_timeout{std::chrono::seconds(config_defaults::kEngineGenerationTimeoutS)};
  // Total budget for draining to DONE after STOP once generation_timeout expires.
  std::chrono::milliseconds stop_drain_timeout{std::chrono::seconds(config_defaults::kEngineStopDrainTimeoutS)};
};
```

(add `#include "secure_frontend/config_defaults.hpp"` to `chat_execution.hpp`; the comment above the struct, lines 21-23, now reads `// Bounds for the native, non-streaming chat path, derived from FrontendConfig by` / `// chat_execution_limits(). Separate from engine-protocol bounds: both layers` / `// must agree before an engine command is issued.`).

`chat_execution.cpp`: replace lines 32-34 with:

```cpp
  const auto plan_result = profile_ == NativeChatProfile::qwen
      ? make_qwen_chat_generation_plan(request, tokenizer, limits_.maximum_prompt_tokens, limits_.default_max_tokens)
      : make_glm53_chat_generation_plan(request, tokenizer, limits_.maximum_prompt_tokens, limits_.default_max_tokens);
```

`chat_generation_plan.hpp`: add `#include "secure_frontend/config_defaults.hpp"` and replace lines 40-48 with:

```cpp
[[nodiscard]] ChatGenerationPlanResult make_qwen_chat_generation_plan(
    const ChatCompletionRequest& request, const TokenizerPack& tokenizer,
    std::size_t maximum_prompt_tokens = static_cast<std::size_t>(config_defaults::kChatMaxPromptTokens),
    std::size_t default_max_tokens = static_cast<std::size_t>(config_defaults::kChatDefaultMaxTokens));

// Converts a schema-validated text request using only the fixed GLM-5.3
// profile. It neither interprets a model template nor contacts the network.
[[nodiscard]] ChatGenerationPlanResult make_glm53_chat_generation_plan(
    const ChatCompletionRequest& request, const TokenizerPack& tokenizer,
    std::size_t maximum_prompt_tokens = static_cast<std::size_t>(config_defaults::kChatMaxPromptTokens),
    std::size_t default_max_tokens = static_cast<std::size_t>(config_defaults::kChatDefaultMaxTokens));
```

`chat_generation_plan.cpp`: delete line 11 (`constexpr std::size_t kDefaultMaxTokens = 256U;`); change both definitions' parameter lists (lines 20-22 and 52-54) to:

```cpp
ChatGenerationPlanResult make_qwen_chat_generation_plan(
    const ChatCompletionRequest& request, const TokenizerPack& tokenizer,
    const std::size_t maximum_prompt_tokens, const std::size_t default_max_tokens) {
```

```cpp
ChatGenerationPlanResult make_glm53_chat_generation_plan(
    const ChatCompletionRequest& request, const TokenizerPack& tokenizer,
    const std::size_t maximum_prompt_tokens, const std::size_t default_max_tokens) {
```

and replace both returns (lines 48-49 and 71-72) with:

```cpp
  return ChatGenerationPlan{request.model, *token_ids, request.max_tokens.value_or(default_max_tokens)};
```

`include/secure_frontend/subsystem_limits.hpp`:

```cpp
#pragma once

#include "secure_frontend/chat_execution.hpp"
#include "secure_frontend/chat_schema.hpp"
#include "secure_frontend/frontend_config.hpp"
#include "secure_frontend/http_socket_server.hpp"

namespace secure_frontend {

// Derived once from the validated configuration, before bind/spawn, and
// passed by value into each subsystem's private const state.
[[nodiscard]] HttpSocketServerLimits http_socket_server_limits(const FrontendConfig& config);
[[nodiscard]] ChatSchemaLimits chat_schema_limits(const FrontendConfig& config);
[[nodiscard]] ChatExecutionLimits chat_execution_limits(const FrontendConfig& config);

}  // namespace secure_frontend
```

`src/subsystem_limits.cpp`:

```cpp
#include "secure_frontend/subsystem_limits.hpp"

namespace secure_frontend {

HttpSocketServerLimits http_socket_server_limits(const FrontendConfig& c) {
  HttpSocketServerLimits limits;
  limits.request_deadline = c.limits.http.request_deadline;
  limits.max_request_bytes = c.limits.http.max_request_bytes;
  limits.max_header_bytes = c.limits.http.max_header_bytes;
  limits.max_headers = c.limits.http.max_headers;
  limits.max_request_line_bytes = c.limits.http.max_request_line_bytes;
  limits.backlog = static_cast<int>(c.listener.backlog);
  return limits;
}

ChatSchemaLimits chat_schema_limits(const FrontendConfig& c) {
  ChatSchemaLimits limits;
  limits.max_body_bytes = c.limits.chat.max_body_bytes;
  limits.max_messages = c.limits.chat.max_messages;
  limits.max_message_bytes = c.limits.chat.max_message_bytes;
  limits.max_completion_tokens = c.limits.chat.max_completion_tokens;
  limits.max_generated_bytes = c.limits.chat.max_generated_bytes;
  return limits;
}

ChatExecutionLimits chat_execution_limits(const FrontendConfig& c) {
  ChatExecutionLimits limits;
  limits.maximum_prompt_tokens = c.limits.chat.max_prompt_tokens;
  limits.maximum_output_tokens = c.limits.chat.max_completion_tokens;
  limits.default_max_tokens = c.limits.chat.default_max_tokens;
  limits.ready_timeout = c.limits.engine.ready_timeout;
  limits.generation_timeout = c.limits.engine.generation_timeout;
  limits.stop_drain_timeout = c.limits.engine.stop_drain_timeout;
  return limits;
}

}  // namespace secure_frontend
```

`secure_server_main.cpp`: add `#include "secure_frontend/subsystem_limits.hpp"` after `#include "secure_frontend/server_cli.hpp"`. In `run()`, replace

```cpp
  const secure_frontend::Config listener = secure_frontend::listener_config(config);
```

with

```cpp
  const secure_frontend::Config listener = secure_frontend::listener_config(config);
  // Derived once, before bind/spawn; each subsystem keeps its own const copy.
  const auto schema_limits = secure_frontend::chat_schema_limits(config);
  const auto listener_limits = secure_frontend::http_socket_server_limits(config);
```

replace

```cpp
  secure_frontend::ChatExecutionService chat(protocol, {}, chat_profile);
```

with

```cpp
  secure_frontend::ChatExecutionService chat(protocol, secure_frontend::chat_execution_limits(config), chat_profile);
```

and replace the whole `auto server = secure_frontend::HttpSocketServer::bind(…);` statement with:

```cpp
  auto server = secure_frontend::HttpSocketServer::bind(
      listener, std::move(policy), [verifier](const std::string_view supplied) { return verifier->matches(supplied); },
      [&router, &chat, &engine, &tokenizer, &model_id, &schema_limits](
          const std::string_view method, const std::string_view target, const std::string_view body) {
        if (method == "GET") return router.dispatch(method, target);
        if (method != "POST" || target != "/v1/chat/completions") return json_error(404, "Not Found");
        if (!engine.running()) return json_error(503, "Service Unavailable");
        const auto request = secure_frontend::parse_chat_completion_request(body, schema_limits);
        if (!request || request->model != model_id) return json_error(400, "Bad Request");
        const auto result = chat.execute(*request, *tokenizer);
        const auto* completion = std::get_if<secure_frontend::ChatExecutionResult>(&result);
        if (completion == nullptr) return json_error(503, "Service Unavailable");
        const auto response =
            secure_frontend::render_chat_completion_response(*request, completion->text, schema_limits);
        if (!response) return json_error(503, "Service Unavailable");
        return secure_frontend::HttpResponse{
            .status = 200,
            .headers = {{"Content-Type", "application/json; charset=utf-8"}, {"Cache-Control", "no-store"}},
            .body = *response};
      },
      nullptr, listener_limits);
```

CMake:

```cmake
add_library(secure_frontend_subsystem_limits src/subsystem_limits.cpp)
target_compile_features(secure_frontend_subsystem_limits PUBLIC cxx_std_20)
target_include_directories(secure_frontend_subsystem_limits PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)
target_link_libraries(secure_frontend_subsystem_limits PUBLIC secure_frontend_typed_config
    secure_frontend_http_socket_server secure_frontend_chat_execution secure_frontend_chat_schema)
target_link_libraries(strata-secure-server PRIVATE secure_frontend_subsystem_limits)
```

and `secure_frontend_subsystem_limits` in the warnings `foreach`.

- [ ] **Step 4: Run to verify it passes**

Run: `cmake --build --preset linux-gcc && ctest --preset linux-gcc | tail -3`
Expected: `parser_honors_configured_max_request_bytes`, `ready_timeout_defaults_to_900_seconds`, `subsystem_limits_follow_config`, `ready_timeout_is_configurable` pass; the existing chat-execution and chat-schema suites pass with the new defaults; `0 tests failed`.
Run: `grep -nE 'kMaximum(RequestBytes|HeaderBytes|Headers|Messages|MessageBytes|GeneratedBytes|Tokens)\b|kDefaultMaxTokens|seconds\(30\)|64U \* 1024U' src/http_request_handler.cpp src/chat_schema.cpp src/chat_generation_plan.cpp include/secure_frontend/http_socket_server.hpp include/secure_frontend/chat_execution.hpp`
Expected: no output (every tuning limit now flows from `config_defaults` / `FrontendConfig`).
Run: `cmake --build --preset windows-llvm-mingw-cross | tail -1 && ctest --preset windows-llvm-mingw-cross | tail -1` → build ok, record result.

- [ ] **Step 5: Commit**

```bash
git -C "$(git rev-parse --show-toplevel)" add secure_frontend
git -C "$(git rev-parse --show-toplevel)" -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "limits: listener, parser, chat schema and chat execution take their limits from FrontendConfig (SP1 C4, C5; defect d)" \
  -m "Regression tests (failed before): parser_honors_configured_max_request_bytes (400 at a 256 KiB config),
ready_timeout_is_configurable, ready_timeout_defaults_to_900_seconds; plus subsystem_limits_follow_config.
Defaults live only in config_defaults.hpp; ready_timeout default is 900 s." \
  -m "$SP1_TRAILERS"
```

---
### Task 8: One relative-path grammar (P1)

**Files:**
- Create: `secure_frontend/include/secure_frontend/canonical_path.hpp`, `secure_frontend/src/canonical_path_common.cpp`, `secure_frontend/tests/paths_test.cpp`
- Modify: `secure_frontend/src/secret_source.cpp:17-41` (delete `is_ascii_space`, `contains_ascii_control`, `is_blank`), `:56-98` (delete the private grammar), `:122-129` (use `parse_relative_path`)
- Modify: `secure_frontend/src/model_inventory.cpp:21-43` (delete `is_windows_reserved_device_name`, `has_windows_ambiguous_suffix`), `:73` (shared reserved-name check), `:80-102` (use `parse_relative_path`)
- Modify: `secure_frontend/CMakeLists.txt` (library `secure_frontend_paths`; `secure_frontend_config` and `secure_frontend_model_inventory` link it; test binary)

**Interfaces:**
- Produces (Tasks 9–21):
```cpp
namespace secure_frontend {
enum class PathErrorCode {
  empty, too_long, component_too_long, absolute, traversal, invalid_character, control_character, reserved_name,
  trailing_dot_or_space, short_name_alias, invalid_utf8, drive_or_stream,                      // grammar (Task 8)
  not_absolute, unsupported_root_form, network_location, root_unavailable, untrusted_owner, shared_writable,  // roots (Task 9)
  not_found, not_directory, not_regular_file, not_owned_by_user, permissions_too_broad, symlink_not_allowed,
  escapes_root, cross_device, hard_link_not_allowed, open_failed, read_failed, too_large, create_failed,  // walks (Tasks 10-14)
};
struct PathError final { PathErrorCode code; std::string message; };   // fixed text; never contains the path
class RelativePath final {
 public:
  [[nodiscard]] const std::string& generic() const noexcept;                 // '/'-separated UTF-8
  [[nodiscard]] const std::vector<std::string>& components() const noexcept;
};
[[nodiscard]] std::variant<RelativePath, PathError> parse_relative_path(std::string_view text);
[[nodiscard]] bool is_windows_reserved_component(std::string_view component) noexcept;
[[nodiscard]] std::filesystem::path path_from_utf8(std::string_view text);
[[nodiscard]] std::string path_to_utf8(const std::filesystem::path& path);
}
```

- [ ] **Step 1: Write the failing tests** (`tests/paths_test.cpp`)

```cpp
#include "secure_frontend/canonical_path.hpp"
#include "support/test_support.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <variant>

namespace {
int failures = 0;
void expect(const bool condition, const std::string& message) {
  if (!condition) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
}
using secure_frontend::PathErrorCode;

bool rejects(const std::string& text, const PathErrorCode code) {
  const auto result = secure_frontend::parse_relative_path(text);
  const auto* error = std::get_if<secure_frontend::PathError>(&result);
  return error != nullptr && error->code == code;
}

void relative_path_accepts_portable_forms() {
  for (const char* text : {"model.gguf", "qwen/model-00001-of-00004.gguf", "Jos\xC3\xA9/mod\xC3\xA8le.gguf", "x~y", "~",
                           "CONSOLE", "COM10", "LPT", "NULL.txt", "con-fig"}) {
    expect(std::holds_alternative<secure_frontend::RelativePath>(secure_frontend::parse_relative_path(text)),
           std::string{"portable path must be accepted: "} + text);
  }
  const auto normalized = secure_frontend::parse_relative_path("a\\b\\c.gguf");
  const auto* path = std::get_if<secure_frontend::RelativePath>(&normalized);
  expect(path != nullptr && path->generic() == "a/b/c.gguf" && path->components().size() == 3U,
         "a backslash is accepted on input and normalized to '/'");
  expect(rejects("\xFF", PathErrorCode::invalid_utf8), "invalid UTF-8");
  expect(rejects(std::string(1025, 'a'), PathErrorCode::too_long), "total length is at most 1024 bytes");
  expect(rejects(std::string(256, 'a'), PathErrorCode::component_too_long), "a component is at most 255 bytes");
}

void cfg02_rejects_traversal_and_absolute() {
  for (const char* text : {"..", "../x", "a/../b", "a/..", ".", "a/./b"}) {
    expect(rejects(text, PathErrorCode::traversal), std::string{"traversal: "} + text);
  }
  for (const char* text : {"/etc/passwd", "\\x", "\\\\server\\share\\x", "\\\\?\\C:\\x", "//x"}) {
    expect(rejects(text, PathErrorCode::absolute), std::string{"absolute or UNC: "} + text);
  }
  for (const char* text : {"", "a//b", "a/", "a\\\\b"}) {
    expect(rejects(text, PathErrorCode::empty), std::string{"empty component: "} + text);
  }
}

void cfg02_rejects_nul_and_control_bytes() {
  for (const std::string text : {std::string("a\0b", 3), std::string("a\nb"), std::string("a\tb"), std::string("a\x7f"),
                                 std::string("\x01")}) {
    expect(rejects(text, PathErrorCode::control_character), "control byte must be rejected");
  }
}

void cfg04_rejects_reserved_and_device_names() {
  for (const char* text : {"CON", "con", "Con.txt", "nul.gguf", "AUX", "PRN.log", "COM0", "COM1", "com9.x", "LPT0",
                           "LPT9", "COM\xC2\xB9", "LPT\xC2\xB3.txt", "CONIN$", "conout$", "CLOCK$", "dir/NUL/file",
                           "CON .txt"}) {
    expect(rejects(text, PathErrorCode::reserved_name), std::string{"reserved device name: "} + text);
  }
}

void cfg04_rejects_trailing_dot_space_and_8dot3() {
  for (const char* text : {"model.", "model ", "dir./x", "dir /x"}) {
    expect(rejects(text, PathErrorCode::trailing_dot_or_space), std::string{"trailing dot or space: "} + text);
  }
  for (const char* text : {"PROGRA~1", "MODEL~2.GGU", "a/b~1/c"}) {
    expect(rejects(text, PathErrorCode::short_name_alias), std::string{"8.3 alias: "} + text);
  }
}

void cfg04_rejects_ads_and_drive_forms() {
  for (const char* text : {"C:x", "C:\\x", "model.gguf:stream", "model.gguf::$DATA", "a/b:c"}) {
    expect(rejects(text, PathErrorCode::drive_or_stream), std::string{"drive or stream: "} + text);
  }
  for (const char* text : {"a<b", "a>b", "a\"b", "a|b", "a?b", "a*b"}) {
    expect(rejects(text, PathErrorCode::invalid_character), std::string{"invalid character: "} + text);
  }
}
}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"relative_path_accepts_portable_forms", relative_path_accepts_portable_forms},
      {"cfg02_rejects_traversal_and_absolute", cfg02_rejects_traversal_and_absolute},
      {"cfg02_rejects_nul_and_control_bytes", cfg02_rejects_nul_and_control_bytes},
      {"cfg04_rejects_reserved_and_device_names", cfg04_rejects_reserved_and_device_names},
      {"cfg04_rejects_trailing_dot_space_and_8dot3", cfg04_rejects_trailing_dot_space_and_8dot3},
      {"cfg04_rejects_ads_and_drive_forms", cfg04_rejects_ads_and_drive_forms},
  }, failures);
}
```

CMake test block:

```cmake
    secure_frontend_add_test_executable(secure_frontend_paths_tests tests/paths_test.cpp secure_frontend_paths)
    secure_frontend_add_cases(secure_frontend_paths_tests
        relative_path_accepts_portable_forms cfg02_rejects_traversal_and_absolute cfg02_rejects_nul_and_control_bytes
        cfg04_rejects_reserved_and_device_names cfg04_rejects_trailing_dot_space_and_8dot3 cfg04_rejects_ads_and_drive_forms)
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --preset linux-gcc && cmake --build --preset linux-gcc 2>&1 | grep -m1 error`
Expected: `fatal error: secure_frontend/canonical_path.hpp: No such file or directory`.

- [ ] **Step 3: Implement**

`include/secure_frontend/canonical_path.hpp`:

```cpp
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace secure_frontend {

enum class PathErrorCode {
  empty, too_long, component_too_long, absolute, traversal, invalid_character, control_character, reserved_name,
  trailing_dot_or_space, short_name_alias, invalid_utf8, drive_or_stream,
  not_absolute, unsupported_root_form, network_location, root_unavailable, untrusted_owner, shared_writable,
  not_found, not_directory, not_regular_file, not_owned_by_user, permissions_too_broad, symlink_not_allowed,
  escapes_root, cross_device, hard_link_not_allowed, open_failed, read_failed, too_large, create_failed,
};

// `message` is fixed text that never contains the path or any part of it.
struct PathError final {
  PathErrorCode code;
  std::string message;
};

// The one portable relative-path grammar (SP1 P1). Input may use '/' or '\';
// components are 1-255 bytes of valid UTF-8, the whole path at most 1024
// bytes. Rejected: empty components, "." and "..", a leading separator,
// ':' anywhere (drives, alternate data streams), < > " | ? *, bytes below
// 0x20 and 0x7F (NUL included), a trailing dot or space, Windows reserved
// device names with any extension (CON PRN AUX NUL COM0-9 LPT0-9 incl. the
// superscript digits, CONIN$ CONOUT$ CLOCK$), and 8.3 aliases ('~' + digit).
class RelativePath final {
 public:
  [[nodiscard]] const std::string& generic() const noexcept { return generic_; }
  [[nodiscard]] const std::vector<std::string>& components() const noexcept { return components_; }

 private:
  friend std::variant<RelativePath, PathError> parse_relative_path(std::string_view text);
  RelativePath(std::string generic, std::vector<std::string> components)
      : generic_(std::move(generic)), components_(std::move(components)) {}
  std::string generic_;
  std::vector<std::string> components_;
};

[[nodiscard]] std::variant<RelativePath, PathError> parse_relative_path(std::string_view text);

// The single reserved-device-name list (also used for model IDs).
[[nodiscard]] bool is_windows_reserved_component(std::string_view component) noexcept;

// UTF-8 <-> std::filesystem::path without the Windows ANSI code page.
[[nodiscard]] std::filesystem::path path_from_utf8(std::string_view text);
[[nodiscard]] std::string path_to_utf8(const std::filesystem::path& path);

}  // namespace secure_frontend
```

`src/canonical_path_common.cpp` (the portable half of the module; Tasks 9-14 add to it):

```cpp
#include "secure_frontend/canonical_path.hpp"

#include <string>
#include <utility>

namespace secure_frontend {
namespace {

PathError fail(const PathErrorCode code, std::string message) { return PathError{code, std::move(message)}; }

char ascii_upper(const char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - ('a' - 'A')) : c; }

bool valid_utf8(const std::string_view text) {
  for (std::size_t i = 0; i < text.size();) {
    const auto b = static_cast<unsigned char>(text[i]);
    std::size_t length = 0;
    std::uint32_t point = 0;
    if (b < 0x80U) { ++i; continue; }
    if (b >= 0xc2U && b <= 0xdfU) { length = 2; point = b & 0x1fU; }
    else if (b >= 0xe0U && b <= 0xefU) { length = 3; point = b & 0x0fU; }
    else if (b >= 0xf0U && b <= 0xf4U) { length = 4; point = b & 0x07U; }
    else return false;
    if (text.size() - i < length) return false;
    for (std::size_t k = 1; k < length; ++k) {
      const auto c = static_cast<unsigned char>(text[i + k]);
      if ((c & 0xc0U) != 0x80U) return false;
      point = (point << 6U) | (c & 0x3fU);
    }
    if ((length == 3 && point < 0x800U) || (length == 4 && (point < 0x10000U || point > 0x10ffffU)) ||
        (point >= 0xd800U && point <= 0xdfffU)) return false;
    i += length;
  }
  return true;
}

std::optional<PathError> check_component(const std::string_view c) {
  if (c.empty()) return fail(PathErrorCode::empty, "path components must not be empty");
  if (c.size() > 255U) return fail(PathErrorCode::component_too_long, "a path component is longer than 255 bytes");
  if (c == "." || c == "..") return fail(PathErrorCode::traversal, "path components must not be . or ..");
  for (const char ch : c) {
    const auto b = static_cast<unsigned char>(ch);
    if (b < 0x20U || b == 0x7fU) return fail(PathErrorCode::control_character, "paths must not contain control bytes");
    if (ch == ':') return fail(PathErrorCode::drive_or_stream, "paths must not contain ':' (drives, streams)");
    if (ch == '<' || ch == '>' || ch == '"' || ch == '|' || ch == '?' || ch == '*') {
      return fail(PathErrorCode::invalid_character, "paths must not contain < > \" | ? *");
    }
  }
  if (c.back() == '.' || c.back() == ' ') {
    return fail(PathErrorCode::trailing_dot_or_space, "path components must not end with a dot or space");
  }
  if (is_windows_reserved_component(c)) return fail(PathErrorCode::reserved_name, "Windows reserved device names are not allowed");
  for (std::size_t i = 0; i + 1 < c.size(); ++i) {
    if (c[i] == '~' && c[i + 1] >= '0' && c[i + 1] <= '9') {
      return fail(PathErrorCode::short_name_alias, "8.3 short-name aliases (~1) are not allowed");
    }
  }
  return std::nullopt;
}

}  // namespace

bool is_windows_reserved_component(const std::string_view component) noexcept {
  std::string_view base = component.substr(0, component.find('.'));
  while (!base.empty() && base.back() == ' ') base.remove_suffix(1);
  std::string upper;
  for (const char c : base) upper.push_back(ascii_upper(c));
  if (upper == "CON" || upper == "PRN" || upper == "AUX" || upper == "NUL" || upper == "CONIN$" ||
      upper == "CONOUT$" || upper == "CLOCK$") return true;
  if (upper.size() >= 4U && (upper.rfind("COM", 0) == 0 || upper.rfind("LPT", 0) == 0)) {
    const std::string_view suffix = std::string_view{upper}.substr(3);
    if (suffix.size() == 1U && suffix[0] >= '0' && suffix[0] <= '9') return true;
    if (suffix == "\xC2\xB9" || suffix == "\xC2\xB2" || suffix == "\xC2\xB3") return true;  // superscript 1 2 3
  }
  return false;
}

std::variant<RelativePath, PathError> parse_relative_path(const std::string_view text) {
  if (text.empty()) return fail(PathErrorCode::empty, "the path is empty");
  if (text.size() > 1024U) return fail(PathErrorCode::too_long, "the path is longer than 1024 bytes");
  if (text.front() == '/' || text.front() == '\\') return fail(PathErrorCode::absolute, "the path must be relative");
  if (!valid_utf8(text)) return fail(PathErrorCode::invalid_utf8, "the path is not valid UTF-8");
  std::vector<std::string> components;
  std::size_t start = 0;
  while (true) {
    const auto end = text.find_first_of("/\\", start);
    const auto component = text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
    if (auto error = check_component(component)) return *error;
    components.emplace_back(component);
    if (end == std::string_view::npos) break;
    start = end + 1U;
  }
  std::string generic;
  for (const auto& component : components) generic += (generic.empty() ? "" : "/") + component;
  return RelativePath{std::move(generic), std::move(components)};
}

std::filesystem::path path_from_utf8(const std::string_view text) {
  const std::u8string utf8(text.begin(), text.end());
  return std::filesystem::path{utf8};
}

std::string path_to_utf8(const std::filesystem::path& path) {
  const std::u8string utf8 = path.u8string();
  return std::string(utf8.begin(), utf8.end());
}

}  // namespace secure_frontend
```

(add `#include <optional>`).

`src/secret_source.cpp`: add `#include "secure_frontend/canonical_path.hpp"`; delete lines 17-41 and 56-98; replace lines 122-129 with:

```cpp
SecretSourceValidation make_protected_file_secret_source(const std::string_view protected_file_path) {
  const auto parsed = parse_relative_path(protected_file_path);
  const auto* path = std::get_if<RelativePath>(&parsed);
  if (path == nullptr) {
    return SecretSourceError{.code = SecretSourceErrorCode::invalid_protected_file_path,
                             .message = "Protected-file paths must be safe relative paths below the configured secret root."};
  }
  return SecretSource{SecretSourceKind::protected_file, path->generic()};
}
```

`src/model_inventory.cpp`: add `#include "secure_frontend/canonical_path.hpp"`; delete lines 21-43; line 73 becomes `if (id.back() == '.' || is_windows_reserved_component(id)) {`; replace lines 80-102 with:

```cpp
ArtifactPathValidation validate_local_artifact_path(const std::string_view path) {
  const auto parsed = parse_relative_path(path);
  if (const auto* valid = std::get_if<RelativePath>(&parsed)) return valid->generic();
  return ModelInventoryError{ModelInventoryErrorCode::invalid_artifact_path,
                             "artifact paths must be portable relative paths below the configured model root"};
}
```

CMake:

```cmake
add_library(secure_frontend_paths src/canonical_path_common.cpp)
target_compile_features(secure_frontend_paths PUBLIC cxx_std_20)
target_include_directories(secure_frontend_paths PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)
target_link_libraries(secure_frontend_config PUBLIC secure_frontend_paths)
target_link_libraries(secure_frontend_model_inventory PUBLIC secure_frontend_paths)
```

and `secure_frontend_paths` in the warnings `foreach`.

- [ ] **Step 4: Run to verify it passes**

Run: `cmake --build --preset linux-gcc && ctest --preset linux-gcc | tail -3`
Expected: the six `secure_frontend_paths_tests.*` cases pass; `secret_source`, `model_inventory`, `engine_arguments` suites pass unchanged; `0 tests failed`.
Run: `cmake --build --preset windows-llvm-mingw-cross | tail -1 && ctest --preset windows-llvm-mingw-cross | tail -1` → build ok, `0 tests failed`.

- [ ] **Step 5: Commit**

```bash
git -C "$(git rev-parse --show-toplevel)" add secure_frontend
git -C "$(git rev-parse --show-toplevel)" -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "paths: one relative-path grammar for secret sources and model artifacts (SP1 P1)" \
  -m "Regression tests: cfg02_rejects_traversal_and_absolute, cfg02_rejects_nul_and_control_bytes,
cfg04_rejects_reserved_and_device_names (COM0, superscripts, CONIN\$), cfg04_rejects_trailing_dot_space_and_8dot3,
cfg04_rejects_ads_and_drive_forms, relative_path_accepts_portable_forms. The secret-source and inventory grammars
(which disagreed on trailing dot/space and COM0) are deleted." \
  -m "$SP1_TRAILERS"
```

---

### Task 9: Canonical roots, the engine executable, containment, and the engine's model path (P2, P3, P7; defect e)

**Files:**
- Modify: `secure_frontend/include/secure_frontend/canonical_path.hpp` (roots, executable, containment, Windows root-form checks)
- Create: `secure_frontend/src/canonical_path_internal.hpp`, `secure_frontend/src/canonical_path_posix.cpp`, `secure_frontend/src/canonical_path_windows.cpp`, `secure_frontend/src/windows_security.hpp`, `secure_frontend/src/windows_security.cpp`
- Modify: `secure_frontend/src/token_reader.cpp:170-312` (the Windows helpers move to `windows_security.cpp`; the reader includes `windows_security.hpp`)
- Modify: `secure_frontend/include/secure_frontend/frontend_config.hpp` (`Roots`, `EngineLaunch`), `secure_frontend/src/frontend_config.cpp` (`absolute_root` → `open_root`/`open_executable`; `describe`)
- Modify: `secure_frontend/include/secure_frontend/engine_arguments.hpp:42-45`, `secure_frontend/src/engine_arguments.cpp` (`build_engine_arguments` takes `const CanonicalRoot&`)
- Modify: `secure_frontend/src/secure_server_main.cpp` (`.path()` on roots and the executable)
- Modify: `secure_frontend/tests/engine_arguments_test.cpp` (real roots), `secure_frontend/tests/paths_test.cpp`, `secure_frontend/tests/server_e2e_test.cpp`
- Modify: `secure_frontend/CMakeLists.txt` (paths sources; `advapi32` on the paths library; typed config and engine arguments link paths)

**Interfaces:**
- Consumes: `RelativePath`, `parse_relative_path`, `path_from_utf8`, `path_to_utf8`, `PathError` (Task 8).
- Produces (Tasks 10–21):
```cpp
namespace secure_frontend {
#if defined(_WIN32)
using NativeHandle = void*;     // HANDLE
#else
using NativeHandle = int;       // file descriptor (O_PATH on Linux)
#endif
class OwnedHandle final { public: explicit OwnedHandle(NativeHandle) noexcept; ~OwnedHandle(); [[nodiscard]] NativeHandle get() const noexcept; [[nodiscard]] NativeHandle release() noexcept; };
class CanonicalRoot final {      // copyable; shares one open handle
 public:
  [[nodiscard]] const std::filesystem::path& path() const noexcept;   // canonical, absolute
  [[nodiscard]] NativeHandle handle() const noexcept;
  [[nodiscard]] std::uint64_t device_id() const noexcept;            // st_dev (POSIX), 0 on Windows
  [[nodiscard]] std::filesystem::path join(const RelativePath& relative) const;
};
class CanonicalFile final { public: [[nodiscard]] const std::filesystem::path& path() const noexcept; };
enum class RootPolicy { existing };                                  // Task 14 adds create_private
[[nodiscard]] std::variant<CanonicalRoot, PathError> open_root(const std::filesystem::path& requested, RootPolicy policy = RootPolicy::existing);
[[nodiscard]] std::variant<CanonicalFile, PathError> open_executable(const std::filesystem::path& requested);
[[nodiscard]] bool is_within(const std::filesystem::path& root, const std::filesystem::path& candidate) noexcept;
[[nodiscard]] std::optional<PathError> check_windows_root_form(std::wstring_view text);         // portable, pure
[[nodiscard]] std::variant<std::wstring, PathError> classify_final_path(std::wstring_view final_path);  // portable, pure
// engine_arguments.hpp
[[nodiscard]] EngineArgumentsResult build_engine_arguments(EngineArgumentsProfile profile, const ModelManifest& selected,
                                                           const CanonicalRoot& models_root, const std::vector<std::string>& user_arguments);
// frontend_config.hpp
struct Roots final { CanonicalRoot models; CanonicalRoot tokenizer_pack; std::optional<CanonicalRoot> secret, logs, cache; };
struct EngineLaunch final { EngineProfile profile; CanonicalFile executable; ModelId model_id; std::vector<std::string> args; };
}
// src/windows_security.hpp (Windows only, internal to the paths, token reader and token store libraries)
namespace secure_frontend::windows_security {
bool current_user_sid(std::array<std::byte, SECURITY_MAX_SID_SIZE>& storage, PSID* result);
std::wstring current_user_sid_string();
bool has_private_owner_dacl(HANDLE handle, PSID current_user);
enum class Trust { trusted, untrusted_owner, shared_writable, unreadable };
Trust write_access_is_private(HANDLE handle);
std::wstring final_path_for_handle(HANDLE handle);
bool is_reparse_point(HANDLE handle);
std::wstring extended_path(const std::wstring& absolute);           // adds \\?\ (never accepted as input)
class PrivateSecurityAttributes final { public: explicit PrivateSecurityAttributes(bool directory); ~PrivateSecurityAttributes(); SECURITY_ATTRIBUTES* get() noexcept; };
}
```

- [ ] **Step 1: Write the failing tests**

Append to `tests/paths_test.cpp` (before `}  // namespace`; add `#include <fstream>`, and `#include <sys/stat.h>` / `#include <unistd.h>` under `#if !defined(_WIN32)`):

```cpp
template <typename T>
bool fails_with(const std::variant<T, secure_frontend::PathError>& result, const PathErrorCode code) {
  const auto* error = std::get_if<secure_frontend::PathError>(&result);
  return error != nullptr && error->code == code;
}

void cfg02_rejects_relative_root() {
  for (const char* text : {"models", "./models", "../models"}) {
    expect(fails_with(secure_frontend::open_root(text), PathErrorCode::not_absolute), std::string{"relative root: "} + text);
  }
  expect(fails_with(secure_frontend::open_root(""), PathErrorCode::empty), "empty root");
  const auto missing = secure_frontend_test::make_scratch_directory() / "missing";
  expect(fails_with(secure_frontend::open_root(missing), PathErrorCode::root_unavailable), "missing root");
}

void cfg02_sibling_prefix_root_rejected() {
#if defined(_WIN32)
  expect(!secure_frontend::is_within(L"C:\\r", L"C:\\r2\\model.gguf"), "C:\\r does not contain C:\\r2");
  expect(secure_frontend::is_within(L"C:\\r", L"c:\\R\\model.gguf"), "comparison ignores case on a separator boundary");
  expect(secure_frontend::is_within(L"C:\\r\\", L"C:\\r\\x"), "a trailing separator on the root is fine");
#else
  expect(!secure_frontend::is_within("/srv/r", "/srv/r2/model.gguf"), "/srv/r does not contain /srv/r2");
  expect(secure_frontend::is_within("/srv/r", "/srv/r/model.gguf"), "/srv/r contains its child");
  expect(secure_frontend::is_within("/srv/r/", "/srv/r/x"), "a trailing separator on the root is fine");
  expect(!secure_frontend::is_within("/srv/r", "/srv/R/x"), "POSIX comparison is exact");
#endif
}

void cfg04_rejects_unc_device_and_extended_roots() {
  for (const wchar_t* text : {L"C:foo", L"\\foo", L"/foo", L"\\\\server\\share\\x", L"\\\\?\\C:\\x", L"\\\\.\\C:\\x",
                              L"\\??\\C:\\x", L"//server/share", L"C:\\models\\file.gguf:stream", L"C:\\CON\\x", L"models"}) {
    expect(secure_frontend::check_windows_root_form(text).has_value(), "unsafe Windows root form must be rejected");
  }
  for (const wchar_t* text : {L"C:\\Users\\Jos\u00e9\\models", L"D:/models", L"c:\\"}) {
    expect(!secure_frontend::check_windows_root_form(text).has_value(), "drive-absolute roots are accepted");
  }
}

// Review Focus 2: a root under OneDrive Known Folder Move or a roaming profile
// resolves to \\?\UNC\...; the error must say what happened and what to do.
void cfg04_final_path_on_network_share_is_actionable() {
  const auto share = secure_frontend::classify_final_path(L"\\\\?\\UNC\\server\\share\\models");
  const auto* error = std::get_if<secure_frontend::PathError>(&share);
  expect(error != nullptr && error->code == PathErrorCode::network_location, "a UNC final path is rejected");
  if (error != nullptr) {
    expect(error->message.find("network share") != std::string::npos &&
               error->message.find("local folder") != std::string::npos,
           "the message is actionable: " + error->message);
  }
  const auto local = secure_frontend::classify_final_path(L"\\\\?\\C:\\Users\\x\\models");
  const auto* text = std::get_if<std::wstring>(&local);
  expect(text != nullptr && *text == L"C:\\Users\\x\\models", "the \\\\?\\ prefix is stripped from a local final path");
}

#if !defined(_WIN32)
void cfg02_rejects_shared_writable_root() {
  for (const mode_t mode : {0777, 0775, 0757}) {
    const auto root = secure_frontend_test::make_scratch_directory();
    chmod(root.c_str(), mode);
    expect(fails_with(secure_frontend::open_root(root), PathErrorCode::shared_writable), "group/other-writable root");
  }
  const auto ok = secure_frontend_test::make_scratch_directory();
  chmod(ok.c_str(), 0755);
  expect(std::holds_alternative<secure_frontend::CanonicalRoot>(secure_frontend::open_root(ok)), "0755 root is fine");
}

void cfg02_root_may_traverse_symlink() {
  const auto scratch = secure_frontend_test::make_scratch_directory();
  secure_frontend_test::make_private_directory(scratch / "real");
  std::filesystem::create_directory_symlink(scratch / "real", scratch / "link");
  const auto root = secure_frontend::open_root(scratch / "link");
  const auto* opened = std::get_if<secure_frontend::CanonicalRoot>(&root);
  expect(opened != nullptr, "a root given through a symlink is accepted");
  if (opened != nullptr) {
    expect(opened->path() == std::filesystem::canonical(scratch / "real"), "and resolved once to its canonical form");
  }
}

void cfg02_engine_executable_must_be_private_regular_file() {
  const auto dir = secure_frontend_test::make_scratch_directory();
  secure_frontend_test::write_file(dir / "engine", "x", 0700);
  expect(std::holds_alternative<secure_frontend::CanonicalFile>(secure_frontend::open_executable(dir / "engine")),
         "a 0700 file in a 0700 directory is accepted");
  secure_frontend_test::write_file(dir / "open", "x", 0777);
  expect(fails_with(secure_frontend::open_executable(dir / "open"), PathErrorCode::shared_writable), "0777 executable");
  expect(fails_with(secure_frontend::open_executable(dir), PathErrorCode::not_regular_file), "a directory");
  expect(fails_with(secure_frontend::open_executable("engine"), PathErrorCode::not_absolute), "relative");
  const auto shared = secure_frontend_test::make_scratch_directory();
  secure_frontend_test::write_file(shared / "engine", "x", 0700);
  chmod(shared.c_str(), 0777);
  expect(fails_with(secure_frontend::open_executable(shared / "engine"), PathErrorCode::shared_writable),
         "an executable in a shared-writable directory");
}
#endif
```

and their `run_cases` entries:

```cpp
      {"cfg02_rejects_relative_root", cfg02_rejects_relative_root},
      {"cfg02_sibling_prefix_root_rejected", cfg02_sibling_prefix_root_rejected},
      {"cfg04_rejects_unc_device_and_extended_roots", cfg04_rejects_unc_device_and_extended_roots},
      {"cfg04_final_path_on_network_share_is_actionable", cfg04_final_path_on_network_share_is_actionable},
#if !defined(_WIN32)
      {"cfg02_rejects_shared_writable_root", cfg02_rejects_shared_writable_root},
      {"cfg02_root_may_traverse_symlink", cfg02_root_may_traverse_symlink},
      {"cfg02_engine_executable_must_be_private_regular_file", cfg02_engine_executable_must_be_private_regular_file},
#endif
```

CMake (extend the paths test registration):

```cmake
    secure_frontend_add_cases(secure_frontend_paths_tests
        cfg02_rejects_relative_root cfg02_sibling_prefix_root_rejected cfg04_rejects_unc_device_and_extended_roots
        cfg04_final_path_on_network_share_is_actionable)
    if(NOT WIN32)
        secure_frontend_add_cases(secure_frontend_paths_tests
            cfg02_rejects_shared_writable_root cfg02_root_may_traverse_symlink
            cfg02_engine_executable_must_be_private_regular_file)
    endif()
```

`tests/server_e2e_test.cpp` (P7 / defect e; Linux, the fixture's models root reached through a symlink):

```cpp
#if !defined(_WIN32)
void cfg03_engine_receives_canonical_models_root() {
  ServerFixture f;
  const auto link = f.dir / "models-link";
  std::filesystem::create_directory_symlink(f.models, link);
  const auto dump = f.dir / "engine-argv.txt";
  auto flags = f.base_flags();
  for (std::size_t i = 0; i + 1 < flags.size(); ++i) if (flags[i] == "--models-root") flags[i + 1] = link.string();
  flags.insert(flags.end(), {"--engine-arg", "--dump=" + dump.string()});
  secure_frontend_test::ChildProcess server;
  expect(f.start(server, flags), "server starts: " + server.stderr_text());
  const std::string argv_dump = secure_frontend_test::read_file(dump);
  const std::string canonical = std::filesystem::canonical(f.models).string();
  expect(contains(argv_dump, "argv:--native\nargv:" + canonical + "/qwen-local.gguf\n"),
         "--native carries the canonical models root: " + argv_dump);
  expect(!contains(argv_dump, "models-link"), "the raw (symlinked) root never reaches the engine");
}
#endif
```

registered in the e2e `run_cases` under `#if !defined(_WIN32)` and in CMake with `if(NOT WIN32) secure_frontend_add_cases(secure_frontend_server_e2e_tests cfg03_engine_receives_canonical_models_root ${SECURE_FRONTEND_SERVER_ARGS} TIMEOUT 120) endif()`.

`tests/engine_arguments_test.cpp` — replace the whole file (as Task 2 left it) with the version below. Every case opens a real scratch directory as the models root (`build_engine_arguments` now takes a `CanonicalRoot`), and every expected `--native` path is the canonical root joined with the artifact path:

```cpp
#include "secure_frontend/canonical_path.hpp"
#include "secure_frontend/engine_arguments.hpp"
#include "support/test_support.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <variant>
#include <vector>

namespace {
int failures = 0;
constexpr auto kStrata = secure_frontend::EngineArgumentsProfile::strata_0143;
constexpr auto kGlm = secure_frontend::EngineArgumentsProfile::lighttransport_glm53;
void expect(bool c, const std::string& m) { if (!c) { ++failures; std::cerr << "FAILED: " << m << '\n'; } }

secure_frontend::ModelManifest manifest() {
  secure_frontend::ModelManifest m; m.id = "qwen-local"; m.relative_artifact_path = "qwen/model-00001-of-00004.gguf"; return m;
}

// A private scratch directory opened as a canonical root (the module's only way to make one).
secure_frontend::CanonicalRoot models_root() {
  auto root = secure_frontend::open_root(secure_frontend_test::make_scratch_directory());
  if (const auto* error = std::get_if<secure_frontend::PathError>(&root)) {
    std::cerr << "scratch root rejected: " << error->message << '\n';
    std::exit(EXIT_FAILURE);
  }
  return std::get<secure_frontend::CanonicalRoot>(root);
}

std::string native_path(const secure_frontend::CanonicalRoot& root) {
  return (root.path() / "qwen" / "model-00001-of-00004.gguf").make_preferred().string();
}

void engine_receives_model_path_from_inventory() {
  const auto root = models_root();
  const auto result = secure_frontend::build_engine_arguments(kStrata, manifest(), root, {"--max-context", "8192"});
  const auto* args = std::get_if<std::vector<std::string>>(&result);
  expect(args != nullptr, "valid pass-through must build");
  if (!args) return;
  const std::vector<std::string> expected{"--max-context", "8192", "--native", native_path(root)};
  expect(*args == expected, "frontend must append --native from the inventory, last");
}

void engine_args_reject_owned_and_network_flags() {
  const auto root = models_root();
  for (const std::string bad : {"--native", "--native=/x.gguf", "--serve", "--serve=1", "--batch", "--batch=2", "--slots", "--slots=4", "--vision", "--vision=/m.gguf"}) {
    const auto r = secure_frontend::build_engine_arguments(kStrata, manifest(), root, {bad, "value"});
    const auto* e = std::get_if<secure_frontend::EngineArgumentsError>(&r);
    expect(e != nullptr, "must reject " + bad);
    if (e) expect(e->code == secure_frontend::EngineArgumentsErrorCode::owned_flag || e->code == secure_frontend::EngineArgumentsErrorCode::unsupported_mode, "code for " + bad);
  }
  const auto nul = secure_frontend::build_engine_arguments(kStrata, manifest(), root, {std::string("--gpu\0 0", 8)});
  expect(std::holds_alternative<secure_frontend::EngineArgumentsError>(nul), "NUL in an argument must be rejected");
  const auto empty = secure_frontend::build_engine_arguments(kStrata, manifest(), root, {});
  expect(std::holds_alternative<std::vector<std::string>>(empty), "no pass-through arguments is valid: the frontend supplies the model");
}

void strata_profile_passes_pack_and_weight_flags_through() {
  // --pack is the expert directory (loaded unconditionally, default cwd-relative pack/full) and the
  // other weight sources are operator-supplied; only --native comes from the inventory.
  const auto root = models_root();
  const std::vector<std::string> user{"--pack", "/w/pack", "--ple-gguf", "/w/ple.gguf", "--max-context", "8192"};
  const auto result = secure_frontend::build_engine_arguments(kStrata, manifest(), root, user);
  const auto* args = std::get_if<std::vector<std::string>>(&result);
  expect(args != nullptr, "--pack and weight flags must pass through");
  if (!args) return;
  auto expected = user;
  expected.emplace_back("--native");
  expected.push_back(native_path(root));
  expect(*args == expected, "pass-through unchanged, --native appended last");
  for (const std::string flag : {"--pack=/w/pack", "--embd-gguf", "--native-head-gguf", "--native-dense-gguf", "--mtp"}) {
    const auto r = secure_frontend::build_engine_arguments(kStrata, manifest(), root, {flag, "/w/x"});
    expect(std::holds_alternative<std::vector<std::string>>(r), "weight flag must pass through: " + flag);
  }
}

void rejects_invalid_relative_artifact_path() {
  const auto root = models_root();
  for (const std::string bad : {"../x.gguf", "/abs.gguf"}) {
    auto m = manifest();
    m.relative_artifact_path = bad;
    const auto r = secure_frontend::build_engine_arguments(kStrata, m, root, {"--max-context", "8192"});
    const auto* e = std::get_if<secure_frontend::EngineArgumentsError>(&r);
    expect(e != nullptr, "invalid artifact path must be rejected: " + bad);
    if (e) expect(e->code == secure_frontend::EngineArgumentsErrorCode::invalid_value, "invalid_value for " + bad);
    if (const auto* a = std::get_if<std::vector<std::string>>(&r)) {
      for (const auto& x : *a) expect(x != "--native", "no --native for " + bad);
    }
  }
}

void glm_profile_passes_arguments_through_without_native() {
  const auto root = models_root();
  const std::vector<std::string> user{"--model", "/x/glm.gguf", "--tokens", "/x/tok"};
  const auto result = secure_frontend::build_engine_arguments(kGlm, manifest(), root, user);
  const auto* args = std::get_if<std::vector<std::string>>(&result);
  expect(args != nullptr, "GLM pass-through must build");
  if (args) expect(*args == user, "GLM arguments must pass through unchanged with no --native");
  for (const std::string bad : {"--serve", "--serve=1"}) {
    const auto r = secure_frontend::build_engine_arguments(kGlm, manifest(), root, {bad});
    const auto* e = std::get_if<secure_frontend::EngineArgumentsError>(&r);
    expect(e != nullptr && e->code == secure_frontend::EngineArgumentsErrorCode::owned_flag, "GLM must reject " + bad);
  }
  const auto nul = secure_frontend::build_engine_arguments(kGlm, manifest(), root, {std::string("--gpu\0 0", 8)});
  expect(std::holds_alternative<secure_frontend::EngineArgumentsError>(nul), "GLM: NUL in an argument must be rejected");
  const auto batch = secure_frontend::build_engine_arguments(kGlm, manifest(), root, {"--batch", "2"});
  expect(std::holds_alternative<std::vector<std::string>>(batch), "GLM: the Strata deny-list does not apply");
}
}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"engine_receives_model_path_from_inventory", engine_receives_model_path_from_inventory},
      {"engine_args_reject_owned_and_network_flags", engine_args_reject_owned_and_network_flags},
      {"strata_profile_passes_pack_and_weight_flags_through", strata_profile_passes_pack_and_weight_flags_through},
      {"rejects_invalid_relative_artifact_path", rejects_invalid_relative_artifact_path},
      {"glm_profile_passes_arguments_through_without_native", glm_profile_passes_arguments_through_without_native},
  }, failures);
}
```

- [ ] **Step 2: Run to verify it fails**

The e2e case uses only the harness, so its binary builds before the module exists:
Run: `cmake --preset linux-gcc && cmake --build --preset linux-gcc --target secure_frontend_server_e2e_tests strata-secure-server secure_frontend_fake_engine && ctest --preset linux-gcc -R cfg03_engine_receives --output-on-failure`
Expected: FAIL with `FAILED: --native carries the canonical models root: …argv:<scratch>/models-link/qwen-local.gguf` (defect e).
Run: `cmake --build --preset linux-gcc 2>&1 | grep -m1 error`
Expected: `error: 'open_root' is not a member of 'secure_frontend'` (paths and engine-arguments tests).

- [ ] **Step 3: Implement**

Add to `canonical_path.hpp` (after `path_to_utf8`; add `#include <memory>`, `#include <optional>`):

```cpp
#if defined(_WIN32)
using NativeHandle = void*;  // HANDLE
#else
using NativeHandle = int;    // file descriptor
#endif

class OwnedHandle final {
 public:
  explicit OwnedHandle(NativeHandle handle) noexcept : handle_(handle) {}
  ~OwnedHandle();
  OwnedHandle(const OwnedHandle&) = delete;
  OwnedHandle& operator=(const OwnedHandle&) = delete;
  [[nodiscard]] NativeHandle get() const noexcept { return handle_; }
  // Gives up ownership (the destructor then closes nothing) and returns the handle.
  [[nodiscard]] NativeHandle release() noexcept;

 private:
  NativeHandle handle_;
};

namespace detail {
struct PathFactory;
}

// A root directory resolved once to its canonical absolute form (the root
// itself may be reached through a symlink or junction) and held open. It is
// owned by the current user (or root / SYSTEM / Administrators) and not
// writable by anyone else. Everything below it is opened relative to it.
class CanonicalRoot final {
 public:
  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
  [[nodiscard]] NativeHandle handle() const noexcept { return handle_->get(); }
  [[nodiscard]] std::uint64_t device_id() const noexcept { return device_; }
  // The canonical root joined with a validated relative path (native separators).
  [[nodiscard]] std::filesystem::path join(const RelativePath& relative) const;

 private:
  friend struct detail::PathFactory;
  CanonicalRoot(std::filesystem::path path, std::shared_ptr<const OwnedHandle> handle, const std::uint64_t device)
      : path_(std::move(path)), handle_(std::move(handle)), device_(device) {}
  std::filesystem::path path_;
  std::shared_ptr<const OwnedHandle> handle_;
  std::uint64_t device_;
};

// engine.executable: absolute, canonical, a regular file not writable by
// group/other (other SIDs on Windows), in a directory with the same property.
class CanonicalFile final {
 public:
  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

 private:
  friend struct detail::PathFactory;
  explicit CanonicalFile(std::filesystem::path path) : path_(std::move(path)) {}
  std::filesystem::path path_;
};

enum class RootPolicy { existing };

[[nodiscard]] std::variant<CanonicalRoot, PathError> open_root(const std::filesystem::path& requested,
                                                               RootPolicy policy = RootPolicy::existing);
[[nodiscard]] std::variant<CanonicalFile, PathError> open_executable(const std::filesystem::path& requested);

// Containment on a separator boundary: "/r" does not contain "/r2". Windows
// compares case-insensitively (CompareStringOrdinal).
[[nodiscard]] bool is_within(const std::filesystem::path& root, const std::filesystem::path& candidate) noexcept;

// Pure string checks, compiled on every platform so they are tested everywhere.
// Accepts only drive-absolute X:\... roots; rejects drive-relative C:foo,
// rooted \foo, UNC \\server\share, \\?\, \\.\, \??\, streams and reserved names.
[[nodiscard]] std::optional<PathError> check_windows_root_form(std::wstring_view text);
// GetFinalPathNameByHandleW output -> X:\... ; \\?\UNC\... is network_location.
[[nodiscard]] std::variant<std::wstring, PathError> classify_final_path(std::wstring_view final_path);
```

`src/canonical_path_internal.hpp`:

```cpp
#pragma once
#include "secure_frontend/canonical_path.hpp"

#include <memory>
#include <string>
#include <utility>

namespace secure_frontend::detail {

struct PathFactory final {
  static CanonicalRoot root(std::filesystem::path path, const NativeHandle handle, const std::uint64_t device) {
    return CanonicalRoot{std::move(path), std::make_shared<const OwnedHandle>(handle), device};
  }
  static CanonicalFile file(std::filesystem::path path) { return CanonicalFile{std::move(path)}; }
};

inline PathError path_error(const PathErrorCode code, std::string message) { return PathError{code, std::move(message)}; }

// Opens an existing root (platform files); open_root() wraps it.
std::variant<CanonicalRoot, PathError> open_existing_root(const std::filesystem::path& requested);

}  // namespace secure_frontend::detail
```

Append to `src/canonical_path_common.cpp` (add `#include "canonical_path_internal.hpp"`):

```cpp
std::filesystem::path CanonicalRoot::join(const RelativePath& relative) const {
  std::filesystem::path joined = path_;
  for (const auto& component : relative.components()) joined /= path_from_utf8(component);
  joined.make_preferred();
  return joined;
}

std::variant<CanonicalRoot, PathError> open_root(const std::filesystem::path& requested, const RootPolicy policy) {
  (void)policy;
  return detail::open_existing_root(requested);
}

std::optional<PathError> check_windows_root_form(const std::wstring_view text) {
  const auto is_separator = [](const wchar_t c) { return c == L'\\' || c == L'/'; };
  if (text.empty()) return detail::path_error(PathErrorCode::empty, "the root path is empty");
  if (text.size() >= 2U && is_separator(text[0]) && is_separator(text[1])) {
    return detail::path_error(PathErrorCode::unsupported_root_form, "UNC, device and \\\\?\\ paths are not accepted as roots");
  }
  if (text.substr(0, 4) == L"\\??\\") {
    return detail::path_error(PathErrorCode::unsupported_root_form, "NT object paths are not accepted as roots");
  }
  if (is_separator(text[0])) {
    return detail::path_error(PathErrorCode::not_absolute, "a rooted path without a drive letter depends on the current drive");
  }
  const bool drive_letter = text.size() >= 2U && ((text[0] >= L'A' && text[0] <= L'Z') || (text[0] >= L'a' && text[0] <= L'z')) &&
                            text[1] == L':';
  if (!drive_letter) return detail::path_error(PathErrorCode::not_absolute, "roots must be absolute (X:\\...)");
  if (text.size() == 2U || !is_separator(text[2])) {
    return detail::path_error(PathErrorCode::not_absolute, "a drive-relative path (C:foo) depends on the current directory");
  }
  std::size_t start = 3U;
  while (start < text.size()) {
    std::size_t end = start;
    while (end < text.size() && !is_separator(text[end])) ++end;
    const std::wstring_view component = text.substr(start, end - start);
    if (component.find(L':') != std::wstring_view::npos) {
      return detail::path_error(PathErrorCode::drive_or_stream, "roots must not name an alternate data stream");
    }
    std::string ascii;
    for (const wchar_t c : component) ascii.push_back(c < 0x80 ? static_cast<char>(c) : '_');
    if (!ascii.empty() && is_windows_reserved_component(ascii)) {
      return detail::path_error(PathErrorCode::reserved_name, "roots must not contain a reserved device name");
    }
    start = end + 1U;
  }
  return std::nullopt;
}

std::variant<std::wstring, PathError> classify_final_path(const std::wstring_view final_path) {
  if (final_path.substr(0, 8) == L"\\\\?\\UNC\\") {
    return detail::path_error(PathErrorCode::network_location,
                              "the folder resolves to a network share or a redirected folder (for example OneDrive "
                              "or a roaming profile); choose a local folder on this computer");
  }
  if (final_path.size() >= 7U && final_path.substr(0, 4) == L"\\\\?\\" && final_path[5] == L':') {
    return std::wstring{final_path.substr(4)};
  }
  return detail::path_error(PathErrorCode::unsupported_root_form, "the folder resolves to an unsupported location");
}
```

(the superscript-digit reserved names cannot appear in the ASCII projection, which maps them to `_`; the `is_windows_reserved_component` check on the UTF-8 relative grammar covers them below a root.)

`src/canonical_path_posix.cpp`:

```cpp
#if !defined(_WIN32)
#include "canonical_path_internal.hpp"

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace secure_frontend {

OwnedHandle::~OwnedHandle() {
  if (handle_ >= 0) ::close(handle_);
}

NativeHandle OwnedHandle::release() noexcept {
  const NativeHandle kept = handle_;
  handle_ = -1;
  return kept;
}

namespace detail {

std::variant<CanonicalRoot, PathError> open_existing_root(const std::filesystem::path& requested) {
  const std::string text = requested.string();
  if (text.empty() || text.find('\0') != std::string::npos) return path_error(PathErrorCode::empty, "the root path is empty");
  if (!requested.is_absolute()) return path_error(PathErrorCode::not_absolute, "roots must be absolute paths");
  char* resolved = ::realpath(text.c_str(), nullptr);
  if (resolved == nullptr) {
    return path_error(PathErrorCode::root_unavailable, "the root does not exist or cannot be resolved");
  }
  const std::string canonical{resolved};
  std::free(resolved);
#if defined(O_PATH)
  const int fd = ::open(canonical.c_str(), O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
#else
  const int fd = ::open(canonical.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
#endif
  if (fd < 0) {
    return path_error(errno == ENOTDIR ? PathErrorCode::not_directory : PathErrorCode::root_unavailable,
                      "the root could not be opened as a directory");
  }
  struct stat st {};
  if (::fstat(fd, &st) != 0 || !S_ISDIR(st.st_mode)) {
    ::close(fd);
    return path_error(PathErrorCode::not_directory, "the root is not a directory");
  }
  if (st.st_uid != ::geteuid() && st.st_uid != 0) {
    ::close(fd);
    return path_error(PathErrorCode::untrusted_owner, "the root must be owned by the current user or root");
  }
  if ((st.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
    ::close(fd);
    return path_error(PathErrorCode::shared_writable, "the root must not be writable by group or others");
  }
  return PathFactory::root(std::filesystem::path{canonical}, fd, static_cast<std::uint64_t>(st.st_dev));
}

}  // namespace detail

std::variant<CanonicalFile, PathError> open_executable(const std::filesystem::path& requested) {
  using detail::path_error;
  const std::string text = requested.string();
  if (text.empty() || !requested.is_absolute()) {
    return path_error(PathErrorCode::not_absolute, "the engine executable must be an absolute path");
  }
  char* resolved = ::realpath(text.c_str(), nullptr);
  if (resolved == nullptr) return path_error(PathErrorCode::not_found, "the engine executable does not exist");
  const std::string canonical{resolved};
  std::free(resolved);
  struct stat st {};
  if (::stat(canonical.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
    return path_error(PathErrorCode::not_regular_file, "the engine executable must be a regular file");
  }
  if (st.st_uid != ::geteuid() && st.st_uid != 0) {
    return path_error(PathErrorCode::untrusted_owner, "the engine executable must be owned by the current user or root");
  }
  if ((st.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
    return path_error(PathErrorCode::shared_writable, "the engine executable must not be writable by group or others");
  }
  const std::filesystem::path canonical_path{canonical};
  struct stat parent {};
  if (::stat(canonical_path.parent_path().c_str(), &parent) != 0 || (parent.st_mode & (S_IWGRP | S_IWOTH)) != 0 ||
      (parent.st_uid != ::geteuid() && parent.st_uid != 0)) {
    return path_error(PathErrorCode::shared_writable,
                      "the engine executable's directory must be private (owned by you or root, not group/other-writable)");
  }
  return detail::PathFactory::file(canonical_path);
}

bool is_within(const std::filesystem::path& root, const std::filesystem::path& candidate) noexcept {
  const std::filesystem::path base = root.has_filename() ? root : root.parent_path();
  auto r = base.begin();
  auto c = candidate.begin();
  for (; r != base.end(); ++r, ++c) {
    if (c == candidate.end() || *r != *c) return false;
  }
  return true;
}

}  // namespace secure_frontend
#endif
```

`src/windows_security.hpp`:

```cpp
#pragma once
#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"

#include <aclapi.h>
#include <sddl.h>

#include <array>
#include <cstddef>
#include <string>

namespace secure_frontend::windows_security {

bool current_user_sid(std::array<std::byte, SECURITY_MAX_SID_SIZE>& storage, PSID* result);
std::wstring current_user_sid_string();
// Owner is the user; DACL protected; allow-ACEs only for the user, SYSTEM, Administrators.
bool has_private_owner_dacl(HANDLE handle, PSID current_user);
enum class Trust { trusted, untrusted_owner, shared_writable, unreadable };
// Owner is the user/SYSTEM/Administrators and no effective allow-ACE grants a
// write right to anyone else (the Windows form of "not group/other-writable").
Trust write_access_is_private(HANDLE handle);
std::wstring final_path_for_handle(HANDLE handle);
bool is_reparse_point(HANDLE handle);
std::wstring extended_path(const std::wstring& absolute);

// O:<user>D:P(A;[OICI];FA;;;<user>)(A;[OICI];FA;;;SY): accepted by has_private_owner_dacl.
class PrivateSecurityAttributes final {
 public:
  explicit PrivateSecurityAttributes(bool directory);
  ~PrivateSecurityAttributes();
  PrivateSecurityAttributes(const PrivateSecurityAttributes&) = delete;
  PrivateSecurityAttributes& operator=(const PrivateSecurityAttributes&) = delete;
  [[nodiscard]] SECURITY_ATTRIBUTES* get() noexcept { return descriptor_ != nullptr ? &attributes_ : nullptr; }

 private:
  PSECURITY_DESCRIPTOR descriptor_{nullptr};
  SECURITY_ATTRIBUTES attributes_{};
};

}  // namespace secure_frontend::windows_security
#endif
```

`src/windows_security.cpp` — move `final_path_for_handle` (token_reader.cpp:202-214), `is_reparse_point` (:225-229), `sid_is_well_known` (:231-239), `is_permitted_acl_principal` (:241-244), `current_process_user_sid` (:246-268, renamed `current_user_sid`, taking `std::array&`), `LocalSecurityDescriptor` (:187-200) and `has_private_owner_dacl` (:274-312) verbatim into `namespace secure_frontend::windows_security` inside `#if defined(_WIN32)`, then add:

```cpp
std::wstring current_user_sid_string() {
  std::array<std::byte, SECURITY_MAX_SID_SIZE> storage{};
  PSID sid = nullptr;
  if (!current_user_sid(storage, &sid)) return {};
  LPWSTR text = nullptr;
  if (ConvertSidToStringSidW(sid, &text) == 0) return {};
  std::wstring out{text};
  LocalFree(text);
  return out;
}

Trust write_access_is_private(const HANDLE handle) {
  PSID owner = nullptr;
  PACL dacl = nullptr;
  PSECURITY_DESCRIPTOR raw = nullptr;
  if (GetSecurityInfo(handle, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr,
                      &dacl, nullptr, &raw) != ERROR_SUCCESS) {
    return Trust::unreadable;
  }
  LocalSecurityDescriptor guard(raw);
  std::array<std::byte, SECURITY_MAX_SID_SIZE> storage{};
  PSID user = nullptr;
  if (!current_user_sid(storage, &user)) return Trust::unreadable;
  if (owner == nullptr || !is_permitted_acl_principal(owner, user)) return Trust::untrusted_owner;
  if (dacl == nullptr) return Trust::shared_writable;  // a NULL DACL grants everyone full access
  constexpr ACCESS_MASK kWrite = FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES |
                                 FILE_DELETE_CHILD | DELETE | WRITE_DAC | WRITE_OWNER | GENERIC_WRITE | GENERIC_ALL;
  for (DWORD index = 0U; index < dacl->AceCount; ++index) {
    void* raw_ace = nullptr;
    if (GetAce(dacl, index, &raw_ace) == 0 || raw_ace == nullptr) return Trust::unreadable;
    const auto* header = static_cast<const ACE_HEADER*>(raw_ace);
    if ((header->AceFlags & INHERIT_ONLY_ACE) != 0U || header->AceType == ACCESS_DENIED_ACE_TYPE) continue;
    if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) return Trust::shared_writable;  // fail closed on other allow forms
    const auto* allowed = static_cast<const ACCESS_ALLOWED_ACE*>(raw_ace);
    PSID sid = reinterpret_cast<PSID>(const_cast<DWORD*>(&allowed->SidStart));
    if ((allowed->Mask & kWrite) != 0U && !is_permitted_acl_principal(sid, user)) return Trust::shared_writable;
  }
  return Trust::trusted;
}

std::wstring extended_path(const std::wstring& absolute) {
  return absolute.rfind(L"\\\\?\\", 0) == 0 ? absolute : L"\\\\?\\" + absolute;
}

PrivateSecurityAttributes::PrivateSecurityAttributes(const bool directory) {
  const std::wstring sid = current_user_sid_string();
  if (sid.empty()) return;
  const std::wstring inherit = directory ? L"OICI" : L"";
  const std::wstring sddl = L"O:" + sid + L"D:P(A;" + inherit + L";FA;;;" + sid + L")(A;" + inherit + L";FA;;;SY)";
  if (ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor_, nullptr) == 0) {
    descriptor_ = nullptr;
    return;
  }
  attributes_ = SECURITY_ATTRIBUTES{sizeof(SECURITY_ATTRIBUTES), descriptor_, FALSE};
}

PrivateSecurityAttributes::~PrivateSecurityAttributes() {
  if (descriptor_ != nullptr) LocalFree(descriptor_);
}
```

(`is_permitted_acl_principal(principal, owner)` compares against the passed SID, so passing the current user is the intended rule.) In `token_reader.cpp` keep `class WindowsHandle` (lines 170-185), delete lines 187-312 (now in `windows_security.cpp`), add `#include "windows_security.hpp"` to the Windows include block (lines 10-13) and `using namespace secure_frontend::windows_security;` at the top of the Windows section; the reader's call `current_process_user_sid(&current_user_storage, &current_user)` (line 338) becomes `current_user_sid(current_user_storage, &current_user)`. The reader keeps working unchanged until Task 11 replaces it.

`src/canonical_path_windows.cpp`:

```cpp
#if defined(_WIN32)
#include "canonical_path_internal.hpp"
#include "windows_security.hpp"

namespace secure_frontend {
namespace ws = windows_security;

OwnedHandle::~OwnedHandle() {
  if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
}

NativeHandle OwnedHandle::release() noexcept {
  const NativeHandle kept = handle_;
  handle_ = nullptr;
  return kept;
}

namespace detail {

std::variant<CanonicalRoot, PathError> open_existing_root(const std::filesystem::path& requested) {
  const std::wstring text = requested.wstring();
  if (auto error = check_windows_root_form(text)) return *error;
  std::wstring full(32768, L'\0');
  const DWORD length = GetFullPathNameW(text.c_str(), static_cast<DWORD>(full.size()), full.data(), nullptr);
  if (length == 0U || length >= full.size()) return path_error(PathErrorCode::root_unavailable, "the root cannot be resolved");
  full.resize(length);
  // No FILE_FLAG_OPEN_REPARSE_POINT: the root itself may be a junction or symlink.
  HANDLE handle = CreateFileW(ws::extended_path(full).c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                              FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return path_error(PathErrorCode::root_unavailable, "the root does not exist or cannot be opened");
  OwnedHandle guard(handle);
  FILE_ATTRIBUTE_TAG_INFO tag{};
  if (GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag, sizeof(tag)) == 0 ||
      (tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U) {
    return path_error(PathErrorCode::not_directory, "the root is not a directory");
  }
  auto classified = classify_final_path(ws::final_path_for_handle(handle));
  if (auto* error = std::get_if<PathError>(&classified)) return *error;
  switch (ws::write_access_is_private(handle)) {
    case ws::Trust::trusted: break;
    case ws::Trust::untrusted_owner:
      return path_error(PathErrorCode::untrusted_owner, "the root must be owned by you, SYSTEM or Administrators");
    case ws::Trust::shared_writable:
      return path_error(PathErrorCode::shared_writable, "the root must not be writable by other users");
    case ws::Trust::unreadable:
      return path_error(PathErrorCode::root_unavailable, "the root's security settings cannot be read");
  }
  // Ownership moves to the CanonicalRoot.
  return PathFactory::root(std::filesystem::path{std::get<std::wstring>(classified)}, guard.release(), 0U);
}

}  // namespace detail

std::variant<CanonicalFile, PathError> open_executable(const std::filesystem::path& requested) {
  using detail::path_error;
  const std::wstring text = requested.wstring();
  if (auto error = check_windows_root_form(text)) return *error;
  // \\?\ disables Win32 normalization ('/' separators, '.' and '..'), so the
  // path is made full and normalized first, exactly as open_existing_root does.
  std::wstring full(32768, L'\0');
  const DWORD length = GetFullPathNameW(text.c_str(), static_cast<DWORD>(full.size()), full.data(), nullptr);
  if (length == 0U || length >= full.size()) {
    return path_error(PathErrorCode::not_found, "the engine executable path cannot be resolved");
  }
  full.resize(length);
  HANDLE handle = CreateFileW(ws::extended_path(full).c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return path_error(PathErrorCode::not_found, "the engine executable does not exist");
  OwnedHandle guard(handle);
  FILE_ATTRIBUTE_TAG_INFO tag{};
  if (GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag, sizeof(tag)) == 0 ||
      (tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U || GetFileType(handle) != FILE_TYPE_DISK) {
    return path_error(PathErrorCode::not_regular_file, "the engine executable must be a regular file");
  }
  auto classified = classify_final_path(ws::final_path_for_handle(handle));
  if (auto* error = std::get_if<PathError>(&classified)) return *error;
  if (ws::write_access_is_private(handle) != ws::Trust::trusted) {
    return path_error(PathErrorCode::shared_writable, "the engine executable must not be writable by other users");
  }
  const std::filesystem::path canonical{std::get<std::wstring>(classified)};
  auto parent = detail::open_existing_root(canonical.parent_path());
  if (std::holds_alternative<PathError>(parent)) {
    return path_error(PathErrorCode::shared_writable, "the engine executable's folder must not be writable by other users");
  }
  return detail::PathFactory::file(canonical);
}

bool is_within(const std::filesystem::path& root, const std::filesystem::path& candidate) noexcept {
  std::wstring base = root.wstring();
  const std::wstring text = candidate.wstring();
  while (base.size() > 3U && (base.back() == L'\\' || base.back() == L'/')) base.pop_back();
  if (base.empty() || text.size() < base.size() ||
      CompareStringOrdinal(base.c_str(), static_cast<int>(base.size()), text.c_str(), static_cast<int>(base.size()), TRUE) !=
          CSTR_EQUAL) {
    return false;
  }
  return text.size() == base.size() || base.back() == L'\\' || text[base.size()] == L'\\' || text[base.size()] == L'/';
}

}  // namespace secure_frontend
#endif
```

`frontend_config.hpp`: add `#include "secure_frontend/canonical_path.hpp"`; `Roots` and `EngineLaunch` become:

```cpp
struct Roots final {
  CanonicalRoot models;
  CanonicalRoot tokenizer_pack;
  std::optional<CanonicalRoot> secret;
  std::optional<CanonicalRoot> logs;
  std::optional<CanonicalRoot> cache;
};
struct EngineLaunch final {
  EngineProfile profile;
  CanonicalFile executable;
  ModelId model_id;
  std::vector<std::string> args;
};
```

`frontend_config.cpp`: replace `absolute_root` with:

```cpp
std::optional<CanonicalRoot> canonical_root(DraftReader& read, ConfigErrors& errors, const std::string_view path) {
  const auto text = read.string(path);
  if (!text.has_value()) return std::nullopt;
  auto opened = open_root(path_from_utf8(*text));
  if (auto* error = std::get_if<PathError>(&opened)) {
    add(errors, std::string{path}, ConfigIssueCode::invalid_value, error->message);
    return std::nullopt;
  }
  return std::get<CanonicalRoot>(std::move(opened));
}
```

In `validate()`, replace the `// roots` block (the six lines from `const auto models = absolute_root(…)` to `const auto cache = absolute_root(…)`) with:

```cpp
  // roots: each is opened once, here, and held open by the FrontendConfig.
  const auto models = canonical_root(read, errors, "roots.models");
  const auto tokenizer = canonical_root(read, errors, "roots.tokenizer_pack");
  auto secret = canonical_root(read, errors, "roots.secret");
  if (!read.present("roots.secret") && draft.config_directory.has_value()) {
    auto opened = open_root(*draft.config_directory);
    if (auto* root = std::get_if<CanonicalRoot>(&opened)) {
      secret = std::move(*root);
    } else {
      add(errors, "roots.secret", ConfigIssueCode::invalid_value,
          "defaults to the config file's folder, which " + std::get<PathError>(opened).message);
    }
  }
  const auto logs = canonical_root(read, errors, "roots.logs");
  const auto cache = canonical_root(read, errors, "roots.cache");
```

replace `const auto executable = absolute_root(read, errors, "engine.executable");` with:

```cpp
  std::optional<CanonicalFile> executable;
  if (const auto text = read.string("engine.executable")) {
    auto opened = open_executable(path_from_utf8(*text));
    if (auto* error = std::get_if<PathError>(&opened)) {
      add(errors, "engine.executable", ConfigIssueCode::invalid_value, error->message);
    } else {
      executable = std::get<CanonicalFile>(std::move(opened));
    }
  }
```

The two construction lines near the end (`Roots roots{*models, *tokenizer, secret, logs, cache};` and `EngineLaunch engine{profile, *executable, std::move(*model_id), args};`) stay as written: the members are now `CanonicalRoot`/`CanonicalFile`, which copy by sharing the open handle.

`PathError::message` is fixed text and never contains the path (Task 8), so `typed_config_errors_never_contain_values` still holds; the `"defaults to the config file's folder, which "` prefix reads as one sentence with messages such as `the root must not be writable by group or others`.

In `describe`, replace the `path_or_unset` lambda and the six path lines with:

```cpp
  const auto root_text = [](const CanonicalRoot& root) { return quoted(path_to_utf8(root.path())); };
  const auto root_or_unset = [&root_text](const std::optional<CanonicalRoot>& root) {
    return root.has_value() ? root_text(*root) : std::string{"(unset)"};
  };
```

```cpp
  line("roots.models", root_text(c.roots.models));
  line("roots.tokenizer_pack", root_text(c.roots.tokenizer_pack));
  line("roots.secret", root_or_unset(c.roots.secret));
  line("roots.logs", root_or_unset(c.roots.logs));
  line("roots.cache", root_or_unset(c.roots.cache));
```

```cpp
  line("engine.executable", quoted(path_to_utf8(c.engine.executable.path())));
```

(the `line("token.source", …)`, `line("engine.profile", …)`, `line("engine.model_id", …)` and `engine.args` lines between and after them are unchanged).

`engine_arguments.hpp` line 42-45: the third parameter becomes `const CanonicalRoot& models_root` (`#include "secure_frontend/canonical_path.hpp"`); in `engine_arguments.cpp` the tail after the profile check becomes:

```cpp
  const auto parsed = parse_relative_path(selected.relative_artifact_path);
  const auto* relative = std::get_if<RelativePath>(&parsed);
  if (relative == nullptr) {
    return EngineArgumentsError{EngineArgumentsErrorCode::invalid_value, "selected model has an invalid artifact path"};
  }
  // The engine receives the canonical root the inventory used, never the raw
  // configured string (defect e).
  arguments.emplace_back("--native");
  arguments.push_back(models_root.join(*relative).string());
  return arguments;
```

`secure_server_main.cpp` — replace `read_configured_token` with:

```cpp
secure_frontend::TokenReadResult read_configured_token(const FrontendConfig& config) {
  return secure_frontend::read_token(config.token, config.roots.secret.has_value() ? config.roots.secret->path()
                                                                                   : std::filesystem::path{"."});
}
```

and in `run()` replace these three statements:

```cpp
  const auto tokenizer_result = secure_frontend::load_local_tokenizer_pack(config.roots.tokenizer_pack);
```
```cpp
  const auto models_result = secure_frontend::enumerate_local_models(config.roots.models);
```
```cpp
  const secure_frontend::EngineCommand command{.executable = config.engine.executable,
                                               .arguments = std::move(*engine_arguments), .environment = {}};
```

with:

```cpp
  const auto tokenizer_result = secure_frontend::load_local_tokenizer_pack(config.roots.tokenizer_pack.path());
```
```cpp
  const auto models_result = secure_frontend::enumerate_local_models(config.roots.models.path());
```
```cpp
  const secure_frontend::EngineCommand command{.executable = config.engine.executable.path(),
                                               .arguments = std::move(*engine_arguments), .environment = {}};
```

The `build_engine_arguments(arguments_profile, *selected, config.roots.models, config.engine.args)` call is unchanged text; `config.roots.models` is now the `CanonicalRoot` it takes.

CMake:

```cmake
target_sources(secure_frontend_paths PRIVATE src/canonical_path_posix.cpp src/canonical_path_windows.cpp src/windows_security.cpp)
target_link_libraries(secure_frontend_typed_config PUBLIC secure_frontend_paths)
target_link_libraries(secure_frontend_engine_arguments PUBLIC secure_frontend_paths)
target_link_libraries(secure_frontend_token_reader PUBLIC secure_frontend_paths)
```

and in the `if(WIN32)` link block add `target_link_libraries(secure_frontend_paths PUBLIC advapi32)`.

**wine decision (made here, not at run time).** From this task on, every case whose Windows run needs `open_root` or `open_executable` to *succeed* on a scratch directory depends on owner and DACL semantics that wine 6.0.3 does not model faithfully (Global Constraints), so it carries `LABELS ${SECURE_FRONTEND_WINDOWS_RUNTIME}` and is excluded under wine; the `windows-msys2-clang` job runs it. Pure string/grammar cases (`check_windows_root_form`, `classify_final_path`, `is_within`, the relative grammar) and cases that only expect a rejection before any handle is opened stay unlabelled. Concretely, change these existing registrations by appending `LABELS ${SECURE_FRONTEND_WINDOWS_RUNTIME}` before `TIMEOUT`/the closing parenthesis:

```cmake
    secure_frontend_add_cases(secure_frontend_engine_arguments_tests
        engine_receives_model_path_from_inventory engine_args_reject_owned_and_network_flags
        strata_profile_passes_pack_and_weight_flags_through rejects_invalid_relative_artifact_path
        glm_profile_passes_arguments_through_without_native
        LABELS ${SECURE_FRONTEND_WINDOWS_RUNTIME})
```

```cmake
    secure_frontend_add_cases(secure_frontend_typed_config_tests
        typed_config_accepts_minimal_file_and_applies_defaults cfg01_unknown_key_rejected cfg01_duplicate_key_rejected
        cfg01_type_confusion_rejected cfg01_missing_required_rejected cfg01_out_of_range_rejected
        typed_config_cross_field_rules typed_config_errors_never_contain_values
        LABELS ${SECURE_FRONTEND_WINDOWS_RUNTIME})
```

```cmake
    secure_frontend_add_cases(secure_frontend_subsystem_limits_tests
        ready_timeout_defaults_to_900_seconds subsystem_limits_follow_config
        LABELS ${SECURE_FRONTEND_WINDOWS_RUNTIME})
```

```cmake
    secure_frontend_add_cases(secure_frontend_server_cli_tests
        cli_config_file_and_flags_share_validator cli_flag_overrides_file_and_is_reported cli_check_prints_no_token
        cli_unknown_option_does_not_echo_value
        ${SECURE_FRONTEND_SERVER_ARGS} LABELS ${SECURE_FRONTEND_WINDOWS_RUNTIME} TIMEOUT 120)
```

```cmake
    secure_frontend_add_cases(secure_frontend_server_e2e_tests
        cfg01_invalid_config_fails_before_listener_and_child net02_remote_addresses_need_opt_in
        net02_error_is_non_secret_and_exits_before_bind ready_timeout_is_configurable
        ${SECURE_FRONTEND_SERVER_ARGS} LABELS ${SECURE_FRONTEND_WINDOWS_RUNTIME} TIMEOUT 120)
```

(`cli_unknown_option_does_not_echo_value` never reaches `validate()`, but it builds the binary's `ServerFixture` and spawns the server like the other three, so it shares their label.) Every later case added to one of these five binaries that builds a `ConfigFixture`/`ServerFixture` or opens a root carries the same label — Tasks 12, 14, 20, 21, 22 and 23 register theirs that way; the two cases that open nothing (Task 22's `cfg06_config_has_no_secret_typed_member`, Task 24's `readme_config_table_matches_schema`) stay unlabelled. The paths cases this task adds (`cfg02_rejects_relative_root`, `cfg02_sibling_prefix_root_rejected`, `cfg04_*`) stay unlabelled: they never open a handle on Windows.

- [ ] **Step 4: Run to verify it passes**

Run: `cmake --build --preset linux-gcc && ctest --preset linux-gcc | tail -3`
Expected: the new paths cases, `cfg03_engine_receives_canonical_models_root`, the updated engine-arguments cases, and every typed-config/subsystem-limits case (their fixtures already use real 0700 directories) pass; `0 tests failed`.
Run: `cmake --build --preset windows-llvm-mingw-cross | tail -1 && ctest --preset windows-llvm-mingw-cross | tail -3`
Expected: build ok with `-Werror`; `0 tests failed` under wine, with the engine-arguments, typed-config, subsystem-limits, server-CLI and server-e2e cases excluded by the label above (record the `ctest -N -L windows-runtime | tail -1` count in the commit body). They are authoritative in the MSYS2 job.

- [ ] **Step 5: Commit**

```bash
git -C "$(git rev-parse --show-toplevel)" add secure_frontend
git -C "$(git rev-parse --show-toplevel)" -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "paths: canonical roots and engine executable; engine gets the canonical models root (SP1 P2, P3, P7; defect e)" \
  -m "Regression tests: cfg03_engine_receives_canonical_models_root (failed before: --native carried the symlinked
root), cfg02_rejects_relative_root, cfg02_rejects_shared_writable_root, cfg02_root_may_traverse_symlink,
cfg02_sibling_prefix_root_rejected, cfg02_engine_executable_must_be_private_regular_file,
cfg04_rejects_unc_device_and_extended_roots, cfg04_final_path_on_network_share_is_actionable (Review Focus 2).
Windows: open_root/open_executable cross-compiled; wine <record>. Labelled windows-runtime from here (open_root
needs real DACLs): engine_arguments, typed_config, subsystem_limits, server_cli and server_e2e cases;
authoritative in windows-msys2-clang." \
  -m "$SP1_TRAILERS"
```

---

### Task 10: Linux handle walk below a root (`openat2` + fallback); token reader uses it (P4)

**Files:**
- Modify: `secure_frontend/include/secure_frontend/canonical_path.hpp` (`OpenKind`, `FileTrust`, `OpenOptions`, `WalkMode`, `OpenedFile`, `open_beneath`)
- Modify: `secure_frontend/src/canonical_path_internal.hpp` (`PathFactory::opened`), `secure_frontend/src/canonical_path_common.cpp` (`OpenedFile` move/destructor, `read_all`), `secure_frontend/src/canonical_path_posix.cpp` (walk; POSIX `close_now`/`read_some`), `secure_frontend/src/canonical_path_windows.cpp` (Windows `close_now`/`read_some`, so the common file links on Windows)
- Modify: `secure_frontend/include/secure_frontend/token_reader.hpp` (62 lines at HEAD 7ddcd755: enum at lines 11-24 gains `hard_link_not_allowed`; the Linux paragraph of the comment, lines 36-39, is rewritten; the `CanonicalRoot` overload and `token_error_from` join the declaration at lines 59-60), `secure_frontend/src/token_reader.cpp:50-167` (Linux reader replaced), `:424-445`
- Modify: `secure_frontend/tests/paths_test.cpp`, `secure_frontend/tests/token_reader_test.cpp:128-137` (expected code), new case
- Modify: `secure_frontend/src/secure_server_main.cpp` (`read_token` with the `CanonicalRoot`)

**Interfaces:**
- Consumes: `CanonicalRoot`, `PathFactory`, `path_error` (Task 9).
- Produces (Tasks 11–18):
```cpp
namespace secure_frontend {
enum class OpenKind { regular_file, directory };
enum class FileTrust { any, not_shared_writable, owner_only };
struct OpenOptions final { OpenKind kind{OpenKind::regular_file}; FileTrust trust{FileTrust::any}; bool reject_hard_links{false}; };
enum class WalkMode { automatic, fallback_only };   // fallback_only: tests force the openat(O_NOFOLLOW) walk
class OpenedFile final {   // move-only
 public:
  [[nodiscard]] NativeHandle native() const noexcept;
  [[nodiscard]] std::uint64_t size() const noexcept;
  [[nodiscard]] std::variant<std::size_t, PathError> read_some(char* buffer, std::size_t capacity) const;  // 0 = EOF
  [[nodiscard]] std::variant<std::string, PathError> read_all(std::size_t max_bytes) const;
};
[[nodiscard]] std::variant<OpenedFile, PathError> open_beneath(const CanonicalRoot& root, const RelativePath& path,
                                                               const OpenOptions& options = {}, WalkMode mode = WalkMode::automatic);
// token_reader.hpp
enum class TokenReadErrorCode { /* existing */ hard_link_not_allowed };
[[nodiscard]] TokenReadResult read_token(const SecretSource& source, const CanonicalRoot& protected_file_root);
[[nodiscard]] TokenReadResult read_token(const SecretSource& source, const std::filesystem::path& protected_file_root);
[[nodiscard]] TokenReadError token_error_from(const PathError& error);
}
```
  `FileTrust::owner_only`: Linux — the final file is owned by the effective user and grants no group/other bits; Windows (Task 11) — the root, every component and the file carry a private owner DACL. `FileTrust::not_shared_writable`: owner is the user or root (SYSTEM/Administrators) and no one else may write.

- [ ] **Step 1: Write the failing tests**

`tests/token_reader_test.cpp` — change the expectation of `rejects_a_symlink_that_escapes_the_root` (lines 134-136) to `secure_frontend::TokenReadErrorCode::symlink_not_allowed` with message `"a symlinked token file is rejected, never followed"`, and add:

```cpp
void cfg03_rejects_hardlinked_token_file() {
  TemporaryDirectory directory;
  writes_file(directory.path() / "token", "linked-token", 0600);
  if (link((directory.path() / "token").c_str(), (directory.path() / "second-name").c_str()) != 0) {
    std::cerr << "link failed\n";
    std::exit(EXIT_FAILURE);
  }
  const auto result = read_file_source(directory.path(), "token");
  const auto* error = std::get_if<secure_frontend::TokenReadError>(&result);
  expect(error != nullptr && error->code == secure_frontend::TokenReadErrorCode::hard_link_not_allowed,
         "a token file with a second hard link must be rejected");
}
```

registered as `cfg03_rejects_hardlinked_token_file` (run_cases and CMake).

`tests/paths_test.cpp` — Linux block (`#if !defined(_WIN32)`; add `#include <sched.h>`, `#include <sys/mount.h>`, `#include <sys/wait.h>`, `#include <fstream>`):

```cpp
secure_frontend::CanonicalRoot root_at(const std::filesystem::path& path) {
  return std::get<secure_frontend::CanonicalRoot>(secure_frontend::open_root(path));
}
secure_frontend::RelativePath rel(const std::string& text) {
  return std::get<secure_frontend::RelativePath>(secure_frontend::parse_relative_path(text));
}
constexpr secure_frontend::WalkMode kModes[] = {secure_frontend::WalkMode::automatic, secure_frontend::WalkMode::fallback_only};

void cfg03_rejects_symlinked_intermediate_directory() {
  const auto root = secure_frontend_test::make_scratch_directory();
  const auto outside = secure_frontend_test::make_scratch_directory();
  secure_frontend_test::make_private_directory(outside / "sub");
  secure_frontend_test::write_file(outside / "sub" / "f", "outside");
  std::filesystem::create_directory_symlink(outside / "sub", root / "sub");
  for (const auto mode : kModes) {
    expect(fails_with(secure_frontend::open_beneath(root_at(root), rel("sub/f"), {}, mode), PathErrorCode::symlink_not_allowed),
           "a symlinked intermediate directory is rejected in both walk modes");
  }
}

void cfg03_rejects_symlinked_final_file() {
  const auto root = secure_frontend_test::make_scratch_directory();
  const auto outside = secure_frontend_test::make_scratch_directory();
  secure_frontend_test::write_file(outside / "f", "outside");
  secure_frontend_test::write_file(root / "real", "inside");
  std::filesystem::create_symlink(outside / "f", root / "escape");
  std::filesystem::create_symlink(root / "real", root / "inside-link");
  for (const auto mode : kModes) {
    expect(fails_with(secure_frontend::open_beneath(root_at(root), rel("escape"), {}, mode), PathErrorCode::symlink_not_allowed),
           "a symlink to a file outside the root is rejected");
    expect(fails_with(secure_frontend::open_beneath(root_at(root), rel("inside-link"), {}, mode), PathErrorCode::symlink_not_allowed),
           "a symlink is rejected even when its target is inside the root");
  }
}

void cfg03_fallback_walk_matches_openat2() {
  const auto root = secure_frontend_test::make_scratch_directory();
  secure_frontend_test::make_private_directory(root / "dir");
  secure_frontend_test::write_file(root / "dir" / "file", "payload");
  std::filesystem::create_symlink(root / "dir" / "file", root / "link");
  mkfifo((root / "fifo").c_str(), 0600);
  struct Row { const char* path; std::optional<PathErrorCode> code; };
  const Row rows[] = {{"dir/file", std::nullopt}, {"dir", PathErrorCode::not_regular_file},
                      {"missing", PathErrorCode::not_found}, {"link", PathErrorCode::symlink_not_allowed},
                      {"fifo", PathErrorCode::not_regular_file}, {"dir/file/x", PathErrorCode::not_directory}};
  for (const auto& row : rows) {
    for (const auto mode : kModes) {
      const auto result = secure_frontend::open_beneath(root_at(root), rel(row.path), {}, mode);
      if (!row.code.has_value()) {
        const auto* opened = std::get_if<secure_frontend::OpenedFile>(&result);
        expect(opened != nullptr, std::string{"opens: "} + row.path);
        if (opened != nullptr) {
          const auto text = opened->read_all(64);
          expect(std::holds_alternative<std::string>(text) && std::get<std::string>(text) == "payload", "reads the file");
        }
      } else {
        expect(fails_with(result, *row.code), std::string{"same outcome in both modes for "} + row.path);
      }
    }
  }
}

// Mounts a tmpfs below the root inside a private user+mount namespace; skips
// (exit 77 -> "SKIPPED:") where unprivileged namespaces are not allowed.
void cfg03_rejects_mount_crossing() {
  const auto root = secure_frontend_test::make_scratch_directory();
  secure_frontend_test::make_private_directory(root / "mnt");
  const uid_t uid = geteuid();
  const gid_t gid = getegid();
  const pid_t child = fork();
  if (child == 0) {
    if (unshare(CLONE_NEWUSER | CLONE_NEWNS) != 0) _exit(77);
    std::ofstream("/proc/self/setgroups") << "deny";
    std::ofstream("/proc/self/uid_map") << uid << ' ' << uid << " 1";
    std::ofstream("/proc/self/gid_map") << gid << ' ' << gid << " 1";
    if (mount("none", (root / "mnt").c_str(), "tmpfs", 0, "mode=0700") != 0) _exit(77);
    std::ofstream(root / "mnt" / "f") << "x";
    for (const auto mode : kModes) {
      if (!fails_with(secure_frontend::open_beneath(root_at(root), rel("mnt/f"), {}, mode), PathErrorCode::cross_device)) _exit(1);
    }
    _exit(0);
  }
  int status = 0;
  waitpid(child, &status, 0);
  if (WIFEXITED(status) && WEXITSTATUS(status) == 77) {
    std::cout << "SKIPPED: no unprivileged mount namespace on this host\n";
    return;
  }
  expect(WIFEXITED(status) && WEXITSTATUS(status) == 0, "a mount below the root is a cross-device walk in both modes");
}
```

Registered under `#if !defined(_WIN32)` in `run_cases` and in CMake:

```cmake
    if(NOT WIN32)
        secure_frontend_add_cases(secure_frontend_paths_tests
            cfg03_rejects_symlinked_intermediate_directory cfg03_rejects_symlinked_final_file
            cfg03_fallback_walk_matches_openat2 cfg03_rejects_mount_crossing)
        set_tests_properties(secure_frontend_paths_tests.cfg03_rejects_mount_crossing PROPERTIES
            SKIP_REGULAR_EXPRESSION "SKIPPED:")
    endif()
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build --preset linux-gcc 2>&1 | grep -m1 error`
Expected: `error: 'open_beneath' is not a member of 'secure_frontend'` (paths tests).
Run: `cmake --build --preset linux-gcc --target secure_frontend_token_reader_tests 2>&1 | grep -m1 error`
Expected: `error: 'hard_link_not_allowed' is not a member of 'secure_frontend::TokenReadErrorCode'`. (Today's reader also follows the hard link and reports `path_escapes_root`, not `symlink_not_allowed`, for the escaping symlink — the two behaviors the cases pin.)

- [ ] **Step 3: Implement**

`canonical_path.hpp` — add after `classify_final_path`:

```cpp
enum class OpenKind { regular_file, directory };
enum class FileTrust { any, not_shared_writable, owner_only };
struct OpenOptions final {
  OpenKind kind{OpenKind::regular_file};
  FileTrust trust{FileTrust::any};
  bool reject_hard_links{false};
};
enum class WalkMode { automatic, fallback_only };

class OpenedFile final {
 public:
  OpenedFile(OpenedFile&& other) noexcept : handle_(other.handle_), size_(other.size_), owns_(other.owns_) { other.owns_ = false; }
  OpenedFile& operator=(OpenedFile&& other) noexcept;
  OpenedFile(const OpenedFile&) = delete;
  OpenedFile& operator=(const OpenedFile&) = delete;
  ~OpenedFile();
  [[nodiscard]] NativeHandle native() const noexcept { return handle_; }
  [[nodiscard]] std::uint64_t size() const noexcept { return size_; }
  // Reads at most `capacity` bytes at the current position; 0 means end of file.
  [[nodiscard]] std::variant<std::size_t, PathError> read_some(char* buffer, std::size_t capacity) const;
  // The whole (regular) file, or too_large if it exceeds max_bytes.
  [[nodiscard]] std::variant<std::string, PathError> read_all(std::size_t max_bytes) const;

 private:
  friend struct detail::PathFactory;
  OpenedFile(const NativeHandle handle, const std::uint64_t size) noexcept : handle_(handle), size_(size), owns_(true) {}
  void close_now() noexcept;
  NativeHandle handle_;
  std::uint64_t size_;
  bool owns_;
};

// Opens `path` below `root` without following any link: Linux uses openat2
// (RESOLVE_BENEATH | NO_SYMLINKS | NO_MAGICLINKS | NO_XDEV) when the kernel has
// it, else a component-wise openat(O_NOFOLLOW) walk with st_dev checks;
// Windows opens each component with FILE_FLAG_OPEN_REPARSE_POINT and checks
// its reparse tag and final-path containment (Task 11).
[[nodiscard]] std::variant<OpenedFile, PathError> open_beneath(const CanonicalRoot& root, const RelativePath& path,
                                                               const OpenOptions& options = {},
                                                               WalkMode mode = WalkMode::automatic);
```

`canonical_path_internal.hpp` — in `PathFactory` add
`static OpenedFile opened(const NativeHandle handle, const std::uint64_t size) { return OpenedFile{handle, size}; }`.

`canonical_path_common.cpp`:

```cpp
OpenedFile& OpenedFile::operator=(OpenedFile&& other) noexcept {
  if (this != &other) {
    close_now();
    handle_ = other.handle_;
    size_ = other.size_;
    owns_ = other.owns_;
    other.owns_ = false;
  }
  return *this;
}

OpenedFile::~OpenedFile() { close_now(); }

std::variant<std::string, PathError> OpenedFile::read_all(const std::size_t max_bytes) const {
  std::string out;
  std::array<char, 65536> chunk{};
  while (true) {
    auto got = read_some(chunk.data(), chunk.size());
    if (auto* error = std::get_if<PathError>(&got)) return *error;
    const std::size_t count = std::get<std::size_t>(got);
    if (count == 0U) return out;
    if (count > max_bytes - out.size()) return detail::path_error(PathErrorCode::too_large, "the file is larger than its limit");
    out.append(chunk.data(), count);
  }
}
```

(`#include <array>`.) These three members call `close_now()` and `read_some()`, which each platform file defines; both definitions land in this task so the common file links on Linux and on Windows.

`canonical_path_windows.cpp` — add `#include <algorithm>` and, after `OwnedHandle::release`:

```cpp
void OpenedFile::close_now() noexcept {
  if (owns_ && handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
  owns_ = false;
}

std::variant<std::size_t, PathError> OpenedFile::read_some(char* const buffer, const std::size_t capacity) const {
  DWORD count = 0;
  const DWORD request = static_cast<DWORD>(std::min<std::size_t>(capacity, 1U << 20U));
  if (ReadFile(handle_, buffer, request, &count, nullptr) == 0) {
    return detail::path_error(PathErrorCode::read_failed, "the file could not be read");
  }
  return static_cast<std::size_t>(count);
}
```

(`open_beneath` itself has no Windows definition until Task 11; nothing on Windows calls it before then.)

`canonical_path_posix.cpp` — add includes `<sys/syscall.h>` (Linux) and:

```cpp
#if defined(__linux__)
#include <sys/syscall.h>
#if __has_include(<linux/openat2.h>)
#include <linux/openat2.h>
#else
struct open_how {
  std::uint64_t flags;
  std::uint64_t mode;
  std::uint64_t resolve;
};
#define RESOLVE_NO_XDEV 0x01
#define RESOLVE_NO_MAGICLINKS 0x02
#define RESOLVE_NO_SYMLINKS 0x04
#define RESOLVE_BENEATH 0x08
#endif
#ifndef SYS_openat2
#define SYS_openat2 437
#endif
#endif
```

and in `namespace secure_frontend` (after `OwnedHandle::~OwnedHandle`):

```cpp
void OpenedFile::close_now() noexcept {
  if (owns_ && handle_ >= 0) ::close(handle_);
  owns_ = false;
}

std::variant<std::size_t, PathError> OpenedFile::read_some(char* const buffer, const std::size_t capacity) const {
  while (true) {
    const ssize_t count = ::read(handle_, buffer, capacity);
    if (count >= 0) return static_cast<std::size_t>(count);
    if (errno != EINTR) return detail::path_error(PathErrorCode::read_failed, "the file could not be read");
  }
}

namespace {
using detail::path_error;

int final_flags(const OpenKind kind) {
  return kind == OpenKind::directory ? (O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
                                     // O_NONBLOCK: a FIFO cannot block before fstat rejects it.
                                     : (O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
}

PathError from_errno(const int error) {
  switch (error) {
    case ELOOP: return path_error(PathErrorCode::symlink_not_allowed, "a path component is a symbolic link");
    case EXDEV: return path_error(PathErrorCode::cross_device, "the path crosses into another file system");
    case ENOENT: return path_error(PathErrorCode::not_found, "the path does not exist");
    case ENOTDIR: return path_error(PathErrorCode::not_directory, "a path component is not a directory");
    default: return path_error(PathErrorCode::open_failed, "the path could not be opened");
  }
}

int try_openat2(const CanonicalRoot& root, const RelativePath& path, const int flags) {
#if defined(__linux__)
  open_how how{};
  how.flags = static_cast<std::uint64_t>(flags);
  how.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS | RESOLVE_NO_XDEV;
  return static_cast<int>(::syscall(SYS_openat2, root.handle(), path.generic().c_str(), &how, sizeof(how)));
#else
  (void)root;
  (void)path;
  (void)flags;
  errno = ENOSYS;
  return -1;
#endif
}

std::variant<int, PathError> walk_components(const CanonicalRoot& root, const RelativePath& path, const int flags) {
  int current = -1;
  const auto& parts = path.components();
  for (std::size_t i = 0; i < parts.size(); ++i) {
    const bool last = i + 1U == parts.size();
    const int base = current < 0 ? root.handle() : current;
    const int next = ::openat(base, parts[i].c_str(), last ? flags : (O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    const int saved = errno;
    if (next < 0) {
      PathError error = from_errno(saved);
      struct stat entry {};
      if (saved == ENOTDIR && ::fstatat(base, parts[i].c_str(), &entry, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(entry.st_mode)) {
        error = path_error(PathErrorCode::symlink_not_allowed, "a path component is a symbolic link");
      }
      if (current >= 0) ::close(current);
      return error;
    }
    if (current >= 0) ::close(current);
    struct stat st {};
    if (::fstat(next, &st) != 0 || static_cast<std::uint64_t>(st.st_dev) != root.device_id()) {
      ::close(next);
      return path_error(PathErrorCode::cross_device, "the path crosses into another file system");
    }
    current = next;
  }
  return current;
}
}  // namespace

std::variant<OpenedFile, PathError> open_beneath(const CanonicalRoot& root, const RelativePath& path,
                                                 const OpenOptions& options, const WalkMode mode) {
  const int flags = final_flags(options.kind);
  int fd = -1;
  if (mode == WalkMode::automatic) {
    fd = try_openat2(root, path, flags);
    if (fd < 0 && errno != ENOSYS && errno != EPERM) return from_errno(errno);
  }
  if (fd < 0) {
    auto walked = walk_components(root, path, flags);
    if (auto* error = std::get_if<PathError>(&walked)) return *error;
    fd = std::get<int>(walked);
  }
  OwnedHandle guard(fd);
  struct stat st {};
  if (::fstat(fd, &st) != 0) return path_error(PathErrorCode::open_failed, "the opened path could not be inspected");
  if (options.kind == OpenKind::directory && !S_ISDIR(st.st_mode)) {
    return path_error(PathErrorCode::not_directory, "the path is not a directory");
  }
  if (options.kind == OpenKind::regular_file && !S_ISREG(st.st_mode)) {
    return path_error(PathErrorCode::not_regular_file, "the path is not a regular file");
  }
  if (static_cast<std::uint64_t>(st.st_dev) != root.device_id()) {
    return path_error(PathErrorCode::cross_device, "the path crosses into another file system");
  }
  if (options.reject_hard_links && st.st_nlink > 1) {
    return path_error(PathErrorCode::hard_link_not_allowed, "the file has more than one hard link");
  }
  if (options.trust == FileTrust::owner_only) {
    if (st.st_uid != ::geteuid()) return path_error(PathErrorCode::not_owned_by_user, "the file must be owned by the current user");
    if ((st.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
      return path_error(PathErrorCode::permissions_too_broad, "the file must not grant group or other access");
    }
  } else if (options.trust == FileTrust::not_shared_writable) {
    if (st.st_uid != ::geteuid() && st.st_uid != 0) {
      return path_error(PathErrorCode::untrusted_owner, "the file must be owned by the current user or root");
    }
    if ((st.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
      return path_error(PathErrorCode::shared_writable, "the file must not be writable by group or others");
    }
  }
  return detail::PathFactory::opened(guard.release(), static_cast<std::uint64_t>(st.st_size));
}
```

`token_reader.hpp`: add `#include "secure_frontend/canonical_path.hpp"` after `#include "secure_frontend/secret_source.hpp"`; add `hard_link_not_allowed,` after `token_too_large,` in `TokenReadErrorCode` (line 23); replace the Linux sentences of the comment (lines 36-39) with:

```cpp
// Resolves a SecretSource at the last responsible moment. For a protected file
// the root is opened once (open_root) and the file is opened below it with
// open_beneath (no symlink, no mount crossing, no second hard link); on Linux
// it must then be owned by the effective user with no group/other bits.
```

and replace the declaration at lines 59-60 with:

```cpp
[[nodiscard]] TokenReadResult read_token(const SecretSource& source, const CanonicalRoot& protected_file_root);
// Opens `protected_file_root` with open_root first; used where only a path is known.
[[nodiscard]] TokenReadResult read_token(const SecretSource& source,
                                         const std::filesystem::path& protected_file_root);
// Maps a canonical-path failure to the token reader's fixed codes and messages.
[[nodiscard]] TokenReadError token_error_from(const PathError& error);
```

`token_reader.cpp`: delete lines 36-48 (`source_relative_path`) and 50-167 (`is_below_or_equal`, `read_protected_file_linux`); add in the anonymous namespace (guarded until Task 11 gives Windows an `open_beneath`, so the Windows build neither warns about an unused function nor references an undefined one):

```cpp
#ifndef _WIN32
TokenReadResult read_protected_file(const SecretSource& source, const CanonicalRoot& root) {
  const auto parsed = parse_relative_path(source.location());
  const auto* relative = std::get_if<RelativePath>(&parsed);
  if (relative == nullptr) return error(TokenReadErrorCode::read_failed, "The protected token file path is not valid.");
  auto opened = open_beneath(root, *relative,
                             OpenOptions{.kind = OpenKind::regular_file, .trust = FileTrust::owner_only,
                                         .reject_hard_links = true});
  if (const auto* failure = std::get_if<PathError>(&opened)) return token_error_from(*failure);
  const auto& file = std::get<OpenedFile>(opened);
  if (file.size() > kMaximumTokenBytes) return error(TokenReadErrorCode::token_too_large, "The protected token exceeds the size limit.");
  auto text = file.read_all(kMaximumTokenBytes);
  if (std::holds_alternative<PathError>(text)) return error(TokenReadErrorCode::read_failed, "The protected token file could not be read.");
  std::string token = std::get<std::string>(std::move(text));
  if (token.empty()) return error(TokenReadErrorCode::empty_token, "The resolved token must not be empty.");
  if (contains_control_character(token)) {
    return error(TokenReadErrorCode::invalid_token, "The resolved token must not contain control characters.");
  }
  return token;
}
#endif  // _WIN32 (removed in Task 11)
```

and after the anonymous namespace:

```cpp
TokenReadError token_error_from(const PathError& failure) {
  switch (failure.code) {
    case PathErrorCode::symlink_not_allowed: return error(TokenReadErrorCode::symlink_not_allowed, "Protected token paths may not include links or reparse points.");
    case PathErrorCode::escapes_root:
    case PathErrorCode::cross_device: return error(TokenReadErrorCode::path_escapes_root, "The protected token file resolves outside the configured root.");
    case PathErrorCode::not_found: return error(TokenReadErrorCode::token_not_found, "The protected token file does not exist.");
    case PathErrorCode::not_directory:
    case PathErrorCode::not_regular_file: return error(TokenReadErrorCode::not_regular_file, "The protected token path must be a regular file.");
    case PathErrorCode::not_owned_by_user: return error(TokenReadErrorCode::file_not_owned_by_current_user, "The protected token file must be owned by the current user.");
    case PathErrorCode::permissions_too_broad:
    case PathErrorCode::untrusted_owner:
    case PathErrorCode::shared_writable: return error(TokenReadErrorCode::insecure_file_permissions, "The protected token file must not grant access to other users.");
    case PathErrorCode::hard_link_not_allowed: return error(TokenReadErrorCode::hard_link_not_allowed, "The protected token file must not have other hard links.");
    case PathErrorCode::not_absolute:
    case PathErrorCode::unsupported_root_form:
    case PathErrorCode::network_location:
    case PathErrorCode::root_unavailable: return error(TokenReadErrorCode::secret_root_unavailable, "The configured protected-file root is unavailable.");
    default: return error(TokenReadErrorCode::read_failed, "The protected token file could not be opened safely.");
  }
}

TokenReadResult read_token(const SecretSource& source, const CanonicalRoot& protected_file_root) {
  if (source.kind() == SecretSourceKind::environment_variable) return read_environment_token(source);
#ifdef _WIN32
  return read_protected_file_windows(source, protected_file_root.path());  // Task 11 switches to the module
#else
  return read_protected_file(source, protected_file_root);
#endif
}

TokenReadResult read_token(const SecretSource& source, const std::filesystem::path& protected_file_root) {
  if (source.kind() == SecretSourceKind::environment_variable) return read_environment_token(source);
#ifdef _WIN32
  return read_protected_file_windows(source, protected_file_root);
#else
  auto root = open_root(protected_file_root);
  if (const auto* failure = std::get_if<PathError>(&root)) return token_error_from(*failure);
  return read_protected_file(source, std::get<CanonicalRoot>(root));
#endif
}
```

where `read_environment_token(source)` is the body of today's environment branch (lines 425-438) moved into the anonymous namespace unchanged. Keep `read_protected_file_windows` and its helpers under `#else` of the old `#ifndef _WIN32` until Task 11. The only Linux-only function, `read_protected_file`, is inside `#ifndef _WIN32` (above), so no `[[maybe_unused]]` is needed on either platform.

`secure_server_main.cpp`: `read_configured_token` becomes

```cpp
secure_frontend::TokenReadResult read_configured_token(const FrontendConfig& config) {
  if (config.roots.secret.has_value()) return secure_frontend::read_token(config.token, *config.roots.secret);
  return secure_frontend::read_token(config.token, std::filesystem::path{"."});
}
```

- [ ] **Step 4: Run to verify it passes**

Run: `cmake --build --preset linux-gcc && ctest --preset linux-gcc -R 'paths_tests|token_reader' --output-on-failure | tail -5 && ctest --preset linux-gcc | tail -1`
Expected: every new case passes (`cfg03_rejects_mount_crossing` passes on this host — kernel 5.15 with unprivileged user namespaces — or reports `Skipped`); `0 tests failed`.
Run: `strace -f -e trace=openat2 out/build/linux-gcc/secure_frontend_paths_tests --case cfg03_fallback_walk_matches_openat2 2>&1 | grep -c 'openat2('` if `strace` is installed (optional evidence that the automatic mode really calls `openat2`); record the count in the commit body, or "strace not installed".
Run: `cmake --build --preset windows-llvm-mingw-cross | tail -1 && ctest --preset windows-llvm-mingw-cross | tail -1` → build and link ok with `-Werror` (`OpenedFile`'s members resolve to the Windows `close_now`/`read_some` above; the Windows reader is unchanged until Task 11); `0 tests failed`.

- [ ] **Step 5: Commit**

```bash
git -C "$(git rev-parse --show-toplevel)" add secure_frontend
git -C "$(git rev-parse --show-toplevel)" -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "paths: openat2 walk below a root with an openat(O_NOFOLLOW) fallback; Linux token reader uses it (SP1 P4)" \
  -m "Regression tests (failed before): cfg03_rejects_hardlinked_token_file; rejects_a_symlink_that_escapes_the_root
now reports symlink_not_allowed. New: cfg03_rejects_symlinked_intermediate_directory, cfg03_rejects_symlinked_final_file,
cfg03_fallback_walk_matches_openat2, cfg03_rejects_mount_crossing (<passed|skipped>)." \
  -m "$SP1_TRAILERS"
```

---

### Task 11: Windows walk with `READ_CONTROL` and reparse rejection; Windows token reader uses it (P5; defect b)

Windows-only behavior. RED/GREEN evidence: cross-compile with `-Werror`, run under wine and record the result, authoritative in the `windows-msys2-clang` job (label `windows-runtime`).

**Files:**
- Modify: `secure_frontend/src/canonical_path_windows.cpp` (`open_beneath`; `OpenedFile::close_now`/`read_some` are already there from Task 10)
- Modify: `secure_frontend/src/token_reader.cpp` (delete `read_protected_file_windows` and `WindowsHandle`; `read_token` uses the portable `read_protected_file` on every platform)
- Modify: `secure_frontend/tests/support/test_support.hpp` (Windows helpers), `secure_frontend/tests/paths_test.cpp`, `secure_frontend/CMakeLists.txt` (paths tests link the token reader)

**Interfaces:**
- Consumes: `windows_security::*`, `open_beneath` declaration, `OpenOptions`, `token_error_from` (Tasks 9, 10).
- Produces (test support, Windows; Tasks 12, 13, 17, 18):
```cpp
namespace secure_frontend_test {
std::wstring current_user_sid_string();
void make_protected_directory(const std::filesystem::path& path);                     // O:<user>D:P(A;OICI;FA;;;<user>)(A;OICI;FA;;;SY)
void write_protected_file(const std::filesystem::path& path, std::string_view bytes);   // O:<user>D:P(A;;FA;;;<user>)(A;;FA;;;SY)
bool create_junction(const std::filesystem::path& link, const std::filesystem::path& target);  // FSCTL_SET_REPARSE_POINT
}
```

- [ ] **Step 1: Write the failing tests**

`tests/support/test_support.hpp` — add inside the namespace, under `#if defined(_WIN32)` (and `#include <winioctl.h>`, `#include <cstring>`, `#include <cstdint>` in the Windows include block; `<sddl.h>` is already there since Task 2):

```cpp
inline std::wstring current_user_sid_string() {
  HANDLE token = nullptr;
  if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == 0) return {};
  DWORD size = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &size);
  std::vector<unsigned char> buffer(size);
  const BOOL ok = GetTokenInformation(token, TokenUser, buffer.data(), size, &size);
  CloseHandle(token);
  if (ok == 0) return {};
  LPWSTR text = nullptr;
  if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &text) == 0) return {};
  std::wstring out{text};
  LocalFree(text);
  return out;
}

struct ProtectedAttributes final {
  PSECURITY_DESCRIPTOR descriptor{nullptr};
  SECURITY_ATTRIBUTES attributes{};
  explicit ProtectedAttributes(const bool directory) {
    const std::wstring sid = current_user_sid_string();
    const std::wstring inherit = directory ? L"OICI" : L"";
    const std::wstring sddl = L"O:" + sid + L"D:P(A;" + inherit + L";FA;;;" + sid + L")(A;" + inherit + L";FA;;;SY)";
    ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr);
    attributes = SECURITY_ATTRIBUTES{sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
  }
  ~ProtectedAttributes() { if (descriptor != nullptr) LocalFree(descriptor); }
};

inline void make_protected_directory(const std::filesystem::path& path) {
  ProtectedAttributes sa(true);
  const std::wstring extended = L"\\\\?\\" + std::filesystem::absolute(path).wstring();
  if (CreateDirectoryW(extended.c_str(), &sa.attributes) == 0) { std::cerr << "protected mkdir failed\n"; std::exit(EXIT_FAILURE); }
}

inline void write_protected_file(const std::filesystem::path& path, const std::string_view bytes) {
  ProtectedAttributes sa(false);
  const std::wstring extended = L"\\\\?\\" + std::filesystem::absolute(path).wstring();
  HANDLE file = CreateFileW(extended.c_str(), GENERIC_WRITE, 0, &sa.attributes, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) { std::cerr << "protected file create failed\n"; std::exit(EXIT_FAILURE); }
  DWORD written = 0;
  if (!bytes.empty()) WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
  CloseHandle(file);
}

// A directory junction (mount point) at `link` pointing at `target`; needs no privilege.
inline bool create_junction(const std::filesystem::path& link, const std::filesystem::path& target) {
  if (CreateDirectoryW(link.c_str(), nullptr) == 0) return false;
  HANDLE directory = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                 FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (directory == INVALID_HANDLE_VALUE) return false;
  const std::wstring print = std::filesystem::absolute(target).wstring();
  const std::wstring substitute = L"\\??\\" + print;
  const std::size_t path_bytes = (substitute.size() + 1U + print.size() + 1U) * sizeof(wchar_t);
  std::vector<unsigned char> buffer(16U + path_bytes, 0U);
  const auto put16 = [&buffer](const std::size_t offset, const std::uint16_t value) { std::memcpy(buffer.data() + offset, &value, 2U); };
  const DWORD tag = IO_REPARSE_TAG_MOUNT_POINT;
  std::memcpy(buffer.data(), &tag, 4U);
  put16(4U, static_cast<std::uint16_t>(8U + path_bytes));                        // ReparseDataLength
  put16(8U, 0U);                                                                // SubstituteNameOffset
  put16(10U, static_cast<std::uint16_t>(substitute.size() * sizeof(wchar_t)));  // SubstituteNameLength
  put16(12U, static_cast<std::uint16_t>((substitute.size() + 1U) * sizeof(wchar_t)));  // PrintNameOffset
  put16(14U, static_cast<std::uint16_t>(print.size() * sizeof(wchar_t)));       // PrintNameLength
  std::memcpy(buffer.data() + 16U, substitute.c_str(), (substitute.size() + 1U) * sizeof(wchar_t));
  std::memcpy(buffer.data() + 16U + (substitute.size() + 1U) * sizeof(wchar_t), print.c_str(), (print.size() + 1U) * sizeof(wchar_t));
  DWORD returned = 0;
  const BOOL ok = DeviceIoControl(directory, FSCTL_SET_REPARSE_POINT, buffer.data(), static_cast<DWORD>(buffer.size()),
                                  nullptr, 0, &returned, nullptr);
  CloseHandle(directory);
  return ok != 0;
}
#endif
```

`tests/paths_test.cpp` (add `#include "secure_frontend/token_reader.hpp"`):

```cpp
// Review Focus 4: non-ASCII names and paths beyond MAX_PATH round-trip through the module.
void paths_handle_non_ascii_and_long_names() {
  const auto root_path = secure_frontend_test::make_scratch_directory() / secure_frontend::path_from_utf8("Jos\xC3\xA9-\xC3\xBC");
  secure_frontend_test::make_private_directory(root_path);
  std::string relative;
  auto current = root_path;
  for (int level = 0; level < 6; ++level) {
    const std::string name = std::string(48, static_cast<char>('a' + level)) + "-\xC3\xA9";
    current /= secure_frontend::path_from_utf8(name);
    secure_frontend_test::make_private_directory(current);
    relative += name + "/";
  }
  relative += "mod\xC3\xA8le.txt";
  secure_frontend_test::write_file(current / secure_frontend::path_from_utf8("mod\xC3\xA8le.txt"), "payload");
  expect(secure_frontend::path_to_utf8(current).size() > 300U, "the test path is longer than MAX_PATH");
  const auto root = secure_frontend::open_root(root_path);
  const auto* opened_root = std::get_if<secure_frontend::CanonicalRoot>(&root);
  expect(opened_root != nullptr, "a non-ASCII root opens");
  if (opened_root == nullptr) return;
  expect(secure_frontend::path_to_utf8(opened_root->path()).find("Jos\xC3\xA9-\xC3\xBC") != std::string::npos,
         "the canonical root keeps its UTF-8 name");
  const auto file = secure_frontend::open_beneath(*opened_root,
                                                  std::get<secure_frontend::RelativePath>(secure_frontend::parse_relative_path(relative)));
  const auto* opened = std::get_if<secure_frontend::OpenedFile>(&file);
  expect(opened != nullptr, "a > 260-character path below the root opens");
  if (opened != nullptr) {
    const auto text = opened->read_all(64);
    expect(std::holds_alternative<std::string>(text) && std::get<std::string>(text) == "payload", "and reads");
  }
}

#if defined(_WIN32)
// Defect (b): the old reader opened directories with FILE_READ_ATTRIBUTES only,
// so GetSecurityInfo failed and every protected token was "insecure".
void auth02_windows_token_file_reads_with_private_dacl() {
  const auto secret = secure_frontend_test::make_scratch_directory() / "secret";
  secure_frontend_test::make_protected_directory(secret);
  const std::string token = "windows-token-0123456789abcdefghijklmnopqrs";
  secure_frontend_test::write_protected_file(secret / "token", token);
  const auto source = std::get<secure_frontend::SecretSource>(secure_frontend::make_protected_file_secret_source("token"));
  const auto result = secure_frontend::read_token(source, secret);
  const auto* error = std::get_if<secure_frontend::TokenReadError>(&result);
  expect(error == nullptr, "a private-DACL token file must read: " + (error ? error->message : std::string{}));
}

void cfg03_rejects_junction_below_root() {
  const auto root = secure_frontend_test::make_scratch_directory();
  const auto outside = secure_frontend_test::make_scratch_directory();
  secure_frontend_test::write_file(outside / "f", "outside");
  expect(secure_frontend_test::create_junction(root / "j", outside), "junction created without privilege");
  expect(fails_with(secure_frontend::open_beneath(std::get<secure_frontend::CanonicalRoot>(secure_frontend::open_root(root)),
                                                  std::get<secure_frontend::RelativePath>(secure_frontend::parse_relative_path("j/f"))),
                    PathErrorCode::symlink_not_allowed),
         "a junction below the root is rejected");
}

void cfg03_rejects_symlink_below_root() {
  const auto root = secure_frontend_test::make_scratch_directory();
  const auto outside = secure_frontend_test::make_scratch_directory();
  secure_frontend_test::write_file(outside / "f", "outside");
  if (CreateSymbolicLinkW((root / "s").c_str(), (outside / "f").c_str(), 0x2 /* ALLOW_UNPRIVILEGED_CREATE */) == 0) {
    std::cout << "SKIPPED: SeCreateSymbolicLinkPrivilege / developer mode not available\n";
    return;
  }
  expect(fails_with(secure_frontend::open_beneath(std::get<secure_frontend::CanonicalRoot>(secure_frontend::open_root(root)),
                                                  std::get<secure_frontend::RelativePath>(secure_frontend::parse_relative_path("s"))),
                    PathErrorCode::symlink_not_allowed),
         "a file symlink below the root is rejected");
}
#endif
```

Registration (`run_cases` and CMake):

```cmake
    target_link_libraries(secure_frontend_paths_tests PRIVATE secure_frontend_token_reader)
    secure_frontend_add_cases(secure_frontend_paths_tests paths_handle_non_ascii_and_long_names
        LABELS ${SECURE_FRONTEND_WINDOWS_RUNTIME})
    if(WIN32)
        secure_frontend_add_cases(secure_frontend_paths_tests
            auth02_windows_token_file_reads_with_private_dacl cfg03_rejects_junction_below_root
            cfg03_rejects_symlink_below_root
            LABELS windows-runtime)
        set_tests_properties(secure_frontend_paths_tests.cfg03_rejects_symlink_below_root PROPERTIES
            SKIP_REGULAR_EXPRESSION "SKIPPED:")
    endif()
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build --preset linux-gcc && ctest --preset linux-gcc -R paths_handle_non_ascii --output-on-failure`
Expected: PASS on Linux (the POSIX module already handles UTF-8 and long paths; this case is Review Focus 4's pin and its Windows half is what fails).
Run: `cmake --build --preset windows-llvm-mingw-cross 2>&1 | grep -m1 error`
Expected: exactly one undefined symbol at link time, `secure_frontend::open_beneath(...)` (referenced by `paths_handle_non_ascii_and_long_names` and the new Windows cases; no Windows definition yet). `OpenedFile::close_now`/`read_some` are not among the errors: Task 10 defined them for Windows. Record. In the MSYS2 job the pre-fix reader fails `auth02_windows_token_file_reads_with_private_dacl` with `insecure_file_permissions` (defect b); that run happens on the owner's push.

- [ ] **Step 3: Implement**

`canonical_path_windows.cpp` — add after `OpenedFile::read_some` (Task 10):

```cpp
std::variant<OpenedFile, PathError> open_beneath(const CanonicalRoot& root, const RelativePath& path,
                                                 const OpenOptions& options, const WalkMode) {
  using detail::path_error;
  std::array<std::byte, SECURITY_MAX_SID_SIZE> user_storage{};
  PSID user = nullptr;
  if (!ws::current_user_sid(user_storage, &user)) return path_error(PathErrorCode::open_failed, "the current user is unknown");
  if (options.trust == FileTrust::owner_only && !ws::has_private_owner_dacl(root.handle(), user)) {
    return path_error(PathErrorCode::permissions_too_broad, "the root must have a protected owner-only access control list");
  }
  std::wstring current = root.path().wstring();
  const auto& parts = path.components();
  for (std::size_t i = 0; i < parts.size(); ++i) {
    const bool last = i + 1U == parts.size();
    current += L'\\';
    current += path_from_utf8(parts[i]).wstring();
    DWORD access = READ_CONTROL | FILE_READ_ATTRIBUTES;  // READ_CONTROL: GetSecurityInfo needs it (defect b)
    if (last && options.kind == OpenKind::regular_file) access |= GENERIC_READ;
    HANDLE raw = CreateFileW(ws::extended_path(current).c_str(), access, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                             FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (raw == INVALID_HANDLE_VALUE) {
      const DWORD code = GetLastError();
      return code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND
                 ? path_error(PathErrorCode::not_found, "the path does not exist")
                 : path_error(PathErrorCode::open_failed, "the path could not be opened");
    }
    OwnedHandle guard(raw);
    if (ws::is_reparse_point(raw)) {
      return path_error(PathErrorCode::symlink_not_allowed, "a path component is a symbolic link, junction or other reparse point");
    }
    const auto final_path = classify_final_path(ws::final_path_for_handle(raw));
    const auto* final_text = std::get_if<std::wstring>(&final_path);
    if (final_text == nullptr || !is_within(root.path(), std::filesystem::path{*final_text})) {
      return path_error(PathErrorCode::escapes_root, "the path resolves outside the root");
    }
    if (options.trust == FileTrust::owner_only && !ws::has_private_owner_dacl(raw, user)) {
      return path_error(PathErrorCode::permissions_too_broad, "every path component must have a protected owner-only ACL");
    }
    FILE_ATTRIBUTE_TAG_INFO tag{};
    GetFileInformationByHandleEx(raw, FileAttributeTagInfo, &tag, sizeof(tag));
    const bool directory = (tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U;
    if (!last) {
      if (!directory) return path_error(PathErrorCode::not_directory, "a path component is not a directory");
      continue;
    }
    if (options.kind == OpenKind::directory && !directory) return path_error(PathErrorCode::not_directory, "the path is not a directory");
    if (options.kind == OpenKind::regular_file && (directory || GetFileType(raw) != FILE_TYPE_DISK)) {
      return path_error(PathErrorCode::not_regular_file, "the path is not a regular file");
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileInformationByHandle(raw, &info) == 0) return path_error(PathErrorCode::open_failed, "the file cannot be inspected");
    if (options.reject_hard_links && info.nNumberOfLinks > 1U) {
      return path_error(PathErrorCode::hard_link_not_allowed, "the file has more than one hard link");
    }
    if (options.trust == FileTrust::not_shared_writable) {
      switch (ws::write_access_is_private(raw)) {
        case ws::Trust::trusted: break;
        case ws::Trust::untrusted_owner: return path_error(PathErrorCode::untrusted_owner, "the file must be owned by you, SYSTEM or Administrators");
        case ws::Trust::shared_writable: return path_error(PathErrorCode::shared_writable, "the file must not be writable by other users");
        case ws::Trust::unreadable: return path_error(PathErrorCode::open_failed, "the file's security settings cannot be read");
      }
    }
    const std::uint64_t size = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32U) | info.nFileSizeLow;
    return detail::PathFactory::opened(guard.release(), size);
  }
  return path_error(PathErrorCode::open_failed, "the path has no components");
}
```

(`#include <array>`; `<algorithm>` is there since Task 10.)

`token_reader.cpp`: delete `class WindowsHandle` and `read_protected_file_windows` and the `#ifdef _WIN32` include block for `aclapi.h`/`sddl.h`; the portable `read_protected_file(source, root)` from Task 10 now serves both platforms (its `#ifndef _WIN32` guard is removed); both `read_token` overloads lose their `#ifdef _WIN32` branches:

```cpp
TokenReadResult read_token(const SecretSource& source, const CanonicalRoot& protected_file_root) {
  if (source.kind() == SecretSourceKind::environment_variable) return read_environment_token(source);
  return read_protected_file(source, protected_file_root);
}

TokenReadResult read_token(const SecretSource& source, const std::filesystem::path& protected_file_root) {
  if (source.kind() == SecretSourceKind::environment_variable) return read_environment_token(source);
  auto root = open_root(protected_file_root);
  if (const auto* failure = std::get_if<PathError>(&root)) return token_error_from(*failure);
  return read_protected_file(source, std::get<CanonicalRoot>(root));
}
```

`token_reader.hpp`: replace the Windows paragraph of the comment (lines 41-58 at HEAD 7ddcd755, from `// On Windows the root and every path component are opened with` to `// faithfully). Every Windows failure mode fails closed.`; Task 10 moved it down by the lines it added above it, so anchor on that text) with:

```cpp
// On Windows the root and every component are opened with READ_CONTROL |
// FILE_READ_ATTRIBUTES and FILE_FLAG_OPEN_REPARSE_POINT; a reparse point is
// symlink_not_allowed; each handle's final path must lie at or below the
// root's (path_escapes_root); the root, every directory and the file must be
// owned by the current user with a protected DACL whose only allow-ACEs name
// that user, SYSTEM or Administrators (insecure_file_permissions); a file with
// more than one hard link is rejected. Runtime-tested only by the
// windows-msys2-clang job (label windows-runtime).
```

- [ ] **Step 4: Run to verify it passes**

Run: `cmake --build --preset linux-gcc && ctest --preset linux-gcc | tail -1` → `0 tests failed` (Linux unchanged).
Run: `cmake --build --preset windows-llvm-mingw-cross | tail -1` → links with `-Werror`.
Run: `ctest --preset windows-llvm-mingw-cross | tail -1` → `0 tests failed` (the `windows-runtime` cases are excluded).
Run (record only): `WINEPREFIX=$PWD/out/wineprefix WINEDLLOVERRIDES=mscoree,mshtml= ctest --test-dir out/build/windows-llvm-mingw-cross -L windows-runtime -R paths_tests --output-on-failure 2>&1 | tail -8` → copy the per-case pass/fail/skip lines into the commit body as "wine (not authoritative)".

- [ ] **Step 5: Commit**

```bash
git -C "$(git rev-parse --show-toplevel)" add secure_frontend
git -C "$(git rev-parse --show-toplevel)" -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "paths(windows): READ_CONTROL handle walk with reparse and hard-link rejection; Windows token reader uses it (SP1 P5; defect b)" \
  -m "Windows-runtime tests (pending windows-msys2-clang): auth02_windows_token_file_reads_with_private_dacl
(fails before the fix: FILE_READ_ATTRIBUTES-only handles cannot GetSecurityInfo), cfg03_rejects_junction_below_root,
cfg03_rejects_symlink_below_root. Review Focus 4: paths_handle_non_ascii_and_long_names (passes on Linux).
Cross build -Werror ok; wine (not authoritative): <record>." \
  -m "$SP1_TRAILERS"
```

---

### Task 12: Reject config files other users can write (C7)

**Files:**
- Modify: `secure_frontend/src/config_file.cpp` (`read_config_file` opens through the module)
- Modify: `secure_frontend/tests/support/test_support.hpp` (Windows `grant_everyone_write`), `secure_frontend/tests/typed_config_test.cpp`, `secure_frontend/CMakeLists.txt`

**Interfaces:**
- Consumes: `open_root`, `parse_relative_path`, `open_beneath` with `FileTrust::not_shared_writable`, `OpenedFile::read_all`, `path_to_utf8` (Tasks 8–11).
- Produces: `read_config_file()` keeps its signature; trust failures are `ConfigIssueCode::file_untrusted` under key `config file`. Test support (Windows): `bool grant_everyone_write(const std::filesystem::path& path);`.

- [ ] **Step 1: Write the failing tests** (`tests/typed_config_test.cpp`; add `#include "secure_frontend/config_file.hpp"` already present)

```cpp
bool untrusted(const std::variant<secure_frontend::FrontendConfigDraft, secure_frontend::ConfigErrors>& result) {
  const auto* errors = std::get_if<secure_frontend::ConfigErrors>(&result);
  return errors != nullptr && !errors->empty() && errors->front().key_path == "config file" &&
         errors->front().code == secure_frontend::ConfigIssueCode::file_untrusted;
}

#if !defined(_WIN32)
void cfg01_group_writable_config_rejected() {
  ConfigFixture f;
  const auto file = f.dir / "config.json";
  secure_frontend_test::write_file(file, f.document(), 0664);
  expect(untrusted(secure_frontend::read_config_file(file)), "a group-writable config file is rejected");
  chmod(file.c_str(), 0644);
  expect(std::holds_alternative<secure_frontend::FrontendConfigDraft>(secure_frontend::read_config_file(file)),
         "0644 is fine: the config is not secret");
  const auto shared = secure_frontend_test::make_scratch_directory();
  secure_frontend_test::write_file(shared / "config.json", f.document(), 0600);
  chmod(shared.c_str(), 0777);
  expect(untrusted(secure_frontend::read_config_file(shared / "config.json")),
         "a config in a directory others can write is rejected (they could swap it)");
  std::filesystem::create_symlink(file, f.dir / "linked.json");
  expect(untrusted(secure_frontend::read_config_file(f.dir / "linked.json")), "a symlinked config file is rejected");
}
#else
void cfg01_config_with_everyone_write_ace_rejected() {
  ConfigFixture f;
  const auto file = f.dir / "config.json";
  secure_frontend_test::write_file(file, f.document());
  expect(std::holds_alternative<secure_frontend::FrontendConfigDraft>(secure_frontend::read_config_file(file)),
         "a config with the inherited user ACL is accepted");
  expect(secure_frontend_test::grant_everyone_write(file), "test setup: add an Everyone write ACE");
  expect(untrusted(secure_frontend::read_config_file(file)), "a config Everyone can write is rejected");
}
#endif
```

Add `#include <sys/stat.h>` under `#if !defined(_WIN32)`. Test support (Windows block; `#include <aclapi.h>`):

```cpp
inline bool grant_everyone_write(const std::filesystem::path& path) {
  PACL old_dacl = nullptr;
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &old_dacl, nullptr,
                            &descriptor) != ERROR_SUCCESS) return false;
  DWORD sid_size = SECURITY_MAX_SID_SIZE;
  std::vector<unsigned char> world(SECURITY_MAX_SID_SIZE);
  CreateWellKnownSid(WinWorldSid, nullptr, world.data(), &sid_size);
  EXPLICIT_ACCESS_W access{};
  access.grfAccessPermissions = FILE_GENERIC_WRITE;
  access.grfAccessMode = GRANT_ACCESS;
  access.grfInheritance = NO_INHERITANCE;
  access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
  access.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
  access.Trustee.ptstrName = reinterpret_cast<LPWSTR>(world.data());
  PACL updated = nullptr;
  const bool ok = SetEntriesInAclW(1, &access, old_dacl, &updated) == ERROR_SUCCESS &&
                  SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                        nullptr, nullptr, updated, nullptr) == ERROR_SUCCESS;
  if (updated != nullptr) LocalFree(updated);
  LocalFree(descriptor);
  return ok;
}
```

CMake:

```cmake
    if(WIN32)
        secure_frontend_add_cases(secure_frontend_typed_config_tests cfg01_config_with_everyone_write_ace_rejected
            LABELS windows-runtime)
    else()
        secure_frontend_add_cases(secure_frontend_typed_config_tests cfg01_group_writable_config_rejected)
    endif()
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build --preset linux-gcc && ctest --preset linux-gcc -R cfg01_group_writable --output-on-failure`
Expected: FAIL — `FAILED: a group-writable config file is rejected` (and the shared-directory and symlink lines).
Windows half: cross-compiles; recorded under wine; authoritative in MSYS2.

- [ ] **Step 3: Implement** — replace `read_config_file` in `config_file.cpp` (add `#include "secure_frontend/canonical_path.hpp"`; drop `<fstream>`):

```cpp
std::variant<FrontendConfigDraft, ConfigErrors> read_config_file(const std::filesystem::path& file) {
  const auto issue = [](const ConfigIssueCode code, std::string reason) {
    return ConfigErrors{ConfigIssue{"config file", code, std::move(reason)}};
  };
  std::error_code error;
  const auto absolute = std::filesystem::absolute(file, error);
  if (error || !absolute.has_filename()) return issue(ConfigIssueCode::file_unavailable, "cannot be resolved");
  // The directory is opened as a root: owned by you (or root) and not writable
  // by others, who could otherwise replace the file between check and use.
  auto directory = open_root(absolute.parent_path());
  if (const auto* failure = std::get_if<PathError>(&directory)) {
    const bool trust = failure->code == PathErrorCode::shared_writable || failure->code == PathErrorCode::untrusted_owner;
    return issue(trust ? ConfigIssueCode::file_untrusted : ConfigIssueCode::file_unavailable, "directory: " + failure->message);
  }
  const auto& root = std::get<CanonicalRoot>(directory);
  const auto name = parse_relative_path(path_to_utf8(absolute.filename()));
  if (!std::holds_alternative<RelativePath>(name)) return issue(ConfigIssueCode::file_unavailable, "has a non-portable file name");
  auto opened = open_beneath(root, std::get<RelativePath>(name),
                             OpenOptions{.kind = OpenKind::regular_file, .trust = FileTrust::not_shared_writable});
  if (const auto* failure = std::get_if<PathError>(&opened)) {
    switch (failure->code) {
      case PathErrorCode::not_found: return issue(ConfigIssueCode::file_unavailable, "does not exist");
      case PathErrorCode::symlink_not_allowed:
      case PathErrorCode::shared_writable:
      case PathErrorCode::untrusted_owner: return issue(ConfigIssueCode::file_untrusted, failure->message);
      default: return issue(ConfigIssueCode::file_unavailable, failure->message);
    }
  }
  auto text = std::get<OpenedFile>(opened).read_all(kMaxConfigFileBytes);
  if (const auto* failure = std::get_if<PathError>(&text)) {
    return failure->code == PathErrorCode::too_large ? issue(ConfigIssueCode::malformed_document, "is larger than 64 KiB")
                                                     : issue(ConfigIssueCode::file_unavailable, failure->message);
  }
  return parse_config_text(std::get<std::string>(text), root.path());
}
```

- [ ] **Step 4: Run to verify it passes**

Run: `cmake --build --preset linux-gcc && ctest --preset linux-gcc | tail -1` → `0 tests failed` (the CLI and e2e fixtures already write `config.json` as 0600 in a 0700 directory).
Run: `cmake --build --preset windows-llvm-mingw-cross | tail -1 && ctest --preset windows-llvm-mingw-cross | tail -1` → build ok; record.

- [ ] **Step 5: Commit**

```bash
git -C "$(git rev-parse --show-toplevel)" add secure_frontend
git -C "$(git rev-parse --show-toplevel)" -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "config: refuse config files (and their directories) that other users can write (SP1 C7)" \
  -m "Regression tests: cfg01_group_writable_config_rejected (Linux, failed before),
cfg01_config_with_everyone_write_ace_rejected (Windows, windows-runtime, pending MSYS2; wine <record>)." \
  -m "$SP1_TRAILERS"
```

---

### Task 13: Inventory and tokenizer pack read through the module (P6)

**Files:**
- Modify: `secure_frontend/include/secure_frontend/canonical_path.hpp` (`EntryType`, `DirectoryEntry`, `list_directory`)
- Modify: `secure_frontend/src/canonical_path_posix.cpp`, `secure_frontend/src/canonical_path_windows.cpp` (`list_directory`)
- Modify: `secure_frontend/include/secure_frontend/model_inventory.hpp:58`, `secure_frontend/src/model_inventory.cpp:104-187`
- Modify: `secure_frontend/include/secure_frontend/tokenizer_pack.hpp:56-57`, `secure_frontend/src/tokenizer_pack.cpp:160-174, 249-253`
- Modify: `secure_frontend/src/secure_server_main.cpp` (pass the `CanonicalRoot`s)
- Modify: `secure_frontend/tests/model_inventory_test.cpp`, `secure_frontend/tests/tokenizer_pack_test.cpp`, `secure_frontend/CMakeLists.txt`

**Interfaces:**
- Consumes: `CanonicalRoot`, `open_root`, `open_beneath`, `parse_relative_path`, `path_to_utf8` (Tasks 8–11).
- Produces:
```cpp
namespace secure_frontend {
enum class EntryType { regular_file, directory, symlink, other };     // symlink covers junctions and other reparse points
struct DirectoryEntry final { std::string name; EntryType type; std::uint64_t size; };   // name is UTF-8
[[nodiscard]] std::variant<std::vector<DirectoryEntry>, PathError> list_directory(const CanonicalRoot& root,
                                                                                  const std::optional<RelativePath>& below = std::nullopt);
[[nodiscard]] ModelInventoryResult enumerate_local_models(const CanonicalRoot& model_root);
[[nodiscard]] ModelInventoryResult enumerate_local_models(const std::filesystem::path& model_root);     // opens the root first
[[nodiscard]] TokenizerPackLoadResult load_local_tokenizer_pack(const CanonicalRoot& pack_root);
[[nodiscard]] TokenizerPackLoadResult load_local_tokenizer_pack(const std::filesystem::path& pack_root); // opens the root first
}
```

- [ ] **Step 1: Write the failing tests**

`tests/model_inventory_test.cpp` — remove the symlink half of `rejects_duplicate_ids_and_symlinks` (lines 95-103), rename it `rejects_duplicate_ids` (also in its `run_cases` entry), add `#include "support/test_support.hpp"` if missing, and add:

```cpp
#if !defined(_WIN32)
void cfg03_inventory_rejects_symlink_entry() {
  const auto root = unique_temp_directory();
  write_file(root / "real.gguf", "a");
  std::filesystem::create_symlink(root / "real.gguf", root / "linked.gguf");
  expect(has_error(secure_frontend::enumerate_local_models(root),
                   secure_frontend::ModelInventoryErrorCode::symbolic_link_not_allowed),
         "inventory must reject, not follow, symbolic links");
}

// Review Focus 1: a model kept on another disk via a symlinked file.
void inventory_symlink_error_names_entry() {
  const auto root = unique_temp_directory();
  const auto other_disk = unique_temp_directory();
  write_file(other_disk / "x.gguf", "big");
  std::filesystem::create_directories(root / "big");
  std::filesystem::create_symlink(other_disk / "x.gguf", root / "big" / "x.gguf");
  const auto result = secure_frontend::enumerate_local_models(root);
  const auto* error = std::get_if<secure_frontend::ModelInventoryError>(&result);
  expect(error != nullptr && error->code == secure_frontend::ModelInventoryErrorCode::symbolic_link_not_allowed,
         "the symlinked file fails the inventory");
  if (error != nullptr) {
    expect(error->message.find("big/x.gguf") != std::string::npos, "the error names the entry: " + error->message);
    expect(error->message.find("roots.models") != std::string::npos, "and says what to do instead: " + error->message);
  }
}

// Review Focus 5: two names a Windows client cannot tell apart.
void inventory_duplicate_detection_is_case_insensitive() {
  const auto root = unique_temp_directory();
  write_file(root / "Qwen.gguf", "a");
  write_file(root / "qwen.gguf", "b");
  expect(has_error(secure_frontend::enumerate_local_models(root), secure_frontend::ModelInventoryErrorCode::duplicate_model_id),
         "IDs differing only in case are duplicates");
}
#else
void cfg03_inventory_rejects_junction_entry() {
  const auto root = unique_temp_directory();
  const auto outside = unique_temp_directory();
  write_file(outside / "far.gguf", "a");
  expect(secure_frontend_test::create_junction(root / "j", outside), "junction created");
  expect(has_error(secure_frontend::enumerate_local_models(root),
                   secure_frontend::ModelInventoryErrorCode::symbolic_link_not_allowed),
         "a junction inside the models root fails the inventory");
}
#endif
```

with `run_cases` entries under the same split:

```cpp
#if !defined(_WIN32)
      {"cfg03_inventory_rejects_symlink_entry", cfg03_inventory_rejects_symlink_entry},
      {"inventory_symlink_error_names_entry", inventory_symlink_error_names_entry},
      {"inventory_duplicate_detection_is_case_insensitive", inventory_duplicate_detection_is_case_insensitive},
#else
      {"cfg03_inventory_rejects_junction_entry", cfg03_inventory_rejects_junction_entry},
#endif
```

`tests/tokenizer_pack_test.cpp` (inside the existing `#ifndef _WIN32` block, plus a matching `run_cases` entry under its `#ifndef _WIN32`):

```cpp
void cfg03_tokenizer_file_swapped_to_symlink_rejected() {
  const auto root = temporary_pack();
  const auto outside = temporary_pack();
  write(outside / "vocab.json", R"({"a":0})");
  write(root / "merges.txt", "");
  write(root / "token_type.json", "[0]");
  write(root / "vocab.json", R"({"a":0})");
  expect(std::holds_alternative<secure_frontend::TokenizerPack>(secure_frontend::load_local_tokenizer_pack(root)),
         "the regular pack loads");
  std::filesystem::remove(root / "vocab.json");
  std::filesystem::create_symlink(outside / "vocab.json", root / "vocab.json");
  expect(has_error(secure_frontend::load_local_tokenizer_pack(root), secure_frontend::TokenizerPackErrorCode::symbolic_link_not_allowed),
         "after the swap the file is opened by handle with O_NOFOLLOW and rejected");
}
```

and its Windows twin (a new `#if defined(_WIN32)` block after the `#ifndef _WIN32` block, with a matching `run_cases` entry under `#if defined(_WIN32)`): a junction needs no privilege, so the swap is always testable there.

```cpp
#if defined(_WIN32)
void cfg03_tokenizer_file_swapped_to_junction_rejected() {
  const auto root = temporary_pack();
  const auto outside = temporary_pack();
  write(outside / "vocab.json", R"({"a":0})");
  write(root / "merges.txt", "");
  write(root / "token_type.json", "[0]");
  write(root / "vocab.json", R"({"a":0})");
  expect(std::holds_alternative<secure_frontend::TokenizerPack>(secure_frontend::load_local_tokenizer_pack(root)),
         "the regular pack loads");
  std::filesystem::remove(root / "vocab.json");
  expect(secure_frontend_test::create_junction(root / "vocab.json", outside), "junction created without privilege");
  expect(has_error(secure_frontend::load_local_tokenizer_pack(root), secure_frontend::TokenizerPackErrorCode::symbolic_link_not_allowed),
         "a junction swapped in for vocab.json is opened with FILE_FLAG_OPEN_REPARSE_POINT and rejected");
}
#endif
```

```cpp
#if defined(_WIN32)
      {"cfg03_tokenizer_file_swapped_to_junction_rejected", cfg03_tokenizer_file_swapped_to_junction_rejected},
#endif
```

CMake: from this task on the inventory and tokenizer cases that enumerate or load go through `open_root`, so under the Task 9 wine rule they carry the label; the two pure model-ID/path grammar cases stay unlabelled. Replace the model-inventory registration with:

```cmake
    secure_frontend_add_cases(secure_frontend_model_inventory_tests
        validates_portable_model_ids validates_portable_artifact_paths)
    set(_inventory_root_cases inventories_only_local_gguf_artifacts rejects_duplicate_ids)
    if(WIN32)
        list(APPEND _inventory_root_cases cfg03_inventory_rejects_junction_entry)
    else()
        list(APPEND _inventory_root_cases cfg03_inventory_rejects_symlink_entry inventory_symlink_error_names_entry
            inventory_duplicate_detection_is_case_insensitive)
    endif()
    secure_frontend_add_cases(secure_frontend_model_inventory_tests ${_inventory_root_cases}
        LABELS ${SECURE_FRONTEND_WINDOWS_RUNTIME})
```

and the tokenizer-pack registration with:

```cmake
    set(_tokenizer_cases loads_validates_and_decodes_offline_pack rejects_ambiguous_or_incompatible_pack_assets
        rejects_malformed_json_and_special_literals_for_decode encoding_fails_closed_for_special_invalid_and_missing_tokens)
    if(WIN32)
        list(APPEND _tokenizer_cases cfg03_tokenizer_file_swapped_to_junction_rejected)
    else()
        list(APPEND _tokenizer_cases rejects_symlinked_assets cfg03_tokenizer_file_swapped_to_symlink_rejected)
    endif()
    secure_frontend_add_cases(secure_frontend_tokenizer_pack_tests ${_tokenizer_cases}
        LABELS ${SECURE_FRONTEND_WINDOWS_RUNTIME})
```

(the model-inventory test already includes `support/test_support.hpp` since Task 2, which provides `create_junction` since Task 11).

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build --preset linux-gcc && ctest --preset linux-gcc -R 'inventory_symlink_error|case_insensitive|swapped_to_symlink|rejects_symlink_entry' --output-on-failure`
Expected: `inventory_symlink_error_names_entry` FAILS (`the error names the entry: symbolic links are not allowed in the model inventory`); `inventory_duplicate_detection_is_case_insensitive` FAILS (two IDs accepted); the symlink-entry and tokenizer-swap cases PASS today (the old `symlink_status` checks catch a static symlink; the race they guard against — a swap between check and `ifstream` open — cannot be forced deterministically; the regression guard is that the open now goes through `open_beneath`). Record this in the commit.

- [ ] **Step 3: Implement**

`canonical_path.hpp`:

```cpp
enum class EntryType { regular_file, directory, symlink, other };
struct DirectoryEntry final {
  std::string name;  // UTF-8
  EntryType type;
  std::uint64_t size;
};
// Lists `root` (or a directory below it, opened with open_beneath) without
// following links; a junction or any reparse point reports EntryType::symlink.
// Entries are sorted by name.
[[nodiscard]] std::variant<std::vector<DirectoryEntry>, PathError> list_directory(
    const CanonicalRoot& root, const std::optional<RelativePath>& below = std::nullopt);
```

`canonical_path_posix.cpp` (add `#include <algorithm>`, `#include <dirent.h>`):

```cpp
std::variant<std::vector<DirectoryEntry>, PathError> list_directory(const CanonicalRoot& root,
                                                                    const std::optional<RelativePath>& below) {
  using detail::path_error;
  int fd = -1;
  if (below.has_value()) {
    auto opened = open_beneath(root, *below, OpenOptions{.kind = OpenKind::directory});
    if (const auto* failure = std::get_if<PathError>(&opened)) return *failure;
    fd = ::fcntl(std::get<OpenedFile>(opened).native(), F_DUPFD_CLOEXEC, 0);
  } else {
    fd = ::openat(root.handle(), ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  }
  if (fd < 0) return path_error(PathErrorCode::open_failed, "the directory could not be opened");
  DIR* directory = ::fdopendir(fd);
  if (directory == nullptr) {
    ::close(fd);
    return path_error(PathErrorCode::open_failed, "the directory could not be listed");
  }
  std::vector<DirectoryEntry> entries;
  while (const dirent* entry = ::readdir(directory)) {
    const std::string name{entry->d_name};
    if (name == "." || name == "..") continue;
    struct stat st {};
    if (::fstatat(::dirfd(directory), entry->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
      ::closedir(directory);
      return path_error(PathErrorCode::open_failed, "a directory entry could not be inspected");
    }
    const EntryType type = S_ISLNK(st.st_mode) ? EntryType::symlink
                           : S_ISDIR(st.st_mode) ? EntryType::directory
                           : S_ISREG(st.st_mode) ? EntryType::regular_file
                                                 : EntryType::other;
    entries.push_back(DirectoryEntry{name, type, static_cast<std::uint64_t>(st.st_size)});
  }
  ::closedir(directory);
  std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
  return entries;
}
```

`canonical_path_windows.cpp`:

```cpp
std::variant<std::vector<DirectoryEntry>, PathError> list_directory(const CanonicalRoot& root,
                                                                    const std::optional<RelativePath>& below) {
  using detail::path_error;
  std::wstring directory = root.path().wstring();
  if (below.has_value()) {
    auto opened = open_beneath(root, *below, OpenOptions{.kind = OpenKind::directory});  // refuses reparse points on the way
    if (const auto* failure = std::get_if<PathError>(&opened)) return *failure;
    for (const auto& component : below->components()) directory += L'\\' + path_from_utf8(component).wstring();
  }
  WIN32_FIND_DATAW data{};
  HANDLE find = FindFirstFileExW(ws::extended_path(directory + L"\\*").c_str(), FindExInfoBasic, &data,
                                 FindExSearchNameMatch, nullptr, 0);
  if (find == INVALID_HANDLE_VALUE) return path_error(PathErrorCode::open_failed, "the directory could not be listed");
  std::vector<DirectoryEntry> entries;
  do {
    const std::wstring name{data.cFileName};
    if (name == L"." || name == L"..") continue;
    const DWORD attributes = data.dwFileAttributes;
    const EntryType type = (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U ? EntryType::symlink
                           : (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U   ? EntryType::directory
                                                                             : EntryType::regular_file;
    const std::uint64_t size = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32U) | data.nFileSizeLow;
    entries.push_back(DirectoryEntry{path_to_utf8(std::filesystem::path{name}), type, size});
  } while (FindNextFileW(find, &data) != 0);
  FindClose(find);
  std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
  return entries;
}
```

`model_inventory.hpp` line 58 → the two overloads (Interfaces; `#include "secure_frontend/canonical_path.hpp"`). `model_inventory.cpp` lines 104-187 become:

```cpp
ModelInventoryResult enumerate_local_models(const CanonicalRoot& model_root) {
  std::vector<ModelManifest> models;
  std::vector<std::optional<RelativePath>> pending{std::nullopt};
  while (!pending.empty()) {
    const auto directory = std::move(pending.back());
    pending.pop_back();
    auto listed = list_directory(model_root, directory);
    if (std::holds_alternative<PathError>(listed)) {
      return ModelInventoryError{ModelInventoryErrorCode::inventory_read_failed,
                                 "unable to list " + (directory ? directory->generic() : std::string{"the model root"})};
    }
    for (const auto& entry : std::get<std::vector<DirectoryEntry>>(listed)) {
      const std::string relative_text = directory ? directory->generic() + "/" + entry.name : entry.name;
      const auto relative = parse_relative_path(relative_text);
      const auto* path = std::get_if<RelativePath>(&relative);
      const std::string shown = path != nullptr ? path->generic() : std::string{"an entry with a non-portable name"};
      if (entry.type == EntryType::symlink) {
        return ModelInventoryError{ModelInventoryErrorCode::symbolic_link_not_allowed,
                                   "symbolic link or junction in the model inventory: " + shown +
                                       "; point roots.models at the other disk instead (the root itself may be a link)"};
      }
      if (path == nullptr) {
        return ModelInventoryError{ModelInventoryErrorCode::invalid_artifact_path,
                                   "a model inventory entry is not a portable path"};
      }
      if (entry.type == EntryType::directory) { pending.push_back(*path); continue; }
      if (entry.type != EntryType::regular_file || !is_gguf_extension(path_from_utf8(entry.name))) continue;
      const std::string id = path_from_utf8(entry.name).stem().string();
      if (const auto* invalid = std::get_if<ModelInventoryError>(&validate_model_id(id))) return *invalid;
      models.push_back(ModelManifest{.id = id, .relative_artifact_path = path->generic(), .artifact_size_bytes = entry.size});
    }
  }
  const auto lower = [](std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
  };
  std::sort(models.begin(), models.end(), [](const ModelManifest& a, const ModelManifest& b) { return a.id < b.id; });
  std::vector<std::string> folded;
  for (const auto& model : models) folded.push_back(lower(model.id));
  std::sort(folded.begin(), folded.end());
  if (std::adjacent_find(folded.begin(), folded.end()) != folded.end()) {
    return ModelInventoryError{ModelInventoryErrorCode::duplicate_model_id,
                               "multiple local artifacts resolve to the same model ID (IDs are compared ignoring case)"};
  }
  return models;
}

ModelInventoryResult enumerate_local_models(const std::filesystem::path& model_root) {
  auto root = open_root(model_root);
  if (const auto* failure = std::get_if<PathError>(&root)) {
    return ModelInventoryError{failure->code == PathErrorCode::not_directory ? ModelInventoryErrorCode::model_root_not_directory
                                                                             : ModelInventoryErrorCode::model_root_unavailable,
                               "model root: " + failure->message};
  }
  return enumerate_local_models(std::get<CanonicalRoot>(root));
}
```

(`validate_model_id` returns by value: bind it first — `const auto validated = validate_model_id(id); if (const auto* invalid = std::get_if<ModelInventoryError>(&validated)) return *invalid;`. Delete the now-unused `filesystem_error` helper and `<system_error>` include.)

`tokenizer_pack.hpp` lines 56-57 → the two overloads. `tokenizer_pack.cpp`: replace `read_required` (lines 160-174) with

```cpp
std::variant<std::string, TokenizerPackError> read_required(const CanonicalRoot& root, const std::string_view name) {
  const auto relative = std::get<RelativePath>(parse_relative_path(name));
  auto opened = open_beneath(root, relative, OpenOptions{.kind = OpenKind::regular_file});
  if (const auto* failure = std::get_if<PathError>(&opened)) {
    switch (failure->code) {
      case PathErrorCode::not_found: return error(TokenizerPackErrorCode::required_file_missing, "required tokenizer pack file is missing");
      case PathErrorCode::symlink_not_allowed: return error(TokenizerPackErrorCode::symbolic_link_not_allowed, "tokenizer pack files must not be symbolic links");
      case PathErrorCode::not_regular_file: return error(TokenizerPackErrorCode::unsafe_pack_path, "tokenizer pack paths must name regular files");
      default: return error(TokenizerPackErrorCode::file_read_failed, "unable to open tokenizer pack file");
    }
  }
  const auto& file = std::get<OpenedFile>(opened);
  if (file.size() > kMaximumFileBytes) return error(TokenizerPackErrorCode::file_too_large, "tokenizer pack file exceeds fixed limit");
  auto text = file.read_all(kMaximumFileBytes);
  if (const auto* failure = std::get_if<PathError>(&text)) {
    return error(failure->code == PathErrorCode::too_large ? TokenizerPackErrorCode::file_too_large : TokenizerPackErrorCode::file_read_failed,
                 "unable to read tokenizer pack file");
  }
  return std::get<std::string>(std::move(text));
}
```

and lines 249-253 with

```cpp
TokenizerPackLoadResult load_local_tokenizer_pack(const std::filesystem::path& pack_root) {
  auto root = open_root(pack_root);
  if (const auto* failure = std::get_if<PathError>(&root)) {
    return error(failure->code == PathErrorCode::not_directory ? TokenizerPackErrorCode::pack_root_not_directory
                                                               : TokenizerPackErrorCode::pack_root_unavailable,
                 "tokenizer pack root must be an existing private directory");
  }
  return load_local_tokenizer_pack(std::get<CanonicalRoot>(root));
}

TokenizerPackLoadResult load_local_tokenizer_pack(const CanonicalRoot& root) {
```

(the rest of the old body continues unchanged from `const auto vocab_text = read_required(root, "vocab.json"); …`; drop `<fstream>` and `<system_error>`; `#include "secure_frontend/canonical_path.hpp"`; link `secure_frontend_tokenizer_pack` PUBLIC to `secure_frontend_paths`).

`secure_server_main.cpp` — in `run()` replace the two statements Task 9 wrote,

```cpp
  const auto tokenizer_result = secure_frontend::load_local_tokenizer_pack(config.roots.tokenizer_pack.path());
```
```cpp
  const auto models_result = secure_frontend::enumerate_local_models(config.roots.models.path());
```

with the `CanonicalRoot` overloads (the roots validation already opened):

```cpp
  const auto tokenizer_result = secure_frontend::load_local_tokenizer_pack(config.roots.tokenizer_pack);
```
```cpp
  const auto models_result = secure_frontend::enumerate_local_models(config.roots.models);
```

- [ ] **Step 4: Run to verify it passes**

Run: `cmake --build --preset linux-gcc && ctest --preset linux-gcc | tail -1` → `0 tests failed` (the smoke executables keep the path overloads).
Run: `cmake --build --preset windows-llvm-mingw-cross | tail -1 && ctest --preset windows-llvm-mingw-cross | tail -1` → build ok; `0 tests failed` with the root-opening inventory/tokenizer cases excluded by their label; record.
Run (record only): `WINEPREFIX=$PWD/out/wineprefix WINEDLLOVERRIDES=mscoree,mshtml= ctest --test-dir out/build/windows-llvm-mingw-cross -L windows-runtime -R 'junction' --output-on-failure 2>&1 | tail -4` → paste into the commit body as "wine (not authoritative)".

- [ ] **Step 5: Commit**

```bash
git -C "$(git rev-parse --show-toplevel)" add secure_frontend
git -C "$(git rev-parse --show-toplevel)" -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "inventory, tokenizer: enumerate and open through the canonical-path module (SP1 P6)" \
  -m "Regression tests (failed before): inventory_symlink_error_names_entry (Review Focus 1),
inventory_duplicate_detection_is_case_insensitive (Review Focus 5). Guards that passed before (static symlinks;
the swap race is not deterministically testable): cfg03_inventory_rejects_symlink_entry,
cfg03_tokenizer_file_swapped_to_symlink_rejected. Windows (windows-runtime, pending MSYS2; wine <record>):
cfg03_inventory_rejects_junction_entry, cfg03_tokenizer_file_swapped_to_junction_rejected. The inventory and
tokenizer cases that open a root are labelled windows-runtime (Task 9 wine rule)." \
  -m "$SP1_TRAILERS"
```

---

### Task 14: Create directories only within a root; logs/cache roots; delete the duplicate checks (P8, P9)

**Files:**
- Modify: `secure_frontend/include/secure_frontend/canonical_path.hpp` (`create_directory_within`; `RootPolicy::create_private`; `check_creatable_root`)
- Modify: `secure_frontend/src/canonical_path_common.cpp` (`open_root` policy, `check_creatable_root`), `secure_frontend/src/canonical_path_posix.cpp`, `secure_frontend/src/canonical_path_windows.cpp`
- Modify: `secure_frontend/include/secure_frontend/frontend_config.hpp`, `secure_frontend/src/frontend_config.cpp` (`roots.logs`/`roots.cache` are checked without side effects; `create_runtime_roots`)
- Modify: `secure_frontend/src/secure_server_main.cpp` (`run()` creates the logs/cache roots before bind/spawn; `--check` never does)
- Modify: `secure_frontend/include/secure_frontend/secret_source.hpp:50-56` (comment names the module)
- Modify: `secure_frontend/tests/paths_test.cpp`, `secure_frontend/tests/typed_config_test.cpp`, `secure_frontend/tests/server_cli_test.cpp`, `secure_frontend/CMakeLists.txt`

**Interfaces:**
- Produces (Tasks 16, 17, 21):
```cpp
namespace secure_frontend {
enum class RootPolicy { existing, create_private };   // create_private: a missing last component is created 0700 / private DACL
[[nodiscard]] std::optional<PathError> create_directory_within(const CanonicalRoot& root, const RelativePath& path);
// Side-effect free: the canonical path of an existing private root, or of an absent one whose
// parent opens as a private root and whose last component is one valid name. Creates nothing.
[[nodiscard]] std::variant<std::filesystem::path, PathError> check_creatable_root(const std::filesystem::path& requested);
// frontend_config.hpp
struct Roots final { CanonicalRoot models; CanonicalRoot tokenizer_pack; std::optional<CanonicalRoot> secret;
                     std::optional<std::filesystem::path> logs; std::optional<std::filesystem::path> cache; };  // logs/cache: canonical, may not exist yet
// The only filesystem side effect of a configuration: creates a missing roots.logs / roots.cache
// (0700 / private DACL). Called by serve (run) and init; never by validate() or --check.
[[nodiscard]] ConfigErrors create_runtime_roots(const FrontendConfig& config);
}
```
  `--check` therefore has no side effects (Global Constraints): `validate()` opens and inspects only, and accepts a logs/cache root that is absent but creatable under a private parent.

- [ ] **Step 1: Write the failing tests**

`tests/paths_test.cpp` (add `#include <algorithm>` and `#include <vector>` to its includes; `listing()` sorts):

```cpp
std::vector<std::string> listing(const std::filesystem::path& base) {
  std::vector<std::string> names;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(base)) {
    names.push_back(secure_frontend::path_to_utf8(entry.path().lexically_relative(base)));
  }
  std::sort(names.begin(), names.end());
  return names;
}

void cfg02_create_within_root_writes_nothing_outside() {
  const auto scratch = secure_frontend_test::make_scratch_directory();
  secure_frontend_test::make_private_directory(scratch / "root");
  secure_frontend_test::make_private_directory(scratch / "outside");
  secure_frontend_test::make_private_directory(scratch / "root2");
  const auto root = std::get<secure_frontend::CanonicalRoot>(secure_frontend::open_root(scratch / "root"));
  const auto rel = [](const char* text) {
    return std::get<secure_frontend::RelativePath>(secure_frontend::parse_relative_path(text));
  };
#if !defined(_WIN32)
  std::filesystem::create_directory_symlink(scratch / "outside", scratch / "root" / "l");
  secure_frontend_test::write_file(scratch / "root" / "f", "file");
#else
  secure_frontend_test::create_junction(scratch / "root" / "l", scratch / "outside");
  secure_frontend_test::write_file(scratch / "root" / "f", "file");
#endif
  const auto before = listing(scratch);
  for (const char* escape : {"../outside/x", "/abs", "a/../../x", "..\\root2\\x"}) {
    expect(std::holds_alternative<secure_frontend::PathError>(secure_frontend::parse_relative_path(escape)),
           std::string{"escapes never become a RelativePath: "} + escape);
  }
  const auto through_link = secure_frontend::create_directory_within(root, rel("l/x"));
  expect(through_link.has_value() && through_link->code == secure_frontend::PathErrorCode::symlink_not_allowed,
         "creating through a link is refused");
  const auto through_file = secure_frontend::create_directory_within(root, rel("f/x"));
  expect(through_file.has_value() && through_file->code == secure_frontend::PathErrorCode::not_directory,
         "creating below a file is refused");
  expect(listing(scratch) == before, "the failed attempts wrote nothing anywhere");
  expect(!secure_frontend::create_directory_within(root, rel("a/b")).has_value(), "a/b is created below the root");
  auto after = listing(scratch);
  std::vector<std::string> expected = before;
  expected.push_back(secure_frontend::path_to_utf8(std::filesystem::path{"root"} / "a"));
  expected.push_back(secure_frontend::path_to_utf8(std::filesystem::path{"root"} / "a" / "b"));
  std::sort(expected.begin(), expected.end());
  expect(after == expected, "exactly root/a and root/a/b were added");
#if !defined(_WIN32)
  struct stat st {};
  stat((scratch / "root" / "a" / "b").c_str(), &st);
  expect((st.st_mode & 0777) == 0700, "created directories are 0700");
#endif
}
```

`tests/typed_config_test.cpp` (validation creates nothing; `create_runtime_roots` does):

```cpp
void cfg02_logs_and_cache_roots_are_created_private() {
  ConfigFixture f;
  auto literals = f.literals;
  literals["roots.logs"] = secure_frontend_test::json_literal((f.dir / "logs").string());
  literals["roots.cache"] = secure_frontend_test::json_literal((f.dir / "cache").string());
  const auto result = validate_text(render_config_json(literals), f.dir);
  const auto* validated = std::get_if<secure_frontend::ValidatedConfig>(&result);
  expect(validated != nullptr, "absent logs/cache roots under a private parent validate: " + issues_of(result));
  expect(!std::filesystem::exists(f.dir / "logs") && !std::filesystem::exists(f.dir / "cache"),
         "validate() creates nothing (--check has no side effects)");
  if (validated == nullptr) return;
  expect(secure_frontend::create_runtime_roots(validated->get()).empty(), "serve/init create the missing roots");
  expect(std::filesystem::is_directory(f.dir / "logs") && std::filesystem::is_directory(f.dir / "cache"), "both exist");
#if !defined(_WIN32)
  struct stat st {};
  stat((f.dir / "logs").c_str(), &st);
  expect((st.st_mode & 0777) == 0700, "created 0700");
#endif
  expect(secure_frontend::create_runtime_roots(validated->get()).empty(), "existing private roots are accepted again");
#if !defined(_WIN32)
  const auto shared = f.dir / "shared";
  secure_frontend_test::make_private_directory(shared);
  chmod(shared.c_str(), 0777);
  literals["roots.logs"] = secure_frontend_test::json_literal((shared / "logs").string());
  expect(has_issue(validate_text(render_config_json(literals), f.dir), "roots.logs", ConfigIssueCode::invalid_value),
         "an absent root whose parent others can write is rejected at validation");
  expect(!std::filesystem::exists(shared / "logs"), "and nothing was created");
#endif
}
```

`tests/server_cli_test.cpp` — the process-level guarantee (uses `snapshot_tree` from Task 5's `server_fixture.hpp`):

```cpp
void cli_check_has_no_side_effects() {
  ServerFixture f;
  auto literals = f.base_literals();
  literals["roots.logs"] = secure_frontend_test::json_literal((f.dir / "logs").string());
  literals["roots.cache"] = secure_frontend_test::json_literal((f.dir / "cache").string());
  f.write_config(literals);
  const auto before = secure_frontend_test::snapshot_tree(f.dir);
  const auto checked = f.run({"--check", "--config", f.config.string()});
  expect(checked.exit_code == 0, "--check accepts absent-but-creatable logs/cache roots: " + checked.err);
  expect(secure_frontend_test::snapshot_tree(f.dir) == before, "--check created, wrote, renamed or removed nothing");
  expect(!std::filesystem::exists(f.dir / "logs") && !std::filesystem::exists(f.dir / "cache"), "--check created no root");
  secure_frontend_test::ChildProcess server;
  expect(f.start(server, {"--config", f.config.string()}), "serve starts: " + server.stderr_text());
  expect(std::filesystem::is_directory(f.dir / "logs") && std::filesystem::is_directory(f.dir / "cache"),
         "serve creates both roots before it listens");
#if !defined(_WIN32)
  struct stat st {};
  stat((f.dir / "logs").c_str(), &st);
  expect((st.st_mode & 0777) == 0700, "serve creates them 0700");
#endif
}
```

Registration — `run_cases` entries `{"cfg02_create_within_root_writes_nothing_outside", …}` (paths), `{"cfg02_logs_and_cache_roots_are_created_private", …}` (typed config), `{"cli_check_has_no_side_effects", …}` (server CLI), and in CMake (all three open roots on Windows: Task 9 wine rule):

```cmake
    secure_frontend_add_cases(secure_frontend_paths_tests cfg02_create_within_root_writes_nothing_outside
        LABELS ${SECURE_FRONTEND_WINDOWS_RUNTIME})
    secure_frontend_add_cases(secure_frontend_typed_config_tests cfg02_logs_and_cache_roots_are_created_private
        LABELS ${SECURE_FRONTEND_WINDOWS_RUNTIME})
    secure_frontend_add_cases(secure_frontend_server_cli_tests cli_check_has_no_side_effects
        ${SECURE_FRONTEND_SERVER_ARGS} LABELS ${SECURE_FRONTEND_WINDOWS_RUNTIME} TIMEOUT 120)
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build --preset linux-gcc 2>&1 | grep -m1 error`
Expected: `error: 'create_directory_within' is not a member of 'secure_frontend'` (paths and typed-config tests; `create_runtime_roots` is missing too). The server-CLI binary builds: run `ctest --preset linux-gcc -R cli_check_has_no_side_effects --output-on-failure` → FAIL with `FAILED: --check accepts absent-but-creatable logs/cache roots: Configuration error: roots.logs: the root does not exist or cannot be resolved` (Task 9's `validate()` requires the roots to exist).

- [ ] **Step 3: Implement**

`canonical_path.hpp`: replace `enum class RootPolicy { existing };` with `enum class RootPolicy { existing, create_private };` and add after `open_executable`:

```cpp
// Creates every missing component of `path` below `root` with mode 0700
// (Linux) or a protected owner-only DACL (Windows), refusing links and
// non-directories on the way. Nothing is created outside `root`.
[[nodiscard]] std::optional<PathError> create_directory_within(const CanonicalRoot& root, const RelativePath& path);

// Side-effect free (used by validation and therefore --check): if `requested`
// exists it must open as a private root, and its canonical path is returned; if
// it does not exist, its parent must open as a private root and its last
// component must be one valid relative name, and canonical(parent)/name is
// returned. Nothing is created, written or held open.
[[nodiscard]] std::variant<std::filesystem::path, PathError> check_creatable_root(const std::filesystem::path& requested);
```

`canonical_path_common.cpp` — `open_root` becomes (the `create_private` branch is reached only from `serve` and `init`):

```cpp
std::variant<CanonicalRoot, PathError> open_root(const std::filesystem::path& requested, const RootPolicy policy) {
  if (policy == RootPolicy::create_private && requested.has_filename() && requested.has_parent_path()) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(requested, error);
    if (status.type() == std::filesystem::file_type::not_found) {
      auto parent = detail::open_existing_root(requested.parent_path());
      if (const auto* failure = std::get_if<PathError>(&parent)) return *failure;
      const auto name = parse_relative_path(path_to_utf8(requested.filename()));
      if (const auto* failure = std::get_if<PathError>(&name)) return *failure;
      if (auto failure = create_directory_within(std::get<CanonicalRoot>(parent), std::get<RelativePath>(name))) return *failure;
    }
  }
  return detail::open_existing_root(requested);
}

std::variant<std::filesystem::path, PathError> check_creatable_root(const std::filesystem::path& requested) {
  std::error_code error;
  const auto status = std::filesystem::symlink_status(requested, error);
  if (status.type() != std::filesystem::file_type::not_found) {
    auto existing = detail::open_existing_root(requested);  // also rejects a dangling link: it is not "absent"
    if (const auto* failure = std::get_if<PathError>(&existing)) return *failure;
    return std::get<CanonicalRoot>(existing).path();
  }
  if (!requested.is_absolute() || !requested.has_filename() || !requested.has_parent_path()) {
    return detail::path_error(PathErrorCode::not_absolute, "roots must be absolute paths naming a directory");
  }
  auto parent = detail::open_existing_root(requested.parent_path());
  if (const auto* failure = std::get_if<PathError>(&parent)) return *failure;
  const auto name = parse_relative_path(path_to_utf8(requested.filename()));
  if (const auto* failure = std::get_if<PathError>(&name)) return *failure;
  return std::get<CanonicalRoot>(parent).path() / requested.filename();
}
```

(`#include <system_error>`.) Both handles opened by `check_creatable_root` are closed when it returns.

`canonical_path_posix.cpp` — on this host a symlinked component opened with `O_DIRECTORY | O_NOFOLLOW` fails with `ENOTDIR`, not `ELOOP` (verified), so, as in Task 10's walk, an `ENOTDIR` (or `ELOOP`) is followed by an `fstatat(AT_SYMLINK_NOFOLLOW)` that tells a link from a file. The check runs before `current` (possibly `base`) is closed:

```cpp
std::optional<PathError> create_directory_within(const CanonicalRoot& root, const RelativePath& path) {
  using detail::path_error;
  int current = -1;
  for (const auto& part : path.components()) {
    const int base = current < 0 ? root.handle() : current;
    if (::mkdirat(base, part.c_str(), 0700) != 0 && errno != EEXIST) {
      if (current >= 0) ::close(current);
      return path_error(PathErrorCode::create_failed, "a directory could not be created");
    }
    const int next = ::openat(base, part.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    const int saved = errno;
    if (next < 0) {
      struct stat entry {};
      const bool is_link = (saved == ENOTDIR || saved == ELOOP) &&
                           ::fstatat(base, part.c_str(), &entry, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(entry.st_mode);
      if (current >= 0) ::close(current);
      if (is_link) return path_error(PathErrorCode::symlink_not_allowed, "a path component is a symbolic link");
      return saved == ENOTDIR ? path_error(PathErrorCode::not_directory, "a path component is not a directory")
                              : path_error(PathErrorCode::create_failed, "a directory could not be opened");
    }
    if (current >= 0) ::close(current);
    struct stat st {};
    if (::fstat(next, &st) != 0 || static_cast<std::uint64_t>(st.st_dev) != root.device_id()) {
      ::close(next);
      return path_error(PathErrorCode::cross_device, "the path crosses into another file system");
    }
    if (st.st_uid != ::geteuid() || (st.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
      ::close(next);
      return path_error(PathErrorCode::shared_writable, "an existing directory on the way is not private");
    }
    current = next;
  }
  if (current >= 0) ::close(current);
  return std::nullopt;
}
```

`canonical_path_windows.cpp`:

```cpp
std::optional<PathError> create_directory_within(const CanonicalRoot& root, const RelativePath& path) {
  using detail::path_error;
  ws::PrivateSecurityAttributes attributes(true);
  if (attributes.get() == nullptr) return path_error(PathErrorCode::create_failed, "a private security descriptor could not be built");
  std::wstring current = root.path().wstring();
  for (const auto& part : path.components()) {
    current += L'\\';
    current += path_from_utf8(part).wstring();
    if (CreateDirectoryW(ws::extended_path(current).c_str(), attributes.get()) == 0 && GetLastError() != ERROR_ALREADY_EXISTS) {
      return path_error(PathErrorCode::create_failed, "a directory could not be created");
    }
    HANDLE raw = CreateFileW(ws::extended_path(current).c_str(), FILE_READ_ATTRIBUTES,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                             FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (raw == INVALID_HANDLE_VALUE) return path_error(PathErrorCode::create_failed, "a directory could not be opened");
    OwnedHandle guard(raw);
    if (ws::is_reparse_point(raw)) return path_error(PathErrorCode::symlink_not_allowed, "a path component is a junction or link");
    FILE_ATTRIBUTE_TAG_INFO tag{};
    GetFileInformationByHandleEx(raw, FileAttributeTagInfo, &tag, sizeof(tag));
    if ((tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U) return path_error(PathErrorCode::not_directory, "a path component is not a directory");
  }
  return std::nullopt;
}
```

`frontend_config.hpp` — `Roots` becomes:

```cpp
struct Roots final {
  CanonicalRoot models;
  CanonicalRoot tokenizer_pack;
  std::optional<CanonicalRoot> secret;
  // Canonical paths that may not exist yet: validate() only checks them
  // (check_creatable_root); create_runtime_roots() creates them for serve/init.
  std::optional<std::filesystem::path> logs;
  std::optional<std::filesystem::path> cache;
};
```

and after `listener_config`:

```cpp
// The only filesystem side effect of a configuration: creates a missing
// roots.logs / roots.cache (0700 / private DACL) and checks an existing one.
// Called by serve and init, never by validate() or --check.
[[nodiscard]] ConfigErrors create_runtime_roots(const FrontendConfig& config);
```

`frontend_config.cpp` — add to the anonymous namespace, after `canonical_root`:

```cpp
// roots.logs / roots.cache: checked, never created, during validation.
std::optional<std::filesystem::path> creatable_root(DraftReader& read, ConfigErrors& errors, const std::string_view path) {
  const auto text = read.string(path);
  if (!text.has_value()) return std::nullopt;
  auto checked = check_creatable_root(path_from_utf8(*text));
  if (auto* error = std::get_if<PathError>(&checked)) {
    add(errors, std::string{path}, ConfigIssueCode::invalid_value, error->message);
    return std::nullopt;
  }
  return std::get<std::filesystem::path>(std::move(checked));
}
```

in `validate()` replace

```cpp
  const auto logs = canonical_root(read, errors, "roots.logs");
  const auto cache = canonical_root(read, errors, "roots.cache");
```

with

```cpp
  const auto logs = creatable_root(read, errors, "roots.logs");
  const auto cache = creatable_root(read, errors, "roots.cache");
```

in `describe()` add back a path lambda after `root_or_unset` and use it for the two lines:

```cpp
  const auto path_or_unset = [](const std::optional<std::filesystem::path>& p) {
    return p.has_value() ? quoted(path_to_utf8(*p)) : std::string{"(unset)"};
  };
```

```cpp
  line("roots.logs", path_or_unset(c.roots.logs));
  line("roots.cache", path_or_unset(c.roots.cache));
```

and append after `listener_config`:

```cpp
ConfigErrors create_runtime_roots(const FrontendConfig& c) {
  ConfigErrors errors;
  const std::pair<const char*, const std::optional<std::filesystem::path>*> roots[] = {
      {"roots.logs", &c.roots.logs}, {"roots.cache", &c.roots.cache}};
  for (const auto& [key, root] : roots) {
    if (!root->has_value()) continue;
    auto opened = open_root(**root, RootPolicy::create_private);
    if (const auto* failure = std::get_if<PathError>(&opened)) add(errors, key, ConfigIssueCode::invalid_value, failure->message);
  }
  return errors;
}
```

(`#include <utility>` for `std::pair`.)

`secure_server_main.cpp` — at the top of `run()`, before the token is read:

```cpp
  // roots.logs / roots.cache: validate() only checked them, so --check stays
  // side-effect free; serve creates a missing one here, before bind or spawn.
  if (const auto issues = secure_frontend::create_runtime_roots(config); !issues.empty()) {
    for (const auto& issue : issues) std::cerr << "Configuration error: " << secure_frontend::format_issue(issue) << '\n';
    return EXIT_FAILURE;
  }
```

(Task 21's `init_frontend` calls the same function after it writes the config; `check()` never calls it.)

`secret_source.hpp` lines 50-56: replace the comment with:

```cpp
// This accepts only a path in the canonical-path module's relative grammar
// (parse_relative_path), below a caller-configured secret root; the token
// reader opens it with open_beneath.
```

- [ ] **Step 4: Run to verify it passes, then the P9 check**

Run: `cmake --build --preset linux-gcc && ctest --preset linux-gcc | tail -1` → `0 tests failed`.
Run (P9 — no second reserved-name list, no path-based canonicalization, no `symlink_status` check left in the five consumers):
`grep -nE '== "(CON|PRN|AUX|NUL)"|starts_with\("(COM|LPT)"\)|weakly_canonical|filesystem::canonical|filesystem::absolute|symlink_status|is_symlink|source_relative_path|is_below_or_equal' src/secret_source.cpp src/model_inventory.cpp src/engine_arguments.cpp src/tokenizer_pack.cpp src/token_reader.cpp`
Expected: no output. (Each now calls the module: `secret_source` → `parse_relative_path`; `model_inventory` → `parse_relative_path`, `is_windows_reserved_component`, `list_directory`, `open_root`; `engine_arguments` → `parse_relative_path`, `CanonicalRoot::join`; `tokenizer_pack` → `open_root`, `open_beneath`; `token_reader` → `open_root`, `open_beneath`.) Tasks 8–13 already deleted every helper this pattern matches (`source_relative_path` and `is_below_or_equal` in Task 10, the reserved-name helpers in Task 8, `symlink_status` in Task 13), so any output is a missed deletion from one of those tasks: delete the matched helper in the file the grep names, rebuild, and rerun this step before committing.
Run: `cmake --build --preset windows-llvm-mingw-cross | tail -1 && ctest --preset windows-llvm-mingw-cross | tail -1` → build ok; `0 tests failed` (the three new cases are `windows-runtime`); record.

- [ ] **Step 5: Commit**

```bash
git -C "$(git rev-parse --show-toplevel)" add secure_frontend
git -C "$(git rev-parse --show-toplevel)" -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "paths: create directories only within a root; private logs/cache roots; duplicate checks gone (SP1 P8, P9)" \
  -m "Regression tests: cfg02_create_within_root_writes_nothing_outside (a symlinked component reports
symlink_not_allowed via fstatat after ENOTDIR), cfg02_logs_and_cache_roots_are_created_private,
cli_check_has_no_side_effects (--check leaves the tree byte-for-byte unchanged; serve creates logs/cache).
validate() only checks logs/cache (check_creatable_root); serve and init create them (create_runtime_roots).
P9 grep over secret_source, model_inventory, engine_arguments, tokenizer_pack, token_reader: no reserved-name list,
canonicalization or symlink_status check remains outside the module." \
  -m "$SP1_TRAILERS"
```

---
### Task 15: `SecretBytes`, CSPRNG and base64url (K1; K6's type)

**Files:**
- Create: `secure_frontend/include/secure_frontend/secret_bytes.hpp`, `secure_frontend/src/secret_bytes.cpp`
- Create: `secure_frontend/include/secure_frontend/token_generator.hpp`, `secure_frontend/src/token_generator.cpp`
- Create: `secure_frontend/tests/secrets_test.cpp`
- Modify: `secure_frontend/CMakeLists.txt` (library `secure_frontend_secrets`; `bcrypt` in the `if(WIN32)` block)

**Interfaces:**
- Produces (Tasks 16–23):
```cpp
namespace secure_frontend {
inline constexpr std::size_t kTokenWindowBytes = 256U;
void secure_wipe(void* data, std::size_t size) noexcept;      // explicit_bzero / SecureZeroMemory
class SecretBytes final {                                      // move-only, fixed capacity, wiped on destroy and move-from
 public:
  static constexpr std::size_t kCapacity = kTokenWindowBytes;
  SecretBytes() noexcept = default;
  SecretBytes(SecretBytes&& other) noexcept;
  SecretBytes& operator=(SecretBytes&& other) noexcept;
  ~SecretBytes();
  [[nodiscard]] bool assign(std::string_view bytes) noexcept;  // false (and empty) if > kCapacity
  [[nodiscard]] bool append(std::string_view bytes) noexcept;
  void clear() noexcept;
  [[nodiscard]] std::string_view view() const noexcept;
  [[nodiscard]] std::size_t size() const noexcept;
  [[nodiscard]] bool empty() const noexcept;
  [[nodiscard]] const unsigned char* storage_for_tests() const noexcept;   // test hook (K6)
};
inline constexpr std::size_t kGeneratedTokenRandomBytes = 32U;
inline constexpr std::size_t kGeneratedTokenCharacters = 43U;
struct TokenGenerationError final { std::string message; };
[[nodiscard]] bool fill_random(unsigned char* buffer, std::size_t size) noexcept;   // getrandom / BCryptGenRandom; fails closed
[[nodiscard]] bool base64url_encode(std::string_view bytes, SecretBytes& out) noexcept;  // RFC 4648 §5, no padding
[[nodiscard]] std::variant<SecretBytes, TokenGenerationError> generate_token();
}
```

- [ ] **Step 1: Write the failing tests** (`tests/secrets_test.cpp`)

```cpp
#include "secure_frontend/secret_bytes.hpp"
#include "secure_frontend/token_generator.hpp"
#include "support/test_support.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <new>
#include <set>
#include <string>
#include <variant>

namespace {
int failures = 0;
void expect(const bool condition, const std::string& message) {
  if (!condition) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
}

std::string encode(const std::string& bytes) {
  secure_frontend::SecretBytes out;
  expect(secure_frontend::base64url_encode(bytes, out), "encode fits");
  return std::string{out.view()};
}

void base64url_known_vectors() {
  // RFC 4648 §10 vectors, unpadded, plus the two URL-safe characters.
  expect(encode("").empty(), "empty");
  expect(encode("f") == "Zg", "f");
  expect(encode("fo") == "Zm8", "fo");
  expect(encode("foo") == "Zm9v", "foo");
  expect(encode("foob") == "Zm9vYg", "foob");
  expect(encode("fooba") == "Zm9vYmE", "fooba");
  expect(encode("foobar") == "Zm9vYmFy", "foobar");
  expect(encode(std::string{"\xfb\xff"}) == "-_8", "uses - and _ instead of + and /");
}

void token_generate_is_43_base64url_chars() {
  const auto result = secure_frontend::generate_token();
  const auto* token = std::get_if<secure_frontend::SecretBytes>(&result);
  expect(token != nullptr, "the OS CSPRNG is available");
  if (token == nullptr) return;
  expect(token->size() == secure_frontend::kGeneratedTokenCharacters, "32 random bytes encode to 43 characters");
  const std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  expect(std::all_of(token->view().begin(), token->view().end(),
                     [&](const char c) { return alphabet.find(c) != std::string_view::npos; }),
         "only base64url characters, no padding, no prefix");
}

void token_generate_differs_across_calls() {
  std::set<std::string> seen;
  for (int i = 0; i < 32; ++i) {
    auto result = secure_frontend::generate_token();
    seen.insert(std::string{std::get<secure_frontend::SecretBytes>(result).view()});
  }
  expect(seen.size() == 32U, "32 generated tokens are all distinct");
}

bool all_zero(const unsigned char* bytes, const std::size_t size) {
  return std::all_of(bytes, bytes + size, [](const unsigned char b) { return b == 0U; });
}

void secret_bytes_wiped_on_destroy_and_move() {
  alignas(secure_frontend::SecretBytes) unsigned char storage[sizeof(secure_frontend::SecretBytes)];
  auto* secret = new (storage) secure_frontend::SecretBytes();
  expect(secret->assign("SENSITIVE-0123456789"), "assign fits");
  secure_frontend::SecretBytes moved = std::move(*secret);
  expect(moved.view() == "SENSITIVE-0123456789", "the value moves");
  expect(secret->empty() && all_zero(secret->storage_for_tests(), secure_frontend::SecretBytes::kCapacity),
         "a moved-from SecretBytes holds only zeros (no SSO remnant)");
  expect(secret->assign("THIRD-SECRET-VALUE"), "reassign");
  secret->~SecretBytes();
  const std::string_view marker = "THIRD-SECRET";
  expect(std::search(storage, storage + sizeof(storage), marker.begin(), marker.end()) == storage + sizeof(storage),
         "destroyed storage no longer holds the secret");
  moved.clear();
  expect(moved.empty() && all_zero(moved.storage_for_tests(), secure_frontend::SecretBytes::kCapacity), "clear wipes");
}

void secret_bytes_rejects_over_capacity() {
  secure_frontend::SecretBytes secret;
  expect(!secret.assign(std::string(secure_frontend::SecretBytes::kCapacity + 1U, 'x')) && secret.empty(),
         "a value over 256 bytes is refused, not truncated");
  expect(secret.assign(std::string(secure_frontend::SecretBytes::kCapacity, 'x')), "exactly 256 bytes fits");
}
}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"base64url_known_vectors", base64url_known_vectors},
      {"token_generate_is_43_base64url_chars", token_generate_is_43_base64url_chars},
      {"token_generate_differs_across_calls", token_generate_differs_across_calls},
      {"secret_bytes_wiped_on_destroy_and_move", secret_bytes_wiped_on_destroy_and_move},
      {"secret_bytes_rejects_over_capacity", secret_bytes_rejects_over_capacity},
  }, failures);
}
```

CMake:

```cmake
    secure_frontend_add_test_executable(secure_frontend_secrets_tests tests/secrets_test.cpp secure_frontend_secrets)
    secure_frontend_add_cases(secure_frontend_secrets_tests
        base64url_known_vectors token_generate_is_43_base64url_chars token_generate_differs_across_calls
        secret_bytes_wiped_on_destroy_and_move secret_bytes_rejects_over_capacity)
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --preset linux-gcc && cmake --build --preset linux-gcc 2>&1 | grep -m1 error`
Expected: `fatal error: secure_frontend/secret_bytes.hpp: No such file or directory`.

- [ ] **Step 3: Implement**

`include/secure_frontend/secret_bytes.hpp`:

```cpp
#pragma once

#include <array>
#include <cstddef>
#include <string_view>

namespace secure_frontend {

inline constexpr std::size_t kTokenWindowBytes = 256U;

// Overwrites memory in a way the optimizer may not remove.
void secure_wipe(void* data, std::size_t size) noexcept;

// Holds a token or other secret inline (never on the heap, so no allocator
// copy survives), move-only, wiped on destruction and on move-from.
class SecretBytes final {
 public:
  static constexpr std::size_t kCapacity = kTokenWindowBytes;

  SecretBytes() noexcept = default;
  SecretBytes(const SecretBytes&) = delete;
  SecretBytes& operator=(const SecretBytes&) = delete;
  SecretBytes(SecretBytes&& other) noexcept;
  SecretBytes& operator=(SecretBytes&& other) noexcept;
  ~SecretBytes();

  [[nodiscard]] bool assign(std::string_view bytes) noexcept;
  [[nodiscard]] bool append(std::string_view bytes) noexcept;
  void clear() noexcept;
  [[nodiscard]] std::string_view view() const noexcept;
  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] bool empty() const noexcept { return size_ == 0U; }
  // Test hook: the inline storage, so a test can prove it holds only zeros.
  [[nodiscard]] const unsigned char* storage_for_tests() const noexcept { return bytes_.data(); }

 private:
  std::array<unsigned char, kCapacity> bytes_{};
  std::size_t size_{0U};
};

}  // namespace secure_frontend
```

`src/secret_bytes.cpp`:

```cpp
#include "secure_frontend/secret_bytes.hpp"

#include <cstring>

#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"
#else
#include <string.h>
#endif

namespace secure_frontend {

void secure_wipe(void* const data, const std::size_t size) noexcept {
  if (data == nullptr || size == 0U) return;
#if defined(_WIN32)
  SecureZeroMemory(data, size);
#elif defined(__GLIBC__)
  explicit_bzero(data, size);
#else
  volatile unsigned char* bytes = static_cast<volatile unsigned char*>(data);
  for (std::size_t index = 0; index < size; ++index) bytes[index] = 0U;
#endif
}

SecretBytes::SecretBytes(SecretBytes&& other) noexcept : size_(other.size_) {
  std::memcpy(bytes_.data(), other.bytes_.data(), other.size_);
  other.clear();
}

SecretBytes& SecretBytes::operator=(SecretBytes&& other) noexcept {
  if (this != &other) {
    clear();
    std::memcpy(bytes_.data(), other.bytes_.data(), other.size_);
    size_ = other.size_;
    other.clear();
  }
  return *this;
}

SecretBytes::~SecretBytes() { clear(); }

bool SecretBytes::assign(const std::string_view bytes) noexcept {
  clear();
  return append(bytes);
}

bool SecretBytes::append(const std::string_view bytes) noexcept {
  if (bytes.size() > kCapacity - size_) return false;
  if (!bytes.empty()) std::memcpy(bytes_.data() + size_, bytes.data(), bytes.size());
  size_ += bytes.size();
  return true;
}

void SecretBytes::clear() noexcept {
  secure_wipe(bytes_.data(), bytes_.size());
  size_ = 0U;
}

std::string_view SecretBytes::view() const noexcept {
  return std::string_view{reinterpret_cast<const char*>(bytes_.data()), size_};
}

}  // namespace secure_frontend
```

`include/secure_frontend/token_generator.hpp`:

```cpp
#pragma once

#include "secure_frontend/secret_bytes.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <variant>

namespace secure_frontend {

inline constexpr std::size_t kGeneratedTokenRandomBytes = 32U;
inline constexpr std::size_t kGeneratedTokenCharacters = 43U;

struct TokenGenerationError final {
  std::string message;
};

// Linux: getrandom(buf, n, 0), looping on EINTR and short reads; any other
// failure (ENOSYS included) fails closed -- there is no /dev/urandom fallback.
// Windows: BCryptGenRandom(nullptr, ..., BCRYPT_USE_SYSTEM_PREFERRED_RNG).
[[nodiscard]] bool fill_random(unsigned char* buffer, std::size_t size) noexcept;
// RFC 4648 §5 alphabet, no padding. False if the output would exceed SecretBytes.
[[nodiscard]] bool base64url_encode(std::string_view bytes, SecretBytes& out) noexcept;
// 32 CSPRNG bytes, base64url: 43 ASCII characters, no prefix.
[[nodiscard]] std::variant<SecretBytes, TokenGenerationError> generate_token();

}  // namespace secure_frontend
```

`src/token_generator.cpp`:

```cpp
#include "secure_frontend/token_generator.hpp"

#include <array>
#include <cerrno>

#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"
#include <bcrypt.h>
#else
#include <sys/random.h>
#endif

namespace secure_frontend {

bool fill_random(unsigned char* const buffer, const std::size_t size) noexcept {
#if defined(_WIN32)
  return BCryptGenRandom(nullptr, buffer, static_cast<ULONG>(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0;
#else
  std::size_t filled = 0U;
  while (filled < size) {
    const ssize_t count = ::getrandom(buffer + filled, size - filled, 0);
    if (count < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    filled += static_cast<std::size_t>(count);
  }
  return true;
#endif
}

bool base64url_encode(const std::string_view bytes, SecretBytes& out) noexcept {
  static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  out.clear();
  std::array<char, 4> quad{};
  const auto at = [&bytes](const std::size_t i) { return static_cast<unsigned>(static_cast<unsigned char>(bytes[i])); };
  std::size_t i = 0U;
  bool ok = true;
  for (; ok && i + 3U <= bytes.size(); i += 3U) {
    const unsigned v = (at(i) << 16U) | (at(i + 1U) << 8U) | at(i + 2U);
    quad = {kAlphabet[(v >> 18U) & 63U], kAlphabet[(v >> 12U) & 63U], kAlphabet[(v >> 6U) & 63U], kAlphabet[v & 63U]};
    ok = out.append(std::string_view{quad.data(), 4U});
  }
  const std::size_t rest = bytes.size() - i;
  if (ok && rest == 1U) {
    const unsigned v = at(i) << 16U;
    quad = {kAlphabet[(v >> 18U) & 63U], kAlphabet[(v >> 12U) & 63U], 0, 0};
    ok = out.append(std::string_view{quad.data(), 2U});
  } else if (ok && rest == 2U) {
    const unsigned v = (at(i) << 16U) | (at(i + 1U) << 8U);
    quad = {kAlphabet[(v >> 18U) & 63U], kAlphabet[(v >> 12U) & 63U], kAlphabet[(v >> 6U) & 63U], 0};
    ok = out.append(std::string_view{quad.data(), 3U});
  }
  secure_wipe(quad.data(), quad.size());
  if (!ok) out.clear();
  return ok;
}

std::variant<SecretBytes, TokenGenerationError> generate_token() {
  std::array<unsigned char, kGeneratedTokenRandomBytes> raw{};
  const bool filled = fill_random(raw.data(), raw.size());
  SecretBytes token;
  const bool encoded =
      filled && base64url_encode(std::string_view{reinterpret_cast<const char*>(raw.data()), raw.size()}, token);
  secure_wipe(raw.data(), raw.size());
  if (!encoded) return TokenGenerationError{"the operating system random number generator is unavailable"};
  return token;
}

}  // namespace secure_frontend
```

CMake:

```cmake
add_library(secure_frontend_secrets src/secret_bytes.cpp src/token_generator.cpp)
target_compile_features(secure_frontend_secrets PUBLIC cxx_std_20)
target_include_directories(secure_frontend_secrets PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)
```

in the `if(WIN32)` block: `target_link_libraries(secure_frontend_secrets PUBLIC bcrypt)`; and `secure_frontend_secrets` in the warnings `foreach`.

- [ ] **Step 4: Run to verify it passes**

Run: `cmake --build --preset linux-gcc && ctest --preset linux-gcc -R secrets_tests --output-on-failure | tail -3 && ctest --preset linux-gcc | tail -1` → five cases pass; `0 tests failed`.
Run: `cmake --build --preset windows-llvm-mingw-cross | tail -1 && ctest --preset windows-llvm-mingw-cross -R secrets_tests | tail -1` → build ok; record the wine result (`BCryptGenRandom` is implemented by wine).

- [ ] **Step 5: Commit**

```bash
git -C "$(git rev-parse --show-toplevel)" add secure_frontend
git -C "$(git rev-parse --show-toplevel)" -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "secrets: SecretBytes, CSPRNG token generator and base64url (SP1 K1, K6 type)" \
  -m "Regression tests: base64url_known_vectors, token_generate_is_43_base64url_chars,
token_generate_differs_across_calls, secret_bytes_wiped_on_destroy_and_move, secret_bytes_rejects_over_capacity.
getrandom fails closed (no /dev/urandom fallback); Windows links bcrypt." \
  -m "$SP1_TRAILERS"
```

---

### Task 16: Atomic owner-only writes and the Linux token store (K2)

**Files:**
- Create: `secure_frontend/include/secure_frontend/token_store.hpp`, `secure_frontend/src/token_store.cpp`, `secure_frontend/tests/token_store_test.cpp`
- Modify: `secure_frontend/CMakeLists.txt` (library `secure_frontend_token_store`)

**Interfaces:**
- Consumes: `CanonicalRoot`, `RelativePath`, `open_beneath` (Tasks 9–10); `SecretBytes`, `fill_random` (Task 15).
- Produces (Tasks 17, 21):
```cpp
namespace secure_frontend {
enum class TokenStoreMode { create_new, replace };
enum class TokenStoreErrorCode { already_exists, unsafe_location, write_failed, protect_failed };
struct TokenStoreError final { TokenStoreErrorCode code; std::string message; };
// Same-directory temp file (O_CREAT|O_EXCL|O_NOFOLLOW, 0600 / private DACL), write, fsync,
// rename (renameat2 RENAME_NOREPLACE or link+unlink for create_new; renameat for replace), fsync(dir).
[[nodiscard]] std::optional<TokenStoreError> write_private_file(const CanonicalRoot& root, const RelativePath& file,
                                                                std::string_view bytes, TokenStoreMode mode);
// Linux: token + one '\n'. Windows (Task 17): "STRATAK1" + CryptProtectData(token).
[[nodiscard]] std::optional<TokenStoreError> store_token(const CanonicalRoot& secret_root, const RelativePath& file,
                                                         const SecretBytes& token, TokenStoreMode mode);
}
```

- [ ] **Step 1: Write the failing tests** (`tests/token_store_test.cpp`)

```cpp
#include "secure_frontend/token_generator.hpp"
#include "secure_frontend/token_store.hpp"
#include "support/test_support.hpp"

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <variant>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace {
int failures = 0;
// The helpers are [[maybe_unused]]: on Windows this task registers no case (Task 17
// adds the Windows ones), and -Werror would otherwise reject the unused functions.
[[maybe_unused]] void expect(const bool condition, const std::string& message) {
  if (!condition) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
}

[[maybe_unused]] secure_frontend::CanonicalRoot root_at(const std::filesystem::path& path) {
  return std::get<secure_frontend::CanonicalRoot>(secure_frontend::open_root(path));
}
[[maybe_unused]] secure_frontend::RelativePath rel(const char* text) {
  return std::get<secure_frontend::RelativePath>(secure_frontend::parse_relative_path(text));
}
[[maybe_unused]] secure_frontend::SecretBytes fresh_token() {
  return std::get<secure_frontend::SecretBytes>(secure_frontend::generate_token());
}

#if !defined(_WIN32)
void token_store_creates_0600_file_in_0700_dir() {
  const auto dir = secure_frontend_test::make_scratch_directory();
  const auto token = fresh_token();
  expect(!secure_frontend::store_token(root_at(dir), rel("token"), token, secure_frontend::TokenStoreMode::create_new),
         "a token is stored in a 0700 directory");
  struct stat st {};
  stat((dir / "token").c_str(), &st);
  expect((st.st_mode & 0777) == 0600 && st.st_uid == geteuid(), "the file is 0600 and owned by the user");
  expect(secure_frontend_test::read_file(dir / "token") == std::string{token.view()} + "\n", "token plus one LF");
  std::size_t entries = 0;
  for ([[maybe_unused]] const auto& entry : std::filesystem::directory_iterator(dir)) ++entries;
  expect(entries == 1U, "no temporary file is left behind");
  const auto again = secure_frontend::store_token(root_at(dir), rel("token"), fresh_token(), secure_frontend::TokenStoreMode::create_new);
  expect(again.has_value() && again->code == secure_frontend::TokenStoreErrorCode::already_exists, "create_new never overwrites");
  const auto shared = secure_frontend_test::make_scratch_directory();
  chmod(shared.c_str(), 0750);
  const auto unsafe = secure_frontend::store_token(root_at(shared), rel("token"), token, secure_frontend::TokenStoreMode::create_new);
  expect(unsafe.has_value() && unsafe->code == secure_frontend::TokenStoreErrorCode::unsafe_location,
         "a directory with group access is refused");
}

void token_store_rotate_is_atomic() {
  const auto dir = secure_frontend_test::make_scratch_directory();
  const auto first = fresh_token();
  const auto second = fresh_token();
  const std::string a = std::string{first.view()} + "\n";
  const std::string b = std::string{second.view()} + "\n";
  expect(!secure_frontend::store_token(root_at(dir), rel("token"), first, secure_frontend::TokenStoreMode::create_new), "initial");
  std::atomic<bool> stop{false};
  std::atomic<int> torn{0};
  std::thread reader([&] {
    while (!stop) {
      const std::string seen = secure_frontend_test::read_file(dir / "token");
      if (seen != a && seen != b) ++torn;
    }
  });
  for (int i = 0; i < 200; ++i) {
    (void)secure_frontend::store_token(root_at(dir), rel("token"), i % 2 == 0 ? second : first, secure_frontend::TokenStoreMode::replace);
  }
  stop = true;
  reader.join();
  expect(torn == 0, "a concurrent reader always sees the old or the new token, never a partial file");
}

void token_store_refuses_symlinked_target() {
  const auto dir = secure_frontend_test::make_scratch_directory();
  const auto outside = secure_frontend_test::make_scratch_directory();
  secure_frontend_test::write_file(outside / "victim", "unchanged");
  std::filesystem::create_symlink(outside / "victim", dir / "token");
  const auto result = secure_frontend::store_token(root_at(dir), rel("token"), fresh_token(), secure_frontend::TokenStoreMode::replace);
  expect(result.has_value() && result->code == secure_frontend::TokenStoreErrorCode::unsafe_location, "a symlinked target is refused");
  expect(secure_frontend_test::read_file(outside / "victim") == "unchanged", "the link target is untouched");
  expect(std::filesystem::is_symlink(dir / "token"), "the symlink itself is not replaced either");
}
#endif
}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
#if !defined(_WIN32)
      {"token_store_creates_0600_file_in_0700_dir", token_store_creates_0600_file_in_0700_dir},
      {"token_store_rotate_is_atomic", token_store_rotate_is_atomic},
      {"token_store_refuses_symlinked_target", token_store_refuses_symlinked_target},
#endif
  }, failures);
}
```

CMake:

```cmake
    secure_frontend_add_test_executable(secure_frontend_token_store_tests tests/token_store_test.cpp
        secure_frontend_token_store)
    if(NOT WIN32)
        secure_frontend_add_cases(secure_frontend_token_store_tests
            token_store_creates_0600_file_in_0700_dir token_store_rotate_is_atomic token_store_refuses_symlinked_target)
    endif()
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --preset linux-gcc && cmake --build --preset linux-gcc 2>&1 | grep -m1 error`
Expected: `fatal error: secure_frontend/token_store.hpp: No such file or directory`.

- [ ] **Step 3: Implement**

`include/secure_frontend/token_store.hpp`:

```cpp
#pragma once

#include "secure_frontend/canonical_path.hpp"
#include "secure_frontend/secret_bytes.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace secure_frontend {

enum class TokenStoreMode { create_new, replace };
enum class TokenStoreErrorCode { already_exists, unsafe_location, write_failed, protect_failed };

// Fixed text; never the path or the token.
struct TokenStoreError final {
  TokenStoreErrorCode code;
  std::string message;
};

// Writes `bytes` to root/file so a reader sees the old or the new content,
// never a partial file. The directory holding the file must be owned by the
// user with no group/other access (Linux) or carry a private owner DACL
// (Windows); an existing target must be a regular file (a symlink is refused).
[[nodiscard]] std::optional<TokenStoreError> write_private_file(const CanonicalRoot& root, const RelativePath& file,
                                                                std::string_view bytes, TokenStoreMode mode);

[[nodiscard]] std::optional<TokenStoreError> store_token(const CanonicalRoot& secret_root, const RelativePath& file,
                                                         const SecretBytes& token, TokenStoreMode mode);

}  // namespace secure_frontend
```

`src/token_store.cpp`:

```cpp
#include "secure_frontend/token_store.hpp"

#include "secure_frontend/token_generator.hpp"

#include <cerrno>
#include <cstdio>
#include <string>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace secure_frontend {
namespace {

TokenStoreError fail(const TokenStoreErrorCode code, const char* message) { return TokenStoreError{code, message}; }

std::string random_suffix() {
  unsigned char nonce[8] = {};
  if (!fill_random(nonce, sizeof(nonce))) return {};
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  for (const unsigned char b : nonce) { out.push_back(kHex[b >> 4U]); out.push_back(kHex[b & 15U]); }
  return out;
}

#if !defined(_WIN32)
int publish_without_replacing(const int dir, const char* temp, const char* name) {
#if defined(RENAME_NOREPLACE)
  if (::renameat2(dir, temp, dir, name, RENAME_NOREPLACE) == 0) return 0;
  if (errno != ENOSYS && errno != EINVAL) return -1;
#endif
  if (::linkat(dir, temp, dir, name, 0) != 0) return -1;  // EEXIST if the name is taken
  ::unlinkat(dir, temp, 0);
  return 0;
}
#endif

}  // namespace

#if !defined(_WIN32)
std::optional<TokenStoreError> write_private_file(const CanonicalRoot& root, const RelativePath& file,
                                                  const std::string_view bytes, const TokenStoreMode mode) {
  const auto& parts = file.components();
  int dir = -1;
  if (parts.size() == 1U) {
    dir = ::openat(root.handle(), ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  } else {
    std::string parent;
    for (std::size_t i = 0; i + 1U < parts.size(); ++i) parent += (parent.empty() ? "" : "/") + parts[i];
    auto opened = open_beneath(root, std::get<RelativePath>(parse_relative_path(parent)), OpenOptions{.kind = OpenKind::directory});
    if (std::holds_alternative<PathError>(opened)) return fail(TokenStoreErrorCode::unsafe_location, "the directory for the file cannot be opened safely");
    dir = ::fcntl(std::get<OpenedFile>(opened).native(), F_DUPFD_CLOEXEC, 0);
  }
  if (dir < 0) return fail(TokenStoreErrorCode::write_failed, "the directory for the file cannot be opened");
  OwnedHandle dir_guard(dir);
  struct stat ds {};
  if (::fstat(dir, &ds) != 0 || ds.st_uid != ::geteuid() || (ds.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
    return fail(TokenStoreErrorCode::unsafe_location, "the directory holding the file must be owned by you with mode 0700");
  }
  const std::string& name = parts.back();
  struct stat existing {};
  if (::fstatat(dir, name.c_str(), &existing, AT_SYMLINK_NOFOLLOW) == 0) {
    if (!S_ISREG(existing.st_mode)) return fail(TokenStoreErrorCode::unsafe_location, "the target exists and is not a regular file");
    if (mode == TokenStoreMode::create_new) return fail(TokenStoreErrorCode::already_exists, "the file already exists");
  }
  const std::string suffix = random_suffix();
  if (suffix.empty()) return fail(TokenStoreErrorCode::write_failed, "no randomness for the temporary file name");
  const std::string temp = "." + name + ".tmp-" + suffix;
  const int fd = ::openat(dir, temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0) return fail(TokenStoreErrorCode::write_failed, "the temporary file cannot be created");
  bool ok = ::fchmod(fd, 0600) == 0;
  std::size_t written = 0U;
  while (ok && written < bytes.size()) {
    const ssize_t count = ::write(fd, bytes.data() + written, bytes.size() - written);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) ok = false;
    else written += static_cast<std::size_t>(count);
  }
  ok = ok && ::fsync(fd) == 0;
  ok = ::close(fd) == 0 && ok;
  if (!ok) {
    ::unlinkat(dir, temp.c_str(), 0);
    return fail(TokenStoreErrorCode::write_failed, "the file could not be written");
  }
  const int published = mode == TokenStoreMode::create_new ? publish_without_replacing(dir, temp.c_str(), name.c_str())
                                                           : ::renameat(dir, temp.c_str(), dir, name.c_str());
  const int saved = errno;
  if (published != 0) {
    ::unlinkat(dir, temp.c_str(), 0);
    return fail(saved == EEXIST ? TokenStoreErrorCode::already_exists : TokenStoreErrorCode::write_failed,
                "the file could not be put in place");
  }
  (void)::fsync(dir);
  return std::nullopt;
}

std::optional<TokenStoreError> store_token(const CanonicalRoot& secret_root, const RelativePath& file,
                                           const SecretBytes& token, const TokenStoreMode mode) {
  SecretBytes content;
  if (!content.assign(token.view()) || !content.append("\n")) return fail(TokenStoreErrorCode::write_failed, "the token is too long");
  return write_private_file(secret_root, file, content.view(), mode);
}
#endif

}  // namespace secure_frontend
```

CMake:

```cmake
add_library(secure_frontend_token_store src/token_store.cpp)
target_compile_features(secure_frontend_token_store PUBLIC cxx_std_20)
target_include_directories(secure_frontend_token_store PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)
target_link_libraries(secure_frontend_token_store PUBLIC secure_frontend_paths secure_frontend_secrets)
```

and `secure_frontend_token_store` in the warnings `foreach`. (On Windows nothing calls the two functions until Task 17 adds their Windows definitions.)

- [ ] **Step 4: Run to verify it passes**

Run: `cmake --build --preset linux-gcc && ctest --preset linux-gcc -R token_store --output-on-failure | tail -3 && ctest --preset linux-gcc | tail -1` → three cases pass; `0 tests failed`.
Run: `cmake --build --preset windows-llvm-mingw-cross | tail -1` → build ok with `-Werror`: `secure_frontend_token_store_tests.exe` compiles with no case yet, because its four helpers are `[[maybe_unused]]` (without that, `-Wunused-function` fails the cross build here, before Task 17).

- [ ] **Step 5: Commit**

```bash
git -C "$(git rev-parse --show-toplevel)" add secure_frontend
git -C "$(git rev-parse --show-toplevel)" -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "token store: atomic owner-only writes (O_EXCL|O_NOFOLLOW temp, fsync, renameat2 NOREPLACE) on Linux (SP1 K2)" \
  -m "Regression tests: token_store_creates_0600_file_in_0700_dir, token_store_rotate_is_atomic,
token_store_refuses_symlinked_target." \
  -m "$SP1_TRAILERS"
```

---

### Task 17: Windows token store with DPAPI and a protected DACL; reader accepts DPAPI and plaintext (K3)

Windows-only behavior. RED/GREEN: cross-compile with `-Werror`, wine run recorded (DPAPI under wine may or may not round-trip), authoritative in `windows-msys2-clang` (label `windows-runtime`).

**Files:**
- Modify: `secure_frontend/src/token_store.cpp` (Windows `write_private_file`, `store_token`)
- Modify: `secure_frontend/include/secure_frontend/token_reader.hpp` (`kDpapiTokenMagic`), `secure_frontend/src/token_reader.cpp` (`read_protected_file` unwraps DPAPI)
- Modify: `secure_frontend/tests/token_store_test.cpp`, `secure_frontend/CMakeLists.txt` (`crypt32` for the token store and token reader; token store links the reader for its tests)

**Interfaces:**
- Consumes: `windows_security::*` (Task 9), `open_beneath` (Task 11), `write_private_file`/`store_token` declarations (Task 16).
- Produces: `inline constexpr std::string_view kDpapiTokenMagic = "STRATAK1";` in `token_reader.hpp`. File format on Windows: 8-byte magic + `CryptProtectData` blob (user scope, `CRYPTPROTECT_UI_FORBIDDEN`); the file is created `CREATE_NEW` with SDDL `O:<user>D:P(A;;FA;;;<user>)(A;;FA;;;SY)` and moved into place with `MoveFileExW(…, MOVEFILE_WRITE_THROUGH [| MOVEFILE_REPLACE_EXISTING])`. (`O:<user>` is added to the spec's SDDL so the owner is the user even for an elevated administrator, whose default owner is `Administrators` and would fail `has_private_owner_dacl`.)

- [ ] **Step 1: Write the failing tests** (append to `tests/token_store_test.cpp`; add `#include "secure_frontend/token_reader.hpp"`, and under `#if defined(_WIN32)` `#include <aclapi.h>`)

```cpp
#if defined(_WIN32)
std::filesystem::path protected_secret_dir() {
  const auto dir = secure_frontend_test::make_scratch_directory() / "secret";
  secure_frontend_test::make_protected_directory(dir);
  return dir;
}

void token_store_windows_round_trips_through_dpapi() {
  const auto dir = protected_secret_dir();
  const auto token = fresh_token();
  const auto stored = secure_frontend::store_token(root_at(dir), rel("token"), token, secure_frontend::TokenStoreMode::create_new);
  expect(!stored.has_value(), "the token is stored: " + (stored ? stored->message : std::string{}));
  const std::string raw = secure_frontend_test::read_file(dir / "token");
  expect(raw.rfind("STRATAK1", 0) == 0, "the file starts with the STRATAK1 magic");
  expect(raw.find(std::string{token.view()}) == std::string::npos, "the token is not stored in plaintext");
  const auto source = std::get<secure_frontend::SecretSource>(secure_frontend::make_protected_file_secret_source("token"));
  const auto read = secure_frontend::read_token(source, dir);
  const auto* error = std::get_if<secure_frontend::TokenReadError>(&read);
  expect(error == nullptr, "the DPAPI file reads back: " + (error ? error->message : std::string{}));
}

void token_store_windows_file_has_private_dacl() {
  const auto dir = protected_secret_dir();
  expect(!secure_frontend::store_token(root_at(dir), rel("token"), fresh_token(), secure_frontend::TokenStoreMode::create_new),
         "stored");
  PACL dacl = nullptr;
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  expect(GetNamedSecurityInfoW((dir / "token").c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl,
                               nullptr, &descriptor) == ERROR_SUCCESS, "security info is readable");
  SECURITY_DESCRIPTOR_CONTROL control = 0;
  DWORD revision = 0;
  GetSecurityDescriptorControl(descriptor, &control, &revision);
  expect((control & SE_DACL_PROTECTED) != 0, "the DACL is protected (nothing inherited)");
  expect(dacl != nullptr && dacl->AceCount == 2, "exactly two ACEs: the user and SYSTEM");
  LocalFree(descriptor);
}

void token_reader_windows_accepts_plaintext_under_private_dacl() {
  const auto dir = protected_secret_dir();
  const std::string token = "operator-token-0123456789abcdefghijklmnop";
  secure_frontend_test::write_protected_file(dir / "token", token);
  const auto source = std::get<secure_frontend::SecretSource>(secure_frontend::make_protected_file_secret_source("token"));
  const auto read = secure_frontend::read_token(source, dir);
  expect(!std::holds_alternative<secure_frontend::TokenReadError>(read), "an operator-made plaintext file is accepted");
}
#endif
```

`run_cases` (under `#if defined(_WIN32)`) and CMake:

```cmake
    target_link_libraries(secure_frontend_token_store_tests PRIVATE secure_frontend_token_reader)
    if(WIN32)
        secure_frontend_add_cases(secure_frontend_token_store_tests
            token_store_windows_round_trips_through_dpapi token_store_windows_file_has_private_dacl
            token_reader_windows_accepts_plaintext_under_private_dacl
            LABELS windows-runtime)
    endif()
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build --preset windows-llvm-mingw-cross 2>&1 | grep -m1 -E 'error|undefined'`
Expected: `undefined symbol: secure_frontend::store_token(...)` (no Windows definition). Record.

- [ ] **Step 3: Implement**

`token_store.cpp` — add `#include "secure_frontend/token_reader.hpp"` and `#include <array>`, and under `#if defined(_WIN32)` the includes `#include "windows_security.hpp"` (same `src/` directory), `<wincrypt.h>`, `<dpapi.h>`; then:

```cpp
#if defined(_WIN32)
namespace ws = windows_security;

std::optional<TokenStoreError> write_private_file(const CanonicalRoot& root, const RelativePath& file,
                                                  const std::string_view bytes, const TokenStoreMode mode) {
  std::wstring directory = root.path().wstring();
  const auto& parts = file.components();
  for (std::size_t i = 0; i + 1U < parts.size(); ++i) directory += L'\\' + path_from_utf8(parts[i]).wstring();
  HANDLE dir = CreateFileW(ws::extended_path(directory).c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (dir == INVALID_HANDLE_VALUE) return fail(TokenStoreErrorCode::write_failed, "the directory for the file cannot be opened");
  OwnedHandle dir_guard(dir);
  std::array<std::byte, SECURITY_MAX_SID_SIZE> storage{};
  PSID user = nullptr;
  if (!ws::current_user_sid(storage, &user) || ws::is_reparse_point(dir) || !ws::has_private_owner_dacl(dir, user)) {
    return fail(TokenStoreErrorCode::unsafe_location, "the directory holding the file must have a protected owner-only ACL");
  }
  const std::wstring target = directory + L'\\' + path_from_utf8(parts.back()).wstring();
  const DWORD attributes = GetFileAttributesW(ws::extended_path(target).c_str());
  if (attributes != INVALID_FILE_ATTRIBUTES) {
    if ((attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0U) {
      return fail(TokenStoreErrorCode::unsafe_location, "the target exists and is not a regular file");
    }
    if (mode == TokenStoreMode::create_new) return fail(TokenStoreErrorCode::already_exists, "the file already exists");
  }
  const std::string suffix = random_suffix();
  if (suffix.empty()) return fail(TokenStoreErrorCode::write_failed, "no randomness for the temporary file name");
  const std::wstring temp = directory + L"\\." + path_from_utf8(parts.back()).wstring() + L".tmp-" + path_from_utf8(suffix).wstring();
  ws::PrivateSecurityAttributes security(false);
  if (security.get() == nullptr) return fail(TokenStoreErrorCode::protect_failed, "a private security descriptor cannot be built");
  HANDLE out = CreateFileW(ws::extended_path(temp).c_str(), GENERIC_WRITE, 0, security.get(), CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
  if (out == INVALID_HANDLE_VALUE) return fail(TokenStoreErrorCode::write_failed, "the temporary file cannot be created");
  DWORD written = 0;
  bool ok = bytes.empty() || (WriteFile(out, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) != 0 &&
                              written == bytes.size());
  ok = FlushFileBuffers(out) != 0 && ok;
  CloseHandle(out);
  if (!ok) {
    DeleteFileW(ws::extended_path(temp).c_str());
    return fail(TokenStoreErrorCode::write_failed, "the file could not be written");
  }
  const DWORD flags = MOVEFILE_WRITE_THROUGH | (mode == TokenStoreMode::replace ? MOVEFILE_REPLACE_EXISTING : 0U);
  if (MoveFileExW(ws::extended_path(temp).c_str(), ws::extended_path(target).c_str(), flags) == 0) {
    const DWORD code = GetLastError();
    DeleteFileW(ws::extended_path(temp).c_str());
    return fail(code == ERROR_ALREADY_EXISTS || code == ERROR_FILE_EXISTS ? TokenStoreErrorCode::already_exists
                                                                          : TokenStoreErrorCode::write_failed,
                "the file could not be put in place");
  }
  return std::nullopt;
}

std::optional<TokenStoreError> store_token(const CanonicalRoot& secret_root, const RelativePath& file,
                                           const SecretBytes& token, const TokenStoreMode mode) {
  DATA_BLOB input{static_cast<DWORD>(token.size()),
                  reinterpret_cast<BYTE*>(const_cast<char*>(token.view().data()))};
  DATA_BLOB output{};
  if (CryptProtectData(&input, L"strata token", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output) == 0) {
    return fail(TokenStoreErrorCode::protect_failed, "DPAPI could not protect the token");
  }
  std::string content{kDpapiTokenMagic};
  content.append(reinterpret_cast<const char*>(output.pbData), output.cbData);
  SecureZeroMemory(output.pbData, output.cbData);
  LocalFree(output.pbData);
  return write_private_file(secret_root, file, content, mode);
}
#endif
```

`token_reader.hpp`: add `inline constexpr std::string_view kDpapiTokenMagic = "STRATAK1";` with the comment "Windows token files written by store_token: this magic followed by a CryptProtectData blob (user scope). Plaintext files under a private DACL are accepted too (operator-made)." `token_reader.cpp`, in `read_protected_file`, after `std::string token = …;`:

```cpp
#if defined(_WIN32)
  if (token.rfind(kDpapiTokenMagic, 0) == 0) {
    DATA_BLOB input{static_cast<DWORD>(token.size() - kDpapiTokenMagic.size()),
                    reinterpret_cast<BYTE*>(token.data() + kDpapiTokenMagic.size())};
    DATA_BLOB output{};
    const BOOL ok = CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output);
    secure_wipe(token.data(), token.size());
    if (ok == 0) return error(TokenReadErrorCode::invalid_token, "The token file could not be decrypted for the current user.");
    token.assign(reinterpret_cast<const char*>(output.pbData), output.cbData);
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
  }
#endif
```

(`#include "secure_frontend/secret_bytes.hpp"`; Windows includes `<wincrypt.h>`, `<dpapi.h>`; the token reader links `secure_frontend_secrets`.) CMake `if(WIN32)` block: `target_link_libraries(secure_frontend_token_store PUBLIC crypt32 advapi32)` and `target_link_libraries(secure_frontend_token_reader PUBLIC crypt32)`; plus `target_link_libraries(secure_frontend_token_reader PUBLIC secure_frontend_secrets)` and `target_link_libraries(secure_frontend_token_store PUBLIC secure_frontend_token_reader)` for every platform.

- [ ] **Step 4: Run to verify it passes**

Run: `cmake --build --preset linux-gcc && ctest --preset linux-gcc | tail -1` → `0 tests failed` (Linux unchanged).
Run: `cmake --build --preset windows-llvm-mingw-cross | tail -1` → links with `-Werror`.
Run (record only): `WINEPREFIX=$PWD/out/wineprefix WINEDLLOVERRIDES=mscoree,mshtml= ctest --test-dir out/build/windows-llvm-mingw-cross -L windows-runtime -R token_store --output-on-failure 2>&1 | tail -6` → copy the three lines into the commit body ("wine, not authoritative"). The MSYS2 job is the evidence.

- [ ] **Step 5: Commit**

```bash
git -C "$(git rev-parse --show-toplevel)" add secure_frontend
git -C "$(git rev-parse --show-toplevel)" -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "token store(windows): DPAPI-wrapped STRATAK1 file with a protected owner-only DACL; reader unwraps DPAPI (SP1 K3)" \
  -m "Windows-runtime tests (pending windows-msys2-clang): token_store_windows_round_trips_through_dpapi,
token_store_windows_file_has_private_dacl, token_reader_windows_accepts_plaintext_under_private_dacl.
Links crypt32. wine (not authoritative): <record>." \
  -m "$SP1_TRAILERS"
```

---

### Task 18: Token reader everywhere; one trailing newline; `SecretBytes` result; 32-byte floor (K4, K5, K6 buffers; defect c)

**Files:**
- Modify: `secure_frontend/include/secure_frontend/token_reader.hpp` (`TokenReadResult` holds `SecretBytes`; `token_too_short`; limits)
- Modify: `secure_frontend/src/token_reader.cpp` (format rule, wiped buffers, env path)
- Replace: `secure_frontend/tests/token_reader_test.cpp` (portable / Linux / Windows split)
- Modify: `secure_frontend/CMakeLists.txt:283-291, 304-306` (un-gate; corrected comment; labels)
- Modify: `secure_frontend/src/secure_server_main.cpp`, `secure_frontend/src/smoke_server_main.cpp:166-172`, `secure_frontend/src/smoke_main.cpp:51-53` (consume `SecretBytes`)

**Interfaces:**
- Consumes: `SecretBytes`, `secure_wipe` (Task 15); `OpenedFile::read_some` (Tasks 10–11).
- Produces (Tasks 19–23):
```cpp
namespace secure_frontend {
inline constexpr std::size_t kMaximumTokenFileBytes = 4096U;
inline constexpr std::size_t kMaximumTokenBytes = 256U;        // == kTokenWindowBytes
inline constexpr std::size_t kMinimumOperatorTokenBytes = 32U;
enum class TokenReadErrorCode { environment_variable_not_set, empty_token, invalid_token, secret_root_unavailable,
  token_not_found, path_escapes_root, symlink_not_allowed, not_regular_file, insecure_file_permissions,
  file_not_owned_by_current_user, read_failed, token_too_large, hard_link_not_allowed, token_too_short };
using TokenReadResult = std::variant<SecretBytes, TokenReadError>;
// Format rule (both OSes): 1-256 printable ASCII bytes 0x21-0x7E; a file may end in exactly one "\n" or "\r\n";
// BOM, whitespace, a second newline, or a token wrapped in quotes is invalid_token with a reason; < 32 bytes is token_too_short.
[[nodiscard]] std::variant<SecretBytes, TokenReadError> token_from_bytes(std::string_view raw, bool allow_trailing_newline);
}
```

- [ ] **Step 1: Write the failing tests** — replace `tests/token_reader_test.cpp` with:

```cpp
#include "secure_frontend/secret_source.hpp"
#include "secure_frontend/token_reader.hpp"
#include "support/test_support.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <variant>

#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {
int failures = 0;
void expect(const bool condition, const std::string_view message) {
  if (!condition) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
}
using secure_frontend::TokenReadErrorCode;
const std::string kToken = "file-token-0123456789abcdefghijklmnopqrstuv";  // 43 bytes

void set_variable(const char* name, const std::string& value) {
#if defined(_WIN32)
  _putenv_s(name, value.c_str());
#else
  setenv(name, value.c_str(), 1);
#endif
}
void unset_variable(const char* name) {
#if defined(_WIN32)
  _putenv_s(name, "");
#else
  unsetenv(name);
#endif
}

// A secret root the reader accepts: 0700 (POSIX) or a protected owner-only DACL (Windows).
std::filesystem::path secret_root() {
#if defined(_WIN32)
  const auto root = secure_frontend_test::make_scratch_directory() / "secret";
  secure_frontend_test::make_protected_directory(root);
  return root;
#else
  return secure_frontend_test::make_scratch_directory();
#endif
}
void write_token_file(const std::filesystem::path& path, const std::string_view bytes) {
#if defined(_WIN32)
  secure_frontend_test::write_protected_file(path, bytes);
#else
  secure_frontend_test::write_file(path, bytes, 0600);
#endif
}
secure_frontend::TokenReadResult read_file_source(const std::filesystem::path& root, const std::string_view path) {
  const auto source = std::get<secure_frontend::SecretSource>(secure_frontend::make_protected_file_secret_source(path));
  return secure_frontend::read_token(source, root);
}
secure_frontend::TokenReadResult read_env(const char* name) {
  const auto source = std::get<secure_frontend::SecretSource>(secure_frontend::make_environment_secret_source(name));
  return secure_frontend::read_token(source, std::filesystem::path{"."});
}
bool is_token(const secure_frontend::TokenReadResult& result, const std::string_view expected) {
  const auto* token = std::get_if<secure_frontend::SecretBytes>(&result);
  return token != nullptr && token->view() == expected;
}
bool is_error(const secure_frontend::TokenReadResult& result, const TokenReadErrorCode code) {
  const auto* error = std::get_if<secure_frontend::TokenReadError>(&result);
  return error != nullptr && error->code == code;
}

// ---- portable (environment) ------------------------------------------------
void reads_the_named_environment_variable() {
  set_variable("SF_TOKEN_READER_TEST", kToken);
  expect(is_token(read_env("SF_TOKEN_READER_TEST"), kToken), "environment source should be read");
  unset_variable("SF_TOKEN_READER_TEST");
}

void rejects_control_characters_in_environment_token() {
  set_variable("SF_TOKEN_READER_TEST", "bad\tvalue-0123456789abcdefghijklmnop");
  expect(is_error(read_env("SF_TOKEN_READER_TEST"), TokenReadErrorCode::invalid_token), "control byte in env token");
  unset_variable("SF_TOKEN_READER_TEST");
}

void auth02_short_operator_token_rejected() {
  set_variable("SF_TOKEN_READER_TEST", std::string(31, 'a'));
  expect(is_error(read_env("SF_TOKEN_READER_TEST"), TokenReadErrorCode::token_too_short), "31 bytes is too short");
  set_variable("SF_TOKEN_READER_TEST", std::string(32, 'a'));
  expect(is_token(read_env("SF_TOKEN_READER_TEST"), std::string(32, 'a')), "32 bytes is the floor");
  unset_variable("SF_TOKEN_READER_TEST");
}

// ---- protected files (Linux; windows-runtime on Windows) -------------------
void reads_an_owner_only_regular_file_below_the_root() {
  const auto root = secret_root();
  write_token_file(root / "token", kToken);
  expect(is_token(read_file_source(root, "token"), kToken), "owner-only regular file should be read");
}

void rejects_control_characters_in_file_token() {
  const auto root = secret_root();
  write_token_file(root / "token", "file-token\x01" + kToken);
  expect(is_error(read_file_source(root, "token"), TokenReadErrorCode::invalid_token), "control byte in file token");
}

void auth02_token_file_with_trailing_lf_accepted() {
  const auto root = secret_root();
  write_token_file(root / "token", kToken + "\n");
  expect(is_token(read_file_source(root, "token"), kToken), "one trailing LF is stripped (defect c)");
}

void auth02_token_file_with_trailing_crlf_accepted() {
  const auto root = secret_root();
  write_token_file(root / "token", kToken + "\r\n");
  expect(is_token(read_file_source(root, "token"), kToken), "one trailing CRLF is stripped");
}

void auth02_token_file_with_two_newlines_rejected() {
  const auto root = secret_root();
  write_token_file(root / "token", kToken + "\n\n");
  expect(is_error(read_file_source(root, "token"), TokenReadErrorCode::invalid_token), "two newlines are rejected");
}

void auth02_token_file_with_bom_rejected() {
  const auto root = secret_root();
  write_token_file(root / "token", "\xEF\xBB\xBF" + kToken);
  expect(is_error(read_file_source(root, "token"), TokenReadErrorCode::invalid_token), "a UTF-8 BOM is rejected");
}

void auth02_token_file_over_4k_rejected() {
  const auto root = secret_root();
  write_token_file(root / "token", std::string(4097, 'a'));
  expect(is_error(read_file_source(root, "token"), TokenReadErrorCode::token_too_large), "a file over 4 KiB is rejected");
  write_token_file(root / "long", std::string(257, 'a'));
  expect(is_error(read_file_source(root, "long"), TokenReadErrorCode::token_too_large), "a token over 256 bytes is rejected");
}

// Review Focus 3: what Notepad and copy-paste do to a token file.
void auth02_notepad_edits_rejected_with_reason() {
  const auto root = secret_root();
  const std::pair<std::string, const char*> cases[] = {
      {"\xEF\xBB\xBF" + kToken, "byte order mark"}, {kToken + " ", "whitespace"}, {kToken + " \r\n", "whitespace"},
      {"\"" + kToken + "\"", "quotes"},               {kToken + "\r\n\r\n", "newline"}};
  int index = 0;
  for (const auto& [bytes, reason] : cases) {
    const std::string name = "token" + std::to_string(index++);
    write_token_file(root / name, bytes);
    const auto result = read_file_source(root, name);
    const auto* error = std::get_if<secure_frontend::TokenReadError>(&result);
    expect(error != nullptr && error->code == TokenReadErrorCode::invalid_token &&
               error->message.find(reason) != std::string::npos,
           std::string{"Notepad-style edit must be rejected naming the "} + reason);
  }
  write_token_file(root / "ok", kToken + "\r\n");
  expect(is_token(read_file_source(root, "ok"), kToken), "Notepad's single CRLF is still fine");
}

#if !defined(_WIN32)
// ---- POSIX-only file semantics ----------------------------------------------
void rejects_a_group_or_world_accessible_file() {
  const auto root = secret_root();
  secure_frontend_test::write_file(root / "token", kToken, 0640);
  expect(is_error(read_file_source(root, "token"), TokenReadErrorCode::insecure_file_permissions), "group-readable token");
}

void rejects_a_symlink_that_escapes_the_root() {
  const auto root = secret_root();
  const auto outside = secret_root();
  write_token_file(outside / "token", kToken);
  std::filesystem::create_symlink(outside / "token", root / "token");
  expect(is_error(read_file_source(root, "token"), TokenReadErrorCode::symlink_not_allowed),
         "a symlinked token file is rejected, never followed");
}

void rejects_a_non_regular_file() {
  const auto root = secret_root();
  mkfifo((root / "token").c_str(), 0600);
  expect(is_error(read_file_source(root, "token"), TokenReadErrorCode::not_regular_file), "FIFO rejected before any read");
}

void cfg03_rejects_hardlinked_token_file() {
  const auto root = secret_root();
  write_token_file(root / "token", kToken);
  expect(link((root / "token").c_str(), (root / "second-name").c_str()) == 0, "test setup: link");
  expect(is_error(read_file_source(root, "token"), TokenReadErrorCode::hard_link_not_allowed),
         "a token file with a second hard link must be rejected");
}
#endif
}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"reads_the_named_environment_variable", reads_the_named_environment_variable},
      {"rejects_control_characters_in_environment_token", rejects_control_characters_in_environment_token},
      {"auth02_short_operator_token_rejected", auth02_short_operator_token_rejected},
      {"reads_an_owner_only_regular_file_below_the_root", reads_an_owner_only_regular_file_below_the_root},
      {"rejects_control_characters_in_file_token", rejects_control_characters_in_file_token},
      {"auth02_token_file_with_trailing_lf_accepted", auth02_token_file_with_trailing_lf_accepted},
      {"auth02_token_file_with_trailing_crlf_accepted", auth02_token_file_with_trailing_crlf_accepted},
      {"auth02_token_file_with_two_newlines_rejected", auth02_token_file_with_two_newlines_rejected},
      {"auth02_token_file_with_bom_rejected", auth02_token_file_with_bom_rejected},
      {"auth02_token_file_over_4k_rejected", auth02_token_file_over_4k_rejected},
      {"auth02_notepad_edits_rejected_with_reason", auth02_notepad_edits_rejected_with_reason},
#if !defined(_WIN32)
      {"rejects_a_group_or_world_accessible_file", rejects_a_group_or_world_accessible_file},
      {"rejects_a_symlink_that_escapes_the_root", rejects_a_symlink_that_escapes_the_root},
      {"rejects_a_non_regular_file", rejects_a_non_regular_file},
      {"cfg03_rejects_hardlinked_token_file", cfg03_rejects_hardlinked_token_file},
#endif
  }, failures);
}
```

CMake — replace the `if(UNIX) … endif()` around `secure_frontend_token_reader_tests` (Task 2's block) with:

```cmake
    # Built and run on every platform (SP1 K4). Protected-file cases need real
    # Windows ACL semantics: windows-runtime there, run by the windows-msys2-clang
    # job and excluded under wine.
    secure_frontend_add_test_executable(secure_frontend_token_reader_tests tests/token_reader_test.cpp
        secure_frontend_token_reader)
    secure_frontend_add_cases(secure_frontend_token_reader_tests
        reads_the_named_environment_variable rejects_control_characters_in_environment_token
        auth02_short_operator_token_rejected)
    secure_frontend_add_cases(secure_frontend_token_reader_tests
        reads_an_owner_only_regular_file_below_the_root rejects_control_characters_in_file_token
        auth02_token_file_with_trailing_lf_accepted auth02_token_file_with_trailing_crlf_accepted
        auth02_token_file_with_two_newlines_rejected auth02_token_file_with_bom_rejected
        auth02_token_file_over_4k_rejected auth02_notepad_edits_rejected_with_reason
        LABELS ${SECURE_FRONTEND_WINDOWS_RUNTIME})
    if(NOT WIN32)
        secure_frontend_add_cases(secure_frontend_token_reader_tests
            rejects_a_group_or_world_accessible_file rejects_a_symlink_that_escapes_the_root rejects_a_non_regular_file
            cfg03_rejects_hardlinked_token_file)
    endif()
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --preset linux-gcc && cmake --build --preset linux-gcc 2>&1 | grep -m1 error`
Expected: `error: 'token_too_short' is not a member of 'secure_frontend::TokenReadErrorCode'` (and `SecretBytes` not held by `TokenReadResult`). Behaviors pinned: today a trailing LF is `invalid_token` (defect c), a 31-byte token is accepted, a 1 MiB file is read.

- [ ] **Step 3: Implement**

`token_reader.hpp`: `#include "secure_frontend/secret_bytes.hpp"`; add the three limits and `token_too_short` (Interfaces); `using TokenReadResult = std::variant<SecretBytes, TokenReadError>;`; declare `token_from_bytes`; the comment above `TokenReadResult` becomes "The token is held in SecretBytes: inline, move-only, wiped on destruction and move-from. Every read buffer is wiped before it goes out of scope."

`token_reader.cpp`: delete `kMaximumTokenBytes` (line 23) and `contains_control_character` (lines 29-34); add in the anonymous namespace:

```cpp
// A stack buffer that is wiped when it leaves scope, whatever the exit path.
struct WipedBuffer final {
  std::array<char, kMaximumTokenFileBytes + 1U> bytes{};
  ~WipedBuffer() { secure_wipe(bytes.data(), bytes.size()); }
};
```

after it (outside the anonymous namespace):

```cpp
std::variant<SecretBytes, TokenReadError> token_from_bytes(const std::string_view raw, const bool allow_trailing_newline) {
  if (raw.size() >= 3U && raw.substr(0, 3) == "\xEF\xBB\xBF") {
    return error(TokenReadErrorCode::invalid_token, "The token file starts with a UTF-8 byte order mark; save it without one.");
  }
  std::string_view value = raw;
  if (allow_trailing_newline) {
    if (value.size() >= 2U && value.substr(value.size() - 2U) == "\r\n") value.remove_suffix(2U);
    else if (!value.empty() && value.back() == '\n') value.remove_suffix(1U);
  }
  if (value.empty()) return error(TokenReadErrorCode::empty_token, "The resolved token must not be empty.");
  if (value.size() > kMaximumTokenBytes) return error(TokenReadErrorCode::token_too_large, "The token is longer than 256 bytes.");
  for (const char c : value) {
    const auto b = static_cast<unsigned char>(c);
    if (b >= 0x21U && b <= 0x7eU) continue;
    if (c == ' ' || c == '\t') {
      return error(TokenReadErrorCode::invalid_token, "The token contains whitespace; only one trailing newline is allowed.");
    }
    if (c == '\n' || c == '\r') {
      return error(TokenReadErrorCode::invalid_token, "The token file has more than one trailing newline (or a line break inside the token).");
    }
    return error(TokenReadErrorCode::invalid_token, "The token must be printable ASCII.");
  }
  if (value.size() >= 2U && ((value.front() == '"' && value.back() == '"') || (value.front() == '\'' && value.back() == '\''))) {
    return error(TokenReadErrorCode::invalid_token, "The token is wrapped in quotes; remove them.");
  }
  if (value.size() < kMinimumOperatorTokenBytes) {
    return error(TokenReadErrorCode::token_too_short,
                 "Tokens must be at least 32 bytes; run strata-secure-server init to generate one.");
  }
  SecretBytes token;
  (void)token.assign(value);
  return token;
}
```

`read_protected_file` keeps the `open_beneath` call and replaces everything after it with:

```cpp
  const auto& file = std::get<OpenedFile>(opened);
  if (file.size() > kMaximumTokenFileBytes) return error(TokenReadErrorCode::token_too_large, "The token file is larger than 4 KiB.");
  WipedBuffer buffer;
  std::size_t used = 0U;
  while (used < buffer.bytes.size()) {
    auto got = file.read_some(buffer.bytes.data() + used, buffer.bytes.size() - used);
    if (std::holds_alternative<PathError>(got)) return error(TokenReadErrorCode::read_failed, "The protected token file could not be read.");
    const std::size_t count = std::get<std::size_t>(got);
    if (count == 0U) break;
    used += count;
  }
  if (used > kMaximumTokenFileBytes) return error(TokenReadErrorCode::token_too_large, "The token file is larger than 4 KiB.");
  const std::string_view raw{buffer.bytes.data(), used};
#if defined(_WIN32)
  if (raw.substr(0, kDpapiTokenMagic.size()) == kDpapiTokenMagic) {
    const std::string_view blob = raw.substr(kDpapiTokenMagic.size());
    DATA_BLOB input{static_cast<DWORD>(blob.size()), reinterpret_cast<BYTE*>(const_cast<char*>(blob.data()))};
    DATA_BLOB output{};
    if (CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output) == 0) {
      return error(TokenReadErrorCode::invalid_token, "The token file could not be decrypted for the current user.");
    }
    auto result = token_from_bytes(std::string_view{reinterpret_cast<const char*>(output.pbData), output.cbData}, false);
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return result;
  }
#endif
  return token_from_bytes(raw, true);
```

(this replaces Task 17's string-based DPAPI block). `read_environment_token` becomes:

```cpp
TokenReadResult read_environment_token(const SecretSource& source) {
  const char* const value = std::getenv(source.location().c_str());
  if (value == nullptr) return error(TokenReadErrorCode::environment_variable_not_set, "The configured token environment variable is not set.");
  return token_from_bytes(value, false);
}
```

Consumers of `TokenReadResult`:
- `secure_server_main.cpp` `run()`: `auto* token = std::get_if<secure_frontend::SecretBytes>(&token_result);` and, until Task 19 removes the local class, `auto verifier = std::make_shared<TokenVerifier>(std::string{token->view()});`.
- `smoke_server_main.cpp` lines 167-172: `auto* token = std::get_if<secure_frontend::SecretBytes>(&token_result);` … `std::make_shared<TokenVerifier>(std::string{token->view()})`.
- `smoke_main.cpp` lines 52-53 and 76: `const auto* token = std::get_if<secure_frontend::SecretBytes>(&token_result);` and the lambda captures `expected = std::string{token->view()}` (Task 19 removes this copy).

- [ ] **Step 4: Run to verify it passes**

Run: `cmake --build --preset linux-gcc && ctest --preset linux-gcc -R token_reader --output-on-failure | tail -3 && ctest --preset linux-gcc | tail -1` → all token-reader cases pass; `0 tests failed`. The e2e fixtures already use a 46-byte token.
Run: `cmake --build --preset windows-llvm-mingw-cross | tail -1 && ctest --preset windows-llvm-mingw-cross -R token_reader | tail -3`
Expected: the token-reader binary now builds for Windows; the three environment cases run and pass under wine; the eight file cases are excluded there (`windows-runtime`) and recorded as pending MSYS2.

- [ ] **Step 5: Commit**

```bash
git -C "$(git rev-parse --show-toplevel)" add secure_frontend
git -C "$(git rev-parse --show-toplevel)" -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "token reader: runs on every platform; one trailing LF/CRLF; SecretBytes result; 32-byte floor (SP1 K4, K5, K6; defect c)" \
  -m "Regression tests (failed before): auth02_token_file_with_trailing_lf_accepted, auth02_token_file_with_trailing_crlf_accepted,
auth02_short_operator_token_rejected, auth02_token_file_over_4k_rejected; plus auth02_token_file_with_two_newlines_rejected,
auth02_token_file_with_bom_rejected, auth02_notepad_edits_rejected_with_reason (Review Focus 3). token_reader_tests is
no longer UNIX-only; Windows file cases are windows-runtime (pending MSYS2). Read buffers are wiped." \
  -m "$SP1_TRAILERS"
```

---

### Task 19: One `TokenVerifier`, compared in a fixed 256-byte window (K6 verifier, K7; AUTH-03)

**Files:**
- Create: `secure_frontend/include/secure_frontend/token_verifier.hpp`, `secure_frontend/src/token_verifier.cpp`
- Modify: `secure_frontend/include/secure_frontend/secret_bytes.hpp`, `secure_frontend/src/secret_bytes.cpp` (`detail::fixed_window_equals`)
- Modify: `secure_frontend/include/secure_frontend/http_request_handler.hpp:32-36`, `secure_frontend/src/http_request_handler.cpp:168-177`
- Modify: `secure_frontend/src/secure_server_main.cpp`, `secure_frontend/src/smoke_server_main.cpp:141-154, 172, 207`, `secure_frontend/src/smoke_main.cpp:76-78`
- Modify: `secure_frontend/tests/secrets_test.cpp`, `secure_frontend/tests/http_request_handler_test.cpp`, `secure_frontend/CMakeLists.txt`

**Interfaces:**
- Consumes: `SecretBytes`, `secure_wipe`, `kTokenWindowBytes` (Task 15); `TokenReadResult` holding `SecretBytes` (Task 18).
- Produces:
```cpp
namespace secure_frontend {
namespace detail {
[[nodiscard]] bool fixed_window_equals(std::string_view presented, std::string_view stored, std::size_t* bytes_compared) noexcept;
}
class TokenVerifier final {
 public:
  explicit TokenVerifier(SecretBytes token) noexcept;
  [[nodiscard]] bool matches(std::string_view presented) const noexcept;
};
[[nodiscard]] std::function<bool(std::string_view)> make_bearer_validator(std::shared_ptr<const TokenVerifier> verifier);
}
```
  `constant_time_token_equals(candidate, expected)` keeps its signature and now calls `detail::fixed_window_equals`.

- [ ] **Step 1: Write the failing tests**

`tests/secrets_test.cpp` (add `#include "secure_frontend/token_verifier.hpp"`):

```cpp
void auth03_compare_touches_fixed_window() {
  const std::string stored(43, 's');
  for (const std::size_t length : {std::size_t{1}, std::size_t{43}, std::size_t{256}}) {
    std::size_t compared = 0;
    (void)secure_frontend::detail::fixed_window_equals(std::string(length, 'p'), stored, &compared);
    expect(compared == secure_frontend::kTokenWindowBytes,
           "every comparison touches all 256 bytes, input length " + std::to_string(length));
  }
  std::size_t compared = 99;
  expect(!secure_frontend::detail::fixed_window_equals(std::string(257, 'p'), stored, &compared) && compared == 0U,
         "a value over the public 256-byte maximum is rejected before comparing");
}

void auth03_prefix_and_suffix_variants_rejected() {
  const std::string stored = "auth03-token-0123456789abcdefghijklmnopqrs";
  secure_frontend::SecretBytes secret;
  expect(secret.assign(stored), "fits");
  const secure_frontend::TokenVerifier verifier(std::move(secret));
  expect(verifier.matches(stored), "the exact token matches");
  std::string first = stored; first.front() = 'X';
  std::string last = stored; last.back() = 'X';
  for (const std::string& variant : {stored.substr(0, stored.size() - 1), stored + "x", "x" + stored, first, last,
                                     std::string{}, stored + std::string(1, '\0')}) {
    expect(!verifier.matches(variant), "a prefix, suffix or one-byte variant must not match");
  }
  const secure_frontend::TokenVerifier empty{secure_frontend::SecretBytes{}};
  expect(!empty.matches(""), "an empty stored token never matches, not even an empty one");
}
```

registered as `auth03_compare_touches_fixed_window` and `auth03_prefix_and_suffix_variants_rejected`.

`tests/http_request_handler_test.cpp` (add `#include "secure_frontend/token_verifier.hpp"`, `#include <memory>`):

```cpp
void auth03_router_uses_verifier() {
  secure_frontend::SecretBytes secret;
  (void)secret.assign("router-token-0123456789abcdefghijklmnopqrs");
  const auto verifier = std::make_shared<const secure_frontend::TokenVerifier>(std::move(secret));
  const auto validator = secure_frontend::make_bearer_validator(verifier);
  bool dispatched = false;
  const secure_frontend::RequestDispatcher record = [&dispatched](std::string_view, std::string_view, std::string_view) {
    dispatched = true;
    return secure_frontend::HttpResponse{.status = 200, .headers = {}, .body = "ok"};
  };
  const auto request = [](const std::string& token) {
    return "GET /health HTTP/1.1\r\nHost: 127.0.0.1:8080\r\nAuthorization: Bearer " + token + "\r\n\r\n";
  };
  const auto good = secure_frontend::handle_http_request(request("router-token-0123456789abcdefghijklmnopqrs"), policy(),
                                                         validator, record);
  expect(good.status == 200 && dispatched, "the verifier's token is accepted through the handler");
  dispatched = false;
  const auto prefix = secure_frontend::handle_http_request(request("router-token-0123456789abcdefghijklmnopqr"), policy(),
                                                           validator, record);
  expect(prefix.status == 401 && !dispatched, "a prefix is rejected and the dispatcher never runs");
}
```

registered as `auth03_router_uses_verifier`.

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build --preset linux-gcc 2>&1 | grep -m1 error`
Expected: `fatal error: secure_frontend/token_verifier.hpp: No such file or directory`. (Today's `constant_time_token_equals` loops `max(len)` times — the length oracle `auth03_compare_touches_fixed_window` pins.)

- [ ] **Step 3: Implement**

`secret_bytes.hpp` — append:

```cpp
namespace detail {
// Copies both values into zero-padded 256-byte windows and XORs all 256
// bytes, folding in the length difference, so the work done is the same for
// every input up to the public maximum. A value longer than 256 bytes is
// rejected before comparing. An empty stored value never matches.
// *bytes_compared (test instrumentation) receives the number of bytes compared.
[[nodiscard]] bool fixed_window_equals(std::string_view presented, std::string_view stored,
                                       std::size_t* bytes_compared) noexcept;
}  // namespace detail
```

`secret_bytes.cpp` — append:

```cpp
namespace detail {
bool fixed_window_equals(const std::string_view presented, const std::string_view stored,
                         std::size_t* const bytes_compared) noexcept {
  if (bytes_compared != nullptr) *bytes_compared = 0U;
  if (presented.size() > kTokenWindowBytes || stored.size() > kTokenWindowBytes) return false;
  std::array<unsigned char, kTokenWindowBytes> left{};
  std::array<unsigned char, kTokenWindowBytes> right{};
  if (!presented.empty()) std::memcpy(left.data(), presented.data(), presented.size());
  if (!stored.empty()) std::memcpy(right.data(), stored.data(), stored.size());
  std::size_t difference = presented.size() ^ stored.size();
  std::size_t compared = 0U;
  for (std::size_t index = 0U; index < kTokenWindowBytes; ++index) {
    difference |= static_cast<std::size_t>(left[index] ^ right[index]);
    ++compared;
  }
  secure_wipe(left.data(), left.size());
  secure_wipe(right.data(), right.size());
  if (bytes_compared != nullptr) *bytes_compared = compared;
  return difference == 0U && !stored.empty();
}
}  // namespace detail
```

`include/secure_frontend/token_verifier.hpp`:

```cpp
#pragma once

#include "secure_frontend/secret_bytes.hpp"

#include <functional>
#include <memory>
#include <string_view>
#include <utility>

namespace secure_frontend {

// The one place the server's token lives after it is read (CFG-06: never in
// FrontendConfig). Comparison is detail::fixed_window_equals.
class TokenVerifier final {
 public:
  explicit TokenVerifier(SecretBytes token) noexcept : token_(std::move(token)) {}
  [[nodiscard]] bool matches(std::string_view presented) const noexcept;

 private:
  SecretBytes token_;
};

// A BearerTokenValidator for HttpSocketServer::bind that shares the verifier;
// the token is never copied into the closure.
[[nodiscard]] std::function<bool(std::string_view)> make_bearer_validator(std::shared_ptr<const TokenVerifier> verifier);

}  // namespace secure_frontend
```

`src/token_verifier.cpp`:

```cpp
#include "secure_frontend/token_verifier.hpp"

namespace secure_frontend {

bool TokenVerifier::matches(const std::string_view presented) const noexcept {
  return detail::fixed_window_equals(presented, token_.view(), nullptr);
}

std::function<bool(std::string_view)> make_bearer_validator(std::shared_ptr<const TokenVerifier> verifier) {
  return [verifier = std::move(verifier)](const std::string_view presented) {
    return verifier != nullptr && verifier->matches(presented);
  };
}

}  // namespace secure_frontend
```

`http_request_handler.hpp` lines 32-36 comment: "Fixed 256-byte window comparison (detail::fixed_window_equals): no early exit on content and no dependence on either length up to the public maximum." `http_request_handler.cpp` lines 168-177:

```cpp
bool constant_time_token_equals(std::string_view candidate, std::string_view expected) {
  return detail::fixed_window_equals(candidate, expected, nullptr);
}
```

(`#include "secure_frontend/secret_bytes.hpp"`; delete `#include <cstdint>` at line 4 — the replaced function was the file's only user of a fixed-width type; `<algorithm>` and `<limits>` stay, line 73 and the header parser use them.)

Mains:
- `secure_server_main.cpp`: delete the local `class TokenVerifier`; `#include "secure_frontend/token_verifier.hpp"`; `auto verifier = std::make_shared<const secure_frontend::TokenVerifier>(std::move(*token));`; `HttpSocketServer::bind(listener, std::move(policy), secure_frontend::make_bearer_validator(verifier), …)`.
- `smoke_server_main.cpp`: delete lines 141-154; the same two changes at lines 172 and 207.
- `smoke_main.cpp`: replace the lambda at lines 76-78 with `secure_frontend::make_bearer_validator(verifier)` where `const auto verifier = std::make_shared<const secure_frontend::TokenVerifier>(std::move(*token));` is declared after line 53 (`token` becomes non-const: `auto token_result = …; auto* token = std::get_if<secure_frontend::SecretBytes>(&token_result);`).

CMake: `target_sources(secure_frontend_secrets PRIVATE src/token_verifier.cpp)`; `target_link_libraries(secure_frontend_http_request_handler PUBLIC secure_frontend_secrets)`; the three executables link `secure_frontend_secrets` (already transitively through the handler).

- [ ] **Step 4: Run to verify it passes**

Run: `cmake --build --preset linux-gcc && ctest --preset linux-gcc | tail -1` → `0 tests failed` (the socket fixture's `"test-token"` validator still works through `constant_time_token_equals`).
Run: `grep -n "class TokenVerifier" src/*.cpp` → no output (one verifier, in the library).
Run: `cmake --build --preset windows-llvm-mingw-cross | tail -1 && ctest --preset windows-llvm-mingw-cross | tail -1` → build ok, `0 tests failed`.

- [ ] **Step 5: Commit**

```bash
git -C "$(git rev-parse --show-toplevel)" add secure_frontend
git -C "$(git rev-parse --show-toplevel)" -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "auth: one TokenVerifier in the library; fixed 256-byte window comparison without a length oracle (SP1 K6, K7)" \
  -m "Regression tests: auth03_compare_touches_fixed_window (failed before: work scaled with the longer input),
auth03_prefix_and_suffix_variants_rejected, auth03_router_uses_verifier. The two copies in the mains and the
smoke lambda's token copy are gone." \
  -m "$SP1_TRAILERS"
```

---

### Task 20: Scrub the environment token; no core dumps (K8, K9)

**Files:**
- Modify: `secure_frontend/src/token_reader.cpp` (`read_environment_token` wipes and unsets)
- Modify: `secure_frontend/include/secure_frontend/process_defaults.hpp`, `secure_frontend/src/process_defaults.cpp`
- Modify: `secure_frontend/tests/token_reader_test.cpp`, `secure_frontend/tests/server_e2e_test.cpp`, `secure_frontend/CMakeLists.txt`

**Interfaces:**
- Consumes: `secure_wipe` (Task 15); the e2e harness (Task 5).
- Produces: `read_token` with an environment source leaves the variable unset and its original bytes zeroed in the process environment block (Linux: the block `/proc/<pid>/environ` shows). Windows — accepted deviation from spec §3.6, owner-ruled: Windows has no `/proc/<pid>/environ` exposure and no API that overwrites a Win32 environment-block entry in place, so the reader wipes the CRT copies (narrow, and wide if present) and then removes the Win32 entry with `SetEnvironmentVariableW(name, nullptr)` — removed, not overwritten in place; Task 24 records this in the handoff. `install_process_defaults()` also sets `PR_SET_DUMPABLE 0` and `RLIMIT_CORE {0, 0}` (Linux/POSIX) and `SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX)` (Windows).

- [ ] **Step 1: Write the failing tests**

`tests/token_reader_test.cpp` (POSIX block; add `#include "support/process_runner.hpp"`):

```cpp
// K8: the child is started with the token in its *initial* environment block
// (what /proc/<pid>/environ shows), reads it, then inspects itself.
void cfg05_env_token_absent_from_proc_environ() {
  const auto& args = secure_frontend_test::extra_arguments();
  if (!args.empty() && args.front() == "--child") {
    expect(is_token(read_env("SF_ENV_TOKEN"), kToken), "the child reads the token");
    expect(std::getenv("SF_ENV_TOKEN") == nullptr, "the variable is unset after reading");
    const std::string environ_block = secure_frontend_test::read_file("/proc/self/environ");
    expect(environ_block.find(kToken) == std::string::npos, "/proc/self/environ no longer contains the token");
    return;
  }
  const auto self = std::filesystem::read_symlink("/proc/self/exe");
  const auto result = secure_frontend_test::run_to_completion(secure_frontend_test::ProcessSpec{
      self, {"--case", "cfg05_env_token_absent_from_proc_environ", "--child"}, {{"SF_ENV_TOKEN", kToken}}});
  expect(result.exit_code == 0, "child check failed: " + result.err);
}
```

registered in the POSIX `run_cases` block and the `if(NOT WIN32)` CMake list.

`tests/server_e2e_test.cpp`:

```cpp
#if !defined(_WIN32)
void server_process_is_not_dumpable() {
  ServerFixture f;
  secure_frontend_test::ChildProcess server;
  expect(f.start(server, f.base_flags()), "server starts: " + server.stderr_text());
  const std::string proc = "/proc/" + std::to_string(server.pid());
  const std::string limits = secure_frontend_test::read_file(proc + "/limits");
  const auto line_start = limits.find("Max core file size");
  std::istringstream fields(limits.substr(line_start == std::string::npos ? 0 : line_start + 18));
  std::string soft, hard;
  fields >> soft >> hard;
  expect(soft == "0" && hard == "0", "RLIMIT_CORE is 0/0, got " + soft + "/" + hard);
  if (geteuid() != 0) {
    // A non-dumpable process's /proc entries belong to root; its environ is unreadable to the same user.
    std::ifstream environ_file(proc + "/environ");
    std::string probe;
    const bool readable = environ_file.good() && static_cast<bool>(std::getline(environ_file, probe));
    expect(!readable, "PR_SET_DUMPABLE=0: /proc/<pid>/environ is not readable by the same user");
  }
}
#endif
```

(`#include <unistd.h>` under `!_WIN32`), registered under `if(NOT WIN32)` with `${SECURE_FRONTEND_SERVER_ARGS} TIMEOUT 120`.

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build --preset linux-gcc && ctest --preset linux-gcc -R 'cfg05_env_token_absent|not_dumpable' --output-on-failure`
Expected: FAIL — `FAILED: the variable is unset after reading`, `FAILED: /proc/self/environ no longer contains the token`, `FAILED: RLIMIT_CORE is 0/0, got unlimited/unlimited` (or the host default), `FAILED: PR_SET_DUMPABLE=0: …`.

- [ ] **Step 3: Implement**

`token_reader.cpp` — `read_environment_token`:

```cpp
TokenReadResult read_environment_token(const SecretSource& source) {
  const std::string& name = source.location();
  char* const value = std::getenv(name.c_str());
  if (value == nullptr) {
    return error(TokenReadErrorCode::environment_variable_not_set, "The configured token environment variable is not set.");
  }
  auto result = token_from_bytes(value, false);
  // Overwrite the value where it lives, then remove the variable, so
  // /proc/<pid>/environ and every later child lack it (CFG-05).
  secure_wipe(value, std::strlen(value));
#if defined(_WIN32)
  // Accepted deviation (spec §3.6): Windows has no /proc/<pid>/environ. The CRT
  // copies are wiped in place -- the narrow one above, the wide one here (the
  // name is ASCII by the env-name grammar) -- then the variable is removed from
  // the CRT and from the Win32 environment block. The block entry is removed,
  // not overwritten in place: no Win32 API can do that.
  const std::wstring wide_name(name.begin(), name.end());
  if (wchar_t* const wide = _wgetenv(wide_name.c_str())) secure_wipe(wide, std::wcslen(wide) * sizeof(wchar_t));
  (void)_putenv_s(name.c_str(), "");
  (void)SetEnvironmentVariableW(wide_name.c_str(), nullptr);
#else
  (void)::unsetenv(name.c_str());
#endif
  return result;
}
```

(`#include <cstdlib>`, `#include <cstring>`, `#include <cwchar>`, `#include <string>`; on Windows `#include "secure_frontend/win32_prelude.hpp"` for `SetEnvironmentVariableW`. `wide_name`'s byte-for-byte widening is exact because `make_environment_secret_source` accepts only `[A-Za-z_][A-Za-z0-9_]{0,127}`.)

`process_defaults.hpp` comment: "…and makes the process non-dumpable with a zero core-size limit (Linux), or suppresses the Windows error-reporting dialogs, so a crash cannot write a token to disk. Full Windows Error Reporting policy is SP7." `process_defaults.cpp`:

```cpp
#include "secure_frontend/process_defaults.hpp"

#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"
#else
#include <csignal>
#include <sys/resource.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif
#endif

namespace secure_frontend {

void install_process_defaults() noexcept {
#if defined(_WIN32)
  (void)SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#else
  struct sigaction action {};
  action.sa_handler = SIG_IGN;
  sigemptyset(&action.sa_mask);
  (void)sigaction(SIGPIPE, &action, nullptr);
  const rlimit no_core{0, 0};
  (void)setrlimit(RLIMIT_CORE, &no_core);
#if defined(__linux__)
  (void)prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
#endif
#endif
}

}  // namespace secure_frontend
```

- [ ] **Step 4: Run to verify it passes**

Run: `cmake --build --preset linux-gcc && ctest --preset linux-gcc | tail -1` → `0 tests failed`. (`net01_default_listener_is_loopback_only` reads only `/proc/net/tcp*`, which non-dumpability does not hide.)
Run: `cmake --build --preset windows-llvm-mingw-cross | tail -1 && ctest --preset windows-llvm-mingw-cross | tail -1` → build ok; record.

- [ ] **Step 5: Commit**

```bash
git -C "$(git rev-parse --show-toplevel)" add secure_frontend
git -C "$(git rev-parse --show-toplevel)" -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "secrets: wipe and unset the environment token after reading; non-dumpable, zero core limit (SP1 K8, K9)" \
  -m "Regression tests (failed before): cfg05_env_token_absent_from_proc_environ (re-exec with the token in the
initial environment block), server_process_is_not_dumpable (/proc/<pid>/limits and environ)." \
  -m "$SP1_TRAILERS"
```

---

### Task 21: `strata-secure-server init --config FILE [--force]` (K10; spec §3.9 mechanics)

**Files:**
- Create: `secure_frontend/include/secure_frontend/frontend_init.hpp`, `secure_frontend/src/frontend_init.cpp`
- Modify: `secure_frontend/include/secure_frontend/config_file.hpp`, `secure_frontend/src/config_file.cpp` (`render_config_document`)
- Modify: `secure_frontend/src/server_cli.cpp` (`init`, `--force`, usage), `secure_frontend/src/secure_server_main.cpp` (`init` command)
- Modify: `secure_frontend/tests/server_cli_test.cpp`, `secure_frontend/CMakeLists.txt`

**Interfaces:**
- Consumes: `validate`, `FrontendConfigDraft`, `ServerInvocation` (Tasks 4–5); `open_root(…, RootPolicy::create_private)`, `create_runtime_roots`, `parse_relative_path` (Tasks 8, 14); `write_private_file`, `store_token`, `TokenStoreMode` (Tasks 16–17); `generate_token` (Task 15).
- Produces:
```cpp
namespace secure_frontend {
// config_file.hpp: strict JSON, two-space indent, keys nested by section; values exactly as in the draft.
[[nodiscard]] std::string render_config_document(const FrontendConfigDraft& draft);
// frontend_init.hpp
enum class InitErrorCode { invalid_config, config_exists, token_exists, write_failed, token_failed };
struct InitError final { InitErrorCode code; std::string message; ConfigErrors issues; };
[[nodiscard]] std::optional<TokenStoreError> write_default_config(const CanonicalRoot& dir, const RelativePath& file,
                                                                  std::string_view json, bool force);
[[nodiscard]] std::optional<TokenStoreError> generate_token(const CanonicalRoot& secret_root, const RelativePath& file, bool force);
[[nodiscard]] std::optional<InitError> init_frontend(const std::filesystem::path& config_file,
                                                     const FrontendConfigDraft& flags, bool force);
}
```
  `init` writes the token first and the config second; with `force` both are replaced atomically, without it nothing is written if either exists. The token goes to `roots.secret` (default: the config file's directory) at `token.source` (default `file:token`); an `env:` source writes no token. Output never contains the token or a path.

- [ ] **Step 1: Write the failing tests** (`tests/server_cli_test.cpp`; add `#include "secure_frontend/token_reader.hpp"`; the binary links `secure_frontend_token_reader`)

```cpp
std::vector<std::string> init_flags(const ServerFixture& f, const std::filesystem::path& config) {
  return {"init", "--config", config.string(), "--models-root", f.models.string(), "--tokenizer-pack",
          f.tokenizer.string(), "--engine", f.engine.string(), "--model-id", "qwen-local", "--port", std::to_string(f.port)};
}

void init_writes_config_and_token_then_server_starts() {
  ServerFixture f;
  const auto config = f.dir / "fresh" / "config.json";
  const auto init = f.run(init_flags(f, config));
  expect(init.exit_code == 0, "init succeeds: " + init.err);
  expect(std::filesystem::exists(config) && std::filesystem::exists(f.dir / "fresh" / "token"), "config and token written");
  const auto source = std::get<secure_frontend::SecretSource>(secure_frontend::make_protected_file_secret_source("token"));
  auto token = secure_frontend::read_token(source, f.dir / "fresh");
  const auto* value = std::get_if<secure_frontend::SecretBytes>(&token);
  expect(value != nullptr && value->size() == 43U, "a generated 43-character token is readable");
  if (value == nullptr) return;
  expect(!contains(init.out, value->view()) && !contains(init.err, value->view()), "init never prints the token");
  secure_frontend_test::ChildProcess server;
  expect(f.start(server, {"--config", config.string()}), "the server starts from the written config alone: " + server.stderr_text());
  const auto response = f.request("GET", "/health", "Bearer " + std::string{value->view()});
  expect(response.rfind("HTTP/1.1 200", 0) == 0, "the generated token authenticates: " + response.substr(0, 16));
}

void init_refuses_to_overwrite_without_force() {
  ServerFixture f;
  const auto config = f.dir / "fresh" / "config.json";
  expect(f.run(init_flags(f, config)).exit_code == 0, "first init");
  const std::string config_before = secure_frontend_test::read_file(config);
  const std::string token_before = secure_frontend_test::read_file(f.dir / "fresh" / "token");
  const auto again = f.run(init_flags(f, config));
  expect(again.exit_code != 0 && contains(again.err, "already exists"), "a second init without --force fails: " + again.err);
  expect(secure_frontend_test::read_file(config) == config_before &&
             secure_frontend_test::read_file(f.dir / "fresh" / "token") == token_before,
         "nothing was changed");
  auto forced = init_flags(f, config);
  forced.insert(forced.begin() + 1, "--force");
  expect(f.run(forced).exit_code == 0, "--force succeeds");
  expect(secure_frontend_test::read_file(f.dir / "fresh" / "token") != token_before, "--force rotates the token");
}
```

CMake: `target_link_libraries(secure_frontend_server_cli_tests PRIVATE secure_frontend_token_reader)` and

```cmake
    secure_frontend_add_cases(secure_frontend_server_cli_tests
        init_writes_config_and_token_then_server_starts init_refuses_to_overwrite_without_force
        ${SECURE_FRONTEND_SERVER_ARGS} LABELS ${SECURE_FRONTEND_WINDOWS_RUNTIME} TIMEOUT 120)
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build --preset linux-gcc && ctest --preset linux-gcc -R 'server_cli_tests\.init_' --output-on-failure`
Expected: FAIL — `FAILED: init succeeds: Error: unexpected argument (not shown); every value must follow its option`.

- [ ] **Step 3: Implement**

`config_file.hpp` — add `[[nodiscard]] std::string render_config_document(const FrontendConfigDraft& draft);`. `config_file.cpp`:

```cpp
std::string render_config_document(const FrontendConfigDraft& draft) {
  struct Node {
    std::vector<std::string> names;
    std::vector<Node> children;
    std::string literal;
  };
  Node root;
  for (const auto& [path, entry] : draft.entries) {
    std::string literal;
    if (const auto* b = std::get_if<bool>(&entry.value)) literal = *b ? "true" : "false";
    else if (const auto* i = std::get_if<std::int64_t>(&entry.value)) literal = std::to_string(*i);
    else if (const auto* s = std::get_if<std::string>(&entry.value)) append_json_string(&literal, *s);
    else {
      literal = "[";
      for (const auto& item : std::get<std::vector<std::string>>(entry.value)) {
        if (literal.size() > 1U) literal += ", ";
        append_json_string(&literal, item);
      }
      literal += "]";
    }
    Node* node = &root;
    std::size_t start = 0;
    while (true) {
      const auto dot = path.find('.', start);
      const std::string name = path.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
      std::size_t index = 0;
      while (index < node->names.size() && node->names[index] != name) ++index;
      if (index == node->names.size()) { node->names.push_back(name); node->children.emplace_back(); }
      node = &node->children[index];
      if (dot == std::string::npos) break;
      start = dot + 1;
    }
    node->literal = literal;
  }
  std::string out;
  const std::function<void(const Node&, std::size_t)> emit = [&](const Node& node, const std::size_t depth) {
    out += "{\n";
    for (std::size_t i = 0; i < node.names.size(); ++i) {
      out.append(2U * (depth + 1U), ' ');
      append_json_string(&out, node.names[i]);
      out += ": ";
      if (node.children[i].names.empty()) out += node.children[i].literal;
      else emit(node.children[i], depth + 1U);
      out += i + 1U < node.names.size() ? ",\n" : "\n";
    }
    out.append(2U * depth, ' ');
    out += "}";
  };
  emit(root, 0U);
  return out + "\n";
}
```

(`#include <functional>`.)

`include/secure_frontend/frontend_init.hpp`:

```cpp
#pragma once

#include "secure_frontend/canonical_path.hpp"
#include "secure_frontend/frontend_config.hpp"
#include "secure_frontend/token_store.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace secure_frontend {

enum class InitErrorCode { invalid_config, config_exists, token_exists, write_failed, token_failed };

// message is fixed text; issues carries config validation problems by key.
struct InitError final {
  InitErrorCode code;
  std::string message;
  ConfigErrors issues;
};

// Both atomic and owner-only; both refuse to overwrite unless `force`.
[[nodiscard]] std::optional<TokenStoreError> write_default_config(const CanonicalRoot& dir, const RelativePath& file,
                                                                  std::string_view json, bool force);
[[nodiscard]] std::optional<TokenStoreError> generate_token(const CanonicalRoot& secret_root, const RelativePath& file,
                                                            bool force);

// The mechanics behind `strata-secure-server init` (spec §3.9). The UX --
// default locations, the strata wrapper, what is printed -- belongs to the
// product-experience spec.
[[nodiscard]] std::optional<InitError> init_frontend(const std::filesystem::path& config_file,
                                                     const FrontendConfigDraft& flags, bool force);

}  // namespace secure_frontend
```

`src/frontend_init.cpp`:

```cpp
#include "secure_frontend/frontend_init.hpp"

#include "secure_frontend/config_file.hpp"
#include "secure_frontend/token_generator.hpp"

#include <system_error>

namespace secure_frontend {

std::optional<TokenStoreError> write_default_config(const CanonicalRoot& dir, const RelativePath& file,
                                                    const std::string_view json, const bool force) {
  return write_private_file(dir, file, json, force ? TokenStoreMode::replace : TokenStoreMode::create_new);
}

std::optional<TokenStoreError> generate_token(const CanonicalRoot& secret_root, const RelativePath& file, const bool force) {
  auto generated = secure_frontend::generate_token();
  if (const auto* failure = std::get_if<TokenGenerationError>(&generated)) {
    return TokenStoreError{TokenStoreErrorCode::write_failed, failure->message};
  }
  return store_token(secret_root, file, std::get<SecretBytes>(generated),
                     force ? TokenStoreMode::replace : TokenStoreMode::create_new);
}

std::optional<InitError> init_frontend(const std::filesystem::path& config_file, const FrontendConfigDraft& flags,
                                       const bool force) {
  std::error_code ignored;
  const auto absolute = std::filesystem::absolute(config_file, ignored);
  auto directory = open_root(absolute.parent_path(), RootPolicy::create_private);
  if (const auto* failure = std::get_if<PathError>(&directory)) {
    return InitError{InitErrorCode::write_failed, "the config directory: " + failure->message, {}};
  }
  const auto& config_dir = std::get<CanonicalRoot>(directory);
  const auto config_name = parse_relative_path(path_to_utf8(absolute.filename()));
  if (!std::holds_alternative<RelativePath>(config_name)) {
    return InitError{InitErrorCode::invalid_config, "the config file name is not portable", {}};
  }
  FrontendConfigDraft draft = flags;
  draft.entries["schema_version"] = ConfigDraftEntry{std::int64_t{1}, ConfigSource::command_line};
  draft.config_directory = config_dir.path();
  auto validated = validate(draft);
  if (auto* issues = std::get_if<ConfigErrors>(&validated)) {
    return InitError{InitErrorCode::invalid_config, "the configuration is not valid", std::move(*issues)};
  }
  const FrontendConfig& config = std::get<ValidatedConfig>(validated).get();
  const bool needs_token = config.token.kind() == SecretSourceKind::protected_file;
  const RelativePath& config_rel = std::get<RelativePath>(config_name);
  const auto token_rel = parse_relative_path(config.token.location());
  if (!force) {
    if (std::filesystem::exists(std::filesystem::symlink_status(config_dir.join(config_rel), ignored))) {
      return InitError{InitErrorCode::config_exists, "the config file already exists; use --force to replace it", {}};
    }
    if (needs_token && std::filesystem::exists(std::filesystem::symlink_status(config.roots.secret->join(std::get<RelativePath>(token_rel)), ignored))) {
      return InitError{InitErrorCode::token_exists, "the token file already exists; use --force to rotate it", {}};
    }
  }
  if (needs_token) {
    if (auto failure = generate_token(*config.roots.secret, std::get<RelativePath>(token_rel), force)) {
      return InitError{failure->code == TokenStoreErrorCode::already_exists ? InitErrorCode::token_exists : InitErrorCode::token_failed,
                       "the token: " + failure->message, {}};
    }
  }
  FrontendConfigDraft written = flags;  // only what the operator chose, plus the version
  written.entries["schema_version"] = ConfigDraftEntry{std::int64_t{1}, ConfigSource::command_line};
  if (auto failure = write_default_config(config_dir, config_rel, render_config_document(written), force)) {
    return InitError{failure->code == TokenStoreErrorCode::already_exists ? InitErrorCode::config_exists : InitErrorCode::write_failed,
                     "the config file: " + failure->message, {}};
  }
  // init, like serve, creates the optional logs/cache roots (Task 14); validate() above only checked them.
  if (auto issues = create_runtime_roots(config); !issues.empty()) {
    return InitError{InitErrorCode::write_failed, "a runtime directory could not be created", std::move(issues)};
  }
  return std::nullopt;
}

}  // namespace secure_frontend
```

`server_cli.cpp`, at the start of `parse_server_command_line`:

```cpp
  int first = 1;
  if (argc > 1 && std::string_view{argv[1]} == "init") {
    invocation.command = ServerCommand::init;
    first = 2;
  }
  for (int index = first; index < argc; ++index) {
```

inside the loop, before the `--config` branch:

```cpp
    if (argument == "--force") {
      if (invocation.command != ServerCommand::init) return CliError{"--force is only valid with init"};
      if (invocation.force) return CliError{"duplicate --force"};
      invocation.force = true;
      continue;
    }
```

The `--check`/`init` rejection lives in the same loop, in the `--check` branch Task 5 wrote, which becomes:

```cpp
    if (argument == "--check") {
      if (invocation.command == ServerCommand::init) return CliError{"--check cannot be combined with init"};
      if (invocation.command == ServerCommand::check) return CliError{"duplicate --check"};
      invocation.command = ServerCommand::check;
      continue;
    }
```

(`init` is recognized only as `argv[1]`, so `--check init` is already rejected as an unexpected argument.) After the loop, replace `return invocation;` with:

```cpp
  if (invocation.command == ServerCommand::init && !invocation.config_file.has_value()) {
    return CliError{"init requires --config FILE"};
  }
  return invocation;
```

In `server_usage()` replace the first two usage lines with:

```cpp
      "Usage: strata-secure-server [--config FILE] [options]\n"
      "       strata-secure-server --check [--config FILE] [options]\n"
      "       strata-secure-server init --config FILE [--force] [options]\n"
```

and insert after the `"  --help                   Print this help\n"` line:

```cpp
      "\n"
      "init writes FILE (owner-only, atomic) from the options given and generates a\n"
      "43-character token at token.source below roots.secret (default: FILE's\n"
      "directory). It refuses to replace either without --force. The token is never\n"
      "printed; the product's `strata key show` will display it.\n"
```

`secure_server_main.cpp` (`#include "secure_frontend/frontend_init.hpp"`), in `main` before `load_server_config`:

```cpp
  if (invocation.command == secure_frontend::ServerCommand::init) {
    const auto failure = secure_frontend::init_frontend(*invocation.config_file, invocation.flags, invocation.force);
    if (failure.has_value()) {
      std::cerr << "init: " << failure->message << '\n';
      for (const auto& issue : failure->issues) std::cerr << "Configuration error: " << secure_frontend::format_issue(issue) << '\n';
      return EXIT_FAILURE;
    }
    std::cout << "init: wrote the configuration file and, for a file token source, a new token (not shown).\n";
    return EXIT_SUCCESS;
  }
```

CMake: `target_sources(secure_frontend_server_cli PRIVATE src/frontend_init.cpp)` and `target_link_libraries(secure_frontend_server_cli PUBLIC secure_frontend_token_store)`.

- [ ] **Step 4: Run to verify it passes**

Run: `cmake --build --preset linux-gcc && ctest --preset linux-gcc | tail -1` → `0 tests failed`.
Run: `cmake --build --preset windows-llvm-mingw-cross | tail -1 && ctest --preset windows-llvm-mingw-cross | tail -1` → build ok; the two init cases are `windows-runtime` (protected DACL + DPAPI), pending MSYS2.

- [ ] **Step 5: Commit**

```bash
git -C "$(git rev-parse --show-toplevel)" add secure_frontend
git -C "$(git rev-parse --show-toplevel)" -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "server: init --config FILE [--force] writes an owner-only config and a generated token (SP1 K10)" \
  -m "Regression tests (failed before): init_writes_config_and_token_then_server_starts,
init_refuses_to_overwrite_without_force. Token first, config second; nothing written when either exists without
--force; the token is never printed." \
  -m "$SP1_TRAILERS"
```

---
### Task 22: CFG-05 end to end; CFG-06 at the type level (K11, K12)

**Files:**
- Modify: `secure_frontend/tests/server_e2e_test.cpp` (new case), `secure_frontend/tests/typed_config_test.cpp` (static asserts + runtime case), `secure_frontend/CMakeLists.txt` (typed-config tests link `secure_frontend_secrets`)

**Interfaces:**
- Consumes: the harness (Task 5), fake engine `--dump` (Task 5), `describe` (Task 4), `SecretBytes` (Task 15), the env scrub (Task 20).
- Produces: acceptance tests only (spec: "none unless it fails"). The behaviors they check were built in Tasks 4–6 and 18–20 exactly as this plan writes them, so both pass on first run; see Step 2 for what a failure means.

- [ ] **Step 1: Write the tests**

`tests/server_e2e_test.cpp`:

```cpp
void cfg05_token_absent_from_child_argv_env_and_outputs() {
  ServerFixture f;
  const auto dump = f.dir / "engine-dump.txt";
  secure_frontend_test::ChildProcess server;
  expect(f.start(server, with(f.base_flags(), {"--engine-arg", "--dump=" + dump.string()})), "server starts: " + server.stderr_text());
  const std::string token{secure_frontend_test::kTestToken};
  std::string seen;
  // A completed chat proves READY was read, so the engine has already written its dump.
  seen += f.request("POST", "/v1/chat/completions", f.bearer(),
                    R"({"model":"qwen-local","messages":[{"role":"user","content":"Hi"}],"max_tokens":2})");
  seen += f.request("GET", "/health", f.bearer());
  seen += f.request("GET", "/v1/models", f.bearer());
  seen += f.request("GET", "/health", "");
  seen += f.request("GET", "/health", "Bearer wrong-token-0123456789abcdefghijklmnop");
  seen += f.request("GET", "/does-not-exist", f.bearer());
  seen += f.request("POST", "/v1/chat/completions", f.bearer(), "{not json");
  expect(contains(seen, "HTTP/1.1 200") && contains(seen, "HTTP/1.1 401") && contains(seen, "HTTP/1.1 404") &&
             contains(seen, "HTTP/1.1 400"),
         "success, auth failure, not found and bad request were all exercised");
  expect(!contains(seen, token), "no response header or body carries the token");
#if !defined(_WIN32)
  const std::string cmdline = secure_frontend_test::read_file("/proc/" + std::to_string(server.pid()) + "/cmdline");
  expect(!cmdline.empty() && !contains(cmdline, token), "the server's /proc/<pid>/cmdline lacks the token");
#endif
  const std::string child = secure_frontend_test::read_file(dump);
  expect(contains(child, "argv:--serve"), "the fake engine dumped its argv and environment");
  expect(!contains(child, token), "the engine child's argv and environment lack the token");
  expect(!contains(child, secure_frontend_test::kTestTokenVariable), "the child does not inherit the token variable at all");
  server.terminate();
  expect(!contains(server.stdout_text(), token) && !contains(server.stderr_text(), token),
         "the server's stdout and stderr lack the token");
}
```

registered with the first e2e list (`run_cases` entry, and the name appended to the first `secure_frontend_add_cases(secure_frontend_server_e2e_tests …)` call, which carries `LABELS ${SECURE_FRONTEND_WINDOWS_RUNTIME}` since Task 9).

`tests/typed_config_test.cpp` (add `#include "secure_frontend/secret_bytes.hpp"`, `#include <string_view>`, `#include <type_traits>`, `#include <cstdlib>`):

```cpp
template <typename T>
constexpr bool is_secret_v = std::is_same_v<std::remove_cv_t<T>, secure_frontend::SecretBytes>;

// CFG-06 at compile time: the long-lived configuration cannot hold a token.
static_assert(!is_secret_v<decltype(secure_frontend::FrontendConfig::listener)>);
static_assert(!is_secret_v<decltype(secure_frontend::FrontendConfig::limits)>);
static_assert(!is_secret_v<decltype(secure_frontend::FrontendConfig::roots)>);
static_assert(!is_secret_v<decltype(secure_frontend::FrontendConfig::token)>);
static_assert(!is_secret_v<decltype(secure_frontend::FrontendConfig::engine)>);
static_assert(std::is_same_v<std::remove_cv_t<decltype(secure_frontend::FrontendConfig::token)>, secure_frontend::SecretSource>,
              "the token member is source metadata (kind + env name or relative path)");
static_assert(!std::is_constructible_v<secure_frontend::SecretSource, secure_frontend::SecretBytes>,
              "a secret cannot become a SecretSource");
static_assert(!std::is_copy_constructible_v<secure_frontend::SecretBytes>,
              "a secret cannot be copied into any config value");

void cfg06_config_has_no_secret_typed_member() {
  expect(true, "enforced by the static_asserts above; this case records that they compiled");
}

void cfg06_validated_config_bytes_do_not_contain_token() {
  ConfigFixture f;
  const std::string token = "cfg06-token-0123456789abcdefghijklmnopqrstu";
#if defined(_WIN32)
  _putenv_s("SF_CFG06_TOKEN", token.c_str());
#else
  setenv("SF_CFG06_TOKEN", token.c_str(), 1);
#endif
  auto literals = f.literals;
  literals["token.source"] = secure_frontend_test::json_literal("env:SF_CFG06_TOKEN");
  const auto result = validate_text(render_config_json(literals), f.dir);
  const auto* validated = std::get_if<secure_frontend::ValidatedConfig>(&result);
  expect(validated != nullptr, "config validates: " + issues_of(result));
  if (validated == nullptr) return;
  const std::string described = secure_frontend::describe(validated->get());
  expect(described.find("env:SF_CFG06_TOKEN") != std::string::npos, "the source is described as metadata");
  expect(described.find(token) == std::string::npos, "the described object graph never contains the token");
  const std::string_view bytes{reinterpret_cast<const char*>(&validated->get()), sizeof(secure_frontend::FrontendConfig)};
  expect(bytes.find(token) == std::string_view::npos, "the object's own bytes never contain the token");
}
```

registered (`run_cases` entries and CMake) as:

```cmake
    target_link_libraries(secure_frontend_typed_config_tests PRIVATE secure_frontend_secrets)
    secure_frontend_add_cases(secure_frontend_typed_config_tests cfg06_config_has_no_secret_typed_member)
    secure_frontend_add_cases(secure_frontend_typed_config_tests cfg06_validated_config_bytes_do_not_contain_token
        LABELS ${SECURE_FRONTEND_WINDOWS_RUNTIME})
```

(the first opens no root and stays unlabelled; the second validates a `ConfigFixture`, so the Task 9 wine rule applies).

- [ ] **Step 2: Run them**

Run: `cmake --build --preset linux-gcc && ctest --preset linux-gcc -R 'cfg05_token_absent_from_child|cfg06_' --output-on-failure`
Expected: PASS on first run — acceptance tests of Tasks 4 (metadata-only config), 5 (no echo), 6, 18–20 (scrubbed env, empty child environment). Record "passed on first run" in the commit. Decision for a failure: it means an earlier task (4–6, 18–20) was implemented differently from this plan, so do not patch around it here — stop, and report the failing assertion and the task it points to (Global Constraints: stop and report).

- [ ] **Step 3: Full run**

Run: `ctest --preset linux-gcc | tail -1` → `0 tests failed`.
Run: `cmake --build --preset windows-llvm-mingw-cross | tail -1 && ctest --preset windows-llvm-mingw-cross | tail -1` → build ok; record.

- [ ] **Step 4: Commit**

```bash
git -C "$(git rev-parse --show-toplevel)" add secure_frontend
git -C "$(git rev-parse --show-toplevel)" -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "tests: CFG-05 end to end and CFG-06 at the type level (SP1 K11, K12)" \
  -m "Acceptance tests (passed on first run unless noted): cfg05_token_absent_from_child_argv_env_and_outputs
(responses, child argv/env dump, /proc/<pid>/cmdline, server stdout/stderr), cfg06_config_has_no_secret_typed_member
(static_asserts), cfg06_validated_config_bytes_do_not_contain_token." \
  -m "$SP1_TRAILERS"
```

---

### Task 23: AUTH-01, AUTH-02, AUTH-04 acceptance tests (A1, A2, A4; A3 is Task 19)

**Files:**
- Modify: `secure_frontend/tests/api_router_test.cpp` (router-level AUTH-01), `secure_frontend/tests/server_e2e_test.cpp` (socket AUTH-01, AUTH-02), `secure_frontend/tests/server_cli_test.cpp` (AUTH-04), `secure_frontend/CMakeLists.txt`

**Interfaces:**
- Consumes: `handle_http_request`, `ApiRouter`, `TokenVerifier`, `make_bearer_validator` (Task 19); `config_schema()` (Task 4); the harness (Task 5).
- Produces: acceptance tests only. They pass on first run (no unauthenticated mode exists, spec §3.8); see Step 2 for what a failure means.

- [ ] **Step 1: Write the tests**

`tests/api_router_test.cpp` (add `#include "secure_frontend/token_verifier.hpp"`, `#include <memory>`, `#include <utility>`):

```cpp
void auth01_every_route_rejects_missing_malformed_wrong_token() {
  secure_frontend::SecretBytes secret;
  (void)secret.assign("auth01-token-0123456789abcdefghijklmnopq");
  const auto validator = secure_frontend::make_bearer_validator(
      std::make_shared<const secure_frontend::TokenVerifier>(std::move(secret)));
  const auto routes_under_test = router();
  const secure_frontend::RequestDispatcher dispatch = [&routes_under_test](std::string_view method, std::string_view target,
                                                                           std::string_view) {
    return routes_under_test.dispatch(method, target);
  };
  const secure_frontend::HttpPolicy policy{.allowed_hosts = {"127.0.0.1:8080"},
                                           .same_origin_origins = {"http://127.0.0.1:8080"},
                                           .cors_allowed_origins = {}, .cors_enabled = false};
  const std::string token = "auth01-token-0123456789abcdefghijklmnopq";
  const std::pair<std::string, int> variants[] = {
      {"", 401},                                                           // missing
      {"Authorization: bearer " + token + "\r\n", 401},                    // lowercase scheme
      {"Authorization: Bearer \r\n", 401},                                 // Bearer + empty
      {"Authorization: Bearer\t" + token + "\r\n", 401},                   // tab separator
      {"Authorization: Basic " + token + "\r\n", 401},                     // other scheme
      {"Authorization: Bearer " + token + "\r\nAuthorization: Bearer " + token + "\r\n", 400},  // duplicate
      {"Authorization: Bearer wrong-token-0123456789abcdefghijklmn\r\n", 401}};
  const std::pair<const char*, const char*> routes[] = {
      {"GET", "/health"}, {"GET", "/v1/models"}, {"POST", "/v1/chat/completions"}, {"GET", "/does-not-exist"}};
  for (const auto& [method, target] : routes) {
    for (const auto& [headers, status] : variants) {
      std::string request = std::string{method} + " " + target + " HTTP/1.1\r\nHost: 127.0.0.1:8080\r\n" + headers;
      request += std::string{method} == "POST" ? "Content-Length: 2\r\n\r\n{}" : "\r\n";
      const auto response = secure_frontend::handle_http_request(request, policy, validator, dispatch);
      expect(response.status == status, std::string{method} + " " + target + " must answer " + std::to_string(status));
      expect(response.body.find("local-model") == std::string::npos && response.body.find("status") == std::string::npos,
             "a rejected request carries no route or model data");
    }
  }
  const auto allowed = secure_frontend::handle_http_request(
      "GET /v1/models HTTP/1.1\r\nHost: 127.0.0.1:8080\r\nAuthorization: Bearer " + token + "\r\n\r\n", policy, validator,
      dispatch);
  expect(allowed.status == 200 && allowed.body.find("local-model") != std::string::npos,
         "the same route answers with the right token (the negatives are not vacuous)");
}
```

registered as `auth01_every_route_rejects_missing_malformed_wrong_token` (`run_cases` entry, and appended to the `secure_frontend_add_cases(secure_frontend_api_router_tests …)` list; pure in-memory, unlabelled).

`tests/server_e2e_test.cpp`:

```cpp
void auth01_socket_rejects_missing_and_wrong_token_on_every_route() {
  ServerFixture f;
  secure_frontend_test::ChildProcess server;
  expect(f.start(server, f.base_flags()), "server starts: " + server.stderr_text());
  for (const auto& [method, target] : {std::pair{"GET", "/health"}, std::pair{"GET", "/v1/models"},
                                       std::pair{"POST", "/v1/chat/completions"}, std::pair{"GET", "/does-not-exist"}}) {
    for (const std::string& authorization : {std::string{}, std::string{"Bearer wrong-token-0123456789abcdefghijklmnop"}}) {
      const auto response = f.request(method, target, authorization, std::string{method} == "POST" ? "{}" : "");
      expect(response.rfind("HTTP/1.1 401", 0) == 0, std::string{method} + " " + target + " over TCP must be 401");
      expect(!contains(response, "qwen-local") && !contains(response, "\"engine\""), "no route or model data leaks");
    }
  }
}

void auth02_valid_token_succeeds_and_is_not_echoed() {
  ServerFixture f;
  secure_frontend_test::ChildProcess server;
  expect(f.start(server, f.base_flags()), "server starts: " + server.stderr_text());
  const auto health = f.request("GET", "/health", f.bearer());
  const auto models = f.request("GET", "/v1/models", f.bearer());
  expect(health.rfind("HTTP/1.1 200", 0) == 0 && models.rfind("HTTP/1.1 200", 0) == 0 && contains(models, "qwen-local"),
         "authorized requests succeed");
  expect(!contains(health, secure_frontend_test::kTestToken) && !contains(models, secure_frontend_test::kTestToken),
         "the token is not echoed in a response");
  server.terminate();
  expect(!contains(server.stdout_text(), secure_frontend_test::kTestToken) &&
             !contains(server.stderr_text(), secure_frontend_test::kTestToken),
         "nor written to stdout/stderr");
}
```

both registered with the first e2e list (`run_cases` entries, and the names appended to the first `secure_frontend_add_cases(secure_frontend_server_e2e_tests …)` call, labelled `windows-runtime` since Task 9).

`tests/server_cli_test.cpp` (add `#include "secure_frontend/frontend_config.hpp"`):

```cpp
void auth04_empty_token_rejected_on_loopback() {
  ServerFixture f;
  const auto marker = f.dir / "engine-started";
  const auto flags = with(f.base_flags(), {"--engine-arg", "--marker=" + marker.string()});
  for (const char* value : {"", "   "}) {
    const auto result = secure_frontend_test::run_to_completion(
        secure_frontend_test::ProcessSpec{f.server_executable, flags, {{secure_frontend_test::kTestTokenVariable, value}}});
    expect(result.exit_code != 0 && contains(result.err, "Unable to read API token"),
           std::string{"an empty or blank token stops the loopback server: \""} + value + "\"");
  }
  secure_frontend_test::write_file(f.secret / "token", "");
  std::vector<std::string> file_flags = f.base_flags();
  for (std::size_t i = 0; i + 1 < file_flags.size(); ++i) {
    if (file_flags[i] == "--token-env") { file_flags[i] = "--token-file"; file_flags[i + 1] = "token"; }
  }
  file_flags.insert(file_flags.end(), {"--secret-root", f.secret.string(), "--engine-arg", "--marker=" + marker.string()});
  const auto empty_file = f.run(file_flags);
  expect(empty_file.exit_code != 0 && contains(empty_file.err, "Unable to read API token"), "an empty token file too");
  expect(!std::filesystem::exists(marker), "the engine never started");
  expect(!secure_frontend_test::can_connect(f.port), "and nothing listens");
}

void auth04_no_switch_disables_auth() {
  for (const auto& key : secure_frontend::config_schema()) {
    const std::string path{key.path};
    for (const char* word : {"auth", "insecure", "unauth", "dev", "anonymous", "disable", "no_token"}) {
      expect(path.find(word) == std::string::npos, "no config key can switch off authentication: " + path);
    }
    if (key.type == secure_frontend::ConfigValueType::boolean) {
      expect(path == "listener.allow_remote" || path == "listener.allow_wildcard_bind",
             "the only switches are the two bind opt-ins: " + path);
    }
  }
  ServerFixture f;
  for (const char* flag : {"--no-auth", "--insecure", "--dev", "--disable-auth", "--allow-unauthenticated", "--auth=none"}) {
    const auto result = f.run(with(f.base_flags(), {flag}));
    expect(result.exit_code != 0 && contains(result.err, "unknown option"), std::string{"no such option: "} + flag);
  }
  // Upstream's STRATA_* knobs are never read (product spec §4); set them all and probe without a token.
  auto environment = f.environment();
  for (const char* name : {"STRATA_API_KEY", "STRATA_NO_AUTH", "STRATA_DEV", "STRATA_INSECURE", "STRATA_DEBUG", "STRATA_AUTH"}) {
    environment.emplace_back(name, std::string{name} == "STRATA_API_KEY" ? "" : "1");
  }
  secure_frontend_test::ChildProcess server;
  expect(server.start(secure_frontend_test::ProcessSpec{f.server_executable, f.base_flags(), environment}), "server spawned");
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (!secure_frontend_test::can_connect(f.port) && std::chrono::steady_clock::now() < deadline &&
         !server.wait_for(std::chrono::milliseconds(50)).has_value()) {
  }
  const auto response = f.request("GET", "/v1/models", "");
  expect(response.rfind("HTTP/1.1 401", 0) == 0, "no environment variable disables authentication");
}
```

registered with the first CLI list (`run_cases` entries, and the names appended to the first `secure_frontend_add_cases(secure_frontend_server_cli_tests …)` call: `${SECURE_FRONTEND_SERVER_ARGS} LABELS ${SECURE_FRONTEND_WINDOWS_RUNTIME} TIMEOUT 120` since Task 9).

- [ ] **Step 2: Run them**

Run: `cmake --build --preset linux-gcc && ctest --preset linux-gcc -R 'auth0[124]' --output-on-failure`
Expected: PASS on first run (acceptance tests: no unauthenticated mode exists, spec §3.8). Record that. Decision for a failure: it means Task 5, 18 or 19 was implemented differently from this plan — stop and report the failing assertion and the task it points to (Global Constraints) instead of patching it here.

- [ ] **Step 3: Full run**

Run: `ctest --preset linux-gcc | tail -1` → `0 tests failed`.
Run: `cmake --build --preset windows-llvm-mingw-cross | tail -1 && ctest --preset windows-llvm-mingw-cross | tail -1` → build ok; record.

- [ ] **Step 4: Commit**

```bash
git -C "$(git rev-parse --show-toplevel)" add secure_frontend
git -C "$(git rev-parse --show-toplevel)" -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "tests: AUTH-01, AUTH-02, AUTH-04 acceptance (SP1 A1, A2, A4)" \
  -m "auth01_every_route_rejects_missing_malformed_wrong_token (router), auth01_socket_rejects_missing_and_wrong_token_on_every_route,
auth02_valid_token_succeeds_and_is_not_echoed, auth04_empty_token_rejected_on_loopback, auth04_no_switch_disables_auth.
AUTH-03 is Task 19. Passed on first run unless noted." \
  -m "$SP1_TRAILERS"
```

---

### Task 24: Documentation, generated key table, bootstrap fix, final verification (D1–D5)

**Files:**
- Modify: `secure_frontend/include/secure_frontend/frontend_config.hpp`, `secure_frontend/src/frontend_config.cpp` (`render_config_schema_markdown`)
- Modify: `secure_frontend/tests/typed_config_test.cpp` (README check), `secure_frontend/CMakeLists.txt`
- Modify: `secure_frontend/README.md` (D3), `docs/security/deployment.md:79-83, 125-128` (D2), `docs/security/engine-protocol-limits.md` (D5), `docs/AGENT_HANDOFF.md` (D1), `docs/secure-frontend-ci.md` (label note)
- Modify: `scripts/dev/bootstrap-llvm-mingw.sh:26` (D4)

**Interfaces:**
- Produces: `[[nodiscard]] std::string render_config_schema_markdown();` — the README key table, generated from `config_schema()` (one source of truth).

- [ ] **Step 1: Failing checks**

Add to `tests/typed_config_test.cpp`:

```cpp
void readme_config_table_matches_schema() {
  const auto& args = secure_frontend_test::extra_arguments();
  expect(!args.empty(), "the README path is passed as ARGS");
  if (args.empty()) return;
  const std::string readme = secure_frontend_test::read_file(args.front());
  const std::string begin = "<!-- config-keys:begin -->\n";
  const std::string end = "<!-- config-keys:end -->";
  const auto start = readme.find(begin);
  const auto stop = readme.find(end);
  expect(start != std::string::npos && stop != std::string::npos, "README has the config-keys markers");
  if (start == std::string::npos || stop == std::string::npos) return;
  const std::string table = readme.substr(start + begin.size(), stop - start - begin.size());
  const std::string expected = secure_frontend::render_config_schema_markdown();
  expect(table == expected, "README key table is stale; replace it with:\n" + expected);
}
```

CMake: `secure_frontend_add_cases(secure_frontend_typed_config_tests readme_config_table_matches_schema ARGS ${CMAKE_CURRENT_SOURCE_DIR}/README.md)`.

Run: `cmake --build --preset linux-gcc 2>&1 | grep -m1 error` → `'render_config_schema_markdown' is not a member of 'secure_frontend'`.
Run (D4): `tmp=$(mktemp -d "$PWD/out/d4-XXXXXX") && mkdir -p "$tmp/scripts/dev" "$tmp/.toolchains/downloads" && cp ../scripts/dev/bootstrap-llvm-mingw.sh "$tmp/scripts/dev/" && echo corrupt > "$tmp/.toolchains/downloads/llvm-mingw-20261006-ucrt-ubuntu-22.04-x86_64.tar.xz" && (bash "$tmp/scripts/dev/bootstrap-llvm-mingw.sh"; echo "exit=$?"); ls "$tmp/.toolchains/downloads"`
Expected before the fix: `exit=1` and the corrupt archive is still listed. (The scratch copy lives under `secure_frontend/out/`, which git ignores; nothing else is touched.)

- [ ] **Step 2: Generator** — `frontend_config.hpp`: `[[nodiscard]] std::string render_config_schema_markdown();` with the comment "The README key table (secure_frontend/README.md, between the config-keys markers); a test fails when they differ." `frontend_config.cpp`:

```cpp
std::string render_config_schema_markdown() {
  std::string out = "| Key | Type | Default | Range | Option | Meaning |\n| --- | --- | --- | --- | --- | --- |\n";
  for (const auto& key : kSchema) {
    std::string type;
    std::string fallback;
    std::string range = "-";
    switch (key.type) {
      case ConfigValueType::boolean:
        type = "boolean";
        fallback = key.default_boolean ? "`true`" : "`false`";
        break;
      case ConfigValueType::integer:
        type = "integer";
        fallback = "`" + std::to_string(key.default_integer) + "`";
        range = std::to_string(key.minimum) + "-" + std::to_string(key.maximum);
        break;
      case ConfigValueType::string:
        type = "string";
        fallback = !key.default_string.empty() ? "`" + std::string{key.default_string} + "`" : key.required ? "required" : "unset";
        break;
      case ConfigValueType::string_array:
        type = "string array";
        fallback = "`[]`";
        break;
    }
    const std::string option = key.flag.empty() ? "-" : "`" + std::string{key.flag} + "`";
    out += "| `" + std::string{key.path} + "` | " + type + " | " + fallback + " | " + range + " | " + option + " | " +
           std::string{key.description} + " |\n";
  }
  return out;
}
```

- [ ] **Step 3: README (D3)** — in `secure_frontend/README.md`, replace the flag list in "Run the native inference server" (line 167 onward) with a "Configuration" section:

````markdown
## Configuration

`strata-secure-server` reads one strict JSON file (`--config FILE`) and the
command-line options; an option wins over the file, and `--check` reports every
key where that happened. Built-in defaults fill the rest. The file is UTF-8
without BOM, no comments, no trailing commas, no duplicate keys, nesting at most
4 levels, at most 64 KiB, integers only where a number is expected, and
`schema_version: 1` is required. It must be owned by you and not writable by
other users (its directory too). It never holds the token: `token.source` names
an environment variable (`env:NAME`) or a file below `roots.secret`
(`file:token`).

    strata-secure-server init --config ~/.config/strata/config.json \
        --models-root ~/models --tokenizer-pack ~/tok --engine ~/strata/strata --model-id qwen-local
    strata-secure-server --check --config ~/.config/strata/config.json
    strata-secure-server --config ~/.config/strata/config.json

`init` writes the config (owner-only, atomic) and a 43-character token generated
from the OS random generator (`0600` on Linux; DPAPI-wrapped with a private ACL on
Windows). It refuses to replace either without `--force`. Where the files live by
default and the `strata` command are defined by the product-experience spec, not
here. On Windows, give non-ASCII paths in the config file: command-line arguments
arrive in the ANSI code page.

Every key (generated from the schema; `readme_config_table_matches_schema` keeps it current):

<!-- config-keys:begin -->
| Key | Type | Default | Range | Option | Meaning |
| --- | --- | --- | --- | --- | --- |
| `schema_version` | integer | `1` | 1-1 | - | Config format version; must be 1. Required in a config file. |
| `listener.host` | string | `127.0.0.1` | - | `--host` | Bind address: an IPv4/IPv6 literal or localhost. Other hostnames are rejected (never resolved). |
| `listener.port` | integer | `8080` | 1-65535 | `--port` | TCP port. |
| `listener.allow_remote` | boolean | `false` | - | `--allow-remote` | Required for every non-loopback address. |
| `listener.allow_wildcard_bind` | boolean | `false` | - | `--allow-wildcard-bind` | Also required for 0.0.0.0, :: and every other spelling of the unspecified address. |
| `listener.backlog` | integer | `16` | 1-4096 | - | listen() backlog. |
| `limits.http.request_deadline_ms` | integer | `30000` | 1-3600000 | - | Total time to read one request, header and body. |
| `limits.http.max_request_bytes` | integer | `65536` | 4096-16777216 | - | Largest request (header and body) the listener and parser accept. |
| `limits.http.max_header_bytes` | integer | `16384` | 1024-65536 | - | Largest header block; at most limits.http.max_request_bytes. |
| `limits.http.max_headers` | integer | `64` | 8-256 | - | Most header fields. |
| `limits.http.max_request_line_bytes` | integer | `4096` | 256-16384 | - | Longest request line; at most limits.http.max_header_bytes. |
| `limits.chat.max_body_bytes` | integer | `49152` | 1-16777216 | - | Largest chat request body; at most max_request_bytes minus max_header_bytes. |
| `limits.chat.max_messages` | integer | `128` | 1-1024 | - | Most messages in one chat request. |
| `limits.chat.max_message_bytes` | integer | `65536` | 1-16777216 | - | Largest single message content. |
| `limits.chat.max_prompt_tokens` | integer | `32768` | 1-1048576 | - | Largest tokenized prompt sent to the engine. |
| `limits.chat.max_completion_tokens` | integer | `4096` | 1-16384 | - | Largest max_tokens a request may ask for. |
| `limits.chat.default_max_tokens` | integer | `256` | 1-16384 | - | max_tokens when a request omits it; at most limits.chat.max_completion_tokens. |
| `limits.chat.max_generated_bytes` | integer | `1048576` | 1-16777216 | - | Largest generated text returned. |
| `limits.engine.ready_timeout_s` | integer | `900` | 1-86400 | - | How long the first request waits for the engine's READY (upstream STRATA_ENGINE_READY_S). |
| `limits.engine.generation_timeout_s` | integer | `120` | 1-86400 | - | Total time for one generation before STOP. |
| `limits.engine.stop_drain_timeout_s` | integer | `5` | 1-600 | - | Time to drain to DONE after STOP before the engine is marked failed. |
| `limits.concurrency.max_concurrent_requests` | integer | `1` | 1-1 | - | Requests served at once; only 1 until SP3. |
| `roots.models` | string | required | - | `--models-root` | Absolute directory of .gguf files. The root itself may be a symlink; nothing below it may be. |
| `roots.tokenizer_pack` | string | required | - | `--tokenizer-pack` | Absolute directory with vocab.json, merges.txt, token_type.json. |
| `roots.secret` | string | unset | - | `--secret-root` | Absolute directory holding the token file. Default: the config file's directory. |
| `roots.logs` | string | unset | - | - | Absolute log directory (created 0700; unused in SP1). |
| `roots.cache` | string | unset | - | - | Absolute cache directory (created 0700; unused in SP1). |
| `token.source` | string | `file:token` | - | `--token-file, --token-env` | file:<relative path below roots.secret> or env:<NAME>. Metadata only; the token is never in the config. |
| `engine.profile` | string | `qwen` | - | `--profile` | qwen or glm53. |
| `engine.executable` | string | required | - | `--engine` | Absolute path of the engine; a regular file not writable by group or others. |
| `engine.model_id` | string | required | - | `--model-id` | Model ID from the local inventory. |
| `engine.args` | string array | `[]` | - | `--engine-arg (repeatable)` | Extra engine arguments; frontend-owned and unsupported flags are rejected. |
<!-- config-keys:end -->
````

The table above is exactly what `render_config_schema_markdown()` produces for the Task 4 schema (no blank line between the markers and the table); no later task changes a schema row. Run `ctest --preset linux-gcc -R readme_config_table --output-on-failure` → passes. (Its failure message prints the generated table, which is how a future schema change updates this README.) Also update the "Security contract" bullets: tokens are generated by `init` (CSPRNG, base64url, 43 characters); operator tokens must be at least 32 bytes; a token file may end in one LF/CRLF; the listener rejects hostnames other than `localhost`.

- [ ] **Step 4: deployment.md (D2)** — replace lines 79-82 with:

```markdown
- Generate the token with `strata-secure-server init` (CSPRNG, 43 characters,
  never printed). On Linux it is an owner-only `0600` file in a `0700`
  directory. On Windows it is a DPAPI-wrapped file (`STRATAK1` + blob) with a
  protected DACL for you and SYSTEM; the reader also accepts an operator-made
  plaintext file under the same ACL rules. Until the `windows-msys2-clang` CI
  job has passed the `windows-runtime` token tests, treat the Windows file path
  as unverified and prefer `--token-env` there.
```

and lines 125-128 with:

```markdown
- Windows token files require a current-user-owned protected DACL on the secret
  directory and the file; the reader opens every component with `READ_CONTROL`
  and rejects reparse points (junctions, symlinks), path escapes, extra hard
  links, inherited or broad ACLs and non-regular files. Roots under OneDrive
  Known Folder Move or a roaming profile resolve to a network path and are
  rejected; keep models, secrets and config on a local disk.
```

(When the owner's push shows the MSYS2 job green for those tests, the last sentence of the first block is removed in a follow-up commit — record that in the handoff as an open item.)

- [ ] **Step 5: engine-protocol-limits.md (D5)** — append:

```markdown
## Configuration keys and internal constants (SP1)

Tunable limits are configuration keys (`secure_frontend/README.md` key table;
defaults in `include/secure_frontend/config_defaults.hpp`): listener backlog,
request deadline, request/header/request-line sizes and header count, chat body
/ message / prompt / completion / generated-text limits, `ready_timeout_s`
(default 900 s, upstream `STRATA_ENGINE_READY_S`), generation and STOP-drain
timeouts, and `max_concurrent_requests` (only 1 until SP3).

These stay internal because they are format or protocol facts, not tuning:
`kProtocolMaxLineBytes` 16 MiB and the 8192-byte inbound record cap; the
`EngineStdioProcess` buffers; process timings (10 ms poll, 1 s post-kill, 250 ms
grace — SP2); POSIX launch `kReportFd`/`kLiftFloor`/`kSweepCap`; the 1000 ms
minimum send budget; 50 ms EMFILE backoff; the 100 ms / 256 KiB half-close drain;
tokenizer-pack caps (64 MiB file, 1 Mi entries, 16 KiB token, 8 MiB decoded);
model ID ≤ 128 bytes; chat `model` field ≤ 256 bytes; token ≤ 256 bytes; token
file ≤ 4 KiB; the frontend-owned and unsupported engine flags.
```

- [ ] **Step 6: bootstrap-llvm-mingw.sh (D4)** — replace line 26 with:

```bash
  if ! echo "$sha256  $dest/downloads/$archive" | sha256sum -c -; then
    rm -f -- "$dest/downloads/$archive"
    echo "bootstrap-llvm-mingw: digest mismatch; the downloaded archive was deleted, re-run to fetch it again" >&2
    exit 1
  fi
```

Rerun the Step 1 D4 command: expected `exit=1` and an empty `downloads` listing.

- [ ] **Step 7: Handoff (D1) and CI doc**

`docs/AGENT_HANDOFF.md`:
- After "### SP0 deliverables …" add "### SP1 deliverables (branch `sp1-typed-config-paths-tokens`)" with one row per task 1–24: item IDs, what changed, and the regression tests named in that task's commit (copy them from `git -C "$(git rev-parse --show-toplevel)" log --format=%B main..HEAD`; Task 2's commit is on `main` itself — the shared pre-task SP2 also consumes — so copy its row from `git -C "$(git rev-parse --show-toplevel)" log --format=%B -1 --grep='windows-runtime label' main`), and for every `windows-runtime` test the words "pending windows-msys2-clang".
- "Verification performed": a new SP1 table with the commands of Step 8 and their actual results (test totals from `ctest -N | tail -1`, the wine summary, the list of `windows-runtime` tests not run locally).
- "Known small gaps": delete the "SP1 (first task)" wildcard bullet, the `bootstrap-llvm-mingw.sh` bullet and the CMake-comment bullet; add: (1) the engine opens model files by path after validation — a swap below a user-owned, non-shared-writable root is a same-user race, accepted until SP2 decides on handle passing (spec §3.4); (2) on Windows, `strata-secure-server` takes `char** argv` in the ANSI code page, so non-ASCII paths belong in the config file until the product `strata` CLI uses `wmain`; (3) Windows environment-token scrub — accepted deviation from spec §3.6, owner-ruled: Windows has no `/proc/<pid>/environ` exposure, so the scrub wipes the CRT copy in place and removes the Win32 environment-block entry with `SetEnvironmentVariableW(name, nullptr)`; the block entry is removed, not overwritten in place (no Win32 API overwrites it); (4) the deployment.md Windows token caveat is removed once the MSYS2 job is green; (5) `cfg03_rejects_mount_crossing` skips where unprivileged user namespaces are disabled (GitHub's ubuntu-24.04 images may skip it); (6) naming: the product-experience spec §4 sketches `listen.*`, flat `limits.*` keys and `token_source`; SP1 supersedes those names with `listener.*`, `limits.<section>.*` (`limits.http.*`, `limits.chat.*`, `limits.engine.*`, `limits.concurrency.*`) and `token.source`, and the future `strata` CLI follows SP1's names (the product spec §4 text is updated when that CLI is planned).
- "Current security behavior": the listener classifies addresses from bytes and rejects hostnames except `localhost`; configuration is schema-validated before bind/spawn and `--check` has no side effects (only serve and init create the logs/cache roots); the engine is spawned after bind; tokens are 43-character CSPRNG values compared in a fixed 256-byte window; env tokens are scrubbed; the server is non-dumpable.

`docs/secure-frontend-ci.md`: add under the presets section: "Each test case is its own CTest test (`<binary>.<case>`). Cases that need real Windows semantics carry the label `windows-runtime`; the `windows-llvm-mingw-cross` test preset excludes it under wine, and `windows-msys2-clang` runs everything."

- [ ] **Step 8: Final verification (spec §9)**

```bash
cd /opt/projects/ai/.worktrees/sp1
export PATH="$(git rev-parse --show-toplevel)/.venv-dev/bin:$PATH"
"$(git rev-parse --show-toplevel)/.venv-dev/bin/pytest" -q | tail -1
cd secure_frontend
cmake --preset linux-gcc && cmake --build --preset linux-gcc && ctest --preset linux-gcc | tail -1
ctest --preset linux-gcc -N | grep -cE '\.(net0[123]|auth0[1234]|cfg0[123456])_'
cmake --preset windows-llvm-mingw-cross && cmake --build --preset windows-llvm-mingw-cross | tail -1
ctest --preset windows-llvm-mingw-cross | tail -1
WINEPREFIX=$PWD/out/wineprefix WINEDLLOVERRIDES=mscoree,mshtml= wine out/build/windows-llvm-mingw-cross/strata-secure-server.exe --help >/dev/null; echo "help exit=$?"
WINEPREFIX=$PWD/out/wineprefix WINEDLLOVERRIDES=mscoree,mshtml= wine out/build/windows-llvm-mingw-cross/strata-secure-server.exe --check --port 0 --models-root 'C:\x' --tokenizer-pack 'C:\y' --engine 'C:\e.exe' --model-id m --token-env X 2>&1 | head -3; echo "check exit=${PIPESTATUS[0]}"
git -C "$(git rev-parse --show-toplevel)" status --short
```

Expected: pytest passes; `100% tests passed` on Linux; the acceptance-ID count is non-zero and every `net0*`, `auth0*`, `cfg0*` case is listed individually; the cross build succeeds with `-Werror`; the wine preset reports `0 tests failed` with `windows-runtime` excluded; `help exit=0`; the `--check` run prints `Configuration error: listener.port: must be between 1 and 65535` (among others) and `check exit=1`; `git status --short` shows only the documentation files of this task (as ` M`/`??` lines under `docs/`, `secure_frontend/` and `scripts/dev/`) plus the two worktree symlinks `?? .toolchains` and `?? .venv-dev`, which are never added.

- [ ] **Step 9: Commit**

```bash
git -C "$(git rev-parse --show-toplevel)" add secure_frontend docs scripts/dev/bootstrap-llvm-mingw.sh
git -C "$(git rev-parse --show-toplevel)" -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "docs: SP1 handoff, deployment, README key table from the schema, limits note; bootstrap deletes a bad archive (SP1 D1-D5)" \
  -m "readme_config_table_matches_schema keeps the README key table equal to config_schema(). Windows runtime
claims are limited to what the windows-msys2-clang job will show; nothing is marked verified before it runs." \
  -m "$SP1_TRAILERS"
```

---

## Self-review (done while writing)

**1. Spec coverage**

| Spec item | Task | Spec item | Task |
| --- | --- | --- | --- |
| N1 byte classification (defect a) | 1 | P6 inventory/tokenizer via module | 13 |
| N2 hostnames rejected, `localhost` | 1 | P7 engine gets canonical root (defect e) | 9 |
| N3 default listener loopback-only | 6 | P8 create within root, logs/cache | 14 |
| N4 NET-02 matrix, non-secret error | 6 | P9 duplicates deleted (grep) | 8, 10, 11, 13, 14 |
| C1 shared strict JSON reader | 3 | K1 CSPRNG + base64url | 15 |
| C2 typed model, `validate()` every key | 4 | K2 Linux atomic store | 16 |
| C3 fail before listener/child | 5 (validate first), 6 (bind before spawn) | K3 Windows DPAPI store | 17 |
| C4 parser honors limits (defect d) | 7 | K4 un-gate token reader tests | 18 |
| C5 `ready_timeout` 900 s, configurable | 4 (default), 7 (wiring) | K5 one trailing LF/CRLF (defect c) | 18 |
| C6 `--config`, `--check`, precedence | 5 | K6 `SecretBytes`, verifier, wiped buffers | 15, 18, 19 |
| C7 untrusted config files | 12 | K7 fixed-window compare | 19 |
| C8 no value echo | 5 | K8 env scrub | 20 |
| P1 one grammar | 8 | K9 core dumps | 20 |
| P2 root rules (+ Windows forms) | 9 | K10 `init` | 21 |
| P3 containment / sibling prefix | 9 | K11 CFG-05 e2e | 22 |
| P4 Linux `openat2` + fallback | 10 | K12 CFG-06 type level | 22 |
| P5 Windows `READ_CONTROL` walk (defect b) | 11 | A1 / A2 / A3 / A4 | 23 / 23 / 19 / 23 |
| §3.10 `--case`, helper, labels, mkdtemp | 2 | D1–D5 | 24 |

Decisions: §3.1 → Tasks 3–4; §3.2 → 5; §3.3 → 4; §3.4 → 8–14; §3.5 → 15–18; §3.6 → 15, 18–20; §3.7 → 1; §3.8 → 23; §3.9 → 21; §3.10 → 2; §3.11 → 4, 7. §6 review focus → plan Review Focus (Tasks 9, 11, 13, 18). §9 acceptance → Task 24 Step 8 plus the owner's push (MSYS2). The CI top-level job's `-R secure_frontend_config_tests` still matches the per-case names (`secure_frontend_config_tests.<case>`).

Ordering note: spec §8 lists C2–C8 before P1–P9. C7 (Task 12) runs after P4/P5 because its fix is "opened via §3.4"; P7 is folded into Task 9 because the `CanonicalRoot` type change is what fixes defect (e); N2 is folded into Task 1 because the same parser decides both. Task 2 executes first, on `main` (shared with SP2), and Task 1 follows it in the worktree. The config model's `Roots`/`EngineLaunch` start as validated absolute paths in Task 4 and become `CanonicalRoot`/`CanonicalFile` in Task 9; `roots.logs`/`roots.cache` become checked canonical paths in Task 14 (created only by serve/init).

**2. Placeholder scan** — searched for "TBD", "TODO", "similar to", "add validation", "handle edge cases": none. Two deliberate fill-ins remain and are instructions, not gaps: `$SP1_TRAILERS` (set once from the harness, Global Constraints) and `<record>` / `<passed|skipped>` in commit bodies (paste the wine or skip result the step just printed). The README key table is written out in full in Task 24 Step 3 and is checked against `config_schema()` by `readme_config_table_matches_schema`.

**3. Type consistency** — `IpAddress`/`parse_listener_address` (1) → 4, 6. `secure_frontend_add_cases`/`run_cases`/`make_scratch_directory`/`write_file` (2) → every task. `JsonValue`/`parse_json`/`append_json_string` (3) → 4, 5, 21. `FrontendConfigDraft`, `ConfigIssue`, `validate`, `describe`, `listener_config`, `config_schema`, `ConfigKey` (4) → 5–7, 12, 21–24. `parse_config_text` (4), `read_config_file`/`merge_drafts` (5, 12), `render_config_document` (21). `ServerInvocation`/`parse_server_command_line`/`load_server_config` (5) → 21. `ServerFixture`/`ChildProcess`/`run_to_completion`/`kTestToken` (5) → 6, 7, 9, 20–23. `HttpParserLimits`, `HttpSocketServerLimits`, `ChatSchemaLimits`, `ChatExecutionLimits`, `http_socket_server_limits`/`chat_schema_limits`/`chat_execution_limits` (7). `RelativePath`/`parse_relative_path`/`PathError`/`PathErrorCode`/`path_from_utf8`/`path_to_utf8` (8) → 9–21. `CanonicalRoot`/`CanonicalFile`/`OwnedHandle::release`/`open_root`/`open_executable`/`is_within`/`check_windows_root_form`/`classify_final_path`/`windows_security::*` (9) → 10–21. `OpenOptions`/`FileTrust`/`OpenKind`/`WalkMode`/`OpenedFile::read_some`/`read_all`/`open_beneath`/`token_error_from` (10) → 11–18. `list_directory`/`EntryType` (13). `RootPolicy::create_private`/`create_directory_within`/`check_creatable_root`/`create_runtime_roots` (14) → 21. `snapshot_tree` (5) → 14. `SecretBytes`/`secure_wipe`/`generate_token`/`fill_random`/`base64url_encode` (15) → 16–23. `write_private_file`/`store_token`/`TokenStoreMode` (16, 17) → 21. `kDpapiTokenMagic` (17) → 18. `TokenReadResult` = `variant<SecretBytes, TokenReadError>`, `token_from_bytes`, `token_too_short` (18) → 19–23. `TokenVerifier`/`make_bearer_validator`/`detail::fixed_window_equals` (19) → 23. `init_frontend`/`write_default_config`/`generate_token(root, rel, force)` (21). `render_config_schema_markdown` (24). The free function `secure_frontend::generate_token()` (15) and the init helper `generate_token(root, rel, force)` (21) are overloads; Task 21 calls the former as `secure_frontend::generate_token()`.

**4. Review Focus** — all five pinned: (1) Task 13 `inventory_symlink_error_names_entry`; (2) Task 9 `cfg04_final_path_on_network_share_is_actionable`; (3) Task 18 `auth02_notepad_edits_rejected_with_reason`; (4) Task 11 `paths_handle_non_ascii_and_long_names`; (5) Task 13 `inventory_duplicate_detection_is_case_insensitive`. (6) is a process constraint, not a test: Task 2's helper names, `<binary>.<case>`, the `windows-runtime` label and the preset exclusion are frozen for SP2.

**Decisions recorded for the implementer (no run-time judgement left):** Task 5 — the four CLI cases run unlabelled under wine. Task 9 — every case that needs `open_root`/`open_executable` to succeed on Windows is labelled `windows-runtime` (engine-arguments, typed-config, subsystem-limits, server-CLI, server-e2e lists; Task 13 adds the root-opening inventory and tokenizer cases). Task 20 — Windows env scrub is the owner-ruled §3.6 deviation (CRT copies wiped, Win32 entry removed with `SetEnvironmentVariableW(name, nullptr)`). Tasks 22/23 — an acceptance failure means an earlier task diverged from the plan: stop and report. Task 10 — `openat2` is skipped in favor of the walk on `ENOSYS` and `EPERM` (seccomp). Task 17 — the SDDL adds `O:<user>` to the spec's form so an elevated administrator's files still pass `has_private_owner_dacl`. Task 18 — a token wrapped in matching quotes is rejected with a reason (Review Focus 3); the spec's printable-ASCII rule alone would accept it and fail later at authentication with no hint.

**5. Revision fixes (controller review, 2026-10-13)** — one line per ruled fix:
1. Task 3 `json_rejects_non_integer_numbers`: `"0x10"` now expects `trailing_data` (the reader takes `0`, then finds `x10`); the decoded-key duplicate case is `{"a":1,"\u0061":2}`.
2. Task 9: `classify_final_path`'s network message now says "choose a local folder on this computer", so `cfg04_final_path_on_network_share_is_actionable`'s `"local folder"` substring matches.
3. Task 14 POSIX `create_directory_within`: `ENOTDIR` (or `ELOOP`) from `openat(O_DIRECTORY|O_NOFOLLOW)` is followed by `fstatat(AT_SYMLINK_NOFOLLOW)` + `S_ISLNK`, before the base fd is closed, and reports `symlink_not_allowed`.
4. Task 16 `token_store_test.cpp`: `expect`, `root_at`, `rel`, `fresh_token` are `[[maybe_unused]]`, so the Windows cross build passes `-Werror` before Task 17.
5. Task 10 adds the Windows `OpenedFile::close_now`/`read_some` (beside the POSIX ones), so the common file links and Step 4's cross build is true; Task 11 no longer defines them and its Step 2 expects only the undefined `open_beneath`.
6. Line references re-verified at HEAD 7ddcd755: Task 1 `http_socket_server.cpp` check is lines 270-273 and `inet_pton` 288/293 (the brief's 268-271 pointed two lines above the `holds_alternative` statement; the plan names the statement by text too); Task 7 `listen` 327 and `handle_http_request` 411; Task 2 presets 115-124; Tasks 10/11 `token_reader.hpp` (62 lines: enum 11-24, Linux comment 36-39, Windows comment 41-58, declaration 59-60).
7. Worktree: every git command is `git -C "$(git rev-parse --show-toplevel)" …`; the `git switch -c` line is gone; shells start in `/opt/projects/ai/.worktrees/sp1/secure_frontend`; pytest is `"$(git rev-parse --show-toplevel)/.venv-dev/bin/pytest" -q`; `git status` expectations allow `?? .toolchains`/`?? .venv-dev`; every direct wine and `ctest --test-dir … -L windows-runtime` call sets `WINEPREFIX=$PWD/out/wineprefix WINEDLLOVERRIDES=mscoree,mshtml=`.
8. `--check` has no side effects: `validate()` only checks logs/cache (`check_creatable_root`, absent-but-creatable under a private parent accepted); `create_runtime_roots` creates them from `serve` (`run()`) and `init`; tests assert a byte-for-byte unchanged tree (`snapshot_tree` in `cli_check_prints_no_token`, new `cli_check_has_no_side_effects`, rewritten `cfg02_logs_and_cache_roots_are_created_private`).
9. Windows `make_scratch_directory`/`make_private_directory` create directories with an explicit protected DACL (user, SYSTEM, Administrators; test binaries link `advapi32`); Windows `open_executable` calls `GetFullPathNameW` before adding `\\?\`.
10. Prose-only steps replaced with code: Task 7 (`http_request_handler` hpp/cpp, `listen`/`handle_http_request` lines, `chat_schema` `valid_content(value, max_message_bytes)` + parse/render with limits, `chat_execution.cpp`, `chat_generation_plan` hpp/cpp, `run()`), Task 9 (`engine_arguments_test.cpp` in full, roots/executable blocks of `validate()`, `describe()` with `root_text`/`root_or_unset`, `main` edits), Task 14 (`canonical_root`'s policy prose superseded by `creatable_root`/`check_creatable_root` code), Task 21 (`--check`/`init` rejection in the `--check` branch, post-loop `--config` check, usage lines), Task 13 `main` edits; conditionals replaced by decisions (Task 2 includes, Task 5/9/13 wine label lists, Task 10 `#ifndef _WIN32` guard, Task 14 P9 grep, Task 19 `<cstdint>`, Tasks 22/23 failure handling, Task 24 README table).
11. §3.6 Windows env scrub recorded as the owner-ruled deviation (Global Constraints, Task 20 code wipes the narrow and wide CRT copies and calls `SetEnvironmentVariableW(name, nullptr)`, Task 24 handoff gap 3); P6 Windows twin `cfg03_tokenizer_file_swapped_to_junction_rejected` added in Task 13 (label `windows-runtime`).
12. `token_verifier.hpp` includes `<utility>`; `paths_test.cpp` includes `<algorithm>` (and `<vector>`) for `listing()`; Task 24's handoff records that product spec §4's `listen.*`, flat `limits.*` and `token_source` are superseded by SP1's `listener.*`, `limits.<section>.*` and `token.source`, which the future `strata` CLI follows.
13. Review Focus 6 and the Global Constraints state that Task 2 is a shared pre-task consumed by SP2 (helper signature, `<binary>.<case>`, `windows-runtime` label and preset exclusion frozen once on `main`), executed on `main` before the rest of SP1, with the SP1 worktree rebased onto it; Task 2 now registers only `main`'s cases (111 Linux / 102 wine) and Task 1 adds its five `net0*` cases.
