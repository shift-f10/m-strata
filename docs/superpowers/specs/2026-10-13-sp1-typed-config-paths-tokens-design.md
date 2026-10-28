# Sub-project 1 — Typed configuration, canonical paths, and tokens for `secure_frontend`

Date: 2026-10-13. Status: APPROVED by the owner 2026-10-13 19:03 UTC (open questions 1-5 resolved per recommendations). Parent
effort: Secure Strata full release path (SP0–SP7), see `docs/AGENT_HANDOFF.md`.
Base: `main` at `7f0da806`. Fact base:
[`../research/sp1-config-paths-tokens.md`](../research/sp1-config-paths-tokens.md)
(cited below as "research §n" and "defect (a)–(e)"). Sibling spec, drafted in
parallel: the **product-experience and upstream-tracking spec**, which owns the
`strata` CLI surface, the config **file format** and the **on-disk locations**.

## 1. Purpose

SP1 replaces the frontend's scattered hardcoded limits, ad-hoc path checks and
operator-supplied token handling with three primitives every later sub-project
builds on:

1. An **immutable, schema-validated configuration model** for the listener,
   request/time/concurrency limits, roots, token source and engine launch.
   Invalid configuration fails before any listener is bound or child started.
2. One **canonical-path module** that absorbs the nine duplicated path checks
   (research §3) and enforces root containment with handle-based walks on
   Linux and Windows.
3. **Secure token generation and storage**: CSPRNG, owner-only atomic files on
   Linux, DPAPI-wrapped private-DACL files on Windows, constant-time comparison
   without a length oracle, and no token in logs, argv, environment, children,
   metrics or core dumps.

It also lands the `NET-01..03`, `AUTH-01..04` and `CFG-01..06` acceptance tests
from `tests/security/README.md` by ID, fixes defects (a)–(e), and gives the
Windows token reader its first real runtime test (MSYS2 job).

## 2. Constraints

Carried from SP0 (§2 there) and the owner decisions in the handoff:

- Dev host: Linux, g++ 11.4, `.venv-dev` cmake/ninja, no root, wine 6.0.3 as
  smoke only. Kernel 5.15 (so `openat2` exists here; the fallback is still
  tested). glibc 2.35 (`getrandom`, `explicit_bzero` available).
- No third-party dependencies in the frontend; keep it that way. No
  `upstream/src/` (engine) change; if one is needed, stop and report.
- Binding rules: `threat-model.md:47-52` (CSPRNG token, constant-time compare,
  dev mode only loopback-only with warning, finite limits) and `:93-108`
  (schema-validated config, unknown keys are errors, no coercion, canonical
  roots, DPAPI allowed, metadata-only long-lived config, reject config files
  writable by untrusted users).
- North star: zero-config loopback first run with an auto-generated token; the
  token-on-loopback rule is **kept**.
- Stricter-than-upstream defaults are not weakened (handoff "Current security
  behavior"). Every defect gets a regression test that fails before its fix.
- Commits are local; the owner pushes. The handoff is updated as part of SP1.

## 3. Decisions

### 3.1 Config file format — strict JSON (assumption, to reconcile)

**Assumption:** the config file is a strict JSON object: UTF-8 without BOM, no
comments, no trailing commas, duplicate keys are an error, nesting depth ≤ 4,
file ≤ 64 KiB, integers only where a number is expected (no floats, no
exponents, no leading `+`, no strings-as-numbers). Upstream already uses JSON
run configs (`strata-<model>.json`, research §6), and the frontend already
contains two private bounded JSON readers (`chat_schema.cpp`,
`tokenizer_pack.cpp`); SP1 extracts one shared strict reader
(`secure_frontend_json`) that all three use. The product-experience spec owns
the file name and location; if it picks another format, only the reader
changes — the typed model, validator and tests in this spec do not.

Rejected: TOML (needs a hand-written parser for a richer grammar); flat
`key=value` (no arrays for `engine.args`, ambiguous types); environment
variables as a config source (invisible to review; upstream's `STRATA_*`
overrides are deliberately not mirrored).

### 3.2 Sources and precedence

Built-in defaults < config file (`--config <file>`) < command-line flags. Every
existing `strata-secure-server` flag maps to exactly one key (§4.2 table), and
flags and file go through the **same** validator. A key set in both the file
and on the command line is allowed (flag wins) and is reported in `--check`
output, never silently. The only environment variable the frontend reads is the
one named by `token.source = env`.

Rejected: flag-only (today; unusable for the north-star first run); file-only
(breaks every existing script and test).

### 3.3 Immutability

Parsing produces a mutable `FrontendConfigDraft` (all fields `std::optional`).
`validate(draft)` returns `std::variant<ValidatedConfig, ConfigErrors>` and
collects **all** errors, each naming the key path (`limits.http.max_request_bytes`)
and a fixed reason, never the offending value. `ValidatedConfig` has a private
constructor, `const` members and is held as `std::shared_ptr<const
FrontendConfig>`; subsystem limit structs (`HttpSocketServerLimits`,
`ChatExecutionLimits`, …) are derived from it once, before bind/spawn, and are
themselves passed by value into private `const` members.

Rejected: validating in each subsystem constructor (errors arrive after the
listener is up; violates CFG-01).

### 3.4 Canonical-path module

One library, `secure_frontend_paths`, replaces every path check (§4.3). Rules:

- **Roots** (`models`, `tokenizer_pack`, `secret`, `logs`, `cache`) must be
  absolute. The root path *itself* may traverse symlinks/junctions: it is
  resolved once at startup to its canonical form, then opened as a handle
  (Linux `O_PATH|O_DIRECTORY|O_CLOEXEC`; Windows
  `FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT` with `READ_CONTROL`).
  The root must be owned by the current user (or root/SYSTEM/Administrators)
  and not writable by group/other (Linux) or by any SID other than
  user/SYSTEM/Administrators (Windows).
- **Below a root**, every path is a portable relative path in one grammar
  (§4.3 P1) and is opened by a handle walk that refuses links: Linux
  `openat2(RESOLVE_BENEATH|RESOLVE_NO_SYMLINKS|RESOLVE_NO_MAGICLINKS|RESOLVE_NO_XDEV)`
  when the syscall exists, else a component-wise `openat(O_NOFOLLOW)` walk with
  `st_dev` checks; Windows per-component `CreateFileW(OPEN_REPARSE_POINT)` with
  `FileAttributeTagInfo` reparse rejection and `GetFinalPathNameByHandleW`
  containment compared with `CompareStringOrdinal` (ignore case) on a separator
  boundary.
- The engine receives the **canonical** root joined with the validated
  relative path (fixes defect (e)). The engine opens files by path after
  validation, so a swap below a user-owned, non-shared-writable root remains a
  same-user race; this is an accepted, documented risk until SP2 decides on
  handle passing.
- **Executable** (`engine.executable`) is a file path, not a root: absolute,
  canonicalized, regular file, not group/other-writable, and its parent
  directory likewise.

Rejected: `std::filesystem::canonical`/`weakly_canonical` as the check (path
based, races the open); allowing symlinks below a root when their target stays
inside it (needs a resolver of our own; see open question 5).

### 3.5 Token generation, format and storage

- **Generation:** 32 bytes from `getrandom(buf, n, 0)` (loop on `EINTR` and
  short reads; `ENOSYS` → fail closed, no `/dev/urandom` fallback, glibc 2.25+
  and Linux 3.17+ are the floor) or `BCryptGenRandom(nullptr, …,
  BCRYPT_USE_SYSTEM_PREFERRED_RNG)` (link `bcrypt`). Encoded base64url without
  padding: 43 ASCII characters, no prefix.
- **Linux storage:** secret root directory created `0700` and owned by `euid`;
  file written to a same-directory temp via
  `openat(O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC, 0600)`, `fchmod 0600`,
  write token + one `\n`, `fsync`, `renameat` (`renameat2(RENAME_NOREPLACE)`
  for first creation), `fsync(dirfd)`. A rotate replaces atomically.
- **Windows storage: DPAPI from day one.** `CryptProtectData` (user scope,
  `CRYPTPROTECT_UI_FORBIDDEN`, link `crypt32`) wraps the token; the file is a
  fixed 8-byte magic `STRATAK1` followed by the DPAPI blob. It is created with
  `CREATE_NEW` and an explicit protected security descriptor
  `D:P(A;;FA;;;<UserSID>)(A;;FA;;;SY)` (matching `has_private_owner_dacl`),
  `FlushFileBuffers`, then `MoveFileExW(REPLACE_EXISTING|WRITE_THROUGH)`.
  The reader accepts the DPAPI form and, for operator-made files, a plaintext
  token under the same DACL rules (so `deployment.md`'s "use env on Windows"
  advice can be retired).
- **Reader format rule (both OSes):** the token is 1–256 bytes of printable
  ASCII `0x21–0x7E`; exactly one trailing `\n` or `\r\n` is stripped (fixes
  defect (c)); anything else (BOM, inner whitespace, two newlines) is
  `invalid_token`. Operator-chosen tokens must be ≥ 32 bytes (open question 2
  covers whether to warn or reject shorter ones; recommendation: reject).

Rejected: DPAPI later (the research already has the API; deferring leaves a
plaintext-at-rest default on Windows); hex encoding (64 chars, no benefit).

### 3.6 Secret lifetime and comparison

- `SecretBytes`: move-only, non-copyable, fixed-capacity buffer; wiped with
  `explicit_bzero` / `SecureZeroMemory` on destruction **and** on move-from
  (fixes the SSO remnant noted in research §2). All read buffers (file chunks,
  DPAPI output, which is wiped before `LocalFree`) are wiped.
- One `TokenVerifier` in the library replaces the two copies in the mains.
- **Comparison without a length oracle:** both the stored and the presented
  token are copied into fixed 256-byte zero-padded windows; the comparison XORs
  all 256 bytes and folds `stored_len ^ presented_len` into the accumulator.
  A presented value longer than 256 bytes is rejected before comparison; that
  only reveals the public maximum. Rejected: SHA-256-then-compare (needs an
  in-tree hash now; SP4 will bring SHA-256 and may revisit).
- **Environment source:** after reading, the value bytes are overwritten in
  place in the process environment block and the variable is unset, so
  `/proc/<pid>/environ` and children never carry it.
- **Core dumps:** Linux `prctl(PR_SET_DUMPABLE, 0)` and `RLIMIT_CORE = 0` in
  `install_process_defaults()` for `strata-secure-server`; Windows
  `SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX)`. Full Windows
  Error Reporting policy is SP7 packaging. `mlock` is not required.

### 3.7 Listener address classification

The host is parsed once with `inet_pton` into bytes; loopback/wildcard/remote
are decided from the bytes (`IN6_IS_ADDR_UNSPECIFIED`, `IN6_IS_ADDR_LOOPBACK`,
`IN6_IS_ADDR_V4MAPPED` + IPv4 `127/8` / `0.0.0.0`). The canonical text form is
what is bound and logged. Hostnames are rejected at validation (today they pass
and fail at `bind()`), with one exception: the exact string `localhost`
(ASCII case-insensitive) maps to `127.0.0.1` with no resolver call.

### 3.8 AUTH-04 — no development mode

No unauthenticated mode exists or is added. AUTH-04 is satisfied by tests that
prove an empty or absent token is rejected on every bind, including loopback,
and that no flag, key or environment variable disables authentication.
Rejected: a loopback-only dev switch (more code, conflicts with the kept
token-on-loopback rule, and the north star already makes the token automatic).

### 3.9 `init` split with the product-experience spec

SP1 owns the **mechanics**: library functions `write_default_config(dir)` and
`generate_token(secret_root, rel)` (both atomic, both refusing to overwrite
without `--force`), exposed as `strata-secure-server init --config <file>
[--force]` so the acceptance tests can run the whole path end to end. The
product-experience spec owns the **UX**: default directories, the `strata`
wrapper, what is printed, `key show`/`key rotate`. Rejected: putting all of
`init` in the product spec (SP1's CFG-05/06 tests would have no generator to
exercise); putting the UX here (duplicates the sibling).

### 3.10 Test registration by acceptance ID

Test binaries stay framework-free `main()` programs but take `--case <name>`.
A CMake helper `secure_frontend_add_cases(<binary> <case>...)` registers each
case as its own CTest test, so acceptance names (`cfg03_…`) appear in `ctest
-N`. Tests that need real Windows semantics carry the CTest label
`windows-runtime`; the wine preset runs with `-LE windows-runtime`, the MSYS2
job runs everything. Fixed `/tmp/...XXXXXX` fixtures move to per-test
`mkdtemp` roots under the build tree.

### 3.11 `ready_timeout`

Default raised from 30 s to **900 s** (upstream `STRATA_ENGINE_READY_S`),
configurable as `limits.engine.ready_timeout_s` in `1..86400`.

## 4. Data model sketch

```cpp
struct ListenerConfig { IpAddress host{127.0.0.1}; uint16_t port{8080};   // 1..65535
  bool allow_remote{false}; bool allow_wildcard_bind{false};
  uint32_t backlog{16}; };                                                 // 1..4096
struct HttpLimits { milliseconds request_deadline{30'000};                 // 1 ms..3600 s
  uint32_t max_request_bytes{65'536};    // 4 KiB..16 MiB, parser honors it
  uint32_t max_header_bytes{16'384};     // 1 KiB..64 KiB, <= max_request_bytes
  uint32_t max_headers{64};              // 8..256
  uint32_t max_request_line_bytes{4'096}; };                               // 256..16 KiB
struct ChatLimits { uint32_t max_body_bytes{262'144};  // <= max_request_bytes - max_header_bytes
  uint32_t max_messages{128}; uint32_t max_message_bytes{65'536};
  uint32_t max_prompt_tokens{32'768}; uint32_t max_completion_tokens{4'096}; // <= 16384
  uint32_t default_max_tokens{256}; uint32_t max_generated_bytes{1 << 20}; };
struct EngineLimits { seconds ready_timeout{900}; seconds generation_timeout{120};
  seconds stop_drain_timeout{5}; };
struct ConcurrencyLimits { uint32_t max_concurrent_requests{1}; };  // only 1 accepted until SP3
struct Limits { HttpLimits http; ChatLimits chat; EngineLimits engine; ConcurrencyLimits concurrency; };
struct Roots { CanonicalRoot models; std::optional<CanonicalRoot> tokenizer_pack;
  std::optional<CanonicalRoot> secret, logs, cache; };   // logs/cache created 0700, unused in SP1
struct TokenSource { enum class Kind { env, file } kind{Kind::file};
  std::string env_name;                  // [A-Za-z_][A-Za-z0-9_]{0,127}
  RelativePath file{"token"}; };          // below roots.secret; metadata only
struct EngineLaunch { Profile profile{qwen}; CanonicalFile executable;
  ModelId model_id; std::vector<std::string> args; };  // args: SP0 deny-list applies
struct FrontendConfig { ListenerConfig listener; Limits limits; Roots roots;
  TokenSource token; EngineLaunch engine; };
```

`FrontendConfig` is immutable after validation (private ctor, `const` members,
held as `shared_ptr<const>`); all five member structs are immutable with it.
`static_assert`s and a type-trait test prove no member of `FrontendConfig` is
`SecretBytes` or derived from the token (CFG-06). `CanonicalRoot`,
`CanonicalFile`, `RelativePath`, `ModelId` and `IpAddress` can only be built by
their validators.

Constants that **stay internal** (format or protocol facts, not tuning):
`kProtocolMaxLineBytes` 16 MiB and the 8192-byte inbound record cap
(`engine-protocol-limits.md`); `EngineStdioProcess` buffer sizes; process
timings (10 ms poll, 1 s post-kill, 250 ms grace — SP2 owns them); POSIX launch
`kReportFd`/`kLiftFloor`/`kSweepCap`; send minimum budget 1000 ms; EMFILE
backoff 50 ms; half-close drain 100 ms / 256 KiB; tokenizer-pack caps (64 MiB /
1 Mi entries / 16 KiB token / 8 MiB decoded); model id ≤ 128; router model
field ≤ 1024; token ≤ 256 bytes; token file ≤ 4 KiB (was 1 MiB); owned and
unsupported engine flags.

## 5. Scope — items and their regression tests

Each row is one task unit: test first (fails), fix, test passes, full `ctest`
green. "Both" = Linux `ctest` and the MSYS2 job; "Linux" = Linux only; "Win" =
Windows-only, label `windows-runtime`, authoritative in the MSYS2 job.

### 5.1 Network classification and NET-*

| Item | Defect / requirement | Fix | Regression test(s) | Platform |
| --- | --- | --- | --- | --- |
| N1 (**Task 1**) | Defect (a): `::0`, `0::0`, `0:0:0:0:0:0:0:0`, `::0.0.0.0`, `::ffff:0.0.0.0` bypass `--allow-wildcard-bind`; `0:0:0:0:0:0:0:1`, `::ffff:127.0.0.1` misread as remote. | §3.7 byte classification; canonical text form bound. | `net03_wildcard_spellings_need_wildcard_opt_in`, `net03_remote_opt_in_alone_rejects_wildcard`, `net01_loopback_spellings_classified_from_bytes` | Both |
| N2 | DNS names pass validation then fail at `bind()`. | Reject hostnames; `localhost` → `127.0.0.1` without resolution. | `net02_hostname_rejected_before_bind`, `net01_localhost_maps_to_ipv4_loopback` | Both |
| N3 | NET-01 only tested at config level. | None unless it fails. | `net01_default_listener_is_loopback_only`: start server with defaults + fake engine, enumerate listening sockets of the pid (`/proc/net/tcp*` on Linux, `GetExtendedTcpTable` on Windows). | Both |
| N4 | NET-02 matrix incomplete. | None unless it fails. | `net02_remote_addresses_need_opt_in` (`0.0.0.0`, `::`, `172.16.1.10`, `10.0.0.1`, `203.0.113.5`, `2001:db8::1`, `fd00::1`); `net02_error_is_non_secret_and_exits_before_bind` (CLI exit code, stderr contains key path, no token, fake engine not launched). | Both |

### 5.2 Typed configuration and CFG-01

| Item | Defect / requirement | Fix | Regression test(s) | Platform |
| --- | --- | --- | --- | --- |
| C1 | Three private JSON readers. | Extract `secure_frontend_json` (strict, bounded, duplicate-key error); chat schema and tokenizer pack use it with unchanged behavior. | Existing chat-schema and tokenizer-pack tests unchanged; `json_rejects_duplicate_keys`, `json_rejects_depth_over_limit`, `json_rejects_non_integer_numbers` | Both |
| C2 | Limits scattered, defaulted, unvalidated (research §1). | §4 model; `validate()` with ranges and cross-field rules. | `cfg01_unknown_key_rejected`, `cfg01_duplicate_key_rejected`, `cfg01_type_confusion_rejected` (`"8080"`, `8080.0`, `true` for port; `1` for bool), `cfg01_missing_required_rejected`, `cfg01_out_of_range_rejected` — each table-driven over **every** key | Both |
| C3 | Failure can happen after bind/spawn. | Mains build `ValidatedConfig` before anything else. | `cfg01_invalid_config_fails_before_listener_and_child`: fake engine writes a marker if started; port probed free afterwards. | Both |
| C4 | Defect (d): parser has its own 64 KiB/16 KiB/64-header caps; raising `max_request_bytes` has no effect. | Parser reads `HttpLimits`. | `parser_honors_configured_max_request_bytes` (128 KiB body accepted at 256 KiB config, rejected at default) | Both |
| C5 | `ready_timeout` 30 s. | 900 s default, configurable. | `ready_timeout_defaults_to_900_seconds`, `ready_timeout_is_configurable` (fake engine delays READY 2 s, config 1 s → `503`) | Both |
| C6 | No `--config`; flags only. | `--config <file>`, `--check` (validate, print effective non-secret config, exit), flag→key mapping, precedence §3.2. | `cli_config_file_and_flags_share_validator`, `cli_flag_overrides_file_and_is_reported`, `cli_check_prints_no_token` | Both |
| C7 | Config files writable by others accepted (threat model :107-108). | Linux: owner `euid`, no group/other write, file and parent opened via §3.4; Windows: DACL write only for user/SYSTEM/Administrators. | `cfg01_group_writable_config_rejected` (Linux), `cfg01_config_with_everyone_write_ace_rejected` (Win) | Linux / Win |
| C8 | `unknown option: <arg>` echoes the argument (could be a pasted token). | Echo only the flag name up to `=` and at most 64 printable bytes; never a value. | `cli_unknown_option_does_not_echo_value` | Both |

### 5.3 Canonical-path module and CFG-02..04

| Item | Defect / requirement | Fix | Regression test(s) | Platform |
| --- | --- | --- | --- | --- |
| P1 | Two relative grammars disagree (research §2, §3). | One grammar: `/`-separated (also `\` on input, normalized), components 1–255 bytes, total ≤ 1024; reject empty/`.`/`..`, leading `/` or `\`, `:` anywhere (drive, ADS), `<>"|?*`, bytes < 0x20 and 0x7F, NUL, trailing dot/space, reserved device names case-insensitive with any extension (`CON PRN AUX NUL COM0-9 LPT0-9`, superscript digits, `CONIN$ CONOUT$ CLOCK$`), 8.3 aliases (`~` + digit). Non-ASCII UTF-8 allowed, validated. | `cfg02_rejects_traversal_and_absolute`, `cfg02_rejects_nul_and_control_bytes`, `cfg04_rejects_reserved_and_device_names`, `cfg04_rejects_trailing_dot_space_and_8dot3`, `cfg04_rejects_ads_and_drive_forms` | Both (string level) |
| P2 | Root paths never validated. | Root rules §3.4: absolute, canonicalized once, owner/writability check. Windows also rejects drive-relative `C:foo`, rooted `\foo`, `\\server\share`, `\\?\`, `\\.\`, `\??\`. | `cfg02_rejects_relative_root`, `cfg02_rejects_shared_writable_root` (Linux `0777` dir), `cfg04_rejects_unc_device_and_extended_roots` (Win) | Linux / Win |
| P3 | Containment ×3 implementations; sibling prefix. | Linux: containment by construction (walk from root fd); Windows: final-path compare on separator boundary. | `cfg02_sibling_prefix_root_rejected` (`/r` vs `/r2`, `C:\r` vs `C:\r2`) | Both |
| P4 | Linux path checks race the open; intermediates unchecked; `openat2` unused. | `openat2` walk with `openat(O_NOFOLLOW)` fallback; `st_dev` must match root; non-regular final rejected; `st_nlink > 1` rejected for the token file. | `cfg03_rejects_symlinked_intermediate_directory`, `cfg03_rejects_symlinked_final_file`, `cfg03_rejects_mount_crossing` (skips without a second fs), `cfg03_fallback_walk_matches_openat2` (forces fallback mode), `cfg03_rejects_hardlinked_token_file` | Linux |
| P5 | Windows reader opens dirs with `FILE_READ_ATTRIBUTES` only; `GetSecurityInfo` needs `READ_CONTROL` (defect (b)). | Request `READ_CONTROL|FILE_READ_ATTRIBUTES`; reparse tag check per component; `nNumberOfLinks > 1` rejected. | `auth02_windows_token_file_reads_with_private_dacl` (fails before fix), `cfg03_rejects_junction_below_root` (junction via `FSCTL_SET_REPARSE_POINT`, no privilege needed), `cfg03_rejects_symlink_below_root` (skips if `SeCreateSymbolicLink` absent) | Win |
| P6 | Inventory, tokenizer pack and engine join use their own checks; tokenizer `ifstream` follows symlinks. | Inventory enumerates through the module; tokenizer files opened by handle then read from it. | `cfg03_inventory_rejects_symlink_entry` (existing, moved), `cfg03_tokenizer_file_swapped_to_symlink_rejected` | Linux (+ Win junction variant) |
| P7 | Defect (e): engine receives raw `--models-root`. | Engine argv built from `CanonicalRoot` + `RelativePath`. | `cfg03_engine_receives_canonical_models_root`: root given through a symlink; fake engine `DUMP` shows the canonical path | Linux |
| P8 | Logs/cache roots do not exist. | Validated, created `0700` / private DACL via the module's `create_directory_within(root, rel)`; nothing writes into them in SP1. | `cfg02_create_within_root_writes_nothing_outside` (attempted escapes leave the parent listing unchanged) | Both |
| P9 | Duplicated reserved-name/control/split code. | Old helpers deleted; `secret_source`, `model_inventory`, `engine_arguments`, `tokenizer_pack`, `token_reader` call the module. | Existing suites stay green; a grep check in review that no second reserved-name list remains | Both |

### 5.4 Token generation, storage and CFG-05/06

| Item | Defect / requirement | Fix | Regression test(s) | Platform |
| --- | --- | --- | --- | --- |
| K1 | No generator. | §3.5 CSPRNG + base64url. | `token_generate_is_43_base64url_chars`, `token_generate_differs_across_calls`, `base64url_known_vectors` | Both |
| K2 | No Linux writer. | Atomic owner-only write §3.5. | `token_store_creates_0600_file_in_0700_dir`, `token_store_rotate_is_atomic` (reader sees old or new, never partial), `token_store_refuses_symlinked_target` | Linux |
| K3 | No Windows writer; no DPAPI. | DPAPI + protected DACL §3.5. | `token_store_windows_round_trips_through_dpapi`, `token_store_windows_file_has_private_dacl`, `token_reader_windows_accepts_plaintext_under_private_dacl` | Win |
| K4 | `token_reader_tests` is UNIX-only; Windows reader never run. | Un-gate the target; split cases into portable / Linux / Win; fix the CMake comment ("MSVC/Windows runtime coverage" → MSYS2). | The whole binary runs in the MSYS2 job; `windows-runtime` label excluded from wine | Both |
| K5 | Defect (c): trailing `\n` rejected. | Strip exactly one `\n` or `\r\n`. | `auth02_token_file_with_trailing_lf_accepted`, `…_crlf_accepted`, `…_two_newlines_rejected`, `…_bom_rejected` | Both |
| K6 | Buffers not wiped; SSO remnant; smoke lambda copy. | `SecretBytes`, single `TokenVerifier`, wiped read buffers. | `secret_bytes_wiped_on_destroy_and_move` (inspects storage through a test hook) | Both |
| K7 | Length leak in `constant_time_token_equals`. | Fixed 256-byte window §3.6. | `auth03_compare_touches_fixed_window` (instrumented byte count equal for 1-, 43-, 256-byte inputs), `auth03_prefix_and_suffix_variants_rejected`, `auth03_router_uses_verifier` | Both |
| K8 | Env token stays in `environ`/`/proc/<pid>/environ`. | In-place wipe + `unsetenv` §3.6. | `cfg05_env_token_absent_from_proc_environ` | Linux |
| K9 | No core-dump protection. | §3.6 `install_process_defaults()`. | `server_process_is_not_dumpable` (reads `/proc/<pid>/status` `Dumpable`/`prctl`) | Linux |
| K10 | No `init`. | §3.9 `init --config <file> [--force]`. | `init_writes_config_and_token_then_server_starts`, `init_refuses_to_overwrite_without_force` | Both |
| K11 | CFG-05 not tested end to end. | None unless it fails. | `cfg05_token_absent_from_child_argv_env_and_outputs`: start with a known token and fake engine `DUMP`; scan child argv/env, server stdout/stderr, every error body, `/health`, `/v1/models`, `/proc/<pid>/cmdline` (Linux) | Both (proc parts Linux) |
| K12 | CFG-06 not tested. | Type-level guarantee §4. | `cfg06_config_has_no_secret_typed_member` (compile-time), `cfg06_validated_config_bytes_do_not_contain_token` (runtime scan of the object graph via its `describe()` dump) | Both |

### 5.5 Authentication AUTH-01..04

| Item | Requirement | Fix | Regression test(s) | Platform |
| --- | --- | --- | --- | --- |
| A1 | AUTH-01 over every public route via router and socket. | None unless it fails. | `auth01_every_route_rejects_missing_malformed_wrong_token` (missing, `bearer` lowercase, `Bearer` + empty, tab separator, duplicate header → `400`, wrong token → `401`; body has no route/model data) | Both |
| A2 | AUTH-02 token absent from response and outputs. | None unless it fails. | `auth02_valid_token_succeeds_and_is_not_echoed` (socket round trip, captured stdout/stderr) | Both |
| A3 | AUTH-03 (K7). | — | (K7 tests) | Both |
| A4 | AUTH-04: no dev mode (§3.8). | None. | `auth04_empty_token_rejected_on_loopback`, `auth04_no_switch_disables_auth` (every flag, key and `STRATA_*` env name in the schema list checked) | Both |

### 5.6 Documentation and tooling

| Item | Change |
| --- | --- |
| D1 | `docs/AGENT_HANDOFF.md`: SP1 rows in "What has changed"; "Verification performed" rerun; Known small gaps: remove the wildcard and CMake-comment items when fixed. |
| D2 | `docs/security/deployment.md:79-83` and the Windows section: DPAPI file is the Windows default once K3/K4 are green in the MSYS2 job; until then the text says so. |
| D3 | `secure_frontend/README.md`: config keys table (generated from the schema list, one source of truth), `--config`, `--check`, `init`. |
| D4 | `scripts/dev/bootstrap-llvm-mingw.sh` deletes the archive after a digest mismatch (handoff gap). |
| D5 | `docs/security/engine-protocol-limits.md` notes which limits became config keys and which stay internal (§4). |

## 6. Review focus — inputs no test pins

1. **Models stored on another disk via symlinked files** (`~/models/x.gguf ->
   /mnt/big/x.gguf`): the whole inventory fails today and still will; the
   error message must say which entry and why (open question 5).
2. **Windows profiles with redirected or synced folders** (OneDrive Known
   Folder Move, roaming `%APPDATA%` on a share): final paths become UNC or
   reparse points and are rejected; check the error is actionable.
3. **Token file edited in Notepad**: UTF-8 BOM, trailing spaces, or pasted with
   quotes; only one LF/CRLF is tolerated.
4. **Non-ASCII user names and long paths** (`C:\Users\José\…`, > 260 chars
   without a long-path manifest): UTF-8 ↔ UTF-16 round trip and `\\?\` handling
   inside the module (prefix is produced internally, never accepted as input).
5. **Case-folding collisions**: `Qwen.gguf` and `qwen.gguf` on a
   case-sensitive Linux fs, or ext4 casefold / NTFS per-directory case
   sensitivity; inventory duplicate detection must be case-insensitive.

## 7. Out of scope (explicit)

The `strata` CLI, config file name/location, default directories and printed
UX (product-experience spec); `x-api-key`, multiple keys, per-app tokens
(SP5/product); concurrency above 1, connection limits, streaming, `DOS-*`,
`WEB-*`, `EXT-*` (SP3); `PROC-*`, handle passing to the engine, eager READY,
restart/backoff, process timings as config (SP2); `acquire` and SHA-256 (SP4);
logging implementation (roots only); Windows Error Reporting policy and
installers (SP7); any `upstream/src/` change.

## 8. Execution model

- Branch `sp1-typed-config-paths-tokens` from `main`. Orchestrator: the main
  session; implementation and review by subagents, one task at a time, each
  followed by a spec-compliance review and a code-quality review by separate
  agents.
- Each task: regression test first (fails), fix, test passes, full `ctest
  --preset linux-gcc` green, cross build green, one local commit with test and
  fix together; the commit message names the item ID and test names.
- Task order: N1 first (owner-flagged security defect), then test
  registration (§3.10) and C1, then config (C2–C8), paths (P1–P9), tokens
  (K1–K12), auth (A1–A4), docs (D1–D5). Windows-only tasks are written and
  cross-compiled on the dev host and marked "pending MSYS2" in the handoff
  until the owner's push runs CI.
- The orchestrator reruns `ctest` itself before accepting a task. Before any
  task touches anything under `upstream/` (the submodule) or our root `CMakeLists.txt`, or
  installs anything outside `.venv-dev/` and `.toolchains/`, work stops and
  the owner decides.

## 9. Acceptance for sub-project 1

1. Dev host: `ctest --preset linux-gcc` green, including every `net0*`,
   `auth0*` and `cfg0*` test above; `ctest -N` lists each acceptance-named
   case individually.
2. `windows-llvm-mingw-cross` builds with `-Werror`; `ctest --preset
   windows-llvm-mingw-cross -LE windows-runtime` passes under wine; `wine
   strata-secure-server.exe --help` and `--check` on a bad config exit as
   specified.
3. CI after the owner pushes: `linux-gcc`, `linux-clang`,
   `windows-llvm-mingw-cross`, `top-level-integration` and **`windows-msys2-clang`
   green, including the un-gated `secure_frontend_token_reader_tests` and all
   `windows-runtime` cases** (DPAPI round trip, private DACL, junction,
   READ_CONTROL fix). `windows-msvc` reported, non-blocking.
4. Defects (a)–(e) and the hostname defect each have a named failing-then-
   passing test recorded in its commit message.
5. No hardcoded tuning limit remains outside `FrontendConfig` except the
   internal constants listed in §4.
6. `pytest -q` at the root still passes; `docs/AGENT_HANDOFF.md`,
   `deployment.md` and the frontend README describe the state accurately;
   nothing is claimed as Windows-runtime-verified beyond the CI logs.

## 10. Open questions for the owner

1. **Config file format.** Strict JSON (§3.1) — must match the
   product-experience spec. *Recommendation:* JSON, reconciled in both specs
   before planning.
2. **Operator-supplied short tokens** (< 32 bytes via `--token-file` or
   `--token-env`). *Recommendation:* reject at startup with a hint to run
   `init`; generated tokens are always 43 characters.
3. **Windows plaintext token files.** DPAPI is what SP1 writes; should the
   reader keep accepting a plaintext file under a private DACL?
   *Recommendation:* yes, with the same DACL rules, so existing operator setups
   keep working; revisit at SP7.
4. **`localhost` as a host value.** *Recommendation:* accept exactly
   `localhost` as `127.0.0.1` without DNS (friendlier, no resolver), reject all
   other hostnames.
5. **Symlinks below the models root** (models on another disk).
   *Recommendation:* keep rejecting them in SP1; users point `roots.models` at
   the other disk (the root itself may be a symlink). Reconsider a
   "target must stay inside an allowed root" rule only with a handle-based
   resolver and its own tests.
