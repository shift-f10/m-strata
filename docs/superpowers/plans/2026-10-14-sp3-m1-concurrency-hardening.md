# SP3 M1 — Concurrency and HTTP Hardening Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. The owner chose **subagent-driven-development** for this plan.

**Goal:** Replace the serial `serve_one` loop of `strata-secure-server` with an accept thread plus a fixed worker pool and a FIFO generation gate, authenticate every request as soon as its header is complete (before any body byte is read), enforce every HTTP/DoS bound of spec §4–§5 with the fixed JSON error model of §7, and land every `NET-*`, `WEB-*`, `DOS-*`, `EXT-*` acceptance case that does not need streaming, plus the frontend egress tripwires, on Linux and Windows.

**Architecture:** The request is split into a header stage (`RequestRouter::authorize`, §4.1 steps 1–7, CPU only) and a body stage (`RequestRouter::handle`). A connection handler drives one accepted, non-blocking socket through phases P1–P8 with 20 ms polled reads, deadlines and abort flags. A `ConnectionPool` owns one accept thread (100 ms slices, per-remote cap, oldest-P1 eviction, refusals on a lingering-close list) and `max_concurrent_requests` workers. Chats pass `ChatRoute` (schema, tokenization, `max_tokens` resolution, context rule) and then a `GenerationGate` that owns the single running slot in front of SP2's `EngineSupervisor`, which every SP3 call reaches through `SupervisorPort` (one mutex). Egress is checked by three test-side tripwires (import table, `LD_PRELOAD` interposer, connection-table scan).

**Tech Stack:** C++20 (g++ 11.4 locally; clang in CI), CMake + Ninja from `.venv-dev/`, llvm-mingw cross build (clang, `-Werror`), wine 6.0.3 (smoke only), GitHub Actions `windows-msys2-clang` (authoritative Windows runtime). OS APIs only: POSIX sockets/`poll`, `/proc`, `dlfcn`; Winsock 2 (`ws2_32`, `WSAPoll`), `iphlpapi` (test binaries only).

**Spec:** `docs/superpowers/specs/2026-10-14-sp3-api-concurrency-streaming-design.md` (the authority; §11.1 decisions resolved by the owner on 2026-10-14 as B1–B12 = the recommended options; engineering choices E1–E10 stand). Interfaces consumed, not redesigned: SP1 spec/plan (`2026-10-13-sp1-typed-config-paths-tokens*`) and SP2 spec/plan (`2026-10-13-sp2-portable-supervision*`). M2 (streaming) is a separate plan that starts after this one is merged.

## Controller notes (rule on these before execution)

Each line is a place where the spec and the SP1/SP2 code or plans disagree, or where this plan makes a choice the spec leaves open. The spec wins for SP3 behavior; the plan does what the line says unless the controller rules otherwise.

1. **cfg01 binary name.** Spec §9.3 names `config_tests.cfg01_*`/`config_tests.web05_*`; SP1's schema cases live in `secure_frontend_typed_config_tests` (`tests/typed_config_test.cpp`). This plan registers the SP3 config cases there, with the spec's case names.
2. **JSON numbers.** SP1's `secure_frontend_json` is integer-only (`non_integer_number`); decision B6 needs fractional `temperature`/`top_p`. Task 7 adds an opt-in `JsonLimits::allow_fractions` (default `false`, so config files stay integer-only) and `JsonKind::decimal` — an SP1 mechanism extension, recorded as SP1 addendum A3.
3. **Chat schema.** SP1's `parse_chat_completion_request` answers `max_tokens > max_completion_tokens` with a parse failure and uses JSON depth 3; spec §3.1 / B2 clamps and uses depth 4 (`kChatMaxJsonDepth`). Task 7 changes both.
4. **READY context.** SP2's `Lease` has no context accessor (READY reaches `ChatExecutionService::adopt_ready`). Task 15 adds `ChatExecutionService::context_tokens()` and `run_generation(...)` (additive; SP2's `execute`/`execute_with_lease` stay for SP2's own tests).
5. **Fake engine.** SP2's fake reads stdin on the main thread and answers `GEN` synchronously. M1 needs slow, stoppable generations, so Task 14 moves stdin to a reader thread and adds `--token-ids`, `--tokens`, `--token-interval-ms`, `--finish`, `--prefill-delay-ms`, `--stop-delay-ms` and the `fake_engine_stream_modes_selftest` case; `--progress-records`, `--crash-mid-gen`, `--err-mid-gen` and the UTF-8 tokenizer ids stay for M2.
6. **README key table.** SP1's generated table has the columns Key/Type/Default/Range/Option/Meaning; spec §5.1 adds "Cross-field rule". The plan keeps SP1's columns, states the rule inside Meaning, and renders derived defaults as `derived: min(4, max(0, workers-2))` / `derived: min(4, workers)`.
7. **What M1 does where §3.4/§3.3 are M2.** §9.3 places decision B4 (timeout → partial text, `length`) and the UTF-8 stream decoder in M2. Until M2: a non-streaming generation timeout answers `503 engine_error` (`x-should-retry: false`), an undecodable output `500 internal_error`, and `"stream": true` answers `400 unsupported_parameter` with `param` `"stream"`.
8. **SP2 pieces still unbuilt at plan time.** SP2 Tasks 6b, 15, 27 and 29 (server wiring through `execute_with_lease`, `install_shutdown_signals`/`shutdown_signalled()`, `HttpSocketServer::shutdown_listener()`, `ext01_extension_fields_spawn_nothing`, the harness constants `kGeneric503`/`kBadRequest`) existed only in the SP2 plan when this plan was written. Task 0 stops if any is missing on `main`.
9. **`iphlpapi`.** Spec §1.6 allows it "in one test only"; SP1 already links it into `secure_frontend_server_e2e_tests`, and the `net01_*` cases of `api_server_tests` need `GetExtendedTcpTable` too. The plan links it into `secure_frontend_api_server_tests` and `secure_frontend_egress_tests` (test binaries only; never a product target).
10. **Static tripwire negative case.** `egress_static_tripwire_fails_on_binary_without_dynamic_section` uses synthetic ELF and PE byte images written by the test instead of a static-linked helper executable (no `libc.a` dependency in CI). Per ruling R-I the images go through the real parser paths: an ELF with a section table holding `SHT_DYNAMIC` but no `SHT_DYNSYM` (must fail), an ELF whose `.dynsym` imports `getaddrinfo` (must be flagged), and a PE whose import directory names `WS2_32.dll` with an ordinal-52 thunk (must be flagged through `read_pe_imports` → `forbidden_imports`).
11. **CI edit.** Task 22 adds a non-blocking `linux-clang-tsan` job (spec R2 "recommended in the M1 plan") to `.github/workflows/secure-frontend.yml`. It edits CI; skip the step if the controller does not approve it.
12. **`cfg01_wildcard_bind_requires_host_aliases`** "also asserts nothing spawned": the validation half is a `typed_config_tests` case (Task 4); the process half is a case of the same name in `api_server_tests` (Task 10).
13. **SP2 stdin write bound.** Spec §5.2 cites `kStdinWriteChunk`/`kStdinWriteDeadline`; on POSIX SP2 shipped one 1000 ms `poll` per write without chunking. The owner assigned the overall bound to SP2's second run; M1 does not touch it.
14. **Shutdown order.** SP2's `shutdown()` kills the engine group at once. The plan bounds the lease holder's drain by `kShutdownDrainBudget` (2 s, `api_constants.hpp`), joins the workers, and only then calls `supervisor.shutdown(remaining)`, so no `Lease` outlives the supervisor's teardown. SP2's 5 s `_Exit` bound is unchanged.
15. **Accept-thread polling.** Spec §2.4 says the lingering list is polled "with the listener". The plan polls the listener and every lingering socket in one `poll_sockets` call per 100 ms slice (same effect; noted so a reviewer does not look for two loops).
16. **Pre-flight rulings applied (review 1, R-A … R-K).** R-A: M1 owns the streaming seam types (`HttpHead`, `StreamSink`, `StreamHandler`, `RouteResult` in `request_router.hpp`), `RequestRouter::handle` returns `RouteResult`, the wire format has a head serializer without `Content-Length`, and `serve_connection` dispatches a `StreamHandler` through `deliver_route_result`; Task 6 is split into 6a/6b. R-D: literal trailers in every commit. R-G: Task 8 is split into 8a/8b with a per-case port table. R-H: shutdown = `context.shutting_down->load() || shutdown_signalled()`; worker-side waits during shutdown are bounded by `kShutdownDrainBudget`. R-I: see note 10. R-J: every `api_server_tests` case and every egress process case carries `LABELS ${SECURE_FRONTEND_WINDOWS_RUNTIME}`. R-K: the TSan job sets `vm.mmap_rnd_bits=28` and bootstraps `.venv-dev`.

## Global Constraints

- **Base and timing.** M1 executes only **after SP1 and SP2 (its parallel-safe set and its second run: Tasks 6b, 15, 16, 18, 19, 25, 26, 27, 28, 29) are merged to `main`**. Line numbers move with those merges, so this plan **never cites existing code by line number**: it names the function and file and quotes an anchor line ("the function `run` in `src/secure_server_main.cpp`, anchor `server->serve_one(&error)`"). If an anchor is not found, re-anchor by searching the quoted text; if the code it names does not exist, **stop and report** (spec R9).
- **Where to work.** Worktree `/opt/projects/ai/.worktrees/sp3`, branch `sp3-api-streaming`, created by the controller from `main` at execution time. Never push, merge, rebase or switch branches; no `rm -rf`, no `git clean`.
- **Every shell starts with** `cd /opt/projects/ai/.worktrees/sp3/secure_frontend && export PATH="$(git rev-parse --show-toplevel)/.venv-dev/bin:$PATH"`.
- **Linux:** `cmake --preset linux-gcc && cmake --build --preset linux-gcc && ctest --preset linux-gcc`. A single case: `ctest --preset linux-gcc -R '^<binary>\.<case>$' --output-on-failure`.
- **Windows cross build:** `cmake --preset windows-llvm-mingw-cross && cmake --build --preset windows-llvm-mingw-cross` (must compile with `-Werror`), then `ctest --preset windows-llvm-mingw-cross` (wine; the preset excludes `windows-runtime`). Every direct wine or `ctest --test-dir out/build/windows-llvm-mingw-cross` call is prefixed `WINEPREFIX=$PWD/out/wineprefix WINEDLLOVERRIDES=mscoree,mshtml=`. wine is a smoke test; Windows claims come only from the MSYS2 `windows-msys2-clang` job (W rows "pending MSYS2" until the owner's push shows them green).
- **No third-party dependencies** in `secure_frontend/` (standard library and OS APIs; Windows `ws2_32`; `iphlpapi` in test binaries only). **No change under `upstream/`** and none to the repository root `CMakeLists.txt`.
- **`-Werror`** on every target: libraries through `secure_frontend_add_library` (Task 1) or `secure_frontend_set_warnings`, tests through `secure_frontend_add_test_executable`.
- **TDD with RED evidence.** Every task writes its tests first, runs them and records the failure reason (the RED line) in the commit body, then implements, then runs the full `ctest --preset linux-gcc` and the cross build before committing.
- **Windows labels (ruling R-J).** Every `secure_frontend_api_server_tests` case and every egress case that starts the real server is registered with `LABELS ${SECURE_FRONTEND_WINDOWS_RUNTIME}`: wine excludes them and the MSYS2 job is authoritative for them; on Linux the variable is empty and they run in every `ctest --preset linux-gcc`.
- **Test registration.** Framework-free `main()` programs with `int failures` and `void expect(bool, const std::string&)` returning `secure_frontend_test::run_cases(argc, argv, {{"<case>", fn}, ...}, failures)`; registered with `secure_frontend_add_cases(<binary> <case>... [ARGS ...] [LABELS ...] [TIMEOUT s])`, CTest name `<binary>.<case>`. Real-Windows-only cases carry `LABELS ${SECURE_FRONTEND_WINDOWS_RUNTIME}`. Case names are the spec §9.3 names; extra unit cases this plan adds are listed per task.
- **Fixed-reason rule (spec §1.6).** No error body, log line or test message printed by the product contains a token, prompt, completion, header/body value, option value, configured value, path, engine line or exit code; every client-visible error is a row of `api_errors.hpp`.
- **Thread rule (spec §1.6).** Only SP2's supervisor thread forks, reaps, kills. Every SP3 call to `snapshot`, `acquire`, `report_fatal` goes through `SupervisorPort`. Only the worker holding the single `Lease` touches its protocol client.
- **Configuration only through SP1's typed keys** (schema rows added by Tasks 4–5, the SP1 addendum); every other number is a named constant in `include/secure_frontend/api_constants.hpp`. No environment variable is read by the product.
- **SP1 is edited only by the SP1-addendum tasks** (Tasks 4, 5 and the JSON-number extension in Task 7); each change is recorded as an SP1 addendum row in `docs/AGENT_HANDOFF.md`.
- **Commits:** one local commit per task. Every commit command carries the two trailer lines **literally** (never through a shell variable, which does not survive between steps):
  `git -C "$(git rev-parse --show-toplevel)" -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit -m "<subject>" -m "<body naming IDs, CTest names, RED line, wine line>" -m "Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>" -m "Claude-Session: <session link>"`.
- **Handoff in-task.** Every task adds or updates its row in a new `docs/AGENT_HANDOFF.md` section "SP3 M1 deliverables (branch `sp3-api-streaming`)" (created by Task 0) with the linux-gcc `tests passed` line and the wine line. Proof status never lives only in chat.
- **Behavior changes are deliberate (spec R3, §9.4):** JSON error bodies everywhere, `415`, `411`, `/v1/models` lists only the served model, default worker count 8, `max_tokens` clamping. Tests that asserted the old bodies are **updated in the task that changes the behavior**, never deleted.

## Review Focus

1. **`Host: LOCALHOST:8080` / `Localhost:8080`** (tools that do not lower-case the name) on a loopback bind must be accepted (ASCII case of the name is folded), while `Origin: http://LOCALHOST:8080` stays `403` (origins are exact) — pinned in Task 3 (`http_policy_tests.web02_host_name_case_is_folded`).
2. **A header whose `\r\n\r\n` is split across two reads, or that arrives one byte per segment from a fast client**, must be parsed exactly as if it came in one segment (no `400`, no `408`) — pinned in Task 8b (`http_socket_server_tests.head_terminator_split_across_reads`).
3. **`Expect: 100-continue` from a client that did not wait and already sent its body in the same segment** must get exactly one final response and no interim `100 Continue` after the body has arrived — pinned in Task 8b (`http_socket_server_tests.expect_continue_skipped_when_body_already_sent`).
4. **`Content-Type` spellings real clients send** (`application/json;charset=UTF-8` without a space, `Application/JSON`, `application/json; charset=utf-8`) must be accepted; `application/json; charset=latin1` and `text/plain` must be `415` — pinned in Task 6b (`http_parser_tests.content_type_variants_accepted`).
5. **Sampling numbers written as JSON integers** (`"temperature": 1`, `"top_p": 1`, `"temperature": 0`, `"presence_penalty": 0`) must be accepted exactly like `1.0`/`0.0` — pinned in Task 7 (`http_parser_tests.sampling_integer_forms_accepted`).

---

## File map

| File | Responsibility | Task |
| --- | --- | --- |
| `include/secure_frontend/api_constants.hpp` | every SP3 internal constant (spec §5.3) | 1 |
| `include/secure_frontend/api_errors.hpp`, `src/api_errors.cpp` | the §7.2 error table, error bodies and responses | 1 |
| `include/secure_frontend/http_wire.hpp`, `src/http_wire.cpp` | response serialization: security headers, `Date`, reason phrases, `100 Continue`; `serialize_head` | 1, 6a |
| `include/secure_frontend/http_request_head.hpp`, `src/http_request_head.cpp` | header scan and §4.2 request-head parser | 2 |
| `include/secure_frontend/http_policy.hpp`, `src/http_policy.cpp` | Host/Origin/CORS/preflight (rewritten) | 3 |
| `include/secure_frontend/frontend_config.hpp`, `src/frontend_config.cpp`, `src/config_file.cpp`, `src/frontend_init.cpp`, `include/secure_frontend/config_defaults.hpp` (SP1 files) | SP1 addendum: new rows, array grammar, derived defaults | 4, 5 |
| `include/secure_frontend/request_router.hpp`, `src/http_request_handler.cpp`, `include/secure_frontend/http_request_handler.hpp` | `RequestRouter`, `RouteDecision`, `kRouteTable`, `RouteResult`/`StreamSink`/`StreamHandler`/`HttpHead`, in-memory `handle_http_request` seam | 6a |
| `include/secure_frontend/api_router.hpp`, `src/api_router.cpp` | `ApiRouter` (authorize/handle, `/health`, `/v1/models[/id]`) | 6b |
| `include/secure_frontend/json.hpp`, `src/json.cpp` (SP1) | opt-in decimals | 7 |
| `include/secure_frontend/sampling_params.hpp`, `include/secure_frontend/chat_schema.hpp`, `src/chat_schema.cpp` | §3.1 request schema with fixed `param` keys; §3.5 response | 7, 15 |
| `include/secure_frontend/socket_io.hpp`, `src/socket_io.cpp` | non-blocking sockets, `poll_sockets`, bounded send, peer probe, half-close drain, `WinsockGuard` | 8a |
| `include/secure_frontend/connection_handler.hpp`, `src/connection_handler.cpp` | P1–P8 for one connection; `deliver_route_result` + socket `StreamSink` | 8b |
| `include/secure_frontend/http_socket_server.hpp`, `src/http_socket_server.cpp` | listener, `accept_next`, `serve_one` seam, idempotent `shutdown_listener` | 8b |
| `include/secure_frontend/connection_pool.hpp`, `src/admission.cpp`, `src/linger_list.cpp`, `src/connection_pool.cpp` | admission state, eviction, refusals, workers | 11, 12 |
| `include/secure_frontend/generation_gate.hpp`, `src/supervisor_port.cpp`, `src/generation_gate.cpp` | `SupervisorPort`, `GenerationGate` | 13 |
| `include/secure_frontend/engine_protocol_client.hpp`, `src/engine_protocol_client.cpp`, `include/secure_frontend/chat_execution.hpp`, `src/chat_execution.cpp` | `GEN` sampling keys; `run_generation`; `context_tokens`; `resolve_max_tokens` | 15 |
| `include/secure_frontend/chat_route.hpp`, `src/chat_route.cpp` | P4 validation, gate, P6 execution, response | 16 |
| `include/secure_frontend/request_log.hpp`, `src/request_log.cpp` | per-request log lines, token bucket | 21 |
| `src/secure_server_main.cpp` | wiring | 6b, 8b, 9, 12, 16, 21 |
| `tests/http_parser_test.cpp` | `secure_frontend_http_parser_tests` | 1, 2, 6a, 6b, 7 |
| `tests/concurrency_test.cpp` | `secure_frontend_concurrency_tests` | 11, 13, 21 |
| `tests/api_server_test.cpp` | `secure_frontend_api_server_tests` | 9, 10, 12, 16, 17, 20, 21, 22 |
| `tests/egress_test.cpp`, `tests/support/binary_imports.hpp`, `tests/support/egress_tripwire.cpp`, `tests/egress_probe_helper.cpp`, `tests/support/egress_support.hpp` | `secure_frontend_egress_tests`, `libstrata_egress_tripwire.so` | 18, 19, 20 |
| `tests/support/connection_scan.hpp` | socket/thread/handle inspection (NET-01, DOS-02, egress scan) | 12 |
| `tests/support/raw_http.hpp` | raw loopback HTTP client for tests | 8b |
| `tests/support/server_harness.{hpp,cpp}` (SP2) | `config_literals`, error-body constants | 9 |
| `tests/server_test.cpp` (SP2) | `ext01` amendment, EXT cases under the tripwires | 9, 20 |
| `.github/workflows/secure-frontend.yml` | non-blocking `linux-clang-tsan` (controller note 11) | 22 |
| `tests/supervision_fake_engine_main.cpp` (SP2) | M1 fake modes | 14 |

Test binaries added: `secure_frontend_http_parser_tests`, `secure_frontend_concurrency_tests`, `secure_frontend_api_server_tests`, `secure_frontend_egress_tests`. Extended: `secure_frontend_http_policy_tests`, `secure_frontend_http_socket_server_tests`, `secure_frontend_http_request_handler_tests`, `secure_frontend_api_router_tests`, `secure_frontend_typed_config_tests`, `secure_frontend_json_tests`, `secure_frontend_chat_schema_tests`, `secure_frontend_chat_execution_tests`, `secure_frontend_engine_protocol_client_tests`, `secure_frontend_fake_engine_tests`, `secure_frontend_server_tests`.

## Shared interfaces (exact; every task uses these names)

```cpp
// api_constants.hpp (Task 1)
namespace secure_frontend::api_constants {
inline constexpr std::chrono::milliseconds kAcceptPollSlice{100};
inline constexpr std::chrono::milliseconds kStreamPollSlice{20};
inline constexpr std::chrono::milliseconds kAcceptBackoff{50};
inline constexpr std::size_t kLingerListMax = 64U;
inline constexpr std::chrono::milliseconds kLingerBudget{100};
inline constexpr std::size_t kChatMaxJsonDepth = 4U;
inline constexpr std::size_t kMaxDeltaTextBytes = 16U * 1024U;     // M2
inline constexpr std::size_t kMaxSseEventBytes = 128U * 1024U;     // M2
inline constexpr std::size_t kMaxSendChunk = 64U * 1024U;
inline constexpr std::chrono::milliseconds kDrainBudget{100};
inline constexpr std::size_t kDrainBytes = 256U * 1024U;
inline constexpr std::size_t kRecvChunk = 4096U;
inline constexpr int kRetryAfterBusyS = 1;
inline constexpr int kRetryAfterEngineS = 5;
inline constexpr std::size_t kLogBurst = 100U;
inline constexpr std::size_t kLogRatePerSecond = 20U;
inline constexpr std::size_t kRefusalMaxBytes = 512U;
inline constexpr std::chrono::milliseconds kShutdownDrainBudget{2000};
inline constexpr std::size_t kChatModelMaxBytes = 256U;
inline constexpr std::size_t kChatUserMaxBytes = 256U;
inline constexpr double kTemperatureMax = 2.0;
inline constexpr std::uint64_t kSeedMax = 9223372036854775807ULL;
inline constexpr std::size_t kContextMarginTokens = 8U;
}

// api_errors.hpp (Task 1)
enum class ApiError { invalid_request, invalid_json, unsupported_parameter, extension_disabled, context_length_exceeded,
  invalid_api_key, forbidden, not_found, model_not_found, method_not_allowed, request_timeout, length_required,
  request_too_large, request_line_too_long, unsupported_media_type, expectation_failed, header_too_large,
  internal_error, server_busy, engine_unavailable, engine_error, server_shutting_down, http_version_not_supported };
struct ApiErrorRow final { ApiError error; int status; std::string_view type; std::string_view code; std::string_view message; };
std::span<const ApiErrorRow> api_error_table() noexcept;
const ApiErrorRow& api_error_row(ApiError) noexcept;
bool is_fixed_param_key(std::string_view key) noexcept;
struct ErrorOptions final { std::string_view param{}; std::optional<int> retry_after{}; std::string_view allow{}; };
std::string render_error_body(ApiError, std::string_view param = {});
HttpResponse make_error_response(ApiError, const ErrorOptions& options = {});

// http_wire.hpp (Task 1)
inline constexpr std::string_view kContinueResponse = "HTTP/1.1 100 Continue\r\n\r\n";
std::string_view reason_phrase(int status) noexcept;
std::string imf_fixdate(std::chrono::system_clock::time_point) ;
std::string serialize_response(const HttpResponse&, std::chrono::system_clock::time_point now);

// http_request_head.hpp (Task 2)
struct HeadLimits final { std::size_t max_request_line_bytes; std::size_t max_header_bytes; std::size_t max_headers; std::size_t max_request_bytes; };
enum class HeadScan { need_more, complete, line_too_long, header_too_large };
struct HeadScanResult final { HeadScan state; std::size_t head_bytes{0}; };  // head_bytes includes the final CRLFCRLF
HeadScanResult scan_request_head(std::string_view received, const HeadLimits&) noexcept;
struct HeaderField final { std::string name; std::string value; };       // name as sent, value with OWS trimmed
struct RequestHead final { std::string method; std::string target; std::vector<HeaderField> fields;
  std::size_t head_bytes{0}; std::optional<std::size_t> content_length; bool expect_continue{false};
  std::vector<std::string_view> values(std::string_view name) const; };   // ASCII-case-insensitive name
using HeadParseResult = std::variant<RequestHead, ApiError>;
HeadParseResult parse_request_head(std::string_view head, const HeadLimits&);

// http_policy.hpp (Task 3)
struct HttpPolicy final { std::vector<std::string> allowed_hosts; std::vector<std::string> same_origins; std::vector<std::string> cors_origins; };
HttpPolicy make_http_policy(const IpAddress& bind, std::uint16_t port, const std::vector<std::string>& host_aliases, const std::vector<std::string>& cors_origins);
enum class PolicyVerdict { allowed, malformed, forbidden };
PolicyVerdict check_host(const HttpPolicy&, const std::vector<std::string_view>& hosts);
struct OriginDecision final { PolicyVerdict verdict; std::optional<std::string> allow_origin; };
OriginDecision check_origin(const HttpPolicy&, const std::vector<std::string_view>& origins);
inline constexpr std::string_view kPreflightAllowHeaders = "authorization, content-type, x-stainless-arch, x-stainless-lang, x-stainless-os, x-stainless-package-version, x-stainless-retry-count, x-stainless-runtime, x-stainless-runtime-version, x-stainless-timeout";
std::optional<HttpResponse> evaluate_preflight(const HttpPolicy&, const RequestHead&);   // nullopt = not a preflight
bool is_valid_host_alias(std::string_view) noexcept; bool is_valid_cors_origin(std::string_view) noexcept;

// request_router.hpp (Task 6a) — ruling R-A: M2 consumes these names verbatim
inline constexpr int kNoResponseStatus = 0;
struct HttpHead final { int status; std::vector<std::pair<std::string, std::string>> headers; };   // never Content-Length
struct StreamSink { virtual bool send_headers(const HttpHead& head) = 0; virtual bool send_event(std::string_view bytes) = 0;
  virtual bool writable(std::chrono::milliseconds timeout) = 0; virtual bool client_gone() = 0;
  virtual void finish() = 0; virtual ~StreamSink() = default; };   // finish: [DONE] already sent -> terminal chunk
using StreamHandler = std::function<void(StreamSink&)>;
using RouteResult = std::variant<HttpResponse, StreamHandler>;
enum class RouteId { health, models, model, chat, other };
struct RouteEntry final { std::string_view method; std::string_view path; bool prefix; RouteId id; };
inline constexpr std::array<RouteEntry, 4> kRouteTable{{{"GET", "/health", false, RouteId::health}, {"GET", "/v1/models", false, RouteId::models},
  {"GET", "/v1/models/", true, RouteId::model}, {"POST", "/v1/chat/completions", false, RouteId::chat}}};
struct RouteDecision final { std::optional<HttpResponse> early; bool expect_continue{false}; std::size_t body_limit{0};
  RouteId route{RouteId::other}; std::optional<std::string> allow_origin; };
struct RequestContext final { std::function<bool()> client_gone; const std::atomic<bool>* shutting_down{nullptr}; };
class RequestRouter { public: virtual ~RequestRouter() = default;
  virtual RouteDecision authorize(const RequestHead&) = 0;
  virtual RouteResult handle(const RequestHead&, std::string_view body, const RouteDecision&, const RequestContext&) = 0; };
HttpResponse handle_http_request(std::string_view request, RequestRouter&, const HeadLimits&);   // in-memory seam (a StreamHandler -> 500)
// http_wire.hpp (Task 6a)
std::string serialize_head(const HttpHead& head, std::chrono::system_clock::time_point now);    // no Content-Length

// connection_handler.hpp (Task 8b)
struct ConnectionLimits final { HeadLimits head; std::chrono::milliseconds header_deadline; std::chrono::milliseconds request_deadline;
  std::chrono::milliseconds write_stall; std::chrono::milliseconds response_write_deadline; };
struct WorkerSignals final { const std::atomic<bool>* abort{nullptr}; const std::atomic<bool>* shutting_down{nullptr};
  std::function<bool()> on_header_complete; };   // returns false when the connection was evicted meanwhile
enum class ConnectionOutcome { responded, evicted, timed_out, client_gone, transport_error, shutdown };
struct ConnectionResult final { ConnectionOutcome outcome; int status{0}; std::string_view code; std::size_t bytes_out{0}; RouteId route{RouteId::other}; };
ConnectionResult serve_connection(NativeSocket, RequestRouter&, const ConnectionLimits&, const WorkerSignals&, std::chrono::steady_clock::time_point accepted_at);
// the StreamHandler dispatch site (ruling R-A)
ConnectionResult deliver_route_result(NativeSocket socket, RouteResult result, const RouteDecision& decision,
                                      const ConnectionLimits& limits, const WorkerSignals& signals);

// generation_gate.hpp (Task 13)
class SupervisorPort final { public: explicit SupervisorPort(EngineSupervisor&);
  HealthSnapshot snapshot(); std::optional<Lease> acquire(); void report_fatal(const Lease&, KillReason); };
enum class GateRefusal { busy, timeout, cancelled, engine_unavailable, shutting_down };
struct GateLimits final { std::uint32_t max_queued; std::chrono::milliseconds queue_timeout; };
class GenerationGate final { public: explicit GenerationGate(GateLimits);
  std::variant<Lease, GateRefusal> admit(SupervisorPort&, std::chrono::steady_clock::time_point deadline, const std::function<bool()>& client_gone);
  void release() noexcept; void shutdown() noexcept; std::size_t waiting() const; };
```

---

## Task 0: Base check and handoff section

**Files:**
- Modify: `docs/AGENT_HANDOFF.md`

**Interfaces:**
- Consumes: `main` with SP1 and SP2 merged.
- Produces: the anchor checks every later task relies on; the handoff section "SP3 M1 deliverables".

- [ ] **Step 1: Verify the merged anchors exist** (each `grep` must print at least one line; any empty result is a STOP: report which anchor is missing to the controller)

```bash
cd /opt/projects/ai/.worktrees/sp3/secure_frontend && export PATH="$(git rev-parse --show-toplevel)/.venv-dev/bin:$PATH"
git -C "$(git rev-parse --show-toplevel)" log --oneline -1
grep -n "class EngineSupervisor" include/secure_frontend/engine_supervisor.hpp
grep -n "execute_with_lease(supervisor" src/secure_server_main.cpp
grep -n "shutdown_signalled" include/secure_frontend/shutdown_signal.hpp src/secure_server_main.cpp
grep -n "shutdown_listener" include/secure_frontend/http_socket_server.hpp
grep -n "kConfigSchemaSize\|std::array<ConfigKey" src/frontend_config.cpp
grep -n "class TokenVerifier" include/secure_frontend/token_verifier.hpp
grep -n "void append_json_string" include/secure_frontend/json.hpp
grep -n "struct HttpParserLimits" include/secure_frontend/http_request_handler.hpp
grep -n "ext01_extension_fields_spawn_nothing" tests/server_test.cpp
grep -n "kGeneric503\|kBadRequest" tests/support/server_harness.hpp
grep -n "class FakeFactory" tests/support/fake_supervised_process.hpp
grep -n "render_config_json" tests/support/config_json.hpp
grep -n "config-keys:begin" README.md
grep -n "adopt_ready" include/secure_frontend/chat_execution.hpp
grep -n "supervisor_config" include/secure_frontend/subsystem_limits.hpp   # SP2 Task 29 landed
grep -n '"roots.state"' src/frontend_config.cpp                            # SP2 Task 29 landed
grep -n "FrontendConfig(ListenerConfig listener_value" include/secure_frontend/frontend_config.hpp   # inline ctor (Task 4 anchor)
grep -n "LD_PRELOAD" include/secure_frontend/engine_environment.hpp       # SP2 Task 18 never forwards it to the engine
```

- [ ] **Step 2: Record the baseline**

```bash
cmake --preset linux-gcc && cmake --build --preset linux-gcc && ctest --preset linux-gcc | tail -3
cmake --preset windows-llvm-mingw-cross && cmake --build --preset windows-llvm-mingw-cross && ctest --preset windows-llvm-mingw-cross | tail -3
```

Expected: `100% tests passed` on both (wine may report skips from SP1/SP2's allowed list). Write the two totals down as N₀ (linux) and W₀ (wine).

- [ ] **Step 3: Add the handoff section** — in `docs/AGENT_HANDOFF.md`, after the last "SP2 deliverables" section, add:

```markdown
### SP3 M1 deliverables (branch `sp3-api-streaming`)

Plan: `docs/superpowers/plans/2026-10-14-sp3-m1-concurrency-hardening.md`. Spec:
`docs/superpowers/specs/2026-10-14-sp3-api-concurrency-streaming-design.md`.
Baseline before Task 1: linux-gcc N₀ = <N₀> tests, wine W₀ = <W₀> tests.

| Task | IDs | What changed | Tests (CTest names) | linux-gcc | wine |
| --- | --- | --- | --- | --- | --- |

#### SP1 addendum (recorded by SP3, spec §5.1, §5.3)

| # | Change | Task |
| --- | --- | --- |
```

(`<N₀>` and `<W₀>` are the two numbers from Step 2.)

- [ ] **Step 4: Commit**

```bash
git -C "$(git rev-parse --show-toplevel)" add docs/AGENT_HANDOFF.md
git -C "$(git rev-parse --show-toplevel)" -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "docs: SP3 M1 handoff section and baseline" \
  -m "Anchors for SP1 and SP2 verified on main. Baseline linux-gcc <N0>, wine <W0>." -m "Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>" -m "Claude-Session: <session link>"
```

---

## Task 1: Internal constants, one JSON writer, the error table and the response wire format

**Files:**
- Create: `include/secure_frontend/api_constants.hpp`, `include/secure_frontend/api_errors.hpp`, `src/api_errors.cpp`, `include/secure_frontend/http_wire.hpp`, `src/http_wire.cpp`, `tests/http_parser_test.cpp`
- Modify: `CMakeLists.txt` (helper `secure_frontend_add_library`, library `secure_frontend_api_core`, test binary), `include/secure_frontend/http_request_handler.hpp` (`HttpResponse::log_code`), `src/api_router.cpp` (delete its private `append_json_string`), `tests/api_router_test.cpp` (new case)

**Interfaces:**
- Consumes: SP1 `append_json_string(std::string*, std::string_view)` from `json.hpp`; `HttpResponse` from `http_request_handler.hpp`.
- Produces: `HttpResponse::log_code` (`std::string_view`, default empty: the §7.2 code for the §8 request log, never serialized), `api_constants`, `ApiError`, `api_error_table()`, `api_error_row()`, `is_fixed_param_key()`, `ErrorOptions`, `render_error_body()`, `make_error_response()`, `kContinueResponse`, `reason_phrase()`, `imf_fixdate()`, `serialize_response()`; CMake function `secure_frontend_add_library(<name> SOURCES ... [LINKS ...])`; library `secure_frontend_api_core`.

- [ ] **Step 1: Write the failing tests** — create `tests/http_parser_test.cpp`:

```cpp
#include "secure_frontend/api_constants.hpp"
#include "secure_frontend/api_errors.hpp"
#include "secure_frontend/http_wire.hpp"
#include "support/test_support.hpp"

#include <chrono>
#include <iostream>
#include <string>
#include <string_view>

namespace {
int failures = 0;
void expect(bool condition, const std::string& message) {
  if (!condition) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
}
using secure_frontend::ApiError;

// Spec §7.2, copied row by row: status, type, code, message.
struct ExpectedRow { ApiError error; int status; const char* type; const char* code; const char* message; };
constexpr ExpectedRow kExpected[] = {
    {ApiError::invalid_request, 400, "invalid_request_error", "invalid_request", "The request is malformed or not supported."},
    {ApiError::invalid_json, 400, "invalid_request_error", "invalid_json", "The request body is not valid JSON within the allowed limits."},
    {ApiError::unsupported_parameter, 400, "invalid_request_error", "unsupported_parameter", "This parameter is not supported by this server."},
    {ApiError::extension_disabled, 400, "invalid_request_error", "extension_disabled", "Tools, functions and extensions are disabled on this server."},
    {ApiError::context_length_exceeded, 400, "invalid_request_error", "context_length_exceeded", "The prompt exceeds the model context or the configured prompt limit."},
    {ApiError::invalid_api_key, 401, "authentication_error", "invalid_api_key", "Missing or invalid API key."},
    {ApiError::forbidden, 403, "permission_error", "forbidden", "The request origin or host is not allowed."},
    {ApiError::not_found, 404, "invalid_request_error", "not_found", "Not found."},
    {ApiError::model_not_found, 404, "invalid_request_error", "model_not_found", "The requested model is not served here."},
    {ApiError::method_not_allowed, 405, "invalid_request_error", "method_not_allowed", "Method not allowed."},
    {ApiError::request_timeout, 408, "invalid_request_error", "request_timeout", "The request was not received in time."},
    {ApiError::length_required, 411, "invalid_request_error", "length_required", "A Content-Length body is required; chunked bodies are not accepted."},
    {ApiError::request_too_large, 413, "invalid_request_error", "request_too_large", "The request body is too large."},
    {ApiError::request_line_too_long, 414, "invalid_request_error", "request_line_too_long", "The request line is too long."},
    {ApiError::unsupported_media_type, 415, "invalid_request_error", "unsupported_media_type", "Send application/json."},
    {ApiError::expectation_failed, 417, "invalid_request_error", "expectation_failed", "Unsupported Expect header."},
    {ApiError::header_too_large, 431, "invalid_request_error", "header_too_large", "The request header is too large."},
    {ApiError::internal_error, 500, "server_error", "internal_error", "Internal error."},
    {ApiError::server_busy, 503, "server_error", "server_busy", "The server is busy; retry later."},
    {ApiError::engine_unavailable, 503, "server_error", "engine_unavailable", "The local engine is not available."},
    {ApiError::engine_error, 503, "server_error", "engine_error", "The local engine could not complete the request."},
    {ApiError::server_shutting_down, 503, "server_error", "server_shutting_down", "The server is shutting down."},
    {ApiError::http_version_not_supported, 505, "invalid_request_error", "http_version_not_supported", "Only HTTP/1.1 is supported."},
};

void err_every_code_renders_fixed_body() {
  expect(secure_frontend::api_error_table().size() == std::size(kExpected), "the table has exactly the 23 rows of spec §7.2");
  for (const auto& row : kExpected) {
    const std::string body = std::string{"{\"error\":{\"message\":\""} + row.message + "\",\"type\":\"" + row.type +
                             "\",\"param\":null,\"code\":\"" + row.code + "\"}}\n";
    expect(secure_frontend::render_error_body(row.error) == body, std::string{"body of "} + row.code);
    const auto response = secure_frontend::make_error_response(row.error);
    expect(response.status == row.status, std::string{"status of "} + row.code);
    expect(response.body == body, std::string{"response body of "} + row.code);
    expect(response.header_value("Content-Type") == std::optional<std::string_view>{"application/json; charset=utf-8"},
           std::string{"content type of "} + row.code);
    expect(response.log_code == row.code, std::string{"log code of "} + row.code);
  }
  const auto unauthorized = secure_frontend::make_error_response(ApiError::invalid_api_key);
  expect(unauthorized.header_value("WWW-Authenticate") == std::optional<std::string_view>{"Bearer"}, "401 carries WWW-Authenticate: Bearer");
  const auto busy = secure_frontend::make_error_response(ApiError::server_busy);
  expect(busy.header_value("Retry-After") == std::optional<std::string_view>{"1"}, "server_busy carries Retry-After: 1");
  const auto engine_error = secure_frontend::make_error_response(ApiError::engine_error);
  expect(engine_error.header_value("x-should-retry") == std::optional<std::string_view>{"false"}, "engine_error carries x-should-retry: false");
  const auto starting = secure_frontend::make_error_response(ApiError::engine_unavailable, {.retry_after = 5});
  expect(starting.header_value("Retry-After") == std::optional<std::string_view>{"5"}, "engine_unavailable can carry Retry-After: 5");
  const auto exhausted = secure_frontend::make_error_response(ApiError::engine_unavailable);
  expect(!exhausted.header_value("Retry-After").has_value(), "engine_unavailable without retry_after carries none");
  const auto method = secure_frontend::make_error_response(ApiError::method_not_allowed, {.allow = "GET"});
  expect(method.header_value("Allow") == std::optional<std::string_view>{"GET"}, "405 carries Allow");
}

void err_param_is_key_only() {
  const std::string tools = secure_frontend::render_error_body(ApiError::extension_disabled, "tools");
  expect(tools.find("\"param\":\"tools\"") != std::string::npos, "a fixed extension key is named in param");
  const std::string stream = secure_frontend::render_error_body(ApiError::unsupported_parameter, "stream");
  expect(stream.find("\"param\":\"stream\"") != std::string::npos, "a fixed unsupported key is named in param");
  const std::string unknown = secure_frontend::render_error_body(ApiError::unsupported_parameter, "secret\"value");
  expect(unknown.find("\"param\":null") != std::string::npos && unknown.find("secret") == std::string::npos,
         "a key outside the fixed lists is never reflected");
  const std::string other_code = secure_frontend::render_error_body(ApiError::invalid_request, "tools");
  expect(other_code.find("\"param\":null") != std::string::npos, "param is null for every other code");
  for (const auto key : {"n", "presence_penalty", "frequency_penalty", "logit_bias", "stop", "logprobs", "top_k", "min_p",
                         "top_logprobs", "response_format", "content", "stream", "tools", "tool_choice", "functions",
                         "function_call", "parallel_tool_calls", "mcp_servers", "role"}) {
    expect(secure_frontend::is_fixed_param_key(key), std::string{"fixed key "} + key);
  }
  expect(!secure_frontend::is_fixed_param_key("model") && !secure_frontend::is_fixed_param_key(""), "other keys are not fixed");
}

void wire_response_has_security_headers_and_date() {
  using namespace std::chrono;
  const auto now = system_clock::time_point{seconds{1791963785}};   // 2026-10-14 07:43:05 UTC
  expect(secure_frontend::imf_fixdate(now) == "Wed, 14 Oct 2026 07:43:05 GMT", "IMF-fixdate");
  expect(secure_frontend::imf_fixdate(system_clock::time_point{seconds{951782400}}) == "Tue, 29 Feb 2000 00:00:00 GMT",
         "IMF-fixdate on a leap day");
  secure_frontend::HttpResponse response{.status = 200, .headers = {{"Content-Type", "application/json; charset=utf-8"},
                                                                    {"X-Bad", "a\r\nInjected: 1"}, {"Server", "leak"}},
                                         .body = "{}\n"};
  const std::string wire = secure_frontend::serialize_response(response, now);
  expect(wire.rfind("HTTP/1.1 200 OK\r\n", 0) == 0, "status line");
  for (const auto line : {"Date: Wed, 14 Oct 2026 07:43:05 GMT\r\n", "Connection: close\r\n", "Content-Length: 3\r\n",
                          "Cache-Control: no-store\r\n", "X-Content-Type-Options: nosniff\r\n",
                          "Content-Security-Policy: default-src 'none'; frame-ancestors 'none'\r\n",
                          "Referrer-Policy: no-referrer\r\n", "Cross-Origin-Resource-Policy: same-origin\r\n",
                          "Content-Type: application/json; charset=utf-8\r\n"}) {
    expect(wire.find(line) != std::string::npos, std::string{"header line "} + line);
  }
  expect(wire.find("Injected") == std::string::npos, "a header value with CR/LF is dropped");
  expect(wire.find("Server:") == std::string::npos, "no Server header is ever written");
  expect(wire.size() >= 3U && wire.substr(wire.size() - 7U) == "\r\n\r\n{}\n", "body follows the blank line");
  const secure_frontend::HttpResponse no_content{.status = 204, .headers = {}, .body = ""};
  const std::string empty = secure_frontend::serialize_response(no_content, now);
  expect(empty.rfind("HTTP/1.1 204 No Content\r\n", 0) == 0 && empty.find("Content-Length") == std::string::npos,
         "204 has no Content-Length");
  for (const int status : {400, 401, 403, 404, 405, 408, 411, 413, 414, 415, 417, 431, 500, 503, 505}) {
    expect(!secure_frontend::reason_phrase(status).empty() && secure_frontend::reason_phrase(status) != "Unknown",
           "reason phrase for " + std::to_string(status));
  }
  expect(secure_frontend::kContinueResponse == "HTTP/1.1 100 Continue\r\n\r\n", "interim 100 response");
}
}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"err_every_code_renders_fixed_body", err_every_code_renders_fixed_body},
      {"err_param_is_key_only", err_param_is_key_only},
      {"wire_response_has_security_headers_and_date", wire_response_has_security_headers_and_date},
  }, failures);
}
```

Append to `tests/api_router_test.cpp` a case `models_list_keeps_utf8_model_id` (add its entry to the file's `run_cases` list) that builds the router exactly as the file's existing `router()` helper does but with a model id `"modèle-ü"` and asserts the `/v1/models` body contains the raw UTF-8 bytes `modèle-ü` and no `\u00`:

```cpp
void models_list_keeps_utf8_model_id() {
  const secure_frontend::ApiRouter utf8_router([] { return true; },
      [] { return std::vector<secure_frontend::ApiModel>{{.id = "mod\xc3\xa8le-\xc3\xbc", .owned_by = "local"}}; });
  const auto response = utf8_router.dispatch("GET", "/v1/models");
  expect(response.status == 200, "models answers 200");
  expect(response.body.find("mod\xc3\xa8le-\xc3\xbc") != std::string::npos, "UTF-8 model id copied unchanged");
  expect(response.body.find("\\u00") == std::string::npos, "no byte >= 0x80 is escaped as \\u00XX");
}
```

(If SP2 changed the `ApiRouter` constructor to four providers, pass `{}` for the two state providers; Task 6b replaces this router anyway and ports this case.)

- [ ] **Step 2: Register and run to see RED** — in `CMakeLists.txt`, directly after the `function(secure_frontend_add_cases ...)` … `endfunction()` block, add:

```cmake
# secure_frontend_add_library(<name> SOURCES <src>... [LINKS <lib>...])  (SP3)
function(secure_frontend_add_library name)
    cmake_parse_arguments(PARSE_ARGV 1 _sfal "" "" "SOURCES;LINKS")
    add_library(${name} ${_sfal_SOURCES})
    target_compile_features(${name} PUBLIC cxx_std_20)
    target_include_directories(${name} PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)
    if(_sfal_LINKS)
        target_link_libraries(${name} PUBLIC ${_sfal_LINKS})
    endif()
    secure_frontend_set_warnings(${name})
endfunction()
```

After the line `add_library(secure_frontend_http_request_handler` block (anchor text `src/http_request_handler.cpp`), add:

```cmake
secure_frontend_add_library(secure_frontend_api_core
    SOURCES src/api_errors.cpp src/http_wire.cpp
    LINKS secure_frontend_json)
```

and `target_link_libraries(secure_frontend_http_request_handler PUBLIC secure_frontend_api_core)` plus `target_link_libraries(secure_frontend_api_router PUBLIC secure_frontend_api_core)`. In the tests block (anchor `secure_frontend_add_test_executable(secure_frontend_http_request_handler_tests`), add:

```cmake
    secure_frontend_add_test_executable(secure_frontend_http_parser_tests tests/http_parser_test.cpp
        secure_frontend_api_core secure_frontend_http_request_handler)
    secure_frontend_add_cases(secure_frontend_http_parser_tests
        err_every_code_renders_fixed_body err_param_is_key_only wire_response_has_security_headers_and_date)
```

and add `models_list_keeps_utf8_model_id` to the `secure_frontend_add_cases(secure_frontend_api_router_tests` list.

Run: `cmake --preset linux-gcc && cmake --build --preset linux-gcc 2>&1 | tail -5`
Expected: FAIL to build: `secure_frontend/api_constants.hpp: No such file or directory`. Record that line. (The router case alone would fail at runtime with "no byte >= 0x80 is escaped".)

- [ ] **Step 3: Implement** — `include/secure_frontend/api_constants.hpp`: exactly the `api_constants` block of "Shared interfaces" above, with `#pragma once`, `#include <chrono>`, `#include <cstddef>`, `#include <cstdint>` and the comment `// Every SP3 internal constant (spec §5.3). Tunables are SP1 schema keys, never here.`

`include/secure_frontend/api_errors.hpp`:

```cpp
#pragma once

#include "secure_frontend/http_request_handler.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace secure_frontend {

// Spec §7.2. Every client-visible error is one of these rows; the message is
// a compile-time literal and never contains request, config or engine data.
enum class ApiError {
  invalid_request, invalid_json, unsupported_parameter, extension_disabled, context_length_exceeded,
  invalid_api_key, forbidden, not_found, model_not_found, method_not_allowed, request_timeout, length_required,
  request_too_large, request_line_too_long, unsupported_media_type, expectation_failed, header_too_large,
  internal_error, server_busy, engine_unavailable, engine_error, server_shutting_down, http_version_not_supported,
};

struct ApiErrorRow final {
  ApiError error;
  int status;
  std::string_view type;
  std::string_view code;
  std::string_view message;
};

[[nodiscard]] std::span<const ApiErrorRow> api_error_table() noexcept;
[[nodiscard]] const ApiErrorRow& api_error_row(ApiError error) noexcept;
// The fixed key lists of spec §3.1 (unsupported_parameter and extension_disabled).
[[nodiscard]] bool is_fixed_param_key(std::string_view key) noexcept;

struct ErrorOptions final {
  std::string_view param{};             // used only for unsupported_parameter / extension_disabled, and only a fixed key
  std::optional<int> retry_after{};     // engine_unavailable: 5 or none (spec §3.8)
  std::string_view allow{};             // method_not_allowed
};

// {"error":{"message":"<fixed>","type":"<type>","param":null|"<key>","code":"<code>"}}\n
[[nodiscard]] std::string render_error_body(ApiError error, std::string_view param = {});
[[nodiscard]] HttpResponse make_error_response(ApiError error, const ErrorOptions& options = {});

}  // namespace secure_frontend
```

`src/api_errors.cpp`:

```cpp
#include "secure_frontend/api_errors.hpp"

#include "secure_frontend/api_constants.hpp"
#include "secure_frontend/json.hpp"

#include <algorithm>
#include <array>

namespace secure_frontend {
namespace {

constexpr std::array<ApiErrorRow, 23> kTable{{
    {ApiError::invalid_request, 400, "invalid_request_error", "invalid_request", "The request is malformed or not supported."},
    {ApiError::invalid_json, 400, "invalid_request_error", "invalid_json", "The request body is not valid JSON within the allowed limits."},
    {ApiError::unsupported_parameter, 400, "invalid_request_error", "unsupported_parameter", "This parameter is not supported by this server."},
    {ApiError::extension_disabled, 400, "invalid_request_error", "extension_disabled", "Tools, functions and extensions are disabled on this server."},
    {ApiError::context_length_exceeded, 400, "invalid_request_error", "context_length_exceeded", "The prompt exceeds the model context or the configured prompt limit."},
    {ApiError::invalid_api_key, 401, "authentication_error", "invalid_api_key", "Missing or invalid API key."},
    {ApiError::forbidden, 403, "permission_error", "forbidden", "The request origin or host is not allowed."},
    {ApiError::not_found, 404, "invalid_request_error", "not_found", "Not found."},
    {ApiError::model_not_found, 404, "invalid_request_error", "model_not_found", "The requested model is not served here."},
    {ApiError::method_not_allowed, 405, "invalid_request_error", "method_not_allowed", "Method not allowed."},
    {ApiError::request_timeout, 408, "invalid_request_error", "request_timeout", "The request was not received in time."},
    {ApiError::length_required, 411, "invalid_request_error", "length_required", "A Content-Length body is required; chunked bodies are not accepted."},
    {ApiError::request_too_large, 413, "invalid_request_error", "request_too_large", "The request body is too large."},
    {ApiError::request_line_too_long, 414, "invalid_request_error", "request_line_too_long", "The request line is too long."},
    {ApiError::unsupported_media_type, 415, "invalid_request_error", "unsupported_media_type", "Send application/json."},
    {ApiError::expectation_failed, 417, "invalid_request_error", "expectation_failed", "Unsupported Expect header."},
    {ApiError::header_too_large, 431, "invalid_request_error", "header_too_large", "The request header is too large."},
    {ApiError::internal_error, 500, "server_error", "internal_error", "Internal error."},
    {ApiError::server_busy, 503, "server_error", "server_busy", "The server is busy; retry later."},
    {ApiError::engine_unavailable, 503, "server_error", "engine_unavailable", "The local engine is not available."},
    {ApiError::engine_error, 503, "server_error", "engine_error", "The local engine could not complete the request."},
    {ApiError::server_shutting_down, 503, "server_error", "server_shutting_down", "The server is shutting down."},
    {ApiError::http_version_not_supported, 505, "invalid_request_error", "http_version_not_supported", "Only HTTP/1.1 is supported."},
}};

constexpr std::array<std::string_view, 19> kFixedParamKeys{
    "n", "presence_penalty", "frequency_penalty", "logit_bias", "stop", "logprobs", "top_k", "min_p", "top_logprobs",
    "response_format", "content", "stream", "tools", "tool_choice", "functions", "function_call", "parallel_tool_calls",
    "mcp_servers", "role"};

}  // namespace

std::span<const ApiErrorRow> api_error_table() noexcept { return kTable; }

const ApiErrorRow& api_error_row(const ApiError error) noexcept {
  return *std::find_if(kTable.begin(), kTable.end(), [error](const ApiErrorRow& row) { return row.error == error; });
}

bool is_fixed_param_key(const std::string_view key) noexcept {
  return std::find(kFixedParamKeys.begin(), kFixedParamKeys.end(), key) != kFixedParamKeys.end();
}

std::string render_error_body(const ApiError error, const std::string_view param) {
  const ApiErrorRow& row = api_error_row(error);
  const bool named = (error == ApiError::unsupported_parameter || error == ApiError::extension_disabled) &&
                     is_fixed_param_key(param);
  std::string body{"{\"error\":{\"message\":"};
  append_json_string(&body, row.message);
  body.append(",\"type\":");
  append_json_string(&body, row.type);
  body.append(",\"param\":");
  if (named) append_json_string(&body, param); else body.append("null");
  body.append(",\"code\":");
  append_json_string(&body, row.code);
  body.append("}}\n");
  return body;
}

HttpResponse make_error_response(const ApiError error, const ErrorOptions& options) {
  const ApiErrorRow& row = api_error_row(error);
  HttpResponse response{.status = row.status,
                        .headers = {{"Content-Type", "application/json; charset=utf-8"}},
                        .body = render_error_body(error, options.param),
                        .log_code = row.code};
  if (error == ApiError::invalid_api_key) response.headers.emplace_back("WWW-Authenticate", "Bearer");
  if (error == ApiError::server_busy) response.headers.emplace_back("Retry-After", std::to_string(api_constants::kRetryAfterBusyS));
  if (error == ApiError::engine_error) response.headers.emplace_back("x-should-retry", "false");
  if (error == ApiError::engine_unavailable && options.retry_after.has_value()) {
    response.headers.emplace_back("Retry-After", std::to_string(*options.retry_after));
  }
  if (error == ApiError::method_not_allowed && !options.allow.empty()) response.headers.emplace_back("Allow", std::string{options.allow});
  return response;
}

}  // namespace secure_frontend
```

`include/secure_frontend/http_wire.hpp`:

```cpp
#pragma once

#include "secure_frontend/http_request_handler.hpp"

#include <chrono>
#include <string>
#include <string_view>

namespace secure_frontend {

inline constexpr std::string_view kContinueResponse = "HTTP/1.1 100 Continue\r\n\r\n";

[[nodiscard]] std::string_view reason_phrase(int status) noexcept;
// RFC 9110 §5.6.7 IMF-fixdate, e.g. "Wed, 14 Oct 2026 07:43:05 GMT".
[[nodiscard]] std::string imf_fixdate(std::chrono::system_clock::time_point now);
// Status line, Date, Connection: close, Content-Length (not for 204), the six
// fixed security headers of spec §4.6, then the response's own headers except
// any that carries CR/LF or names a header written here or Server/X-Powered-By.
[[nodiscard]] std::string serialize_response(const HttpResponse& response, std::chrono::system_clock::time_point now);

}  // namespace secure_frontend
```

`src/http_wire.cpp`:

```cpp
#include "secure_frontend/http_wire.hpp"

#include <array>
#include <cstdio>

namespace secure_frontend {
namespace {

constexpr std::array<std::pair<std::string_view, std::string_view>, 5> kSecurityHeaders{{
    {"Cache-Control", "no-store"},
    {"X-Content-Type-Options", "nosniff"},
    {"Content-Security-Policy", "default-src 'none'; frame-ancestors 'none'"},
    {"Referrer-Policy", "no-referrer"},
    {"Cross-Origin-Resource-Policy", "same-origin"},
}};

bool ascii_iequal(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; };
    if (lower(a[i]) != lower(b[i])) return false;
  }
  return true;
}

bool is_reserved_header(std::string_view name) {
  for (const auto& [fixed, value] : kSecurityHeaders) {
    (void)value;
    if (ascii_iequal(name, fixed)) return true;
  }
  return ascii_iequal(name, "Date") || ascii_iequal(name, "Connection") || ascii_iequal(name, "Content-Length") ||
         ascii_iequal(name, "Transfer-Encoding") || ascii_iequal(name, "Server") || ascii_iequal(name, "X-Powered-By");
}

}  // namespace

std::string_view reason_phrase(const int status) noexcept {
  switch (status) {
    case 100: return "Continue";
    case 200: return "OK";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 411: return "Length Required";
    case 413: return "Content Too Large";
    case 414: return "URI Too Long";
    case 415: return "Unsupported Media Type";
    case 417: return "Expectation Failed";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    case 505: return "HTTP Version Not Supported";
    default: return "Unknown";
  }
}

std::string imf_fixdate(const std::chrono::system_clock::time_point now) {
  // Civil-from-days (H. Hinnant), integer only: no <chrono> calendar types,
  // which g++ 11.4's libstdc++ does not fully provide.
  static constexpr const char* kDays[] = {"Thu", "Fri", "Sat", "Sun", "Mon", "Tue", "Wed"};   // 1970-01-01 was a Thursday
  static constexpr const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  const long long total = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
  long long days = total / 86400;
  long long secs = total % 86400;
  if (secs < 0) { secs += 86400; --days; }
  const long long weekday = ((days % 7) + 7) % 7;
  const long long z = days + 719468;
  const long long era = (z >= 0 ? z : z - 146096) / 146097;
  const long long doe = z - era * 146097;
  const long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const long long mp = (5 * doy + 2) / 153;
  const long long day = doy - (153 * mp + 2) / 5 + 1;
  const long long month = mp < 10 ? mp + 3 : mp - 9;
  const long long year = yoe + era * 400 + (month <= 2 ? 1 : 0);
  char buffer[40];
  std::snprintf(buffer, sizeof(buffer), "%s, %02lld %s %04lld %02lld:%02lld:%02lld GMT", kDays[weekday], day,
                kMonths[month - 1], year, secs / 3600, (secs / 60) % 60, secs % 60);
  return buffer;
}

std::string serialize_response(const HttpResponse& response, const std::chrono::system_clock::time_point now) {
  std::string output{"HTTP/1.1 "};
  output += std::to_string(response.status);
  output += ' ';
  output += reason_phrase(response.status);
  output += "\r\nDate: ";
  output += imf_fixdate(now);
  output += "\r\nConnection: close\r\n";
  if (response.status != 204) {
    output += "Content-Length: ";
    output += std::to_string(response.body.size());
    output += "\r\n";
  }
  for (const auto& [name, value] : kSecurityHeaders) {
    output += name;
    output += ": ";
    output += value;
    output += "\r\n";
  }
  for (const auto& [name, value] : response.headers) {
    // Values come from code, never from the request; the CR/LF guard stays (SP0).
    if (name.find_first_of("\r\n") != std::string::npos || value.find_first_of("\r\n") != std::string::npos ||
        is_reserved_header(name)) {
      continue;
    }
    output += name;
    output += ": ";
    output += value;
    output += "\r\n";
  }
  output += "\r\n";
  if (response.status != 204) output += response.body;
  return output;
}

}  // namespace secure_frontend
```

In `include/secure_frontend/http_request_handler.hpp`, add to `struct HttpResponse` after `body` the member `std::string_view log_code{};   // §7.2 code for the request log (spec §8); never sent` (a trailing member with a default initializer keeps every designated initializer valid).

In `src/api_router.cpp`: delete the function `append_json_string` from its anonymous namespace (anchor `if (character < 0x20U || character >= 0x80U) {`) and add `#include "secure_frontend/json.hpp"`, so the router uses SP1's shared writer (spec §3.9).

- [ ] **Step 4: Run to see GREEN**

```bash
cmake --build --preset linux-gcc && ctest --preset linux-gcc -R 'http_parser_tests|models_list_keeps_utf8_model_id' --output-on-failure
ctest --preset linux-gcc | tail -1
cmake --build --preset windows-llvm-mingw-cross && ctest --preset windows-llvm-mingw-cross -R 'http_parser_tests|api_router_tests' | tail -1
```

Expected: the four new tests pass; full suite `100% tests passed` (N₀ + 4); wine passes.

- [ ] **Step 5: Handoff row and commit** — add the row `| 1 | §3.9, §4.6, §5.3, §7 | api_constants.hpp, api_errors (§7.2 table), http_wire (security headers, Date), router uses the shared JSON writer | http_parser_tests.err_every_code_renders_fixed_body, …err_param_is_key_only, …wire_response_has_security_headers_and_date, api_router_tests.models_list_keeps_utf8_model_id | <line> | <line> |` and commit:

```bash
git -C "$(git rev-parse --show-toplevel)" add secure_frontend docs/AGENT_HANDOFF.md
git -C "$(git rev-parse --show-toplevel)" -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "api: one error table, one JSON writer, security headers and Date on every response (SP3 §3.9, §4.6, §7)" \
  -m "Tests: http_parser_tests.err_every_code_renders_fixed_body, err_param_is_key_only, wire_response_has_security_headers_and_date; api_router_tests.models_list_keeps_utf8_model_id. RED: <line>. wine: <line>." \
  -m "Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>" -m "Claude-Session: <session link>"
```

---

## Task 2: Request-head scan and the §4.2 parser

**Files:**
- Create: `include/secure_frontend/http_request_head.hpp`, `src/http_request_head.cpp`
- Modify: `CMakeLists.txt` (add `src/http_request_head.cpp` to `secure_frontend_api_core`), `tests/http_parser_test.cpp`

**Interfaces:**
- Consumes: `ApiError` (Task 1).
- Produces: `HeadLimits`, `HeadScan`, `HeadScanResult`, `scan_request_head()`, `HeaderField`, `RequestHead` (+ `values()`), `HeadParseResult`, `parse_request_head()`, `ascii_iequal()`, `is_http_token()`. (The SP1 `HttpLimits` → `HeadLimits` conversion lives in `subsystem_limits` — Task 8b — so `secure_frontend_api_core` never depends on the typed configuration.)

Parser order (one place, so every test can rely on it): (1) request line — split, version (`HTTP/1.1` ok; `HTTP/1.0`, `HTTP/2.0` → 505; other → 400), method token, target rules → 400; (2) header lines — obs-fold, bare CR/LF, NUL, name token, value bytes → 400; (3) field count > `max_headers` → 431; (4) any `Transfer-Encoding` → 411; (5) `Content-Length` count/syntax → 400; over `max_request_bytes − head_bytes` → 413; (6) `GET` with `Content-Length` > 0 → 400; `POST` without `Content-Length` → 411; (7) `Expect` other than `100-continue` (case-insensitive) → 417. The scan (before the parse) yields 414 (no CRLF within `max_request_line_bytes`, or a request line longer than it) and 431 (head longer than `max_header_bytes`).

- [ ] **Step 1: Write the failing tests** — add to `tests/http_parser_test.cpp` (include `secure_frontend/http_request_head.hpp`; add each case to `run_cases`):

```cpp
constexpr secure_frontend::HeadLimits kLimits{.max_request_line_bytes = 4096, .max_header_bytes = 16384,
                                              .max_headers = 64, .max_request_bytes = 65536};

std::variant<secure_frontend::RequestHead, ApiError> head_of(const std::string& text) {
  const auto scan = secure_frontend::scan_request_head(text, kLimits);
  if (scan.state == secure_frontend::HeadScan::line_too_long) return ApiError::request_line_too_long;
  if (scan.state == secure_frontend::HeadScan::header_too_large) return ApiError::header_too_large;
  if (scan.state != secure_frontend::HeadScan::complete) return ApiError::internal_error;   // "incomplete" marker for tests
  return secure_frontend::parse_request_head(std::string_view{text}.substr(0, scan.head_bytes), kLimits);
}
bool is_error(const std::variant<secure_frontend::RequestHead, ApiError>& r, ApiError e) {
  return std::holds_alternative<ApiError>(r) && std::get<ApiError>(r) == e;
}
const std::string kHost = "Host: 127.0.0.1:8080\r\n";

void dos01_request_line_over_limit_is_414() {
  const std::string long_target(4100, 'a');
  expect(is_error(head_of("GET /" + long_target + " HTTP/1.1\r\n" + kHost + "\r\n"), ApiError::request_line_too_long),
         "a complete head with a request line over 4096 bytes is 414");
  const auto partial = secure_frontend::scan_request_head("GET /" + long_target, kLimits);
  expect(partial.state == secure_frontend::HeadScan::line_too_long, "414 is known before the line ends");
  const auto short_partial = secure_frontend::scan_request_head("GET /abc", kLimits);
  expect(short_partial.state == secure_frontend::HeadScan::need_more, "a short partial line needs more bytes");
}

void dos01_header_block_over_limit_is_431() {
  std::string head = "GET /health HTTP/1.1\r\n" + kHost;
  while (head.size() < 16500U) head += "X-Fill: " + std::string(100, 'f') + "\r\n";
  expect(secure_frontend::scan_request_head(head, kLimits).state == secure_frontend::HeadScan::header_too_large,
         "431 is known before the blank line arrives");
  expect(is_error(head_of(head + "\r\n"), ApiError::header_too_large), "a complete head over 16384 bytes is 431");
}

void dos01_too_many_headers_is_431() {
  std::string head = "GET /health HTTP/1.1\r\n" + kHost;
  for (int i = 0; i < 64; ++i) head += "X-" + std::to_string(i) + ": v\r\n";
  expect(is_error(head_of(head + "\r\n"), ApiError::header_too_large), "65 fields are 431");
  std::string ok = "GET /health HTTP/1.1\r\n" + kHost;
  for (int i = 0; i < 63; ++i) ok += "X-" + std::to_string(i) + ": v\r\n";
  expect(std::holds_alternative<secure_frontend::RequestHead>(head_of(ok + "\r\n")), "64 fields are accepted");
}

void dos01_transfer_encoding_variants_are_411() {
  for (const std::string te : {"Transfer-Encoding: chunked\r\n", "Transfer-Encoding: Chunked\r\n", "Transfer-Encoding:  chunked\r\n",
                               "Transfer-Encoding: gzip, chunked\r\n", "transfer-encoding: identity\r\n",
                               "Transfer-Encoding: chunked\r\nContent-Length: 2\r\n"}) {
    expect(is_error(head_of("POST /v1/chat/completions HTTP/1.1\r\n" + kHost + te + "\r\n"), ApiError::length_required),
           "411 for " + te);
  }
  expect(is_error(head_of("POST /v1/chat/completions HTTP/1.1\r\n" + kHost + "Transfer-Encoding : chunked\r\n\r\n"),
                  ApiError::invalid_request), "space before the colon is 400");
}

void dos01_content_length_variants() {
  const auto post = [](const std::string& cl) { return head_of("POST /v1/chat/completions HTTP/1.1\r\n" + kHost + cl + "\r\n"); };
  expect(is_error(post("Content-Length: 2\r\nContent-Length: 2\r\n"), ApiError::invalid_request), "duplicate");
  expect(is_error(post("Content-Length: +1\r\n"), ApiError::invalid_request), "sign");
  expect(is_error(post("Content-Length: 0x10\r\n"), ApiError::invalid_request), "hex");
  expect(is_error(post("Content-Length: 12345678901\r\n"), ApiError::invalid_request), "11 digits");
  expect(is_error(post("Content-Length: \r\n"), ApiError::invalid_request), "empty");
  expect(is_error(post("Content-Length: 70000\r\n"), ApiError::request_too_large), "over the whole-request limit is 413");
  expect(is_error(post(""), ApiError::length_required), "POST without Content-Length is 411");
  expect(is_error(head_of("GET /health HTTP/1.1\r\n" + kHost + "Content-Length: 1\r\n\r\n"), ApiError::invalid_request),
         "GET with a body is 400");
  const auto ok = post("Content-Length: 0000000002\r\n");
  expect(std::holds_alternative<secure_frontend::RequestHead>(ok) && std::get<secure_frontend::RequestHead>(ok).content_length == 2U,
         "ten digits with leading zeros are accepted");
  // Mismatch and bytes after the body are body-stage rules: see the in-memory seam of Task 6a.
}

void head_request_line_and_header_syntax() {
  expect(is_error(head_of("GET /health HTTP/1.0\r\n" + kHost + "\r\n"), ApiError::http_version_not_supported), "1.0 is 505");
  expect(is_error(head_of("GET /health HTTP/2.0\r\n" + kHost + "\r\n"), ApiError::http_version_not_supported), "2.0 is 505");
  expect(is_error(head_of("GET /health HTTP/1.2\r\n" + kHost + "\r\n"), ApiError::invalid_request), "other versions are 400");
  expect(is_error(head_of("GET  /health HTTP/1.1\r\n" + kHost + "\r\n"), ApiError::invalid_request), "double space");
  for (const std::string target : {"/a?b", "/a#b", "/a%2e", "/a\\b", "*", "http://x/", "/a\x7f"}) {
    expect(is_error(head_of("GET " + target + " HTTP/1.1\r\n" + kHost + "\r\n"), ApiError::invalid_request), "target " + target);
  }
  expect(is_error(head_of("G(T /health HTTP/1.1\r\n" + kHost + "\r\n"), ApiError::invalid_request), "method token");
  expect(is_error(head_of("GET /health HTTP/1.1\r\n" + kHost + " folded\r\n\r\n"), ApiError::invalid_request), "obs-fold");
  expect(is_error(head_of("GET /health HTTP/1.1\r\n" + kHost + "X: a\nY: b\r\n\r\n"), ApiError::invalid_request), "bare LF");
  expect(is_error(head_of("GET /health HTTP/1.1\r\n" + kHost + std::string("X: a\0b\r\n\r\n", 10)), ApiError::invalid_request), "NUL");
  expect(is_error(head_of("GET /health HTTP/1.1\r\n" + kHost + "X: caf\xc3\xa9\r\n\r\n"), ApiError::invalid_request), "non-ASCII value");
  expect(is_error(head_of("POST /v1/chat/completions HTTP/1.1\r\n" + kHost + "Content-Length: 2\r\nExpect: foo\r\n\r\n"),
                  ApiError::expectation_failed), "Expect: foo is 417");
  const auto cont = head_of("POST /v1/chat/completions HTTP/1.1\r\n" + kHost + "Content-Length: 2\r\nExpect: 100-Continue\r\n\r\n");
  expect(std::holds_alternative<secure_frontend::RequestHead>(cont) && std::get<secure_frontend::RequestHead>(cont).expect_continue,
         "Expect: 100-continue is recognised case-insensitively");
  const auto parsed = head_of("DELETE /v1/models HTTP/1.1\r\n" + kHost + "Origin: http://127.0.0.1:8080\r\n\r\n");
  expect(std::holds_alternative<secure_frontend::RequestHead>(parsed), "any token method parses (405 comes after auth)");
  if (const auto* head = std::get_if<secure_frontend::RequestHead>(&parsed)) {
    expect(head->method == "DELETE" && head->target == "/v1/models", "method and target");
    expect(head->values("HOST").size() == 1U && head->values("origin").front() == "http://127.0.0.1:8080", "values()");
  }
}
```

Register in the `secure_frontend_http_parser_tests` case list: `dos01_request_line_over_limit_is_414 dos01_header_block_over_limit_is_431 dos01_too_many_headers_is_431 dos01_transfer_encoding_variants_are_411 dos01_content_length_variants head_request_line_and_header_syntax`.

- [ ] **Step 2: Run to see RED**

Run: `cmake --build --preset linux-gcc 2>&1 | tail -3`
Expected: build FAIL `secure_frontend/http_request_head.hpp: No such file or directory`.

- [ ] **Step 3: Implement** — `include/secure_frontend/http_request_head.hpp`:

```cpp
#pragma once

#include "secure_frontend/api_errors.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace secure_frontend {

struct HeadLimits final {
  std::size_t max_request_line_bytes;
  std::size_t max_header_bytes;
  std::size_t max_headers;
  std::size_t max_request_bytes;
};

enum class HeadScan { need_more, complete, line_too_long, header_too_large };
struct HeadScanResult final {
  HeadScan state;
  std::size_t head_bytes{0};   // complete only: bytes up to and including the final CRLFCRLF
};
// Looks only at the bytes received so far; never allocates.
[[nodiscard]] HeadScanResult scan_request_head(std::string_view received, const HeadLimits& limits) noexcept;

struct HeaderField final {
  std::string name;    // as sent
  std::string value;   // optional whitespace trimmed
};

struct RequestHead final {
  std::string method;
  std::string target;
  std::vector<HeaderField> fields;
  std::size_t head_bytes{0};
  std::optional<std::size_t> content_length;
  bool expect_continue{false};
  // Every value of the named field (ASCII case-insensitive), in order.
  [[nodiscard]] std::vector<std::string_view> values(std::string_view name) const;
};

using HeadParseResult = std::variant<RequestHead, ApiError>;
// `head` is exactly the scanned head (request line, fields, CRLFCRLF). The
// rules and their order are spec §4.2 / plan Task 2.
[[nodiscard]] HeadParseResult parse_request_head(std::string_view head, const HeadLimits& limits);

[[nodiscard]] bool ascii_iequal(std::string_view a, std::string_view b) noexcept;
[[nodiscard]] bool is_http_token(std::string_view value) noexcept;

}  // namespace secure_frontend
```

`src/http_request_head.cpp`:

```cpp
#include "secure_frontend/http_request_head.hpp"

#include <algorithm>

namespace secure_frontend {
namespace {

char lower(char c) noexcept { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c; }

std::string_view trim_ows(std::string_view value) noexcept {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1U);
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1U);
  return value;
}

bool target_ok(std::string_view target) noexcept {
  if (target.empty() || target.front() != '/') return false;
  return std::all_of(target.begin(), target.end(), [](char c) {
    const auto byte = static_cast<unsigned char>(c);
    return byte >= 0x21U && byte <= 0x7eU && c != '?' && c != '#' && c != '%' && c != '\\';
  });
}

bool value_ok(std::string_view value) noexcept {
  return std::all_of(value.begin(), value.end(), [](char c) {
    const auto byte = static_cast<unsigned char>(c);
    return (byte >= 0x20U && byte <= 0x7eU) || c == '\t';
  });
}

}  // namespace

bool ascii_iequal(const std::string_view a, const std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (lower(a[i]) != lower(b[i])) return false;
  }
  return true;
}

bool is_http_token(const std::string_view value) noexcept {
  if (value.empty()) return false;
  return std::all_of(value.begin(), value.end(), [](char c) {
    const bool alnum = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    return alnum || std::string_view{"!#$%&'*+-.^_`|~"}.find(c) != std::string_view::npos;
  });
}

HeadScanResult scan_request_head(const std::string_view received, const HeadLimits& limits) noexcept {
  const std::size_t line_end = received.find("\r\n");
  if (line_end == std::string_view::npos) {
    return {received.size() > limits.max_request_line_bytes ? HeadScan::line_too_long : HeadScan::need_more, 0U};
  }
  if (line_end > limits.max_request_line_bytes) return {HeadScan::line_too_long, 0U};
  const std::size_t end = received.find("\r\n\r\n");
  if (end == std::string_view::npos) {
    return {received.size() > limits.max_header_bytes ? HeadScan::header_too_large : HeadScan::need_more, 0U};
  }
  const std::size_t head_bytes = end + 4U;
  if (head_bytes > limits.max_header_bytes) return {HeadScan::header_too_large, 0U};
  return {HeadScan::complete, head_bytes};
}

std::vector<std::string_view> RequestHead::values(const std::string_view name) const {
  std::vector<std::string_view> found;
  for (const auto& field : fields) {
    if (ascii_iequal(field.name, name)) found.emplace_back(field.value);
  }
  return found;
}

HeadParseResult parse_request_head(const std::string_view head, const HeadLimits& limits) {
  if (head.size() < 4U || head.substr(head.size() - 4U) != "\r\n\r\n") return ApiError::invalid_request;
  const std::string_view block = head.substr(0, head.size() - 2U);   // keeps the last field's CRLF
  // Bare CR or bare LF anywhere is a syntax error.
  for (std::size_t i = 0; i < block.size(); ++i) {
    if (block[i] == '\n' && (i == 0U || block[i - 1U] != '\r')) return ApiError::invalid_request;
    if (block[i] == '\r' && (i + 1U >= block.size() || block[i + 1U] != '\n')) return ApiError::invalid_request;
    if (block[i] == '\0') return ApiError::invalid_request;
  }
  const std::size_t line_end = block.find("\r\n");
  const std::string_view line = block.substr(0, line_end);
  const std::size_t sp1 = line.find(' ');
  const std::size_t sp2 = sp1 == std::string_view::npos ? sp1 : line.find(' ', sp1 + 1U);
  if (sp1 == std::string_view::npos || sp2 == std::string_view::npos || line.find(' ', sp2 + 1U) != std::string_view::npos) {
    return ApiError::invalid_request;
  }
  const std::string_view method = line.substr(0, sp1);
  const std::string_view target = line.substr(sp1 + 1U, sp2 - sp1 - 1U);
  const std::string_view version = line.substr(sp2 + 1U);
  if (version == "HTTP/1.0" || version == "HTTP/2.0") return ApiError::http_version_not_supported;
  if (version != "HTTP/1.1" || !is_http_token(method) || !target_ok(target)) return ApiError::invalid_request;

  RequestHead parsed;
  parsed.method = std::string{method};
  parsed.target = std::string{target};
  parsed.head_bytes = head.size();
  std::size_t start = line_end + 2U;
  while (start < block.size()) {
    const std::size_t end = block.find("\r\n", start);
    const std::string_view field = block.substr(start, end - start);
    if (field.empty() || field.front() == ' ' || field.front() == '\t') return ApiError::invalid_request;   // obs-fold
    const std::size_t colon = field.find(':');
    if (colon == std::string_view::npos || !is_http_token(field.substr(0, colon))) return ApiError::invalid_request;
    const std::string_view raw = field.substr(colon + 1U);
    if (!value_ok(raw)) return ApiError::invalid_request;
    parsed.fields.push_back({std::string{field.substr(0, colon)}, std::string{trim_ows(raw)}});
    start = end + 2U;
  }
  if (parsed.fields.size() > limits.max_headers) return ApiError::header_too_large;
  if (!parsed.values("transfer-encoding").empty()) return ApiError::length_required;
  const auto lengths = parsed.values("content-length");
  if (lengths.size() > 1U) return ApiError::invalid_request;
  if (lengths.size() == 1U) {
    const std::string_view digits = lengths.front();
    if (digits.empty() || digits.size() > 10U ||
        !std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; })) {
      return ApiError::invalid_request;
    }
    std::size_t value = 0;
    for (const char c : digits) value = value * 10U + static_cast<std::size_t>(c - '0');
    if (value > limits.max_request_bytes - std::min(limits.max_request_bytes, head.size())) return ApiError::request_too_large;
    parsed.content_length = value;
  }
  if (parsed.method == "GET" && parsed.content_length.value_or(0U) > 0U) return ApiError::invalid_request;
  if (parsed.method == "POST" && !parsed.content_length.has_value()) return ApiError::length_required;
  for (const auto expectation : parsed.values("expect")) {
    if (!ascii_iequal(expectation, "100-continue")) return ApiError::expectation_failed;
    parsed.expect_continue = true;
  }
  return parsed;
}

}  // namespace secure_frontend
```

Add `src/http_request_head.cpp` to the `SOURCES` of `secure_frontend_api_core`.

- [ ] **Step 4: Run to see GREEN**

```bash
cmake --build --preset linux-gcc && ctest --preset linux-gcc -R http_parser_tests --output-on-failure && ctest --preset linux-gcc | tail -1
cmake --build --preset windows-llvm-mingw-cross && ctest --preset windows-llvm-mingw-cross -R http_parser_tests | tail -1
```

Expected: 9 http_parser_tests pass; full suite passes.

- [ ] **Step 5: Handoff row and commit** — row `| 2 | DOS-01, §4.2 | request-head scan + parser (414/431/411/413/505/417/400) | http_parser_tests.dos01_request_line_over_limit_is_414, …dos01_header_block_over_limit_is_431, …dos01_too_many_headers_is_431, …dos01_transfer_encoding_variants_are_411, …dos01_content_length_variants, …head_request_line_and_header_syntax | … | … |`.

```bash
git -C "$(git rev-parse --show-toplevel)" add secure_frontend docs/AGENT_HANDOFF.md
git -C "$(git rev-parse --show-toplevel)" -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "http: request-head scan and strict parser, 414/431/411/413/505/417 (SP3 DOS-01, §4.2)" \
  -m "Tests: http_parser_tests.dos01_request_line_over_limit_is_414, dos01_header_block_over_limit_is_431, dos01_too_many_headers_is_431, dos01_transfer_encoding_variants_are_411, dos01_content_length_variants, head_request_line_and_header_syntax. RED: <line>. wine: <line>." \
  -m "Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>" -m "Claude-Session: <session link>"
```

---

## Task 3: Host, Origin, CORS and preflight policy

**Files:**
- Modify: `include/secure_frontend/http_policy.hpp`, `src/http_policy.cpp` (rewritten), `tests/http_policy_test.cpp` (rewritten cases), `CMakeLists.txt`

**Interfaces:**
- Consumes: SP1 `IpAddress` (`family()`, `scope()`, `canonical_text()`), `AddressScope`; `RequestHead`, `ascii_iequal`, `is_http_token` (Task 2); `make_error_response` is **not** used here (the router maps verdicts).
- Produces: `HttpPolicy`, `make_http_policy()`, `PolicyVerdict`, `check_host()`, `OriginDecision`, `check_origin()`, `kPreflightAllowHeaders`, `evaluate_preflight()`, `is_valid_host_alias()`, `is_valid_cors_origin()`. The old `HttpPolicyError`, `HttpPolicyDecision`, `validate_http_request_headers` are deleted (their only caller, `handle_http_request`, is rewritten in Task 6a; until then Task 3 keeps `src/http_request_handler.cpp` compiling by the shim in Step 3).

Rules (spec §4.3, §4.4): allowed hosts = bound authority (`127.0.0.1:8080`, `[::1]:8080`) and, for a loopback bind, `localhost:<port>`; plus every `listener.host_aliases` entry (lower-cased); for a wildcard bind **only** the aliases. A `Host` value is compared after ASCII lower-casing (ports have no letters, so this folds names only). One `Host` exactly: none or several → malformed (400); syntactically invalid → malformed (400); valid but not allowed → forbidden (403). `Origin`: absent → allowed; several → forbidden; equal to `"http://" + allowed host` → allowed without CORS; exactly in `cors_origins` → allowed with `allow_origin`; anything else (including `null` and every malformed value) → forbidden. No folding for origins. Preflight = `OPTIONS` + `Origin` + `Access-Control-Request-Method`: allowed only for a `cors_origins` origin, method exactly `GET` or `POST` (one value), every `Access-Control-Request-Headers` item (comma lists, any number of fields, trimmed, case-insensitive) in the fixed list → `204` with `Access-Control-Allow-Origin`, `Access-Control-Allow-Methods: GET, POST`, `Access-Control-Allow-Headers: <kPreflightAllowHeaders>`, `Access-Control-Max-Age: 600`, `Vary: Origin`; any other preflight → `403 forbidden` (as an `HttpResponse` from `make_error_response`).

- [ ] **Step 1: Write the failing tests** — replace the body of `tests/http_policy_test.cpp` (keep its `failures`/`expect` and the `run_cases` main shape) with:

```cpp
#include "secure_frontend/api_errors.hpp"
#include "secure_frontend/config.hpp"
#include "secure_frontend/http_policy.hpp"
#include "secure_frontend/http_request_head.hpp"
#include "support/test_support.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {
int failures = 0;
void expect(bool condition, const std::string& message) {
  if (!condition) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
}
using secure_frontend::PolicyVerdict;

secure_frontend::HttpPolicy policy_for(const char* host, std::vector<std::string> aliases = {},
                                       std::vector<std::string> cors = {}) {
  return secure_frontend::make_http_policy(*secure_frontend::parse_listener_address(host), 8080, aliases, cors);
}
PolicyVerdict host(const secure_frontend::HttpPolicy& p, std::vector<std::string_view> values) {
  return secure_frontend::check_host(p, values);
}

void web01_host_matrix() {
  const auto p = policy_for("127.0.0.1", {"strata.lan:8080"});
  expect(host(p, {"127.0.0.1:8080"}) == PolicyVerdict::allowed, "bound authority");
  expect(host(p, {"localhost:8080"}) == PolicyVerdict::allowed, "localhost alias on loopback (E10)");
  expect(host(p, {"strata.lan:8080"}) == PolicyVerdict::allowed, "configured alias");
  expect(host(p, {}) == PolicyVerdict::malformed, "missing Host is 400");
  expect(host(p, {"127.0.0.1:8080", "127.0.0.1:8080"}) == PolicyVerdict::malformed, "duplicate Host is 400");
  expect(host(p, {"evil.example:8080"}) == PolicyVerdict::forbidden, "unknown name is 403");
  expect(host(p, {"127.0.0.2:8080"}) == PolicyVerdict::forbidden, "other IP literal is 403");
  expect(host(p, {"127.0.0.1:8081"}) == PolicyVerdict::forbidden, "other port is 403");
  expect(host(p, {"127.0.0.1"}) == PolicyVerdict::forbidden, "missing port is 403");
  expect(host(p, {"localhost.:8080"}) == PolicyVerdict::malformed, "trailing dot is 400");
  expect(host(p, {"user@127.0.0.1:8080"}) == PolicyVerdict::malformed, "userinfo is 400");
  expect(host(p, {"127.0.0.1:8080\x01"}) == PolicyVerdict::malformed, "control byte is 400");
  expect(host(p, {"::1:8080"}) == PolicyVerdict::malformed, "unbracketed IPv6 is 400");
  const auto v6 = policy_for("::1");
  expect(host(v6, {"[::1]:8080"}) == PolicyVerdict::allowed, "bracketed bound IPv6 authority");
  expect(host(v6, {"[::1]"}) == PolicyVerdict::forbidden && host(v6, {"::1"}) == PolicyVerdict::malformed, "v6 without port");
  const auto wildcard = policy_for("0.0.0.0", {"box.lan:8080"});
  expect(host(wildcard, {"0.0.0.0:8080"}) == PolicyVerdict::forbidden, "a wildcard bind has no bound authority");
  expect(host(wildcard, {"localhost:8080"}) == PolicyVerdict::forbidden, "no localhost alias without a loopback bind");
  expect(host(wildcard, {"box.lan:8080"}) == PolicyVerdict::allowed, "a wildcard bind trusts only the aliases");
}

void web02_host_name_case_is_folded() {
  const auto p = policy_for("127.0.0.1", {"Strata.LAN:8080"});
  expect(host(p, {"LOCALHOST:8080"}) == PolicyVerdict::allowed && host(p, {"Localhost:8080"}) == PolicyVerdict::allowed,
         "the ASCII case of a name is folded");
  expect(host(p, {"strata.lan:8080"}) == PolicyVerdict::allowed, "aliases are stored lower-cased");
  const auto origin = secure_frontend::check_origin(p, {"http://LOCALHOST:8080"});
  expect(origin.verdict == PolicyVerdict::forbidden, "origins are compared exactly (no folding)");
}

void web03_origin_matrix() {
  const auto p = policy_for("127.0.0.1", {}, {"http://app.example:3000"});
  const auto origin = [&p](std::vector<std::string_view> values) { return secure_frontend::check_origin(p, values); };
  expect(origin({}).verdict == PolicyVerdict::allowed && !origin({}).allow_origin, "no Origin is allowed, no CORS");
  expect(origin({"http://127.0.0.1:8080"}).verdict == PolicyVerdict::allowed && !origin({"http://127.0.0.1:8080"}).allow_origin,
         "same origin, no CORS headers");
  expect(origin({"http://localhost:8080"}).verdict == PolicyVerdict::allowed, "same origin through the localhost alias");
  const auto trusted = origin({"http://app.example:3000"});
  expect(trusted.verdict == PolicyVerdict::allowed && trusted.allow_origin == std::optional<std::string>{"http://app.example:3000"},
         "configured origin gets its own exact value");
  for (const std::string_view bad : {"null", "http://app.example.evil:3000", "http://APP.example:3000", "http://127.0.0.1:80",
                                     "https://127.0.0.1:8080", "http://127.0.0.1:8080/", "http://app.example:3000/",
                                     "http://127.0.0.1", "file://", "http://app.example:3000 "}) {
    expect(origin({bad}).verdict == PolicyVerdict::forbidden && !origin({bad}).allow_origin, "403 for origin " + std::string{bad});
  }
  expect(origin({"http://127.0.0.1:8080", "http://127.0.0.1:8080"}).verdict == PolicyVerdict::forbidden, "duplicate Origin is 403");
}

secure_frontend::RequestHead preflight(std::string origin, std::string method, std::vector<std::string> request_headers) {
  secure_frontend::RequestHead head;
  head.method = "OPTIONS";
  head.target = "/v1/chat/completions";
  head.fields.push_back({"Host", "127.0.0.1:8080"});
  head.fields.push_back({"Origin", std::move(origin)});
  head.fields.push_back({"Access-Control-Request-Method", std::move(method)});
  for (auto& value : request_headers) head.fields.push_back({"Access-Control-Request-Headers", std::move(value)});
  return head;
}

void web06_preflight_rules_unit() {
  const auto p = policy_for("127.0.0.1", {}, {"http://app.example:3000"});
  const auto ok = secure_frontend::evaluate_preflight(p, preflight("http://app.example:3000", "POST",
                                                                   {"Authorization, Content-Type", "X-Stainless-OS,x-stainless-lang"}));
  expect(ok.has_value() && ok->status == 204, "allowed origin, method and headers give 204");
  if (ok) {
    expect(ok->header_value("Access-Control-Allow-Origin") == std::optional<std::string_view>{"http://app.example:3000"}, "ACAO exact");
    expect(ok->header_value("Access-Control-Allow-Methods") == std::optional<std::string_view>{"GET, POST"}, "methods");
    expect(ok->header_value("Access-Control-Allow-Headers") == std::optional<std::string_view>{secure_frontend::kPreflightAllowHeaders}, "headers");
    expect(ok->header_value("Access-Control-Max-Age") == std::optional<std::string_view>{"600"}, "max age");
    expect(ok->header_value("Vary") == std::optional<std::string_view>{"Origin"}, "vary");
    expect(!ok->header_value("Access-Control-Allow-Credentials").has_value(), "never credentials");
  }
  const auto forbidden = [&p](secure_frontend::RequestHead head) {
    const auto r = secure_frontend::evaluate_preflight(p, head);
    return r.has_value() && r->status == 403 && !r->header_value("Access-Control-Allow-Origin").has_value();
  };
  expect(forbidden(preflight("http://127.0.0.1:8080", "POST", {})), "same origin is not a CORS origin");
  expect(forbidden(preflight("http://other.example:3000", "POST", {})), "unconfigured origin");
  expect(forbidden(preflight("http://app.example:3000", "DELETE", {})), "other method");
  expect(forbidden(preflight("http://app.example:3000", "post", {})), "method is case-sensitive");
  expect(forbidden(preflight("http://app.example:3000", "POST", {"x-api-key"})), "header outside the list");
  secure_frontend::RequestHead plain = preflight("http://app.example:3000", "POST", {});
  plain.fields.pop_back();   // no Access-Control-Request-Method: not a preflight
  expect(!secure_frontend::evaluate_preflight(p, plain).has_value(), "OPTIONS without ACRM is not a preflight");
}

void cors_and_alias_grammar() {
  for (const char* good : {"http://app.example:3000", "https://app.example", "http://127.0.0.1:5173", "http://[::1]:5173"}) {
    expect(secure_frontend::is_valid_cors_origin(good), std::string{"valid origin "} + good);
  }
  for (const char* bad : {"*", "null", "http://app.example/", "http://App.example", "ftp://x", "http://x:0", "http://", "app.example"}) {
    expect(!secure_frontend::is_valid_cors_origin(bad), std::string{"invalid origin "} + bad);
  }
  for (const char* good : {"strata.lan:8080", "strata.lan", "172.16.1.5:8080", "[fe80::1]:8080"}) {
    expect(secure_frontend::is_valid_host_alias(good), std::string{"valid alias "} + good);
  }
  for (const char* bad : {"", "http://x", "x:", "x:99999", "a b", "x.:80", "user@x"}) {
    expect(!secure_frontend::is_valid_host_alias(bad), std::string{"invalid alias "} + bad);
  }
}
}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"web01_host_matrix", web01_host_matrix},
      {"web02_host_name_case_is_folded", web02_host_name_case_is_folded},
      {"web03_origin_matrix", web03_origin_matrix},
      {"web06_preflight_rules_unit", web06_preflight_rules_unit},
      {"cors_and_alias_grammar", cors_and_alias_grammar},
  }, failures);
}
```

In `CMakeLists.txt` replace the `secure_frontend_add_cases(secure_frontend_http_policy_tests` list (anchor `accepts_exact_host_without_browser_origin`) with `web01_host_matrix web02_host_name_case_is_folded web03_origin_matrix web06_preflight_rules_unit cors_and_alias_grammar`, and add `secure_frontend_api_core secure_frontend_config secure_frontend_http_request_handler` to that test's link list.

- [ ] **Step 2: Run to see RED**

Run: `cmake --build --preset linux-gcc 2>&1 | grep -m1 error`
Expected: FAIL `'make_http_policy' is not a member of 'secure_frontend'`.

- [ ] **Step 3: Implement** — `include/secure_frontend/http_policy.hpp`:

```cpp
#pragma once

#include "secure_frontend/config.hpp"
#include "secure_frontend/http_request_handler.hpp"
#include "secure_frontend/http_request_head.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace secure_frontend {

// Computed once at startup from the validated listener (spec §4.3, §4.4).
// allowed_hosts and cors_origins hold exact, lower-case serializations.
struct HttpPolicy final {
  std::vector<std::string> allowed_hosts;
  std::vector<std::string> same_origins;   // "http://" + each allowed host
  std::vector<std::string> cors_origins;
};

[[nodiscard]] HttpPolicy make_http_policy(const IpAddress& bind, std::uint16_t port,
                                          const std::vector<std::string>& host_aliases,
                                          const std::vector<std::string>& cors_origins);

enum class PolicyVerdict { allowed, malformed, forbidden };
[[nodiscard]] PolicyVerdict check_host(const HttpPolicy& policy, const std::vector<std::string_view>& hosts);

struct OriginDecision final {
  PolicyVerdict verdict;
  std::optional<std::string> allow_origin;   // set only for a configured CORS origin
};
[[nodiscard]] OriginDecision check_origin(const HttpPolicy& policy, const std::vector<std::string_view>& origins);

inline constexpr std::string_view kPreflightAllowHeaders =
    "authorization, content-type, x-stainless-arch, x-stainless-lang, x-stainless-os, x-stainless-package-version, "
    "x-stainless-retry-count, x-stainless-runtime, x-stainless-runtime-version, x-stainless-timeout";
// nullopt: not a preflight. Otherwise a 204 or a 403 response (never reaches auth or a route).
[[nodiscard]] std::optional<HttpResponse> evaluate_preflight(const HttpPolicy& policy, const RequestHead& head);

// Config grammars (SP1 addendum rows listener.host_aliases / listener.cors_origins).
[[nodiscard]] bool is_valid_host_alias(std::string_view value) noexcept;
[[nodiscard]] bool is_valid_cors_origin(std::string_view value) noexcept;
[[nodiscard]] bool is_valid_host_header(std::string_view value);

}  // namespace secure_frontend
```

`src/http_policy.cpp`: keep the existing helpers `has_control_or_non_ascii`, `is_decimal_port`, `is_valid_ipv4`, `is_valid_hostname`, `is_hexadecimal`, `is_valid_ipv6` and the public `is_valid_host_header` unchanged (anchor `bool is_valid_host_header(std::string_view value) {`); delete `is_valid_origin_header`, `is_allowed`, `reject` and `validate_http_request_headers`; add `#include "secure_frontend/api_errors.hpp"` and `#include <algorithm>` and, at namespace scope after `is_valid_host_header`:

```cpp
namespace {

std::string lower_ascii(std::string_view value) {
  std::string out{value};
  for (char& c : out) c = c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
  return out;
}

bool contains(const std::vector<std::string>& values, std::string_view candidate) {
  return std::find(values.begin(), values.end(), candidate) != values.end();
}

std::string bound_authority(const IpAddress& bind, std::uint16_t port) {
  const std::string text = bind.canonical_text();
  return (bind.family() == IpFamily::v6 ? "[" + text + "]" : text) + ":" + std::to_string(port);
}

constexpr std::string_view kAllowedRequestHeaders[] = {
    "authorization", "content-type", "x-stainless-arch", "x-stainless-lang", "x-stainless-os",
    "x-stainless-package-version", "x-stainless-retry-count", "x-stainless-runtime",
    "x-stainless-runtime-version", "x-stainless-timeout"};

bool request_headers_allowed(const std::vector<std::string_view>& lists) {
  for (std::string_view list : lists) {
    while (!list.empty()) {
      const std::size_t comma = list.find(',');
      std::string_view item = list.substr(0, comma);
      while (!item.empty() && (item.front() == ' ' || item.front() == '\t')) item.remove_prefix(1U);
      while (!item.empty() && (item.back() == ' ' || item.back() == '\t')) item.remove_suffix(1U);
      if (!item.empty()) {
        const std::string lowered = lower_ascii(item);
        if (std::find(std::begin(kAllowedRequestHeaders), std::end(kAllowedRequestHeaders), lowered) ==
            std::end(kAllowedRequestHeaders)) {
          return false;
        }
      }
      if (comma == std::string_view::npos) break;
      list.remove_prefix(comma + 1U);
    }
  }
  return true;
}

}  // namespace

bool is_valid_host_alias(const std::string_view value) noexcept {
  return value.find('@') == std::string_view::npos && is_valid_host_header(value);
}

bool is_valid_cors_origin(const std::string_view value) noexcept {
  std::string_view rest;
  if (value.rfind("http://", 0) == 0) rest = value.substr(7U);
  else if (value.rfind("https://", 0) == 0) rest = value.substr(8U);
  else return false;
  if (rest.empty() || rest.find_first_of("/?#@") != std::string_view::npos) return false;
  if (std::any_of(rest.begin(), rest.end(), [](char c) { return c >= 'A' && c <= 'Z'; })) return false;
  return is_valid_host_header(rest);
}

HttpPolicy make_http_policy(const IpAddress& bind, const std::uint16_t port, const std::vector<std::string>& host_aliases,
                            const std::vector<std::string>& cors_origins) {
  HttpPolicy policy;
  if (bind.scope() != AddressScope::wildcard) policy.allowed_hosts.push_back(bound_authority(bind, port));
  if (bind.scope() == AddressScope::loopback) policy.allowed_hosts.push_back("localhost:" + std::to_string(port));
  for (const auto& alias : host_aliases) {
    const std::string lowered = lower_ascii(alias);
    if (!contains(policy.allowed_hosts, lowered)) policy.allowed_hosts.push_back(lowered);
  }
  for (const auto& host : policy.allowed_hosts) policy.same_origins.push_back("http://" + host);
  policy.cors_origins = cors_origins;
  return policy;
}

PolicyVerdict check_host(const HttpPolicy& policy, const std::vector<std::string_view>& hosts) {
  if (hosts.size() != 1U || !is_valid_host_header(hosts.front())) return PolicyVerdict::malformed;
  return contains(policy.allowed_hosts, lower_ascii(hosts.front())) ? PolicyVerdict::allowed : PolicyVerdict::forbidden;
}

OriginDecision check_origin(const HttpPolicy& policy, const std::vector<std::string_view>& origins) {
  if (origins.empty()) return {PolicyVerdict::allowed, std::nullopt};
  if (origins.size() != 1U) return {PolicyVerdict::forbidden, std::nullopt};
  if (contains(policy.same_origins, origins.front())) return {PolicyVerdict::allowed, std::nullopt};
  if (contains(policy.cors_origins, origins.front())) return {PolicyVerdict::allowed, std::string{origins.front()}};
  return {PolicyVerdict::forbidden, std::nullopt};
}

std::optional<HttpResponse> evaluate_preflight(const HttpPolicy& policy, const RequestHead& head) {
  const auto origins = head.values("origin");
  const auto methods = head.values("access-control-request-method");
  if (head.method != "OPTIONS" || origins.empty() || methods.empty()) return std::nullopt;
  const bool allowed = origins.size() == 1U && contains(policy.cors_origins, origins.front()) && methods.size() == 1U &&
                       (methods.front() == "GET" || methods.front() == "POST") &&
                       request_headers_allowed(head.values("access-control-request-headers"));
  if (!allowed) return make_error_response(ApiError::forbidden);
  return HttpResponse{.status = 204,
                      .headers = {{"Access-Control-Allow-Origin", std::string{origins.front()}},
                                  {"Access-Control-Allow-Methods", "GET, POST"},
                                  {"Access-Control-Allow-Headers", std::string{kPreflightAllowHeaders}},
                                  {"Access-Control-Max-Age", "600"},
                                  {"Vary", "Origin"}},
                      .body = ""};
}
```

Temporary compile shim (removed in Task 6a, which rewrites `src/http_request_handler.cpp`): in `src/http_request_handler.cpp`, replace the call `validate_http_request_headers(policy, parsed.hosts, parsed.origins)` and the following `if (!policy_decision.allowed) { ... }` block and the CORS-preflight block with:

```cpp
  const PolicyVerdict host_verdict = check_host(policy, parsed.hosts);
  if (host_verdict != PolicyVerdict::allowed) return host_verdict == PolicyVerdict::malformed ? bad_request() : forbidden();
  const OriginDecision policy_decision = check_origin(policy, parsed.origins);
  if (policy_decision.verdict != PolicyVerdict::allowed) return forbidden();
```

and change `append_cors_headers` to read `decision.allow_origin`. Delete the four old cases of `http_request_handler_tests` that test the removed policy shapes only if they no longer compile: `fails_closed_before_calling_validator_for_untrusted_origin`, `emits_cors_only_for_policy_approved_origin`, `handles_policy_approved_preflight_without_exposing_an_endpoint` — **do not delete them**; instead change their `HttpPolicy{...}` initializers to `make_http_policy(*parse_listener_address("127.0.0.1"), 8080, {}, {"http://app.example"})` and keep their assertions; Task 6b ports them to the new router. Link `secure_frontend_http_policy` to `secure_frontend_api_core` and `secure_frontend_config` (`target_link_libraries(secure_frontend_http_policy PUBLIC secure_frontend_api_core secure_frontend_config)`).

- [ ] **Step 4: Run to see GREEN**

```bash
cmake --build --preset linux-gcc && ctest --preset linux-gcc -R 'http_policy_tests|http_request_handler_tests' --output-on-failure && ctest --preset linux-gcc | tail -1
cmake --build --preset windows-llvm-mingw-cross && ctest --preset windows-llvm-mingw-cross -R http_policy_tests | tail -1
```

Expected: 5 policy cases pass; handler tests pass with the shim; full suite passes (the policy test count changes from 5 old to 5 new).

- [ ] **Step 5: Handoff row and commit** — row `| 3 | WEB-01, WEB-02, WEB-03, WEB-06 (unit) | Host allowlist (bound, localhost on loopback, aliases; wildcard = aliases only), exact origins, preflight list | http_policy_tests.web01_host_matrix, …web02_host_name_case_is_folded, …web03_origin_matrix, …web06_preflight_rules_unit, …cors_and_alias_grammar | … | … |`.

```bash
git -C "$(git rev-parse --show-toplevel)" add secure_frontend docs/AGENT_HANDOFF.md
git -C "$(git rev-parse --show-toplevel)" -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "http: Host allowlist from the listener, exact origins, fixed preflight list (SP3 WEB-01..03, WEB-06, §4.3-4.4)" \
  -m "Tests: http_policy_tests.web01_host_matrix, web02_host_name_case_is_folded (Review Focus 1), web03_origin_matrix, web06_preflight_rules_unit, cors_and_alias_grammar. RED: <line>. wine: <line>." \
  -m "Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>" -m "Claude-Session: <session link>"
```

---
## Task 4: SP1 addendum A1 — string-array grammar, Host/CORS keys, HTTP deadline keys, `logging.requests`

**Files:**
- Modify (SP1 files): `include/secure_frontend/frontend_config.hpp`, `src/frontend_config.cpp`, `include/secure_frontend/config_defaults.hpp`, `tests/typed_config_test.cpp`, `CMakeLists.txt`, `README.md` (generated key table), `docs/AGENT_HANDOFF.md` (SP1 addendum rows)

**Interfaces:**
- Consumes: SP1 `ConfigKey`, `kSchema`, `DraftReader`, `add()`, `ConfigIssueCode`, `describe()`, `render_config_schema_markdown()`; `is_valid_host_alias`, `is_valid_cors_origin` (Task 3).
- Produces: `struct ConfigKeyExtension { std::string_view path; std::size_t max_entries; bool (*entry_valid)(std::string_view) noexcept; std::string_view entry_reason; std::string_view derived_default; }`, `const ConfigKeyExtension* find_config_key_extension(std::string_view path) noexcept`; `ListenerConfig::cors_origins`, `ListenerConfig::host_aliases` (`std::vector<std::string>`); `HttpLimits::header_deadline`, `write_stall`, `response_write_deadline`, `stream_keepalive` (`std::chrono::milliseconds`); `struct LoggingConfig { bool requests; }` and `FrontendConfig::logging`; `config_defaults::kHeaderDeadlineMs` 10'000, `kWriteStallMs` 10'000, `kResponseWriteDeadlineMs` 60'000, `kStreamKeepaliveMs` 15'000, `kMaxListEntries` 16.

The SP3 extension is a **side table** (`kSchemaExtensions`) keyed by path, so SP1's positional `kSchema` rows stay byte-identical and no aggregate gains members. New rows (schema order: after `listener.backlog`, after `limits.http.max_request_line_bytes`, and a new last section):

| Key | Type | Default | Range | Rule |
| --- | --- | --- | --- | --- |
| `listener.cors_origins` | string array | `[]` | 0-16 entries | each `is_valid_cors_origin`; no duplicates |
| `listener.host_aliases` | string array | `[]` | 0-16 entries | each `is_valid_host_alias`; no duplicates; non-empty when the bind is a wildcard |
| `limits.http.header_deadline_ms` | integer | `10000` | 1-3600000 | `≤ limits.http.request_deadline_ms` |
| `limits.http.write_stall_ms` | integer | `10000` | 100-300000 | — |
| `limits.http.response_write_deadline_ms` | integer | `60000` | 1000-3600000 | `≥ limits.http.write_stall_ms` |
| `limits.http.stream_keepalive_ms` | integer | `15000` | 1000-300000 | — (consumed by M2) |
| `logging.requests` | boolean | `false` | — | — |

- [ ] **Step 1: Write the failing tests** — append to `tests/typed_config_test.cpp` (add each to `run_cases`):

```cpp
struct Sp3Row { const char* path; ConfigValueType type; std::int64_t minimum; std::int64_t maximum; };
constexpr Sp3Row kSp3RowsA1[] = {
    {"listener.cors_origins", ConfigValueType::string_array, 0, 0},
    {"listener.host_aliases", ConfigValueType::string_array, 0, 0},
    {"limits.http.header_deadline_ms", ConfigValueType::integer, 1, 3'600'000},
    {"limits.http.write_stall_ms", ConfigValueType::integer, 100, 300'000},
    {"limits.http.response_write_deadline_ms", ConfigValueType::integer, 1'000, 3'600'000},
    {"limits.http.stream_keepalive_ms", ConfigValueType::integer, 1'000, 300'000},
    {"logging.requests", ConfigValueType::boolean, 0, 0},
};

std::string string_array(const std::vector<std::string>& values) {
  std::string out = "[";
  for (std::size_t i = 0; i < values.size(); ++i) out += (i ? "," : "") + secure_frontend_test::json_literal(values[i]);
  return out + "]";
}

secure_frontend::ConfigValidationResult with_literals(const ConfigFixture& f, const std::map<std::string, std::string>& extra) {
  auto literals = f.literals;
  for (const auto& [k, v] : extra) literals[k] = v;
  return validate_text(render_config_json(literals), f.dir);
}

void cfg01_sp3_rows_type_and_range() {
  ConfigFixture f;
  for (const auto& row : kSp3RowsA1) {
    const auto* key = secure_frontend::find_config_key(row.path);
    expect(key != nullptr && key->type == row.type, std::string{"row exists with its type: "} + row.path);
    if (key != nullptr && row.type == ConfigValueType::integer) {
      expect(key->minimum == row.minimum && key->maximum == row.maximum, std::string{"range of "} + row.path);
    }
  }
  const auto defaults = with_literals(f, {});
  const auto* validated = std::get_if<secure_frontend::ValidatedConfig>(&defaults);
  expect(validated != nullptr, "defaults validate: " + issues_of(defaults));
  if (validated != nullptr) {
    const auto& c = validated->get();
    expect(c.listener.cors_origins.empty() && c.listener.host_aliases.empty(), "array defaults are empty");
    expect(c.limits.http.header_deadline == std::chrono::milliseconds(10000) && c.limits.http.write_stall == std::chrono::milliseconds(10000) &&
           c.limits.http.response_write_deadline == std::chrono::milliseconds(60000) &&
           c.limits.http.stream_keepalive == std::chrono::milliseconds(15000), "HTTP deadline defaults");
    expect(!c.logging.requests, "request logging is off by default");
    const std::string text = secure_frontend::describe(c);
    for (const auto& row : kSp3RowsA1) expect(text.find(row.path) != std::string::npos, std::string{"--check prints "} + row.path);
  }
  std::vector<std::string> seventeen;
  for (int i = 0; i < 17; ++i) seventeen.push_back("h" + std::to_string(i) + ".lan:8080");
  expect(has_issue(with_literals(f, {{"listener.host_aliases", string_array(seventeen)}}), "listener.host_aliases",
                   ConfigIssueCode::out_of_range), "17 aliases are rejected");
  seventeen.pop_back();
  expect(!has_issue(with_literals(f, {{"listener.host_aliases", string_array(seventeen)}}), "listener.host_aliases",
                    ConfigIssueCode::out_of_range), "16 aliases are accepted");
  expect(has_issue(with_literals(f, {{"listener.cors_origins", "[1]"}}), "listener.cors_origins", ConfigIssueCode::wrong_type),
         "a non-string entry is a type error");
}

void cfg01_header_deadline_le_request_deadline() {
  ConfigFixture f;
  expect(has_issue(with_literals(f, {{"limits.http.header_deadline_ms", "30001"}}), "limits.http.header_deadline_ms",
                   ConfigIssueCode::conflicting_values), "header deadline above the request deadline is rejected");
  expect(std::holds_alternative<secure_frontend::ValidatedConfig>(with_literals(f, {{"limits.http.header_deadline_ms", "30000"}})),
         "equal deadlines are valid");
}

void cfg01_response_deadline_ge_write_stall() {
  ConfigFixture f;
  expect(has_issue(with_literals(f, {{"limits.http.write_stall_ms", "20000"}, {"limits.http.response_write_deadline_ms", "19999"}}),
                   "limits.http.response_write_deadline_ms", ConfigIssueCode::conflicting_values),
         "response deadline below the stall bound is rejected");
  expect(std::holds_alternative<secure_frontend::ValidatedConfig>(
             with_literals(f, {{"limits.http.write_stall_ms", "20000"}, {"limits.http.response_write_deadline_ms", "20000"}})),
         "equal values are valid");
}

void cfg01_wildcard_bind_requires_host_aliases() {
  ConfigFixture f;
  const std::map<std::string, std::string> wildcard{{"listener.host", "\"0.0.0.0\""}, {"listener.allow_remote", "true"},
                                                    {"listener.allow_wildcard_bind", "true"}};
  expect(has_issue(with_literals(f, wildcard), "listener.host_aliases", ConfigIssueCode::invalid_value),
         "a wildcard bind without aliases names listener.host_aliases");
  auto with_alias = wildcard;
  with_alias["listener.host_aliases"] = string_array({"box.lan:8080"});
  expect(std::holds_alternative<secure_frontend::ValidatedConfig>(with_literals(f, with_alias)), "one alias is enough");
}

void cfg01_host_alias_grammar_and_count() {
  ConfigFixture f;
  for (const std::string bad : {"http://x", "x:", "x:99999", "a b", "user@x", "x.:80", ""}) {
    expect(has_issue(with_literals(f, {{"listener.host_aliases", string_array({bad})}}), "listener.host_aliases",
                     ConfigIssueCode::invalid_value), "alias rejected: " + bad);
  }
  expect(has_issue(with_literals(f, {{"listener.host_aliases", string_array({"a.lan:1", "a.lan:1"})}}), "listener.host_aliases",
                   ConfigIssueCode::invalid_value), "duplicate aliases are rejected");
  const auto ok = with_literals(f, {{"listener.host_aliases", string_array({"Strata.LAN:8080", "[fe80::1]:8080", "box"})}});
  expect(std::holds_alternative<secure_frontend::ValidatedConfig>(ok), "valid aliases: " + issues_of(ok));
  const auto text = issues_of(with_literals(f, {{"listener.host_aliases", string_array({"secret-host-value:"})}}));
  expect(text.find("secret-host-value") == std::string::npos, "the issue never echoes the value");
}

void web05_cors_origin_config_rejects_wildcard_null_path_duplicate_and_17_entries() {
  ConfigFixture f;
  for (const std::string bad : {"*", "null", "http://app.example/", "http://app.example/path", "http://App.example",
                                "ftp://app.example", "app.example:3000", "http://app.example:3000?x"}) {
    expect(has_issue(with_literals(f, {{"listener.cors_origins", string_array({bad})}}), "listener.cors_origins",
                     ConfigIssueCode::invalid_value), "origin rejected: " + bad);
  }
  expect(has_issue(with_literals(f, {{"listener.cors_origins", string_array({"http://a.example", "http://a.example"})}}),
                   "listener.cors_origins", ConfigIssueCode::invalid_value), "duplicate origins are rejected");
  std::vector<std::string> seventeen;
  for (int i = 0; i < 17; ++i) seventeen.push_back("http://a" + std::to_string(i) + ".example");
  expect(has_issue(with_literals(f, {{"listener.cors_origins", string_array(seventeen)}}), "listener.cors_origins",
                   ConfigIssueCode::out_of_range), "17 origins are rejected");
  const auto ok = with_literals(f, {{"listener.cors_origins", string_array({"http://app.example:3000", "https://ui.example"})}});
  expect(std::holds_alternative<secure_frontend::ValidatedConfig>(ok), "two exact origins: " + issues_of(ok));
}
```

In `CMakeLists.txt` add to the `secure_frontend_add_cases(secure_frontend_typed_config_tests` list: `cfg01_sp3_rows_type_and_range cfg01_header_deadline_le_request_deadline cfg01_response_deadline_ge_write_stall cfg01_wildcard_bind_requires_host_aliases cfg01_host_alias_grammar_and_count web05_cors_origin_config_rejects_wildcard_null_path_duplicate_and_17_entries`.

- [ ] **Step 2: Run to see RED**

Run: `cmake --build --preset linux-gcc 2>&1 | grep -m1 error`
Expected: FAIL `'struct secure_frontend::ListenerConfig' has no member named 'cors_origins'`.

- [ ] **Step 3: Implement**

`include/secure_frontend/config_defaults.hpp` — add after `kMaxRequestLineBytes`:

```cpp
// SP3 addendum (spec §5.1).
inline constexpr std::int64_t kHeaderDeadlineMs = 10'000;
inline constexpr std::int64_t kWriteStallMs = 10'000;
inline constexpr std::int64_t kResponseWriteDeadlineMs = 60'000;
inline constexpr std::int64_t kStreamKeepaliveMs = 15'000;
inline constexpr std::int64_t kMaxListEntries = 16;
```

`include/secure_frontend/frontend_config.hpp`:
- `ListenerConfig`: add after `backlog`: `std::vector<std::string> cors_origins; std::vector<std::string> host_aliases;` (include `<vector>`, `<string>`).
- `HttpLimits`: add after `max_request_line_bytes`: `std::chrono::milliseconds header_deadline; std::chrono::milliseconds write_stall; std::chrono::milliseconds response_write_deadline; std::chrono::milliseconds stream_keepalive;`.
- After `struct Limits`: `struct LoggingConfig final { bool requests; };`
- `FrontendConfig`: add the public member `const LoggingConfig logging;` after `engine`, and a sixth constructor parameter `LoggingConfig logging_value` (SP1 defines that private constructor **inline in `include/secure_frontend/frontend_config.hpp`** (anchor `FrontendConfig(ListenerConfig listener_value, Limits limits_value, Roots roots_value, SecretSource token_value,`): add the parameter there and `, logging(logging_value)` to its member-initializer list).
- After `find_config_key`:

```cpp
// SP3 addendum: per-key extras kept beside SP1's positional rows.
struct ConfigKeyExtension final {
  std::string_view path;
  std::size_t max_entries;                          // string_array: most entries (0 = unbounded)
  bool (*entry_valid)(std::string_view) noexcept;   // string_array: per-entry grammar (nullptr = any)
  std::string_view entry_reason;                    // fixed reason when an entry fails entry_valid
  std::string_view derived_default;                 // non-empty: default derived from other keys (README / --check text)
};
[[nodiscard]] const ConfigKeyExtension* find_config_key_extension(std::string_view path) noexcept;
```

`src/frontend_config.cpp`:
- Add `#include "secure_frontend/http_policy.hpp"` and `#include <set>`.
- In `kSchema` (bump the array size from its current count by 7), insert after the `listener.backlog` row:

```cpp
    {"listener.cors_origins", T::string_array, false, 0, 0, 0, false, "", "",
     "Exact browser origins allowed CORS: http:// or https://, a lower-case host, an optional port, no path; never * or null; at most 16."},
    {"listener.host_aliases", T::string_array, false, 0, 0, 0, false, "", "",
     "Extra exact Host values (host or host:port) clients use; required when the bind is a wildcard; at most 16."},
```

after the `limits.http.max_request_line_bytes` row:

```cpp
    {"limits.http.header_deadline_ms", T::integer, false, 1, 3'600'000, d::kHeaderDeadlineMs, false, "", "",
     "Time from accept to the end of the request header; at most limits.http.request_deadline_ms."},
    {"limits.http.write_stall_ms", T::integer, false, 100, 300'000, d::kWriteStallMs, false, "", "",
     "Longest send without progress before the client is treated as gone."},
    {"limits.http.response_write_deadline_ms", T::integer, false, 1'000, 3'600'000, d::kResponseWriteDeadlineMs, false, "", "",
     "Time to send a whole complete response, or a stream's tail after DONE; at least limits.http.write_stall_ms."},
    {"limits.http.stream_keepalive_ms", T::integer, false, 1'000, 300'000, d::kStreamKeepaliveMs, false, "", "",
     "SSE comment interval while no content flows (streaming)."},
```

and as the last row:

```cpp
    {"logging.requests", T::boolean, false, 0, 0, 0, false, "", "",
     "One line per request to stderr: route, status, code, time, bytes, loopback or remote; never paths, headers, tokens or text."},
```

- After `find_config_key`, add:

```cpp
namespace {
constexpr std::array<ConfigKeyExtension, 2> kSchemaExtensions{{
    {"listener.cors_origins", static_cast<std::size_t>(config_defaults::kMaxListEntries), &is_valid_cors_origin,
     "each entry must be http:// or https://, a lower-case host, an optional port and no path; * and null are not allowed", ""},
    {"listener.host_aliases", static_cast<std::size_t>(config_defaults::kMaxListEntries), &is_valid_host_alias,
     "each entry must be a host or host:port", ""},
}};

// Reads a string-array row and applies its SP3 bounds; any failure records one
// fixed issue naming the key and yields an empty list.
std::vector<std::string> read_bounded_strings(DraftReader& read, ConfigErrors& errors, const std::string_view path) {
  std::vector<std::string> values = read.strings(path);
  const ConfigKeyExtension* extension = find_config_key_extension(path);
  if (extension == nullptr) return values;
  if (extension->max_entries != 0U && values.size() > extension->max_entries) {
    add(errors, std::string{path}, ConfigIssueCode::out_of_range,
        "must have at most " + std::to_string(extension->max_entries) + " entries");
    return {};
  }
  std::set<std::string> seen;
  for (const auto& value : values) {
    if (extension->entry_valid != nullptr && !extension->entry_valid(value)) {
      add(errors, std::string{path}, ConfigIssueCode::invalid_value, std::string{extension->entry_reason});
      return {};
    }
    if (!seen.insert(value).second) {
      add(errors, std::string{path}, ConfigIssueCode::invalid_value, "must not repeat an entry");
      return {};
    }
  }
  return values;
}
}  // namespace

const ConfigKeyExtension* find_config_key_extension(const std::string_view path) noexcept {
  for (const auto& extension : kSchemaExtensions) if (extension.path == path) return &extension;
  return nullptr;
}
```

(`DraftReader` and `add` are defined earlier in the same anonymous namespace; if `DraftReader` is declared after `find_config_key`, place this block directly after the `DraftReader` class instead.)

- In `validate()`, next to the other listener reads (anchor `const auto host = parse_listener_address(host_text);`), add:

```cpp
  std::vector<std::string> cors_origins = read_bounded_strings(read, errors, "listener.cors_origins");
  std::vector<std::string> host_aliases = read_bounded_strings(read, errors, "listener.host_aliases");
  if (host.has_value() && host->scope() == AddressScope::wildcard && host_aliases.empty() &&
      !has_issue_for(errors, "listener.host_aliases")) {
    add(errors, "listener.host_aliases", ConfigIssueCode::invalid_value,
        "must name at least one Host value when the bind is a wildcard");
  }
```

with the helper (anonymous namespace) `bool has_issue_for(const ConfigErrors& errors, std::string_view path) { return std::any_of(errors.begin(), errors.end(), [path](const ConfigIssue& i) { return i.key_path == path; }); }`.

Next to the HTTP reads (anchor `read.integer("limits.http.request_deadline_ms")`), add:

```cpp
  const auto header_deadline_ms = read.integer("limits.http.header_deadline_ms");
  const auto write_stall_ms = read.integer("limits.http.write_stall_ms");
  const auto response_deadline_ms = read.integer("limits.http.response_write_deadline_ms");
  const auto keepalive_ms = read.integer("limits.http.stream_keepalive_ms");
  const bool log_requests = read.boolean("logging.requests");
```

and with the other cross-field rules (anchor `if (default_max > max_completion) {`):

```cpp
  if (header_deadline_ms > deadline_ms) {
    add(errors, "limits.http.header_deadline_ms", ConfigIssueCode::conflicting_values,
        "must not exceed limits.http.request_deadline_ms");
  }
  if (response_deadline_ms < write_stall_ms) {
    add(errors, "limits.http.response_write_deadline_ms", ConfigIssueCode::conflicting_values,
        "must be at least limits.http.write_stall_ms");
  }
```

(`deadline_ms` is the local SP1 reads `limits.http.request_deadline_ms` into; if SP1 named it differently, use that name.) Construct the new members: the `ListenerConfig listener{...}` initializer gains `, std::move(cors_origins), std::move(host_aliases)`; the `HttpLimits{...}` initializer gains `, std::chrono::milliseconds(header_deadline_ms), std::chrono::milliseconds(write_stall_ms), std::chrono::milliseconds(response_deadline_ms), std::chrono::milliseconds(keepalive_ms)`; `new FrontendConfig(...)` gains `, LoggingConfig{log_requests}`.

- In `describe()` (anchor `line("listener.backlog", std::to_string(c.listener.backlog));`), add the lines in schema order:

```cpp
  const auto list_text = [](const std::vector<std::string>& values) {
    std::string text = "[";
    for (std::size_t i = 0; i < values.size(); ++i) text += (i ? "," : "") + json_quoted(values[i]);
    return text + "]";
  };
  line("listener.cors_origins", list_text(c.listener.cors_origins));
  line("listener.host_aliases", list_text(c.listener.host_aliases));
```

after `line("limits.http.max_request_line_bytes", ...)`:

```cpp
  line("limits.http.header_deadline_ms", std::to_string(c.limits.http.header_deadline.count()));
  line("limits.http.write_stall_ms", std::to_string(c.limits.http.write_stall.count()));
  line("limits.http.response_write_deadline_ms", std::to_string(c.limits.http.response_write_deadline.count()));
  line("limits.http.stream_keepalive_ms", std::to_string(c.limits.http.stream_keepalive.count()));
```

and last: `line("logging.requests", c.logging.requests ? "true" : "false");`. (Values of these keys are the operator's own settings, which `--check` already prints for every key; the fixed-reason rule concerns errors and logs, not `--check`'s effective-value listing.)

- Link: `target_link_libraries(secure_frontend_typed_config PUBLIC secure_frontend_http_policy)`.
- Every other construction of `FrontendConfig`/`ListenerConfig`/`HttpLimits` outside `validate()` (search `grep -rn "HttpLimits{\|ListenerConfig{" src tests`) gets the new members with the defaults above.

- [ ] **Step 4: README table and GREEN**

```bash
cmake --build --preset linux-gcc && ctest --preset linux-gcc -R typed_config_tests --output-on-failure
ctest --preset linux-gcc -R readme_config_table --output-on-failure | sed -n '/config-keys:begin/,/config-keys:end/p'
```

The README test fails and prints the generated table; replace the block between `<!-- config-keys:begin -->` and `<!-- config-keys:end -->` in `README.md` with exactly that printed table. Re-run: `ctest --preset linux-gcc | tail -1` → `100% tests passed`; `cmake --build --preset windows-llvm-mingw-cross && ctest --preset windows-llvm-mingw-cross -R typed_config | tail -1` (these cases carry `windows-runtime` in SP1's registration, so wine reports them excluded; record that).

- [ ] **Step 5: Handoff (row + SP1 addendum A1) and commit** — handoff row `| 4 | WEB-05 (config), CFG-01 (SP3 rows) | listener.cors_origins/host_aliases, HTTP deadline keys, logging.requests; string-array bound + grammar side table | typed_config_tests.cfg01_sp3_rows_type_and_range, …cfg01_header_deadline_le_request_deadline, …cfg01_response_deadline_ge_write_stall, …cfg01_wildcard_bind_requires_host_aliases, …cfg01_host_alias_grammar_and_count, …web05_cors_origin_config_rejects_wildcard_null_path_duplicate_and_17_entries | … | … |`; SP1 addendum row `| A1 | Rows listener.cors_origins, listener.host_aliases, limits.http.{header_deadline_ms, write_stall_ms, response_write_deadline_ms, stream_keepalive_ms}, logging.requests; string-array entry bound and per-entry grammar (ConfigKeyExtension side table); cross-field header ≤ request, response ≥ stall, wildcard ⇒ aliases | 4 |`.

```bash
git -C "$(git rev-parse --show-toplevel)" add secure_frontend docs/AGENT_HANDOFF.md
git -C "$(git rev-parse --show-toplevel)" -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "config: SP1 addendum A1 - Host aliases, CORS origins, HTTP deadlines, request logging key (SP3 §5.1, WEB-05)" \
  -m "Tests: typed_config_tests.cfg01_sp3_rows_type_and_range, cfg01_header_deadline_le_request_deadline, cfg01_response_deadline_ge_write_stall, cfg01_wildcard_bind_requires_host_aliases, cfg01_host_alias_grammar_and_count, web05_cors_origin_config_rejects_wildcard_null_path_duplicate_and_17_entries. README key table regenerated. RED: <line>. wine: <line>." \
  -m "Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>" -m "Claude-Session: <session link>"
```

---

## Task 5: SP1 addendum A2 — concurrency rows, derived defaults, `default_max_tokens` auto, `resolve_max_tokens`

**Files:**
- Modify (SP1 files): `include/secure_frontend/frontend_config.hpp`, `src/frontend_config.cpp`, `include/secure_frontend/config_defaults.hpp`, `tests/typed_config_test.cpp`, `README.md`
- Modify: `include/secure_frontend/chat_execution.hpp`, `src/chat_execution.cpp`, `src/chat_generation_plan.cpp`, `tests/chat_execution_test.cpp`, `CMakeLists.txt`

**Interfaces:**
- Consumes: Task 4's `ConfigKeyExtension` side table.
- Produces: `ConcurrencyLimits{ std::uint32_t max_concurrent_requests; std::uint32_t max_queued_generations; std::chrono::milliseconds queue_timeout; std::uint32_t max_connections_per_remote_address; bool queue_derived; bool remote_cap_derived; }`; `config_defaults::kMaxConcurrentRequests` = 8, `kQueueTimeoutMs` = 300'000, `kChatDefaultMaxTokens` = 0, `kDerivedQueueCap` = 4, `kDerivedRemoteCap` = 4; `std::uint32_t derived_max_queued_generations(std::uint32_t workers) noexcept`, `std::uint32_t derived_max_connections_per_remote_address(std::uint32_t workers) noexcept` (in `frontend_config.hpp`); `std::optional<std::size_t> resolve_max_tokens(std::optional<std::size_t> requested, std::size_t prompt_tokens, std::size_t context_tokens, std::size_t max_completion_tokens, std::size_t default_max_tokens) noexcept` in `chat_execution.hpp` (nullopt = `context_length_exceeded`; `context_tokens == 0` = unknown, no context clamp; `default_max_tokens == 0` = auto).

Rows: `limits.concurrency.max_concurrent_requests` range `1-256`, default `8`; `limits.concurrency.max_queued_generations` `0-254`, derived `min(4, max(0, workers − 2))`; `limits.concurrency.queue_timeout_ms` `1000-3600000`, default `300000`; `limits.concurrency.max_connections_per_remote_address` `1-256`, derived `min(4, workers)`; `limits.chat.default_max_tokens` range `0-16384`, default `0` (auto). Cross-field: workers = 1 ⇒ queue = 0; workers ≥ 2 ⇒ queue + 2 ≤ workers; remote cap ≤ workers. A derived value is used only when the key is absent; an explicit value is validated as written. SP1's `init` writes only the operator's flags plus `schema_version` (verify: `grep -n "written = flags" src/frontend_init.cpp`), so the two derived keys are never written and stay derived.

- [ ] **Step 1: Write the failing tests** — append to `tests/typed_config_test.cpp`:

```cpp
constexpr Sp3Row kSp3RowsA2[] = {
    {"limits.concurrency.max_concurrent_requests", ConfigValueType::integer, 1, 256},
    {"limits.concurrency.max_queued_generations", ConfigValueType::integer, 0, 254},
    {"limits.concurrency.queue_timeout_ms", ConfigValueType::integer, 1'000, 3'600'000},
    {"limits.concurrency.max_connections_per_remote_address", ConfigValueType::integer, 1, 256},
    {"limits.chat.default_max_tokens", ConfigValueType::integer, 0, 16'384},
};

const secure_frontend::FrontendConfig* config_of(const secure_frontend::ConfigValidationResult& result) {
  const auto* validated = std::get_if<secure_frontend::ValidatedConfig>(&result);
  return validated == nullptr ? nullptr : &validated->get();
}

void cfg01_sp3_concurrency_rows_and_defaults() {
  ConfigFixture f;
  for (const auto& row : kSp3RowsA2) {
    const auto* key = secure_frontend::find_config_key(row.path);
    expect(key != nullptr && key->type == row.type && key->minimum == row.minimum && key->maximum == row.maximum,
           std::string{"row and range: "} + row.path);
  }
  const auto result = with_literals(f, {});
  const auto* c = config_of(result);
  expect(c != nullptr, "defaults validate: " + issues_of(result));
  if (c == nullptr) return;
  expect(c->limits.concurrency.max_concurrent_requests == 8U && c->limits.concurrency.max_queued_generations == 4U &&
         c->limits.concurrency.queue_timeout == std::chrono::milliseconds(300000) &&
         c->limits.concurrency.max_connections_per_remote_address == 4U, "spec §5.1 defaults (B11)");
  expect(c->limits.concurrency.queue_derived && c->limits.concurrency.remote_cap_derived, "absent keys are derived");
  expect(c->limits.chat.default_max_tokens == 0U, "default_max_tokens defaults to 0 = auto (B1)");
  const std::string text = secure_frontend::describe(*c);
  expect(text.find("limits.concurrency.max_queued_generations: 4 (derived)") != std::string::npos &&
         text.find("limits.concurrency.max_connections_per_remote_address: 4 (derived)") != std::string::npos,
         "--check marks derived values: " + text);
}

void cfg01_queue_plus_two_le_workers() {
  ConfigFixture f;
  expect(has_issue(with_literals(f, {{"limits.concurrency.max_concurrent_requests", "5"}, {"limits.concurrency.max_queued_generations", "4"}}),
                   "limits.concurrency.max_queued_generations", ConfigIssueCode::conflicting_values), "4 + 2 > 5 is rejected");
  expect(config_of(with_literals(f, {{"limits.concurrency.max_concurrent_requests", "6"}, {"limits.concurrency.max_queued_generations", "4"}})) != nullptr,
         "4 + 2 = 6 is valid");
}

void cfg01_serial_requires_zero_queue() {
  ConfigFixture f;
  expect(has_issue(with_literals(f, {{"limits.concurrency.max_concurrent_requests", "1"}, {"limits.concurrency.max_queued_generations", "1"}}),
                   "limits.concurrency.max_queued_generations", ConfigIssueCode::conflicting_values), "serial needs queue 0");
  expect(config_of(with_literals(f, {{"limits.concurrency.max_concurrent_requests", "1"}, {"limits.concurrency.max_queued_generations", "0"}})) != nullptr,
         "serial with queue 0 is valid");
}

void cfg01_remote_cap_le_workers() {
  ConfigFixture f;
  expect(has_issue(with_literals(f, {{"limits.concurrency.max_concurrent_requests", "3"},
                                     {"limits.concurrency.max_connections_per_remote_address", "4"}}),
                   "limits.concurrency.max_connections_per_remote_address", ConfigIssueCode::conflicting_values), "cap above workers");
}

void cfg01_sp1_config_with_one_worker_still_valid() {
  ConfigFixture f;
  const auto* c = config_of(with_literals(f, {{"limits.concurrency.max_concurrent_requests", "1"}}));
  expect(c != nullptr, "an SP1-era file with one worker validates");
  if (c != nullptr) {
    expect(c->limits.concurrency.max_queued_generations == 0U && c->limits.concurrency.max_connections_per_remote_address == 1U,
           "queue 0 and remote cap 1 are derived");
  }
}

void cfg01_derived_defaults_follow_worker_count() {
  ConfigFixture f;
  const std::pair<int, std::pair<unsigned, unsigned>> table[] = {{1, {0U, 1U}}, {2, {0U, 2U}}, {3, {1U, 3U}},
                                                                  {6, {4U, 4U}}, {8, {4U, 4U}}, {256, {4U, 4U}}};
  for (const auto& [workers, expected] : table) {
    const auto* c = config_of(with_literals(f, {{"limits.concurrency.max_concurrent_requests", std::to_string(workers)}}));
    expect(c != nullptr && c->limits.concurrency.max_queued_generations == expected.first &&
           c->limits.concurrency.max_connections_per_remote_address == expected.second,
           "derived for " + std::to_string(workers) + " workers");
    expect(secure_frontend::derived_max_queued_generations(static_cast<std::uint32_t>(workers)) == expected.first &&
           secure_frontend::derived_max_connections_per_remote_address(static_cast<std::uint32_t>(workers)) == expected.second,
           "derivation functions for " + std::to_string(workers));
  }
}

void cfg01_explicit_values_are_not_adjusted() {
  ConfigFixture f;
  const auto result = with_literals(f, {{"limits.concurrency.max_concurrent_requests", "1"}, {"limits.concurrency.max_queued_generations", "4"}});
  expect(config_of(result) == nullptr && has_issue(result, "limits.concurrency.max_queued_generations", ConfigIssueCode::conflicting_values),
         "an explicit queue of 4 with one worker is an error naming the key, never silently lowered");
  const auto* c = config_of(with_literals(f, {{"limits.concurrency.max_queued_generations", "2"}}));
  expect(c != nullptr && c->limits.concurrency.max_queued_generations == 2U && !c->limits.concurrency.queue_derived,
         "an explicit valid value is kept as written");
}
```

In `typed_config_accepts_minimal_file_and_applies_defaults` change the two SP1 expectations (spec §9.4 "SP1 config tests for max_concurrent_requests"): `c.limits.chat.default_max_tokens == 256U` → `== 0U`, and `c.limits.concurrency.max_concurrent_requests == 1U, "one request at a time until SP3"` → `== 8U, "eight workers (SP3 B11)"`.

Append to `tests/chat_execution_test.cpp` and register (`secure_frontend_add_cases(secure_frontend_chat_execution_tests` list):

```cpp
void resolve_max_tokens_follows_decisions_1_and_2() {
  using secure_frontend::resolve_max_tokens;
  // requested, prompt, ctx, max_completion, default -> result
  expect(resolve_max_tokens(100U, 10U, 4096U, 4096U, 0U) == std::optional<std::size_t>{100U}, "fits as asked");
  expect(resolve_max_tokens(16384U, 10U, 100000U, 4096U, 0U) == std::optional<std::size_t>{4096U}, "clamped to max_completion (B2)");
  expect(resolve_max_tokens(1000U, 50U, 100U, 4096U, 0U) == std::optional<std::size_t>{42U}, "clamped to ctx - n - 8 (B2)");
  expect(resolve_max_tokens(std::nullopt, 50U, 100U, 4096U, 0U) == std::optional<std::size_t>{42U}, "auto = min(max, ctx - n - 8) (B1)");
  expect(resolve_max_tokens(std::nullopt, 10U, 100000U, 4096U, 0U) == std::optional<std::size_t>{4096U}, "auto capped by max_completion");
  expect(resolve_max_tokens(std::nullopt, 10U, 4096U, 4096U, 256U) == std::optional<std::size_t>{256U}, "a configured default applies");
  expect(!resolve_max_tokens(5U, 92U, 100U, 4096U, 0U).has_value(), "prompt alone fills the context: 400 (B2)");
  expect(resolve_max_tokens(5U, 91U, 100U, 4096U, 0U) == std::optional<std::size_t>{1U}, "one token of room is still allowed");
  expect(resolve_max_tokens(5U, 100000U, 0U, 4096U, 0U) == std::optional<std::size_t>{5U}, "unknown context: no context clamp");
}
```

- [ ] **Step 2: Run to see RED**

Run: `cmake --build --preset linux-gcc 2>&1 | grep -m1 error`
Expected: FAIL `'struct secure_frontend::ConcurrencyLimits' has no member named 'max_queued_generations'`.

- [ ] **Step 3: Implement**

`config_defaults.hpp`: change `kMaxConcurrentRequests = 1` to `= 8`, `kChatDefaultMaxTokens = 256` to `= 0`, and add:

```cpp
inline constexpr std::int64_t kQueueTimeoutMs = 300'000;
inline constexpr std::int64_t kDerivedQueueCap = 4;   // max_queued_generations = min(4, max(0, workers - 2))
inline constexpr std::int64_t kDerivedRemoteCap = 4;  // max_connections_per_remote_address = min(4, workers)
```

`frontend_config.hpp`: replace `struct ConcurrencyLimits` with

```cpp
struct ConcurrencyLimits final {
  std::uint32_t max_concurrent_requests;
  std::uint32_t max_queued_generations;
  std::chrono::milliseconds queue_timeout;
  std::uint32_t max_connections_per_remote_address;
  bool queue_derived;
  bool remote_cap_derived;
};
[[nodiscard]] std::uint32_t derived_max_queued_generations(std::uint32_t workers) noexcept;
[[nodiscard]] std::uint32_t derived_max_connections_per_remote_address(std::uint32_t workers) noexcept;
```

`frontend_config.cpp`:
- `kSchema`: change the `max_concurrent_requests` row to
  `{"limits.concurrency.max_concurrent_requests", T::integer, false, 1, 256, d::kMaxConcurrentRequests, false, "", "", "Worker threads: connections read or answered at once; 1 = serial."},`
  and the `default_max_tokens` row's minimum `1` to `0` and its description to `"max_tokens when a request omits it; 0 = auto: min(max_completion_tokens, context - prompt - 8); at most limits.chat.max_completion_tokens."`; after `max_concurrent_requests` add (bump the array size by 3):

```cpp
    {"limits.concurrency.max_queued_generations", T::integer, false, 0, 254, 0, false, "", "",
     "Chats waiting behind the running generation; 0 when max_concurrent_requests is 1, otherwise at most max_concurrent_requests - 2."},
    {"limits.concurrency.queue_timeout_ms", T::integer, false, 1'000, 3'600'000, d::kQueueTimeoutMs, false, "", "",
     "Longest wait for the engine before 503 server_busy."},
    {"limits.concurrency.max_connections_per_remote_address", T::integer, false, 1, 256, 0, false, "", "",
     "Workers one non-loopback address may hold; at most max_concurrent_requests. Loopback is exempt."},
```

- `kSchemaExtensions` (size 2 → 4) gains
  `{"limits.concurrency.max_queued_generations", 0U, nullptr, "", "derived: min(4, max(0, workers-2))"},`
  `{"limits.concurrency.max_connections_per_remote_address", 0U, nullptr, "", "derived: min(4, workers)"},`
  and `render_config_schema_markdown()` prints `ext->derived_default` in the Default column when it is non-empty (anchor: the place that formats `key.default_integer` for integer rows: `if (const auto* ext = find_config_key_extension(key.path); ext != nullptr && !ext->derived_default.empty()) default_text = "`" + std::string{ext->derived_default} + "`";`).
- Definitions:

```cpp
std::uint32_t derived_max_queued_generations(const std::uint32_t workers) noexcept {
  const std::uint32_t spare = workers > 2U ? workers - 2U : 0U;
  return std::min<std::uint32_t>(static_cast<std::uint32_t>(config_defaults::kDerivedQueueCap), spare);
}
std::uint32_t derived_max_connections_per_remote_address(const std::uint32_t workers) noexcept {
  return std::min<std::uint32_t>(static_cast<std::uint32_t>(config_defaults::kDerivedRemoteCap), workers);
}
```

- In `validate()` replace `const auto concurrent = read.integer("limits.concurrency.max_concurrent_requests");` with

```cpp
  const auto concurrent = read.integer("limits.concurrency.max_concurrent_requests");
  const auto workers = static_cast<std::uint32_t>(concurrent);
  const bool queue_set = read.present("limits.concurrency.max_queued_generations");
  const auto queued = queue_set ? read.integer("limits.concurrency.max_queued_generations")
                                : static_cast<std::int64_t>(derived_max_queued_generations(workers));
  const auto queue_timeout_ms = read.integer("limits.concurrency.queue_timeout_ms");
  const bool cap_set = read.present("limits.concurrency.max_connections_per_remote_address");
  const auto remote_cap = cap_set ? read.integer("limits.concurrency.max_connections_per_remote_address")
                                  : static_cast<std::int64_t>(derived_max_connections_per_remote_address(workers));
```

  and with the cross-field rules:

```cpp
  if (concurrent == 1 && queued != 0) {
    add(errors, "limits.concurrency.max_queued_generations", ConfigIssueCode::conflicting_values,
        "must be 0 when limits.concurrency.max_concurrent_requests is 1");
  } else if (concurrent >= 2 && queued + 2 > concurrent) {
    add(errors, "limits.concurrency.max_queued_generations", ConfigIssueCode::conflicting_values,
        "plus 2 must not exceed limits.concurrency.max_concurrent_requests");
  }
  if (remote_cap > concurrent) {
    add(errors, "limits.concurrency.max_connections_per_remote_address", ConfigIssueCode::conflicting_values,
        "must not exceed limits.concurrency.max_concurrent_requests");
  }
```

  and construct `ConcurrencyLimits{u32(concurrent), u32(queued), std::chrono::milliseconds(queue_timeout_ms), u32(remote_cap), !queue_set, !cap_set}`.
- In `describe()` replace the `max_concurrent_requests` line with:

```cpp
  const auto& cc = c.limits.concurrency;
  line("limits.concurrency.max_concurrent_requests", std::to_string(cc.max_concurrent_requests));
  line("limits.concurrency.max_queued_generations",
       std::to_string(cc.max_queued_generations) + (cc.queue_derived ? " (derived)" : ""));
  line("limits.concurrency.queue_timeout_ms", std::to_string(cc.queue_timeout.count()));
  line("limits.concurrency.max_connections_per_remote_address",
       std::to_string(cc.max_connections_per_remote_address) + (cc.remote_cap_derived ? " (derived)" : ""));
```

  (If SP1's `line()` prints `key: value`, the test strings above match; if it prints `key = value`, change the two test strings to that separator.)

`chat_execution.hpp` — add before `class ChatExecutionService`:

```cpp
// Spec §5.4, owner decisions B1/B2. Returns the max_new for GEN, or nullopt
// when the prompt alone leaves no room (400 context_length_exceeded).
// context_tokens 0 = not known yet (no context clamp); default_max_tokens 0 =
// auto (min(max_completion_tokens, context - prompt - 8)).
[[nodiscard]] std::optional<std::size_t> resolve_max_tokens(std::optional<std::size_t> requested, std::size_t prompt_tokens,
                                                            std::size_t context_tokens, std::size_t max_completion_tokens,
                                                            std::size_t default_max_tokens) noexcept;
```

`chat_execution.cpp`:

```cpp
std::optional<std::size_t> resolve_max_tokens(const std::optional<std::size_t> requested, const std::size_t prompt_tokens,
                                              const std::size_t context_tokens, const std::size_t max_completion_tokens,
                                              const std::size_t default_max_tokens) noexcept {
  constexpr std::size_t kMargin = 8U;   // the engine's rule: n + max_new + 8 <= ctx
  std::size_t room = max_completion_tokens;
  if (context_tokens != 0U) {
    if (prompt_tokens + kMargin >= context_tokens) return std::nullopt;
    room = context_tokens - prompt_tokens - kMargin;
  }
  const std::size_t base = requested.has_value() ? *requested
                           : (default_max_tokens == 0U ? max_completion_tokens : default_max_tokens);
  const std::size_t resolved = std::min({base, max_completion_tokens, room});
  if (resolved == 0U) return std::nullopt;
  return resolved;
}
```

In `ChatExecutionService::execute` replace the output-bound check (anchor `if (plan->max_tokens == 0U || plan->max_tokens > limits_.maximum_output_tokens ||`) and the context check (anchor `plan->prompt_token_ids.size() + plan->max_tokens + 8U > engine_context_tokens_`) with one resolution after READY is known:

```cpp
  const auto budget = resolve_max_tokens(plan->max_tokens == 0U ? std::nullopt : std::optional<std::size_t>{plan->max_tokens},
                                         plan->prompt_token_ids.size(), engine_context_tokens_,
                                         limits_.maximum_output_tokens, 0U);
  if (!protocol_.ready() || engine_context_tokens_ == 0U || !budget.has_value()) {
    return error(ChatExecutionErrorCode::context_limit_exceeded, "request exceeds the local engine context limit");
  }
```

and pass `static_cast<std::uint32_t>(*budget)` to `begin_generation` and compare `generation.token_ids.size() > *budget`. In `src/chat_generation_plan.cpp` keep rejecting an explicit `max_tokens == 0` request but let a zero `default_max_tokens` through (it means auto): no change is needed if the plan only rejects `request.max_tokens.has_value() && *request.max_tokens == 0U` (verify by reading `make_qwen_chat_generation_plan`; if it also rejects `default_max_tokens == 0`, drop that clause in both plan functions).

Update `rejects_context_and_output_limits_before_generation` in `tests/chat_execution_test.cpp` (spec §9.4: `max_tokens` clamping is a deliberate behavior change): give the second transport `limit_transport.reads = {line("READY 512"), line("DONE 0")};` and replace its expectation with

```cpp
  expect(std::holds_alternative<secure_frontend::ChatExecutionResult>(limit_result) &&
             limit_transport.writes.size() == 1U && limit_transport.writes.front().rfind("GEN 1 ", 0) == 0,
         "a request above the local output bound is clamped to it (B2), not rejected");
```

Register `cfg01_sp3_concurrency_rows_and_defaults cfg01_queue_plus_two_le_workers cfg01_serial_requires_zero_queue cfg01_remote_cap_le_workers cfg01_sp1_config_with_one_worker_still_valid cfg01_derived_defaults_follow_worker_count cfg01_explicit_values_are_not_adjusted` in `secure_frontend_typed_config_tests` and `resolve_max_tokens_follows_decisions_1_and_2` in `secure_frontend_chat_execution_tests`.

- [ ] **Step 4: README and GREEN** — regenerate the README block exactly as in Task 4 Step 4, then:

```bash
cmake --build --preset linux-gcc && ctest --preset linux-gcc -R 'typed_config_tests|chat_execution_tests|readme' --output-on-failure
ctest --preset linux-gcc | tail -1
cmake --build --preset windows-llvm-mingw-cross && ctest --preset windows-llvm-mingw-cross | tail -1
```

Expected: all pass. Any other failing test that asserted `max_concurrent_requests == 1`, `default_max_tokens == 256`, or a rejected over-limit `max_tokens` is updated to the new value in this task (spec §9.4); name each in the commit body.

- [ ] **Step 5: Handoff (row + SP1 addendum A2) and commit** — row `| 5 | CFG-01 (SP3 rows), B1, B2, B11 | concurrency rows with derived defaults, default_max_tokens 0 = auto, resolve_max_tokens | typed_config_tests.cfg01_sp3_concurrency_rows_and_defaults, …cfg01_queue_plus_two_le_workers, …cfg01_serial_requires_zero_queue, …cfg01_remote_cap_le_workers, …cfg01_sp1_config_with_one_worker_still_valid, …cfg01_derived_defaults_follow_worker_count, …cfg01_explicit_values_are_not_adjusted; chat_execution_tests.resolve_max_tokens_follows_decisions_1_and_2 | … | … |`; SP1 addendum row `| A2 | max_concurrent_requests 1-256 default 8; max_queued_generations, queue_timeout_ms, max_connections_per_remote_address (derived defaults, `--check` marks "(derived)", init omits them); default_max_tokens 0-16384 default 0 = auto | 5 |`.

```bash
git -C "$(git rev-parse --show-toplevel)" add secure_frontend docs/AGENT_HANDOFF.md
git -C "$(git rev-parse --show-toplevel)" -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "config: SP1 addendum A2 - worker pool and queue keys with derived defaults; max_tokens auto and clamp (SP3 §5.1, B1, B2, B11)" \
  -m "Tests: typed_config_tests.cfg01_sp3_concurrency_rows_and_defaults, cfg01_queue_plus_two_le_workers, cfg01_serial_requires_zero_queue, cfg01_remote_cap_le_workers, cfg01_sp1_config_with_one_worker_still_valid, cfg01_derived_defaults_follow_worker_count, cfg01_explicit_values_are_not_adjusted; chat_execution_tests.resolve_max_tokens_follows_decisions_1_and_2. Updated per spec §9.4: <names>. RED: <line>. wine: <line>." \
  -m "Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>" -m "Claude-Session: <session link>"
```

---

## Task 6a: Router types, the streaming seam and the in-memory request seam

**Files:**
- Create: `include/secure_frontend/request_router.hpp`
- Modify: `include/secure_frontend/http_request_handler.hpp`, `src/http_request_handler.cpp` (rewritten), `include/secure_frontend/http_wire.hpp`, `src/http_wire.cpp` (head serializer), `tests/http_parser_test.cpp`, `tests/http_request_handler_test.cpp` (bodies only), `CMakeLists.txt`

**Interfaces:**
- Consumes: `RequestHead`, `parse_request_head`, `scan_request_head`, `HeadLimits`, `ascii_iequal` (Task 2); `HttpPolicy`, `check_host`, `check_origin`, `evaluate_preflight` (Task 3); `make_error_response`, `ApiError`, `serialize_response` (Task 1); SP1 `BearerTokenValidator`.
- Produces (ruling R-A; M2 consumes these names verbatim): `RouteId`, `RouteEntry`, `kRouteTable`, `RouteDecision`, `RequestContext`, `kNoResponseStatus`, `HttpHead`, `StreamSink`, `StreamHandler`, `RouteResult`, `RequestRouter` (whose `handle` returns `RouteResult`), `handle_http_request(std::string_view, RequestRouter&, const HeadLimits&)`, and in `http_wire.hpp`:

```cpp
// Status line, Date, Connection: close, the security headers and head.headers;
// never Content-Length (a stream frames itself); ends with the blank line.
[[nodiscard]] std::string serialize_head(const HttpHead& head, std::chrono::system_clock::time_point now);
```

  `serialize_response` is rebuilt on the same helper: it adds `Content-Length` (not for 204) and drops any `Transfer-Encoding` a complete response might carry. Kept until Task 9 deletes it: the legacy overload `handle_http_request(request, const HttpPolicy&, const BearerTokenValidator&, const RequestDispatcher&, const HttpParserLimits&)` (a thin adapter), so `HttpSocketServer` and `main` keep compiling unchanged in this task.

- [ ] **Step 1: Write the failing tests** — add to `tests/http_parser_test.cpp` (include `secure_frontend/request_router.hpp`; register both in `secure_frontend_http_parser_tests`):

```cpp
void wire_head_has_no_content_length() {
  const auto now = std::chrono::system_clock::time_point{std::chrono::seconds{1791963785}};
  const std::string head = secure_frontend::serialize_head(
      {200, {{"Content-Type", "text/event-stream; charset=utf-8"}, {"Transfer-Encoding", "chunked"}, {"Content-Length", "5"}}}, now);
  expect(head.rfind("HTTP/1.1 200 OK\r\n", 0) == 0, "status line");
  expect(head.find("Content-Length") == std::string::npos, "a head never carries Content-Length");
  expect(head.find("Transfer-Encoding: chunked\r\n") != std::string::npos, "a stream may set its own framing header");
  expect(head.find("X-Content-Type-Options: nosniff\r\n") != std::string::npos && head.find("Connection: close\r\n") != std::string::npos,
         "security headers and Connection: close");
  expect(head.size() >= 4U && head.substr(head.size() - 4U) == "\r\n\r\n", "ends with the blank line");
  const std::string complete = secure_frontend::serialize_response(
      {.status = 200, .headers = {{"Transfer-Encoding", "chunked"}}, .body = "ok"}, now);
  expect(complete.find("Transfer-Encoding") == std::string::npos && complete.find("Content-Length: 2\r\n") != std::string::npos,
         "complete responses always use Content-Length");
}

struct SeamRouter final : secure_frontend::RequestRouter {
  bool stream{false};
  std::string seen_body;
  secure_frontend::RouteDecision authorize(const secure_frontend::RequestHead& head) override {
    secure_frontend::RouteDecision decision;
    decision.route = secure_frontend::RouteId::chat;
    decision.body_limit = head.content_length.value_or(0U);
    return decision;
  }
  secure_frontend::RouteResult handle(const secure_frontend::RequestHead&, std::string_view body, const secure_frontend::RouteDecision&,
                                      const secure_frontend::RequestContext&) override {
    seen_body = std::string{body};
    if (stream) return secure_frontend::StreamHandler{[](secure_frontend::StreamSink&) {}};
    return secure_frontend::HttpResponse{.status = 200, .headers = {}, .body = "{}\n"};
  }
};

void route_result_seam_returns_complete_response() {
  SeamRouter router;
  const std::string request = "POST /x HTTP/1.1\r\nHost: h\r\nContent-Length: 2\r\n\r\n{}";
  const auto ok = secure_frontend::handle_http_request(request, router, kLimits);
  expect(ok.status == 200 && router.seen_body == "{}", "a complete response passes through the seam");
  expect(secure_frontend::handle_http_request(request + "x", router, kLimits).status == 400, "bytes after the body: 400");
  router.stream = true;
  const auto streamed = secure_frontend::handle_http_request(request, router, kLimits);
  expect(streamed.status == 500 && streamed.body == secure_frontend::render_error_body(ApiError::internal_error),
         "a StreamHandler needs a socket: the in-memory seam fails closed");
}
```

- [ ] **Step 2: Run to see RED**

Run: `cmake --build --preset linux-gcc 2>&1 | grep -m1 error`
Expected: FAIL `secure_frontend/request_router.hpp: No such file or directory`.

- [ ] **Step 3: Implement** — `include/secure_frontend/request_router.hpp`:

```cpp
#pragma once

#include "secure_frontend/http_request_handler.hpp"
#include "secure_frontend/http_request_head.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace secure_frontend {

enum class RouteId { health, models, model, chat, other };

struct RouteEntry final {
  std::string_view method;
  std::string_view path;
  bool prefix;   // /v1/models/<id>: the rest of the target is the id
  RouteId id;
};

// The one route table (spec §3.6); auth and EXT tests iterate it.
inline constexpr std::array<RouteEntry, 4> kRouteTable{{
    {"GET", "/health", false, RouteId::health},
    {"GET", "/v1/models", false, RouteId::models},
    {"GET", "/v1/models/", true, RouteId::model},
    {"POST", "/v1/chat/completions", false, RouteId::chat},
}};

struct RouteDecision final {
  std::optional<HttpResponse> early;      // set: answer now, the body is never read
  bool expect_continue{false};
  std::size_t body_limit{0};              // bytes the body stage reads (= Content-Length)
  RouteId route{RouteId::other};
  std::optional<std::string> allow_origin;
};

struct RequestContext final {
  std::function<bool()> client_gone;                // chat only; empty = never gone
  const std::atomic<bool>* shutting_down{nullptr};  // null = never
};

// A router returns this status when the client is gone and nothing may be written (spec §2.6).
inline constexpr int kNoResponseStatus = 0;

// Streaming seam (spec §3.11). M1 declares it and dispatches it in the connection
// handler (deliver_route_result, Task 8b); M2 produces StreamHandlers.
struct HttpHead final {
  int status;
  std::vector<std::pair<std::string, std::string>> headers;   // never Content-Length; a stream sets its own framing headers
};

// Implemented by the connection handler over one accepted socket. Every write is
// bounded by limits.http.write_stall_ms on progress (and by kShutdownDrainBudget
// once shutdown has begun); false = the client is gone or the write failed.
struct StreamSink {
  virtual bool send_headers(const HttpHead& head) = 0;           // status line, Date, Connection: close, security headers,
                                                                // head.headers, plus the decision's CORS headers
  virtual bool send_event(std::string_view bytes) = 0;           // one HTTP/1.1 chunk carrying exactly `bytes`
  virtual bool writable(std::chrono::milliseconds timeout) = 0;  // the socket can take more bytes within `timeout`
  virtual bool client_gone() = 0;                                // EOF, reset, or any byte after the body (probe_peer)
  virtual void finish() = 0;                                     // [DONE] already sent -> terminal chunk "0\r\n\r\n"
  virtual ~StreamSink() = default;
};
using StreamHandler = std::function<void(StreamSink&)>;
using RouteResult = std::variant<HttpResponse, StreamHandler>;

class RequestRouter {
 public:
  virtual ~RequestRouter() = default;
  // Spec §4.1 steps 3-8; CPU only; runs before any body byte is read.
  [[nodiscard]] virtual RouteDecision authorize(const RequestHead& head) = 0;
  // Runs after exactly decision.body_limit body bytes were read.
  [[nodiscard]] virtual RouteResult handle(const RequestHead& head, std::string_view body, const RouteDecision& decision,
                                           const RequestContext& context) = 0;
};

// In-memory seam over one complete request: scan, parse, authorize, body
// length check (mismatch or extra bytes: 400), handle. A StreamHandler needs a
// socket, so the seam answers it with a fixed 500 internal_error.
[[nodiscard]] HttpResponse handle_http_request(std::string_view request, RequestRouter& router, const HeadLimits& limits);

}  // namespace secure_frontend
```

`include/secure_frontend/http_request_handler.hpp` keeps `BearerTokenValidator`, `HttpResponse`, `RequestDispatcher`, `HttpParserLimits` and the legacy `handle_http_request(std::string_view, const HttpPolicy&, const BearerTokenValidator&, const RequestDispatcher&, const HttpParserLimits& = {})` declaration (comment: `// Legacy adapter for the serial server; deleted in SP3 Task 9.`); it no longer declares the policy-shaped helpers. Add `#include "secure_frontend/http_policy.hpp"` only in the `.cpp` (forward-declare `struct HttpPolicy;` in the header to avoid an include cycle).

`src/http_request_handler.cpp` (rewritten):

```cpp
#include "secure_frontend/http_request_handler.hpp"

#include "secure_frontend/api_errors.hpp"
#include "secure_frontend/http_policy.hpp"
#include "secure_frontend/request_router.hpp"

namespace secure_frontend {

std::optional<std::string_view> HttpResponse::header_value(const std::string_view name) const {
  for (const auto& [header_name, value] : headers) {
    if (ascii_iequal(header_name, name)) return value;
  }
  return std::nullopt;
}

HttpResponse handle_http_request(const std::string_view request, RequestRouter& router, const HeadLimits& limits) {
  const HeadScanResult scan = scan_request_head(request, limits);
  if (scan.state == HeadScan::line_too_long) return make_error_response(ApiError::request_line_too_long);
  if (scan.state == HeadScan::header_too_large) return make_error_response(ApiError::header_too_large);
  if (scan.state != HeadScan::complete) return make_error_response(ApiError::invalid_request);
  const HeadParseResult parsed = parse_request_head(request.substr(0, scan.head_bytes), limits);
  if (const auto* error = std::get_if<ApiError>(&parsed)) return make_error_response(*error);
  const RequestHead& head = std::get<RequestHead>(parsed);
  RouteDecision decision = router.authorize(head);
  if (decision.early.has_value()) return std::move(*decision.early);
  const std::string_view body = request.substr(scan.head_bytes);
  if (body.size() != head.content_length.value_or(0U)) return make_error_response(ApiError::invalid_request);
  RouteResult result = router.handle(head, body, decision, RequestContext{});
  if (auto* complete = std::get_if<HttpResponse>(&result)) return std::move(*complete);
  return make_error_response(ApiError::internal_error);
}

namespace {
// Adapts the SP0/SP1 dispatcher to RequestRouter for the serial server until Task 9.
class DispatcherRouter final : public RequestRouter {
 public:
  DispatcherRouter(const HttpPolicy& policy, const BearerTokenValidator& validator, const RequestDispatcher& dispatch)
      : policy_(policy), validator_(validator), dispatch_(dispatch) {}
  RouteDecision authorize(const RequestHead& head) override {
    RouteDecision decision;
    const PolicyVerdict host = check_host(policy_, head.values("host"));
    if (host != PolicyVerdict::allowed) {
      decision.early = make_error_response(host == PolicyVerdict::malformed ? ApiError::invalid_request : ApiError::forbidden);
      return decision;
    }
    const OriginDecision origin = check_origin(policy_, head.values("origin"));
    if (origin.verdict != PolicyVerdict::allowed) { decision.early = make_error_response(ApiError::forbidden); return decision; }
    if (auto preflight = evaluate_preflight(policy_, head)) { decision.early = std::move(*preflight); return decision; }
    const auto auth = head.values("authorization");
    if (auth.size() > 1U) { decision.early = make_error_response(ApiError::invalid_request); return decision; }
    bool ok = false;
    if (auth.size() == 1U && auth.front().rfind("Bearer ", 0) == 0 && auth.front().size() > 7U) {
      try { ok = validator_ && validator_(auth.front().substr(7U)); } catch (...) { ok = false; }
    }
    if (!ok) decision.early = make_error_response(ApiError::invalid_api_key);
    decision.body_limit = head.content_length.value_or(0U);
    decision.allow_origin = origin.allow_origin;
    return decision;
  }
  RouteResult handle(const RequestHead& head, std::string_view body, const RouteDecision& decision, const RequestContext&) override {
    HttpResponse response = make_error_response(ApiError::engine_unavailable);
    try { if (dispatch_) response = dispatch_(head.method, head.target, body); } catch (...) {}
    if (decision.allow_origin) {
      response.headers.emplace_back("Access-Control-Allow-Origin", *decision.allow_origin);
      response.headers.emplace_back("Vary", "Origin");
    }
    return response;
  }
 private:
  const HttpPolicy& policy_;
  const BearerTokenValidator& validator_;
  const RequestDispatcher& dispatch_;
};
}  // namespace

HttpResponse handle_http_request(const std::string_view request, const HttpPolicy& policy, const BearerTokenValidator& validator,
                                 const RequestDispatcher& dispatch, const HttpParserLimits& limits) {
  DispatcherRouter router(policy, validator, dispatch);
  return handle_http_request(request, router,
                             HeadLimits{limits.max_request_line_bytes, limits.max_header_bytes, limits.max_headers, limits.max_request_bytes});
}

}  // namespace secure_frontend
```

`include/secure_frontend/http_wire.hpp`: add `#include "secure_frontend/request_router.hpp"` and the `serialize_head` declaration above. `src/http_wire.cpp`: replace `is_reserved_header` and `serialize_response` with

```cpp
bool is_reserved_header(std::string_view name, const bool complete) {
  for (const auto& [fixed, value] : kSecurityHeaders) {
    (void)value;
    if (ascii_iequal(name, fixed)) return true;
  }
  return ascii_iequal(name, "Date") || ascii_iequal(name, "Connection") || ascii_iequal(name, "Content-Length") ||
         ascii_iequal(name, "Server") || ascii_iequal(name, "X-Powered-By") || (complete && ascii_iequal(name, "Transfer-Encoding"));
}

std::string head_text(const int status, const std::vector<std::pair<std::string, std::string>>& headers,
                      const std::chrono::system_clock::time_point now, const std::optional<std::size_t> content_length) {
  std::string output{"HTTP/1.1 "};
  output += std::to_string(status);
  output += ' ';
  output += reason_phrase(status);
  output += "\r\nDate: ";
  output += imf_fixdate(now);
  output += "\r\nConnection: close\r\n";
  if (content_length.has_value()) output += "Content-Length: " + std::to_string(*content_length) + "\r\n";
  for (const auto& [name, value] : kSecurityHeaders) {
    output += name;
    output += ": ";
    output += value;
    output += "\r\n";
  }
  for (const auto& [name, value] : headers) {
    // Values come from code, never from the request; the CR/LF guard stays (SP0).
    if (name.find_first_of("\r\n") != std::string::npos || value.find_first_of("\r\n") != std::string::npos ||
        is_reserved_header(name, content_length.has_value() || status == 204)) {
      continue;
    }
    output += name;
    output += ": ";
    output += value;
    output += "\r\n";
  }
  output += "\r\n";
  return output;
}
```

(both in the anonymous namespace; the old `is_reserved_header(std::string_view)` and the body of the old `serialize_response` are deleted) and at namespace scope

```cpp
std::string serialize_head(const HttpHead& head, const std::chrono::system_clock::time_point now) {
  return head_text(head.status, head.headers, now, std::nullopt);
}

std::string serialize_response(const HttpResponse& response, const std::chrono::system_clock::time_point now) {
  std::string output = head_text(response.status, response.headers, now,
                                 response.status == 204 ? std::nullopt : std::optional<std::size_t>{response.body.size()});
  if (response.status != 204) output += response.body;
  return output;
}
```

(`#include <optional>`, `<vector>`, `<utility>`, and `secure_frontend/http_request_head.hpp` for `ascii_iequal`; delete the file-local `ascii_iequal` copy.) Link: `target_link_libraries(secure_frontend_http_request_handler PUBLIC secure_frontend_http_policy)`.

`tests/http_request_handler_test.cpp` still calls the legacy overload; in this task only its assertions on SP0 `text/plain` bodies (`"Bad Request\n"`, `"Unauthorized\n"`, `"Forbidden\n"`) change to `secure_frontend::render_error_body(<code>)` for the same status (spec §9.4); Task 6b rewrites the file.

- [ ] **Step 4: Run to see GREEN**

```bash
cmake --build --preset linux-gcc && ctest --preset linux-gcc -R 'http_parser_tests|http_request_handler_tests|http_socket_server_tests' --output-on-failure
ctest --preset linux-gcc | tail -1
cmake --build --preset windows-llvm-mingw-cross && ctest --preset windows-llvm-mingw-cross | tail -1
```

Expected: all pass; any other assertion on an SP0 `text/plain` body produced by the request handler is updated here to the §7.2 body and named in the commit body.

- [ ] **Step 5: Handoff row and commit** — row `| 6a | §3.11 seam, §4.1 seam | RequestRouter returning RouteResult; HttpHead/StreamSink/StreamHandler; serialize_head; in-memory handle_http_request; legacy adapter | http_parser_tests.wire_head_has_no_content_length, …route_result_seam_returns_complete_response | … | … |`.

```bash
git -C "$(git rev-parse --show-toplevel)" add secure_frontend docs/AGENT_HANDOFF.md
git -C "$(git rev-parse --show-toplevel)" -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "http: router types with the streaming seam (RouteResult, StreamSink); head serializer without Content-Length (SP3 §3.11, §4.1)" \
  -m "Tests: http_parser_tests.wire_head_has_no_content_length, route_result_seam_returns_complete_response. Handler test bodies updated per spec §9.4: <names>. RED: <line>. wine: <line>." \
  -m "Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>" -m "Claude-Session: <session link>"
```

---

## Task 6b: `ApiRouter` — header-first authentication, `kRouteTable` routing, `/health`, `/v1/models[/id]`

**Files:**
- Modify: `include/secure_frontend/api_router.hpp`, `src/api_router.cpp` (rewritten), `tests/http_request_handler_test.cpp` (rewritten, case names kept), `tests/api_router_test.cpp` (rewritten, case names kept), `tests/http_parser_test.cpp`, `src/secure_server_main.cpp`, `CMakeLists.txt`

**Interfaces:**
- Consumes: Task 6a's `RequestRouter`, `RouteResult`, `RouteDecision`, `RequestContext`, `kRouteTable`, `handle_http_request`; `HttpPolicy`, `check_host`, `check_origin`, `evaluate_preflight` (Task 3); `make_error_response`, `ApiError` (Task 1); SP1 `BearerTokenValidator`, `make_bearer_validator`.
- Produces: `HealthView{bool healthy; std::string_view engine; std::string_view recovery;}`, `HealthProvider = std::function<HealthView()>`, `ChatHandler = std::function<RouteResult(std::string_view body, const RequestContext&)>`, `ApiRouterConfig{HttpPolicy policy; BearerTokenValidator validator; std::string model_id; std::string owned_by; std::size_t chat_body_limit; HealthProvider health; ChatHandler chat;}`, `class ApiRouter final : public RequestRouter` (`authorize`, `handle` → `RouteResult`); `bool is_json_content_type(std::string_view) noexcept`.

`authorize` order (spec §4.1 steps 3–8): Host → Origin → preflight → `Authorization` (more than one → 400; missing, not exactly `Bearer <non-empty>`, or rejected → 401 with `WWW-Authenticate: Bearer`; a throwing validator → 401) → route (`kRouteTable`, byte-exact; `/v1/models/<id>` needs a non-empty id; a known path with another method → 405 with `Allow`; anything else → 404) → chat: `Content-Type` (exactly one field, `is_json_content_type`) → 415; `Content-Length` > `chat_body_limit` → 413 → `expect_continue`, `body_limit = content_length`. Every response after a CORS-approved origin carries `Access-Control-Allow-Origin` and `Vary: Origin` (also the early ones, so a browser can read a `401`).

- [ ] **Step 1: Write the failing tests** — add to `tests/http_parser_test.cpp` (include `secure_frontend/api_router.hpp`, `secure_frontend/token_verifier.hpp`, `secure_frontend/secret_bytes.hpp`, `secure_frontend/config.hpp`; add the cases to `run_cases`):

```cpp
constexpr std::string_view kToken = "parser-token-0123456789abcdefghijklmnopq";

secure_frontend::ApiRouter make_router(std::vector<std::string> cors = {}, int* chat_calls = nullptr) {
  secure_frontend::SecretBytes secret;
  (void)secret.assign(kToken);
  secure_frontend::ApiRouterConfig config{
      .policy = secure_frontend::make_http_policy(*secure_frontend::parse_listener_address("127.0.0.1"), 8080, {}, cors),
      .validator = secure_frontend::make_bearer_validator(std::make_shared<const secure_frontend::TokenVerifier>(std::move(secret))),
      .model_id = "fake-model", .owned_by = "local", .chat_body_limit = 49152,
      .health = [] { return secure_frontend::HealthView{true, "ready", ""}; },
      .chat = [chat_calls](std::string_view, const secure_frontend::RequestContext&) {
        if (chat_calls != nullptr) ++*chat_calls;
        return secure_frontend::HttpResponse{.status = 200, .headers = {{"Content-Type", "application/json; charset=utf-8"}}, .body = "{}\n"};
      }};
  return secure_frontend::ApiRouter(std::move(config));
}

std::string chat_request(const std::string& extra_headers, const std::string& body = "{}") {
  return "POST /v1/chat/completions HTTP/1.1\r\nHost: 127.0.0.1:8080\r\nAuthorization: Bearer " + std::string{kToken} +
         "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n" + extra_headers + "\r\n" + body;
}

void dos01_status_matrix_415_417_505() {
  auto router = make_router();
  const auto status_of = [&router](const std::string& request) {
    return secure_frontend::handle_http_request(request, router, kLimits).status;
  };
  expect(status_of(chat_request("Content-Type: text/plain\r\n")) == 415, "text/plain body is 415");
  expect(status_of(chat_request("")) == 415, "missing Content-Type is 415");
  expect(status_of(chat_request("Content-Type: application/json\r\nExpect: foo\r\n")) == 417, "Expect: foo is 417");
  expect(status_of("GET /health HTTP/1.0\r\nHost: 127.0.0.1:8080\r\n\r\n") == 505, "HTTP/1.0 is 505");
  expect(status_of("GET /health HTTP/2.0\r\nHost: 127.0.0.1:8080\r\n\r\n") == 505, "HTTP/2.0 is 505");
  expect(status_of(chat_request("Content-Type: application/json\r\n")) == 200, "a correct chat reaches the handler");
}

void content_type_variants_accepted() {
  auto router = make_router();
  for (const char* type : {"application/json", "application/json; charset=utf-8", "application/json;charset=UTF-8",
                           "Application/JSON", "application/json ;  charset=utf-8"}) {
    expect(secure_frontend::handle_http_request(chat_request(std::string{"Content-Type: "} + type + "\r\n"), router, kLimits).status == 200,
           std::string{"accepted Content-Type "} + type);
    expect(secure_frontend::is_json_content_type(type), std::string{"is_json_content_type "} + type);
  }
  for (const char* type : {"application/json; charset=latin1", "text/plain", "application/jsonx", "application/json; charset=utf-8; x=1",
                           "multipart/form-data"}) {
    expect(secure_frontend::handle_http_request(chat_request(std::string{"Content-Type: "} + type + "\r\n"), router, kLimits).status == 415,
           std::string{"rejected Content-Type "} + type);
  }
  expect(secure_frontend::handle_http_request(chat_request("Content-Type: application/json\r\nContent-Type: application/json\r\n"),
                                              router, kLimits).status == 415, "two Content-Type fields are 415");
}

void auth_and_route_order_follows_spec() {
  int chat_calls = 0;
  auto router = make_router({"http://app.example:3000"}, &chat_calls);
  const auto run = [&router](const std::string& request) { return secure_frontend::handle_http_request(request, router, kLimits); };
  // Unauthenticated: 401 on every path and method, before 404/405/415/413.
  for (const std::string line : {"GET /nope", "DELETE /health", "POST /v1/chat/completions", "GET /v1/models/x", "PUT /v1/models"}) {
    const std::string body_headers = line.rfind("POST", 0) == 0 || line.rfind("PUT", 0) == 0 ? "Content-Length: 99999\r\n" : "";
    const auto r = run(line + " HTTP/1.1\r\nHost: 127.0.0.1:8080\r\n" + body_headers + "\r\n");
    expect(r.status == (body_headers.empty() ? 401 : 413), line + " without a token");
  }
  const auto unauth = run("POST /v1/chat/completions HTTP/1.1\r\nHost: 127.0.0.1:8080\r\nContent-Length: 2\r\nContent-Type: text/plain\r\n\r\n{}");
  expect(unauth.status == 401 && unauth.header_value("WWW-Authenticate") == std::optional<std::string_view>{"Bearer"},
         "401 wins over 415 and carries WWW-Authenticate");
  const std::string auth = "Authorization: Bearer " + std::string{kToken} + "\r\n";
  expect(run("DELETE /health HTTP/1.1\r\nHost: 127.0.0.1:8080\r\n" + auth + "\r\n").status == 405, "known path, other method: 405");
  expect(run("DELETE /health HTTP/1.1\r\nHost: 127.0.0.1:8080\r\n" + auth + "\r\n").header_value("Allow") == std::optional<std::string_view>{"GET"},
         "405 names Allow");
  expect(run("GET /v1/chat/completions HTTP/1.1\r\nHost: 127.0.0.1:8080\r\n" + auth + "\r\n").header_value("Allow") ==
         std::optional<std::string_view>{"POST"}, "chat Allow: POST");
  expect(run("GET /nope HTTP/1.1\r\nHost: 127.0.0.1:8080\r\n" + auth + "\r\n").status == 404, "unknown path after auth: 404");
  expect(run("GET /v1/models/ HTTP/1.1\r\nHost: 127.0.0.1:8080\r\n" + auth + "\r\n").status == 404, "empty model id: 404");
  expect(run(chat_request("Content-Type: application/json\r\n", std::string(49153, ' '))).status == 413, "route body limit: 413");
  expect(run("GET /health HTTP/1.1\r\nHost: evil:8080\r\n" + auth + "\r\n").status == 403, "Host before auth: 403");
  expect(run("GET /health HTTP/1.1\r\nHost: 127.0.0.1:8080\r\nOrigin: null\r\n" + auth + "\r\n").status == 403, "Origin before auth: 403");
  const auto cors = run("GET /health HTTP/1.1\r\nHost: 127.0.0.1:8080\r\nOrigin: http://app.example:3000\r\n\r\n");
  expect(cors.status == 401 && cors.header_value("Access-Control-Allow-Origin") == std::optional<std::string_view>{"http://app.example:3000"},
         "an approved CORS origin gets ACAO even on 401");
  const auto preflight = run("OPTIONS /v1/chat/completions HTTP/1.1\r\nHost: 127.0.0.1:8080\r\nOrigin: http://app.example:3000\r\n"
                             "Access-Control-Request-Method: POST\r\n\r\n");
  expect(preflight.status == 204, "preflight needs no token");
  expect(run("GET /health HTTP/1.1\r\nHost: 127.0.0.1:8080\r\n" + auth + auth + "\r\n").status == 400, "two Authorization fields: 400");
  expect(chat_calls == 0, "no chat handler call for any refused request");
  // Body-stage rules of the in-memory seam: Content-Length mismatch and stray bytes are 400.
  expect(run(chat_request("Content-Type: application/json\r\n") + "x").status == 400, "bytes after the body: 400");
  std::string short_body = chat_request("Content-Type: application/json\r\n");
  short_body.pop_back();
  expect(run(short_body).status == 400, "fewer bytes than Content-Length: 400");
}
```

Rewrite `tests/api_router_test.cpp` keeping every registered case name, over the new API (each case builds `make_router`-style config like above; the file's `main` lists the same names):

```cpp
// health_samples_status_before_state_once_each: the provider is called exactly once per /health.
void health_samples_status_before_state_once_each() {
  int calls = 0;
  auto router = router_with([&calls] { ++calls; return secure_frontend::HealthView{true, "ready", ""}; });
  (void)router.handle(head_of("GET", "/health"), "", decision_of(router, "GET", "/health"), {});
  expect(calls == 1, "one snapshot per /health request");
}
void health_includes_engine_state_when_provided() {
  auto router = router_with([] { return secure_frontend::HealthView{true, "generating", ""}; });
  const auto r = complete(router.handle(head_of("GET", "/health"), "", decision_of(router, "GET", "/health"), {}));
  expect(r.status == 200 && r.body == "{\"status\":\"ok\",\"engine\":\"generating\"}\n", "SP2 body unchanged: " + r.body);
}
void health_is_503_with_engine_state_when_unavailable() {
  auto router = router_with([] { return secure_frontend::HealthView{false, "failed", "restarting"}; });
  const auto r = complete(router.handle(head_of("GET", "/health"), "", decision_of(router, "GET", "/health"), {}));
  expect(r.status == 503 && r.body == "{\"status\":\"unavailable\",\"engine\":\"failed\",\"recovery\":\"restarting\"}\n", "503 body: " + r.body);
}
void dispatches_exact_health_route() {
  auto router = router_with([] { return secure_frontend::HealthView{true, "ready", ""}; });
  expect(decision_of(router, "GET", "/health").route == secure_frontend::RouteId::health && !decision_of(router, "GET", "/health").early,
         "/health is routed");
}
void dispatches_models_with_json_escaping() {
  auto router = router_with({}, "quote\"model", "own\\er");
  const auto r = complete(router.handle(head_of("GET", "/v1/models"), "", decision_of(router, "GET", "/v1/models"), {}));
  expect(r.status == 200 && r.body == "{\"object\":\"list\",\"data\":[{\"id\":\"quote\\\"model\",\"object\":\"model\",\"created\":0,\"owned_by\":\"own\\\\er\"}]}\n",
         "only the served model, escaped: " + r.body);
  const auto model = complete(router.handle(head_of("GET", "/v1/models/other"), "", decision_of(router, "GET", "/v1/models/other"), {}));
  expect(model.status == 404 && model.body == secure_frontend::render_error_body(secure_frontend::ApiError::model_not_found),
         "other model id: 404 model_not_found");
}
void rejects_non_exact_routes() {
  auto router = router_with({});
  for (const char* target : {"/health/", "//health", "/HEALTH", "/v1/models/", "/v1//models", "/v1/chat/completions/"}) {
    const auto d = decision_of(router, "GET", target);
    expect(d.early.has_value() && d.early->status == 404, std::string{"404 for "} + target);
  }
}
void fails_closed_when_provider_throws_or_returns_invalid_model() {
  auto router = router_with([]() -> secure_frontend::HealthView { throw 1; });
  const auto r = complete(router.handle(head_of("GET", "/health"), "", decision_of(router, "GET", "/health"), {}));
  expect(r.status == 503 && r.body == secure_frontend::render_error_body(secure_frontend::ApiError::engine_unavailable),
         "a throwing health provider is a fixed 503");
  auto bad = router_with({}, "bad\x01model");
  const auto models = complete(bad.handle(head_of("GET", "/v1/models"), "", decision_of(bad, "GET", "/v1/models"), {}));
  expect(models.status == 500 && models.body == secure_frontend::render_error_body(secure_frontend::ApiError::internal_error),
         "a control byte in the model id fails closed");
}
void auth01_every_route_rejects_missing_malformed_wrong_token() {
  auto router = router_with({});
  const std::string token{kRouterToken};
  const std::pair<std::string, int> variants[] = {
      {"", 401}, {"Authorization: bearer " + token + "\r\n", 401}, {"Authorization: Bearer \r\n", 401},
      {"Authorization: Bearer\t" + token + "\r\n", 401}, {"Authorization: Basic " + token + "\r\n", 401},
      {"Authorization: Bearer " + token + "\r\nAuthorization: Bearer " + token + "\r\n", 400},
      {"Authorization: Bearer wrong-token-0123456789abcdefghijklmn\r\n", 401}};
  std::vector<std::pair<std::string, std::string>> routes;
  for (const auto& entry : secure_frontend::kRouteTable) {
    routes.emplace_back(std::string{entry.method}, entry.prefix ? std::string{entry.path} + "fake-model" : std::string{entry.path});
  }
  routes.emplace_back("GET", "/does-not-exist");
  for (const auto& [method, target] : routes) {
    for (const auto& [headers, status] : variants) {
      std::string request = method + " " + target + " HTTP/1.1\r\nHost: 127.0.0.1:8080\r\n" + headers;
      request += method == "POST" ? "Content-Length: 2\r\nContent-Type: application/json\r\n\r\n{}" : "\r\n";
      const auto response = secure_frontend::handle_http_request(request, router, kRouterLimits);
      expect(response.status == status, method + " " + target + " must answer " + std::to_string(status));
      expect(response.body.find("fake-model") == std::string::npos && response.body.find("status") == std::string::npos,
             "no route data before authentication");
    }
  }
}
void models_list_keeps_utf8_model_id() {
  auto router = router_with({}, "mod\xc3\xa8le-\xc3\xbc");
  const auto r = complete(router.handle(head_of("GET", "/v1/models"), "", decision_of(router, "GET", "/v1/models"), {}));
  expect(r.status == 200 && r.body.find("mod\xc3\xa8le-\xc3\xbc") != std::string::npos && r.body.find("\\u00") == std::string::npos,
         "UTF-8 model id copied unchanged");
}
```

with these helpers at the top of the rewritten file:

```cpp
constexpr std::string_view kRouterToken = "router-token-0123456789abcdefghijklmnopq";
constexpr secure_frontend::HeadLimits kRouterLimits{4096, 16384, 64, 65536};

secure_frontend::ApiRouter router_with(secure_frontend::HealthProvider health, std::string model = "fake-model",
                                       std::string owned_by = "local") {
  secure_frontend::SecretBytes secret;
  (void)secret.assign(kRouterToken);
  return secure_frontend::ApiRouter({
      .policy = secure_frontend::make_http_policy(*secure_frontend::parse_listener_address("127.0.0.1"), 8080, {}, {}),
      .validator = secure_frontend::make_bearer_validator(std::make_shared<const secure_frontend::TokenVerifier>(std::move(secret))),
      .model_id = std::move(model), .owned_by = std::move(owned_by), .chat_body_limit = 49152,
      .health = health ? std::move(health) : secure_frontend::HealthProvider{[] { return secure_frontend::HealthView{true, "ready", ""}; }},
      .chat = {}});
}
secure_frontend::RequestHead head_of(std::string method, std::string target) {
  secure_frontend::RequestHead head;
  head.method = std::move(method);
  head.target = std::move(target);
  head.fields = {{"Host", "127.0.0.1:8080"}, {"Authorization", "Bearer " + std::string{kRouterToken}}};
  return head;
}
secure_frontend::RouteDecision decision_of(secure_frontend::ApiRouter& router, std::string method, std::string target) {
  return router.authorize(head_of(std::move(method), std::move(target)));
}
secure_frontend::HttpResponse complete(secure_frontend::RouteResult result) {
  return std::get<secure_frontend::HttpResponse>(std::move(result));
}
```

Rewrite `tests/http_request_handler_test.cpp` keeping its registered names over the new seam (`handle_http_request(request, router, limits)` with a `RecordingRouter : RequestRouter` that records the body it gets and answers 200):
- `accepts_authorized_request` — a valid GET reaches `handle` once;
- `passes_the_bounded_raw_body_to_an_authorized_dispatcher` — the POST body bytes arrive unchanged at `handle`;
- `rejects_malformed_and_ambiguous_messages` — `GET /x HTTP/1.1\r\n\r\n` without Host → 400 body `render_error_body(invalid_request)`; bare LF → 400; TE → 411;
- `requires_one_well_formed_bearer_token` — the seven `auth01` variants on `/health`;
- `fails_closed_before_calling_validator_for_untrusted_origin` — a validator that counts calls is never called for `Origin: http://evil.example` (403);
- `emits_cors_only_for_policy_approved_origin` — ACAO present only for the configured origin, never for same-origin, never `*`;
- `handles_policy_approved_preflight_without_exposing_an_endpoint` — `OPTIONS` preflight → 204, `handle` never called;
- `auth03_router_uses_verifier` — `make_bearer_validator(TokenVerifier)` accepts the exact token and rejects a prefix and a one-byte-longer value;
- `constant_time_helper_matches_only_equal_tokens` — keep the existing body if `constant_time_token_equals` still exists on `main` (`grep -n constant_time_token_equals include/secure_frontend/http_request_handler.hpp`); if SP1 deleted the helper, delete this case and its registration and say so in the commit body.

`RecordingRouter` for that file implements `authorize` by delegating to an `ApiRouter` built with `router_with`-equivalent code and a `chat` handler that records the body; its `handle` returns the `RouteResult` of that `ApiRouter`.

Register in `CMakeLists.txt`: add `dos01_status_matrix_415_417_505 content_type_variants_accepted auth_and_route_order_follows_spec` to `secure_frontend_http_parser_tests` (and link it with `secure_frontend_api_router secure_frontend_secrets secure_frontend_http_policy`); link `secure_frontend_api_router_tests` and `secure_frontend_http_request_handler_tests` with `secure_frontend_api_router secure_frontend_http_policy secure_frontend_secrets`.

- [ ] **Step 2: Run to see RED**

Run: `cmake --build --preset linux-gcc 2>&1 | grep -m1 error`
Expected: FAIL `'ApiRouterConfig' is not a member of 'secure_frontend'`.

- [ ] **Step 3: Implement** — `include/secure_frontend/api_router.hpp` (rewritten):

```cpp
#pragma once

#include "secure_frontend/http_policy.hpp"
#include "secure_frontend/request_router.hpp"

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

namespace secure_frontend {

// One SP2 snapshot rendered as the /health body. engine/recovery are SP2's
// fixed literals (health_for); recovery empty = absent.
struct HealthView final {
  bool healthy;
  std::string_view engine;
  std::string_view recovery;
};
using HealthProvider = std::function<HealthView()>;
// A chat handler returns a complete response or (M2) a StreamHandler.
using ChatHandler = std::function<RouteResult(std::string_view body, const RequestContext& context)>;

struct ApiRouterConfig final {
  HttpPolicy policy;
  BearerTokenValidator validator;
  std::string model_id;
  std::string owned_by;
  std::size_t chat_body_limit;
  HealthProvider health;
  ChatHandler chat;
};

[[nodiscard]] bool is_json_content_type(std::string_view value) noexcept;

class ApiRouter final : public RequestRouter {
 public:
  explicit ApiRouter(ApiRouterConfig config);
  [[nodiscard]] RouteDecision authorize(const RequestHead& head) override;
  [[nodiscard]] RouteResult handle(const RequestHead& head, std::string_view body, const RouteDecision& decision,
                                   const RequestContext& context) override;

 private:
  ApiRouterConfig config_;
};

}  // namespace secure_frontend
```

`src/api_router.cpp` (rewritten):

```cpp
#include "secure_frontend/api_router.hpp"

#include "secure_frontend/api_errors.hpp"
#include "secure_frontend/json.hpp"

#include <algorithm>
#include <utility>

namespace secure_frontend {
namespace {

HttpResponse json_response(int status, std::string body) {
  return {.status = status, .headers = {{"Content-Type", "application/json; charset=utf-8"}}, .body = std::move(body)};
}

bool valid_field(std::string_view value) {
  return !value.empty() && value.size() <= 1024U &&
         std::none_of(value.begin(), value.end(), [](char c) { return static_cast<unsigned char>(c) < 0x20U || c == 0x7f; });
}

std::string model_object(std::string_view id, std::string_view owned_by) {
  std::string body{"{\"id\":"};
  append_json_string(&body, id);
  body.append(",\"object\":\"model\",\"created\":0,\"owned_by\":");
  append_json_string(&body, owned_by);
  body.push_back('}');
  return body;
}

std::string lower(std::string_view value) {
  std::string out{value};
  for (char& c : out) c = c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c;
  return out;
}

std::string_view trim(std::string_view v) {
  while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.remove_prefix(1U);
  while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.remove_suffix(1U);
  return v;
}

void add_cors(HttpResponse* response, const std::optional<std::string>& allow_origin) {
  if (!allow_origin.has_value()) return;
  response->headers.emplace_back("Access-Control-Allow-Origin", *allow_origin);
  response->headers.emplace_back("Vary", "Origin");
}

}  // namespace

bool is_json_content_type(const std::string_view value) noexcept {
  const std::string text = lower(trim(value));
  constexpr std::string_view kType = "application/json";
  if (text.rfind(kType, 0) != 0) return false;
  std::string_view rest = trim(std::string_view{text}.substr(kType.size()));
  if (rest.empty()) return true;
  if (rest.front() != ';') return false;
  return trim(rest.substr(1U)) == "charset=utf-8";
}

ApiRouter::ApiRouter(ApiRouterConfig config) : config_(std::move(config)) {}

RouteDecision ApiRouter::authorize(const RequestHead& head) {
  RouteDecision decision;
  const auto early = [&decision](HttpResponse response) {
    add_cors(&response, decision.allow_origin);
    decision.early = std::move(response);
    return decision;
  };
  const PolicyVerdict host = check_host(config_.policy, head.values("host"));
  if (host != PolicyVerdict::allowed) {
    return early(make_error_response(host == PolicyVerdict::malformed ? ApiError::invalid_request : ApiError::forbidden));
  }
  const OriginDecision origin = check_origin(config_.policy, head.values("origin"));
  if (origin.verdict != PolicyVerdict::allowed) return early(make_error_response(ApiError::forbidden));
  if (auto preflight = evaluate_preflight(config_.policy, head)) { decision.early = std::move(*preflight); return decision; }
  decision.allow_origin = origin.allow_origin;

  const auto authorizations = head.values("authorization");
  if (authorizations.size() > 1U) return early(make_error_response(ApiError::invalid_request));
  constexpr std::string_view kBearer = "Bearer ";
  bool authorized = false;
  if (authorizations.size() == 1U && authorizations.front().rfind(kBearer, 0) == 0 &&
      authorizations.front().size() > kBearer.size()) {
    try {
      authorized = config_.validator && config_.validator(authorizations.front().substr(kBearer.size()));
    } catch (...) {
      authorized = false;
    }
  }
  if (!authorized) return early(make_error_response(ApiError::invalid_api_key));

  const RouteEntry* matched = nullptr;
  std::string_view allow;
  for (const auto& entry : kRouteTable) {
    const bool path_match = entry.prefix ? (head.target.size() > entry.path.size() && head.target.rfind(entry.path, 0) == 0)
                                         : head.target == entry.path;
    if (!path_match) continue;
    allow = entry.method;
    if (head.method == entry.method) matched = &entry;
  }
  if (matched == nullptr) {
    if (allow.empty()) return early(make_error_response(ApiError::not_found));
    return early(make_error_response(ApiError::method_not_allowed, {.allow = allow}));
  }
  decision.route = matched->id;
  if (decision.route == RouteId::chat) {
    const auto types = head.values("content-type");
    if (types.size() != 1U || !is_json_content_type(types.front())) return early(make_error_response(ApiError::unsupported_media_type));
    if (head.content_length.value_or(0U) > config_.chat_body_limit) return early(make_error_response(ApiError::request_too_large));
  }
  decision.expect_continue = head.expect_continue;
  decision.body_limit = head.content_length.value_or(0U);
  return decision;
}

RouteResult ApiRouter::handle(const RequestHead& head, const std::string_view body, const RouteDecision& decision,
                              const RequestContext& context) {
  HttpResponse response = make_error_response(ApiError::internal_error);
  try {
    switch (decision.route) {
      case RouteId::health: {
        const HealthView view = config_.health ? config_.health() : HealthView{false, "failed", ""};
        std::string text{view.healthy ? "{\"status\":\"ok\",\"engine\":" : "{\"status\":\"unavailable\",\"engine\":"};
        append_json_string(&text, view.engine);
        if (!view.recovery.empty()) {
          text.append(",\"recovery\":");
          append_json_string(&text, view.recovery);
        }
        text.append("}\n");
        response = json_response(view.healthy ? 200 : 503, std::move(text));
        break;
      }
      case RouteId::models:
        if (!valid_field(config_.model_id) || !valid_field(config_.owned_by)) break;
        response = json_response(200, "{\"object\":\"list\",\"data\":[" + model_object(config_.model_id, config_.owned_by) + "]}\n");
        break;
      case RouteId::model: {
        const std::string_view id = std::string_view{head.target}.substr(std::string_view{"/v1/models/"}.size());
        if (!valid_field(config_.model_id) || !valid_field(config_.owned_by)) break;
        response = id == config_.model_id ? json_response(200, model_object(config_.model_id, config_.owned_by) + "\n")
                                          : make_error_response(ApiError::model_not_found);
        break;
      }
      case RouteId::chat: {
        RouteResult result = config_.chat ? config_.chat(body, context) : RouteResult{make_error_response(ApiError::engine_unavailable)};
        // A StreamHandler gets the CORS headers from deliver_route_result (Task 8b), which adds them to its head.
        if (auto* complete = std::get_if<HttpResponse>(&result)) add_cors(complete, decision.allow_origin);
        return result;
      }
      case RouteId::other:
        response = make_error_response(ApiError::not_found);
        break;
    }
  } catch (...) {
    response = make_error_response(decision.route == RouteId::health ? ApiError::engine_unavailable : ApiError::internal_error);
  }
  add_cors(&response, decision.allow_origin);
  return response;
}

}  // namespace secure_frontend
```

(`valid_field` failure on `/v1/models` leaves `internal_error`, matching the fail-closed test.) Link: `target_link_libraries(secure_frontend_api_router PUBLIC secure_frontend_http_policy secure_frontend_http_request_handler)`.

`src/secure_server_main.cpp` (still the serial server until Task 9): the `ApiRouter` it builds for `/health` and `/v1/models` (SP2 Task 6b's four-provider form, anchor `const secure_frontend::ApiRouter router(`) is no longer constructible. Replace that construction with

```cpp
  secure_frontend::ApiRouter router(secure_frontend::ApiRouterConfig{
      .policy = secure_frontend::make_http_policy(config.listener.host, config.listener.port, config.listener.host_aliases,
                                                  config.listener.cors_origins),
      .validator = secure_frontend::make_bearer_validator(verifier),
      .model_id = std::string{config.engine.model_id.value()},
      .owned_by = selected->owned_by,
      .chat_body_limit = config.limits.chat.max_body_bytes,
      .health = [&supervisor] {
        const auto s = supervisor.snapshot();
        return secure_frontend::HealthView{s.healthy, s.engine, s.recovery};
      },
      .chat = {}});
```

(`config`, `verifier`, `selected` and `supervisor` are the names SP1/SP2 use in `run`; if a name differs, use the local that holds the same object.) The legacy adapter has already authenticated the request, so the dispatcher lambda's GET branch (anchor `if (method == "GET") return router.dispatch(method, target);`) becomes a direct `handle` call with the route computed from the target:

```cpp
const auto route_for_get = [](std::string_view target) {
  if (target == "/health") return secure_frontend::RouteId::health;
  if (target == "/v1/models") return secure_frontend::RouteId::models;
  if (target.rfind("/v1/models/", 0) == 0 && target.size() > 11U) return secure_frontend::RouteId::model;
  return secure_frontend::RouteId::other;
};
// in the dispatcher lambda:
if (method == "GET") {
  secure_frontend::RequestHead get_head;
  get_head.method = "GET";
  get_head.target = std::string{target};
  return std::get<secure_frontend::HttpResponse>(
      router.handle(get_head, "", secure_frontend::RouteDecision{.route = route_for_get(target)}, {}));
}
```

(The legacy adapter has already authenticated the request; Task 9 replaces this whole lambda.)

- [ ] **Step 4: Run to see GREEN**

```bash
cmake --build --preset linux-gcc && ctest --preset linux-gcc -R 'http_parser_tests|api_router_tests|http_request_handler_tests' --output-on-failure
ctest --preset linux-gcc | tail -1
cmake --build --preset windows-llvm-mingw-cross && ctest --preset windows-llvm-mingw-cross | tail -1
```

Expected: all pass. Tests elsewhere that asserted SP0's `text/plain` error bodies or `{"error":"Not Found"}` from the router (spec §9.4 first row) are updated in this task to `secure_frontend::render_error_body(<code>)` for the same status; list each in the commit body. `/v1/models` now lists only the served model (E9): update any SP1/SP2 assertion that expected every inventory model.

- [ ] **Step 5: Handoff row and commit** — row `| 6b | AUTH-01..04 kept, DOS-01 (415/417/505), §4.1, §3.6 | header-first RequestRouter/ApiRouter, kRouteTable, 401/404/405/415/413 order, /v1/models[/id] only the served model | http_parser_tests.dos01_status_matrix_415_417_505, …content_type_variants_accepted, …auth_and_route_order_follows_spec; api_router_tests.* (rewritten, names kept); http_request_handler_tests.* (rewritten, names kept) | … | … |`.

```bash
git -C "$(git rev-parse --show-toplevel)" add secure_frontend docs/AGENT_HANDOFF.md
git -C "$(git rev-parse --show-toplevel)" -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "http: header-first routing; authenticate before the body; route table, 404/405/415/413 order (SP3 §4.1, §3.6)" \
  -m "Tests: http_parser_tests.dos01_status_matrix_415_417_505, content_type_variants_accepted (Review Focus 4), auth_and_route_order_follows_spec; api_router_tests and http_request_handler_tests rewritten with their names kept (auth01 now iterates kRouteTable). Bodies updated per spec §9.4: <names>. RED: <line>. wine: <line>." \
  -m "Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>" -m "Claude-Session: <session link>"
```

---

## Task 7: Chat request schema (§3.1) with fixed `param` keys and opt-in JSON decimals

**Files:**
- Create: `include/secure_frontend/sampling_params.hpp`
- Modify (SP1): `include/secure_frontend/json.hpp`, `src/json.cpp`, `tests/json_test.cpp`
- Modify: `include/secure_frontend/chat_schema.hpp`, `src/chat_schema.cpp`, `tests/chat_schema_test.cpp`, `tests/http_parser_test.cpp`, `CMakeLists.txt`

**Interfaces:**
- Consumes: SP1 `parse_json`, `JsonLimits`, `JsonValue`; `ApiError`, `api_constants::kChatMaxJsonDepth`, `kTemperatureMax`, `kSeedMax`, `kChatModelMaxBytes`, `kChatUserMaxBytes` (Task 1).
- Produces: `JsonLimits::allow_fractions` (`bool`, default `false`), `JsonKind::decimal`, `JsonValue::make_decimal(double)`, `const double* JsonValue::if_decimal() const noexcept`; `struct SamplingParams { std::optional<double> temperature; std::optional<double> top_p; std::optional<std::uint64_t> seed; bool empty() const noexcept; }`; `ChatCompletionRequest` gains `bool stream{false}; bool include_usage{false}; SamplingParams sampling{};` (`max_tokens` keeps its type and now holds any value ≥ 1 up to `UINT32_MAX`, clamped later); `struct ChatRequestError { ApiError error; std::string_view param; }`; `using ChatRequestResult = std::variant<ChatCompletionRequest, ChatRequestError>`; `ChatRequestResult parse_chat_request(std::string_view json, const ChatSchemaLimits&)`. `parse_chat_completion_request` stays as `std::optional` wrapper (`nullopt` on any error).

Rules (spec §3.1, decision B6, M1 note 7): unknown key → `invalid_request`; extension keys (`tools`, `tool_choice`, `functions`, `function_call`, `parallel_tool_calls`, `mcp_servers`) or a message role `tool`/`function` → `extension_disabled` with that key (`role` for roles) — checked **before** any other member error, first in document order; `stream: true` → `unsupported_parameter` `stream` (M1); `stream_options` only with `stream: true` (so in M1 always `invalid_request` unless it is accompanied by `stream:true`, which is already refused); `max_tokens`/`max_completion_tokens`: integer ≥ 1 (0, negative, decimal → `invalid_request`), both present → `invalid_request`, larger than `UINT32_MAX` → clamped to `UINT32_MAX` at parse; `n`: `1` ok, other integer → `unsupported_parameter` `n`; `user`: string ≤ 256 bytes, ignored; `temperature`: number 0–2; `top_p`: number in (0, 1]; `seed`: integer 0–2⁶³−1 — out-of-range → `invalid_request`; `presence_penalty`/`frequency_penalty`: number `0` → no-op, other → `unsupported_parameter`; `logit_bias`: `{}` no-op, other → `unsupported_parameter`; `stop`: `null` or `[]` no-op, other → `unsupported_parameter`; `logprobs`: `false` no-op, `true` → `unsupported_parameter`; `top_k`, `min_p`, `top_logprobs`, `response_format` → `unsupported_parameter`; `content` not a string → `unsupported_parameter` `content`; JSON syntax/size/depth > 4 → `invalid_json`; messages count/size, model grammar, control bytes, remote schemes (SP0 rules) → `invalid_request`.

- [ ] **Step 1: Write the failing tests** — `tests/json_test.cpp`, new case `json_decimals_only_when_enabled` (registered):

```cpp
void json_decimals_only_when_enabled() {
  const secure_frontend::JsonLimits strict{.max_bytes = 1024, .max_depth = 4, .max_container_entries = 16, .max_string_bytes = 64};
  auto relaxed = strict;
  relaxed.allow_fractions = true;
  const auto rejected = secure_frontend::parse_json("{\"t\":0.5}", strict);
  expect(std::holds_alternative<secure_frontend::JsonError>(rejected) &&
         std::get<secure_frontend::JsonError>(rejected).code == secure_frontend::JsonErrorCode::non_integer_number,
         "the default (config) reader still rejects fractions");
  for (const auto& [text, value] : std::vector<std::pair<std::string, double>>{{"0.5", 0.5}, {"1e0", 1.0}, {"-2.5E-1", -0.25}, {"1.0", 1.0}}) {
    const auto parsed = secure_frontend::parse_json("{\"t\":" + text + "}", relaxed);
    const auto* root = std::get_if<secure_frontend::JsonValue>(&parsed);
    const auto* member = root != nullptr ? root->if_object() : nullptr;
    expect(member != nullptr && member->front().value.if_decimal() != nullptr && *member->front().value.if_decimal() == value,
           "decimal " + text);
  }
  const auto integer = secure_frontend::parse_json("{\"t\":2}", relaxed);
  expect(std::get<secure_frontend::JsonValue>(integer).if_object()->front().value.if_integer() != nullptr, "integers stay integers");
  for (const char* bad : {"{\"t\":.5}", "{\"t\":1.}", "{\"t\":1e}", "{\"t\":01.5}", "{\"t\":1e400}"}) {
    expect(std::holds_alternative<secure_frontend::JsonError>(secure_frontend::parse_json(bad, relaxed)), std::string{"invalid "} + bad);
  }
}
```

`tests/http_parser_test.cpp` (include `secure_frontend/chat_schema.hpp`; register each):

```cpp
const secure_frontend::ChatSchemaLimits kSchema{};
std::string body_with(const std::string& members, const std::string& messages = R"([{"role":"user","content":"Hi"}])") {
  return R"({"model":"fake-model","messages":)" + messages + (members.empty() ? "" : "," + members) + "}";
}
secure_frontend::ChatRequestResult parse(const std::string& json) { return secure_frontend::parse_chat_request(json, kSchema); }
bool fails_with(const secure_frontend::ChatRequestResult& r, ApiError error, std::string_view param = {}) {
  const auto* e = std::get_if<secure_frontend::ChatRequestError>(&r);
  return e != nullptr && e->error == error && e->param == param;
}
const secure_frontend::ChatCompletionRequest* ok(const secure_frontend::ChatRequestResult& r) {
  return std::get_if<secure_frontend::ChatCompletionRequest>(&r);
}

void dos01_json_depth_5_is_400() {
  expect(fails_with(parse(body_with(R"("logit_bias":{"a":{"b":{"c":{"d":1}}}})")), ApiError::invalid_json), "depth 5 is invalid_json");
  expect(fails_with(parse(body_with(R"("logit_bias":{"a":{"b":1}})")), ApiError::unsupported_parameter, "logit_bias"),
         "depth 4 parses and reaches the schema");
  expect(fails_with(parse("{\"model\":"), ApiError::invalid_json), "truncated JSON is invalid_json");
  expect(fails_with(parse(body_with(R"("model":"x")")), ApiError::invalid_json), "duplicate key is invalid_json");
}

void dos01_json_limits() {
  std::string many = "[";
  for (int i = 0; i < 129; ++i) many += std::string{i ? "," : ""} + R"({"role":"user","content":"x"})";
  expect(fails_with(parse(body_with("", many + "]")), ApiError::invalid_request) ||
         fails_with(parse(body_with("", many + "]")), ApiError::invalid_json), "129 messages are refused");
  expect(fails_with(parse(body_with("", "[]")), ApiError::invalid_request), "no messages");
  expect(fails_with(parse(body_with("", R"([{"role":"user","content":")" + std::string(65537, 'a') + "\"}]")), ApiError::invalid_request) ||
         fails_with(parse(body_with("", R"([{"role":"user","content":")" + std::string(65537, 'a') + "\"}]")), ApiError::invalid_json),
         "a message over 64 KiB is refused");
  expect(fails_with(parse(body_with(R"("max_tokens":0)")), ApiError::invalid_request), "max_tokens 0");
  expect(fails_with(parse(body_with(R"("max_tokens":-1)")), ApiError::invalid_request), "max_tokens -1");
  expect(fails_with(parse(body_with(R"("max_tokens":1.5)")), ApiError::invalid_request), "max_tokens 1.5");
  expect(fails_with(parse(body_with(R"("max_tokens":1e3)")), ApiError::invalid_request), "max_tokens 1e3");
  expect(fails_with(parse(body_with(R"("max_tokens":"8")")), ApiError::invalid_request), "max_tokens as a string");
  expect(fails_with(parse(body_with(R"("max_tokens":01)")), ApiError::invalid_json), "leading zero is invalid JSON");
  expect(fails_with(parse(body_with(R"("max_tokens":5,"max_completion_tokens":5)")), ApiError::invalid_request), "both names");
  const auto big = parse(body_with(R"("max_tokens":99999999999)"));
  expect(ok(big) != nullptr && ok(big)->max_tokens == std::optional<std::size_t>{4294967295U}, "huge max_tokens is accepted and clamped later");
  const auto alias = parse(body_with(R"("max_completion_tokens":7)"));
  expect(ok(alias) != nullptr && ok(alias)->max_tokens == std::optional<std::size_t>{7U}, "max_completion_tokens is max_tokens");
  expect(fails_with(parse(body_with(R"("unknown":1)")), ApiError::invalid_request), "unknown key");
  expect(fails_with(parse(body_with(R"("user":")" + std::string(257, 'u') + "\"")), ApiError::invalid_request), "user over 256 bytes");
  expect(ok(parse(body_with(R"("user":"alice")"))) != nullptr, "user is accepted and ignored");
  expect(fails_with(parse(body_with("", R"([{"role":"user","content":"https://x"}])")), ApiError::invalid_request), "remote scheme (SP0)");
  expect(fails_with(parse(R"({"model":"pull:x","messages":[{"role":"user","content":"Hi"}]})"), ApiError::invalid_request), "model grammar");
}

void dos01_content_array_is_unsupported_parameter() {
  expect(fails_with(parse(body_with("", R"([{"role":"user","content":[{"type":"text","text":"Hi"}]}])")),
                    ApiError::unsupported_parameter, "content"), "array content is unsupported_parameter content");
  expect(fails_with(parse(body_with("", R"([{"role":"tool","content":"run"}])")), ApiError::extension_disabled, "role"), "role tool");
  expect(fails_with(parse(body_with("", R"([{"role":"function","content":"run"}])")), ApiError::extension_disabled, "role"), "role function");
  for (const char* key : {"tools", "tool_choice", "functions", "function_call", "parallel_tool_calls", "mcp_servers"}) {
    expect(fails_with(parse(body_with(std::string{"\""} + key + "\":null,\"zzz\":1")), ApiError::extension_disabled, key),
           std::string{"extension key wins over an unknown key: "} + key);
  }
  expect(fails_with(parse(body_with(R"("stream":true)")), ApiError::unsupported_parameter, "stream"), "stream:true is M2");
  expect(ok(parse(body_with(R"("stream":false)"))) != nullptr, "stream:false is the complete response");
  expect(fails_with(parse(body_with(R"("stream_options":{"include_usage":true})")), ApiError::invalid_request), "stream_options without stream");
}

void sampling_neutral_values_are_no_ops() {
  const auto r = parse(body_with(R"("presence_penalty":0,"frequency_penalty":0.0,"logit_bias":{},"stop":null,"n":1,"logprobs":false)"));
  expect(ok(r) != nullptr && ok(r)->sampling.empty(), "neutral values parse and change nothing");
  expect(ok(parse(body_with(R"("stop":[])"))) != nullptr, "stop [] is a no-op");
  expect(fails_with(parse(body_with(R"("presence_penalty":0.5)")), ApiError::unsupported_parameter, "presence_penalty"), "non-zero penalty");
  expect(fails_with(parse(body_with(R"("frequency_penalty":-1)")), ApiError::unsupported_parameter, "frequency_penalty"), "negative penalty");
  expect(fails_with(parse(body_with(R"("logit_bias":{"1":2})")), ApiError::unsupported_parameter, "logit_bias"), "non-empty logit_bias");
  expect(fails_with(parse(body_with(R"("stop":"x")")), ApiError::unsupported_parameter, "stop"), "stop string");
  expect(fails_with(parse(body_with(R"("stop":["x"])")), ApiError::unsupported_parameter, "stop"), "stop array");
  expect(fails_with(parse(body_with(R"("n":2)")), ApiError::unsupported_parameter, "n"), "n 2");
  expect(fails_with(parse(body_with(R"("logprobs":true)")), ApiError::unsupported_parameter, "logprobs"), "logprobs true");
  for (const char* key : {"top_k", "min_p", "top_logprobs", "response_format"}) {
    expect(fails_with(parse(body_with(std::string{"\""} + key + "\":1")), ApiError::unsupported_parameter, key), std::string{"SP5 key "} + key);
  }
  const auto passed = parse(body_with(R"("temperature":0.5,"top_p":0.9,"seed":42)"));
  expect(ok(passed) != nullptr && ok(passed)->sampling.temperature == std::optional<double>{0.5} &&
         ok(passed)->sampling.top_p == std::optional<double>{0.9} && ok(passed)->sampling.seed == std::optional<std::uint64_t>{42U},
         "bounded sampling values pass through (B6 option e)");
}

void sampling_bounds_rejected() {
  for (const char* member : {R"("temperature":-0.1)", R"("temperature":2.01)", R"("top_p":0)", R"("top_p":1.0001)",
                             R"("seed":-1)", R"("seed":1.5)", R"("temperature":"1")", R"("top_p":null)"}) {
    expect(fails_with(parse(body_with(member)), ApiError::invalid_request), std::string{"out of bounds: "} + member);
  }
  expect(ok(parse(body_with(R"("temperature":2,"top_p":1,"seed":9223372036854775807)"))) != nullptr, "upper bounds are inclusive");
}

void sampling_integer_forms_accepted() {
  const auto r = parse(body_with(R"("temperature":1,"top_p":1,"presence_penalty":0,"frequency_penalty":0)"));
  expect(ok(r) != nullptr && ok(r)->sampling.temperature == std::optional<double>{1.0} && ok(r)->sampling.top_p == std::optional<double>{1.0},
         "integer-valued numbers are numbers");
  const auto zero = parse(body_with(R"("temperature":0)"));
  expect(ok(zero) != nullptr && ok(zero)->sampling.temperature == std::optional<double>{0.0}, "temperature 0 is valid");
}
```

Register: `secure_frontend_json_tests` + `json_decimals_only_when_enabled`; `secure_frontend_http_parser_tests` + `dos01_json_depth_5_is_400 dos01_json_limits dos01_content_array_is_unsupported_parameter sampling_neutral_values_are_no_ops sampling_bounds_rejected sampling_integer_forms_accepted` (link `secure_frontend_chat_schema`).

- [ ] **Step 2: Run to see RED**

Run: `cmake --build --preset linux-gcc 2>&1 | grep -m1 error`
Expected: FAIL `'struct secure_frontend::JsonLimits' has no member named 'allow_fractions'`.

- [ ] **Step 3: Implement**

`json.hpp`: `enum class JsonKind { null, boolean, integer, decimal, string, array, object };`; in `JsonValue` add `static JsonValue make_decimal(double value);`, `[[nodiscard]] const double* if_decimal() const noexcept { return kind_ == JsonKind::decimal ? &decimal_ : nullptr; }` and the member `double decimal_{0.0};`; in `JsonLimits` add the last member `bool allow_fractions{false};   // SP3: chat sampling numbers; config files keep integers only`; update the file comment "Numbers are integers only" to "Numbers are integers unless JsonLimits::allow_fractions".

`json.cpp`: `JsonValue JsonValue::make_decimal(const double value) { JsonValue v; v.kind_ = JsonKind::decimal; v.decimal_ = value; return v; }` and in `parse_number` replace the line `return fail(JsonErrorCode::non_integer_number, start);` with:

```cpp
      if (!limits_.allow_fractions) return fail(JsonErrorCode::non_integer_number, start);
      return parse_decimal_tail(start, out);
```

and add to the parser class (it holds the limits as `limits_`; if SP1 named the member differently, use that name):

```cpp
  // RFC 8259 frac/exp after the integer part; the whole lexeme is converted
  // with from_chars, so "1.", ".5" and "1e" fail and 1e400 (inf) fails.
  bool parse_decimal_tail(const std::size_t start, JsonValue& out) {
    if (!at_end() && text_[position_] == '.') {
      ++position_;
      const std::size_t digits = position_;
      while (!at_end() && text_[position_] >= '0' && text_[position_] <= '9') ++position_;
      if (position_ == digits) return fail(JsonErrorCode::syntax, start);
    }
    if (!at_end() && (text_[position_] == 'e' || text_[position_] == 'E')) {
      ++position_;
      if (!at_end() && (text_[position_] == '+' || text_[position_] == '-')) ++position_;
      const std::size_t digits = position_;
      while (!at_end() && text_[position_] >= '0' && text_[position_] <= '9') ++position_;
      if (position_ == digits) return fail(JsonErrorCode::syntax, start);
    }
    double value = 0.0;
    const char* first = text_.data() + start;
    const char* last = text_.data() + position_;
    const auto [ptr, ec] = std::from_chars(first, last, value);
    if (ec != std::errc{} || ptr != last || !std::isfinite(value)) return fail(JsonErrorCode::integer_out_of_range, start);
    out = JsonValue::make_decimal(value);
    return true;
  }
```

(`#include <charconv>`, `<cmath>`; a leading-zero integer part such as `01.5` already fails in the integer scan before this point.)

`include/secure_frontend/sampling_params.hpp`:

```cpp
#pragma once

#include <cstdint>
#include <optional>

namespace secure_frontend {

// Decision B6 option (e): the only sampling keys passed to the engine's GEN
// line, each already bounded by the chat schema. Absent = the engine default
// (greedy).
struct SamplingParams final {
  std::optional<double> temperature;   // 0..2
  std::optional<double> top_p;         // (0, 1]
  std::optional<std::uint64_t> seed;   // 0..2^63-1
  [[nodiscard]] bool empty() const noexcept { return !temperature && !top_p && !seed; }
};

}  // namespace secure_frontend
```

`chat_schema.hpp`: include `api_errors.hpp`, `sampling_params.hpp`, `<variant>`; extend `ChatCompletionRequest` with `bool stream{false}; bool include_usage{false}; SamplingParams sampling{};`; add

```cpp
struct ChatRequestError final {
  ApiError error;
  std::string_view param;   // a fixed key from api_errors (is_fixed_param_key) or empty
};
using ChatRequestResult = std::variant<ChatCompletionRequest, ChatRequestError>;
[[nodiscard]] ChatRequestResult parse_chat_request(std::string_view json, const ChatSchemaLimits& limits = {});
```

`chat_schema.cpp`: replace the body of `parse_chat_completion_request` with `const auto r = parse_chat_request(json, limits); if (const auto* req = std::get_if<ChatCompletionRequest>(&r)) return *req; return std::nullopt;` and add (keep SP1's `valid_model`, `valid_content`, `valid_utf8`, `contains_remote_scheme` helpers):

```cpp
namespace {

constexpr std::string_view kExtensionKeys[] = {"tools", "tool_choice", "functions", "function_call", "parallel_tool_calls", "mcp_servers"};
constexpr std::string_view kSp5Keys[] = {"top_k", "min_p", "top_logprobs", "response_format"};
template <std::size_t N>
std::string_view find_key(const std::string_view (&keys)[N], std::string_view key) {
  for (const auto k : keys) if (k == key) return k;
  return {};
}

std::optional<double> number_of(const JsonValue& value) {
  if (const auto* i = value.if_integer()) return static_cast<double>(*i);
  if (const auto* d = value.if_decimal()) return *d;
  return std::nullopt;
}

ChatRequestError fail(ApiError error, std::string_view param = {}) { return {error, param}; }

}  // namespace

ChatRequestResult parse_chat_request(const std::string_view json, const ChatSchemaLimits& limits) {
  const JsonLimits json_limits{.max_bytes = limits.max_body_bytes, .max_depth = api_constants::kChatMaxJsonDepth,
                               .max_container_entries = std::max<std::size_t>(limits.max_messages, 16U),
                               .max_string_bytes = limits.max_body_bytes, .allow_fractions = true};
  const auto parsed = parse_json(json, json_limits);
  const auto* root = std::get_if<JsonValue>(&parsed);
  if (root == nullptr) return fail(ApiError::invalid_json);
  const auto* members = root->if_object();
  if (members == nullptr) return fail(ApiError::invalid_request);

  // Extensions first, in document order (EXT-01), including message roles.
  for (const auto& member : *members) {
    if (const auto key = find_key(kExtensionKeys, member.key); !key.empty()) return fail(ApiError::extension_disabled, key);
    if (member.key == "messages") {
      if (const auto* items = member.value.if_array()) {
        for (const auto& item : *items) {
          const auto* fields = item.if_object();
          if (fields == nullptr) continue;
          for (const auto& field : *fields) {
            const auto* role = field.value.if_string();
            if (field.key == "role" && role != nullptr && (*role == "tool" || *role == "function")) {
              return fail(ApiError::extension_disabled, "role");
            }
          }
        }
      }
    }
  }

  ChatCompletionRequest request;
  bool model_seen = false, messages_seen = false, max_seen = false, stream_options_seen = false;
  for (const auto& member : *members) {
    const std::string& key = member.key;
    const JsonValue& value = member.value;
    if (key == "model") {
      const auto* text = value.if_string();
      if (text == nullptr || text->size() > api_constants::kChatModelMaxBytes || !valid_model(*text)) return fail(ApiError::invalid_request);
      request.model = *text;
      model_seen = true;
    } else if (key == "messages") {
      const auto* items = value.if_array();
      if (items == nullptr || items->empty() || items->size() > limits.max_messages) return fail(ApiError::invalid_request);
      for (const auto& item : *items) {
        const auto* fields = item.if_object();
        if (fields == nullptr) return fail(ApiError::invalid_request);
        ChatMessage message;
        bool role_seen = false, content_seen = false;
        for (const auto& field : *fields) {
          if (field.key == "content" && field.value.kind() == JsonKind::array) return fail(ApiError::unsupported_parameter, "content");
          const auto* text = field.value.if_string();
          if (text == nullptr) return fail(ApiError::invalid_request);
          if (field.key == "role" && (*text == "system" || *text == "user" || *text == "assistant")) {
            message.role = *text;
            role_seen = true;
          } else if (field.key == "content" && valid_content(*text, limits.max_message_bytes)) {
            message.content = *text;
            content_seen = true;
          } else {
            return fail(ApiError::invalid_request);
          }
        }
        if (!role_seen || !content_seen) return fail(ApiError::invalid_request);
        request.messages.push_back(std::move(message));
      }
      messages_seen = true;
    } else if (key == "max_tokens" || key == "max_completion_tokens") {
      const auto* integer = value.if_integer();
      if (max_seen || integer == nullptr || *integer < 1) return fail(ApiError::invalid_request);
      request.max_tokens = static_cast<std::size_t>(std::min<std::int64_t>(*integer, 4294967295LL));
      max_seen = true;
    } else if (key == "stream") {
      const auto* flag = value.if_boolean();
      if (flag == nullptr) return fail(ApiError::invalid_request);
      if (*flag) return fail(ApiError::unsupported_parameter, "stream");   // M1; M2 implements streaming
      request.stream = false;
    } else if (key == "stream_options") {
      stream_options_seen = true;
    } else if (key == "n") {
      const auto* integer = value.if_integer();
      if (integer == nullptr) return fail(ApiError::invalid_request);
      if (*integer != 1) return fail(ApiError::unsupported_parameter, "n");
    } else if (key == "user") {
      const auto* text = value.if_string();
      if (text == nullptr || text->size() > api_constants::kChatUserMaxBytes) return fail(ApiError::invalid_request);
    } else if (key == "temperature") {
      const auto number = number_of(value);
      if (!number || *number < 0.0 || *number > api_constants::kTemperatureMax) return fail(ApiError::invalid_request);
      request.sampling.temperature = *number;
    } else if (key == "top_p") {
      const auto number = number_of(value);
      if (!number || *number <= 0.0 || *number > 1.0) return fail(ApiError::invalid_request);
      request.sampling.top_p = *number;
    } else if (key == "seed") {
      const auto* integer = value.if_integer();
      if (integer == nullptr || *integer < 0) return fail(ApiError::invalid_request);
      request.sampling.seed = static_cast<std::uint64_t>(*integer);
    } else if (key == "presence_penalty" || key == "frequency_penalty") {
      const auto number = number_of(value);
      if (!number) return fail(ApiError::invalid_request);
      if (*number != 0.0) return fail(ApiError::unsupported_parameter, key == "presence_penalty" ? "presence_penalty" : "frequency_penalty");
    } else if (key == "logit_bias") {
      const auto* object = value.if_object();
      if (object == nullptr || !object->empty()) return fail(ApiError::unsupported_parameter, "logit_bias");
    } else if (key == "stop") {
      const auto* array = value.if_array();
      if (!(value.kind() == JsonKind::null || (array != nullptr && array->empty()))) return fail(ApiError::unsupported_parameter, "stop");
    } else if (key == "logprobs") {
      const auto* flag = value.if_boolean();
      if (flag == nullptr) return fail(ApiError::invalid_request);
      if (*flag) return fail(ApiError::unsupported_parameter, "logprobs");
    } else if (const auto sp5 = find_key(kSp5Keys, key); !sp5.empty()) {
      return fail(ApiError::unsupported_parameter, sp5);
    } else {
      return fail(ApiError::invalid_request);
    }
  }
  if (stream_options_seen && !request.stream) return fail(ApiError::invalid_request);
  if (!model_seen || !messages_seen) return fail(ApiError::invalid_request);
  return request;
}
```

(`#include "secure_frontend/api_constants.hpp"`, `<algorithm>`. `seed` above `INT64_MAX` cannot occur: SP1's reader rejects it as `integer_out_of_range` → `invalid_json`; a `seed` of `1.5` is a decimal → `invalid_request`.) Update `tests/chat_schema_test.cpp`'s `rejects_unsupported_and_unsafe_features` if it asserted that `"stream":true` or `max_tokens` above 4096 return `nullopt` from `parse_chat_completion_request`: `stream:true` still returns `nullopt`; `max_tokens` 5000 now parses (B2) — change that one expectation and name it in the commit body.

- [ ] **Step 4: Run to see GREEN**

```bash
cmake --build --preset linux-gcc && ctest --preset linux-gcc -R 'json_tests|http_parser_tests|chat_schema_tests' --output-on-failure
ctest --preset linux-gcc | tail -1
cmake --build --preset windows-llvm-mingw-cross && ctest --preset windows-llvm-mingw-cross | tail -1
```

- [ ] **Step 5: Handoff (row + SP1 addendum A3) and commit** — row `| 7 | DOS-01 (JSON), EXT-01 (schema), B6 | §3.1 schema: fixed param keys, extensions first, neutral no-ops, bounded temperature/top_p/seed, max_tokens alias and clamp-at-resolve | http_parser_tests.dos01_json_depth_5_is_400, …dos01_json_limits, …dos01_content_array_is_unsupported_parameter, …sampling_neutral_values_are_no_ops, …sampling_bounds_rejected, …sampling_integer_forms_accepted; json_tests.json_decimals_only_when_enabled | … | … |`; SP1 addendum `| A3 | secure_frontend_json: opt-in JsonLimits::allow_fractions + JsonKind::decimal (config stays integer-only); chat schema depth 4, max_tokens clamped instead of rejected | 7 |`.

```bash
git -C "$(git rev-parse --show-toplevel)" add secure_frontend docs/AGENT_HANDOFF.md
git -C "$(git rev-parse --show-toplevel)" -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "chat: OpenAI request schema with fixed error params, sampling subset, opt-in JSON decimals (SP3 §3.1, B6, DOS-01)" \
  -m "Tests: http_parser_tests.dos01_json_depth_5_is_400, dos01_json_limits, dos01_content_array_is_unsupported_parameter, sampling_neutral_values_are_no_ops, sampling_bounds_rejected, sampling_integer_forms_accepted (Review Focus 5); json_tests.json_decimals_only_when_enabled. RED: <line>. wine: <line>." \
  -m "Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>" -m "Claude-Session: <session link>"
```

---
## Task 8a: Polled, non-blocking socket I/O (`socket_io`)

**Files:**
- Create: `include/secure_frontend/socket_io.hpp`, `src/socket_io.cpp`
- Modify: `tests/http_socket_server_test.cpp` (two new cases), `CMakeLists.txt`

**Interfaces:**
- Consumes: `api_constants` (Task 1).
- Produces (`socket_io.hpp`): `NativeSocket`, `kInvalidNativeSocket`, `close_native_socket()`, `class Socket` (RAII, move-only: `get()`, `release()`, `valid()`, `reset()`), `set_non_blocking()`, `set_no_delay()`, `struct PollEntry { NativeSocket socket; bool want_read; bool want_write; bool readable{false}; bool writable{false}; bool failed{false}; }`, `int poll_sockets(std::span<PollEntry>, std::chrono::milliseconds)`, `enum class IoStatus { done, would_block, closed, failed }`, `struct IoResult { IoStatus status; std::size_t bytes{0}; }`, `recv_some()`, `send_some()` (EINTR retried), `enum class WriteOutcome { complete, stalled, deadline, failed }`, `send_all_bounded()` (stall measured on progress), `enum class PeerState { alive, gone }`, `probe_peer()`, `bytes_pending()`, `half_close_and_drain()`, `class WinsockGuard`, `namespace detail { using SendCall; send_some_with(); last_socket_error(); set_last_socket_error(); kInterruptedCode; accept_backoff_for(); }`; library `secure_frontend_socket_io`. Nothing calls it yet outside its tests; Task 8b builds the connection handler on it.

- [ ] **Step 1: Write the failing tests** — add to `tests/http_socket_server_test.cpp` (include `secure_frontend/socket_io.hpp`, `<cerrno>`; register both in the file's `run_cases` and in `secure_frontend_add_cases(secure_frontend_http_socket_server_tests`; link the test with `secure_frontend_socket_io`):

```cpp
void send_retries_on_eintr() {
  static int calls = 0;
  calls = 0;
  const auto flaky = [](secure_frontend::NativeSocket, const char*, std::size_t size) noexcept -> long {
    if (++calls == 1) { secure_frontend::detail::set_last_socket_error(secure_frontend::detail::kInterruptedCode); return -1; }
    return static_cast<long>(size);
  };
  const auto result = secure_frontend::detail::send_some_with(flaky, secure_frontend::kInvalidNativeSocket, "hello");
  expect(result.status == secure_frontend::IoStatus::done && result.bytes == 5U && calls == 2, "an interrupted send is retried, not failed");
}

void accept_backs_off_on_transient_failure() {
#if defined(_WIN32)
  const int transient[] = {WSAECONNRESET, WSAEMFILE, WSAENOBUFS, WSAEINTR};
  const int fatal[] = {WSAENOTSOCK, WSAEINVAL, WSANOTINITIALISED};
#else
  const int transient[] = {ECONNABORTED, EMFILE, ENFILE, ENOBUFS, ENOMEM, EINTR, EPROTO, EPERM};
  const int fatal[] = {EBADF, ENOTSOCK, EINVAL};
#endif
  for (const int code : transient) {
    expect(secure_frontend::detail::accept_backoff_for(code) == std::optional<std::chrono::milliseconds>{std::chrono::milliseconds(50)},
           "50 ms backoff for transient accept error " + std::to_string(code));
  }
  for (const int code : fatal) {
    expect(!secure_frontend::detail::accept_backoff_for(code).has_value(), "fatal accept error " + std::to_string(code));
  }
}
```

- [ ] **Step 2: Run to see RED**

Run: `cmake --build --preset linux-gcc 2>&1 | grep -m1 error`
Expected: FAIL `secure_frontend/socket_io.hpp: No such file or directory`.

- [ ] **Step 3: Implement**

`include/secure_frontend/socket_io.hpp`:

```cpp
#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"
#endif

namespace secure_frontend {

#if defined(_WIN32)
using NativeSocket = SOCKET;
inline constexpr NativeSocket kInvalidNativeSocket = INVALID_SOCKET;
#else
using NativeSocket = int;
inline constexpr NativeSocket kInvalidNativeSocket = -1;
#endif

void close_native_socket(NativeSocket socket) noexcept;

class Socket final {
 public:
  Socket() = default;
  explicit Socket(NativeSocket socket) noexcept : socket_(socket) {}
  Socket(Socket&& other) noexcept : socket_(other.release()) {}
  Socket& operator=(Socket&& other) noexcept { if (this != &other) { reset(); socket_ = other.release(); } return *this; }
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
  ~Socket() { reset(); }
  [[nodiscard]] NativeSocket get() const noexcept { return socket_; }
  [[nodiscard]] bool valid() const noexcept { return socket_ != kInvalidNativeSocket; }
  NativeSocket release() noexcept { const NativeSocket s = socket_; socket_ = kInvalidNativeSocket; return s; }
  void reset() noexcept { close_native_socket(release()); }

 private:
  NativeSocket socket_{kInvalidNativeSocket};
};

[[nodiscard]] bool set_non_blocking(NativeSocket socket) noexcept;
void set_no_delay(NativeSocket socket) noexcept;

struct PollEntry final {
  NativeSocket socket;
  bool want_read;
  bool want_write;
  bool readable{false};
  bool writable{false};
  bool failed{false};
};
// poll()/WSAPoll(); returns the number of ready entries, 0 on timeout, -1 on error.
int poll_sockets(std::span<PollEntry> entries, std::chrono::milliseconds timeout) noexcept;

enum class IoStatus { done, would_block, closed, failed };
struct IoResult final {
  IoStatus status;
  std::size_t bytes{0};
};
[[nodiscard]] IoResult recv_some(NativeSocket socket, char* buffer, std::size_t size) noexcept;
// At most api_constants::kMaxSendChunk bytes; EINTR is retried (SP0 gap fixed).
[[nodiscard]] IoResult send_some(NativeSocket socket, std::string_view bytes) noexcept;

enum class WriteOutcome { complete, stalled, deadline, failed };
// Spec §2.7 / E4: `stall` is measured on progress, never on call return;
// `deadline` bounds the whole write.
[[nodiscard]] WriteOutcome send_all_bounded(NativeSocket socket, std::string_view bytes, std::chrono::milliseconds stall,
                                            std::chrono::steady_clock::time_point deadline) noexcept;

enum class PeerState { alive, gone };
// recv(MSG_PEEK) on the non-blocking socket: EOF, reset or ANY byte -> gone (spec §2.6).
[[nodiscard]] PeerState probe_peer(NativeSocket socket) noexcept;
// One non-blocking MSG_PEEK: true when at least one unread byte is waiting (EOF and errors are false).
[[nodiscard]] bool bytes_pending(NativeSocket socket) noexcept;
// Half-close, then drain for kDrainBudget / kDrainBytes (SP0 S4), polled.
void half_close_and_drain(NativeSocket socket) noexcept;

// WSAStartup/WSACleanup once per process (spec §10); a no-op on POSIX.
class WinsockGuard final {
 public:
  WinsockGuard() noexcept;
  ~WinsockGuard();
  WinsockGuard(const WinsockGuard&) = delete;
  WinsockGuard& operator=(const WinsockGuard&) = delete;
  [[nodiscard]] bool ok() const noexcept { return ok_; }
 private:
  bool ok_{true};
};

namespace detail {
using SendCall = long (*)(NativeSocket, const char*, std::size_t) noexcept;
[[nodiscard]] IoResult send_some_with(SendCall call, NativeSocket socket, std::string_view bytes) noexcept;
[[nodiscard]] int last_socket_error() noexcept;
void set_last_socket_error(int code) noexcept;
#if defined(_WIN32)
inline constexpr int kInterruptedCode = WSAEINTR;
#else
inline constexpr int kInterruptedCode = 4;   // EINTR on Linux
#endif
// nullopt: the listening socket itself is broken; otherwise the backoff before the next accept.
[[nodiscard]] std::optional<std::chrono::milliseconds> accept_backoff_for(int error_code) noexcept;
}  // namespace detail

}  // namespace secure_frontend
```

(On POSIX use `EINTR` itself: write `inline constexpr int kInterruptedCode = EINTR;` with `#include <cerrno>` in the non-Windows branch instead of the literal.)

`src/socket_io.cpp`:

```cpp
#include "secure_frontend/socket_io.hpp"

#include "secure_frontend/api_constants.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>

#if !defined(_WIN32)
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace secure_frontend {
namespace {
using Clock = std::chrono::steady_clock;

long native_send(NativeSocket socket, const char* data, std::size_t size) noexcept {
#if defined(_WIN32)
  return ::send(socket, data, static_cast<int>(std::min<std::size_t>(size, INT_MAX)), 0);
#elif defined(MSG_NOSIGNAL)
  return static_cast<long>(::send(socket, data, size, MSG_NOSIGNAL));
#else
  return static_cast<long>(::send(socket, data, size, 0));
#endif
}

bool would_block(int code) noexcept {
#if defined(_WIN32)
  return code == WSAEWOULDBLOCK;
#else
  return code == EAGAIN || code == EWOULDBLOCK;
#endif
}

int wait_one(NativeSocket socket, bool read, std::chrono::milliseconds timeout) noexcept {
  PollEntry entry{socket, read, !read};
  return poll_sockets(std::span<PollEntry>{&entry, 1U}, timeout) > 0 && !entry.failed ? 1 : 0;
}

}  // namespace

void close_native_socket(const NativeSocket socket) noexcept {
  if (socket == kInvalidNativeSocket) return;
#if defined(_WIN32)
  closesocket(socket);
#else
  ::close(socket);
#endif
}

bool set_non_blocking(const NativeSocket socket) noexcept {
#if defined(_WIN32)
  u_long mode = 1;
  return ioctlsocket(socket, FIONBIO, &mode) == 0;
#else
  const int flags = fcntl(socket, F_GETFL, 0);
  return flags >= 0 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

void set_no_delay(const NativeSocket socket) noexcept {
  const int one = 1;
  (void)setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
}

int poll_sockets(const std::span<PollEntry> entries, const std::chrono::milliseconds timeout) noexcept {
  constexpr std::size_t kMax = 128U;
  if (entries.size() > kMax) return -1;
#if defined(_WIN32)
  std::array<WSAPOLLFD, kMax> fds{};
#else
  std::array<pollfd, kMax> fds{};
#endif
  for (std::size_t i = 0; i < entries.size(); ++i) {
    fds[i].fd = entries[i].socket;
    fds[i].events = static_cast<short>((entries[i].want_read ? POLLIN : 0) | (entries[i].want_write ? POLLOUT : 0));
    fds[i].revents = 0;
  }
  const int ms = static_cast<int>(std::clamp<long long>(timeout.count(), 0, INT_MAX));
#if defined(_WIN32)
  const int ready = WSAPoll(fds.data(), static_cast<ULONG>(entries.size()), ms);
#else
  int ready = ::poll(fds.data(), static_cast<nfds_t>(entries.size()), ms);
  if (ready < 0 && errno == EINTR) ready = 0;
#endif
  if (ready <= 0) return ready;
  for (std::size_t i = 0; i < entries.size(); ++i) {
    entries[i].readable = (fds[i].revents & (POLLIN | POLLHUP)) != 0;
    entries[i].writable = (fds[i].revents & POLLOUT) != 0;
    entries[i].failed = (fds[i].revents & (POLLERR | POLLNVAL)) != 0;
  }
  return ready;
}

IoResult recv_some(const NativeSocket socket, char* buffer, const std::size_t size) noexcept {
  for (;;) {
#if defined(_WIN32)
    const int n = ::recv(socket, buffer, static_cast<int>(std::min<std::size_t>(size, INT_MAX)), 0);
#else
    const ssize_t n = ::recv(socket, buffer, size, 0);
#endif
    if (n > 0) return {IoStatus::done, static_cast<std::size_t>(n)};
    if (n == 0) return {IoStatus::closed};
    const int code = detail::last_socket_error();
    if (code == detail::kInterruptedCode) continue;
    return {would_block(code) ? IoStatus::would_block : IoStatus::failed};
  }
}

IoResult send_some(const NativeSocket socket, const std::string_view bytes) noexcept {
  return detail::send_some_with(&native_send, socket, bytes);
}

WriteOutcome send_all_bounded(const NativeSocket socket, std::string_view bytes, const std::chrono::milliseconds stall,
                              const Clock::time_point deadline) noexcept {
  auto last_progress = Clock::now();
  while (!bytes.empty()) {
    const auto now = Clock::now();
    if (now >= deadline) return WriteOutcome::deadline;
    if (now - last_progress >= stall) return WriteOutcome::stalled;
    const IoResult result = send_some(socket, bytes);
    if (result.status == IoStatus::done) {
      bytes.remove_prefix(result.bytes);
      last_progress = Clock::now();
      continue;
    }
    if (result.status != IoStatus::would_block) return WriteOutcome::failed;
    const auto wait = std::min({std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now),
                                std::chrono::duration_cast<std::chrono::milliseconds>(last_progress + stall - now),
                                api_constants::kStreamPollSlice});
    (void)wait_one(socket, false, std::max(wait, std::chrono::milliseconds(1)));
  }
  return WriteOutcome::complete;
}

PeerState probe_peer(const NativeSocket socket) noexcept {
  char byte = 0;
#if defined(_WIN32)
  const int n = ::recv(socket, &byte, 1, MSG_PEEK);
#else
  const ssize_t n = ::recv(socket, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
#endif
  if (n < 0 && would_block(detail::last_socket_error())) return PeerState::alive;
  return PeerState::gone;   // EOF (0), reset (<0) or a byte after the body (>0)
}

bool bytes_pending(const NativeSocket socket) noexcept {
  char byte = 0;
#if defined(_WIN32)
  return ::recv(socket, &byte, 1, MSG_PEEK) > 0;
#else
  return ::recv(socket, &byte, 1, MSG_PEEK | MSG_DONTWAIT) > 0;
#endif
}

void half_close_and_drain(const NativeSocket socket) noexcept {
#if defined(_WIN32)
  if (::shutdown(socket, SD_SEND) != 0) return;
#else
  if (::shutdown(socket, SHUT_WR) != 0) return;
#endif
  const auto deadline = Clock::now() + api_constants::kDrainBudget;
  std::array<char, 4096> sink{};
  std::size_t drained = 0;
  while (drained < api_constants::kDrainBytes) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
    if (left.count() <= 0 || wait_one(socket, true, left) == 0) return;
    const IoResult r = recv_some(socket, sink.data(), sink.size());
    if (r.status != IoStatus::done) return;
    drained += r.bytes;
  }
}

WinsockGuard::WinsockGuard() noexcept {
#if defined(_WIN32)
  WSADATA data{};
  ok_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
#endif
}

WinsockGuard::~WinsockGuard() {
#if defined(_WIN32)
  if (ok_) WSACleanup();
#endif
}

namespace detail {

IoResult send_some_with(const SendCall call, const NativeSocket socket, const std::string_view bytes) noexcept {
  const std::size_t size = std::min(bytes.size(), api_constants::kMaxSendChunk);
  for (;;) {
    const long n = call(socket, bytes.data(), size);
    if (n > 0) return {IoStatus::done, static_cast<std::size_t>(n)};
    if (n == 0) return {IoStatus::failed};
    const int code = last_socket_error();
    if (code == kInterruptedCode) continue;
    return {would_block(code) ? IoStatus::would_block : IoStatus::failed};
  }
}

int last_socket_error() noexcept {
#if defined(_WIN32)
  return WSAGetLastError();
#else
  return errno;
#endif
}

void set_last_socket_error(const int code) noexcept {
#if defined(_WIN32)
  WSASetLastError(code);
#else
  errno = code;
#endif
}

std::optional<std::chrono::milliseconds> accept_backoff_for(const int code) noexcept {
#if defined(_WIN32)
  const bool fatal = code == WSAENOTSOCK || code == WSAEINVAL || code == WSANOTINITIALISED;
#else
  const bool fatal = code == EBADF || code == ENOTSOCK || code == EINVAL;
#endif
  if (fatal) return std::nullopt;
  return api_constants::kAcceptBackoff;
}

}  // namespace detail
}  // namespace secure_frontend
```

CMake:

```cmake
secure_frontend_add_library(secure_frontend_socket_io SOURCES src/socket_io.cpp LINKS secure_frontend_api_core)
if(WIN32)
    target_link_libraries(secure_frontend_socket_io PUBLIC ws2_32)
endif()
```

- [ ] **Step 4: Run to see GREEN**

```bash
cmake --build --preset linux-gcc && ctest --preset linux-gcc -R 'http_socket_server_tests\.(send_retries_on_eintr|accept_backs_off)' --output-on-failure
ctest --preset linux-gcc | tail -1
cmake --build --preset windows-llvm-mingw-cross && ctest --preset windows-llvm-mingw-cross -R http_socket_server_tests | tail -1
```

- [ ] **Step 5: Handoff row and commit** — row `| 8a | §2.2 polled reads, §2.7 stall rule, §9.4 SP0 gaps (EINTR, transient accept), §10 | socket_io: non-blocking I/O, poll_sockets/WSAPoll, bounded send, peer probe, WinsockGuard, accept backoff | http_socket_server_tests.send_retries_on_eintr, …accept_backs_off_on_transient_failure | … | … |`.

```bash
git -C "$(git rev-parse --show-toplevel)" add secure_frontend docs/AGENT_HANDOFF.md
git -C "$(git rev-parse --show-toplevel)" -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "net: non-blocking polled socket I/O with a progress-based write stall, EINTR retry and transient-accept backoff (SP3 §2.2, §2.7, §9.4)" \
  -m "Tests: http_socket_server_tests.send_retries_on_eintr, accept_backs_off_on_transient_failure. RED: <line>. wine: <line>." \
  -m "Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>" -m "Claude-Session: <session link>"
```

---

## Task 8b: P1–P8 connection handler, the stream dispatch site, `HttpSocketServer` rebuilt

**Files:**
- Create: `include/secure_frontend/connection_handler.hpp`, `src/connection_handler.cpp`, `tests/support/raw_http.hpp`
- Modify: `include/secure_frontend/http_socket_server.hpp`, `src/http_socket_server.cpp` (rewritten), `include/secure_frontend/subsystem_limits.hpp`, `src/subsystem_limits.cpp`, `tests/subsystem_limits_test.cpp`, `tests/http_socket_server_test.cpp`, `tests/support/test_paths.hpp` (`ipv6_loopback_available`), `src/secure_server_main.cpp`, `CMakeLists.txt`

**Interfaces:**
- Consumes: `socket_io` (Task 8a); `RequestRouter`, `RouteResult`, `StreamSink`, `StreamHandler`, `HttpHead`, `kNoResponseStatus`, `RouteDecision`, `RequestContext` (Task 6a); `ApiRouter` (Task 6b, test fixture); `serialize_head` (Task 6a); `scan_request_head`, `parse_request_head`, `HeadLimits` (Task 2); `serialize_response`, `kContinueResponse`, `make_error_response` (Task 1); SP1 `Config` (listener primitive), `validate_config`, `AddressScope`; SP1 `FrontendConfig` limits incl. Task 4's deadlines.
- Produces:
  - `connection_handler.hpp`: `ConnectionLimits`, `WorkerSignals`, `ConnectionOutcome`, `ConnectionResult`, `serve_connection()` (exact shapes in "Shared interfaces"), and the **stream dispatch site** `ConnectionResult deliver_route_result(NativeSocket socket, RouteResult result, const RouteDecision& decision, const ConnectionLimits& limits, const WorkerSignals& signals)` with its socket-backed `StreamSink` (ruling R-A).
  - `HttpSocketServer`: `static std::optional<HttpSocketServer> bind(const Config& listener, int backlog, std::string* error)` (port 0 = ephemeral, tests only), `enum class AcceptStatus { accepted, timeout, transient, listener_failed, stopped }`, `struct Accepted { Socket socket; AddressScope peer_scope; std::array<unsigned char, 17> peer_key; std::chrono::steady_clock::time_point accepted_at; }`, `AcceptStatus accept_next(std::chrono::milliseconds slice, Accepted* out)`, `bool serve_one(RequestRouter&, const ConnectionLimits&, std::string* error)` (test seam and serial server), `port()`, `listener_failed()`, `shutdown_listener() noexcept` (SP2's name, kept; **idempotent** — it `exchange`s the handle with `kInvalidNativeSocket`, so the pool's call and SP2's on-stopped call may both run), `NativeSocket native_handle() const noexcept`.
  - `subsystem_limits.hpp`: `ConnectionLimits connection_limits(const FrontendConfig&)`; `HttpSocketServerLimits` and `http_socket_server_limits()` are deleted.

P1–P8 in `serve_connection` (spec §2.2): the socket is non-blocking; every wait is a `poll` of at most `kStreamPollSlice` (20 ms) so `abort`/`shutting_down` are seen within one slice. P1 reads until `scan_request_head` is `complete` (414/431 as soon as known; `408 request_timeout` at `accepted_at + header_deadline`; abort → close without a response, outcome `evicted`; shutdown → close, outcome `shutdown`; peer EOF → `client_gone`). Then `on_header_complete()` (false → evicted meanwhile → close). P2 `parse_request_head` + `router.authorize` (early → respond, drain). Bytes already received beyond `body_limit` → `400`. `100 Continue` is sent only when `expect_continue` and no body byte has arrived yet (Review Focus 3). P3 reads to `body_limit` (`408` at `accepted_at + request_deadline`). Then one non-blocking `MSG_PEEK`: any further byte → `400 invalid_request`; EOF is fine (a `GET` half-close still gets its answer). P4–P6 `router.handle` with `RequestContext{client_gone = probe_peer(socket) == gone, shutting_down}`; its `RouteResult` goes to `deliver_route_result`: a complete response with status `kNoResponseStatus` → close, outcome `client_gone`; otherwise P7 `send_all_bounded(write_stall, deadline = now + response_write_deadline, or now + kShutdownDrainBudget once shutdown has begun — ruling R-H)` and P8 `half_close_and_drain`; a `StreamHandler` runs against a `SocketStreamSink` (head via `serialize_head` plus the decision's CORS headers, one HTTP/1.1 chunk per `send_event`, terminal chunk on `finish`; a stream cut without `finish` gets no terminal chunk).

- [ ] **Step 1: Write the test client and the failing tests**

`tests/support/raw_http.hpp` (test-only; `connect()` lives only in tests and, from M2, the loopback client library):

```cpp
#pragma once
// Test-only loopback HTTP client over raw sockets. Callers on Windows hold a
// secure_frontend::WinsockGuard for the whole test process.
#include <chrono>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace raw_http {

#if defined(_WIN32)
using Native = SOCKET;
inline constexpr Native kBad = INVALID_SOCKET;
inline void close_native(Native s) { closesocket(s); }
inline int poll_one(Native s, short events, int ms) { WSAPOLLFD p{s, events, 0}; return WSAPoll(&p, 1, ms); }
#else
using Native = int;
inline constexpr Native kBad = -1;
inline void close_native(Native s) { ::close(s); }
inline int poll_one(Native s, short events, int ms) { pollfd p{s, events, 0}; return ::poll(&p, 1, ms); }
#endif

class Connection final {
 public:
  static std::optional<Connection> open(std::string_view ip, std::uint16_t port) {
    sockaddr_storage address{};
    socklen_t size = 0;
    int family = AF_INET;
    const std::string text{ip};
    auto* v4 = reinterpret_cast<sockaddr_in*>(&address);
    auto* v6 = reinterpret_cast<sockaddr_in6*>(&address);
    if (inet_pton(AF_INET, text.c_str(), &v4->sin_addr) == 1) {
      v4->sin_family = AF_INET; v4->sin_port = htons(port); size = sizeof(sockaddr_in);
    } else if (inet_pton(AF_INET6, text.c_str(), &v6->sin6_addr) == 1) {
      family = AF_INET6; v6->sin6_family = AF_INET6; v6->sin6_port = htons(port); size = sizeof(sockaddr_in6);
    } else {
      return std::nullopt;
    }
    const Native s = ::socket(family, SOCK_STREAM, IPPROTO_TCP);
    if (s == kBad) return std::nullopt;
    if (::connect(s, reinterpret_cast<const sockaddr*>(&address), size) != 0) { close_native(s); return std::nullopt; }
    const int one = 1;
    (void)setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
    return Connection{s};
  }
  Connection(Connection&& other) noexcept : s_(std::exchange(other.s_, kBad)) {}
  Connection& operator=(Connection&& other) noexcept { if (this != &other) { close(); s_ = std::exchange(other.s_, kBad); } return *this; }
  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;
  ~Connection() { close(); }

  bool send(std::string_view bytes) {
    while (!bytes.empty()) {
#if defined(_WIN32)
      const int n = ::send(s_, bytes.data(), static_cast<int>(std::min<std::size_t>(bytes.size(), 1U << 20)), 0);
#else
      const ssize_t n = ::send(s_, bytes.data(), bytes.size(), MSG_NOSIGNAL);
#endif
      if (n <= 0) return false;
      bytes.remove_prefix(static_cast<std::size_t>(n));
    }
    return true;
  }
  bool shutdown_send() {
#if defined(_WIN32)
    return ::shutdown(s_, SD_SEND) == 0;
#else
    return ::shutdown(s_, SHUT_WR) == 0;
#endif
  }
  // Everything that arrives until EOF/reset or `timeout`; *closed says which.
  std::string read_until_close(std::chrono::milliseconds timeout, bool* closed = nullptr) {
    std::string out;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    if (closed != nullptr) *closed = false;
    char buffer[65536];
    while (std::chrono::steady_clock::now() < deadline) {
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
      if (poll_one(s_, POLLIN, static_cast<int>(std::max<long long>(1, left.count()))) <= 0) continue;
      const auto n = ::recv(s_, buffer, sizeof(buffer), 0);
      if (n <= 0) { if (closed != nullptr) *closed = true; break; }
      out.append(buffer, static_cast<std::size_t>(n));
    }
    return out;
  }
  // Whatever arrives within `window`, without waiting for EOF.
  std::string read_for(std::chrono::milliseconds window) {
    std::string out;
    const auto deadline = std::chrono::steady_clock::now() + window;
    char buffer[65536];
    while (std::chrono::steady_clock::now() < deadline) {
      if (poll_one(s_, POLLIN, 10) <= 0) continue;
      const auto n = ::recv(s_, buffer, sizeof(buffer), 0);
      if (n <= 0) break;
      out.append(buffer, static_cast<std::size_t>(n));
    }
    return out;
  }
  bool wait_closed(std::chrono::milliseconds timeout) {
    bool closed = false;
    (void)read_until_close(timeout, &closed);
    return closed;
  }
  void close() { if (s_ != kBad) { close_native(s_); s_ = kBad; } }
  [[nodiscard]] Native native() const { return s_; }

 private:
  explicit Connection(Native s) : s_(s) {}
  Native s_{kBad};
};

struct Response final {
  int status{0};
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
  bool saw_continue{false};
  [[nodiscard]] std::optional<std::string> header(std::string_view name) const {
    for (const auto& [n, v] : headers) {
      if (n.size() == name.size() && std::equal(n.begin(), n.end(), name.begin(), [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
          })) {
        return v;
      }
    }
    return std::nullopt;
  }
};

// Parses one complete response; an interim "100 Continue" block is skipped and recorded.
inline std::optional<Response> parse_response(std::string_view raw) {
  Response response;
  if (raw.rfind("HTTP/1.1 100 ", 0) == 0) {
    const auto end = raw.find("\r\n\r\n");
    if (end == std::string_view::npos) return std::nullopt;
    raw.remove_prefix(end + 4U);
    response.saw_continue = true;
  }
  const auto head_end = raw.find("\r\n\r\n");
  if (raw.size() < 12U || raw.rfind("HTTP/1.1 ", 0) != 0 || head_end == std::string_view::npos) return std::nullopt;
  response.status = std::atoi(std::string{raw.substr(9U, 3U)}.c_str());
  std::size_t start = raw.find("\r\n") + 2U;
  while (start < head_end) {
    const auto end = raw.find("\r\n", start);
    const auto line = raw.substr(start, end - start);
    const auto colon = line.find(':');
    if (colon != std::string_view::npos) {
      std::string_view value = line.substr(colon + 1U);
      while (!value.empty() && value.front() == ' ') value.remove_prefix(1U);
      response.headers.emplace_back(std::string{line.substr(0, colon)}, std::string{value});
    }
    start = end + 2U;
  }
  response.body = std::string{raw.substr(head_end + 4U)};
  if (const auto length = response.header("Content-Length")) {
    const auto expected = static_cast<std::size_t>(std::stoull(*length));
    if (response.body.size() < expected) return std::nullopt;
    response.body.resize(expected);
  }
  return response;
}

inline std::string request(std::string_view method, std::string_view target, std::string_view host,
                           const std::vector<std::string>& header_lines, std::string_view body = {}) {
  std::string out = std::string{method} + " " + std::string{target} + " HTTP/1.1\r\nHost: " + std::string{host} + "\r\n";
  for (const auto& line : header_lines) out += line + "\r\n";
  if (!body.empty() || method == "POST") out += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  out += "\r\n";
  out += body;
  return out;
}

inline std::optional<Response> exchange(std::string_view ip, std::uint16_t port, std::string_view raw_request,
                                        std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
  auto connection = Connection::open(ip, port);
  if (!connection || !connection->send(raw_request)) return std::nullopt;
  return parse_response(connection->read_until_close(timeout));
}

inline std::string bearer(std::string_view token) { return "Authorization: Bearer " + std::string{token}; }

}  // namespace raw_http
```

(`#include <algorithm>`, `<cctype>`, `<cstdlib>` at the top as well.)

`tests/http_socket_server_test.cpp` — port the existing cases to the new API and add the new ones. Replace the file's `Fixture` with:

```cpp
constexpr std::string_view kSocketToken = "socket-token-0123456789abcdefghijklmnopqr";

secure_frontend::ConnectionLimits fast_limits() {
  return {.head = {4096, 16384, 64, 65536}, .header_deadline = std::chrono::milliseconds(1000),
          .request_deadline = std::chrono::milliseconds(2000), .write_stall = std::chrono::milliseconds(500),
          .response_write_deadline = std::chrono::milliseconds(5000)};
}

struct Fixture final {
  std::optional<secure_frontend::HttpSocketServer> server;
  std::unique_ptr<secure_frontend::ApiRouter> router;
  std::string error;
  std::string host;
  std::uint16_t port{0};
  std::string big_body;   // returned by the chat handler when set (write-stall tests)
  bool stream{false};     // the chat handler returns a StreamHandler (dispatch-site test)
  explicit Fixture(std::string bind_host = "127.0.0.1") : host(std::move(bind_host)) {
    secure_frontend::Config listener{};
    listener.host = host; listener.port = 0; listener.allow_remote = false; listener.allow_wildcard_bind = false;
    listener.has_api_key = true;
    server = secure_frontend::HttpSocketServer::bind(listener, 16, &error);
    if (!server) return;
    port = server->port();
    const std::string authority = (host.find(':') != std::string::npos ? "[" + host + "]" : host) + ":" + std::to_string(port);
    secure_frontend::SecretBytes secret;
    (void)secret.assign(kSocketToken);
    router = std::make_unique<secure_frontend::ApiRouter>(secure_frontend::ApiRouterConfig{
        .policy = secure_frontend::make_http_policy(*secure_frontend::parse_listener_address(host), port, {}, {}),
        .validator = secure_frontend::make_bearer_validator(std::make_shared<const secure_frontend::TokenVerifier>(std::move(secret))),
        .model_id = "fake-model", .owned_by = "local", .chat_body_limit = 49152,
        .health = [] { return secure_frontend::HealthView{true, "ready", ""}; },
        .chat = [this](std::string_view body, const secure_frontend::RequestContext&) -> secure_frontend::RouteResult {
          if (stream) {
            return secure_frontend::StreamHandler{[](secure_frontend::StreamSink& sink) {
              (void)sink.send_headers({200, {{"Content-Type", "text/event-stream; charset=utf-8"}, {"Transfer-Encoding", "chunked"}}});
              (void)sink.send_event("data: a\n\n");
              (void)sink.send_event("data: [DONE]\n\n");
              sink.finish();
            }};
          }
          return secure_frontend::HttpResponse{.status = 200, .headers = {{"Content-Type", "application/json; charset=utf-8"}},
                                               .body = big_body.empty() ? "{\"bytes\":" + std::to_string(body.size()) + "}\n" : big_body};
        }});
  }
  [[nodiscard]] std::string authority() const {
    return (host.find(':') != std::string::npos ? "[" + host + "]" : host) + ":" + std::to_string(port);
  }
  // Serves exactly one connection on a background thread.
  std::thread serve_once(bool* result = nullptr) {
    return std::thread([this, result] {
      std::string e;
      const bool ok = server->serve_one(*router, fast_limits(), &e);
      if (result != nullptr) *result = ok;
    });
  }
};
```

(`secure_frontend::Config` field names are SP1's listener primitive; if SP1 renamed them, use its names. The `Config` value with port 0 is accepted by `bind` only — the schema range stays 1–65535.) Every case's `main` holds `secure_frontend::WinsockGuard winsock;` before `run_cases`.

**Port of the existing `http_socket_server_tests` cases (ruling R-G).** Every existing case keeps its name and registration. "Mechanical" means: replace the fixed-port bind loop with `Fixture f;` (or `Fixture f{"::1"}`), the old `bind(...)` arguments with nothing, `serve_one(&error)` with `f.server->serve_one(*f.router, fast_limits(), &error)` (or `f.serve_once(&served)` where the old case served on a thread), and any `text/plain` body assertion with `secure_frontend::render_error_body(<code>)` for the same status.

| Case | Change |
| --- | --- |
| `serves_a_loopback_request_without_outbound_connections` | mechanical; the request now carries `Authorization: Bearer <kSocketToken>` and `Host: f.authority()` |
| `socket_server_survives_client_early_close` | mechanical; `serve_one` returns `false` for the client that closed before a complete header |
| `socket_server_rejects_slow_request_past_deadline` | mechanical; expects `408` with `render_error_body(ApiError::request_timeout)` at the **1 s header deadline** of `fast_limits()` (was the 30 s request deadline) |
| `socket_server_send_times_out_on_stalled_reader` | **behavior change — full code below** (stall rule replaces `kMinimumSendBudget`) |
| `socket_server_rejects_oversize_request_over_tcp` | mechanical; `Content-Length: 70000` with the body bytes sent → `413`, body `render_error_body(ApiError::request_too_large)` (decided at parse time, before authentication) |
| `socket_server_accept_waits_without_timeout` | **behavior change — full code below** (the listener is polled in 100 ms slices instead of a blocking `accept`) |
| `socket_server_ipv6_bind_is_v6only` | **behavior change — full code below** (ephemeral port; skip without IPv6 loopback) |
| `socket_server_response_is_complete` | mechanical; the large body comes from `f.big_body` (set it to the old case's size) through the chat route |
| `socket_server_survives_reset_before_accept` | mechanical |
| `socket_server_responds_after_client_half_close` | **must stay green for `GET` (spec §2.6, §9.4) — full code below** |
| `parser_honors_configured_max_request_bytes` | mechanical; uses `fast_limits()` with `head.max_request_bytes` lowered to the old case's value |

Full code of the four behavior-changing cases (replace their bodies):

```cpp
void socket_server_send_times_out_on_stalled_reader() {
  Fixture f;
  f.big_body = std::string(32U * 1024U * 1024U, 'x');   // far more than any socket buffer
  bool served = true;
  auto worker = f.serve_once(&served);
  auto client = raw_http::Connection::open(f.host, f.port);
  expect(client && client->send(chat_request_for(f, "{}")), "request sent; the client then never reads");
  const auto started = std::chrono::steady_clock::now();
  worker.join();
  expect(!served, "a reader that never reads makes the send fail");
  expect(std::chrono::steady_clock::now() - started < std::chrono::seconds(3),
         "at the 500 ms write stall on progress, not the 5 s response deadline");
}

void socket_server_accept_waits_without_timeout() {
  Fixture f;
  std::atomic<bool> returned{false};
  bool served = false;
  std::thread worker([&] {
    std::string e;
    served = f.server->serve_one(*f.router, fast_limits(), &e);
    returned = true;
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));   // many 100 ms accept slices
  expect(!returned.load(), "serve_one keeps waiting for a client across poll slices");
  const auto response = raw_http::exchange(f.host, f.port,
      raw_http::request("GET", "/health", f.authority(), {raw_http::bearer(kSocketToken)}));
  worker.join();
  expect(served && response && response->status == 200, "the late client is served");
}

void socket_server_ipv6_bind_is_v6only() {
  if (!test_support::ipv6_loopback_available()) test_support::skip("no IPv6 loopback on this host");
  Fixture f{"::1"};
  expect(f.server.has_value(), "binds ::1: " + f.error);
  if (!f.server) return;
  expect(!raw_http::Connection::open("127.0.0.1", f.port).has_value(), "an IPv6 listener serves IPv6 only");
  auto worker = f.serve_once();
  const auto response = raw_http::exchange("::1", f.port,
      raw_http::request("GET", "/health", f.authority(), {raw_http::bearer(kSocketToken)}));
  worker.join();
  expect(response && response->status == 200, "[::1]:port is served");
}

void socket_server_responds_after_client_half_close() {
  Fixture f;
  bool served = false;
  auto worker = f.serve_once(&served);
  auto client = raw_http::Connection::open(f.host, f.port);
  expect(client && client->send(raw_http::request("GET", "/health", f.authority(), {raw_http::bearer(kSocketToken)})), "GET sent");
  expect(client && client->shutdown_send(), "client half-closes after its GET");
  const auto response = raw_http::parse_response(client ? client->read_until_close(std::chrono::seconds(5)) : "");
  worker.join();
  expect(served && response && response->status == 200 && response->body == "{\"status\":\"ok\",\"engine\":\"ready\"}\n",
         "a GET whose client half-closed still gets its answer (EOF is not a stray byte)");
}
```

with the helper (top of the file)

```cpp
std::string chat_request_for(const Fixture& f, const std::string& body) {
  return raw_http::request("POST", "/v1/chat/completions", f.authority(),
                           {raw_http::bearer(kSocketToken), "Content-Type: application/json"}, body);
}
```

and, in `tests/support/test_paths.hpp`, `ipv6_loopback_available()` (used again in Tasks 10 and 12):

```cpp
// True when a TCP socket can bind ::1 (port 0). Uses only bind, never connect.
inline bool ipv6_loopback_available() {
#if defined(_WIN32)
  const SOCKET s = ::socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET) return false;
#else
  const int s = ::socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
  if (s < 0) return false;
#endif
  sockaddr_in6 address{};
  address.sin6_family = AF_INET6;
  address.sin6_addr = in6addr_loopback;
  const bool ok = ::bind(s, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
#if defined(_WIN32)
  closesocket(s);
#else
  ::close(s);
#endif
  return ok;
}
```

(`<ws2tcpip.h>` on Windows; `<netinet/in.h>`, `<sys/socket.h>`, `<unistd.h>` on POSIX.) Register `set_tests_properties(secure_frontend_http_socket_server_tests.socket_server_ipv6_bind_is_v6only PROPERTIES SKIP_RETURN_CODE 77)`.

The stream dispatch site gets its own case (registered with the others):

```cpp
void stream_handler_dispatch_writes_head_chunks_and_terminal_chunk() {
  Fixture f;
  f.stream = true;
  bool served = false;
  auto worker = f.serve_once(&served);
  auto client = raw_http::Connection::open(f.host, f.port);
  expect(client && client->send(chat_request_for(f, "{}")), "request sent");
  const std::string raw = client ? client->read_until_close(std::chrono::seconds(5)) : "";
  worker.join();
  expect(served && raw.rfind("HTTP/1.1 200 OK\r\n", 0) == 0, "the stream head is written by deliver_route_result");
  const auto head_end = raw.find("\r\n\r\n");
  const std::string head = raw.substr(0, head_end);
  expect(head.find("Content-Length") == std::string::npos && head.find("Transfer-Encoding: chunked") != std::string::npos &&
         head.find("X-Content-Type-Options: nosniff") != std::string::npos, "chunked head without Content-Length, security headers kept");
  expect(raw.substr(head_end + 4U) == "9\r\ndata: a\n\n\r\ne\r\ndata: [DONE]\n\n\r\n0\r\n\r\n",
         "one chunk per event, then the terminal chunk");
}
```

New cases:

```cpp
void socket_server_rejects_oversize_header_with_431() {
  Fixture f;
  bool served = true;
  auto worker = f.serve_once(&served);
  auto client = raw_http::Connection::open(f.host, f.port);
  std::string head = "GET /health HTTP/1.1\r\nHost: " + f.authority() + "\r\n";
  while (head.size() < 17000U) head += "X-Fill: " + std::string(200, 'f') + "\r\n";
  expect(client && client->send(head), "send an over-long header without its blank line");
  const auto response = raw_http::parse_response(client ? client->read_until_close(std::chrono::seconds(5)) : "");
  worker.join();
  expect(response && response->status == 431 && response->body == secure_frontend::render_error_body(secure_frontend::ApiError::header_too_large),
         "431 header_too_large before the blank line");
}

void head_terminator_split_across_reads() {
  Fixture f;
  auto worker = f.serve_once();
  auto client = raw_http::Connection::open(f.host, f.port);
  const std::string request = "GET /health HTTP/1.1\r\nHost: " + f.authority() + "\r\n" + raw_http::bearer(kSocketToken) + "\r\n\r\n";
  bool sent = client.has_value();
  for (std::size_t i = 0; sent && i < request.size(); ++i) {   // one byte per segment, \r\n | \r\n split too
    sent = client->send(std::string_view{request}.substr(i, 1U));
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  const auto response = raw_http::parse_response(client ? client->read_until_close(std::chrono::seconds(5)) : "");
  worker.join();
  expect(response && response->status == 200, "a header delivered byte by byte parses exactly like one segment");
}

void expect_continue_skipped_when_body_already_sent() {
  Fixture f;
  auto worker = f.serve_once();
  const std::string body = R"({"x":1})";
  const std::string request = "POST /v1/chat/completions HTTP/1.1\r\nHost: " + f.authority() + "\r\n" + raw_http::bearer(kSocketToken) +
                              "\r\nContent-Type: application/json\r\nExpect: 100-continue\r\nContent-Length: " +
                              std::to_string(body.size()) + "\r\n\r\n" + body;
  auto client = raw_http::Connection::open(f.host, f.port);
  expect(client && client->send(request), "header and body in one write");
  const std::string raw = client ? client->read_until_close(std::chrono::seconds(5)) : "";
  worker.join();
  expect(raw.find("100 Continue") == std::string::npos, "no interim 100 after the body has arrived");
  const auto response = raw_http::parse_response(raw);
  expect(response && response->status == 200, "exactly one final response");
}

void expect_continue_sent_after_authorization() {
  Fixture f;
  auto worker = f.serve_once();
  const std::string body = R"({"x":1})";
  auto client = raw_http::Connection::open(f.host, f.port);
  const std::string head = "POST /v1/chat/completions HTTP/1.1\r\nHost: " + f.authority() + "\r\n" + raw_http::bearer(kSocketToken) +
                           "\r\nContent-Type: application/json\r\nExpect: 100-continue\r\nContent-Length: " +
                           std::to_string(body.size()) + "\r\n\r\n";
  expect(client && client->send(head), "header only");
  const std::string interim = client ? client->read_for(std::chrono::milliseconds(300)) : "";
  expect(interim == "HTTP/1.1 100 Continue\r\n\r\n", "100 Continue once the header passed every check");
  expect(client && client->send(body), "then the body");
  const auto response = raw_http::parse_response(client ? client->read_until_close(std::chrono::seconds(5)) : "");
  worker.join();
  expect(response && response->status == 200, "final response after the body");
}

void win_send_chunks_respect_write_stall() {
  Fixture f;
  f.big_body = std::string(32U * 1024U * 1024U, 'x');
  bool served = true;
  auto worker = f.serve_once(&served);
  auto client = raw_http::Connection::open(f.host, f.port);
  const std::string request = "POST /v1/chat/completions HTTP/1.1\r\nHost: " + f.authority() + "\r\n" + raw_http::bearer(kSocketToken) +
                              "\r\nContent-Type: application/json\r\nContent-Length: 2\r\n\r\n{}";
  expect(client && client->send(request), "request sent; the client then never reads");
  const auto started = std::chrono::steady_clock::now();
  worker.join();
  const auto elapsed = std::chrono::steady_clock::now() - started;
  expect(!served, "a 32 MiB response to a reader that never reads fails");
  expect(elapsed < std::chrono::seconds(3), "it ends at the 500 ms write stall, not the 5 s response deadline");
}
```

Register in the `secure_frontend_add_cases(secure_frontend_http_socket_server_tests` call: add `socket_server_rejects_oversize_header_with_431 head_terminator_split_across_reads expect_continue_skipped_when_body_already_sent expect_continue_sent_after_authorization stream_handler_dispatch_writes_head_chunks_and_terminal_chunk` (the ported cases keep their existing registrations), and add a separate registration for the W case:

```cmake
    if(WIN32)
        secure_frontend_add_cases(secure_frontend_http_socket_server_tests win_send_chunks_respect_write_stall
            LABELS ${SECURE_FRONTEND_WINDOWS_RUNTIME} TIMEOUT 120)
    endif()
```

Link the test with `secure_frontend_api_router secure_frontend_secrets secure_frontend_http_policy secure_frontend_socket_io`.

- [ ] **Step 2: Run to see RED**

Run: `cmake --build --preset linux-gcc 2>&1 | grep -m1 error`
Expected: FAIL `secure_frontend/connection_handler.hpp: No such file or directory` (from `fast_limits()`'s `ConnectionLimits`).

- [ ] **Step 3: Implement**

`include/secure_frontend/connection_handler.hpp` declares `ConnectionLimits`, `WorkerSignals`, `ConnectionOutcome`, `ConnectionResult` and `serve_connection` exactly as in "Shared interfaces", plus the stream dispatch site (includes `request_router.hpp`, `socket_io.hpp`, `<atomic>`, `<functional>`):

```cpp
// Writes a RouteResult to the client (spec §2.2 P7-P8, §3.11): a complete response
// with Content-Length (kNoResponseStatus = close without writing), or a
// StreamHandler run against a socket-backed StreamSink (HTTP/1.1 chunks; the
// decision's CORS headers are added to the stream head). M2 produces the handlers.
[[nodiscard]] ConnectionResult deliver_route_result(NativeSocket socket, RouteResult result, const RouteDecision& decision,
                                                    const ConnectionLimits& limits, const WorkerSignals& signals);
```

**`SocketStreamSink` is an M1 placeholder** (coordinator ruling): a minimal blocking implementation used only by `stream_handler_dispatch_writes_head_chunks_and_terminal_chunk`; M2 Task 10 replaces it with the non-blocking `ChunkedStreamSink` (spec §2.7 — a slow reader must never stall the engine-reading loop) and adds `virtual void set_log_code(std::string_view)` to `StreamSink` plus `bool streamed{false}` on `ConnectionResult`/`RequestLogRecord`. Implementers must not add buffering or stall accounting to the placeholder.

`src/connection_handler.cpp`:

```cpp
#include "secure_frontend/connection_handler.hpp"

#include "secure_frontend/api_constants.hpp"
#include "secure_frontend/api_errors.hpp"
#include "secure_frontend/http_wire.hpp"

#include <array>
#include <cstdio>
#include <optional>

namespace secure_frontend {
namespace {
using Clock = std::chrono::steady_clock;

bool stopping(const WorkerSignals& signals) { return signals.shutting_down != nullptr && signals.shutting_down->load(); }

// Ruling R-H: once shutdown has begun, a write may take at most kShutdownDrainBudget.
Clock::time_point write_deadline(const ConnectionLimits& limits, const WorkerSignals& signals) {
  return Clock::now() + (stopping(signals) ? api_constants::kShutdownDrainBudget : limits.response_write_deadline);
}

// One 20 ms slice: poll, then a non-blocking recv when readable.
IoResult read_slice(NativeSocket socket, char* buffer, std::size_t size) {
  PollEntry entry{socket, true, false};
  const int ready = poll_sockets(std::span<PollEntry>{&entry, 1U}, api_constants::kStreamPollSlice);
  if (ready < 0) return {IoStatus::failed};
  if (ready == 0) return {IoStatus::would_block};
  return recv_some(socket, buffer, size);
}

ConnectionResult respond_complete(const NativeSocket socket, const HttpResponse& response, const ConnectionLimits& limits,
                                  const WorkerSignals& signals, const ConnectionOutcome outcome, ConnectionResult result) {
  result.status = response.status;
  result.code = response.log_code;
  const std::string wire = serialize_response(response, std::chrono::system_clock::now());
  const WriteOutcome written = send_all_bounded(socket, wire, limits.write_stall, write_deadline(limits, signals));
  if (written == WriteOutcome::complete) {
    result.bytes_out = wire.size();
    result.outcome = outcome;
    half_close_and_drain(socket);
  } else {
    result.outcome = written == WriteOutcome::stalled ? ConnectionOutcome::client_gone : ConnectionOutcome::transport_error;
  }
  return result;
}

// M1 placeholder: minimal blocking implementation used only by
// stream_handler_dispatch_writes_head_chunks_and_terminal_chunk. M2 Task 10 replaces it with the
// non-blocking ChunkedStreamSink (spec §2.7: a slow reader must never stall the engine-reading loop)
// and adds `virtual void set_log_code(std::string_view)` to StreamSink plus `bool streamed{false}` on
// ConnectionResult/RequestLogRecord. Do not add buffering or stall accounting here.
class SocketStreamSink final : public StreamSink {
 public:
  SocketStreamSink(NativeSocket socket, const ConnectionLimits& limits, const WorkerSignals& signals, std::optional<std::string> allow_origin)
      : socket_(socket), limits_(limits), signals_(signals), allow_origin_(std::move(allow_origin)) {}
  bool send_headers(const HttpHead& head) override {
    if (headers_sent_ || failed_) return false;
    HttpHead with_cors = head;
    if (allow_origin_.has_value()) {
      with_cors.headers.emplace_back("Access-Control-Allow-Origin", *allow_origin_);
      with_cors.headers.emplace_back("Vary", "Origin");
    }
    status_ = head.status;
    headers_sent_ = true;
    return write(serialize_head(with_cors, std::chrono::system_clock::now()));
  }
  bool send_event(const std::string_view bytes) override {
    if (!headers_sent_ || failed_ || finished_) return false;
    if (bytes.empty()) return true;   // a zero-size chunk would end the body
    char size[24];
    const int n = std::snprintf(size, sizeof(size), "%zx\r\n", bytes.size());
    std::string chunk(size, static_cast<std::size_t>(n));
    chunk.append(bytes);
    chunk.append("\r\n");
    return write(chunk);
  }
  bool writable(const std::chrono::milliseconds timeout) override {
    PollEntry entry{socket_, false, true};
    return !failed_ && poll_sockets(std::span<PollEntry>{&entry, 1U}, timeout) > 0 && entry.writable && !entry.failed;
  }
  bool client_gone() override { return failed_ || probe_peer(socket_) == PeerState::gone; }
  void finish() override {
    if (headers_sent_ && !failed_ && !finished_) finished_ = write("0\r\n\r\n");
  }
  [[nodiscard]] int status() const noexcept { return status_; }
  [[nodiscard]] std::size_t bytes() const noexcept { return bytes_; }
  [[nodiscard]] bool failed() const noexcept { return failed_; }
  [[nodiscard]] bool finished() const noexcept { return finished_; }

 private:
  bool write(const std::string_view bytes) {
    const WriteOutcome outcome = send_all_bounded(socket_, bytes, limits_.write_stall, write_deadline(limits_, signals_));
    if (outcome != WriteOutcome::complete) { failed_ = true; return false; }
    bytes_ += bytes.size();
    return true;
  }
  NativeSocket socket_;
  const ConnectionLimits& limits_;
  const WorkerSignals& signals_;
  std::optional<std::string> allow_origin_;
  int status_{0};
  std::size_t bytes_{0};
  bool headers_sent_{false};
  bool failed_{false};
  bool finished_{false};
};

}  // namespace

ConnectionResult deliver_route_result(const NativeSocket socket, RouteResult result, const RouteDecision& decision,
                                      const ConnectionLimits& limits, const WorkerSignals& signals) {
  ConnectionResult connection{ConnectionOutcome::responded};
  connection.route = decision.route;
  if (auto* complete = std::get_if<HttpResponse>(&result)) {
    if (complete->status == kNoResponseStatus) {
      connection.status = 0;
      connection.code = complete->log_code;
      connection.outcome = ConnectionOutcome::client_gone;
      return connection;
    }
    return respond_complete(socket, *complete, limits, signals, ConnectionOutcome::responded, connection);
  }
  SocketStreamSink sink(socket, limits, signals, decision.allow_origin);
  try {
    std::get<StreamHandler>(result)(sink);
  } catch (...) {
    // A throwing handler before its head becomes a fixed 500; after it, the stream is cut (no terminal chunk).
    if (sink.status() == 0) return respond_complete(socket, make_error_response(ApiError::internal_error), limits, signals,
                                                    ConnectionOutcome::responded, connection);
  }
  connection.status = sink.status();
  connection.bytes_out = sink.bytes();
  if (sink.finished()) {
    half_close_and_drain(socket);
    connection.outcome = ConnectionOutcome::responded;
  } else {
    connection.outcome = sink.failed() ? ConnectionOutcome::client_gone : ConnectionOutcome::transport_error;
  }
  return connection;
}

ConnectionResult serve_connection(const NativeSocket socket, RequestRouter& router, const ConnectionLimits& limits,
                                  const WorkerSignals& signals, const Clock::time_point accepted_at) {
  ConnectionResult result{ConnectionOutcome::responded};
  const auto evicted = [&signals] { return signals.abort != nullptr && signals.abort->load(); };
  const auto respond = [&](const HttpResponse& response, const ConnectionOutcome outcome) {
    return respond_complete(socket, response, limits, signals, outcome, result);
  };

  // P1: header.
  std::string buffer;
  std::array<char, api_constants::kRecvChunk> chunk{};
  HeadScanResult scan{HeadScan::need_more};
  for (;;) {
    scan = scan_request_head(buffer, limits.head);
    if (scan.state == HeadScan::line_too_long) return respond(make_error_response(ApiError::request_line_too_long), ConnectionOutcome::responded);
    if (scan.state == HeadScan::header_too_large) return respond(make_error_response(ApiError::header_too_large), ConnectionOutcome::responded);
    if (scan.state == HeadScan::complete) break;
    if (evicted()) { result.outcome = ConnectionOutcome::evicted; return result; }
    if (stopping(signals)) { result.outcome = ConnectionOutcome::shutdown; return result; }
    if (Clock::now() >= accepted_at + limits.header_deadline) {
      return respond(make_error_response(ApiError::request_timeout), ConnectionOutcome::timed_out);
    }
    const IoResult r = read_slice(socket, chunk.data(), chunk.size());
    if (r.status == IoStatus::closed || r.status == IoStatus::failed) { result.outcome = ConnectionOutcome::client_gone; return result; }
    if (r.status == IoStatus::done) buffer.append(chunk.data(), r.bytes);
  }
  if (signals.on_header_complete && !signals.on_header_complete()) { result.outcome = ConnectionOutcome::evicted; return result; }

  // P2: parse and authorize (CPU only).
  const HeadParseResult parsed = parse_request_head(std::string_view{buffer}.substr(0, scan.head_bytes), limits.head);
  if (const auto* error = std::get_if<ApiError>(&parsed)) return respond(make_error_response(*error), ConnectionOutcome::responded);
  const RequestHead& head = std::get<RequestHead>(parsed);
  const RouteDecision decision = router.authorize(head);
  result.route = decision.route;
  if (decision.early.has_value()) return respond(*decision.early, ConnectionOutcome::responded);
  std::string body = buffer.substr(scan.head_bytes);
  if (body.size() > decision.body_limit) return respond(make_error_response(ApiError::invalid_request), ConnectionOutcome::responded);
  if (decision.expect_continue && decision.body_limit > 0U && body.empty()) {
    if (send_all_bounded(socket, kContinueResponse, limits.write_stall, write_deadline(limits, signals)) != WriteOutcome::complete) {
      result.outcome = ConnectionOutcome::client_gone;
      return result;
    }
  }

  // P3: body (authenticated clients only).
  while (body.size() < decision.body_limit) {
    if (stopping(signals)) { result.outcome = ConnectionOutcome::shutdown; return result; }
    if (Clock::now() >= accepted_at + limits.request_deadline) {
      return respond(make_error_response(ApiError::request_timeout), ConnectionOutcome::timed_out);
    }
    const IoResult r = read_slice(socket, chunk.data(), std::min(chunk.size(), decision.body_limit - body.size() + 1U));
    if (r.status == IoStatus::closed || r.status == IoStatus::failed) { result.outcome = ConnectionOutcome::client_gone; return result; }
    if (r.status == IoStatus::done) body.append(chunk.data(), r.bytes);
    if (body.size() > decision.body_limit) return respond(make_error_response(ApiError::invalid_request), ConnectionOutcome::responded);
  }
  // Bytes that arrived with the request beyond Content-Length: 400. EOF is not an error
  // (a GET half-close still gets its answer: socket_server_responds_after_client_half_close).
  if (bytes_pending(socket)) return respond(make_error_response(ApiError::invalid_request), ConnectionOutcome::responded);

  // P4-P6: route; P7-P8: deliver.
  const RequestContext context{.client_gone = [socket] { return probe_peer(socket) == PeerState::gone; },
                               .shutting_down = signals.shutting_down};
  return deliver_route_result(socket, router.handle(head, body, decision, context), decision, limits, signals);
}

}  // namespace secure_frontend
```

`include/secure_frontend/http_socket_server.hpp` (rewritten):

```cpp
#pragma once

#include "secure_frontend/config.hpp"
#include "secure_frontend/connection_handler.hpp"
#include "secure_frontend/socket_io.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace secure_frontend {

// The listening socket. It only accepts TCP clients; it never creates
// outbound sockets or resolves names (spec §6.1). Networking is initialized
// by the process's WinsockGuard, not here.
class HttpSocketServer final {
 public:
  // port 0 binds an ephemeral port (tests only; the schema range is 1-65535).
  [[nodiscard]] static std::optional<HttpSocketServer> bind(const Config& listener, int backlog, std::string* error);
  HttpSocketServer(HttpSocketServer&&) noexcept;
  HttpSocketServer& operator=(HttpSocketServer&&) noexcept;
  ~HttpSocketServer();

  enum class AcceptStatus { accepted, timeout, transient, listener_failed, stopped };
  struct Accepted final {
    Socket socket;
    AddressScope peer_scope{AddressScope::remote};
    std::array<unsigned char, 17> peer_key{};   // family byte + 16 address bytes (IPv4-mapped folded to v4)
    std::chrono::steady_clock::time_point accepted_at{};
  };
  // Waits at most `slice` for a client; a transient failure sleeps the 50 ms backoff first.
  [[nodiscard]] AcceptStatus accept_next(std::chrono::milliseconds slice, Accepted* out);
  // Accepts one client (polling in kAcceptPollSlice) and serves it with serve_connection.
  [[nodiscard]] bool serve_one(RequestRouter& router, const ConnectionLimits& limits, std::string* error);
  [[nodiscard]] std::uint16_t port() const;
  [[nodiscard]] bool listener_failed() const noexcept;
  // Closes the listener; accept_next/serve_one return `stopped` afterwards. Thread-safe.
  void shutdown_listener() noexcept;
  [[nodiscard]] NativeSocket native_handle() const noexcept;

 private:
  struct State;
  explicit HttpSocketServer(std::unique_ptr<State> state);
  std::unique_ptr<State> state_;
};

}  // namespace secure_frontend
```

`src/http_socket_server.cpp` (rewritten; the SP0 bind body is kept — numeric address, `IPV6_V6ONLY`, `SO_REUSEADDR`/`SO_EXCLUSIVEADDRUSE`, re-validation — minus `WSAStartup`, plus port 0 and non-blocking accepted sockets):

```cpp
#include "secure_frontend/http_socket_server.hpp"

#include "secure_frontend/api_constants.hpp"

#include <atomic>
#include <cstring>
#include <thread>
#include <utility>
#include <variant>

#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace secure_frontend {
namespace {

#if defined(_WIN32)
using AddressLength = int;
#else
using AddressLength = socklen_t;
#endif

void classify_peer(const sockaddr_storage& address, HttpSocketServer::Accepted* out) {
  out->peer_key.fill(0);
  if (address.ss_family == AF_INET) {
    const auto* v4 = reinterpret_cast<const sockaddr_in*>(&address);
    unsigned char bytes[4];
    std::memcpy(bytes, &v4->sin_addr, 4U);
    out->peer_key[0] = 4;
    std::memcpy(out->peer_key.data() + 1, bytes, 4U);
    out->peer_scope = bytes[0] == 127U ? AddressScope::loopback : AddressScope::remote;
    return;
  }
  const auto* v6 = reinterpret_cast<const sockaddr_in6*>(&address);
  unsigned char bytes[16];
  std::memcpy(bytes, &v6->sin6_addr, 16U);
  static constexpr unsigned char kMapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
  static constexpr unsigned char kLoopback[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
  if (std::memcmp(bytes, kMapped, 12U) == 0) {   // ::ffff:a.b.c.d is the IPv4 peer a.b.c.d
    out->peer_key[0] = 4;
    std::memcpy(out->peer_key.data() + 1, bytes + 12, 4U);
    out->peer_scope = bytes[12] == 127U ? AddressScope::loopback : AddressScope::remote;
    return;
  }
  out->peer_key[0] = 6;
  std::memcpy(out->peer_key.data() + 1, bytes, 16U);
  out->peer_scope = std::memcmp(bytes, kLoopback, 16U) == 0 ? AddressScope::loopback : AddressScope::remote;
}

}  // namespace

struct HttpSocketServer::State final {
  std::atomic<NativeSocket> socket{kInvalidNativeSocket};
  std::uint16_t port{0};
  std::atomic<bool> listener_failed{false};
  std::atomic<bool> stopped{false};
  ~State() { close_native_socket(socket.exchange(kInvalidNativeSocket)); }
};

HttpSocketServer::HttpSocketServer(std::unique_ptr<State> state) : state_(std::move(state)) {}
HttpSocketServer::HttpSocketServer(HttpSocketServer&&) noexcept = default;
HttpSocketServer& HttpSocketServer::operator=(HttpSocketServer&&) noexcept = default;
HttpSocketServer::~HttpSocketServer() = default;

std::optional<HttpSocketServer> HttpSocketServer::bind(const Config& listener, const int backlog, std::string* error) {
  if (error != nullptr) error->clear();
  Config checked = listener;
  if (checked.port == 0U) checked.port = 1U;   // the policy check does not depend on the port value
  const ConfigInput verification{.host = checked.host, .port = checked.port,
                                 .api_key = checked.has_api_key ? std::optional<std::string>{"present"} : std::nullopt,
                                 .allow_remote = checked.allow_remote, .allow_wildcard_bind = checked.allow_wildcard_bind};
  if (!std::holds_alternative<Config>(validate_config(verification))) {
    if (error != nullptr) *error = "invalid listener configuration";
    return std::nullopt;
  }
  sockaddr_storage address{};
  AddressLength size = 0;
  int family = AF_UNSPEC;
  auto* v4 = reinterpret_cast<sockaddr_in*>(&address);
  auto* v6 = reinterpret_cast<sockaddr_in6*>(&address);
  if (inet_pton(AF_INET, listener.host.c_str(), &v4->sin_addr) == 1) {
    family = AF_INET; v4->sin_family = AF_INET; v4->sin_port = htons(listener.port); size = sizeof(sockaddr_in);
  } else if (inet_pton(AF_INET6, listener.host.c_str(), &v6->sin6_addr) == 1) {
    family = AF_INET6; v6->sin6_family = AF_INET6; v6->sin6_port = htons(listener.port); size = sizeof(sockaddr_in6);
  } else {
    if (error != nullptr) *error = "listener host must be a numeric IP literal";
    return std::nullopt;
  }
  Socket socket{::socket(family, SOCK_STREAM, IPPROTO_TCP)};
  if (!socket.valid()) { if (error != nullptr) *error = "listener socket setup failed"; return std::nullopt; }
  const int one = 1;
#if defined(_WIN32)
  (void)setsockopt(socket.get(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&one), sizeof(one));
#else
  (void)setsockopt(socket.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#endif
  if (family == AF_INET6 &&
      setsockopt(socket.get(), IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&one), sizeof(one)) != 0) {
    if (error != nullptr) *error = "listener socket setup failed";
    return std::nullopt;
  }
  if (::bind(socket.get(), reinterpret_cast<const sockaddr*>(&address), size) != 0 || ::listen(socket.get(), backlog) != 0 ||
      !set_non_blocking(socket.get())) {
    if (error != nullptr) *error = "listener socket setup failed";
    return std::nullopt;
  }
  sockaddr_storage bound{};
  AddressLength bound_size = sizeof(bound);
  if (getsockname(socket.get(), reinterpret_cast<sockaddr*>(&bound), &bound_size) != 0) {
    if (error != nullptr) *error = "listener socket setup failed";
    return std::nullopt;
  }
  auto state = std::make_unique<State>();
  state->port = ntohs(family == AF_INET ? reinterpret_cast<sockaddr_in*>(&bound)->sin_port
                                        : reinterpret_cast<sockaddr_in6*>(&bound)->sin6_port);
  state->socket = socket.release();
  return HttpSocketServer{std::move(state)};
}

HttpSocketServer::AcceptStatus HttpSocketServer::accept_next(const std::chrono::milliseconds slice, Accepted* out) {
  if (state_->stopped.load()) return AcceptStatus::stopped;
  const NativeSocket listener = state_->socket.load();
  PollEntry entry{listener, true, false};
  const int ready = poll_sockets(std::span<PollEntry>{&entry, 1U}, slice);
  if (state_->stopped.load()) return AcceptStatus::stopped;
  if (ready == 0) return AcceptStatus::timeout;
  sockaddr_storage peer{};
  AddressLength peer_size = sizeof(peer);
  const NativeSocket client = ::accept(listener, reinterpret_cast<sockaddr*>(&peer), &peer_size);
  if (client == kInvalidNativeSocket) {
    const int code = detail::last_socket_error();
#if defined(_WIN32)
    if (code == WSAEWOULDBLOCK) return AcceptStatus::timeout;
#else
    if (code == EAGAIN || code == EWOULDBLOCK) return AcceptStatus::timeout;
#endif
    const auto backoff = detail::accept_backoff_for(code);
    if (!backoff.has_value()) { state_->listener_failed = true; return AcceptStatus::listener_failed; }
    std::this_thread::sleep_for(*backoff);
    return AcceptStatus::transient;
  }
  out->socket = Socket{client};
  if (!set_non_blocking(client)) { out->socket.reset(); return AcceptStatus::transient; }
  set_no_delay(client);
#if defined(SO_NOSIGPIPE)
  { const int one = 1; (void)setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)); }
#endif
  classify_peer(peer, out);
  out->accepted_at = std::chrono::steady_clock::now();
  return AcceptStatus::accepted;
}

bool HttpSocketServer::serve_one(RequestRouter& router, const ConnectionLimits& limits, std::string* error) {
  if (error != nullptr) error->clear();
  Accepted accepted;
  for (;;) {
    const AcceptStatus status = accept_next(api_constants::kAcceptPollSlice, &accepted);
    if (status == AcceptStatus::accepted) break;
    if (status == AcceptStatus::listener_failed) { if (error != nullptr) *error = "accept failed"; return false; }
    if (status == AcceptStatus::stopped) { if (error != nullptr) *error = "listener stopped"; return false; }
    if (status == AcceptStatus::transient) { if (error != nullptr) *error = "accept interrupted"; return false; }
  }
  const ConnectionResult result = serve_connection(accepted.socket.get(), router, limits, WorkerSignals{}, accepted.accepted_at);
  if (result.outcome != ConnectionOutcome::responded) { if (error != nullptr) *error = "client request not completed"; return false; }
  return true;
}

std::uint16_t HttpSocketServer::port() const { return state_ ? state_->port : 0U; }
bool HttpSocketServer::listener_failed() const noexcept { return state_ && state_->listener_failed.load(); }
void HttpSocketServer::shutdown_listener() noexcept {
  if (!state_) return;
  state_->stopped = true;
  close_native_socket(state_->socket.exchange(kInvalidNativeSocket));
}
NativeSocket HttpSocketServer::native_handle() const noexcept { return state_ ? state_->socket.load() : kInvalidNativeSocket; }

}  // namespace secure_frontend
```

(`serve_one` returns `true` for every answered request including `4xx`/`5xx`, as SP0 did; it returns `false` for a client that never completed a request, so the existing `socket_server_*` "returns false" assertions keep their meaning.)

`subsystem_limits.hpp/.cpp`: delete `http_socket_server_limits` and add

```cpp
[[nodiscard]] ConnectionLimits connection_limits(const FrontendConfig& config);

ConnectionLimits connection_limits(const FrontendConfig& config) {
  const auto& http = config.limits.http;
  return {.head = {.max_request_line_bytes = http.max_request_line_bytes, .max_header_bytes = http.max_header_bytes,
                   .max_headers = http.max_headers, .max_request_bytes = http.max_request_bytes},
          .header_deadline = http.header_deadline, .request_deadline = http.request_deadline,
          .write_stall = http.write_stall, .response_write_deadline = http.response_write_deadline};
}
```

In `tests/subsystem_limits_test.cpp`, the case that asserted `http_socket_server_limits(config)` fields asserts the same values through `connection_limits(config)` plus `header_deadline == 10000 ms`, `write_stall == 10000 ms`, `response_write_deadline == 60000 ms` (keep the case name). In `src/secure_server_main.cpp` (anchor `HttpSocketServer::bind(`) the call becomes `secure_frontend::HttpSocketServer::bind(listener, static_cast<int>(config.listener.backlog), &bind_error)` and the serve loop's `server->serve_one(&error)` becomes `server->serve_one(legacy_router, secure_frontend::connection_limits(config), &error)` where `legacy_router` is a `DispatcherRouter`-equivalent: add to `http_request_handler.hpp` the public factory `std::unique_ptr<RequestRouter> make_dispatcher_router(HttpPolicy policy, BearerTokenValidator validator, RequestDispatcher dispatch);` (it returns the Task 6a `DispatcherRouter`, now holding its three members by value), and build it from the policy/validator/dispatcher main already passes. `main` (anchor `int main(int argc, char* argv[]) {`) gains `secure_frontend::WinsockGuard winsock;` as its first statement.

CMake: add `src/connection_handler.cpp` to the existing `add_library(secure_frontend_http_socket_server` sources and `target_link_libraries(secure_frontend_http_socket_server PUBLIC secure_frontend_socket_io secure_frontend_http_request_handler secure_frontend_config secure_frontend_api_core)`.

- [ ] **Step 4: Run to see GREEN**

```bash
cmake --build --preset linux-gcc && ctest --preset linux-gcc -R 'http_socket_server_tests|subsystem_limits' --output-on-failure
ctest --preset linux-gcc | tail -1
cmake --build --preset windows-llvm-mingw-cross && ctest --preset windows-llvm-mingw-cross -R http_socket_server_tests | tail -1
```

Expected: all pass — in particular `socket_server_responds_after_client_half_close` stays green for `GET` (spec §2.6); wine excludes `win_send_chunks_respect_write_stall` (record "pending MSYS2").

- [ ] **Step 5: Handoff row and commit** — row `| 8b | DOS-01 (P1/P3 deadlines), §2.2, §2.7 complete responses, §3.11 dispatch site, §9.4 | header-first connection handler, deliver_route_result + socket StreamSink, listener polled in slices, ephemeral-port tests, kMinimumSendBudget deleted | http_socket_server_tests.socket_server_rejects_oversize_header_with_431, …head_terminator_split_across_reads, …expect_continue_skipped_when_body_already_sent, …expect_continue_sent_after_authorization, …stream_handler_dispatch_writes_head_chunks_and_terminal_chunk, …socket_server_send_times_out_on_stalled_reader, …socket_server_accept_waits_without_timeout, …socket_server_ipv6_bind_is_v6only, …socket_server_responds_after_client_half_close (kept green), …win_send_chunks_respect_write_stall (W, pending MSYS2) | … | … |`.

```bash
git -C "$(git rev-parse --show-toplevel)" add secure_frontend docs/AGENT_HANDOFF.md
git -C "$(git rev-parse --show-toplevel)" -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "net: header-first P1-P8 connection handler with the stream dispatch site; listener polled in slices (SP3 §2.2, §2.7, §3.11)" \
  -m "Tests: http_socket_server_tests.socket_server_rejects_oversize_header_with_431, head_terminator_split_across_reads (Review Focus 2), expect_continue_skipped_when_body_already_sent (Review Focus 3), expect_continue_sent_after_authorization, stream_handler_dispatch_writes_head_chunks_and_terminal_chunk, win_send_chunks_respect_write_stall (W); ported per the case table: socket_server_send_times_out_on_stalled_reader, socket_server_accept_waits_without_timeout, socket_server_ipv6_bind_is_v6only, socket_server_responds_after_client_half_close (kept green) and the mechanical rest. RED: <line>. wine: <line>." \
  -m "Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>" -m "Claude-Session: <session link>"
```

---

## Task 9: Server on the new router; `secure_frontend_api_server_tests` with the header-first DOS-01 cases

**Files:**
- Create: `tests/api_server_test.cpp`
- Modify: `src/secure_server_main.cpp`, `include/secure_frontend/api_errors.hpp`, `src/api_errors.cpp` (`engine_retry_after`), `include/secure_frontend/http_request_handler.hpp`, `src/http_request_handler.cpp` (delete the legacy overload and `make_dispatcher_router`), `tests/support/server_harness.hpp`, `tests/support/server_harness.cpp`, `tests/server_test.cpp` (SP2 body constants, `ext01` amendment part 1), `CMakeLists.txt`

**Interfaces:**
- Consumes: `ApiRouter`, `ApiRouterConfig`, `HealthView` (Task 6b); `HttpSocketServer::serve_one(router, limits, error)`, `connection_limits()` (Task 8b), `WinsockGuard` (Task 8a); `parse_chat_request` (Task 7); SP2 `execute_with_lease`, `EngineSupervisor::snapshot()`, `HealthSnapshot`, `SupervisorState`; SP2 harness `ServerProcess`, `ServerOptions`; SP1 `render_config_json` (`tests/support/config_json.hpp`).
- Produces: `std::optional<int> engine_retry_after(std::string_view recovery) noexcept` (`"exhausted"` → none, otherwise `kRetryAfterEngineS`); `ServerOptions::config_literals` (`std::map<std::string, std::string>`, key path → JSON literal; non-empty → the harness writes `<root>/config.json` with `schema_version: 1` plus these keys, owner-only, and passes `--config <path>`); harness constants `kGeneric503`/`kBadRequest` redefined as `inline const std::string` holding the §7.2 bodies of `engine_unavailable`/`invalid_request`; test binary `secure_frontend_api_server_tests` with helpers `start_server(literals, engine_args)`, `authority(server)`, `chat_body(...)`, `raw(server, request)`.

From this task on `strata-secure-server` serves every request through `ApiRouter` with header-first authentication; it is still serial (`serve_one` in SP2's loop) until Task 12.

- [ ] **Step 1: Write the failing tests** — `tests/api_server_test.cpp`:

```cpp
#include "secure_frontend/api_errors.hpp"
#include "secure_frontend/socket_io.hpp"
#include "support/raw_http.hpp"
#include "support/server_harness.hpp"
#include "support/test_paths.hpp"
#include "support/test_support.hpp"

#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace std::chrono_literals;
using secure_frontend::ApiError;
int failures = 0;
void expect(bool condition, const std::string& message) {
  if (!condition) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
}

std::unique_ptr<test_support::ServerProcess> start_server(std::map<std::string, std::string> literals = {},
                                                         std::vector<std::string> engine_args = {}) {
  test_support::ServerOptions options;
  options.config_literals = std::move(literals);
  options.engine_args = std::move(engine_args);
  auto server = test_support::ServerProcess::start(options);
  const bool ready = server != nullptr && server->wait_until_listening(10s) && server->wait_for_health(test_support::kHealthReady, 15s);
  expect(ready, "server ready" + (server ? std::string{": "} + server->stderr_text() : std::string{}));
  return ready ? std::move(server) : nullptr;
}
std::string authority(const test_support::ServerProcess& s) { return "127.0.0.1:" + std::to_string(s.port()); }
std::string token() { return std::string{test_support::kHarnessToken}; }
std::string chat_body(const std::string& extra = R"("max_tokens":2)", const std::string& content = "Hi") {
  return R"({"model":"fake-model","messages":[{"role":"user","content":")" + content + R"("}])" +
         (extra.empty() ? "" : "," + extra) + "}";
}
std::string chat_request(const test_support::ServerProcess& s, const std::string& body, std::vector<std::string> extra = {}) {
  extra.insert(extra.begin(), {raw_http::bearer(token()), "Content-Type: application/json"});
  return raw_http::request("POST", "/v1/chat/completions", authority(s), extra, body);
}
std::optional<raw_http::Response> raw(const test_support::ServerProcess& s, const std::string& request,
                                      std::chrono::milliseconds timeout = 10s) {
  return raw_http::exchange("127.0.0.1", s.port(), request, timeout);
}
std::string record_of(const std::filesystem::path& file) {
  std::ifstream in(file, std::ios::binary);
  return std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
std::size_t count_of(const std::string& text, std::string_view needle) {
  std::size_t n = 0;
  for (std::size_t p = text.find(needle); p != std::string::npos; p = text.find(needle, p + 1U)) ++n;
  return n;
}

void dos01_body_over_limit_is_413_before_reading() {
  auto server = start_server();
  if (!server) return;
  auto c = raw_http::Connection::open("127.0.0.1", server->port());
  const std::string head = "POST /v1/chat/completions HTTP/1.1\r\nHost: " + authority(*server) + "\r\n" + raw_http::bearer(token()) +
                           "\r\nContent-Type: application/json\r\nContent-Length: 49153\r\n\r\n";
  const auto started = std::chrono::steady_clock::now();
  expect(c && c->send(head), "header only");
  const auto r = raw_http::parse_response(c ? c->read_until_close(5s) : "");
  expect(r && r->status == 413 && r->body == secure_frontend::render_error_body(ApiError::request_too_large),
         "49153 > max_body_bytes: 413 after auth, body never read");
  expect(std::chrono::steady_clock::now() - started < 1s, "answered at once");
}

void dos01_content_length_over_request_limit_is_413_before_auth() {
  auto server = start_server();
  if (!server) return;
  auto c = raw_http::Connection::open("127.0.0.1", server->port());
  const auto started = std::chrono::steady_clock::now();
  expect(c && c->send("POST /v1/chat/completions HTTP/1.1\r\nHost: " + authority(*server) + "\r\nContent-Length: 70000\r\n\r\n"),
         "no token, no body");
  const auto r = raw_http::parse_response(c ? c->read_until_close(5s) : "");
  expect(r && r->status == 413, "413 (a global limit), not 401");
  expect(std::chrono::steady_clock::now() - started < 100ms + 400ms, "within 100 ms plus scheduling slack");
}

void dos01_unauthenticated_gets_401_before_body_is_read() {
  auto server = start_server();
  if (!server) return;
  auto c = raw_http::Connection::open("127.0.0.1", server->port());
  const auto started = std::chrono::steady_clock::now();
  expect(c && c->send("POST /v1/chat/completions HTTP/1.1\r\nHost: " + authority(*server) +
                      "\r\nAuthorization: Bearer wrong-token-0123456789abcdefghijklmnop\r\nContent-Type: application/json\r\n"
                      "Content-Length: 40000\r\n\r\n"), "wrong token, body withheld");
  const auto r = raw_http::parse_response(c ? c->read_until_close(5s) : "");
  expect(r && r->status == 401 && r->header("WWW-Authenticate") == std::optional<std::string>{"Bearer"}, "401 before any body byte");
  expect(std::chrono::steady_clock::now() - started < 500ms, "within 100 ms plus slack");
}

void dos01_expect_continue_after_auth_only() {
  auto server = start_server();
  if (!server) return;
  const std::string body = chat_body();
  auto bad = raw_http::Connection::open("127.0.0.1", server->port());
  expect(bad && bad->send("POST /v1/chat/completions HTTP/1.1\r\nHost: " + authority(*server) +
                          "\r\nAuthorization: Bearer nope-0123456789abcdefghijklmnopqrstuv\r\nContent-Type: application/json\r\n"
                          "Expect: 100-continue\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n"), "bad token");
  const std::string refused = bad ? bad->read_until_close(5s) : "";
  expect(refused.find("100 Continue") == std::string::npos && refused.rfind("HTTP/1.1 401", 0) == 0, "no 100 before auth");
  auto good = raw_http::Connection::open("127.0.0.1", server->port());
  expect(good && good->send("POST /v1/chat/completions HTTP/1.1\r\nHost: " + authority(*server) + "\r\n" + raw_http::bearer(token()) +
                            "\r\nContent-Type: application/json\r\nExpect: 100-continue\r\nContent-Length: " +
                            std::to_string(body.size()) + "\r\n\r\n"), "valid token");
  expect(good && good->read_for(500ms) == "HTTP/1.1 100 Continue\r\n\r\n", "100 after auth");
  expect(good && good->send(body), "body");
  const auto r = raw_http::parse_response(good ? good->read_until_close(10s) : "");
  expect(r && r->status == 200, "the chat completes");
}

void dos01_slow_header_is_408_at_header_deadline() {
  auto server = start_server({{"limits.http.header_deadline_ms", "1000"}});
  if (!server) return;
  auto c = raw_http::Connection::open("127.0.0.1", server->port());
  const auto started = std::chrono::steady_clock::now();
  expect(c && c->send("GET /health HTTP/1.1\r\nHost: "), "partial header");
  const auto r = raw_http::parse_response(c ? c->read_until_close(5s) : "");
  const auto elapsed = std::chrono::steady_clock::now() - started;
  expect(r && r->status == 408 && r->body == secure_frontend::render_error_body(ApiError::request_timeout), "408 request_timeout");
  expect(elapsed >= 900ms && elapsed < 2s, "at the 1 s header deadline");
}

void dos01_slow_body_is_408_at_request_deadline() {
  auto server = start_server({{"limits.http.header_deadline_ms", "1000"}, {"limits.http.request_deadline_ms", "2000"}});
  if (!server) return;
  auto c = raw_http::Connection::open("127.0.0.1", server->port());
  const auto started = std::chrono::steady_clock::now();
  const std::string body = chat_body();
  expect(c && c->send("POST /v1/chat/completions HTTP/1.1\r\nHost: " + authority(*server) + "\r\n" + raw_http::bearer(token()) +
                      "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" +
                      body.substr(0, 5)), "valid token, partial body");
  const auto r = raw_http::parse_response(c ? c->read_until_close(5s) : "");
  const auto elapsed = std::chrono::steady_clock::now() - started;
  expect(r && r->status == 408, "408 for an authenticated slow body");
  expect(elapsed >= 1900ms && elapsed < 3s, "at the 2 s request deadline");
}

void dos01_incomplete_body_then_close_frees_worker() {
  auto server = start_server();
  if (!server) return;
  {
    auto c = raw_http::Connection::open("127.0.0.1", server->port());
    expect(c && c->send("POST /v1/chat/completions HTTP/1.1\r\nHost: " + authority(*server) + "\r\n" + raw_http::bearer(token()) +
                        "\r\nContent-Type: application/json\r\nContent-Length: 100\r\n\r\n{\"model\""), "partial body");
  }   // closed here
  const auto started = std::chrono::steady_clock::now();
  const auto r = raw(*server, chat_request(*server, chat_body()));
  expect(r && r->status == 200 && std::chrono::steady_clock::now() - started < 2s, "the next client is served at once");
}

void err_unauthenticated_is_401_on_every_path_and_method() {
  auto server = start_server();
  if (!server) return;
  for (const char* method : {"GET", "POST", "PUT", "DELETE", "PATCH", "OPTIONS", "HEAD"}) {
    for (const char* target : {"/health", "/v1/models", "/v1/models/fake-model", "/v1/chat/completions", "/v1/completions", "/x"}) {
      const std::string request = std::string{method} + " " + target + " HTTP/1.1\r\nHost: " + authority(*server) +
                                  (std::string{method} == "POST" || std::string{method} == "PUT" ? "\r\nContent-Length: 0" : "") + "\r\n\r\n";
      const auto r = raw(*server, request);
      expect(r && r->status == 401 && r->body == secure_frontend::render_error_body(ApiError::invalid_api_key),
             std::string{method} + " " + target + " is 401 without a token");
    }
  }
}

void security_headers_on_every_response() {
  auto server = start_server();
  if (!server) return;
  const std::vector<std::string> requests{
      raw_http::request("GET", "/health", authority(*server), {raw_http::bearer(token())}),
      raw_http::request("GET", "/v1/models", authority(*server), {raw_http::bearer(token())}),
      raw_http::request("GET", "/nope", authority(*server), {raw_http::bearer(token())}),
      raw_http::request("GET", "/health", authority(*server), {}),
      raw_http::request("GET", "/health", "evil:1", {raw_http::bearer(token())}),
      chat_request(*server, chat_body()),
      chat_request(*server, "{"),
      "GET /health HTTP/1.0\r\nHost: " + authority(*server) + "\r\n\r\n"};
  for (const auto& request : requests) {
    const auto r = raw(*server, request);
    expect(r.has_value(), "response for " + request.substr(0, request.find('\r')));
    if (!r) continue;
    for (const auto& [name, value] : std::vector<std::pair<std::string, std::string>>{
             {"Connection", "close"}, {"Cache-Control", "no-store"}, {"X-Content-Type-Options", "nosniff"},
             {"Content-Security-Policy", "default-src 'none'; frame-ancestors 'none'"}, {"Referrer-Policy", "no-referrer"},
             {"Cross-Origin-Resource-Policy", "same-origin"}}) {
      expect(r->header(name) == std::optional<std::string>{value}, name + " on " + std::to_string(r->status));
    }
    expect(r->header("Date").has_value() && !r->header("Server") && !r->header("X-Powered-By"), "Date, no Server, no X-Powered-By");
  }
}
}  // namespace

int main(int argc, char** argv) {
  secure_frontend::WinsockGuard winsock;
  return secure_frontend_test::run_cases(argc, argv, {
      {"dos01_body_over_limit_is_413_before_reading", dos01_body_over_limit_is_413_before_reading},
      {"dos01_content_length_over_request_limit_is_413_before_auth", dos01_content_length_over_request_limit_is_413_before_auth},
      {"dos01_unauthenticated_gets_401_before_body_is_read", dos01_unauthenticated_gets_401_before_body_is_read},
      {"dos01_expect_continue_after_auth_only", dos01_expect_continue_after_auth_only},
      {"dos01_slow_header_is_408_at_header_deadline", dos01_slow_header_is_408_at_header_deadline},
      {"dos01_slow_body_is_408_at_request_deadline", dos01_slow_body_is_408_at_request_deadline},
      {"dos01_incomplete_body_then_close_frees_worker", dos01_incomplete_body_then_close_frees_worker},
      {"err_unauthenticated_is_401_on_every_path_and_method", err_unauthenticated_is_401_on_every_path_and_method},
      {"security_headers_on_every_response", security_headers_on_every_response},
  }, failures);
}
```

CMake (tests block, after `secure_frontend_server_tests`):

```cmake
    secure_frontend_add_test_executable(secure_frontend_api_server_tests tests/api_server_test.cpp
        secure_frontend_test_support secure_frontend_api_core secure_frontend_socket_io)
    secure_frontend_test_paths(secure_frontend_api_server_tests)
    set(SECURE_FRONTEND_API_SERVER_CASES
        dos01_body_over_limit_is_413_before_reading dos01_content_length_over_request_limit_is_413_before_auth
        dos01_unauthenticated_gets_401_before_body_is_read dos01_expect_continue_after_auth_only
        dos01_slow_header_is_408_at_header_deadline dos01_slow_body_is_408_at_request_deadline
        dos01_incomplete_body_then_close_frees_worker err_unauthenticated_is_401_on_every_path_and_method
        security_headers_on_every_response)
    # Ruling R-J: these cases start the real server over sockets; wine excludes them, MSYS2 is authoritative.
    secure_frontend_add_cases(secure_frontend_api_server_tests ${SECURE_FRONTEND_API_SERVER_CASES}
        LABELS ${SECURE_FRONTEND_WINDOWS_RUNTIME} TIMEOUT 120)
```

(Later tasks append to `SECURE_FRONTEND_API_SERVER_CASES` before this call, or add their own `secure_frontend_add_cases` call when they need labels.)

Harness (`tests/support/server_harness.hpp`): add the last `ServerOptions` member `std::map<std::string, std::string> config_literals{};` and replace the two constants:

```cpp
inline const std::string kGeneric503 = secure_frontend::render_error_body(secure_frontend::ApiError::engine_unavailable);
inline const std::string kBadRequest = secure_frontend::render_error_body(secure_frontend::ApiError::invalid_request);
```

(include `secure_frontend/api_errors.hpp`; link `secure_frontend_test_support` with `secure_frontend_api_core`). In `ServerProcess::start` (`tests/support/server_harness.cpp`), before the `extra_args` are appended (anchor: the loop over `options.extra_args`), add:

```cpp
  if (!options.config_literals.empty()) {
    std::map<std::string, std::string> literals = options.config_literals;
    literals.emplace("schema_version", "1");
    const auto config_file = root / "config.json";
    secure_frontend_test::write_file(config_file, secure_frontend_test::render_config_json(literals), 0600);
    arguments.push_back("--config");
    arguments.push_back(test_support::utf8_path(config_file));
  }
```

(`root` and `arguments` are the harness's names for the per-server directory and argv vector; use its names. Include `support/config_json.hpp`.) Check that `http_request` sends `Content-Type: application/json` with a body; if it sends another type, change it to `application/json` (a `415` would otherwise hide every chat).

`tests/server_test.cpp`, `ext01_extension_fields_spawn_nothing` amendment part 1 (spec §6.4): replace its `bodies` vector and loop with

```cpp
  const std::vector<std::pair<std::string, std::string>> bodies{
      {R"({"model":"fake-model","messages":[{"role":"user","content":"Hi"}],"tools":[{"type":"function","function":{"name":"x"}}]})", "tools"},
      {R"({"model":"fake-model","messages":[{"role":"user","content":"Hi"}],"tool_choice":"auto"})", "tool_choice"},
      {R"({"model":"fake-model","messages":[{"role":"user","content":"Hi"}],"functions":[{"name":"x"}]})", "functions"},
      {R"({"model":"fake-model","messages":[{"role":"user","content":"Hi"}],"function_call":"auto"})", "function_call"},
      {R"({"model":"fake-model","messages":[{"role":"user","content":"Hi"}],"parallel_tool_calls":true})", "parallel_tool_calls"},
      {R"({"model":"fake-model","messages":[{"role":"user","content":"Hi"}],"mcp_servers":[{"url":"mcp://x"}]})", "mcp_servers"},
      {R"({"model":"fake-model","messages":[{"role":"tool","content":"run"}]})", "role"},
      {R"({"model":"fake-model","messages":[{"role":"function","content":"run"}]})", "role"}};
  for (const auto& [body, key] : bodies) {
    const auto response = server->post_chat(body);
    expect(response && response->status == 400 &&
               response->body == secure_frontend::render_error_body(secure_frontend::ApiError::extension_disabled, key),
           "refused with the fixed extension_disabled body naming " + key + ": " + describe(response));
  }
```

(the `{"stream":true}` body is removed: streaming is a feature, spec §6.4). Update `proc04_chat_during_restart_is_generic_503` and `proc04_abort_in_generation_is_generic_503` only if their assertions no longer hold through the redefined `kGeneric503` (they compare against the constant, which now holds the §7.2 `engine_unavailable` body).

- [ ] **Step 2: Run to see RED**

```bash
cmake --build --preset linux-gcc && ctest --preset linux-gcc -R 'api_server_tests|server_tests' --output-on-failure | tail -20
```

Expected: FAIL — e.g. `dos01_unauthenticated_gets_401_before_body_is_read` gets no response within 500 ms (the legacy adapter path still waits for the body) and `ext01_extension_fields_spawn_nothing` fails with "refused with the fixed extension_disabled body naming tools". Record one line of each.

- [ ] **Step 3: Implement**

`api_errors.hpp/.cpp`: add

```cpp
// Spec §3.8: engine_unavailable carries Retry-After 5 unless recovery=exhausted.
[[nodiscard]] std::optional<int> engine_retry_after(std::string_view recovery) noexcept;

std::optional<int> engine_retry_after(const std::string_view recovery) noexcept {
  if (recovery == "exhausted") return std::nullopt;
  return api_constants::kRetryAfterEngineS;
}
```

`src/secure_server_main.cpp`, in `run` — replace the policy/validator/dispatcher construction and the Task 6/8 legacy wiring with one router (the Task 6b/8b legacy wiring; anchors: `make_dispatcher_router(`, `const secure_frontend::ApiRouter router(` / `secure_frontend::ApiRouter router(secure_frontend::ApiRouterConfig{`):

```cpp
  const secure_frontend::ChatSchemaLimits schema_limits = secure_frontend::chat_schema_limits(config);
  const std::string model_id{config.engine.model_id.value()};
  const auto engine_refusal = [&supervisor]() {
    const auto snapshot = supervisor.snapshot();
    if (snapshot.state == secure_frontend::SupervisorState::shutting_down || snapshot.state == secure_frontend::SupervisorState::stopped) {
      return secure_frontend::make_error_response(secure_frontend::ApiError::server_shutting_down);
    }
    return secure_frontend::make_error_response(secure_frontend::ApiError::engine_unavailable,
                                                {.retry_after = secure_frontend::engine_retry_after(snapshot.recovery)});
  };
  secure_frontend::ApiRouter router(secure_frontend::ApiRouterConfig{
      .policy = secure_frontend::make_http_policy(config.listener.host, config.listener.port, config.listener.host_aliases,
                                                  config.listener.cors_origins),
      .validator = secure_frontend::make_bearer_validator(verifier),
      .model_id = model_id,
      .owned_by = selected->owned_by,
      .chat_body_limit = config.limits.chat.max_body_bytes,
      .health = [&supervisor] {
        const auto s = supervisor.snapshot();
        return secure_frontend::HealthView{s.healthy, s.engine, s.recovery};
      },
      // Serial chat through SP2's lease until Task 16 replaces it with ChatRoute.
      .chat = [&](std::string_view body, const secure_frontend::RequestContext&) -> secure_frontend::HttpResponse {
        const auto parsed = secure_frontend::parse_chat_request(body, schema_limits);
        if (const auto* error = std::get_if<secure_frontend::ChatRequestError>(&parsed)) {
          return secure_frontend::make_error_response(error->error, {.param = error->param});
        }
        const auto& request = std::get<secure_frontend::ChatCompletionRequest>(parsed);
        if (request.model != model_id) return secure_frontend::make_error_response(secure_frontend::ApiError::model_not_found);
        const auto outcome = secure_frontend::execute_with_lease(supervisor, request, *tokenizer);
        if (!outcome.has_value()) return engine_refusal();
        if (const auto* failure = std::get_if<secure_frontend::ChatExecutionError>(&*outcome)) {
          using Code = secure_frontend::ChatExecutionErrorCode;
          switch (failure->code) {
            case Code::context_limit_exceeded: return secure_frontend::make_error_response(secure_frontend::ApiError::context_length_exceeded);
            case Code::invalid_request:
            case Code::planning_failed: return secure_frontend::make_error_response(secure_frontend::ApiError::invalid_request);
            case Code::generation_timed_out: return secure_frontend::make_error_response(secure_frontend::ApiError::engine_error);
            case Code::output_decode_failed: return secure_frontend::make_error_response(secure_frontend::ApiError::internal_error);
            case Code::engine_protocol_failed: return engine_refusal();
          }
          return secure_frontend::make_error_response(secure_frontend::ApiError::internal_error);
        }
        const auto& result = std::get<secure_frontend::ChatExecutionResult>(*outcome);
        const auto rendered = secure_frontend::render_chat_completion_response(request, result.text, schema_limits);
        if (!rendered) return secure_frontend::make_error_response(secure_frontend::ApiError::internal_error);
        return secure_frontend::HttpResponse{.status = 200, .headers = {{"Content-Type", "application/json; charset=utf-8"}},
                                             .body = *rendered};
      }});
  const secure_frontend::ConnectionLimits limits = secure_frontend::connection_limits(config);
```

and the serve loop's call becomes `server->serve_one(router, limits, &error)` (keep SP2's loop condition and shutdown handling exactly; only the arguments change). Delete the old `json_error` helper if nothing else uses it, the dispatcher lambda, the `route_for_get` lambda, and `make_dispatcher_router`, the `DispatcherRouter` class and the legacy `handle_http_request(..., const HttpPolicy&, ...)` overload with `HttpParserLimits` from `http_request_handler.{hpp,cpp}` (search `grep -rn "HttpParserLimits\|RequestDispatcher" src include tests` and port any remaining test use to the router seam). Link `strata-secure-server` with `secure_frontend_api_router secure_frontend_socket_io secure_frontend_api_core`.

- [ ] **Step 4: Run to see GREEN**

```bash
cmake --build --preset linux-gcc && ctest --preset linux-gcc -R 'api_server_tests|server_tests|server_e2e' --output-on-failure
ctest --preset linux-gcc | tail -1
cmake --build --preset windows-llvm-mingw-cross && ctest --preset windows-llvm-mingw-cross -R 'api_server_tests|server_tests' | tail -1
```

Expected: all pass. SP1 `server_e2e_tests` that asserted old error bodies are updated here to the §7.2 bodies (name each).

- [ ] **Step 5: Handoff row and commit** — row `| 9 | DOS-01 (header-first over TCP), AUTH-01, §4.6, §7, EXT-01 (amended ext01 bodies) | server serves through ApiRouter, one Winsock guard, harness config files | api_server_tests.dos01_body_over_limit_is_413_before_reading, …dos01_content_length_over_request_limit_is_413_before_auth, …dos01_unauthenticated_gets_401_before_body_is_read, …dos01_expect_continue_after_auth_only, …dos01_slow_header_is_408_at_header_deadline, …dos01_slow_body_is_408_at_request_deadline, …dos01_incomplete_body_then_close_frees_worker, …err_unauthenticated_is_401_on_every_path_and_method, …security_headers_on_every_response; server_tests.ext01_extension_fields_spawn_nothing (amended) | … | … |`.

```bash
git -C "$(git rev-parse --show-toplevel)" add secure_frontend docs/AGENT_HANDOFF.md
git -C "$(git rev-parse --show-toplevel)" -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "server: every request through the header-first router; 401/413 before the body over TCP (SP3 DOS-01, §4.1, §7)" \
  -m "Tests: api_server_tests.dos01_body_over_limit_is_413_before_reading, dos01_content_length_over_request_limit_is_413_before_auth, dos01_unauthenticated_gets_401_before_body_is_read, dos01_expect_continue_after_auth_only, dos01_slow_header_is_408_at_header_deadline, dos01_slow_body_is_408_at_request_deadline, dos01_incomplete_body_then_close_frees_worker, err_unauthenticated_is_401_on_every_path_and_method, security_headers_on_every_response; server_tests.ext01_extension_fields_spawn_nothing amended (spec §6.4). Legacy dispatcher adapter deleted. RED: <line>. wine: <line>." \
  -m "Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>" -m "Claude-Session: <session link>"
```

---

## Task 10: WEB-01..06 over TCP, AUTH-01 over the route table, the wildcard-bind process check

**Files:**
- Modify: `tests/api_server_test.cpp`, `CMakeLists.txt`

**Interfaces:**
- Consumes: Task 9's helpers (`start_server`, `authority`, `token`, `chat_body`, `chat_request`, `raw`, `record_of`, `count_of`); `kRouteTable` (Task 6a); harness `ServerOptions::engine_args`, `ServerProcess::active_processes()`, `wait_exit()`, `stderr_text()`, `test_support::make_temp_dir`, `utf8_path`.
- Produces: helper `bool has_no_cors_headers(const raw_http::Response&)`; cases below. No product code is expected to change; a failure here is a defect in Tasks 3/6/9 and is fixed in the file that owns the rule, named in the commit.

- [ ] **Step 1: Write the tests** — append to `tests/api_server_test.cpp` (include `secure_frontend/request_router.hpp`, `<filesystem>`) and register every case in `SECURE_FRONTEND_API_SERVER_CASES`:

```cpp
bool has_no_cors_headers(const raw_http::Response& r) {
  return !r.header("Access-Control-Allow-Origin") && !r.header("Access-Control-Allow-Credentials");
}
std::vector<std::string> route_targets() {
  std::vector<std::string> targets;
  for (const auto& e : secure_frontend::kRouteTable) targets.push_back(e.prefix ? std::string{e.path} + "fake-model" : std::string{e.path});
  return targets;
}
std::string route_request(const test_support::ServerProcess& s, const secure_frontend::RouteEntry& e, const std::string& host,
                          std::vector<std::string> extra = {}) {
  extra.insert(extra.begin(), raw_http::bearer(token()));
  const std::string target = e.prefix ? std::string{e.path} + "fake-model" : std::string{e.path};
  if (e.method == "POST") {
    extra.push_back("Content-Type: application/json");
    return raw_http::request("POST", target, host, extra, chat_body());
  }
  return raw_http::request(e.method, target, host, extra);
}

void web01_bad_host_rejected_on_every_route() {
  auto server = start_server();
  if (!server) return;
  const std::string port = std::to_string(server->port());
  const std::vector<std::pair<std::string, int>> hosts{
      {"evil.example:" + port, 403}, {"127.0.0.2:" + port, 403}, {"127.0.0.1:1", 403}, {"127.0.0.1", 403},
      {"localhost.:" + port, 400}, {"user@127.0.0.1:" + port, 400}, {"::1:" + port, 400}};
  for (const auto& entry : secure_frontend::kRouteTable) {
    for (const auto& [host, status] : hosts) {
      const auto r = raw(*server, route_request(*server, entry, host));
      expect(r && r->status == status && r->body.find("fake-model") == std::string::npos && r->body.find("\"status\"") == std::string::npos,
             std::string{entry.path} + " with Host " + host + " is " + std::to_string(status) + " and carries no route data");
    }
    const auto twice = raw(*server, route_request(*server, entry, authority(*server), {"Host: " + authority(*server)}));
    expect(twice && twice->status == 400, std::string{entry.path} + " with two Host fields is 400");
  }
}

void web01_ipv6_bound_host_rules() {
  if (!test_support::ipv6_loopback_available()) test_support::skip("no IPv6 loopback on this host");
  test_support::ServerOptions options;
  options.config_literals = {{"listener.host", "\"::1\""}};
  auto server = test_support::ServerProcess::start(options);
  expect(server != nullptr, "server starts on ::1");
  if (!server) return;
  const std::string port = std::to_string(server->port());
  std::optional<raw_http::Response> ok;
  for (int i = 0; i < 100 && !(ok && ok->status == 200); ++i) {
    ok = raw_http::exchange("::1", server->port(), raw_http::request("GET", "/health", "[::1]:" + port, {raw_http::bearer(token())}));
    if (!(ok && ok->status == 200)) std::this_thread::sleep_for(100ms);
  }
  expect(ok && ok->status == 200, "[::1]:port is the bound authority");
  const auto unbracketed = raw_http::exchange("::1", server->port(), raw_http::request("GET", "/health", "::1", {raw_http::bearer(token())}));
  expect(unbracketed && unbracketed->status == 400, "::1 is malformed");
  const auto no_port = raw_http::exchange("::1", server->port(), raw_http::request("GET", "/health", "[::1]", {raw_http::bearer(token())}));
  expect(no_port && no_port->status == 403, "[::1] without the port is not allowed");
  const auto local = raw_http::exchange("::1", server->port(), raw_http::request("GET", "/health", "localhost:" + port, {raw_http::bearer(token())}));
  expect(local && local->status == 200, "localhost:port is allowed on a loopback bind (E10)");
}

void web02_bound_authority_and_localhost_alias_succeed() {
  auto server = start_server();
  if (!server) return;
  for (const std::string host : {authority(*server), "localhost:" + std::to_string(server->port()), "LOCALHOST:" + std::to_string(server->port())}) {
    for (const auto& entry : secure_frontend::kRouteTable) {
      const auto r = raw(*server, route_request(*server, entry, host));
      expect(r && r->status == 200, std::string{entry.path} + " succeeds with Host " + host);
    }
  }
}

void web02_host_alias_from_config_succeeds() {
  auto server = start_server({{"listener.host_aliases", "[\"strata.lan:8080\",\"box.lan\"]"}});
  if (!server) return;
  for (const std::string host : {"strata.lan:8080", "box.lan"}) {
    const auto r = raw(*server, raw_http::request("GET", "/health", host, {raw_http::bearer(token())}));
    expect(r && r->status == 200, "configured alias " + host);
  }
  const auto other = raw(*server, raw_http::request("GET", "/health", "strata.lan:8081", {raw_http::bearer(token())}));
  expect(other && other->status == 403, "the alias is exact (other port refused)");
}

void web03_untrusted_origin_no_cors_header() {
  auto server = start_server({{"listener.cors_origins", "[\"http://app.example:3000\"]"}});
  if (!server) return;
  const std::string port = std::to_string(server->port());
  for (const std::string origin : {"null", "http://app.example.evil:3000", "http://APP.example:3000", "http://127.0.0.1:80",
                                   "https://127.0.0.1:" + port, "http://app.example:3000/"}) {
    const auto r = raw(*server, raw_http::request("GET", "/health", authority(*server), {raw_http::bearer(token()), "Origin: " + origin}));
    expect(r && r->status == 403 && has_no_cors_headers(*r), "origin " + origin + " is 403 without CORS headers");
  }
  const auto dup = raw(*server, raw_http::request("GET", "/health", authority(*server),
                                                  {raw_http::bearer(token()), "Origin: http://app.example:3000", "Origin: http://app.example:3000"}));
  expect(dup && dup->status == 403 && has_no_cors_headers(*dup), "duplicate Origin is 403");
}

void web04_default_emits_no_cors_header_anywhere() {
  const auto dir = test_support::make_temp_dir("web04");
  auto server = start_server({}, {"--record-file=" + test_support::utf8_path(dir / "record.txt")});
  if (!server) return;
  const std::string a = authority(*server);
  const std::string origin = "Origin: http://" + a;
  std::vector<std::string> requests{
      raw_http::request("GET", "/health", a, {raw_http::bearer(token()), origin}),
      raw_http::request("GET", "/v1/models", a, {raw_http::bearer(token()), origin}),
      raw_http::request("GET", "/v1/models/fake-model", a, {raw_http::bearer(token()), origin}),
      raw_http::request("GET", "/v1/models/other", a, {raw_http::bearer(token()), origin}),
      chat_request(*server, chat_body(), {origin}),
      chat_request(*server, "{", {origin}),
      chat_request(*server, chat_body(R"("tools":[])"), {origin}),
      raw_http::request("GET", "/nope", a, {raw_http::bearer(token()), origin}),
      raw_http::request("DELETE", "/health", a, {raw_http::bearer(token()), origin}),
      raw_http::request("GET", "/health", a, {origin}),
      raw_http::request("GET", "/health", "evil:1", {origin}),
      raw_http::request("GET", "/health", a, {raw_http::bearer(token()), "Origin: http://evil.example"}),
      raw_http::request("OPTIONS", "/v1/chat/completions", a, {"Origin: http://evil.example", "Access-Control-Request-Method: POST"}),
      raw_http::request("POST", "/v1/chat/completions", a, {raw_http::bearer(token()), "Content-Type: text/plain"}, "{}"),
      raw_http::request("POST", "/v1/chat/completions", a, {raw_http::bearer(token()), "Transfer-Encoding: chunked"}),
      raw_http::request("POST", "/v1/chat/completions", a, {raw_http::bearer(token()), "Content-Type: application/json", "Expect: x"}, "{}"),
      "GET /health HTTP/1.0\r\nHost: " + a + "\r\n\r\n",
      "GET /" + std::string(5000, 'a') + " HTTP/1.1\r\nHost: " + a + "\r\n\r\n",
      raw_http::request("GET", "/health", a, {"X-Fill: " + std::string(17000, 'f')}),
      raw_http::request("POST", "/v1/chat/completions", a, {}, std::string(70000, ' '))};
  std::set<int> statuses;
  for (const auto& request : requests) {
    const auto r = raw(*server, request);
    if (!r) { expect(false, "no response for " + request.substr(0, 40)); continue; }
    statuses.insert(r->status);
    expect(has_no_cors_headers(*r), "no CORS header on a " + std::to_string(r->status));
  }
  for (const int s : {200, 400, 401, 403, 404, 405, 411, 413, 414, 415, 417, 431, 505}) {
    expect(statuses.count(s) == 1U, "status " + std::to_string(s) + " was exercised");
  }
}

void web05_only_exact_configured_origin_allowed() {
  auto server = start_server({{"listener.cors_origins", "[\"http://app.example:3000\"]"}});
  if (!server) return;
  const auto ok = raw(*server, raw_http::request("GET", "/v1/models", authority(*server), {raw_http::bearer(token()), "Origin: http://app.example:3000"}));
  expect(ok && ok->status == 200 && ok->header("Access-Control-Allow-Origin") == std::optional<std::string>{"http://app.example:3000"} &&
         ok->header("Vary") == std::optional<std::string>{"Origin"} && !ok->header("Access-Control-Allow-Credentials"),
         "the configured origin gets its exact value, Vary, never credentials");
  const auto other = raw(*server, raw_http::request("GET", "/v1/models", authority(*server), {raw_http::bearer(token()), "Origin: http://other.example:3000"}));
  expect(other && other->status == 403 && has_no_cors_headers(*other), "another origin is never reflected");
  const auto same = raw(*server, raw_http::request("GET", "/v1/models", authority(*server), {raw_http::bearer(token()), "Origin: http://" + authority(*server)}));
  expect(same && same->status == 200 && has_no_cors_headers(*same), "same origin needs no CORS headers");
}

void web06_preflight_matrix() {
  const auto dir = test_support::make_temp_dir("web06p");
  auto server = start_server({{"listener.cors_origins", "[\"http://app.example:3000\"]"}},
                             {"--record-file=" + test_support::utf8_path(dir / "record.txt")});
  if (!server) return;
  const std::string a = authority(*server);
  const auto preflight = [&](const std::string& origin, const std::string& method, const std::string& headers) {
    std::vector<std::string> lines{"Origin: " + origin, "Access-Control-Request-Method: " + method};
    if (!headers.empty()) lines.push_back("Access-Control-Request-Headers: " + headers);
    return raw(*server, raw_http::request("OPTIONS", "/v1/chat/completions", a, lines));
  };
  for (const std::string method : {"GET", "POST"}) {
    for (const std::string headers : {"", "authorization", "authorization, content-type",
                                      "Authorization,Content-Type,X-Stainless-OS,x-stainless-lang,x-stainless-arch,x-stainless-runtime,"
                                      "x-stainless-runtime-version,x-stainless-package-version,x-stainless-retry-count,x-stainless-timeout"}) {
      const auto r = preflight("http://app.example:3000", method, headers);
      expect(r && r->status == 204 && r->header("Access-Control-Allow-Origin") == std::optional<std::string>{"http://app.example:3000"} &&
             r->header("Access-Control-Allow-Methods") == std::optional<std::string>{"GET, POST"} &&
             r->header("Access-Control-Max-Age") == std::optional<std::string>{"600"},
             "allowed preflight " + method + " [" + headers + "] needs no token");
    }
  }
  for (const auto& [origin, method, headers] : std::vector<std::tuple<std::string, std::string, std::string>>{
           {"http://evil.example", "POST", ""}, {"http://app.example:3000", "PUT", ""}, {"http://app.example:3000", "POST", "x-api-key"},
           {"http://" + a, "POST", ""}, {"null", "POST", ""}}) {
    const auto r = preflight(origin, method, headers);
    expect(r && r->status == 403 && has_no_cors_headers(*r), "refused preflight " + origin + " " + method + " " + headers);
  }
  expect(record_of(dir / "record.txt").find("GEN") == std::string::npos, "no preflight reached a route");
}

void web06_cross_origin_post_without_trust_rejected() {
  const auto dir = test_support::make_temp_dir("web06");
  auto server = start_server({}, {"--record-file=" + test_support::utf8_path(dir / "record.txt")});
  if (!server) return;
  const auto r = raw(*server, chat_request(*server, chat_body(), {"Origin: http://evil.example"}));
  expect(r && r->status == 403 && has_no_cors_headers(*r), "a valid token from a foreign origin is 403");
  expect(record_of(dir / "record.txt").find("GEN") == std::string::npos, "the engine received no GEN");
}

void auth01_every_route_rejects_missing_malformed_wrong_token() {
  auto server = start_server();
  if (!server) return;
  const std::pair<std::string, int> variants[] = {
      {"", 401}, {"Authorization: bearer " + token(), 401}, {"Authorization: Bearer ", 401}, {"Authorization: Basic " + token(), 401},
      {"Authorization: Bearer wrong-token-0123456789abcdefghijklmn", 401},
      {"Authorization: Bearer " + token() + "\r\nAuthorization: Bearer " + token(), 400}};
  for (const auto& entry : secure_frontend::kRouteTable) {
    for (const auto& [line, status] : variants) {
      std::vector<std::string> lines;
      if (!line.empty()) lines.push_back(line);
      const std::string target = entry.prefix ? std::string{entry.path} + "fake-model" : std::string{entry.path};
      if (entry.method == "POST") lines.push_back("Content-Type: application/json");
      const auto r = raw(*server, raw_http::request(entry.method, target, authority(*server), lines, entry.method == "POST" ? chat_body() : ""));
      expect(r && r->status == status && r->body.find("fake-model") == std::string::npos,
             std::string{entry.method} + " " + target + " → " + std::to_string(status));
    }
  }
}

void cfg01_wildcard_bind_requires_host_aliases() {
  const auto dir = test_support::make_temp_dir("wild");
  test_support::ServerOptions options;
  options.config_literals = {{"listener.host", "\"0.0.0.0\""}, {"listener.allow_remote", "true"}, {"listener.allow_wildcard_bind", "true"}};
  options.engine_args = {"--record-file=" + test_support::utf8_path(dir / "record.txt")};
  auto server = test_support::ServerProcess::start(options);
  expect(server != nullptr, "the process starts");
  if (!server) return;
  const auto code = server->wait_exit(10s);
  expect(code.has_value() && *code != 0, "a wildcard bind without listener.host_aliases fails validation");
  expect(server->stderr_text().find("listener.host_aliases") != std::string::npos, "the error names the key");
  expect(!std::filesystem::exists(dir / "record.txt"), "no engine was spawned (the fake creates its record file at start)");
}
```

(`test_support::ipv6_loopback_available()` comes from Task 8b; include `<set>`, `<tuple>` in the test.) Register all ten names in `SECURE_FRONTEND_API_SERVER_CASES`; `web01_ipv6_bound_host_rules` also needs `SKIP_RETURN_CODE 77`: after the `secure_frontend_add_cases` call add `set_tests_properties(secure_frontend_api_server_tests.web01_ipv6_bound_host_rules PROPERTIES SKIP_RETURN_CODE 77)`.

- [ ] **Step 2: Run** — `cmake --build --preset linux-gcc && ctest --preset linux-gcc -R api_server_tests --output-on-failure`. Expected: PASS on first run (the rules were implemented in Tasks 3, 4, 6, 9). If a case fails, fix the owning file (policy → `http_policy.cpp`, order → `api_router.cpp`, config → `frontend_config.cpp`), re-run, and name the defect in the commit body. Record "acceptance tests, passed on first run" or the defect.

- [ ] **Step 3: Full suite and cross build**

```bash
ctest --preset linux-gcc | tail -1
cmake --build --preset windows-llvm-mingw-cross && ctest --preset windows-llvm-mingw-cross -R api_server_tests | tail -1
```

- [ ] **Step 4: Handoff row and commit** — row `| 10 | WEB-01..06, AUTH-01 (route table), NET-03 (wildcard needs aliases) | acceptance over TCP | api_server_tests.web01_bad_host_rejected_on_every_route, …web01_ipv6_bound_host_rules, …web02_bound_authority_and_localhost_alias_succeed, …web02_host_alias_from_config_succeeds, …web03_untrusted_origin_no_cors_header, …web04_default_emits_no_cors_header_anywhere, …web05_only_exact_configured_origin_allowed, …web06_preflight_matrix, …web06_cross_origin_post_without_trust_rejected, …auth01_every_route_rejects_missing_malformed_wrong_token, …cfg01_wildcard_bind_requires_host_aliases | … | … |`.

```bash
git -C "$(git rev-parse --show-toplevel)" add secure_frontend docs/AGENT_HANDOFF.md
git -C "$(git rev-parse --show-toplevel)" -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "test: WEB-01..06 and AUTH-01 over TCP on every route; wildcard bind needs aliases and spawns nothing (SP3)" \
  -m "Tests: api_server_tests.web01_bad_host_rejected_on_every_route, web01_ipv6_bound_host_rules, web02_bound_authority_and_localhost_alias_succeed, web02_host_alias_from_config_succeeds, web03_untrusted_origin_no_cors_header, web04_default_emits_no_cors_header_anywhere, web05_only_exact_configured_origin_allowed, web06_preflight_matrix, web06_cross_origin_post_without_trust_rejected, auth01_every_route_rejects_missing_malformed_wrong_token, cfg01_wildcard_bind_requires_host_aliases. <passed on first run | defects fixed: ...>. wine: <line>." \
  -m "Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>" -m "Claude-Session: <session link>"
```

---
## Task 11: Admission state, oldest-P1 eviction, per-remote cap and the lingering-close list (unit level)

**Files:**
- Create: `include/secure_frontend/connection_pool.hpp`, `src/admission.cpp`, `src/linger_list.cpp`, `tests/concurrency_test.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `Socket`, `PollEntry`, `poll_sockets`, `recv_some`, `send_some`, `half_close_and_drain` semantics (Task 8a); `HttpSocketServer::Accepted` (Task 8b), `AddressScope`; `make_error_response`, `serialize_response` (Task 1); `api_constants::kLingerListMax`, `kLingerBudget`, `kDrainBytes`, `kRefusalMaxBytes`.
- Produces (in `connection_pool.hpp`):

```cpp
using PeerKey = std::array<unsigned char, 17>;
enum class WorkerPhase { idle, header, busy };
class AdmissionState final {
 public:
  using Clock = std::chrono::steady_clock;
  AdmissionState(std::uint32_t workers, std::uint32_t per_remote_cap);
  enum class Kind { assign, evict, refuse };
  struct Decision final { Kind kind; std::size_t worker{0}; std::uint64_t connection{0}; };
  // Spec §2.3: remote over cap -> refuse; idle worker -> assign; else the worker longest in P1 -> evict; else refuse.
  // assign/evict put `worker` in P1 since `now` for the new peer and return the new connection id.
  [[nodiscard]] Decision admit(AddressScope scope, const PeerKey& peer, Clock::time_point now);
  // P1 -> busy for `connection`; false when that connection was evicted meanwhile.
  [[nodiscard]] bool header_complete(std::size_t worker, std::uint64_t connection);
  // busy/header -> idle and the peer count released; ignored for a stale (evicted) connection.
  void finished(std::size_t worker, std::uint64_t connection);
  [[nodiscard]] WorkerPhase phase(std::size_t worker) const;
  [[nodiscard]] std::size_t remote_count(const PeerKey& peer) const;
};
class LingerList final {
 public:
  using Clock = std::chrono::steady_clock;
  explicit LingerList(std::size_t max = api_constants::kLingerListMax);
  void add(Socket socket, Clock::time_point now);        // full: the oldest is closed first
  void append_poll_entries(std::vector<PollEntry>& entries) const;
  void service(Clock::time_point now);                    // drain non-blocking; close on EOF, error, 256 KiB or 100 ms
  [[nodiscard]] std::size_t size() const noexcept;
  void clear() noexcept;
};
[[nodiscard]] std::string refusal_response(std::chrono::system_clock::time_point now);   // fixed 503 server_busy, <= 512 bytes
void refuse_connection(Socket socket, LingerList& linger, LingerList::Clock::time_point now);  // one non-blocking send, half-close, linger
```

The class `AdmissionState` is not thread-safe; the pool (Task 12) calls it under its own mutex.

- [ ] **Step 1: Write the failing tests** — `tests/concurrency_test.cpp`:

```cpp
#include "secure_frontend/api_constants.hpp"
#include "secure_frontend/connection_pool.hpp"
#include "secure_frontend/http_socket_server.hpp"
#include "secure_frontend/socket_io.hpp"
#include "support/raw_http.hpp"
#include "support/test_support.hpp"

#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace std::chrono_literals;
using secure_frontend::AdmissionState;
int failures = 0;
void expect(bool condition, const std::string& message) {
  if (!condition) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
}
secure_frontend::PeerKey peer(unsigned char last) { secure_frontend::PeerKey k{}; k[0] = 4; k[1] = 192; k[2] = 168; k[3] = 1; k[4] = last; return k; }
const secure_frontend::PeerKey kLoop = [] { secure_frontend::PeerKey k{}; k[0] = 4; k[1] = 127; k[4] = 1; return k; }();

void dos01_oldest_header_phase_connection_is_evicted() {
  AdmissionState state(3, 3);
  const auto t0 = AdmissionState::Clock::time_point{} + 1s;
  const auto a = state.admit(secure_frontend::AddressScope::loopback, kLoop, t0);
  const auto b = state.admit(secure_frontend::AddressScope::loopback, kLoop, t0 + 10ms);
  const auto c = state.admit(secure_frontend::AddressScope::loopback, kLoop, t0 + 20ms);
  expect(a.kind == AdmissionState::Kind::assign && b.kind == AdmissionState::Kind::assign && c.kind == AdmissionState::Kind::assign,
         "three idle workers take three connections");
  expect(state.header_complete(a.worker, a.connection), "a authenticates and leaves P1");
  const auto d = state.admit(secure_frontend::AddressScope::loopback, kLoop, t0 + 30ms);
  expect(d.kind == AdmissionState::Kind::evict && d.worker == b.worker, "the longest-waiting P1 connection (b) is evicted, never a");
  expect(!state.header_complete(b.worker, b.connection), "the evicted connection learns it was evicted");
  expect(state.phase(b.worker) == secure_frontend::WorkerPhase::header, "the worker now serves the new connection in P1");
  expect(state.header_complete(c.worker, c.connection) && state.header_complete(d.worker, d.connection), "c and d authenticate");
  const auto e = state.admit(secure_frontend::AddressScope::loopback, kLoop, t0 + 40ms);
  expect(e.kind == AdmissionState::Kind::refuse, "no idle and no P1 worker: refuse");
  state.finished(b.worker, b.connection);   // stale id: ignored
  expect(state.phase(b.worker) == secure_frontend::WorkerPhase::busy, "a stale finish does not free the worker");
  state.finished(d.worker, d.connection);
  expect(state.phase(d.worker) == secure_frontend::WorkerPhase::idle, "the real finish frees it");
}

void dos01_remote_address_cap() {
  AdmissionState state(8, 2);
  const auto t0 = AdmissionState::Clock::time_point{} + 1s;
  const auto first = state.admit(secure_frontend::AddressScope::remote, peer(5), t0);
  const auto second = state.admit(secure_frontend::AddressScope::remote, peer(5), t0);
  const auto third = state.admit(secure_frontend::AddressScope::remote, peer(5), t0);
  expect(first.kind == AdmissionState::Kind::assign && second.kind == AdmissionState::Kind::assign, "two connections from one remote address");
  expect(third.kind == AdmissionState::Kind::refuse, "the third is refused although workers are idle");
  expect(state.admit(secure_frontend::AddressScope::remote, peer(6), t0).kind == AdmissionState::Kind::assign, "another address is admitted");
  for (int i = 0; i < 4; ++i) {
    expect(state.admit(secure_frontend::AddressScope::loopback, kLoop, t0).kind == AdmissionState::Kind::assign,
           "loopback is exempt from the per-address cap");
  }
  state.finished(first.worker, first.connection);
  expect(state.remote_count(peer(5)) == 1U, "finishing releases the address count");
  expect(state.admit(secure_frontend::AddressScope::remote, peer(5), t0).kind == AdmissionState::Kind::assign, "admitted again under the cap");
}

void dos01_linger_list_overflow_closes_oldest() {
  secure_frontend::Config listener{};
  listener.host = "127.0.0.1"; listener.port = 0; listener.has_api_key = true;
  std::string error;
  auto server = secure_frontend::HttpSocketServer::bind(listener, 128, &error);
  expect(server.has_value(), "listener: " + error);
  if (!server) return;
  secure_frontend::LingerList linger;
  std::vector<raw_http::Connection> clients;
  const auto now = secure_frontend::LingerList::Clock::now();
  for (int i = 0; i < 65; ++i) {
    auto client = raw_http::Connection::open("127.0.0.1", server->port());
    expect(client.has_value(), "client connects");
    if (!client) return;
    expect(client->send("GET /health HTTP/1.1\r\n"), "client sends a partial request it will never finish");
    secure_frontend::HttpSocketServer::Accepted accepted;
    auto status = secure_frontend::HttpSocketServer::AcceptStatus::timeout;
    for (int tries = 0; tries < 50 && status != secure_frontend::HttpSocketServer::AcceptStatus::accepted; ++tries) {
      status = server->accept_next(100ms, &accepted);
    }
    expect(status == secure_frontend::HttpSocketServer::AcceptStatus::accepted, "accepted");
    secure_frontend::refuse_connection(std::move(accepted.socket), linger, now);
    clients.push_back(std::move(*client));
  }
  expect(linger.size() == 64U, "at most kLingerListMax refused sockets are kept");
  const std::string oldest = clients.front().read_until_close(1s);
  expect(oldest.rfind("HTTP/1.1 503 ", 0) == 0 && oldest.find("server_busy") != std::string::npos, "the refused client read the fixed 503");
  expect(oldest.size() <= secure_frontend::api_constants::kRefusalMaxBytes, "refusal is at most 512 bytes");
  linger.service(now + 150ms);
  expect(linger.size() == 0U, "every lingering socket is closed 100 ms after its refusal");
}

void refusal_response_is_fixed_and_small() {
  const std::string r = secure_frontend::refusal_response(std::chrono::system_clock::now());
  expect(r.rfind("HTTP/1.1 503 Service Unavailable\r\n", 0) == 0 && r.find("Retry-After: 1\r\n") != std::string::npos &&
         r.find("\"code\":\"server_busy\"") != std::string::npos && r.size() <= secure_frontend::api_constants::kRefusalMaxBytes,
         "fixed 503 server_busy with Retry-After: 1 in at most 512 bytes (" + std::to_string(r.size()) + ")");
}
}  // namespace

int main(int argc, char** argv) {
  secure_frontend::WinsockGuard winsock;
  return secure_frontend_test::run_cases(argc, argv, {
      {"dos01_oldest_header_phase_connection_is_evicted", dos01_oldest_header_phase_connection_is_evicted},
      {"dos01_remote_address_cap", dos01_remote_address_cap},
      {"dos01_linger_list_overflow_closes_oldest", dos01_linger_list_overflow_closes_oldest},
      {"refusal_response_is_fixed_and_small", refusal_response_is_fixed_and_small},
  }, failures);
}
```

CMake:

```cmake
secure_frontend_add_library(secure_frontend_connection_pool
    SOURCES src/admission.cpp src/linger_list.cpp
    LINKS secure_frontend_http_socket_server secure_frontend_socket_io secure_frontend_api_core)
```

and in the tests block:

```cmake
    secure_frontend_add_test_executable(secure_frontend_concurrency_tests tests/concurrency_test.cpp
        secure_frontend_connection_pool)
    set(SECURE_FRONTEND_CONCURRENCY_CASES dos01_oldest_header_phase_connection_is_evicted dos01_remote_address_cap
        dos01_linger_list_overflow_closes_oldest refusal_response_is_fixed_and_small)
    secure_frontend_add_cases(secure_frontend_concurrency_tests ${SECURE_FRONTEND_CONCURRENCY_CASES} TIMEOUT 120)
```

- [ ] **Step 2: Run to see RED** — `cmake --preset linux-gcc 2>&1 | tail -2` → FAIL `Cannot find source file: src/admission.cpp`.

- [ ] **Step 3: Implement** — `include/secure_frontend/connection_pool.hpp` with the declarations above (includes `api_constants.hpp`, `config.hpp`, `http_socket_server.hpp`, `socket_io.hpp`, `<array>`, `<chrono>`, `<cstdint>`, `<deque>`, `<map>`, `<string>`, `<vector>`; private members `struct Slot { WorkerPhase phase{WorkerPhase::idle}; Clock::time_point since{}; std::uint64_t connection{0}; bool remote{false}; PeerKey peer{}; }; std::vector<Slot> slots_; std::uint32_t per_remote_cap_; std::map<PeerKey, std::size_t> remote_counts_; std::uint64_t next_connection_{0};` for `AdmissionState` and `struct Entry { Socket socket; Clock::time_point since; std::size_t drained{0}; }; std::deque<Entry> entries_; std::size_t max_;` for `LingerList`).

`src/admission.cpp`:

```cpp
#include "secure_frontend/connection_pool.hpp"

namespace secure_frontend {

AdmissionState::AdmissionState(const std::uint32_t workers, const std::uint32_t per_remote_cap)
    : slots_(workers), per_remote_cap_(per_remote_cap) {}

AdmissionState::Decision AdmissionState::admit(const AddressScope scope, const PeerKey& peer, const Clock::time_point now) {
  const bool remote = scope != AddressScope::loopback;
  if (remote && remote_counts_[peer] >= per_remote_cap_) return {Kind::refuse};
  std::size_t chosen = slots_.size();
  Kind kind = Kind::assign;
  for (std::size_t i = 0; i < slots_.size(); ++i) {
    if (slots_[i].phase == WorkerPhase::idle) { chosen = i; break; }
  }
  if (chosen == slots_.size()) {
    for (std::size_t i = 0; i < slots_.size(); ++i) {
      if (slots_[i].phase == WorkerPhase::header && (chosen == slots_.size() || slots_[i].since < slots_[chosen].since)) chosen = i;
    }
    if (chosen == slots_.size()) return {Kind::refuse};
    kind = Kind::evict;
    Slot& old = slots_[chosen];
    if (old.remote && --remote_counts_[old.peer] == 0U) remote_counts_.erase(old.peer);
  }
  Slot& slot = slots_[chosen];
  slot = Slot{WorkerPhase::header, now, ++next_connection_, remote, peer};
  if (remote) ++remote_counts_[peer];
  return {kind, chosen, slot.connection};
}

bool AdmissionState::header_complete(const std::size_t worker, const std::uint64_t connection) {
  Slot& slot = slots_.at(worker);
  if (slot.connection != connection || slot.phase != WorkerPhase::header) return false;
  slot.phase = WorkerPhase::busy;
  return true;
}

void AdmissionState::finished(const std::size_t worker, const std::uint64_t connection) {
  Slot& slot = slots_.at(worker);
  if (slot.connection != connection || slot.phase == WorkerPhase::idle) return;
  if (slot.remote) {
    const auto it = remote_counts_.find(slot.peer);
    if (it != remote_counts_.end() && --it->second == 0U) remote_counts_.erase(it);
  }
  slot.phase = WorkerPhase::idle;
}

WorkerPhase AdmissionState::phase(const std::size_t worker) const { return slots_.at(worker).phase; }

std::size_t AdmissionState::remote_count(const PeerKey& peer) const {
  const auto it = remote_counts_.find(peer);
  return it == remote_counts_.end() ? 0U : it->second;
}

}  // namespace secure_frontend
```

`src/linger_list.cpp`:

```cpp
#include "secure_frontend/connection_pool.hpp"

#include "secure_frontend/api_errors.hpp"
#include "secure_frontend/http_wire.hpp"

#include <array>

#if !defined(_WIN32)
#include <sys/socket.h>
#endif

namespace secure_frontend {

LingerList::LingerList(const std::size_t max) : max_(max) {}

void LingerList::add(Socket socket, const Clock::time_point now) {
  while (entries_.size() >= max_ && !entries_.empty()) entries_.pop_front();   // Socket's destructor closes it
  entries_.push_back(Entry{std::move(socket), now, 0U});
}

void LingerList::append_poll_entries(std::vector<PollEntry>& entries) const {
  for (const auto& entry : entries_) entries.push_back(PollEntry{entry.socket.get(), true, false});
}

void LingerList::service(const Clock::time_point now) {
  std::array<char, 4096> sink{};
  for (auto it = entries_.begin(); it != entries_.end();) {
    bool close = now - it->since >= api_constants::kLingerBudget;
    while (!close) {
      const IoResult r = recv_some(it->socket.get(), sink.data(), sink.size());
      if (r.status == IoStatus::would_block) break;
      if (r.status != IoStatus::done) { close = true; break; }
      it->drained += r.bytes;
      close = it->drained >= api_constants::kDrainBytes;
    }
    it = close ? entries_.erase(it) : std::next(it);
  }
}

std::size_t LingerList::size() const noexcept { return entries_.size(); }
void LingerList::clear() noexcept { entries_.clear(); }

std::string refusal_response(const std::chrono::system_clock::time_point now) {
  return serialize_response(make_error_response(ApiError::server_busy), now);
}

void refuse_connection(Socket socket, LingerList& linger, const LingerList::Clock::time_point now) {
  const std::string bytes = refusal_response(std::chrono::system_clock::now());
  (void)send_some(socket.get(), bytes);   // one non-blocking send; the accept thread never waits on a client
#if defined(_WIN32)
  (void)::shutdown(socket.get(), SD_SEND);
#else
  (void)::shutdown(socket.get(), SHUT_WR);
#endif
  linger.add(std::move(socket), now);
}

}  // namespace secure_frontend
```

- [ ] **Step 4: Run to see GREEN**

```bash
cmake --preset linux-gcc && cmake --build --preset linux-gcc && ctest --preset linux-gcc -R concurrency_tests --output-on-failure
ctest --preset linux-gcc | tail -1
cmake --build --preset windows-llvm-mingw-cross && ctest --preset windows-llvm-mingw-cross -R concurrency_tests | tail -1
```

- [ ] **Step 5: Handoff row and commit** — row `| 11 | DOS-01 (eviction, per-remote cap, linger list), §2.3, §2.4 | AdmissionState, LingerList, fixed refusal | concurrency_tests.dos01_oldest_header_phase_connection_is_evicted, …dos01_remote_address_cap, …dos01_linger_list_overflow_closes_oldest, …refusal_response_is_fixed_and_small | … | … |`.

```bash
git -C "$(git rev-parse --show-toplevel)" add secure_frontend docs/AGENT_HANDOFF.md
git -C "$(git rev-parse --show-toplevel)" -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "net: admission state with oldest-P1 eviction and per-remote cap; lingering close for refusals (SP3 §2.3, §2.4, DOS-01)" \
  -m "Tests: concurrency_tests.dos01_oldest_header_phase_connection_is_evicted, dos01_remote_address_cap, dos01_linger_list_overflow_closes_oldest, refusal_response_is_fixed_and_small. RED: <line>. wine: <line>." \
  -m "Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>" -m "Claude-Session: <session link>"
```

---

## Task 12: `ConnectionPool` — accept thread and workers; the server becomes concurrent

**Files:**
- Create: `src/connection_pool.cpp`, `tests/support/connection_scan.hpp`
- Modify: `include/secure_frontend/connection_pool.hpp`, `src/secure_server_main.cpp`, `tests/api_server_test.cpp`, `CMakeLists.txt`

**Interfaces:**
- Consumes: `AdmissionState`, `LingerList`, `refuse_connection` (Task 11); `HttpSocketServer::accept_next`, `shutdown_listener`, `native_handle` (Task 8b); `serve_connection`, `WorkerSignals`, `ConnectionResult` (Task 8b); SP2 `shutdown_signalled()` (anchor in `run`).
- Produces:

```cpp
struct PoolLimits final { std::uint32_t workers; std::uint32_t per_remote_cap; ConnectionLimits connection; };
struct FinishedConnection final { ConnectionResult result; AddressScope peer; std::chrono::milliseconds elapsed; };
using ConnectionObserver = std::function<void(const FinishedConnection&)>;
enum class PoolExit { stopped, listener_failed };
class ConnectionPool final {
 public:
  ConnectionPool(HttpSocketServer& listener, PoolLimits limits, RequestRouter& router, ConnectionObserver observer = {});
  ~ConnectionPool();                                  // joins (run_until normally already did)
  // Accept loop on the calling thread until stop() is true or the listener fails; then spec §2.8:
  // shutting_down set, on_shutdown() (main: gate.shutdown()), listener closed, lingering sockets closed, workers joined.
  PoolExit run_until(const std::function<bool()>& stop, const std::function<void()>& on_shutdown = {});
  [[nodiscard]] const std::atomic<bool>& shutting_down() const noexcept;
};
```

and in `tests/support/connection_scan.hpp`: `struct SocketRow { std::string kind; bool listening; std::uint16_t local_port; std::string local_address; }` (kind ∈ `tcp`, `tcp6`, `udp`, `udp6`, `unix`), `std::vector<SocketRow> sockets_of(long pid)` (Linux `/proc/<pid>/fd` + `/proc/<pid>/net/*`; Windows `GetExtendedTcpTable`/`GetExtendedUdpTable` filtered by pid), `std::size_t thread_count(long pid)`, `std::size_t handle_count(long pid)` (Linux: `/proc/<pid>/task` and `/proc/<pid>/fd` entries; Windows: Toolhelp thread snapshot and `GetProcessHandleCount`).

- [ ] **Step 1: Write the failing tests** — `tests/support/connection_scan.hpp`:

```cpp
#pragma once
// Test-only process socket/thread/handle inspection (spec §6.2 table scan, DOS-02 idle counts, NET-01).
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"
#include <iphlpapi.h>
#include <tlhelp32.h>
#else
#include <unistd.h>
#endif

namespace test_support {

struct SocketRow final {
  std::string kind;           // tcp, tcp6, udp, udp6, unix
  bool listening{false};
  std::uint16_t local_port{0};
  std::string local_address;  // kernel hex (Linux) or dotted/hex (Windows); compared only for loopback checks
};

#if defined(_WIN32)
inline std::vector<SocketRow> sockets_of(const long pid) {
  std::vector<SocketRow> rows;
  for (const ULONG family : {static_cast<ULONG>(AF_INET), static_cast<ULONG>(AF_INET6)}) {
    DWORD size = 0;
    GetExtendedTcpTable(nullptr, &size, FALSE, family, TCP_TABLE_OWNER_PID_ALL, 0);
    std::vector<unsigned char> buffer(size);
    if (GetExtendedTcpTable(buffer.data(), &size, FALSE, family, TCP_TABLE_OWNER_PID_ALL, 0) == NO_ERROR) {
      if (family == AF_INET) {
        const auto* table = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(buffer.data());
        for (DWORD i = 0; i < table->dwNumEntries; ++i) {
          const auto& r = table->table[i];
          if (static_cast<long>(r.dwOwningPid) != pid) continue;
          rows.push_back({"tcp", r.dwState == MIB_TCP_STATE_LISTEN, ntohs(static_cast<u_short>(r.dwLocalPort)), std::to_string(r.dwLocalAddr)});
        }
      } else {
        const auto* table = reinterpret_cast<const MIB_TCP6TABLE_OWNER_PID*>(buffer.data());
        for (DWORD i = 0; i < table->dwNumEntries; ++i) {
          const auto& r = table->table[i];
          if (static_cast<long>(r.dwOwningPid) != pid) continue;
          std::string addr(reinterpret_cast<const char*>(r.ucLocalAddr), 16U);
          rows.push_back({"tcp6", r.dwState == MIB_TCP_STATE_LISTEN, ntohs(static_cast<u_short>(r.dwLocalPort)), addr});
        }
      }
    }
    size = 0;
    GetExtendedUdpTable(nullptr, &size, FALSE, family, UDP_TABLE_OWNER_PID, 0);
    std::vector<unsigned char> udp(size);
    if (GetExtendedUdpTable(udp.data(), &size, FALSE, family, UDP_TABLE_OWNER_PID, 0) == NO_ERROR) {
      if (family == AF_INET) {
        const auto* table = reinterpret_cast<const MIB_UDPTABLE_OWNER_PID*>(udp.data());
        for (DWORD i = 0; i < table->dwNumEntries; ++i) {
          if (static_cast<long>(table->table[i].dwOwningPid) == pid) rows.push_back({"udp", false, ntohs(static_cast<u_short>(table->table[i].dwLocalPort)), {}});
        }
      } else {
        const auto* table = reinterpret_cast<const MIB_UDP6TABLE_OWNER_PID*>(udp.data());
        for (DWORD i = 0; i < table->dwNumEntries; ++i) {
          if (static_cast<long>(table->table[i].dwOwningPid) == pid) rows.push_back({"udp6", false, ntohs(static_cast<u_short>(table->table[i].dwLocalPort)), {}});
        }
      }
    }
  }
  return rows;
}
inline std::size_t thread_count(const long pid) {
  const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return 0;
  THREADENTRY32 entry{};
  entry.dwSize = sizeof(entry);
  std::size_t n = 0;
  for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
    if (static_cast<long>(entry.th32OwnerProcessID) == pid) ++n;
  }
  CloseHandle(snapshot);
  return n;
}
inline std::size_t handle_count(const long pid) {
  const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
  if (process == nullptr) return 0;
  DWORD count = 0;
  GetProcessHandleCount(process, &count);
  CloseHandle(process);
  return count;
}
#else
inline std::set<std::string> socket_inodes(const long pid) {
  std::set<std::string> inodes;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator("/proc/" + std::to_string(pid) + "/fd", ec)) {
    std::error_code link_error;
    const std::string target = std::filesystem::read_symlink(entry.path(), link_error).string();
    if (target.rfind("socket:[", 0) == 0) inodes.insert(target.substr(8, target.size() - 9));
  }
  return inodes;
}
inline std::vector<SocketRow> sockets_of(const long pid) {
  const auto inodes = socket_inodes(pid);
  std::vector<SocketRow> rows;
  for (const std::string kind : {"tcp", "tcp6", "udp", "udp6"}) {
    std::ifstream table("/proc/" + std::to_string(pid) + "/net/" + kind);
    std::string line;
    std::getline(table, line);   // header
    while (std::getline(table, line)) {
      std::istringstream fields(line);
      std::string slot, local, remote, state, queues, timer, retransmit, uid, timeout, inode;
      fields >> slot >> local >> remote >> state >> queues >> timer >> retransmit >> uid >> timeout >> inode;
      if (!inodes.count(inode)) continue;
      const auto colon = local.rfind(':');
      rows.push_back({kind, state == "0A", static_cast<std::uint16_t>(std::stoul(local.substr(colon + 1U), nullptr, 16)), local.substr(0, colon)});
    }
  }
  std::ifstream unix_table("/proc/" + std::to_string(pid) + "/net/unix");
  std::string line;
  std::getline(unix_table, line);
  while (std::getline(unix_table, line)) {
    std::istringstream fields(line);
    std::string num, refs, protocol, flags, type, st, inode;
    fields >> num >> refs >> protocol >> flags >> type >> st >> inode;
    if (inodes.count(inode)) rows.push_back({"unix", false, 0, {}});
  }
  return rows;
}
inline std::size_t count_entries(const std::string& dir) {
  std::size_t n = 0;
  std::error_code ec;
  for (auto it = std::filesystem::directory_iterator(dir, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) ++n;
  return n;
}
inline std::size_t thread_count(const long pid) { return count_entries("/proc/" + std::to_string(pid) + "/task"); }
inline std::size_t handle_count(const long pid) { return count_entries("/proc/" + std::to_string(pid) + "/fd"); }
#endif

// Loopback in the kernel's /proc hex form (127.x.y.z little-endian ends in "7F"; ::1 is ...01000000) or Windows numbers.
inline bool is_loopback_row(const SocketRow& row) {
#if defined(_WIN32)
  if (row.kind == "tcp") return (std::stoul(row.local_address) & 0xffU) == 127U;
  return row.local_address == std::string("\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\x01", 16);
#else
  if (row.kind == "tcp") return row.local_address.size() == 8U && row.local_address.substr(6) == "7F";
  return row.local_address == "00000000000000000000000001000000";
#endif
}

}  // namespace test_support
```

Append to `tests/api_server_test.cpp` (include `support/connection_scan.hpp`, `<atomic>`):

```cpp
void dos01_eviction_over_tcp_admits_new_client() {
  auto server = start_server({{"limits.http.header_deadline_ms", "30000"}});
  if (!server) return;
  std::vector<raw_http::Connection> holders;
  for (int i = 0; i < 8; ++i) {
    auto c = raw_http::Connection::open("127.0.0.1", server->port());
    expect(c && c->send("GET /health HTTP/1.1\r\nHost: "), "slow-header client " + std::to_string(i));
    if (c) holders.push_back(std::move(*c));
    std::this_thread::sleep_for(20ms);
  }
  std::this_thread::sleep_for(200ms);   // all eight workers now sit in P1
  const auto r = raw(*server, raw_http::request("GET", "/health", authority(*server), {raw_http::bearer(token())}), 5s);
  expect(r && r->status == 200, "a ninth client completes while eight workers are held in P1");
  bool someone_closed = false;
  for (auto& h : holders) someone_closed = someone_closed || h.wait_closed(100ms);
  expect(someone_closed, "one P1 client was evicted (closed without a response)");
}

void dos01_slowloris_cannot_starve_a_real_client() {
  auto server = start_server({{"limits.http.header_deadline_ms", "30000"}});
  if (!server) return;
  std::atomic<bool> stop{false};
  std::vector<std::thread> lorises;
  for (int i = 0; i < 16; ++i) {
    lorises.emplace_back([&stop, port = server->port()] {
      while (!stop) {
        auto c = raw_http::Connection::open("127.0.0.1", port);
        if (!c) continue;
        const std::string head = "GET /health HTTP/1.1\r\nHost: x\r\nX-Slow: ";
        for (std::size_t i = 0; i < head.size() && !stop; ++i) {
          if (!c->send(std::string_view{head}.substr(i, 1U))) break;
          std::this_thread::sleep_for(1s);
        }
      }
    });
  }
  std::this_thread::sleep_for(500ms);
  const auto started = std::chrono::steady_clock::now();
  const auto r = raw(*server, chat_request(*server, chat_body()), 20s);
  expect(r && r->status == 200, "a real chat completes under 16 one-byte-per-second header clients");
  expect(std::chrono::steady_clock::now() - started < 10s, "and promptly");
  stop = true;
  for (auto& t : lorises) t.join();
}

void dos01_slow_body_clients_cannot_starve_a_real_client() {
  auto server = start_server();
  if (!server) return;
  std::vector<raw_http::Connection> dribblers;
  for (int i = 0; i < 16; ++i) {
    auto c = raw_http::Connection::open("127.0.0.1", server->port());
    const auto started = std::chrono::steady_clock::now();
    expect(c && c->send("POST /v1/chat/completions HTTP/1.1\r\nHost: " + authority(*server) +
                        "\r\nAuthorization: Bearer nope-0123456789abcdefghijklmnopqrstuvw\r\nContent-Type: application/json\r\n"
                        "Content-Length: 40000\r\n\r\n{"), "header with a bad token, then a dribbled body");
    const std::string reply = c ? c->read_until_close(2s) : "";
    expect(reply.rfind("HTTP/1.1 401", 0) == 0 && std::chrono::steady_clock::now() - started < 1s, "401 at once, connection closed");
    if (c) dribblers.push_back(std::move(*c));
  }
  const auto r = raw(*server, chat_request(*server, chat_body()), 10s);
  expect(r && r->status == 200, "a real chat completes");
}

void net01_workers_add_no_listener() {
  auto server = start_server();
  if (!server) return;
  std::vector<std::thread> clients;
  for (int i = 0; i < 20; ++i) {
    clients.emplace_back([&server, i] {
      if (i % 2 == 0) (void)raw(*server, raw_http::request("GET", i % 4 == 0 ? "/health" : "/v1/models", authority(*server), {raw_http::bearer(token())}));
      else (void)raw(*server, chat_request(*server, chat_body()));
    });
  }
  for (auto& t : clients) t.join();
  const auto rows = test_support::sockets_of(server->pid());
  std::size_t listening = 0;
  for (const auto& row : rows) {
    if (!row.listening) continue;
    ++listening;
    expect(row.local_port == server->port() && test_support::is_loopback_row(row), "the only listener is the loopback one");
  }
  expect(listening == 1U, "exactly one listening socket after 20 mixed requests (" + std::to_string(listening) + ")");
}

void net01_ipv6_loopback_listener_only() {
  if (!test_support::ipv6_loopback_available()) test_support::skip("no IPv6 loopback on this host");
  test_support::ServerOptions options;
  options.config_literals = {{"listener.host", "\"::1\""}};
  auto server = test_support::ServerProcess::start(options);
  expect(server != nullptr, "server starts on ::1");
  if (!server) return;
  std::optional<raw_http::Response> r;
  for (int i = 0; i < 100 && !(r && r->status == 200); ++i) {
    r = raw_http::exchange("::1", server->port(), raw_http::request("GET", "/health", "[::1]:" + std::to_string(server->port()), {raw_http::bearer(token())}));
    if (!(r && r->status == 200)) std::this_thread::sleep_for(100ms);
  }
  expect(r && r->status == 200, "served on ::1");
  std::size_t listening = 0;
  for (const auto& row : test_support::sockets_of(server->pid())) {
    if (!row.listening) continue;
    ++listening;
    expect(row.kind == "tcp6" && test_support::is_loopback_row(row), "the listener is ::1 only");
  }
  expect(listening == 1U, "exactly one listener");
  expect(!raw_http::Connection::open("127.0.0.1", server->port()).has_value(), "nothing listens on IPv4");
}
```

Register the five cases in `SECURE_FRONTEND_API_SERVER_CASES`; add `set_tests_properties(secure_frontend_api_server_tests.net01_ipv6_loopback_listener_only PROPERTIES SKIP_RETURN_CODE 77)`; on Windows link `iphlpapi` into `secure_frontend_api_server_tests` (`if(WIN32) target_link_libraries(secure_frontend_api_server_tests PRIVATE iphlpapi) endif()`).

- [ ] **Step 2: Run to see RED** — `cmake --build --preset linux-gcc && ctest --preset linux-gcc -R 'api_server_tests\.(dos01_eviction|dos01_slowloris|dos01_slow_body_clients)' --output-on-failure`. Expected: FAIL — the serial server answers nothing while a slow-header client holds it ("a ninth client completes while eight workers are held in P1"). Record the line.

- [ ] **Step 3: Implement** — add to `connection_pool.hpp` the declarations from Interfaces plus private members:

```cpp
 private:
  struct Worker final {
    std::thread thread;
    std::atomic<bool> abort{false};
    std::optional<HttpSocketServer::Accepted> handoff;
    std::uint64_t connection{0};
    AddressScope peer{AddressScope::remote};
    std::condition_variable wake;
  };
  void worker_loop(std::size_t index);
  HttpSocketServer& listener_;
  PoolLimits limits_;
  RequestRouter& router_;
  ConnectionObserver observer_;
  std::mutex mutex_;
  AdmissionState admission_;
  LingerList linger_;
  std::vector<std::unique_ptr<Worker>> workers_;
  std::atomic<bool> shutting_down_{false};
  bool stopping_{false};
```

`src/connection_pool.cpp`:

```cpp
#include "secure_frontend/connection_pool.hpp"

namespace secure_frontend {

ConnectionPool::ConnectionPool(HttpSocketServer& listener, PoolLimits limits, RequestRouter& router, ConnectionObserver observer)
    : listener_(listener), limits_(limits), router_(router), observer_(std::move(observer)),
      admission_(limits.workers, limits.per_remote_cap) {
  for (std::uint32_t i = 0; i < limits.workers; ++i) workers_.push_back(std::make_unique<Worker>());
  for (std::size_t i = 0; i < workers_.size(); ++i) workers_[i]->thread = std::thread([this, i] { worker_loop(i); });
}

ConnectionPool::~ConnectionPool() {
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
    shutting_down_ = true;
  }
  for (auto& w : workers_) w->wake.notify_all();
  for (auto& w : workers_) if (w->thread.joinable()) w->thread.join();
}

const std::atomic<bool>& ConnectionPool::shutting_down() const noexcept { return shutting_down_; }

void ConnectionPool::worker_loop(const std::size_t index) {
  Worker& self = *workers_[index];
  std::unique_lock lock(mutex_);
  for (;;) {
    self.wake.wait(lock, [&] { return self.handoff.has_value() || stopping_; });
    if (!self.handoff.has_value()) return;
    HttpSocketServer::Accepted accepted = std::move(*self.handoff);
    self.handoff.reset();
    self.abort = false;
    const std::uint64_t connection = self.connection;
    lock.unlock();
    const WorkerSignals signals{&self.abort, &shutting_down_, [this, index, connection] {
                                  std::lock_guard guard(mutex_);
                                  return admission_.header_complete(index, connection);
                                }};
    const ConnectionResult result = serve_connection(accepted.socket.get(), router_, limits_.connection, signals, accepted.accepted_at);
    accepted.socket.reset();
    if (observer_) {
      observer_(FinishedConnection{result, accepted.peer_scope,
                                   std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - accepted.accepted_at)});
    }
    lock.lock();
    admission_.finished(index, connection);   // ignored when an eviction already handed this worker a new connection
  }
}

PoolExit ConnectionPool::run_until(const std::function<bool()>& stop, const std::function<void()>& on_shutdown) {
  PoolExit exit = PoolExit::stopped;
  std::vector<PollEntry> entries;
  while (!stop()) {
    entries.clear();
    entries.push_back(PollEntry{listener_.native_handle(), true, false});
    linger_.append_poll_entries(entries);
    (void)poll_sockets(entries, api_constants::kAcceptPollSlice);
    linger_.service(std::chrono::steady_clock::now());
    if (!entries.front().readable) continue;
    HttpSocketServer::Accepted accepted;
    const auto status = listener_.accept_next(std::chrono::milliseconds(0), &accepted);
    if (status == HttpSocketServer::AcceptStatus::listener_failed) { exit = PoolExit::listener_failed; break; }
    if (status == HttpSocketServer::AcceptStatus::stopped) break;
    if (status != HttpSocketServer::AcceptStatus::accepted) continue;
    std::unique_lock lock(mutex_);
    const auto decision = admission_.admit(accepted.peer_scope, accepted.peer_key, accepted.accepted_at);
    if (decision.kind == AdmissionState::Kind::refuse) {
      lock.unlock();
      refuse_connection(std::move(accepted.socket), linger_, std::chrono::steady_clock::now());
      continue;
    }
    Worker& worker = *workers_[decision.worker];
    worker.connection = decision.connection;
    worker.peer = accepted.peer_scope;
    worker.handoff = std::move(accepted);
    if (decision.kind == AdmissionState::Kind::evict) worker.abort = true;   // seen within one 20 ms slice
    worker.wake.notify_one();
  }
  // Spec §2.8.
  {
    std::lock_guard lock(mutex_);
    shutting_down_ = true;
  }
  if (on_shutdown) on_shutdown();
  listener_.shutdown_listener();
  linger_.clear();
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
    for (auto& w : workers_) {
      w->abort = true;
      w->handoff.reset();   // a connection handed over but not yet started is closed unanswered
    }
  }
  for (auto& w : workers_) w->wake.notify_all();
  for (auto& w : workers_) if (w->thread.joinable()) w->thread.join();
  return exit;
}

}  // namespace secure_frontend
```

(A worker whose `serve_connection` is in P5/P6 when shutdown starts sees `shutting_down` through its `RequestContext` and answers `server_shutting_down` — Task 17; one in P1/P3 closes at once.) Add `src/connection_pool.cpp` to `secure_frontend_connection_pool` and `#include <condition_variable>`, `<functional>`, `<mutex>`, `<thread>` to the header.

`src/secure_server_main.cpp`, in `run` — replace SP2's serve loop (anchor: the loop that calls `server->serve_one(router, limits, &error)`, whose condition polls `shutdown_signalled()`) with:

```cpp
  secure_frontend::ConnectionPool pool(*server,
      secure_frontend::PoolLimits{.workers = config.limits.concurrency.max_concurrent_requests,
                                  .per_remote_cap = config.limits.concurrency.max_connections_per_remote_address,
                                  .connection = limits},
      router);
  const auto pool_exit = pool.run_until([] { return secure_frontend::shutdown_signalled(); });
  if (pool_exit == secure_frontend::PoolExit::listener_failed) {
    std::cerr << "listener stopped\n";
    // fall through to SP2's orderly supervisor shutdown, then exit non-zero exactly as SP2's loop did on listener failure
  }
```

and keep SP2's shutdown sequence that followed the loop (`supervisor.shutdown(...)`, `notify_shutdown_complete()`, exit code) unchanged after it; the `ConnectionPool` is constructed after `supervisor.start()` and destroyed (it already joined) before `supervisor.shutdown(...)`. Link `strata-secure-server` with `secure_frontend_connection_pool`.

- [ ] **Step 4: Run to see GREEN**

```bash
cmake --build --preset linux-gcc && ctest --preset linux-gcc -R 'api_server_tests|server_tests|concurrency' --output-on-failure
ctest --preset linux-gcc | tail -1
cmake --build --preset windows-llvm-mingw-cross && ctest --preset windows-llvm-mingw-cross -R 'api_server_tests|server_tests' | tail -1
```

Expected: all pass, including SP2's `proc05_*` shutdown cases (Review Focus of SP2: SIGTERM still exits 0 within 5 s).

- [ ] **Step 5: Handoff row and commit** — row `| 12 | DOS-01 (slowloris, slow body, eviction over TCP), NET-01 (workers add no listener), §2.1, §2.8 | ConnectionPool: accept thread + 8 workers; server concurrent | api_server_tests.dos01_eviction_over_tcp_admits_new_client, …dos01_slowloris_cannot_starve_a_real_client, …dos01_slow_body_clients_cannot_starve_a_real_client, …net01_workers_add_no_listener, …net01_ipv6_loopback_listener_only | … | … |`.

```bash
git -C "$(git rev-parse --show-toplevel)" add secure_frontend docs/AGENT_HANDOFF.md
git -C "$(git rev-parse --show-toplevel)" -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "net: accept thread plus fixed worker pool with oldest-P1 eviction; the server answers while one client stalls (SP3 §2.1-2.4, DOS-01, NET-01)" \
  -m "Tests: api_server_tests.dos01_eviction_over_tcp_admits_new_client, dos01_slowloris_cannot_starve_a_real_client, dos01_slow_body_clients_cannot_starve_a_real_client, net01_workers_add_no_listener, net01_ipv6_loopback_listener_only. RED: <line>. wine: <line>." \
  -m "Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>" -m "Claude-Session: <session link>"
```

---

## Task 13: `SupervisorPort` and `GenerationGate`

**Files:**
- Create: `include/secure_frontend/generation_gate.hpp`, `src/supervisor_port.cpp`, `src/generation_gate.cpp`
- Modify: `tests/concurrency_test.cpp`, `CMakeLists.txt`

**Interfaces:**
- Consumes: SP2 `EngineSupervisor` (`snapshot`, `acquire`, `report_fatal`), `Lease`, `KillReason`, `HealthSnapshot`, `SupervisorState`; test support `test_support::FakeFactory`, `FakeChild::push_line`, `ManualClock`, `wait_generation_ready` (`tests/support/fake_supervised_process.hpp`); `api_constants::kStreamPollSlice`.
- Produces: `SupervisorPort`, `GateRefusal`, `GateLimits`, `GenerationGate` exactly as in "Shared interfaces", plus `GenerationGate(GateLimits limits, std::function<std::chrono::steady_clock::time_point()> now = &std::chrono::steady_clock::now)`.

Algorithm (spec §2.5): `admit` first takes one `snapshot()` (fast fail: `starting`, `stopping`, `backoff`, `circuit_open` → `engine_unavailable`; `shutting_down`, `stopped` → `shutting_down`). If the slot is free and nobody waits, the caller becomes head at once; otherwise it gets a ticket if fewer than `max_queued` wait, else `busy`. A waiter wakes on `release()` or every 20 ms to check `client_gone` (→ `cancelled`), the deadline (→ `timeout`) and shutdown (→ `shutting_down`); it becomes head only when it is first in line and the slot is free. The head holds the slot and loops: `ready` → `acquire()` (a lease → return it; empty → release the slot, `engine_unavailable`); `generating` → re-check every 20 ms (a release still settling in SP2) until the deadline; any other state → release the slot and refuse as above. Every refusal by a head calls `release()` so the next ticket proceeds; the gate never wakes a head while a `Lease` exists, because only the holder calls `release()`, after destroying its `Lease`.

- [ ] **Step 1: Write the failing tests** — append to `tests/concurrency_test.cpp` (include `secure_frontend/generation_gate.hpp`, `support/fake_supervised_process.hpp`, `<optional>`, `<mutex>`):

```cpp
struct GateFixture final {
  test_support::FakeFactory fakes{[](test_support::FakeChild& child, std::uint64_t) { child.push_line("READY 4096 stop"); }};
  std::shared_ptr<test_support::ManualClock> clock = std::make_shared<test_support::ManualClock>();
  secure_frontend::EngineSupervisor supervisor{{}, fakes.factory(), clock};
  secure_frontend::SupervisorPort port{supervisor};
  GateFixture() {
    supervisor.start();
    expect(test_support::wait_generation_ready(supervisor, 1U, 5s), "fake generation ready");
  }
  ~GateFixture() { supervisor.shutdown(5s); }
};
auto deadline_in(std::chrono::milliseconds d) { return std::chrono::steady_clock::now() + d; }
const std::function<bool()> kStaying = [] { return false; };

void gate_first_request_gets_the_slot() {
  GateFixture f;
  secure_frontend::GenerationGate gate({.max_queued = 4, .queue_timeout = 1000ms});
  auto result = gate.admit(f.port, deadline_in(1s), kStaying);
  expect(std::holds_alternative<secure_frontend::Lease>(result), "the first chat gets the lease at once");
  expect(f.port.snapshot().state == secure_frontend::SupervisorState::generating, "SP2 is generating");
}

void gate_queue_full_is_busy() {
  GateFixture f;
  secure_frontend::GenerationGate gate({.max_queued = 1, .queue_timeout = 5000ms});
  auto holder = gate.admit(f.port, deadline_in(1s), kStaying);
  std::thread waiter([&] { (void)gate.admit(f.port, deadline_in(2s), kStaying); });
  for (int i = 0; i < 100 && gate.waiting() != 1U; ++i) std::this_thread::sleep_for(5ms);
  const auto started = std::chrono::steady_clock::now();
  auto refused = gate.admit(f.port, deadline_in(5s), kStaying);
  expect(std::holds_alternative<secure_frontend::GateRefusal>(refused) &&
         std::get<secure_frontend::GateRefusal>(refused) == secure_frontend::GateRefusal::busy, "a full queue answers busy");
  expect(std::chrono::steady_clock::now() - started < 100ms, "at once");
  { auto lease = std::get<secure_frontend::Lease>(std::move(holder)); }
  gate.release();
  waiter.join();
}

void gate_fifo_order() {
  GateFixture f;
  secure_frontend::GenerationGate gate({.max_queued = 3, .queue_timeout = 10000ms});
  auto holder = std::optional<secure_frontend::Lease>{std::get<secure_frontend::Lease>(gate.admit(f.port, deadline_in(1s), kStaying))};
  std::mutex order_mutex;
  std::vector<int> order;
  std::vector<std::thread> waiters;
  for (int i = 0; i < 3; ++i) {
    waiters.emplace_back([&, i] {
      auto r = gate.admit(f.port, deadline_in(10s), kStaying);
      if (!std::holds_alternative<secure_frontend::Lease>(r)) return;
      { std::lock_guard g(order_mutex); order.push_back(i); }
      { auto lease = std::get<secure_frontend::Lease>(std::move(r)); }
      gate.release();
    });
    for (int t = 0; t < 100 && gate.waiting() != static_cast<std::size_t>(i + 1); ++t) std::this_thread::sleep_for(5ms);
  }
  holder.reset();
  gate.release();
  for (auto& t : waiters) t.join();
  expect(order == std::vector<int>{0, 1, 2}, "strict FIFO by admission time");
}

void gate_timeout_returns_timeout() {
  GateFixture f;
  secure_frontend::GenerationGate gate({.max_queued = 1, .queue_timeout = 300ms});
  auto holder = gate.admit(f.port, deadline_in(1s), kStaying);
  const auto started = std::chrono::steady_clock::now();
  auto r = gate.admit(f.port, deadline_in(300ms), kStaying);
  const auto elapsed = std::chrono::steady_clock::now() - started;
  expect(std::holds_alternative<secure_frontend::GateRefusal>(r) && std::get<secure_frontend::GateRefusal>(r) == secure_frontend::GateRefusal::timeout,
         "the queue deadline answers timeout");
  expect(elapsed >= 280ms && elapsed < 1s, "at the deadline");
  expect(gate.waiting() == 0U, "the ticket left the queue");
}

void gate_cancelled_waiter_leaves_queue() {
  GateFixture f;
  secure_frontend::GenerationGate gate({.max_queued = 2, .queue_timeout = 5000ms});
  auto holder = gate.admit(f.port, deadline_in(1s), kStaying);
  std::atomic<bool> gone{false};
  std::optional<secure_frontend::GateRefusal> refusal;
  std::thread waiter([&] {
    auto r = gate.admit(f.port, deadline_in(5s), [&gone] { return gone.load(); });
    if (std::holds_alternative<secure_frontend::GateRefusal>(r)) refusal = std::get<secure_frontend::GateRefusal>(r);
  });
  for (int i = 0; i < 100 && gate.waiting() != 1U; ++i) std::this_thread::sleep_for(5ms);
  gone = true;
  waiter.join();
  expect(refusal == std::optional<secure_frontend::GateRefusal>{secure_frontend::GateRefusal::cancelled}, "a gone client leaves silently");
  expect(gate.waiting() == 0U, "and frees its queue place");
}

void gate_fast_fails_when_engine_not_ready() {
  test_support::FakeFactory silent([](test_support::FakeChild&, std::uint64_t) {});   // never READY: stays starting
  secure_frontend::EngineSupervisor supervisor({}, silent.factory(), std::make_shared<test_support::ManualClock>());
  supervisor.start();
  secure_frontend::SupervisorPort port(supervisor);
  secure_frontend::GenerationGate gate({.max_queued = 4, .queue_timeout = 5000ms});
  const auto started = std::chrono::steady_clock::now();
  auto r = gate.admit(port, deadline_in(5s), kStaying);
  expect(std::holds_alternative<secure_frontend::GateRefusal>(r) &&
         std::get<secure_frontend::GateRefusal>(r) == secure_frontend::GateRefusal::engine_unavailable, "starting: engine_unavailable");
  expect(std::chrono::steady_clock::now() - started < 100ms, "never waits for READY (SP2 V11)");
  supervisor.shutdown(5s);
}

void gate_shutdown_refuses_waiters() {
  GateFixture f;
  secure_frontend::GenerationGate gate({.max_queued = 2, .queue_timeout = 5000ms});
  auto holder = gate.admit(f.port, deadline_in(1s), kStaying);
  std::optional<secure_frontend::GateRefusal> refusal;
  std::thread waiter([&] {
    auto r = gate.admit(f.port, deadline_in(5s), kStaying);
    if (std::holds_alternative<secure_frontend::GateRefusal>(r)) refusal = std::get<secure_frontend::GateRefusal>(r);
  });
  for (int i = 0; i < 100 && gate.waiting() != 1U; ++i) std::this_thread::sleep_for(5ms);
  gate.shutdown();
  waiter.join();
  expect(refusal == std::optional<secure_frontend::GateRefusal>{secure_frontend::GateRefusal::shutting_down}, "queued tickets get shutting_down");
  auto late = gate.admit(f.port, deadline_in(1s), kStaying);
  expect(std::holds_alternative<secure_frontend::GateRefusal>(late), "nothing is admitted after shutdown");
}
```

Register `gate_first_request_gets_the_slot gate_queue_full_is_busy gate_fifo_order gate_timeout_returns_timeout gate_cancelled_waiter_leaves_queue gate_fast_fails_when_engine_not_ready gate_shutdown_refuses_waiters` in `SECURE_FRONTEND_CONCURRENCY_CASES`; link `secure_frontend_concurrency_tests` with `secure_frontend_generation_gate` (and add `secure_frontend_test_paths(secure_frontend_concurrency_tests)` if `fake_supervised_process.hpp` needs it).

- [ ] **Step 2: Run to see RED** — `cmake --build --preset linux-gcc 2>&1 | grep -m1 error` → FAIL `secure_frontend/generation_gate.hpp: No such file or directory`.

- [ ] **Step 3: Implement** — `include/secure_frontend/generation_gate.hpp`:

```cpp
#pragma once

#include "secure_frontend/engine_supervisor.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <variant>

namespace secure_frontend {

// Serializes SP3's explicit calls into SP2 (spec §2.5, D-1). ~Lease and
// EngineSupervisor::shutdown() do not pass through here; they rely on SP2's
// own mutex.
class SupervisorPort final {
 public:
  explicit SupervisorPort(EngineSupervisor& supervisor) noexcept : supervisor_(supervisor) {}
  [[nodiscard]] HealthSnapshot snapshot();
  [[nodiscard]] std::optional<Lease> acquire();
  void report_fatal(const Lease& lease, KillReason reason);

 private:
  EngineSupervisor& supervisor_;
  std::mutex mutex_;
};

enum class GateRefusal { busy, timeout, cancelled, engine_unavailable, shutting_down };

struct GateLimits final {
  std::uint32_t max_queued;
  std::chrono::milliseconds queue_timeout;
};

// FIFO of tickets owning the single running slot (spec §2.5).
class GenerationGate final {
 public:
  using Clock = std::chrono::steady_clock;
  explicit GenerationGate(GateLimits limits, std::function<Clock::time_point()> now = &Clock::now);
  [[nodiscard]] std::variant<Lease, GateRefusal> admit(SupervisorPort& port, Clock::time_point deadline,
                                                       const std::function<bool()>& client_gone);
  // Called by the holder after its Lease was destroyed; wakes the next head.
  void release() noexcept;
  void shutdown() noexcept;
  [[nodiscard]] std::size_t waiting() const;

 private:
  void leave(std::uint64_t ticket);
  GateLimits limits_;
  std::function<Clock::time_point()> now_;
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::deque<std::uint64_t> queue_;
  bool running_{false};
  bool shutdown_{false};
  std::uint64_t next_ticket_{0};
};

}  // namespace secure_frontend
```

`src/supervisor_port.cpp`:

```cpp
#include "secure_frontend/generation_gate.hpp"

namespace secure_frontend {

HealthSnapshot SupervisorPort::snapshot() {
  std::lock_guard lock(mutex_);
  return supervisor_.snapshot();
}

std::optional<Lease> SupervisorPort::acquire() {
  std::lock_guard lock(mutex_);
  return supervisor_.acquire();
}

void SupervisorPort::report_fatal(const Lease& lease, const KillReason reason) {
  std::lock_guard lock(mutex_);
  supervisor_.report_fatal(lease, reason);
}

}  // namespace secure_frontend
```

`src/generation_gate.cpp`:

```cpp
#include "secure_frontend/generation_gate.hpp"

#include "secure_frontend/api_constants.hpp"

#include <algorithm>
#include <thread>

namespace secure_frontend {
namespace {

std::optional<GateRefusal> refusal_for(const SupervisorState state) {
  switch (state) {
    case SupervisorState::ready:
    case SupervisorState::generating: return std::nullopt;
    case SupervisorState::shutting_down:
    case SupervisorState::stopped: return GateRefusal::shutting_down;
    default: return GateRefusal::engine_unavailable;
  }
}

}  // namespace

GenerationGate::GenerationGate(const GateLimits limits, std::function<Clock::time_point()> now)
    : limits_(limits), now_(std::move(now)) {}

void GenerationGate::leave(const std::uint64_t ticket) {
  queue_.erase(std::remove(queue_.begin(), queue_.end(), ticket), queue_.end());
  changed_.notify_all();
}

std::variant<Lease, GateRefusal> GenerationGate::admit(SupervisorPort& port, const Clock::time_point deadline,
                                                       const std::function<bool()>& client_gone) {
  if (const auto refusal = refusal_for(port.snapshot().state)) return *refusal;
  std::unique_lock lock(mutex_);
  if (shutdown_) return GateRefusal::shutting_down;
  if (running_ || !queue_.empty()) {
    if (queue_.size() >= limits_.max_queued) return GateRefusal::busy;
    const std::uint64_t ticket = ++next_ticket_;
    queue_.push_back(ticket);
    for (;;) {
      if (shutdown_) { leave(ticket); return GateRefusal::shutting_down; }
      if (!running_ && queue_.front() == ticket) { queue_.pop_front(); break; }
      if (now_() >= deadline) { leave(ticket); return GateRefusal::timeout; }
      lock.unlock();
      const bool gone = client_gone && client_gone();
      lock.lock();
      if (gone) { leave(ticket); return GateRefusal::cancelled; }
      changed_.wait_for(lock, api_constants::kStreamPollSlice);
    }
  }
  running_ = true;   // this caller is the head and holds the slot
  lock.unlock();
  for (;;) {
    const HealthSnapshot snapshot = port.snapshot();
    if (snapshot.state == SupervisorState::ready) {
      if (auto lease = port.acquire()) return std::move(*lease);
      release();
      return GateRefusal::engine_unavailable;
    }
    if (snapshot.state != SupervisorState::generating) {
      release();
      return refusal_for(snapshot.state).value_or(GateRefusal::engine_unavailable);
    }
    {
      std::lock_guard guard(mutex_);
      if (shutdown_) { running_ = false; changed_.notify_all(); return GateRefusal::shutting_down; }
    }
    if (now_() >= deadline) { release(); return GateRefusal::timeout; }
    if (client_gone && client_gone()) { release(); return GateRefusal::cancelled; }
    std::this_thread::sleep_for(api_constants::kStreamPollSlice);
  }
}

void GenerationGate::release() noexcept {
  std::lock_guard lock(mutex_);
  running_ = false;
  changed_.notify_all();
}

void GenerationGate::shutdown() noexcept {
  std::lock_guard lock(mutex_);
  shutdown_ = true;
  changed_.notify_all();
}

std::size_t GenerationGate::waiting() const {
  std::lock_guard lock(mutex_);
  return queue_.size();
}

}  // namespace secure_frontend
```

CMake: `secure_frontend_add_library(secure_frontend_generation_gate SOURCES src/supervisor_port.cpp src/generation_gate.cpp LINKS secure_frontend_engine_supervisor secure_frontend_api_core)`.

- [ ] **Step 4: Run to see GREEN**

```bash
cmake --build --preset linux-gcc && ctest --preset linux-gcc -R concurrency_tests --output-on-failure
ctest --preset linux-gcc | tail -1
cmake --build --preset windows-llvm-mingw-cross && ctest --preset windows-llvm-mingw-cross -R concurrency_tests | tail -1
```

- [ ] **Step 5: Handoff row and commit** — row `| 13 | DOS-02 (unit), §2.5, D-1 | SupervisorPort (one mutex), GenerationGate (FIFO, busy, timeout, cancel, fast fail, shutdown) | concurrency_tests.gate_first_request_gets_the_slot, …gate_queue_full_is_busy, …gate_fifo_order, …gate_timeout_returns_timeout, …gate_cancelled_waiter_leaves_queue, …gate_fast_fails_when_engine_not_ready, …gate_shutdown_refuses_waiters | … | … |`.

```bash
git -C "$(git rev-parse --show-toplevel)" add secure_frontend docs/AGENT_HANDOFF.md
git -C "$(git rev-parse --show-toplevel)" -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "engine: SupervisorPort serializes SP3's calls; FIFO generation gate owns the running slot (SP3 §2.5)" \
  -m "Tests: concurrency_tests.gate_first_request_gets_the_slot, gate_queue_full_is_busy, gate_fifo_order, gate_timeout_returns_timeout, gate_cancelled_waiter_leaves_queue, gate_fast_fails_when_engine_not_ready, gate_shutdown_refuses_waiters. RED: <line>. wine: <line>." \
  -m "Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>" -m "Claude-Session: <session link>"
```

---

## Task 14: Fake engine M1 modes — stdin reader thread, slow and stoppable generations

**Files:**
- Modify: `tests/supervision_fake_engine_main.cpp` (SP2), `tests/fake_engine_test.cpp` (SP2), `CMakeLists.txt`

**Interfaces:**
- Consumes: SP2's fake (`Options`, `parse_options`, `value_of`, `serve`, the `--record-file` append, `crash`, `kFatalPrefixes`), SP2 test `Child` helper in `tests/fake_engine_test.cpp`.
- Produces: fake flags `--token-ids=<csv>` (default `7`), `--tokens=<n>` (repeat the id list until n `T` records; default: the list length), `--token-interval-ms=<n>`, `--finish=<stop|length|nonfinite>` (default `stop`), `--prefill-delay-ms=<n>`, `--stop-delay-ms=<n>`; `DONE <generated> <prompt> 0 0 <finish>` (full six-field form) whenever any of these is given; `STOP` during a generation ends it with `DONE <generated> <prompt> 0 0 cancel` after `--stop-delay-ms`; a generation that reaches `max_new` before `--tokens` ends with `length`. Every stdin line is recorded by the reader thread **as it arrives**, so a test sees `STOP` in `--record-file` while the generation runs. Case `fake_engine_stream_modes_selftest` (spec §9.2 name; M2 extends it).

`--hang-in-gen`, `--abort-in-gen`, `--fatal-line`, `--stall-stdin`, `--spew` keep their SP2 meaning: the reader thread is started only after READY and only when neither `--stall-stdin` nor `--spew` is set; `--hang-in-gen` stops consuming the queue (STOP is never acted on).

- [ ] **Step 1: Write the failing test** — append to `tests/fake_engine_test.cpp` and register it in the `secure_frontend_add_cases(secure_frontend_fake_engine_tests` list:

```cpp
void fake_engine_stream_modes_selftest() {
  {
    Child child({"--token-ids=5,6", "--tokens=5", "--token-interval-ms=10"});
    expect(child.line() == "READY 4096 stop", "ready");
    child.send("GEN 100 1,2,3");
    std::vector<std::string> lines;
    for (int i = 0; i < 6; ++i) lines.push_back(child.line());
    expect(lines == std::vector<std::string>{"T 5", "T 6", "T 5", "T 6", "T 5", "DONE 5 3 0 0 stop"}, "ids cycle until --tokens, full DONE");
  }
  {
    Child child({"--tokens=50", "--token-interval-ms=5"});
    (void)child.line();
    child.send("GEN 3 1");
    expect(child.line() == "T 7" && child.line() == "T 7" && child.line() == "T 7" && child.line() == "DONE 3 1 0 0 length",
           "max_new reached first ends with length");
  }
  {
    Child child({"--tokens=1", "--finish=nonfinite"});
    (void)child.line();
    child.send("GEN 10 1,2");
    expect(child.line() == "T 7" && child.line() == "DONE 1 2 0 0 nonfinite", "--finish sets the finish field");
  }
  {
    Child child({"--tokens=10000", "--token-interval-ms=5", "--stop-delay-ms=100"});
    (void)child.line();
    child.send("GEN 100000 1");
    expect(child.line() == "T 7", "generation running");
    const auto sent = std::chrono::steady_clock::now();
    child.send("STOP");
    std::string line;
    do { line = child.line(); } while (line == "T 7");
    expect(line.rfind("DONE ", 0) == 0 && line.size() > 7 && line.substr(line.size() - 6) == "cancel", "STOP ends with DONE ... cancel: " + line);
    expect(std::chrono::steady_clock::now() - sent >= 100ms, "--stop-delay-ms delays the cancel");
  }
  {
    Child child({"--prefill-delay-ms=300", "--tokens=1"});
    (void)child.line();
    child.send("GEN 5 1");
    expect(child.kind(200ms) == secure_frontend::EngineReadKind::timeout, "--prefill-delay-ms holds the first T back");
    expect(child.line() == "T 7", "then the token");
  }
  {
    Child child({});
    (void)child.line();
    child.send("GEN 5 1");
    expect(child.line() == "T 7" && child.line() == "DONE stop", "without new modes the SP2 short form is unchanged");
  }
}
```

- [ ] **Step 2: Run to see RED** — `cmake --build --preset linux-gcc && ctest --preset linux-gcc -R fake_engine_stream_modes_selftest --output-on-failure` → FAIL "ids cycle until --tokens, full DONE" (the fake answers `T 7` / `DONE stop`).

- [ ] **Step 3: Implement** — in `tests/supervision_fake_engine_main.cpp`:
- `Options` gains: `std::vector<unsigned long> token_ids{7}; std::optional<unsigned long> tokens; int token_interval_ms{0}; std::string finish{"stop"}; int prefill_delay_ms{0}; int stop_delay_ms{0}; bool stream_modes{false};`
- `parse_options` gains (each also sets `options.stream_modes = true`):

```cpp
    else if (value_of(argument, "--token-ids", value)) {
      options.token_ids.clear();
      std::stringstream ids(value);
      for (std::string id; std::getline(ids, id, ',');) options.token_ids.push_back(std::stoul(id));
      options.stream_modes = true;
    }
    else if (value_of(argument, "--tokens", value)) { options.tokens = std::stoul(value); options.stream_modes = true; }
    else if (value_of(argument, "--token-interval-ms", value)) { options.token_interval_ms = std::stoi(value); options.stream_modes = true; }
    else if (value_of(argument, "--finish", value)) { options.finish = value; options.stream_modes = true; }
    else if (value_of(argument, "--prefill-delay-ms", value)) { options.prefill_delay_ms = std::stoi(value); options.stream_modes = true; }
    else if (value_of(argument, "--stop-delay-ms", value)) { options.stop_delay_ms = std::stoi(value); options.stream_modes = true; }
```

- Add, before `serve`, the reader and the generation:

```cpp
// Stdin is read on its own thread (spec §9.2) so STOP is seen while T records
// are written. Each line is recorded the moment it arrives.
class StdinReader final {
 public:
  explicit StdinReader(std::string record_file) : record_file_(std::move(record_file)) {
    thread_ = std::thread([this] {
      std::string line;
      while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!record_file_.empty()) {
          std::ofstream out(record_file_, std::ios::app);
          out << line << '\n';
        }
        std::lock_guard lock(mutex_);
        lines_.push_back(line);
        ready_.notify_all();
      }
      std::lock_guard lock(mutex_);
      eof_ = true;
      ready_.notify_all();
    });
    thread_.detach();   // the process exits through return/_Exit; std::getline may block forever
  }
  // Next line, or nullopt at EOF.
  std::optional<std::string> next() {
    std::unique_lock lock(mutex_);
    ready_.wait(lock, [this] { return !lines_.empty() || eof_; });
    if (lines_.empty()) return std::nullopt;
    std::string line = std::move(lines_.front());
    lines_.pop_front();
    return line;
  }
  // True when a STOP line is waiting (and consumes it).
  bool take_stop() {
    std::lock_guard lock(mutex_);
    const auto it = std::find(lines_.begin(), lines_.end(), "STOP");
    if (it == lines_.end()) return false;
    lines_.erase(it);
    return true;
  }

 private:
  std::string record_file_;
  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<std::string> lines_;
  bool eof_{false};
};

void run_generation(const Options& options, const std::string& line, StdinReader& reader) {
  // GEN <max_new> [k=v ...] <ids>
  std::istringstream fields(line.substr(4));
  unsigned long max_new = 0;
  fields >> max_new;
  std::string token;
  std::size_t prompt = 0;
  while (fields >> token) {
    if (token.find('=') != std::string::npos) continue;
    prompt = static_cast<std::size_t>(std::count(token.begin(), token.end(), ',')) + 1U;
  }
  const unsigned long total = options.tokens.value_or(options.token_ids.size());
  if (options.prefill_delay_ms > 0) sleep_ms(options.prefill_delay_ms);
  unsigned long generated = 0;
  std::string finish = options.finish;
  for (; generated < total; ++generated) {
    if (reader.take_stop()) { finish = "cancel"; break; }
    if (generated >= max_new) { finish = "length"; break; }
    std::cout << "T " << options.token_ids[generated % options.token_ids.size()] << '\n' << std::flush;
    if (options.token_interval_ms > 0) sleep_ms(options.token_interval_ms);
  }
  if (finish == "cancel" && options.stop_delay_ms > 0) sleep_ms(options.stop_delay_ms);
  std::cout << "DONE " << generated << ' ' << prompt << " 0 0 " << finish << '\n' << std::flush;
}
```

(includes: `<condition_variable>`, `<deque>`, `<mutex>`, `<sstream>`, `<algorithm>`, `<optional>`.) With `max_new` 3 and `--tokens=50`, the loop writes three `T` and stops with `length` on the fourth iteration.
- In `serve`, replace the main `while (std::getline(std::cin, line)) { ... }` loop (anchor `if (line.rfind("GEN ", 0) == 0) {`; the `--spew` branch above it stays as it is) with:

```cpp
  StdinReader reader(options.record_file);
  while (const auto next = reader.next()) {
    const std::string& line = *next;
    if (line.rfind("GEN ", 0) == 0) {
      if (options.hang_in_gen) sleep_forever();  // stops acting on stdin: STOP is never seen
      if (options.abort_in_gen) {
        std::cout << "T 7\n" << std::flush;
        crash("ABRT");
      }
      if (options.fatal_line) {
        const auto index = static_cast<std::size_t>(*options.fatal_line);
        std::cout << "T 7\n" << kFatalPrefixes[index] << (index < 2U ? "12" : "") << "\nDONE stop\n" << std::flush;
        continue;
      }
      if (options.stream_modes) { run_generation(options, line, reader); continue; }
      std::cout << "T 7\nDONE stop\n" << std::flush;
    } else if (line == "QUIT") {
      return 0;
    } else if (line == "DUMP") {
      dump();
    }
  }
  return 0;
```

(the per-line `--record-file` append moves into `StdinReader`; delete it from the loop body). Link `strata-supervision-fake-engine` with `Threads::Threads` if the toolchain needs it (`find_package(Threads REQUIRED)` is already present when SP2 linked threads; otherwise add both lines next to `add_executable(strata-supervision-fake-engine`).

- [ ] **Step 4: Run to see GREEN** — the whole SP2 fake suite must stay green:

```bash
cmake --build --preset linux-gcc && ctest --preset linux-gcc -R 'fake_engine_tests|engine_supervisor_tests|server_tests|engine_child' --output-on-failure
ctest --preset linux-gcc | tail -1
cmake --build --preset windows-llvm-mingw-cross && ctest --preset windows-llvm-mingw-cross -R fake_engine | tail -1
```

- [ ] **Step 5: Handoff row and commit** — row `| 14 | test infrastructure (spec §9.2, M1 part) | fake engine: stdin reader thread; --token-ids, --tokens, --token-interval-ms, --finish, --prefill-delay-ms, --stop-delay-ms; full DONE | fake_engine_tests.fake_engine_stream_modes_selftest | … | … |`.

```bash
git -C "$(git rev-parse --show-toplevel)" add secure_frontend docs/AGENT_HANDOFF.md
git -C "$(git rev-parse --show-toplevel)" -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "test: fake engine reads stdin on a thread; slow, stoppable generations with full DONE (SP3 §9.2, M1 modes)" \
  -m "Tests: fake_engine_tests.fake_engine_stream_modes_selftest; SP2 fake, supervisor and server suites unchanged and green. RED: <line>. wine: <line>." \
  -m "Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>" -m "Claude-Session: <session link>"
```

---
## Task 15: `GEN` sampling keys, `run_generation` with cancel/timeout/shutdown, `context_tokens`, the §3.5 response shape

**Files:**
- Modify: `include/secure_frontend/engine_protocol_client.hpp`, `src/engine_protocol_client.cpp`, `include/secure_frontend/chat_execution.hpp`, `src/chat_execution.cpp`, `include/secure_frontend/chat_schema.hpp`, `src/chat_schema.cpp`, `tests/engine_protocol_client_test.cpp`, `tests/chat_execution_test.cpp`, `tests/chat_schema_test.cpp`, `CMakeLists.txt`

**Interfaces:**
- Consumes: `SamplingParams` (Task 7); `resolve_max_tokens` (Task 5); `api_constants::kStreamPollSlice`, `kShutdownDrainBudget`; SP0/SP2 `EngineProtocolClient` (`read_generation` with a total budget that leaves the generation active on timeout, `stop`, `abandon`, `closed`, `idle`), `decode_token_ids`, `ChatExecutionService` (`received_ready_`, `engine_context_tokens_`, `adopt_ready`, `limits_`).
- Produces:

```cpp
// engine_protocol_client.hpp
[[nodiscard]] std::optional<EngineProtocolError> begin_generation(std::uint32_t max_new_tokens,
    const std::vector<std::uint32_t>& prompt_token_ids, const SamplingParams& sampling = {});
[[nodiscard]] std::string render_sampling_keys(const SamplingParams& sampling);   // " temperature=0.5 top_p=0.9 seed=42" or ""
// chat_execution.hpp
enum class FinishReason { stop, length };
struct GenerationSettings final { std::size_t max_tokens; SamplingParams sampling{}; };
struct ExecutionControl final { std::function<bool()> cancelled; const std::atomic<bool>* shutting_down{nullptr};
  std::function<bool()> shutdown_requested{}; };   // ruling R-H: ChatRoute passes [] { return shutdown_signalled(); }
struct GenerationResult final { std::string text; std::size_t prompt_tokens; std::size_t completion_tokens; FinishReason finish; };
enum class GenerationFailure { cancelled, timed_out, engine_error, engine_unavailable, decode_failed, shutting_down };
using GenerationOutcome = std::variant<GenerationResult, GenerationFailure>;
// ChatExecutionService:
[[nodiscard]] std::size_t context_tokens() const noexcept;   // READY context, 0 before READY
[[nodiscard]] GenerationOutcome run_generation(const std::vector<std::uint32_t>& prompt_ids, const GenerationSettings& settings,
                                               const ExecutionControl& control, const TokenizerPack& tokenizer);
// chat_schema.hpp
struct CompletionView final { std::string_view id; std::int64_t created; std::string_view model; std::string_view text;
  std::string_view finish_reason; std::size_t prompt_tokens; std::size_t completion_tokens; };
[[nodiscard]] std::optional<std::string> render_chat_completion(const CompletionView& view, const ChatSchemaLimits& limits = {});
```

`run_generation` (P6; shutdown = `control.shutting_down->load() || control.shutdown_requested()`, ruling R-H): requires READY (adopted or awaited exactly like `execute`); writes `GEN` with the sampling keys; reads in `kStreamPollSlice` slices (each `read_generation` call continues the active generation); between slices it checks `control.cancelled()` (→ `STOP`, drain within `stop_drain_timeout`, `cancelled`), the shutdown flag (→ `STOP`, drain within `min(stop_drain_timeout, kShutdownDrainBudget)`, `shutting_down`) and `generation_timeout` (→ `STOP`, drain, `timed_out`; M1, controller note 7). A failed `STOP` write or a drain that does not reach `DONE` → `abandon()` and `engine_unavailable` (or `shutting_down` during shutdown). `DONE … nonfinite` → `engine_error` (protocol stays idle); `ERR` / fatal prefix → `engine_error` (protocol closed); any other failure → `engine_unavailable`. More tokens than `max_tokens` → `abandon()`, `engine_error`. Finish: `DONE` field 6 `length` → `length`, otherwise `stop`. Text through `decode_token_ids` (failure → `decode_failed`; M2 replaces it with the stream decoder).

- [ ] **Step 1: Write the failing tests**

`tests/engine_protocol_client_test.cpp` — new case `gen_carries_bounded_sampling_keys` (registered):

```cpp
class RecordingTransport final : public secure_frontend::EngineLineTransport {
 public:
  std::deque<std::string> reads;
  std::vector<std::string> writes;
  bool write_line(const std::string_view line, std::string&) override { writes.emplace_back(line); return true; }
  secure_frontend::EngineReadResult read_line(std::chrono::milliseconds) override {
    if (reads.empty()) return {secure_frontend::EngineReadKind::timeout, {}, {}};
    std::string next = std::move(reads.front());
    reads.pop_front();
    return {secure_frontend::EngineReadKind::line, std::move(next), {}};
  }
};

void gen_carries_bounded_sampling_keys() {
  RecordingTransport transport;
  transport.reads = {"READY 4096 stop"};
  secure_frontend::EngineProtocolClient client(transport);
  secure_frontend::EngineReady ready{};
  expect(!client.await_ready(std::chrono::seconds(1), ready).has_value(), "ready");
  secure_frontend::SamplingParams sampling;
  sampling.temperature = 0.5;
  sampling.top_p = 0.9;
  sampling.seed = 42U;
  expect(!client.begin_generation(16U, {1U, 2U, 3U}, sampling).has_value(), "GEN written");
  expect(transport.writes.back() == "GEN 16 temperature=0.5 top_p=0.9 seed=42 1,2,3", "keys between max_new and the ids: " + transport.writes.back());
  expect(secure_frontend::render_sampling_keys({}).empty(), "no keys when the request sets none (engine stays greedy)");
  secure_frontend::SamplingParams ints;
  ints.temperature = 1.0;
  ints.top_p = 1.0;
  expect(secure_frontend::render_sampling_keys(ints) == " temperature=1 top_p=1", "shortest round-trip form, never request bytes");
  secure_frontend::SamplingParams big;
  big.seed = 9223372036854775807ULL;
  expect(secure_frontend::render_sampling_keys(big) == " seed=9223372036854775807", "full seed range");
}
```

`tests/chat_execution_test.cpp` — new cases (registered):

```cpp
// Answers STOP with "DONE <n> <p> 0 0 cancel" the next time it is read.
class StopAwareTransport final : public secure_frontend::EngineLineTransport {
 public:
  std::deque<std::string> reads;
  std::vector<std::string> writes;
  std::string cancel_line{"DONE 1 3 0 0 cancel"};
  bool answer_stop{true};
  bool write_line(const std::string_view line, std::string&) override {
    writes.emplace_back(line);
    if (line == "STOP" && answer_stop) reads.push_back(cancel_line);
    return true;
  }
  secure_frontend::EngineReadResult read_line(const std::chrono::milliseconds timeout) override {
    if (reads.empty()) {
      std::this_thread::sleep_for(std::min(timeout, std::chrono::milliseconds(2)));
      return {secure_frontend::EngineReadKind::timeout, {}, {}};
    }
    std::string next = std::move(reads.front());
    reads.pop_front();
    return {secure_frontend::EngineReadKind::line, std::move(next), {}};
  }
};

struct ReadyService final {
  StopAwareTransport transport;
  secure_frontend::EngineProtocolClient protocol{transport};
  secure_frontend::ChatExecutionService service;
  explicit ReadyService(secure_frontend::ChatExecutionLimits limits = {}) : service(protocol, limits) {
    transport.reads = {"READY 512 stop"};
    secure_frontend::EngineReady ready{};
    expect(!protocol.await_ready(std::chrono::seconds(1), ready).has_value(), "READY consumed");
    service.adopt_ready(ready);
  }
};

void run_generation_reports_finish_usage_and_context() {
  const auto tokenizer = tokenizer_for("OK");
  ReadyService f;
  expect(f.service.context_tokens() == 512U, "context_tokens is the READY context");
  const auto o = token_id_for(tokenizer, "O");
  const auto k = token_id_for(tokenizer, "K");
  f.transport.reads = {"T " + std::to_string(o), "T " + std::to_string(k), "DONE 2 3 1.0 1.0 length"};
  const auto outcome = f.service.run_generation({1U, 2U, 3U}, {.max_tokens = 2U}, {}, tokenizer);
  const auto* result = std::get_if<secure_frontend::GenerationResult>(&outcome);
  expect(result != nullptr && result->text == "OK" && result->prompt_tokens == 3U && result->completion_tokens == 2U &&
         result->finish == secure_frontend::FinishReason::length, "text, usage and finish from DONE");
  expect(f.transport.writes.front().rfind("GEN 2 ", 0) == 0, "GEN carries the resolved budget");
  f.transport.reads = {"T " + std::to_string(o), "DONE stop"};
  const auto short_form = f.service.run_generation({1U}, {.max_tokens = 5U}, {}, tokenizer);
  expect(std::holds_alternative<secure_frontend::GenerationResult>(short_form) &&
         std::get<secure_frontend::GenerationResult>(short_form).finish == secure_frontend::FinishReason::stop, "short DONE is stop");
}

void run_generation_cancel_sends_stop_and_drains() {
  const auto tokenizer = tokenizer_for("O");
  ReadyService f;
  f.transport.reads = {"T " + std::to_string(token_id_for(tokenizer, "O"))};
  int checks = 0;
  const auto outcome = f.service.run_generation({1U, 2U, 3U}, {.max_tokens = 100U}, {.cancelled = [&checks] { return ++checks > 2; }}, tokenizer);
  expect(std::holds_alternative<secure_frontend::GenerationFailure>(outcome) &&
         std::get<secure_frontend::GenerationFailure>(outcome) == secure_frontend::GenerationFailure::cancelled, "a gone client cancels");
  expect(std::count(f.transport.writes.begin(), f.transport.writes.end(), "STOP") == 1, "exactly one STOP");
  expect(f.protocol.idle(), "drained to DONE: the engine is idle for the next request");
}

void run_generation_timeout_stops_and_reports_timed_out() {
  const auto tokenizer = tokenizer_for("O");
  ReadyService f({.generation_timeout = std::chrono::milliseconds(100), .stop_drain_timeout = std::chrono::milliseconds(500)});
  const auto started = std::chrono::steady_clock::now();
  const auto outcome = f.service.run_generation({1U}, {.max_tokens = 100U}, {}, tokenizer);
  expect(std::holds_alternative<secure_frontend::GenerationFailure>(outcome) &&
         std::get<secure_frontend::GenerationFailure>(outcome) == secure_frontend::GenerationFailure::timed_out, "timeout");
  expect(std::chrono::steady_clock::now() - started < std::chrono::seconds(1), "bounded by generation_timeout + drain");
  expect(f.protocol.idle(), "STOP drained");
}

void run_generation_drain_failure_abandons() {
  const auto tokenizer = tokenizer_for("O");
  ReadyService f({.generation_timeout = std::chrono::milliseconds(50), .stop_drain_timeout = std::chrono::milliseconds(100)});
  f.transport.answer_stop = false;
  const auto outcome = f.service.run_generation({1U}, {.max_tokens = 100U}, {}, tokenizer);
  expect(std::holds_alternative<secure_frontend::GenerationFailure>(outcome) &&
         std::get<secure_frontend::GenerationFailure>(outcome) == secure_frontend::GenerationFailure::engine_unavailable,
         "an engine that ignores STOP is unavailable");
  expect(f.protocol.closed(), "and abandoned (the route reports drain_failed through the port)");
}

void run_generation_shutdown_uses_short_drain() {
  const auto tokenizer = tokenizer_for("O");
  ReadyService f;
  std::atomic<bool> shutting{true};
  const auto outcome = f.service.run_generation({1U}, {.max_tokens = 100U}, {.shutting_down = &shutting}, tokenizer);
  expect(std::holds_alternative<secure_frontend::GenerationFailure>(outcome) &&
         std::get<secure_frontend::GenerationFailure>(outcome) == secure_frontend::GenerationFailure::shutting_down, "shutdown");
  expect(std::count(f.transport.writes.begin(), f.transport.writes.end(), "STOP") == 1, "STOP sent during shutdown");
}

void run_generation_maps_engine_errors() {
  const auto tokenizer = tokenizer_for("O");
  {
    ReadyService f;
    f.transport.reads = {"DONE 0 1 1.0 1.0 nonfinite"};
    const auto outcome = f.service.run_generation({1U}, {.max_tokens = 5U}, {}, tokenizer);
    expect(std::get<secure_frontend::GenerationFailure>(outcome) == secure_frontend::GenerationFailure::engine_error && f.protocol.idle(),
           "nonfinite: engine_error, engine stays idle");
  }
  {
    ReadyService f;
    f.transport.reads = {"ERR bad request"};
    const auto outcome = f.service.run_generation({1U}, {.max_tokens = 5U}, {}, tokenizer);
    expect(std::get<secure_frontend::GenerationFailure>(outcome) == secure_frontend::GenerationFailure::engine_error && f.protocol.closed(),
           "ERR: engine_error, protocol closed (SP2 restarts and counts)");
  }
  {
    ReadyService f;
    f.transport.reads = {"T 0", "T 0", "DONE 2 1 0 0 stop"};
    const auto outcome = f.service.run_generation({1U}, {.max_tokens = 1U}, {}, tokenizer);
    expect(std::get<secure_frontend::GenerationFailure>(outcome) == secure_frontend::GenerationFailure::engine_error, "more tokens than asked");
  }
}
```

(`#include <atomic>`, `<algorithm>`, `<thread>` in the test if missing.)

`tests/chat_schema_test.cpp` — new case `renders_openai_completion_shape` (registered):

```cpp
void renders_openai_completion_shape() {
  const auto body = secure_frontend::render_chat_completion({.id = "chatcmpl-0123456789abcdef0123456789abcdef", .created = 1791963785,
                                                             .model = "fake-model", .text = "Hi \"there\"\n", .finish_reason = "length",
                                                             .prompt_tokens = 7U, .completion_tokens = 3U});
  expect(body == std::optional<std::string>{
                     "{\"id\":\"chatcmpl-0123456789abcdef0123456789abcdef\",\"object\":\"chat.completion\",\"created\":1791963785,"
                     "\"model\":\"fake-model\",\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":\"Hi \\\"there\\\"\\n\"},"
                     "\"logprobs\":null,\"finish_reason\":\"length\"}],\"usage\":{\"prompt_tokens\":7,\"completion_tokens\":3,\"total_tokens\":10}}\n"},
         "spec §3.5 shape: " + body.value_or("<nullopt>"));
  expect(!secure_frontend::render_chat_completion({.id = "x", .created = 0, .model = "m", .text = "\xff", .finish_reason = "stop",
                                                   .prompt_tokens = 0U, .completion_tokens = 0U}).has_value(), "invalid UTF-8 is refused");
}
```

Register: `gen_carries_bounded_sampling_keys` (engine_protocol_client_tests); `run_generation_reports_finish_usage_and_context run_generation_cancel_sends_stop_and_drains run_generation_timeout_stops_and_reports_timed_out run_generation_drain_failure_abandons run_generation_shutdown_uses_short_drain run_generation_maps_engine_errors` (chat_execution_tests); `renders_openai_completion_shape` (chat_schema_tests).

- [ ] **Step 2: Run to see RED** — `cmake --build --preset linux-gcc 2>&1 | grep -m1 error` → FAIL `no matching function for call to 'secure_frontend::EngineProtocolClient::begin_generation(unsigned int, <brace-enclosed initializer list>, secure_frontend::SamplingParams&)'`.

- [ ] **Step 3: Implement**

`engine_protocol_client.hpp`: `#include "secure_frontend/sampling_params.hpp"`; change `begin_generation`'s declaration to the three-parameter form above and declare `render_sampling_keys`. `engine_protocol_client.cpp`:

```cpp
std::string render_sampling_keys(const SamplingParams& sampling) {
  std::string out;
  const auto number = [&out](std::string_view key, const double value) {
    char buffer[64];
    const auto [end, ec] = std::to_chars(buffer, buffer + sizeof(buffer), value);   // shortest round-trip
    if (ec != std::errc{}) return;
    out += ' ';
    out += key;
    out += '=';
    out.append(buffer, end);
  };
  if (sampling.temperature) number("temperature", *sampling.temperature);
  if (sampling.top_p) number("top_p", *sampling.top_p);
  if (sampling.seed) out += " seed=" + std::to_string(*sampling.seed);
  return out;
}
```

and in `begin_generation` (anchor `std::string command = "GEN " + std::to_string(max_new_tokens) + " ";`):

```cpp
  std::string command = "GEN " + std::to_string(max_new_tokens) + render_sampling_keys(sampling) + " ";
```

(`#include <charconv>`; values were bounded by the chat schema, so only finite numbers in `[0, 2]` reach here.)

`chat_execution.hpp`: add the types from Interfaces (`#include "secure_frontend/sampling_params.hpp"`, `<atomic>`, `<functional>`), and the two member functions. `chat_execution.cpp`:

```cpp
std::size_t ChatExecutionService::context_tokens() const noexcept { return received_ready_ ? engine_context_tokens_ : 0U; }

GenerationOutcome ChatExecutionService::run_generation(const std::vector<std::uint32_t>& prompt_ids, const GenerationSettings& settings,
                                                       const ExecutionControl& control, const TokenizerPack& tokenizer) {
  using Clock = std::chrono::steady_clock;
  if (!received_ready_) {
    EngineReady ready{};
    if (protocol_.await_ready(limits_.ready_timeout, ready).has_value()) return GenerationFailure::engine_unavailable;
    adopt_ready(ready);
  }
  if (!protocol_.idle() || settings.max_tokens == 0U || settings.max_tokens > std::numeric_limits<std::uint32_t>::max()) {
    return GenerationFailure::engine_unavailable;
  }
  if (protocol_.begin_generation(static_cast<std::uint32_t>(settings.max_tokens), prompt_ids, settings.sampling).has_value()) {
    return protocol_.closed() ? GenerationFailure::engine_unavailable : GenerationFailure::engine_error;
  }
  const auto deadline = Clock::now() + limits_.generation_timeout;
  EngineGeneration generation{};
  for (;;) {
    const auto error = protocol_.read_generation(api_constants::kStreamPollSlice, generation);
    if (!error.has_value()) break;
    if (error->code != EngineProtocolErrorCode::timeout) {
      return error->code == EngineProtocolErrorCode::engine_error || error->code == EngineProtocolErrorCode::fatal_engine_line
                 ? GenerationFailure::engine_error
                 : GenerationFailure::engine_unavailable;
    }
    // Ruling R-H: the pool's flag or SP2's signal flag, whichever is seen first.
    const bool shutting = (control.shutting_down != nullptr && control.shutting_down->load()) ||
                          (control.shutdown_requested && control.shutdown_requested());
    const bool cancelled = !shutting && control.cancelled && control.cancelled();
    const bool timed_out = Clock::now() >= deadline;
    if (!shutting && !cancelled && !timed_out) continue;
    const GenerationFailure failed = shutting ? GenerationFailure::shutting_down : GenerationFailure::engine_unavailable;
    if (protocol_.stop().has_value()) { protocol_.abandon(); return failed; }
    const auto drain = shutting ? std::min(limits_.stop_drain_timeout, api_constants::kShutdownDrainBudget) : limits_.stop_drain_timeout;
    if (const auto drain_error = protocol_.read_generation(drain, generation); drain_error.has_value()) {
      if (drain_error->code == EngineProtocolErrorCode::timeout) protocol_.abandon();
      return failed;
    }
    return shutting ? GenerationFailure::shutting_down : cancelled ? GenerationFailure::cancelled : GenerationFailure::timed_out;
  }
  if (generation.token_ids.size() > settings.max_tokens) { protocol_.abandon(); return GenerationFailure::engine_error; }
  std::vector<std::size_t> ids(generation.token_ids.begin(), generation.token_ids.end());
  const auto decoded = decode_token_ids(tokenizer, ids);
  const auto* text = std::get_if<std::string>(&decoded);
  if (text == nullptr) return GenerationFailure::decode_failed;
  std::size_t field = 0;
  std::string_view finish_field;
  for (std::size_t start = 0; start <= generation.done_line.size();) {
    const std::size_t end = std::min(generation.done_line.find(' ', start), generation.done_line.size());
    if (field++ == 5U) { finish_field = std::string_view{generation.done_line}.substr(start, end - start); break; }
    start = end + 1U;
  }
  return GenerationResult{*text, prompt_ids.size(), generation.token_ids.size(),
                          finish_field == "length" ? FinishReason::length : FinishReason::stop};
}
```

(If SP2's error code enum has no `fatal_engine_line`, drop that comparison: the fatal prefixes then arrive as `engine_error`. `#include "secure_frontend/api_constants.hpp"`, `<limits>`, `<algorithm>`.)

`chat_schema.hpp/.cpp`: add `CompletionView` and

```cpp
std::optional<std::string> render_chat_completion(const CompletionView& view, const ChatSchemaLimits& limits) {
  if (view.text.size() > limits.max_generated_bytes || !valid_utf8(view.text) || !valid_utf8(view.model)) return std::nullopt;
  std::string out{"{\"id\":"};
  append_json_string(&out, view.id);
  out += ",\"object\":\"chat.completion\",\"created\":" + std::to_string(view.created) + ",\"model\":";
  append_json_string(&out, view.model);
  out += ",\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":";
  append_json_string(&out, view.text);
  out += "},\"logprobs\":null,\"finish_reason\":";
  append_json_string(&out, view.finish_reason);
  out += "}],\"usage\":{\"prompt_tokens\":" + std::to_string(view.prompt_tokens) + ",\"completion_tokens\":" +
         std::to_string(view.completion_tokens) + ",\"total_tokens\":" + std::to_string(view.prompt_tokens + view.completion_tokens) + "}}\n";
  return out;
}
```

Link `secure_frontend_chat_execution` and `secure_frontend_engine_protocol_client` with `secure_frontend_api_core` (constants header only) if not already reachable.

- [ ] **Step 4: Run to see GREEN**

```bash
cmake --build --preset linux-gcc && ctest --preset linux-gcc -R 'engine_protocol_client_tests|chat_execution_tests|chat_schema_tests' --output-on-failure
ctest --preset linux-gcc | tail -1
cmake --build --preset windows-llvm-mingw-cross && ctest --preset windows-llvm-mingw-cross -R 'chat_|engine_protocol' | tail -1
```

- [ ] **Step 5: Handoff row and commit** — row `| 15 | B6 (GEN keys), §2.6 P6 cancel, §3.5, §5.4 | GEN temperature/top_p/seed; run_generation (20 ms slices, cancel/timeout/shutdown STOP+drain); context_tokens; OpenAI completion shape | engine_protocol_client_tests.gen_carries_bounded_sampling_keys; chat_execution_tests.run_generation_reports_finish_usage_and_context, …run_generation_cancel_sends_stop_and_drains, …run_generation_timeout_stops_and_reports_timed_out, …run_generation_drain_failure_abandons, …run_generation_shutdown_uses_short_drain, …run_generation_maps_engine_errors; chat_schema_tests.renders_openai_completion_shape | … | … |`.

```bash
git -C "$(git rev-parse --show-toplevel)" add secure_frontend docs/AGENT_HANDOFF.md
git -C "$(git rev-parse --show-toplevel)" -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "engine: bounded sampling keys on GEN; cancellable generation with STOP and drain; OpenAI completion shape (SP3 B6, §2.6, §3.5)" \
  -m "Tests: engine_protocol_client_tests.gen_carries_bounded_sampling_keys; chat_execution_tests.run_generation_reports_finish_usage_and_context, run_generation_cancel_sends_stop_and_drains, run_generation_timeout_stops_and_reports_timed_out, run_generation_drain_failure_abandons, run_generation_shutdown_uses_short_drain, run_generation_maps_engine_errors; chat_schema_tests.renders_openai_completion_shape. RED: <line>. wine: <line>." \
  -m "Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>" -m "Claude-Session: <session link>"
```

---

## Task 16: `ChatRoute` — P4 validation, the gate, P6 through the lease; the server's chats are queued

**Files:**
- Create: `include/secure_frontend/chat_route.hpp`, `src/chat_route.cpp`
- Modify: `src/secure_server_main.cpp`, `tests/api_server_test.cpp`, `CMakeLists.txt`

**Interfaces:**
- Consumes: `parse_chat_request` (Task 7); `make_qwen_chat_generation_plan`/`make_glm53_chat_generation_plan` (SP0/SP1, default parameter `default_max_tokens` passed as `1` because SP3 resolves the budget itself); `resolve_max_tokens` (Task 5); `SupervisorPort`, `GenerationGate`, `GateRefusal` (Task 13); `Lease::chat()`, `Lease::protocol()`, `kill_reason_for` (SP2); `run_generation`, `context_tokens`, `GenerationFailure` (Task 15); `render_chat_completion` (Task 15); `make_error_response`, `engine_retry_after` (Tasks 1, 9); `kNoResponseStatus` (Task 6a); SP1 `fill_random` (`token_generator.hpp`).
- Produces:

```cpp
struct ChatRouteConfig final {
  ChatSchemaLimits schema;
  std::string model_id;
  NativeChatProfile profile;
  std::size_t max_prompt_tokens;
  std::size_t max_completion_tokens;
  std::size_t default_max_tokens;          // 0 = auto
  std::chrono::milliseconds queue_timeout;
};
[[nodiscard]] std::optional<std::string> make_completion_id();   // "chatcmpl-" + 32 lowercase hex from the CSPRNG
class ChatRoute final {
 public:
  ChatRoute(ChatRouteConfig config, SupervisorPort& port, GenerationGate& gate, const TokenizerPack& tokenizer);
  [[nodiscard]] HttpResponse handle(std::string_view body, const RequestContext& context);
};
```

Log-only codes put in `HttpResponse::log_code` (spec §8; never sent): `client_gone` (with `kNoResponseStatus`), `generation_timeout` (on the `503 engine_error` of a timeout).

- [ ] **Step 1: Write the failing tests** — append to `tests/api_server_test.cpp` and register in `SECURE_FRONTEND_API_SERVER_CASES`:

```cpp
std::vector<std::string> gen_lines(const std::string& record) {
  std::vector<std::string> lines;
  std::istringstream in(record);
  for (std::string line; std::getline(in, line);) if (line.rfind("GEN ", 0) == 0) lines.push_back(line);
  return lines;
}
std::size_t gen_max(const std::string& gen) { return std::stoul(gen.substr(4, gen.find(' ', 4) - 4)); }
std::size_t gen_prompt(const std::string& gen) {
  const std::string ids = gen.substr(gen.rfind(' ') + 1U);
  return static_cast<std::size_t>(std::count(ids.begin(), ids.end(), ',')) + 1U;
}
struct Recorded final {
  std::filesystem::path record;
  std::unique_ptr<test_support::ServerProcess> server;
};
Recorded start_recorded(const std::string& tag, std::map<std::string, std::string> literals = {}, std::vector<std::string> engine = {}) {
  Recorded r;
  r.record = test_support::make_temp_dir(tag) / "record.txt";
  engine.push_back("--record-file=" + test_support::utf8_path(r.record));
  r.server = start_server(std::move(literals), std::move(engine));
  return r;
}

void max_tokens_above_limit_is_clamped() {
  auto r = start_recorded("mt1", {{"limits.chat.max_completion_tokens", "16"}, {"limits.chat.default_max_tokens", "0"}});
  if (!r.server) return;
  const auto response = raw(*r.server, chat_request(*r.server, chat_body(R"("max_tokens":16384)")));
  expect(response && response->status == 200, "an over-limit max_tokens is clamped, not rejected (B2)");
  const auto gens = gen_lines(record_of(r.record));
  expect(gens.size() == 1U && gen_max(gens.front()) == 16U, "GEN max_new is the configured cap");
}

void max_tokens_clamped_to_context_room() {
  auto r = start_recorded("mt2", {}, {"--ready-context=200"});
  if (!r.server) return;
  const auto response = raw(*r.server, chat_request(*r.server, chat_body(R"("max_tokens":1000)")));
  expect(response && response->status == 200, "clamped to the context room");
  const auto gens = gen_lines(record_of(r.record));
  expect(gens.size() == 1U && gen_max(gens.front()) == 200U - gen_prompt(gens.front()) - 8U, "max_new = ctx - n - 8");
}

void prompt_alone_over_context_is_400() {
  auto r = start_recorded("mt3", {}, {"--ready-context=64"});
  if (!r.server) return;
  const auto response = raw(*r.server, chat_request(*r.server, chat_body("", std::string(100, 'x'))));
  expect(response && response->status == 400 &&
         response->body == secure_frontend::render_error_body(ApiError::context_length_exceeded), "the prompt alone fills the context");
  expect(gen_lines(record_of(r.record)).empty(), "no GEN was written");
}

void absent_max_tokens_uses_auto() {
  auto r = start_recorded("mt4");
  if (!r.server) return;
  const auto response = raw(*r.server, chat_request(*r.server, chat_body("")));
  expect(response && response->status == 200, "a request without max_tokens");
  const auto gens = gen_lines(record_of(r.record));
  expect(gens.size() == 1U && gen_max(gens.front()) == std::min<std::size_t>(4096U, 4096U - gen_prompt(gens.front()) - 8U),
         "auto = min(max_completion_tokens, ctx - n - 8) (B1)");
}

void sampling_passthrough_reaches_gen() {
  auto r = start_recorded("smp");
  if (!r.server) return;
  const auto response = raw(*r.server, chat_request(*r.server, chat_body(R"("max_tokens":2,"temperature":0.5,"top_p":0.9,"seed":42,"presence_penalty":0)")));
  expect(response && response->status == 200, "sampling accepted");
  const auto gens = gen_lines(record_of(r.record));
  expect(gens.size() == 1U && gens.front().find(" temperature=0.5 top_p=0.9 seed=42 ") != std::string::npos, "GEN carries the keys: " +
         (gens.empty() ? std::string{} : gens.front()));
}

void dos01_context_rule_is_400_without_gen() {
  auto r = start_recorded("ctx", {}, {"--ready-context=64"});
  if (!r.server) return;
  const auto response = raw(*r.server, chat_request(*r.server, chat_body(R"("max_tokens":5)", std::string(100, 'y'))));
  expect(response && response->status == 400 && response->body.find("context_length_exceeded") != std::string::npos, "400");
  expect(gen_lines(record_of(r.record)).empty() && r.server->wait_for_health(test_support::kHealthReady, 2s),
         "no GEN; the engine never saw a request it would refuse and stays ready");
}

void chat_response_has_openai_shape() {
  auto r = start_recorded("shape");
  if (!r.server) return;
  const auto first = raw(*r.server, chat_request(*r.server, chat_body()));
  const auto second = raw(*r.server, chat_request(*r.server, chat_body()));
  expect(first && first->status == 200 && second && second->status == 200, "two chats");
  if (!first || !second) return;
  const auto id_of = [](const std::string& body) { const auto p = body.find("\"id\":\"chatcmpl-"); return p == std::string::npos ? std::string{} : body.substr(p + 15, 32); };
  expect(id_of(first->body).size() == 32U && id_of(first->body).find_first_not_of("0123456789abcdef") == std::string::npos, "32 lowercase hex");
  expect(id_of(first->body) != id_of(second->body), "a fresh id per response");
  expect(first->body.find("\"created\":0") == std::string::npos && first->body.find("\"finish_reason\":") != std::string::npos &&
         first->body.find("\"completion_tokens\":1") != std::string::npos, "created, finish_reason and usage are real");
}

void dos02_excess_connections_get_503_retry_after() {
  auto r = start_recorded("exc", {{"limits.concurrency.max_concurrent_requests", "2"}}, {"--tokens=60", "--token-interval-ms=50"});
  if (!r.server) return;
  std::thread chat([&] { (void)raw(*r.server, chat_request(*r.server, chat_body("")), 30s); });
  std::this_thread::sleep_for(300ms);   // worker 1 in P6
  auto body_holder = raw_http::Connection::open("127.0.0.1", r.server->port());
  expect(body_holder && body_holder->send("POST /v1/chat/completions HTTP/1.1\r\nHost: " + authority(*r.server) + "\r\n" +
                                          raw_http::bearer(token()) + "\r\nContent-Type: application/json\r\nContent-Length: 100\r\n\r\n{"),
         "worker 2 held in P3 by an authenticated slow body");
  std::this_thread::sleep_for(200ms);
  const auto refused = raw(*r.server, raw_http::request("GET", "/health", authority(*r.server), {raw_http::bearer(token())}), 5s);
  expect(refused && refused->status == 503 && refused->header("Retry-After") == std::optional<std::string>{"1"} &&
         refused->body == secure_frontend::render_error_body(ApiError::server_busy), "no worker free: 503 server_busy, Retry-After: 1, JSON");
  body_holder.reset();
  chat.join();
}

void dos02_queue_full_is_503() {
  auto r = start_recorded("qf", {}, {"--tokens=20", "--token-interval-ms=50"});
  if (!r.server) return;
  std::vector<std::thread> holders;
  for (int i = 0; i < 5; ++i) {
    holders.emplace_back([&] { (void)raw(*r.server, chat_request(*r.server, chat_body("")), 30s); });
    std::this_thread::sleep_for(50ms);
  }
  std::this_thread::sleep_for(200ms);   // 1 running + 4 queued
  const auto started = std::chrono::steady_clock::now();
  const auto refused = raw(*r.server, chat_request(*r.server, chat_body("")));
  expect(refused && refused->status == 503 && refused->body == secure_frontend::render_error_body(ApiError::server_busy) &&
         refused->header("Retry-After") == std::optional<std::string>{"1"}, "the sixth chat is refused at once");
  expect(std::chrono::steady_clock::now() - started < 500ms, "without waiting");
  for (auto& t : holders) t.join();
}

void dos02_queue_timeout_is_503() {
  auto r = start_recorded("qt", {{"limits.concurrency.queue_timeout_ms", "1000"}}, {"--tokens=30", "--token-interval-ms=100"});
  if (!r.server) return;
  std::thread running([&] { (void)raw(*r.server, chat_request(*r.server, chat_body("")), 30s); });
  std::this_thread::sleep_for(200ms);
  const auto started = std::chrono::steady_clock::now();
  const auto waited = raw(*r.server, chat_request(*r.server, chat_body("")), 10s);
  const auto elapsed = std::chrono::steady_clock::now() - started;
  expect(waited && waited->status == 503 && waited->body.find("server_busy") != std::string::npos, "queue timeout is 503 server_busy");
  expect(elapsed >= 900ms && elapsed < 2500ms, "after about 1 s");
  running.join();
}

void dos02_queue_is_fifo() {
  auto r = start_recorded("fifo", {}, {"--tokens=8", "--token-interval-ms=50"});
  if (!r.server) return;
  std::mutex m;
  std::vector<int> done;
  std::vector<std::thread> clients;
  for (int i = 0; i < 5; ++i) {
    clients.emplace_back([&, i] {
      const auto response = raw(*r.server, chat_request(*r.server, chat_body("", "client" + std::to_string(i))), 30s);
      std::lock_guard g(m);
      if (response && response->status == 200) done.push_back(i);
    });
    std::this_thread::sleep_for(100ms);
  }
  for (auto& t : clients) t.join();
  expect(done == std::vector<int>{0, 1, 2, 3, 4}, "completion order equals admission order");
}

void dos02_health_answers_while_generation_places_full() {
  auto r = start_recorded("hp", {}, {"--tokens=40", "--token-interval-ms=50"});
  if (!r.server) return;
  std::vector<std::thread> holders;
  for (int i = 0; i < 5; ++i) holders.emplace_back([&] { (void)raw(*r.server, chat_request(*r.server, chat_body("")), 60s); });
  std::this_thread::sleep_for(500ms);
  const auto started = std::chrono::steady_clock::now();
  const auto health = raw(*r.server, raw_http::request("GET", "/health", authority(*r.server), {raw_http::bearer(token())}));
  const auto models = raw(*r.server, raw_http::request("GET", "/v1/models", authority(*r.server), {raw_http::bearer(token())}));
  expect(health && health->status == 200 && health->body == "{\"status\":\"ok\",\"engine\":\"generating\"}\n", "/health answers at once");
  expect(models && models->status == 200, "/v1/models answers at once");
  expect(std::chrono::steady_clock::now() - started < 1s, "within one second while every generation place is taken");
  for (auto& t : holders) t.join();
}

void dos02_serial_mode_one_worker() {
  auto r = start_recorded("serial", {{"limits.concurrency.max_concurrent_requests", "1"}}, {"--tokens=40", "--token-interval-ms=50"});
  if (!r.server) return;
  std::thread first([&] { (void)raw(*r.server, chat_request(*r.server, chat_body("")), 30s); });
  std::this_thread::sleep_for(300ms);
  const auto second = raw(*r.server, chat_request(*r.server, chat_body("")));
  expect(second && second->status == 503 && second->body.find("server_busy") != std::string::npos, "the second chat is busy in serial mode");
  first.join();
}
```

(`#include <sstream>`, `<mutex>`, `<algorithm>`.)

- [ ] **Step 2: Run to see RED** — `cmake --build --preset linux-gcc && ctest --preset linux-gcc -R 'api_server_tests\.(max_tokens|prompt_alone|absent_max|sampling_pass|dos01_context|chat_response|dos02_)' --output-on-failure | tail -20`. Expected: FAIL — e.g. `sampling_passthrough_reaches_gen` ("GEN carries the keys": SP2's `execute` path writes no keys), `chat_response_has_openai_shape` ("32 lowercase hex": still `chatcmpl-local`), `dos02_queue_full_is_503` (concurrent chats get `engine_unavailable` from `acquire()` instead of queueing).

- [ ] **Step 3: Implement** — `include/secure_frontend/chat_route.hpp`:

```cpp
#pragma once

#include "secure_frontend/chat_execution.hpp"
#include "secure_frontend/chat_schema.hpp"
#include "secure_frontend/generation_gate.hpp"
#include "secure_frontend/request_router.hpp"
#include "secure_frontend/tokenizer_pack.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace secure_frontend {

struct ChatRouteConfig final {
  ChatSchemaLimits schema;
  std::string model_id;
  NativeChatProfile profile;
  std::size_t max_prompt_tokens;
  std::size_t max_completion_tokens;
  std::size_t default_max_tokens;   // 0 = auto (B1)
  std::chrono::milliseconds queue_timeout;
};

[[nodiscard]] std::optional<std::string> make_completion_id();

// POST /v1/chat/completions, non-streaming (spec §2.2 P4-P6, §2.5, §3.1, §3.5).
class ChatRoute final {
 public:
  ChatRoute(ChatRouteConfig config, SupervisorPort& port, GenerationGate& gate, const TokenizerPack& tokenizer);
  [[nodiscard]] HttpResponse handle(std::string_view body, const RequestContext& context);

 private:
  [[nodiscard]] HttpResponse refusal(GateRefusal refusal);
  [[nodiscard]] HttpResponse engine_unavailable();
  ChatRouteConfig config_;
  SupervisorPort& port_;
  GenerationGate& gate_;
  const TokenizerPack& tokenizer_;
  std::atomic<std::size_t> last_context_{0};   // last READY context seen through a lease; 0 = unknown
};

}  // namespace secure_frontend
```

`src/chat_route.cpp`:

```cpp
#include "secure_frontend/chat_route.hpp"

#include "secure_frontend/api_errors.hpp"
#include "secure_frontend/chat_generation_plan.hpp"
#include "secure_frontend/shutdown_signal.hpp"
#include "secure_frontend/token_generator.hpp"

#include <array>
#include <limits>

namespace secure_frontend {

std::optional<std::string> make_completion_id() {
  std::array<unsigned char, 16> bytes{};
  if (!fill_random(bytes.data(), bytes.size())) return std::nullopt;
  static constexpr char kHex[] = "0123456789abcdef";
  std::string id{"chatcmpl-"};
  for (const unsigned char b : bytes) { id.push_back(kHex[b >> 4U]); id.push_back(kHex[b & 0x0fU]); }
  return id;
}

ChatRoute::ChatRoute(ChatRouteConfig config, SupervisorPort& port, GenerationGate& gate, const TokenizerPack& tokenizer)
    : config_(std::move(config)), port_(port), gate_(gate), tokenizer_(tokenizer) {}

HttpResponse ChatRoute::engine_unavailable() {
  const HealthSnapshot snapshot = port_.snapshot();
  if (snapshot.state == SupervisorState::shutting_down || snapshot.state == SupervisorState::stopped) {
    return make_error_response(ApiError::server_shutting_down);
  }
  return make_error_response(ApiError::engine_unavailable, {.retry_after = engine_retry_after(snapshot.recovery)});
}

HttpResponse ChatRoute::refusal(const GateRefusal refusal) {
  switch (refusal) {
    case GateRefusal::busy:
    case GateRefusal::timeout: return make_error_response(ApiError::server_busy);
    case GateRefusal::cancelled: return HttpResponse{.status = kNoResponseStatus, .headers = {}, .body = {}, .log_code = "client_gone"};
    case GateRefusal::engine_unavailable: return engine_unavailable();
    case GateRefusal::shutting_down: return make_error_response(ApiError::server_shutting_down);
  }
  return make_error_response(ApiError::internal_error);
}

HttpResponse ChatRoute::handle(const std::string_view body, const RequestContext& context) {
  // P4: schema, tokenization, budget (spec §5.4) - before any queue place is taken.
  const ChatRequestResult parsed = parse_chat_request(body, config_.schema);
  if (const auto* error = std::get_if<ChatRequestError>(&parsed)) return make_error_response(error->error, {.param = error->param});
  const ChatCompletionRequest& request = std::get<ChatCompletionRequest>(parsed);
  if (request.model != config_.model_id) return make_error_response(ApiError::model_not_found);
  const auto plan_result = config_.profile == NativeChatProfile::qwen
                               ? make_qwen_chat_generation_plan(request, tokenizer_, config_.max_prompt_tokens, 1U)
                               : make_glm53_chat_generation_plan(request, tokenizer_, config_.max_prompt_tokens, 1U);
  if (const auto* plan_error = std::get_if<ChatGenerationPlanError>(&plan_result)) {
    return make_error_response(plan_error->code == ChatGenerationPlanErrorCode::prompt_too_large ? ApiError::context_length_exceeded
                                                                                                 : ApiError::invalid_request);
  }
  const ChatGenerationPlan& plan = std::get<ChatGenerationPlan>(plan_result);
  std::vector<std::uint32_t> prompt_ids;
  prompt_ids.reserve(plan.prompt_token_ids.size());
  for (const std::size_t id : plan.prompt_token_ids) {
    if (id > std::numeric_limits<std::uint32_t>::max()) return make_error_response(ApiError::invalid_request);
    prompt_ids.push_back(static_cast<std::uint32_t>(id));
  }
  if (!resolve_max_tokens(request.max_tokens, prompt_ids.size(), last_context_.load(), config_.max_completion_tokens,
                          config_.default_max_tokens)) {
    return make_error_response(ApiError::context_length_exceeded);
  }

  // P5: the gate.
  auto admitted = gate_.admit(port_, std::chrono::steady_clock::now() + config_.queue_timeout, context.client_gone);
  if (const auto* refused = std::get_if<GateRefusal>(&admitted)) return refusal(*refused);
  std::optional<Lease> lease;
  lease.emplace(std::get<Lease>(std::move(admitted)));

  // P6 through the lease; the context rule is re-checked against this generation's READY.
  GenerationOutcome outcome = GenerationFailure::engine_unavailable;
  bool context_refused = false;
  const std::size_t context_tokens = lease->chat().context_tokens();
  last_context_ = context_tokens;
  const auto budget = resolve_max_tokens(request.max_tokens, prompt_ids.size(), context_tokens, config_.max_completion_tokens,
                                         config_.default_max_tokens);
  if (!budget) {
    context_refused = true;
  } else {
    outcome = lease->chat().run_generation(prompt_ids, {.max_tokens = *budget, .sampling = request.sampling},
                                           {.cancelled = context.client_gone, .shutting_down = context.shutting_down,
                                            .shutdown_requested = [] { return shutdown_signalled(); }},
                                           tokenizer_);
    // Ruling R-H: never report_fatal once shutdown has begun (pool flag or SP2 signal).
    const bool shutting = (context.shutting_down != nullptr && context.shutting_down->load()) || shutdown_signalled();
    if (lease->protocol().closed() && !shutting) port_.report_fatal(*lease, kill_reason_for(lease->protocol()));
  }
  lease.reset();     // SP2: generating -> ready
  gate_.release();   // only now may the next head proceed

  if (context_refused) return make_error_response(ApiError::context_length_exceeded);
  if (const auto* failure = std::get_if<GenerationFailure>(&outcome)) {
    switch (*failure) {
      case GenerationFailure::cancelled: return HttpResponse{.status = kNoResponseStatus, .headers = {}, .body = {}, .log_code = "client_gone"};
      case GenerationFailure::timed_out: {
        HttpResponse response = make_error_response(ApiError::engine_error);   // M1; decision B4 lands in M2
        response.log_code = "generation_timeout";
        return response;
      }
      case GenerationFailure::engine_error: return make_error_response(ApiError::engine_error);
      case GenerationFailure::engine_unavailable: return engine_unavailable();
      case GenerationFailure::decode_failed: return make_error_response(ApiError::internal_error);
      case GenerationFailure::shutting_down: return make_error_response(ApiError::server_shutting_down);
    }
  }
  const GenerationResult& result = std::get<GenerationResult>(outcome);
  const auto id = make_completion_id();
  if (!id) return make_error_response(ApiError::internal_error);
  const auto created = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  const auto rendered = render_chat_completion({.id = *id, .created = created, .model = config_.model_id, .text = result.text,
                                                .finish_reason = result.finish == FinishReason::length ? "length" : "stop",
                                                .prompt_tokens = result.prompt_tokens, .completion_tokens = result.completion_tokens},
                                               config_.schema);
  if (!rendered) return make_error_response(ApiError::internal_error);
  return HttpResponse{.status = 200, .headers = {{"Content-Type", "application/json; charset=utf-8"}}, .body = *rendered};
}

}  // namespace secure_frontend
```

CMake: `secure_frontend_add_library(secure_frontend_chat_route SOURCES src/chat_route.cpp LINKS secure_frontend_generation_gate secure_frontend_chat_execution secure_frontend_chat_schema secure_frontend_chat_generation_plan secure_frontend_secrets secure_frontend_api_core)` plus the library that holds SP2 Task 15's `src/shutdown_signal.cpp` (find it with `grep -n shutdown_signal.cpp CMakeLists.txt`) for `shutdown_signalled()`; link `strata-secure-server` with it.

`src/secure_server_main.cpp`, in `run` — before the router (anchor `secure_frontend::ApiRouter router(secure_frontend::ApiRouterConfig{`), add:

```cpp
  secure_frontend::SupervisorPort port(supervisor);
  secure_frontend::GenerationGate gate({.max_queued = config.limits.concurrency.max_queued_generations,
                                        .queue_timeout = config.limits.concurrency.queue_timeout});
  secure_frontend::ChatRoute chat_route(
      secure_frontend::ChatRouteConfig{.schema = schema_limits, .model_id = model_id, .profile = profile,
                                       .max_prompt_tokens = config.limits.chat.max_prompt_tokens,
                                       .max_completion_tokens = config.limits.chat.max_completion_tokens,
                                       .default_max_tokens = config.limits.chat.default_max_tokens,
                                       .queue_timeout = config.limits.concurrency.queue_timeout},
      port, gate, *tokenizer);
```

(`profile` is the `NativeChatProfile` local SP1/SP2 derive from `config.engine.profile`.) In the router config, `.health` becomes `[&port] { const auto s = port.snapshot(); return secure_frontend::HealthView{s.healthy, s.engine, s.recovery}; }` and `.chat` becomes `[&chat_route](std::string_view body, const secure_frontend::RequestContext& context) { return chat_route.handle(body, context); }` (the Task 9 lambda, `engine_refusal` and the `execute_with_lease` call are deleted; `render_chat_completion_response` stays only if a test still uses it). The pool call becomes `pool.run_until([] { return secure_frontend::shutdown_signalled(); }, [&gate] { gate.shutdown(); })`. Declaration order in `run`: supervisor, port, gate, chat route, router, server, pool — so the pool (joined in `run_until`) never outlives the objects its workers use.

- [ ] **Step 4: Run to see GREEN**

```bash
cmake --build --preset linux-gcc && ctest --preset linux-gcc -R 'api_server_tests|server_tests|server_e2e' --output-on-failure
ctest --preset linux-gcc | tail -1
cmake --build --preset windows-llvm-mingw-cross && ctest --preset windows-llvm-mingw-cross -R 'api_server_tests|server_tests' | tail -1
```

Expected: all pass, including SP2's `proc04_client_err_lines_open_circuit_until_restart` (an `ERR` still reports through the port) and `proc04_abort_in_generation_is_generic_503` (`engine_unavailable`).

- [ ] **Step 5: Handoff row and commit** — row `| 16 | DOS-02 (queue, busy, timeout, FIFO, health while full, serial), B1, B2, B6, §3.5, §5.4 | ChatRoute: P4 validation before a queue place, gate, P6 via lease, OpenAI response | api_server_tests.max_tokens_above_limit_is_clamped, …max_tokens_clamped_to_context_room, …prompt_alone_over_context_is_400, …absent_max_tokens_uses_auto, …sampling_passthrough_reaches_gen, …dos01_context_rule_is_400_without_gen, …chat_response_has_openai_shape, …dos02_excess_connections_get_503_retry_after, …dos02_queue_full_is_503, …dos02_queue_timeout_is_503, …dos02_queue_is_fifo, …dos02_health_answers_while_generation_places_full, …dos02_serial_mode_one_worker | … | … |`.

```bash
git -C "$(git rev-parse --show-toplevel)" add secure_frontend docs/AGENT_HANDOFF.md
git -C "$(git rev-parse --show-toplevel)" -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "chat: validated chats queue FIFO behind one generation; max_tokens resolved, sampling passed, real ids and usage (SP3 §2.5, §3.5, B1, B2, B6, DOS-02)" \
  -m "Tests: api_server_tests.max_tokens_above_limit_is_clamped, max_tokens_clamped_to_context_room, prompt_alone_over_context_is_400, absent_max_tokens_uses_auto, sampling_passthrough_reaches_gen, dos01_context_rule_is_400_without_gen, chat_response_has_openai_shape, dos02_excess_connections_get_503_retry_after, dos02_queue_full_is_503, dos02_queue_timeout_is_503, dos02_queue_is_fifo, dos02_health_answers_while_generation_places_full, dos02_serial_mode_one_worker. RED: <line>. wine: <line>." \
  -m "Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>" -m "Claude-Session: <session link>"
```

---

## Task 17: Disconnect cancellation, resource release and shutdown — acceptance

**Files:**
- Modify: `tests/api_server_test.cpp`, `CMakeLists.txt`; product files only if a case exposes a defect (named in the commit)

**Interfaces:**
- Consumes: everything from Tasks 6a–16; `test_support::thread_count`, `handle_count` (Task 12); harness `ServerProcess::stop`, `wait_exit`, `stderr_text`, `pid`.
- Produces: the DOS-02 cancel/release cases, the SP3 shutdown rule (`shutdown_errors_never_report_fatal`) and the service-health sweep. The behavior was built in Tasks 6a (`kNoResponseStatus`), 8a/8b (the `client_gone` probe and `deliver_route_result`), 13 (cancelled waiters), 15 (STOP + drain, shutdown drain) and 16 (no `report_fatal` while shutting down); these are acceptance tests that should pass on their first run — if one fails, stop, fix the owning file, and name the defect.

- [ ] **Step 1: Write the tests** — append and register in `SECURE_FRONTEND_API_SERVER_CASES`:

```cpp
void dos02_queued_client_disconnect_leaves_queue() {
  auto r = start_recorded("qdc", {}, {"--tokens=40", "--token-interval-ms=50"});
  if (!r.server) return;
  std::thread a([&] { (void)raw(*r.server, chat_request(*r.server, chat_body("", "AAA")), 30s); });
  std::this_thread::sleep_for(300ms);
  {
    auto b = raw_http::Connection::open("127.0.0.1", r.server->port());
    expect(b && b->send(chat_request(*r.server, chat_body("", "BBB"))), "B queued");
    std::this_thread::sleep_for(300ms);
  }   // B goes away while queued
  const auto c = raw(*r.server, chat_request(*r.server, chat_body("", "CCC")), 30s);
  a.join();
  expect(c && c->status == 200, "C is served");
  expect(gen_lines(record_of(r.record)).size() == 2U, "only A and C reached the engine; B left the queue silently");
}

void dos02_non_stream_disconnect_cancels() {
  auto r = start_recorded("ndc", {}, {"--tokens=10000", "--token-interval-ms=5"});
  if (!r.server) return;
  {
    auto c = raw_http::Connection::open("127.0.0.1", r.server->port());
    expect(c && c->send(chat_request(*r.server, chat_body(""))), "a long chat");
    std::this_thread::sleep_for(300ms);
  }   // client gone during P6
  bool stopped = false;
  for (int i = 0; i < 50 && !stopped; ++i) {
    stopped = record_of(r.record).find("\nSTOP\n") != std::string::npos;
    if (!stopped) std::this_thread::sleep_for(20ms);
  }
  expect(stopped, "STOP reached the engine within 1 s of the disconnect");
  const auto started = std::chrono::steady_clock::now();
  const auto next = raw(*r.server, chat_request(*r.server, chat_body(R"("max_tokens":2)")), 5s);
  expect(next && next->status == 200 && std::chrono::steady_clock::now() - started < 1s, "the next request is served within 1 s");
  expect(r.server->stderr_text().find("killed_by_supervisor") == std::string::npos, "a drained cancel never kills the engine");
}

void dos02_resources_return_to_idle() {
  auto r = start_recorded("idle", {}, {"--tokens=10000", "--token-interval-ms=5"});
  if (!r.server) return;
  (void)raw(*r.server, raw_http::request("GET", "/health", authority(*r.server), {raw_http::bearer(token())}));
  std::this_thread::sleep_for(300ms);
  const auto threads0 = test_support::thread_count(r.server->pid());
  const auto handles0 = test_support::handle_count(r.server->pid());
  for (int i = 0; i < 100; ++i) {
    auto c = raw_http::Connection::open("127.0.0.1", r.server->port());
    if (c) (void)c->send(chat_request(*r.server, chat_body("")));
    std::this_thread::sleep_for(30ms);
  }
  bool idle = false;
  for (int i = 0; i < 100 && !idle; ++i) {
    std::this_thread::sleep_for(50ms);
    idle = test_support::thread_count(r.server->pid()) == threads0 && test_support::handle_count(r.server->pid()) == handles0;
  }
  expect(idle, "after 100 cancelled chats threads (" + std::to_string(test_support::thread_count(r.server->pid())) + "/" +
               std::to_string(threads0) + ") and handles (" + std::to_string(test_support::handle_count(r.server->pid())) + "/" +
               std::to_string(handles0) + ") return to their idle values");
  const auto health = raw(*r.server, raw_http::request("GET", "/health", authority(*r.server), {raw_http::bearer(token())}));
  expect(health && health->status == 200, "and the service is healthy");
}

void shutdown_errors_never_report_fatal() {
  auto r = start_recorded("sd", {}, {"--tokens=10000", "--token-interval-ms=5"});
  if (!r.server) return;
  std::optional<raw_http::Response> answer;
  std::string raw_answer;
  std::thread chat([&] {
    auto c = raw_http::Connection::open("127.0.0.1", r.server->port());
    if (c && c->send(chat_request(*r.server, chat_body("")))) raw_answer = c->read_until_close(10s);
  });
  std::this_thread::sleep_for(500ms);
  const auto started = std::chrono::steady_clock::now();
#if defined(_WIN32)
  r.server->stop(test_support::ServerProcess::Stop::ctrl_break);
#else
  r.server->stop(test_support::ServerProcess::Stop::term);
#endif
  const auto code = r.server->wait_exit(10s);
  chat.join();
  expect(code == std::optional<int>{0} && std::chrono::steady_clock::now() - started < 5s, "exit 0 within SP2's 5 s bound");
  answer = raw_http::parse_response(raw_answer);
  expect(raw_answer.empty() || (answer && answer->status == 503 && answer->body == secure_frontend::render_error_body(ApiError::server_shutting_down)),
         "the in-flight chat gets 503 server_shutting_down or a close");
  const std::string log = r.server->stderr_text();
  expect(log.find("drain_failed") == std::string::npos && log.find("protocol_failed") == std::string::npos &&
         log.find("write_timeout") == std::string::npos, "no worker-side failure was reported as fatal during shutdown");
}

void dos01_service_healthy_after_each_attack() {
  auto server = start_server({{"limits.http.header_deadline_ms", "500"}, {"limits.http.request_deadline_ms", "1000"}});
  if (!server) return;
  const std::string a = authority(*server);
  const std::vector<std::pair<std::string, std::function<void()>>> attacks{
      {"oversize header", [&] { (void)raw(*server, "GET /health HTTP/1.1\r\nHost: " + a + "\r\nX: " + std::string(20000, 'x') + "\r\n\r\n"); }},
      {"deep JSON", [&] { (void)raw(*server, chat_request(*server, R"({"model":"fake-model","messages":[{"role":"user","content":"Hi"}],"logit_bias":{"a":{"b":{"c":{"d":1}}}}})")); }},
      {"chunked body", [&] { (void)raw(*server, raw_http::request("POST", "/v1/chat/completions", a, {raw_http::bearer(token()), "Transfer-Encoding: chunked"})); }},
      {"slow then closed body", [&] {
         auto c = raw_http::Connection::open("127.0.0.1", server->port());
         if (c) (void)c->send(chat_request(*server, chat_body()).substr(0, 120));
       }},
      {"slow header", [&] {
         auto c = raw_http::Connection::open("127.0.0.1", server->port());
         if (c) { (void)c->send("GET /he"); (void)c->read_until_close(2s); }
       }},
      {"body over limit", [&] { (void)raw(*server, chat_request(*server, std::string(60000, ' '))); }},
      {"garbage", [&] { (void)raw(*server, std::string("\x16\x03\x01\x02\x00", 5) + std::string(100, '\x01')); }}};
  for (const auto& [name, attack] : attacks) {
    attack();
    const auto health = raw(*server, raw_http::request("GET", "/health", a, {raw_http::bearer(token())}));
    const auto chat = raw(*server, chat_request(*server, chat_body()));
    expect(health && health->status == 200 && chat && chat->status == 200, "healthy and serving after: " + name);
  }
}
```

(`#include <functional>`.)

- [ ] **Step 2: Run** — `cmake --build --preset linux-gcc && ctest --preset linux-gcc -R 'api_server_tests\.(dos02_queued|dos02_non_stream|dos02_resources|shutdown_errors|dos01_service)' --output-on-failure`. Expected: PASS (record "acceptance, passed on first run"); a failure is a defect in the task that owns the behavior: fix it there, re-run, and name it.

- [ ] **Step 3: Full suite and cross build**

```bash
ctest --preset linux-gcc | tail -1
cmake --build --preset windows-llvm-mingw-cross && ctest --preset windows-llvm-mingw-cross -R api_server_tests | tail -1
```

- [ ] **Step 4: Handoff row and commit** — row `| 17 | DOS-02 (cancel, release), DOS-01 (health after attacks), §2.6, §2.8 | acceptance | api_server_tests.dos02_queued_client_disconnect_leaves_queue, …dos02_non_stream_disconnect_cancels, …dos02_resources_return_to_idle, …shutdown_errors_never_report_fatal, …dos01_service_healthy_after_each_attack | … | … |`.

```bash
git -C "$(git rev-parse --show-toplevel)" add secure_frontend docs/AGENT_HANDOFF.md
git -C "$(git rev-parse --show-toplevel)" -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "test: disconnect cancels and frees the engine, resources return to idle, shutdown never reports fatal (SP3 DOS-02, §2.6, §2.8)" \
  -m "Tests: api_server_tests.dos02_queued_client_disconnect_leaves_queue, dos02_non_stream_disconnect_cancels, dos02_resources_return_to_idle, shutdown_errors_never_report_fatal, dos01_service_healthy_after_each_attack. <passed on first run | defects fixed: ...>. wine: <line>." \
  -m "Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>" -m "Claude-Session: <session link>"
```

---
## Task 18: Static egress tripwire — ELF and PE import readers

**Files:**
- Create: `tests/support/binary_imports.hpp`, `tests/egress_test.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: SP2 `test_support::server_path()` (`STRATA_SECURE_SERVER_PATH` from `secure_frontend_test_paths`).
- Produces: `struct ImportReport { bool ok; std::string error; std::vector<std::string> libraries; std::vector<std::string> symbols; std::vector<std::pair<std::string, std::uint16_t>> ordinals; }`, `ImportReport read_elf_imports(const std::vector<unsigned char>&)`, `ImportReport read_pe_imports(const std::vector<unsigned char>&)`, `ImportReport read_binary_imports(const std::filesystem::path&)` (by magic), `std::vector<std::string> forbidden_imports(const ImportReport&)` (empty = clean); test binary `secure_frontend_egress_tests` with `egress_server_imports_no_resolver_or_http_client` and `egress_static_tripwire_fails_on_binary_without_dynamic_section`.

ELF (x86-64, little-endian, ELFCLASS64): undefined `.dynsym` symbols (`st_shndx == 0`, non-empty name) and `DT_NEEDED` libraries; **no `SHT_DYNSYM` → `ok = false`, "no dynamic section"** (a static build must replace the check deliberately). Forbidden (after stripping leading `_`): `getaddrinfo`, `gethostbyname`, `gethostbyname2`, `gethostbyaddr`, `getnameinfo`, `res_query`, `res_search`, `res_nquery`. PE (PE32+): import directory (data directory 1) → DLL names, by-name imports and by-ordinal imports; **no import directory → `ok = false`**. Forbidden: DLLs `winhttp.dll`, `wininet.dll`, `urlmon.dll`, `dnsapi.dll` (case-insensitive); names `getaddrinfo`, `GetAddrInfoW`, `GetAddrInfoExW`, `gethostbyname`, `WSAConnect`; **any ordinal-only import from `ws2_32.dll`** (so `gethostbyname`/`gethostbyaddr` by ordinal 52/51 are caught too).

- [ ] **Step 1: Write the failing tests** — `tests/egress_test.cpp`:

```cpp
#include "support/binary_imports.hpp"
#include "support/test_paths.hpp"
#include "support/test_support.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {
int failures = 0;
void expect(bool condition, const std::string& message) {
  if (!condition) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
}
bool has(const std::vector<std::string>& values, std::string_view wanted) {
  return std::any_of(values.begin(), values.end(), [wanted](const std::string& v) {
    return v.size() == wanted.size() && std::equal(v.begin(), v.end(), wanted.begin(), [](char a, char b) {
             return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
           });
  });
}

void egress_server_imports_no_resolver_or_http_client() {
  const auto report = test_support::read_binary_imports(test_support::server_path());
  expect(report.ok, "the server binary has an import table: " + report.error);
  // Positive controls: the reader really sees the socket imports the listener needs.
#if defined(_WIN32)
  expect(has(report.libraries, "ws2_32.dll"), "ws2_32.dll is imported");
#else
  expect(has(report.symbols, "accept") || has(report.symbols, "accept4"), "accept is an undefined dynamic symbol");
#endif
  const auto forbidden = test_support::forbidden_imports(report);
  std::string list;
  for (const auto& f : forbidden) list += f + " ";
  expect(forbidden.empty(), "no resolver, HTTP client or outbound-connect import: " + list);
}

// Little-endian writers for the synthetic images (ruling R-I: they go through the real parsers).
void put(std::vector<unsigned char>& b, std::size_t at, std::uint64_t value, std::size_t width) {
  if (b.size() < at + width) b.resize(at + width, 0);
  for (std::size_t i = 0; i < width; ++i) b[at + i] = static_cast<unsigned char>(value >> (8U * i));
}
void put_text(std::vector<unsigned char>& b, std::size_t at, std::string_view text) {
  if (b.size() < at + text.size() + 1U) b.resize(at + text.size() + 1U, 0);
  std::copy(text.begin(), text.end(), b.begin() + static_cast<std::ptrdiff_t>(at));
}
// ELF64 x86-64: .dynstr at 64, optional .dynsym at 96 (null + getaddrinfo + accept, all undefined),
// .dynamic at 168 (DT_NEEDED libc.so.6), section table at 200.
std::vector<unsigned char> synthetic_elf(const bool with_dynsym) {
  std::vector<unsigned char> b(64, 0);
  b[0] = 0x7f; b[1] = 'E'; b[2] = 'L'; b[3] = 'F'; b[4] = 2; b[5] = 1; b[6] = 1;
  put(b, 0x10, 2, 2);      // ET_EXEC
  put(b, 0x12, 0x3e, 2);   // EM_X86_64
  put_text(b, 65, "getaddrinfo");   // dynstr: "\0getaddrinfo\0accept\0libc.so.6\0" (offsets 1, 13, 20)
  put_text(b, 77, "accept");
  put_text(b, 84, "libc.so.6");
  put(b, 96 + 24 * 1, 1, 4);   // sym 1: st_name = 1, st_shndx = 0 (undefined)
  put(b, 96 + 24 * 2, 13, 4);  // sym 2: st_name = 13
  put(b, 168, 1, 8);           // DT_NEEDED
  put(b, 176, 20, 8);          //   -> "libc.so.6"
  put(b, 199, 0, 1);           // zero DT_NULL terminator up to the section table
  const std::size_t table = 200;
  std::size_t index = 1;       // section 0 is the null section
  const auto section = [&](std::uint32_t type, std::uint64_t offset, std::uint64_t size, std::uint32_t link, std::uint64_t entsize) {
    const std::size_t base = table + index * 64U;
    put(b, base + 4, type, 4);
    put(b, base + 0x18, offset, 8);
    put(b, base + 0x20, size, 8);
    put(b, base + 0x28, link, 4);
    put(b, base + 0x38, entsize, 8);
    ++index;
  };
  section(3, 64, 30, 0, 0);                    // 1: .dynstr (SHT_STRTAB)
  if (with_dynsym) section(11, 96, 72, 1, 24); // 2: .dynsym (SHT_DYNSYM) -> strtab 1
  section(6, 168, 32, 1, 16);                  // .dynamic (SHT_DYNAMIC) -> strtab 1
  put(b, table + index * 64U - 1U, 0, 1);      // size the buffer to the whole table
  put(b, 0x28, table, 8);                      // e_shoff
  put(b, 0x3a, 64, 2);                         // e_shentsize
  put(b, 0x3c, index, 2);                      // e_shnum
  return b;
}
// PE32+: one section (VA 0x1000, raw 0x200); import directory at RVA 0x1000 naming WS2_32.dll
// with one ordinal-only thunk (52 = gethostbyname).
std::vector<unsigned char> synthetic_pe() {
  std::vector<unsigned char> b(0x400, 0);
  b[0] = 'M'; b[1] = 'Z';
  put(b, 0x3c, 0x40, 4);              // e_lfanew
  put_text(b, 0x40, "PE");            // "PE\0\0"
  put(b, 0x44, 0x8664, 2);            // machine x64
  put(b, 0x46, 1, 2);                 // one section
  put(b, 0x54, 240, 2);               // SizeOfOptionalHeader
  put(b, 0x58, 0x20b, 2);             // PE32+ magic (optional header at 0x58)
  put(b, 0x58 + 120, 0x1000, 4);      // import directory RVA
  put(b, 0x58 + 124, 40, 4);          // import directory size
  const std::size_t section = 0x58 + 240;
  put(b, section + 8, 0x200, 4);      // VirtualSize
  put(b, section + 12, 0x1000, 4);    // VirtualAddress
  put(b, section + 16, 0x200, 4);     // SizeOfRawData
  put(b, section + 20, 0x200, 4);     // PointerToRawData
  put(b, 0x200 + 0, 0x1040, 4);       // descriptor 0: OriginalFirstThunk
  put(b, 0x200 + 12, 0x1060, 4);      //               Name
  put(b, 0x200 + 16, 0x1040, 4);      //               FirstThunk (descriptor 1 stays zero)
  put(b, 0x240, (1ULL << 63U) | 52U, 8);   // ordinal-only thunk, then a zero thunk
  put_text(b, 0x260, "WS2_32.dll");
  return b;
}

void egress_static_tripwire_fails_on_binary_without_dynamic_section() {
  const auto stripped = test_support::read_elf_imports(synthetic_elf(false));
  expect(!stripped.ok && stripped.error.find("no dynamic section") != std::string::npos,
         "SHT_DYNAMIC without SHT_DYNSYM fails the tripwire, never passes vacuously: " + stripped.error);
  const auto resolver = test_support::read_elf_imports(synthetic_elf(true));
  expect(resolver.ok && has(resolver.symbols, "getaddrinfo") && has(resolver.symbols, "accept") && has(resolver.libraries, "libc.so.6"),
         "the parser reads .dynsym and DT_NEEDED: " + resolver.error);
  const auto flagged = test_support::forbidden_imports(resolver);
  expect(flagged.size() == 1U && flagged.front() == "getaddrinfo", "an undefined getaddrinfo is flagged");
  const auto pe = test_support::read_pe_imports(synthetic_pe());
  expect(pe.ok && has(pe.libraries, "ws2_32.dll") && pe.ordinals.size() == 1U && pe.ordinals.front().second == 52U,
         "the PE parser walks the import directory to the ordinal thunk: " + pe.error);
  expect(!test_support::forbidden_imports(pe).empty(), "an ordinal-only ws2_32 import (gethostbyname #52) is flagged");
  std::vector<unsigned char> no_imports = synthetic_pe();
  put(no_imports, 0x58 + 120, 0, 8);   // clear the import directory entry
  const auto empty = test_support::read_pe_imports(no_imports);
  expect(!empty.ok && empty.error.find("no import directory") != std::string::npos, "a PE without an import directory fails");
}
}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"egress_server_imports_no_resolver_or_http_client", egress_server_imports_no_resolver_or_http_client},
      {"egress_static_tripwire_fails_on_binary_without_dynamic_section", egress_static_tripwire_fails_on_binary_without_dynamic_section},
  }, failures);
}
```

CMake (tests block):

```cmake
    secure_frontend_add_test_executable(secure_frontend_egress_tests tests/egress_test.cpp secure_frontend_test_support)
    secure_frontend_test_paths(secure_frontend_egress_tests)
    set(SECURE_FRONTEND_EGRESS_CASES egress_server_imports_no_resolver_or_http_client
        egress_static_tripwire_fails_on_binary_without_dynamic_section)
    secure_frontend_add_cases(secure_frontend_egress_tests ${SECURE_FRONTEND_EGRESS_CASES} TIMEOUT 120)
```

- [ ] **Step 2: Run to see RED** — `cmake --preset linux-gcc && cmake --build --preset linux-gcc 2>&1 | grep -m1 error` → FAIL `support/binary_imports.hpp: No such file or directory`.

- [ ] **Step 3: Implement** — `tests/support/binary_imports.hpp`:

```cpp
#pragma once
// Test-only readers for the static egress tripwire (spec §6.2). Standard library only.
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace test_support {

struct ImportReport final {
  bool ok{false};
  std::string error;
  std::vector<std::string> libraries;
  std::vector<std::string> symbols;
  std::vector<std::pair<std::string, std::uint16_t>> ordinals;   // (dll, ordinal) for ordinal-only imports
};

namespace detail_imports {
template <typename T>
bool read_le(const std::vector<unsigned char>& bytes, std::uint64_t offset, T* out) {
  if (offset > bytes.size() || bytes.size() - offset < sizeof(T)) return false;
  T value = 0;
  for (std::size_t i = 0; i < sizeof(T); ++i) value |= static_cast<T>(static_cast<T>(bytes[offset + i]) << (8U * i));
  *out = value;
  return true;
}
inline std::string c_string(const std::vector<unsigned char>& bytes, std::uint64_t offset) {
  std::string out;
  while (offset < bytes.size() && bytes[offset] != 0) out.push_back(static_cast<char>(bytes[offset++]));
  return out;
}
inline std::string lower(std::string value) {
  for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return value;
}
}  // namespace detail_imports

inline ImportReport read_elf_imports(const std::vector<unsigned char>& b) {
  using detail_imports::read_le;
  ImportReport r;
  if (b.size() < 64U || b[0] != 0x7f || b[1] != 'E' || b[2] != 'L' || b[3] != 'F' || b[4] != 2 || b[5] != 1) {
    r.error = "not a 64-bit little-endian ELF";
    return r;
  }
  std::uint64_t shoff = 0;
  std::uint16_t shentsize = 0, shnum = 0;
  if (!read_le(b, 0x28, &shoff) || !read_le(b, 0x3a, &shentsize) || !read_le(b, 0x3c, &shnum)) { r.error = "truncated header"; return r; }
  struct Section { std::uint32_t type; std::uint64_t offset, size, entsize; std::uint32_t link; };
  std::vector<Section> sections;
  for (std::uint16_t i = 0; i < shnum; ++i) {
    const std::uint64_t base = shoff + static_cast<std::uint64_t>(i) * shentsize;
    Section s{};
    if (!read_le(b, base + 4, &s.type) || !read_le(b, base + 0x18, &s.offset) || !read_le(b, base + 0x20, &s.size) ||
        !read_le(b, base + 0x28, &s.link) || !read_le(b, base + 0x38, &s.entsize)) {
      r.error = "truncated section table";
      return r;
    }
    sections.push_back(s);
  }
  const auto dynsym = std::find_if(sections.begin(), sections.end(), [](const Section& s) { return s.type == 11U; });
  if (dynsym == sections.end() || dynsym->link >= sections.size()) { r.error = "no dynamic section (static binary?)"; return r; }
  const Section& strtab = sections[dynsym->link];
  for (std::uint64_t off = dynsym->offset; off + 24U <= dynsym->offset + dynsym->size; off += 24U) {
    std::uint32_t name = 0;
    std::uint16_t shndx = 0;
    if (!read_le(b, off, &name) || !read_le(b, off + 6, &shndx)) break;
    if (shndx == 0U && name != 0U) r.symbols.push_back(detail_imports::c_string(b, strtab.offset + name));
  }
  for (const Section& dynamic : sections) {
    if (dynamic.type != 6U || dynamic.link >= sections.size()) continue;
    for (std::uint64_t off = dynamic.offset; off + 16U <= dynamic.offset + dynamic.size; off += 16U) {
      std::uint64_t tag = 0, value = 0;
      if (!read_le(b, off, &tag) || !read_le(b, off + 8, &value) || tag == 0U) break;
      if (tag == 1U) r.libraries.push_back(detail_imports::c_string(b, sections[dynamic.link].offset + value));
    }
  }
  r.ok = true;
  return r;
}

inline ImportReport read_pe_imports(const std::vector<unsigned char>& b) {
  using detail_imports::read_le;
  ImportReport r;
  std::uint32_t pe = 0;
  if (b.size() < 0x40U || b[0] != 'M' || b[1] != 'Z' || !read_le(b, 0x3c, &pe) || pe + 24U > b.size() ||
      std::memcmp(b.data() + pe, "PE\0\0", 4) != 0) {
    r.error = "not a PE image";
    return r;
  }
  std::uint16_t sections = 0, optional_size = 0, magic = 0;
  read_le(b, pe + 6, &sections);
  read_le(b, pe + 20, &optional_size);
  const std::uint64_t optional = pe + 24U;
  if (!read_le(b, optional, &magic) || magic != 0x20bU) { r.error = "not PE32+"; return r; }
  std::uint32_t import_rva = 0, import_size = 0;
  if (optional_size < 128U || !read_le(b, optional + 120, &import_rva) || !read_le(b, optional + 124, &import_size) || import_rva == 0U) {
    r.error = "no import directory";
    return r;
  }
  const std::uint64_t table = optional + optional_size;
  const auto to_offset = [&](std::uint64_t rva) -> std::uint64_t {
    for (std::uint16_t i = 0; i < sections; ++i) {
      std::uint32_t vsize = 0, vaddr = 0, raw_size = 0, raw = 0;
      const std::uint64_t s = table + static_cast<std::uint64_t>(i) * 40U;
      if (!read_le(b, s + 8, &vsize) || !read_le(b, s + 12, &vaddr) || !read_le(b, s + 16, &raw_size) || !read_le(b, s + 20, &raw)) break;
      if (rva >= vaddr && rva < vaddr + std::max(vsize, raw_size)) return raw + (rva - vaddr);
    }
    return UINT64_MAX;
  };
  for (std::uint64_t d = to_offset(import_rva); d != UINT64_MAX && d + 20U <= b.size(); d += 20U) {
    std::uint32_t oft = 0, name = 0, ft = 0;
    read_le(b, d, &oft);
    read_le(b, d + 12, &name);
    read_le(b, d + 16, &ft);
    if (oft == 0U && name == 0U && ft == 0U) break;
    const std::string dll = detail_imports::c_string(b, to_offset(name));
    r.libraries.push_back(dll);
    for (std::uint64_t t = to_offset(oft != 0U ? oft : ft); t != UINT64_MAX && t + 8U <= b.size(); t += 8U) {
      std::uint64_t thunk = 0;
      read_le(b, t, &thunk);
      if (thunk == 0U) break;
      if ((thunk & (1ULL << 63U)) != 0U) r.ordinals.emplace_back(dll, static_cast<std::uint16_t>(thunk & 0xffffU));
      else r.symbols.push_back(detail_imports::c_string(b, to_offset(thunk & 0x7fffffffULL) + 2U));
    }
  }
  r.ok = true;
  return r;
}

inline ImportReport read_binary_imports(const std::filesystem::path& file) {
  std::ifstream in(file, std::ios::binary);
  const std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  if (bytes.size() >= 2U && bytes[0] == 'M' && bytes[1] == 'Z') return read_pe_imports(bytes);
  return read_elf_imports(bytes);
}

inline std::vector<std::string> forbidden_imports(const ImportReport& report) {
  static const std::vector<std::string> kSymbols{"getaddrinfo", "gethostbyname", "gethostbyname2", "gethostbyaddr", "getnameinfo",
                                                 "res_query", "res_search", "res_nquery", "getaddrinfow", "getaddrinfoexw", "wsaconnect"};
  static const std::vector<std::string> kLibraries{"winhttp.dll", "wininet.dll", "urlmon.dll", "dnsapi.dll"};
  std::vector<std::string> found;
  for (const auto& symbol : report.symbols) {
    std::string bare = symbol;
    bare.erase(0, bare.find_first_not_of('_'));
    if (std::find(kSymbols.begin(), kSymbols.end(), detail_imports::lower(bare)) != kSymbols.end()) found.push_back(symbol);
  }
  for (const auto& library : report.libraries) {
    if (std::find(kLibraries.begin(), kLibraries.end(), detail_imports::lower(library)) != kLibraries.end()) found.push_back(library);
  }
  for (const auto& [dll, ordinal] : report.ordinals) {
    if (detail_imports::lower(dll) == "ws2_32.dll") found.push_back(dll + "#" + std::to_string(ordinal));
  }
  return found;
}

}  // namespace test_support
```

(`<climits>`/`<cstdint>` give `UINT64_MAX`.)

- [ ] **Step 4: Run to see GREEN** — Linux and wine (the PE reader runs on the `.exe` under wine; it only reads a file):

```bash
cmake --build --preset linux-gcc && ctest --preset linux-gcc -R egress_tests --output-on-failure && ctest --preset linux-gcc | tail -1
cmake --build --preset windows-llvm-mingw-cross && ctest --preset windows-llvm-mingw-cross -R egress_tests --output-on-failure | tail -3
```

If the Linux server reports a forbidden import, **stop**: that is a real egress path in the serve binary (spec §6.3); report it to the controller.

- [ ] **Step 5: Handoff row and commit** — row `| 18 | PRIV-01/DL-01 (frontend part), §6.2 static tripwire | ELF/PE import readers; fails on a binary without a dynamic section/import directory | egress_tests.egress_server_imports_no_resolver_or_http_client, …egress_static_tripwire_fails_on_binary_without_dynamic_section | … | … |`.

```bash
git -C "$(git rev-parse --show-toplevel)" add secure_frontend docs/AGENT_HANDOFF.md
git -C "$(git rev-parse --show-toplevel)" -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "test: static egress tripwire over the server's ELF/PE imports; never passes vacuously (SP3 §6.2)" \
  -m "Tests: egress_tests.egress_server_imports_no_resolver_or_http_client, egress_static_tripwire_fails_on_binary_without_dynamic_section (synthetic images, controller note 10). RED: <line>. wine: <line>." \
  -m "Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>" -m "Claude-Session: <session link>"
```

---

## Task 19: Runtime egress checks — `LD_PRELOAD` interposer, connection-table scan, command-like values

**Files:**
- Create: `tests/support/egress_tripwire.cpp`, `tests/egress_probe_helper.cpp`, `tests/support/egress_support.hpp`
- Modify: `tests/egress_test.cpp`, `tests/support/server_harness.hpp` (only if `extra_env` is not already a `ServerOptions` member), `CMakeLists.txt`

**Interfaces:**
- Consumes: harness `ServerOptions::extra_env`, `config_literals`, `engine_args`, `ServerProcess::{pid, port, descendants, active_processes, stop, wait_exit, stderr_text}`; `test_support::sockets_of` (Task 12); `raw_http` (Task 8b).
- Produces: shared library `strata_egress_tripwire` (Linux only; output `libstrata_egress_tripwire.so`), executable `strata-egress-probe-helper` (Linux only), compile definitions `STRATA_EGRESS_TRIPWIRE_PATH` / `STRATA_EGRESS_PROBE_PATH` added by `secure_frontend_test_paths()` on Linux; `egress_support.hpp`: `bool arm_egress_tripwire(ServerOptions&, const std::filesystem::path& record)` (false where there is no interposer), `class SocketScan { SocketScan(long pid, std::uint16_t port); std::vector<std::string> stop(); }` (samples every 50 ms the server and, on POSIX, its descendants; a violation is any TCP socket whose local port is not the listener port, any UDP socket, any Unix socket), `void run_lifecycle(test_support::ServerProcess&, const std::filesystem::path& record)`.

The interposer wraps `connect`, `sendto` (with a destination), `sendmsg` (with `msg_name`), `getaddrinfo`, `gethostbyname`, and `socket` for `AF_UNIX`, `AF_NETLINK` and `SOCK_DGRAM`; each call appends one line to `$STRATA_EGRESS_RECORD` (test-only library; reading this variable is test code) and fails with `EACCES` (`EAI_FAIL` for `getaddrinfo`, `nullptr` for `gethostbyname`). The record must be empty after the lifecycle. The interposer is loaded into the frontend only: SP2 Task 18's typed environment allowlist builds the engine child's environment from scratch and lists `LD_PRELOAD` among the never-forwarded variables (`kNeverForwardedEnvironment` in `engine_environment.hpp`; Task 0 greps it), so the fake engine never inherits it — the connection-table scan covers the engine instead. **If the record shows a call the server makes for a legitimate local reason (for example an NSS lookup opening a Unix socket), stop and report — do not widen the wrapper.**

- [ ] **Step 1: Write the failing tests** — `tests/support/egress_support.hpp`:

```cpp
#pragma once
#include "support/connection_scan.hpp"
#include "support/raw_http.hpp"
#include "support/server_harness.hpp"
#include "support/test_paths.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace test_support {

inline bool arm_egress_tripwire(ServerOptions& options, const std::filesystem::path& record) {
#if defined(STRATA_EGRESS_TRIPWIRE_PATH)
  options.extra_env["LD_PRELOAD"] = STRATA_EGRESS_TRIPWIRE_PATH;
  options.extra_env["STRATA_EGRESS_RECORD"] = utf8_path(record);
  return true;
#else
  (void)options;
  (void)record;
  return false;
#endif
}

inline std::string file_text(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Spec §6.2 connection-table scan, every 50 ms, server and (POSIX) its descendants.
class SocketScan final {
 public:
  SocketScan(const long pid, const std::uint16_t port) : pid_(pid), port_(port), thread_([this] { loop(); }) {}
  ~SocketScan() { (void)stop(); }
  std::vector<std::string> stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
    std::lock_guard lock(mutex_);
    return violations_;
  }

 private:
  void check(const long pid, const bool is_server) {
    for (const auto& row : sockets_of(pid)) {
      const bool tcp = row.kind == "tcp" || row.kind == "tcp6";
      const bool ok = tcp && is_server && row.local_port == port_;
      if (ok) continue;
      std::lock_guard lock(mutex_);
      violations_.push_back((is_server ? "server " : "engine ") + row.kind + " local port " + std::to_string(row.local_port));
    }
  }
  void loop() {
    while (running_) {
      check(pid_, true);
#if !defined(_WIN32)
      for (const long child : descendants_of(pid_)) check(child, false);
#endif
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }
  long pid_;
  std::uint16_t port_;
  std::atomic<bool> running_{true};
  std::mutex mutex_;
  std::vector<std::string> violations_;
  std::thread thread_;
};

// The §6.2 lifecycle (M1 part; M2 adds a stream): health, models, chat, cancel,
// command-like values, refusal flood, shutdown. The engine restart happened at
// startup (--fail-first=1).
inline void run_lifecycle(ServerProcess& server, const std::string& token) {
  using namespace std::chrono_literals;
  const std::string a = "127.0.0.1:" + std::to_string(server.port());
  const auto get = [&](const std::string& target) {
    return raw_http::exchange("127.0.0.1", server.port(), raw_http::request("GET", target, a, {raw_http::bearer(token)}));
  };
  const auto chat = [&](const std::string& body) {
    return raw_http::exchange("127.0.0.1", server.port(),
                              raw_http::request("POST", "/v1/chat/completions", a, {raw_http::bearer(token), "Content-Type: application/json"}, body),
                              30s);
  };
  (void)get("/health");
  (void)get("/v1/models");
  (void)chat(R"({"model":"fake-model","messages":[{"role":"user","content":"Hi"}],"max_tokens":2})");
  {
    auto c = raw_http::Connection::open("127.0.0.1", server.port());
    if (c) (void)c->send(raw_http::request("POST", "/v1/chat/completions", a, {raw_http::bearer(token), "Content-Type: application/json"},
                                           R"({"model":"fake-model","messages":[{"role":"user","content":"Hi"}]})"));
    std::this_thread::sleep_for(300ms);
  }   // cancel
  for (const std::string body : {R"({"model":"pull:x","messages":[{"role":"user","content":"Hi"}]})",
                                 R"({"model":"fake-model","messages":[{"role":"user","content":"see https://example.com/m.gguf"}]})",
                                 R"({"model":"fake-model","messages":[{"role":"user","content":"mcp://tools.example"}]})",
                                 R"({"model":"fake-model","messages":[{"role":"user","content":"file:///etc/passwd"}]})",
                                 R"({"model":"fake-model","messages":[{"role":"user","content":"data:text/plain,x"}]})"}) {
    (void)chat(body);
  }
  (void)get("/v1/models/pull:x");
  std::vector<raw_http::Connection> flood;
  for (int i = 0; i < 40; ++i) {
    auto c = raw_http::Connection::open("127.0.0.1", server.port());
    if (c) { (void)c->send("GET /health HTTP/1.1\r\n"); flood.push_back(std::move(*c)); }
  }
  std::this_thread::sleep_for(200ms);
  flood.clear();
  (void)get("/health");
}

}  // namespace test_support
```

(`#include <fstream>`, `<iterator>`; `descendants_of` is SP2's harness function.)

Append to `tests/egress_test.cpp` (include `support/egress_support.hpp`, `support/server_harness.hpp`, `secure_frontend/socket_io.hpp`, `<cstdlib>`; hold a `secure_frontend::WinsockGuard` in `main`):

```cpp
void egress_interposer_self_test() {
#if defined(STRATA_EGRESS_PROBE_PATH) && defined(STRATA_EGRESS_TRIPWIRE_PATH)
  const auto dir = test_support::make_temp_dir("probe");
  const auto record = dir / "record.txt";
  const auto out = dir / "out.txt";
  const std::string command = std::string{"LD_PRELOAD='"} + STRATA_EGRESS_TRIPWIRE_PATH + "' STRATA_EGRESS_RECORD='" +
                              test_support::utf8_path(record) + "' '" + STRATA_EGRESS_PROBE_PATH + "' > '" + test_support::utf8_path(out) + "'";
  expect(std::system(command.c_str()) == 0, "the probe helper runs");
  const std::string recorded = test_support::file_text(record);
  for (const char* call : {"getaddrinfo", "connect", "socket_unix", "socket_dgram", "gethostbyname"}) {
    expect(recorded.find(call) != std::string::npos, std::string{"recorded: "} + call);
  }
  const std::string printed = test_support::file_text(out);
  expect(printed.find("getaddrinfo=denied") != std::string::npos && printed.find("connect=denied") != std::string::npos &&
         printed.find("unix=denied") != std::string::npos, "and denied: " + printed);
#else
  test_support::skip("the LD_PRELOAD interposer is Linux-only");
#endif
}

void egress_interposer_records_nothing_during_lifecycle() {
  const auto dir = test_support::make_temp_dir("ipose");
  const auto record = dir / "egress.txt";
  test_support::ServerOptions options;
  options.engine_args = {"--fail-first=1", "--counter-file=" + test_support::utf8_path(dir / "count"), "--tokens=10000", "--token-interval-ms=5"};
  if (!test_support::arm_egress_tripwire(options, record)) test_support::skip("the LD_PRELOAD interposer is Linux-only");
  auto server = test_support::ServerProcess::start(options);
  expect(server && server->wait_until_listening(10s) && server->wait_for_health(test_support::kHealthReady, 20s), "ready after one restart");
  if (!server) return;
  test_support::run_lifecycle(*server, std::string{test_support::kHarnessToken});
  server->stop(test_support::ServerProcess::Stop::term);
  expect(server->wait_exit(10s) == std::optional<int>{0}, "clean shutdown");
  expect(test_support::file_text(record).empty(), "no outbound call, DNS lookup, UDP or Unix socket: " + test_support::file_text(record));
}

void egress_table_scan_during_lifecycle() {
  const auto dir = test_support::make_temp_dir("scan");
  test_support::ServerOptions options;
  options.engine_args = {"--fail-first=1", "--counter-file=" + test_support::utf8_path(dir / "count"), "--tokens=10000", "--token-interval-ms=5"};
  auto server = test_support::ServerProcess::start(options);
  expect(server && server->wait_until_listening(10s), "listening");
  if (!server) return;
  test_support::SocketScan scan(server->pid(), server->port());
  expect(server->wait_for_health(test_support::kHealthReady, 20s), "ready after one restart");
  test_support::run_lifecycle(*server, std::string{test_support::kHarnessToken});
#if defined(_WIN32)
  server->stop(test_support::ServerProcess::Stop::ctrl_break);
#else
  server->stop(test_support::ServerProcess::Stop::term);
#endif
  (void)server->wait_exit(10s);
  std::string list;
  for (const auto& v : scan.stop()) list += v + "; ";
  expect(list.empty(), "only the listener and its accepted connections, no UDP, no Unix socket: " + list);
}

void egress_command_like_values_trigger_nothing() {
  const auto dir = test_support::make_temp_dir("cmd");
  const auto record = dir / "record.txt";
  const auto egress = dir / "egress.txt";
  test_support::ServerOptions options;
  options.engine_args = {"--record-file=" + test_support::utf8_path(record)};
  const bool armed = test_support::arm_egress_tripwire(options, egress);
  auto server = test_support::ServerProcess::start(options);
  expect(server && server->wait_until_listening(10s) && server->wait_for_health(test_support::kHealthReady, 15s), "ready");
  if (!server) return;
  const auto before = server->active_processes();
  const std::string a = "127.0.0.1:" + std::to_string(server->port());
  const std::string token{test_support::kHarnessToken};
  for (const std::string content : {"https://example.com/model.gguf", "mcp://x", "file:///etc/passwd", "data:text/plain,x", "HTTP://EXAMPLE.COM"}) {
    const auto r = raw_http::exchange("127.0.0.1", server->port(),
        raw_http::request("POST", "/v1/chat/completions", a, {raw_http::bearer(token), "Content-Type: application/json"},
                          R"({"model":"fake-model","messages":[{"role":"user","content":")" + content + R"("}]})"));
    expect(r && r->status == 400, "remote-looking content is refused: " + content);
  }
  const auto pull = raw_http::exchange("127.0.0.1", server->port(),
      raw_http::request("POST", "/v1/chat/completions", a, {raw_http::bearer(token), "Content-Type: application/json", "X-Strata-Update: now"},
                        R"({"model":"pull:qwen","messages":[{"role":"user","content":"Hi"}]})"));
  expect(pull && pull->status == 400, "a pull-like model value is refused");
  for (const std::string target : {"/api/pull", "/v1/pull", "/update", "/v1/models/pull:x"}) {
    const auto r = raw_http::exchange("127.0.0.1", server->port(), raw_http::request("GET", target, a, {raw_http::bearer(token)}));
    expect(r && r->status == 404, "no acquire/update route: " + target);
  }
  expect(test_support::file_text(record).find("GEN") == std::string::npos, "the engine received no GEN");
  expect(server->active_processes() == before, "no process started or ended");
  if (armed) expect(test_support::file_text(egress).empty(), "no outbound attempt (interposer)");
}
```

Register (ruling R-J: every case that starts the real server carries `LABELS ${SECURE_FRONTEND_WINDOWS_RUNTIME}`, empty on Linux; on Windows the two interposer cases are not registered — there is no interposer there):

```cmake
    if(WIN32)
        secure_frontend_add_cases(secure_frontend_egress_tests egress_command_like_values_trigger_nothing
            LABELS ${SECURE_FRONTEND_WINDOWS_RUNTIME} TIMEOUT 120)
        secure_frontend_add_cases(secure_frontend_egress_tests egress_table_scan_during_lifecycle
            LABELS ${SECURE_FRONTEND_WINDOWS_RUNTIME} TIMEOUT 120)
        target_link_libraries(secure_frontend_egress_tests PRIVATE iphlpapi)
    else()
        secure_frontend_add_cases(secure_frontend_egress_tests egress_interposer_self_test
            egress_interposer_records_nothing_during_lifecycle egress_table_scan_during_lifecycle
            egress_command_like_values_trigger_nothing LABELS ${SECURE_FRONTEND_WINDOWS_RUNTIME} TIMEOUT 120)
    endif()
```

- [ ] **Step 2: Run to see RED** — `cmake --preset linux-gcc && cmake --build --preset linux-gcc 2>&1 | tail -3` → FAIL (`STRATA_EGRESS_TRIPWIRE_PATH` undefined → `egress_interposer_self_test` skips; that is not GREEN: the case must run on Linux). Expected RED evidence: `ctest --preset linux-gcc -R egress_interposer_self_test` reports "Skipped" with "Linux-only" printed. Record it.

- [ ] **Step 3: Implement** — `tests/support/egress_tripwire.cpp` (Linux only; built as a shared library, never linked into a product target):

```cpp
// Test-only LD_PRELOAD egress tripwire (spec §6.2). Every outbound attempt is
// appended to $STRATA_EGRESS_RECORD and fails with EACCES.
#include <dlfcn.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace {
void record(const char* what) {
  const char* path = std::getenv("STRATA_EGRESS_RECORD");
  if (path == nullptr || *path == '\0') return;
  const int fd = ::open(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
  if (fd < 0) return;
  const std::size_t n = std::strlen(what);
  if (::write(fd, what, n) == static_cast<ssize_t>(n)) (void)::write(fd, "\n", 1);
  ::close(fd);
}
template <typename Fn>
Fn next_symbol(const char* name) { return reinterpret_cast<Fn>(dlsym(RTLD_NEXT, name)); }
}  // namespace

extern "C" {

int connect(int, const sockaddr*, socklen_t) {
  record("connect");
  errno = EACCES;
  return -1;
}

ssize_t sendto(int fd, const void* buffer, size_t length, int flags, const sockaddr* address, socklen_t address_length) {
  if (address != nullptr) { record("sendto"); errno = EACCES; return -1; }
  using Fn = ssize_t (*)(int, const void*, size_t, int, const sockaddr*, socklen_t);
  static const Fn real = next_symbol<Fn>("sendto");
  return real(fd, buffer, length, flags, address, address_length);
}

ssize_t sendmsg(int fd, const msghdr* message, int flags) {
  if (message != nullptr && message->msg_name != nullptr) { record("sendmsg"); errno = EACCES; return -1; }
  using Fn = ssize_t (*)(int, const msghdr*, int);
  static const Fn real = next_symbol<Fn>("sendmsg");
  return real(fd, message, flags);
}

int getaddrinfo(const char*, const char*, const addrinfo*, addrinfo** result) {
  record("getaddrinfo");
  if (result != nullptr) *result = nullptr;
  return EAI_FAIL;
}

hostent* gethostbyname(const char*) {
  record("gethostbyname");
  h_errno = NO_RECOVERY;
  return nullptr;
}

int socket(int domain, int type, int protocol) {
  if (domain == AF_UNIX) { record("socket_unix"); errno = EACCES; return -1; }
  if (domain == AF_NETLINK) { record("socket_netlink"); errno = EACCES; return -1; }
  if ((type & 0xf) == SOCK_DGRAM) { record("socket_dgram"); errno = EACCES; return -1; }
  using Fn = int (*)(int, int, int);
  static const Fn real = next_symbol<Fn>("socket");
  return real(domain, type, protocol);
}

}  // extern "C"
```

`tests/egress_probe_helper.cpp` (Linux only):

```cpp
// Calls the outbound functions the tripwire must record and deny.
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>

int main() {
  addrinfo* result = nullptr;
  const int rc = getaddrinfo("localhost", "80", nullptr, &result);
  std::printf("getaddrinfo=%s\n", rc == EAI_FAIL ? "denied" : "allowed");
  if (rc == 0) freeaddrinfo(result);
  const hostent* host = gethostbyname("localhost");
  std::printf("gethostbyname=%s\n", host == nullptr ? "denied" : "allowed");
  const int tcp = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(9);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  const int connected = tcp >= 0 ? connect(tcp, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) : -1;
  std::printf("connect=%s\n", connected != 0 && errno == EACCES ? "denied" : "allowed");
  if (tcp >= 0) close(tcp);
  const int local = socket(AF_UNIX, SOCK_STREAM, 0);
  std::printf("unix=%s\n", local < 0 && errno == EACCES ? "denied" : "allowed");
  const int udp = socket(AF_INET, SOCK_DGRAM, 0);
  std::printf("udp=%s\n", udp < 0 && errno == EACCES ? "denied" : "allowed");
  return 0;
}
```

CMake (inside `if(STRATA_BUILD_SECURE_FRONTEND_TESTS)`):

```cmake
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_library(strata_egress_tripwire SHARED tests/support/egress_tripwire.cpp)
        target_link_libraries(strata_egress_tripwire PRIVATE ${CMAKE_DL_LIBS})
        target_compile_features(strata_egress_tripwire PRIVATE cxx_std_20)
        secure_frontend_set_warnings(strata_egress_tripwire)
        add_executable(strata-egress-probe-helper tests/egress_probe_helper.cpp)
        target_compile_features(strata-egress-probe-helper PRIVATE cxx_std_20)
        secure_frontend_set_warnings(strata-egress-probe-helper)
    endif()
```

and in `function(secure_frontend_test_paths target)` (SP2), append:

```cmake
    if(TARGET strata_egress_tripwire)
        target_compile_definitions(${target} PRIVATE
            STRATA_EGRESS_TRIPWIRE_PATH="$<TARGET_FILE:strata_egress_tripwire>"
            STRATA_EGRESS_PROBE_PATH="$<TARGET_FILE:strata-egress-probe-helper>")
        add_dependencies(${target} strata_egress_tripwire strata-egress-probe-helper)
    endif()
```

(The function is defined before the tripwire target; `if(TARGET ...)` is evaluated at call time, so define the two targets **above** the first `secure_frontend_test_paths(...)` call in the tests block.) Link `secure_frontend_egress_tests` with `secure_frontend_socket_io`.

- [ ] **Step 4: Run to see GREEN**

```bash
cmake --preset linux-gcc && cmake --build --preset linux-gcc && ctest --preset linux-gcc -R egress_tests --output-on-failure
ctest --preset linux-gcc | tail -1
cmake --build --preset windows-llvm-mingw-cross && ctest --preset windows-llvm-mingw-cross -R egress_tests | tail -1
```

Expected: all Linux egress cases pass (none skipped); wine runs the two static cases; `egress_command_like_values_trigger_nothing` and `egress_table_scan_during_lifecycle` are "pending MSYS2".

- [ ] **Step 5: Handoff row and commit** — row `| 19 | PRIV-01, PRIV-03, DL-01 (frontend part), §6.2 runtime | LD_PRELOAD interposer (Linux), /proc and GetExtendedTcpTable scans, command-like values | egress_tests.egress_interposer_self_test (L), …egress_interposer_records_nothing_during_lifecycle (L), …egress_table_scan_during_lifecycle (L; W pending MSYS2), …egress_command_like_values_trigger_nothing | … | … |`.

```bash
git -C "$(git rev-parse --show-toplevel)" add secure_frontend docs/AGENT_HANDOFF.md
git -C "$(git rev-parse --show-toplevel)" -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "test: runtime egress tripwires - LD_PRELOAD interposer and connection-table scans over the server lifecycle (SP3 §6.2, PRIV-01/03, DL-01)" \
  -m "Tests: egress_tests.egress_interposer_self_test, egress_interposer_records_nothing_during_lifecycle, egress_table_scan_during_lifecycle (windows-runtime on Windows), egress_command_like_values_trigger_nothing. RED: <line>. wine: <line>." \
  -m "Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>" -m "Claude-Session: <session link>"
```

---

## Task 20: EXT-01/EXT-02 amendments and additions

**Files:**
- Modify: `tests/server_test.cpp` (SP2), `tests/api_server_test.cpp`, `CMakeLists.txt`

**Interfaces:**
- Consumes: `arm_egress_tripwire`, `SocketScan`, `file_text` (Task 19); Task 9's `ext01` body list; SP2's `ext02_*` cases (SP2 Task 27); `kRouteTable`.
- Produces: `ext01_extension_fields_spawn_nothing` and every `ext02_*` case run under the interposer (Linux) and the scan; `api_server_tests.ext01_upstream_control_routes_are_404_after_auth`, `api_server_tests.ext02_http_headers_cannot_enable_extensions`. (`ext01_extension_fields_rejected_on_stream_path` is M2.)

- [ ] **Step 1: Write the tests** — in `tests/server_test.cpp` include `support/egress_support.hpp` and, in `ext01_extension_fields_spawn_nothing` and in **each** case whose name starts with `ext02_`, wrap the existing body: right after the `ServerOptions options;` line add

```cpp
  const auto egress_record = test_support::make_temp_dir("egress") / "egress.txt";
  const bool interposed = test_support::arm_egress_tripwire(options, egress_record);
```

right after the server is ready add `test_support::SocketScan scan(server->pid(), server->port());`, and at the end of the case add

```cpp
  std::string violations;
  for (const auto& v : scan.stop()) violations += v + "; ";
  expect(violations.empty(), "scan: only the listener's sockets: " + violations);
  if (interposed) expect(test_support::file_text(egress_record).empty(), "interposer recorded nothing");
```

(For an `ext02_*` case that expects the server to refuse to start — e.g. an unknown `mcp_servers` config key — there is no running server: keep only the interposer lines, placed before `start`, and assert the record is empty after `wait_exit`.) On Windows give `secure_frontend_server_tests` the `iphlpapi` link and keep its existing labels.

Append to `tests/api_server_test.cpp` and register:

```cpp
void ext01_upstream_control_routes_are_404_after_auth() {
  auto server = start_server();
  if (!server) return;
  for (const std::string target : {"/v1/load", "/settings", "/config", "/slots", "/metrics", "/props", "/mcp", "/v1/completions",
                                   "/v1/messages", "/v1/responses", "/v1/embeddings", "/api/chat", "/api/generate", "/api/tags",
                                   "/api/pull", "/v1/tools", "/v1/mcp"}) {
    for (const std::string method : {"GET", "POST"}) {
      std::vector<std::string> lines{raw_http::bearer(token())};
      if (method == "POST") lines.push_back("Content-Type: application/json");
      const auto r = raw(*server, raw_http::request(method, target, authority(*server), lines, method == "POST" ? "{}" : ""));
      expect(r && r->status == 404 && r->body == secure_frontend::render_error_body(ApiError::not_found),
             method + " " + target + " is 404 after authentication");
    }
  }
}

void ext02_http_headers_cannot_enable_extensions() {
  const auto dir = test_support::make_temp_dir("ext02h");
  auto server = start_server({}, {"--record-file=" + test_support::utf8_path(dir / "record.txt")});
  if (!server) return;
  const auto before = server->active_processes();
  const std::vector<std::string> switches{"X-Strata-Enable-Tools: 1", "X-Strata-Mcp: on", "X-Strata-Extensions: all",
                                          "X-Stainless-Helper-Method: tools"};
  const auto r = raw(*server, chat_request(*server, chat_body(R"("max_tokens":2,"tools":[{"type":"function","function":{"name":"x"}}])"), switches));
  expect(r && r->status == 400 && r->body == secure_frontend::render_error_body(ApiError::extension_disabled, "tools"),
         "header switches change nothing: tools stay disabled");
  const auto plain = raw(*server, chat_request(*server, chat_body(), switches));
  expect(plain && plain->status == 200 && plain->body.find("tool") == std::string::npos, "an ordinary chat is unaffected");
  expect(count_of(record_of(dir / "record.txt"), "GEN ") == 1U, "only the ordinary chat reached the engine");
  expect(server->active_processes() == before, "no process started");
}
```

- [ ] **Step 2: Run** — `cmake --build --preset linux-gcc && ctest --preset linux-gcc -R '(server_tests\.ext0|api_server_tests\.ext0)' --output-on-failure`. Expected: PASS (acceptance; the rules exist since Tasks 6b, 7, 9). If the interposer or the scan records anything during an SP2 `ext*` case, stop and report: it is a real finding.

- [ ] **Step 3: Full suite and cross build**

```bash
ctest --preset linux-gcc | tail -1
cmake --build --preset windows-llvm-mingw-cross && ctest --preset windows-llvm-mingw-cross -R 'ext0' | tail -1
```

- [ ] **Step 4: Handoff row and commit** — row `| 20 | EXT-01, EXT-02 | SP2 ext01/ext02 under the interposer and scan; control routes 404; header switches inert | server_tests.ext01_extension_fields_spawn_nothing (amended), server_tests.ext02_* (under tripwires), api_server_tests.ext01_upstream_control_routes_are_404_after_auth, …ext02_http_headers_cannot_enable_extensions | … | … |`.

```bash
git -C "$(git rev-parse --show-toplevel)" add secure_frontend docs/AGENT_HANDOFF.md
git -C "$(git rev-parse --show-toplevel)" -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "test: EXT-01/02 under the egress tripwires; upstream control routes 404; HTTP header switches inert (SP3 §6.4)" \
  -m "Tests: server_tests.ext01_extension_fields_spawn_nothing and ext02_* now under interposer + scan; api_server_tests.ext01_upstream_control_routes_are_404_after_auth, ext02_http_headers_cannot_enable_extensions. <passed on first run | defects>. wine: <line>." \
  -m "Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>" -m "Claude-Session: <session link>"
```

---

## Task 21: Request logging without telemetry (§8) and the fixed-reason sweep

**Files:**
- Create: `include/secure_frontend/request_log.hpp`, `src/request_log.cpp`
- Modify: `src/secure_server_main.cpp`, `tests/concurrency_test.cpp`, `tests/api_server_test.cpp`, `CMakeLists.txt`

**Interfaces:**
- Consumes: `ConnectionPool` observer `FinishedConnection` (Task 12), `ConnectionOutcome`, `RouteId`, `AddressScope`; `FrontendConfig::logging.requests` (Task 4); `api_constants::kLogBurst`, `kLogRatePerSecond`.
- Produces:

```cpp
struct RequestLogRecord final { RouteId route; int status; std::string_view code; std::chrono::milliseconds elapsed;
  std::size_t bytes_out; AddressScope peer; };
[[nodiscard]] std::string format_request_line(std::uint64_t sequence, const RequestLogRecord& record);
[[nodiscard]] RequestLogRecord log_record_for(const FinishedConnection& finished);   // maps outcomes to §8 log-only codes
class RequestLogger final {
 public:
  using Clock = std::chrono::steady_clock;
  using Sink = std::function<void(std::string_view line)>;
  RequestLogger(bool enabled, Sink sink, std::function<Clock::time_point()> now = &Clock::now);
  void record(const RequestLogRecord& record);   // thread-safe; never blocks beyond its mutex
};
[[nodiscard]] RequestLogger::Sink stderr_sink();   // fixed-size buffer, one fwrite per line, failures dropped
```

Line format (spec §8): `req <seq> <route> <status> <code|-> <ms> <bytes_out> <peer>`; `<route>` ∈ `health`, `models`, `model`, `chat`, `other`; `<code>` the §7.2 code or a log-only outcome `generation_timeout`, `client_gone`, `write_stall`, `evicted`; `<peer>` `loopback` or `remote`. Token bucket: `kLogBurst` (100) lines, refilled at `kLogRatePerSecond` (20/s); drops are counted and reported as `log: <n> request lines suppressed`, at most once per second, when the bucket again has a token. Always logged (not rate-limited, not per request): the startup summary `strata: listening <authority>, workers <n>, queued generations <q>` written once after bind.

- [ ] **Step 1: Write the failing tests** — `tests/concurrency_test.cpp` (register `request_log_format_and_token_bucket`):

```cpp
void request_log_format_and_token_bucket() {
  using secure_frontend::RequestLogger;
  const secure_frontend::RequestLogRecord rec{secure_frontend::RouteId::chat, 200, "", std::chrono::milliseconds(12), 345U,
                                              secure_frontend::AddressScope::loopback};
  expect(secure_frontend::format_request_line(7, rec) == "req 7 chat 200 - 12 345 loopback", "line format");
  const secure_frontend::RequestLogRecord gone{secure_frontend::RouteId::health, 0, "client_gone", std::chrono::milliseconds(1), 0U,
                                               secure_frontend::AddressScope::remote};
  expect(secure_frontend::format_request_line(8, gone) == "req 8 health 0 client_gone 1 0 remote", "log-only code");
  auto now = RequestLogger::Clock::time_point{} + std::chrono::seconds(10);
  std::vector<std::string> lines;
  RequestLogger logger(true, [&lines](std::string_view line) { lines.emplace_back(line); }, [&now] { return now; });
  for (int i = 0; i < 300; ++i) logger.record(rec);
  expect(lines.size() == 100U, "a burst of 100 lines");
  now += std::chrono::seconds(1);
  for (int i = 0; i < 300; ++i) logger.record(rec);
  const auto suppressed = std::count_if(lines.begin(), lines.end(), [](const std::string& l) { return l.rfind("log: ", 0) == 0; });
  expect(suppressed == 1 && lines[100] == "log: 200 request lines suppressed", "one suppression line with the drop count");
  expect(lines.size() == 100U + 1U + 20U, "20 more lines after one second of refill");
  RequestLogger off(false, [&lines](std::string_view line) { lines.emplace_back(line); }, [&now] { return now; });
  const auto before = lines.size();
  off.record(rec);
  expect(lines.size() == before, "logging.requests=false writes nothing per request");
}
```

`tests/api_server_test.cpp` (register both):

```cpp
void log_line_format_and_rate_limit() {
  auto server = start_server({{"logging.requests", "true"}});
  if (!server) return;
  const auto started = std::chrono::steady_clock::now();
  std::vector<std::thread> clients;
  for (int t = 0; t < 6; ++t) {
    clients.emplace_back([&] { for (int i = 0; i < 50; ++i) (void)raw(*server, raw_http::request("GET", "/health", authority(*server), {raw_http::bearer(token())})); });
  }
  for (auto& c : clients) c.join();
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  std::this_thread::sleep_for(1100ms);
  (void)raw(*server, raw_http::request("GET", "/health", authority(*server), {raw_http::bearer(token())}));   // lets a suppression line out
  const std::string log = server->stderr_text();
  std::istringstream in(log);
  std::size_t requests = 0, suppressions = 0;
  for (std::string line; std::getline(in, line);) {
    if (line.rfind("req ", 0) == 0) {
      ++requests;
      std::istringstream f(line);
      std::string req, seq, route, status, code, ms, bytes, peer, extra;
      f >> req >> seq >> route >> status >> code >> ms >> bytes >> peer;
      expect(route == "health" && status == "200" && code == "-" && peer == "loopback" && !(f >> extra), "fixed fields only: " + line);
    }
    if (line.rfind("log: ", 0) == 0 && line.find(" request lines suppressed") != std::string::npos) ++suppressions;
  }
  const auto bound = 100.0 + 20.0 * (seconds + 1.2) + 1.0;
  expect(static_cast<double>(requests) <= bound, "rate-limited: " + std::to_string(requests) + " <= " + std::to_string(bound));
  expect(requests >= 100U && suppressions >= 1U && static_cast<double>(suppressions) <= seconds + 2.0, "burst then counted drops");
  expect(log.find("strata: listening 127.0.0.1:") != std::string::npos && log.find("workers 8, queued generations 4") != std::string::npos,
         "startup summary");
}

void err_no_token_prompt_or_path_in_any_body_or_log() {
  const std::string marker = "ZZPROMPTMARKERZZ";
  auto server = start_server({{"logging.requests", "true"}});
  if (!server) return;
  const std::string a = authority(*server);
  const std::string root = test_support::utf8_path(server->root());
  std::vector<std::string> bodies;
  const auto send = [&](const std::string& request) {
    if (auto c = raw_http::Connection::open("127.0.0.1", server->port()); c && c->send(request)) bodies.push_back(c->read_until_close(10s));
  };
  send(chat_request(*server, chat_body(R"("max_tokens":2)", marker)));
  send(chat_request(*server, chat_body(R"("max_tokens":2,"tools":[")" + marker + "\"]")));
  send(chat_request(*server, "{\"" + marker + "\":1}"));
  send(chat_request(*server, R"({"model":")" + marker + R"(","messages":[{"role":"user","content":"Hi"}]})"));
  send(raw_http::request("GET", "/" + marker, a, {raw_http::bearer(token())}));
  send(raw_http::request("GET", "/health", a, {"Authorization: Bearer " + marker + "-0123456789abcdefghijk"}));
  send(raw_http::request("GET", "/health", a, {raw_http::bearer(token()), "X-Marker: " + marker}));
  send(raw_http::request("GET", "/health", marker + ":1", {raw_http::bearer(token())}));
  send(raw_http::request("GET", "/health", a, {raw_http::bearer(token()), "Origin: http://" + marker}));
  send(chat_request(*server, chat_body(R"("max_tokens":2,"temperature":9.5)", marker)));
  std::this_thread::sleep_for(200ms);
  const std::string log = server->stderr_text();
  for (std::size_t i = 0; i < bodies.size(); ++i) {
    // The 200 chat legitimately echoes generated text, never the prompt; the fake emits token 7, not the marker.
    expect(bodies[i].find(marker) == std::string::npos, "response " + std::to_string(i) + " never reflects the marker");
    expect(bodies[i].find(token()) == std::string::npos, "response " + std::to_string(i) + " never contains the token");
    expect(bodies[i].find(root) == std::string::npos, "response " + std::to_string(i) + " never contains a configured path");
  }
  expect(log.find(marker) == std::string::npos && log.find(token()) == std::string::npos && log.find(root) == std::string::npos,
         "stderr (with logging.requests=true) has no marker, token or path");
}
```

- [ ] **Step 2: Run to see RED** — `cmake --build --preset linux-gcc 2>&1 | grep -m1 error` → FAIL `secure_frontend/request_log.hpp: No such file or directory`.

- [ ] **Step 3: Implement** — `include/secure_frontend/request_log.hpp` with the declarations from Interfaces (includes `connection_pool.hpp`, `request_router.hpp`, `<functional>`, `<mutex>`). `src/request_log.cpp`:

```cpp
#include "secure_frontend/request_log.hpp"

#include "secure_frontend/api_constants.hpp"

#include <algorithm>
#include <cstdio>

namespace secure_frontend {
namespace {
std::string_view route_name(const RouteId route) {
  switch (route) {
    case RouteId::health: return "health";
    case RouteId::models: return "models";
    case RouteId::model: return "model";
    case RouteId::chat: return "chat";
    case RouteId::other: return "other";
  }
  return "other";
}
}  // namespace

std::string format_request_line(const std::uint64_t sequence, const RequestLogRecord& r) {
  std::string line = "req " + std::to_string(sequence) + " ";
  line += route_name(r.route);
  line += " " + std::to_string(r.status) + " ";
  line += r.code.empty() ? std::string_view{"-"} : r.code;
  line += " " + std::to_string(r.elapsed.count()) + " " + std::to_string(r.bytes_out) + " ";
  line += r.peer == AddressScope::loopback ? "loopback" : "remote";
  return line;
}

RequestLogRecord log_record_for(const FinishedConnection& f) {
  std::string_view code = f.result.code;
  if (f.result.outcome == ConnectionOutcome::evicted) code = "evicted";
  else if (f.result.outcome == ConnectionOutcome::client_gone && f.result.status != 0) code = "write_stall";
  else if (f.result.outcome == ConnectionOutcome::client_gone && code.empty()) code = "client_gone";
  return {f.result.route, f.result.status, code, f.elapsed, f.result.bytes_out, f.peer};
}

RequestLogger::RequestLogger(const bool enabled, Sink sink, std::function<Clock::time_point()> now)
    : enabled_(enabled), sink_(std::move(sink)), now_(std::move(now)), tokens_(static_cast<double>(api_constants::kLogBurst)),
      last_refill_(now_()), last_report_(now_()) {}

void RequestLogger::record(const RequestLogRecord& record) {
  if (!enabled_ || !sink_) return;
  std::lock_guard lock(mutex_);
  const Clock::time_point now = now_();
  const double elapsed = std::chrono::duration<double>(now - last_refill_).count();
  tokens_ = std::min(static_cast<double>(api_constants::kLogBurst), tokens_ + elapsed * static_cast<double>(api_constants::kLogRatePerSecond));
  last_refill_ = now;
  if (tokens_ < 1.0) { ++dropped_; return; }
  if (dropped_ > 0U && now - last_report_ >= std::chrono::seconds(1)) {
    sink_("log: " + std::to_string(dropped_) + " request lines suppressed");
    dropped_ = 0U;
    last_report_ = now;
  }
  tokens_ -= 1.0;
  sink_(format_request_line(++sequence_, record));
}

RequestLogger::Sink stderr_sink() {
  return [](const std::string_view line) {
    char buffer[256];
    const std::size_t n = std::min(line.size(), sizeof(buffer) - 1U);
    std::copy_n(line.data(), n, buffer);
    buffer[n] = '\n';
    (void)std::fwrite(buffer, 1, n + 1U, stderr);   // a failed write is dropped, never retried
    (void)std::fflush(stderr);
  };
}

}  // namespace secure_frontend
```

(private members in the header: `bool enabled_; Sink sink_; std::function<Clock::time_point()> now_; std::mutex mutex_; double tokens_; Clock::time_point last_refill_; Clock::time_point last_report_; std::uint64_t sequence_{0}; std::uint64_t dropped_{0};`; the test's suppression-line expectation holds because the first report is due one second after construction.)

`src/secure_server_main.cpp`: construct `secure_frontend::RequestLogger request_log(config.logging.requests, secure_frontend::stderr_sink());` before the pool; pass the observer `[&request_log](const secure_frontend::FinishedConnection& f) { request_log.record(secure_frontend::log_record_for(f)); }` to `ConnectionPool`; after a successful bind and before `supervisor.start()` print once to stderr

```cpp
  std::cerr << "strata: listening " << authority << ", workers " << config.limits.concurrency.max_concurrent_requests
            << ", queued generations " << config.limits.concurrency.max_queued_generations << '\n' << std::flush;
```

(`authority` is the bound authority string `main` already prints on stdout; these are the operator's own settings, as `--check` prints them.) CMake: `secure_frontend_add_library(secure_frontend_request_log SOURCES src/request_log.cpp LINKS secure_frontend_connection_pool)`; link `strata-secure-server` and `secure_frontend_concurrency_tests` with it.

- [ ] **Step 4: Run to see GREEN**

```bash
cmake --build --preset linux-gcc && ctest --preset linux-gcc -R 'concurrency_tests|api_server_tests\.(log_|err_no_)' --output-on-failure
ctest --preset linux-gcc | tail -1
cmake --build --preset windows-llvm-mingw-cross && ctest --preset windows-llvm-mingw-cross -R 'concurrency_tests|api_server_tests' | tail -1
```

- [ ] **Step 5: Handoff row and commit** — row `| 21 | §8, PRIV-02 (frontend part), §7 fixed reasons | per-request log lines (off by default), token bucket, startup summary | concurrency_tests.request_log_format_and_token_bucket; api_server_tests.log_line_format_and_rate_limit, …err_no_token_prompt_or_path_in_any_body_or_log | … | … |`.

```bash
git -C "$(git rev-parse --show-toplevel)" add secure_frontend docs/AGENT_HANDOFF.md
git -C "$(git rev-parse --show-toplevel)" -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "log: optional fixed-field request lines with a token bucket; no token, prompt or path anywhere (SP3 §8, §7)" \
  -m "Tests: concurrency_tests.request_log_format_and_token_bucket; api_server_tests.log_line_format_and_rate_limit, err_no_token_prompt_or_path_in_any_body_or_log. RED: <line>. wine: <line>." \
  -m "Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>" -m "Claude-Session: <session link>"
```

---

## Task 22: Windows parity cases and the non-blocking ThreadSanitizer job

**Files:**
- Modify: `tests/api_server_test.cpp`, `CMakeLists.txt`, `.github/workflows/secure-frontend.yml` (controller note 11)

**Interfaces:**
- Consumes: Tasks 8b, 12, 16 (refusal path, eviction, peer probe); harness; `start_recorded`, `gen_lines`.
- Produces: the W rows `win_refusal_is_read_not_reset`, `win_eviction_wakes_worker_within_one_slice`, `win_disconnect_detected_by_peek` (registered only on Windows, `windows-runtime`), and the CI job `linux-clang-tsan` (`continue-on-error: true`).

- [ ] **Step 1: Write the tests** — append (compiled everywhere, registered on Windows only):

```cpp
void win_refusal_is_read_not_reset() {
  auto server = start_server({{"limits.concurrency.max_concurrent_requests", "1"}});
  if (!server) return;
  auto holder = raw_http::Connection::open("127.0.0.1", server->port());
  expect(holder && holder->send("POST /v1/chat/completions HTTP/1.1\r\nHost: " + authority(*server) + "\r\n" + raw_http::bearer(token()) +
                                "\r\nContent-Type: application/json\r\nContent-Length: 100\r\n\r\n{"), "the only worker is held in P3");
  std::this_thread::sleep_for(200ms);
  int complete = 0;
  std::vector<std::thread> flood;
  std::mutex m;
  for (int i = 0; i < 30; ++i) {
    flood.emplace_back([&] {
      auto c = raw_http::Connection::open("127.0.0.1", server->port());
      if (!c || !c->send(raw_http::request("GET", "/health", authority(*server), {raw_http::bearer(token())}))) return;
      const auto r = raw_http::parse_response(c->read_until_close(5s));
      std::lock_guard g(m);
      if (r && r->status == 503 && r->body == secure_frontend::render_error_body(ApiError::server_busy)) ++complete;
    });
  }
  for (auto& t : flood) t.join();
  expect(complete == 30, "every refused client read the whole 503 (lingering close, no RST): " + std::to_string(complete));
}

void win_eviction_wakes_worker_within_one_slice() {
  auto server = start_server({{"limits.concurrency.max_concurrent_requests", "1"}, {"limits.http.header_deadline_ms", "30000"}});
  if (!server) return;
  auto slow = raw_http::Connection::open("127.0.0.1", server->port());
  expect(slow && slow->send("GET /health HTTP/1.1\r\nHost: "), "P1 holder");
  std::this_thread::sleep_for(200ms);
  const auto started = std::chrono::steady_clock::now();
  const auto r = raw(*server, raw_http::request("GET", "/health", authority(*server), {raw_http::bearer(token())}), 5s);
  const auto elapsed = std::chrono::steady_clock::now() - started;
  expect(r && r->status == 200, "the new client is served");
  expect(elapsed < 250ms, "the evicted worker woke within one 20 ms slice plus slack (" +
                          std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()) + " ms)");
}

void win_disconnect_detected_by_peek() {
  auto r = start_recorded("wpeek", {}, {"--tokens=10000", "--token-interval-ms=5"});
  if (!r.server) return;
  {
    auto c = raw_http::Connection::open("127.0.0.1", r.server->port());
    expect(c && c->send(chat_request(*r.server, chat_body(""))), "long chat");
    std::this_thread::sleep_for(300ms);
  }
  bool stopped = false;
  for (int i = 0; i < 50 && !stopped; ++i) {
    stopped = record_of(r.record).find("\nSTOP\n") != std::string::npos;
    if (!stopped) std::this_thread::sleep_for(20ms);
  }
  expect(stopped, "recv(MSG_PEEK) saw the close on Winsock and STOP was sent within 1 s");
}
```

CMake:

```cmake
    if(WIN32)
        secure_frontend_add_cases(secure_frontend_api_server_tests win_refusal_is_read_not_reset
            win_eviction_wakes_worker_within_one_slice win_disconnect_detected_by_peek
            LABELS ${SECURE_FRONTEND_WINDOWS_RUNTIME} TIMEOUT 120)
    endif()
```

(The three functions stay in the file's `run_cases` list on every platform, so a developer can run them manually on Linux: `out/build/linux-gcc/secure_frontend_api_server_tests --case win_refusal_is_read_not_reset`.)

- [ ] **Step 2: Run** — Linux manual run of the three cases (they must pass on Linux too; they exercise the same code):

```bash
cmake --build --preset linux-gcc
for c in win_refusal_is_read_not_reset win_eviction_wakes_worker_within_one_slice win_disconnect_detected_by_peek; do
  SECURE_FRONTEND_TEST_TMP=$PWD/out/build/linux-gcc/test-tmp out/build/linux-gcc/secure_frontend_api_server_tests --case $c; echo "$c exit=$?"; done
cmake --build --preset windows-llvm-mingw-cross && ctest --preset windows-llvm-mingw-cross | tail -1
WINEPREFIX=$PWD/out/wineprefix WINEDLLOVERRIDES=mscoree,mshtml= ctest --test-dir out/build/windows-llvm-mingw-cross -N -L windows-runtime | grep -c win_
```

Expected: three `exit=0` on Linux; the cross build succeeds; `-N -L windows-runtime` lists the three cases (they are excluded from the wine run and authoritative in MSYS2 — record "pending MSYS2").

- [ ] **Step 3: ThreadSanitizer job (controller note 11; skip this step if not approved)** — in `.github/workflows/secure-frontend.yml` add a job after `linux`:

```yaml
  linux-clang-tsan:
    name: linux-clang-tsan (non-blocking)
    runs-on: ubuntu-24.04  # pinned like the other Linux jobs
    timeout-minutes: 60
    continue-on-error: true
    steps:
      - uses: actions/checkout@v4
      - name: ThreadSanitizer needs fewer mmap random bits on 6.x kernels
        run: sudo sysctl vm.mmap_rnd_bits=28
      - name: bootstrap .venv-dev (cmake, ninja)
        run: scripts/dev/bootstrap-linux.sh
      - run: echo "$GITHUB_WORKSPACE/.venv-dev/bin" >> "$GITHUB_PATH"
      - run: cmake --preset linux-clang -DCMAKE_CXX_FLAGS="-fsanitize=thread -g" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread" -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=thread"
        working-directory: secure_frontend
      - run: cmake --build --preset linux-clang --parallel
        working-directory: secure_frontend
      - run: ctest --preset linux-clang -R '(concurrency_tests|api_server_tests|http_socket_server_tests)' -E 'egress_interposer'
        working-directory: secure_frontend
```

(The interposer cases are excluded: a TSan runtime and an `LD_PRELOAD` library do not mix reliably.) Update the header comment's "Non-blocking:" list to `windows-msvc (informational), linux-clang-tsan (data races in the SP3 pool, gate, port and logger)`.

- [ ] **Step 4: Full suite**

```bash
ctest --preset linux-gcc | tail -1
```

- [ ] **Step 5: Handoff row and commit** — row `| 22 | §10 Windows parity, R2 | refusal read not reset, eviction wake, peek disconnect on Winsock; non-blocking TSan job | api_server_tests.win_refusal_is_read_not_reset, …win_eviction_wakes_worker_within_one_slice, …win_disconnect_detected_by_peek (W, pending MSYS2); CI linux-clang-tsan (non-blocking) | … | … |`.

```bash
git -C "$(git rev-parse --show-toplevel)" add secure_frontend .github/workflows/secure-frontend.yml docs/AGENT_HANDOFF.md
git -C "$(git rev-parse --show-toplevel)" -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "test: Winsock refusal, eviction and disconnect cases; non-blocking ThreadSanitizer job (SP3 §10, R2)" \
  -m "Tests: api_server_tests.win_refusal_is_read_not_reset, win_eviction_wakes_worker_within_one_slice, win_disconnect_detected_by_peek (windows-runtime; pass on Linux by hand; authoritative in windows-msys2-clang after the owner's push). CI: linux-clang-tsan, continue-on-error. wine: <line>." \
  -m "Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>" -m "Claude-Session: <session link>"
```

---

## Task 23: Documentation truth pass and M1 handoff

**Files:**
- Modify: `docs/AGENT_HANDOFF.md`, `docs/security/threat-model.md`, `docs/security/engine-protocol-limits.md`, `secure_frontend/README.md`, `docs/secure-frontend-ci.md`

**Interfaces:**
- Consumes: every task's handoff row and commit body.
- Produces: accurate docs; M1 status, counts and the deferred list for M2.

- [ ] **Step 1: Final verification** (record every output line quoted below in the handoff):

```bash
cd /opt/projects/ai/.worktrees/sp3/secure_frontend && export PATH="$(git rev-parse --show-toplevel)/.venv-dev/bin:$PATH"
cmake --preset linux-gcc && cmake --build --preset linux-gcc && ctest --preset linux-gcc | tail -3
ctest --preset linux-gcc -N | grep -cE '\.(net0[123]|web0[1-6]|dos0[12]|ext0[12]|egress_|err_|cfg01_|win_)'
ctest --preset linux-gcc -N | tail -1
cmake --preset windows-llvm-mingw-cross && cmake --build --preset windows-llvm-mingw-cross | tail -1
ctest --preset windows-llvm-mingw-cross | tail -3
grep -rn "kMinimumSendBudget\|chatcmpl-local\|text/plain; charset" src include || echo "none left"
grep -rnE "std::chrono::(milli)?seconds\([0-9]{3,}\)" src | grep -v api_constants.hpp | grep -v config_defaults.hpp || echo "no stray tuning numbers"
git -C "$(git rev-parse --show-toplevel)" status --short
```

Expected: `100% tests passed` (skips only `web01_ipv6_bound_host_rules`/`net01_ipv6_loopback_listener_only` where the host has no IPv6 loopback, plus SP1/SP2's listed skips); every §9.3 M1 case listed individually by `ctest -N`; the cross build succeeds with `-Werror`; wine `0 tests failed` with `windows-runtime` excluded; the two greps print `none left` / `no stray tuning numbers`; `git status` shows only this task's doc files.

- [ ] **Step 2: `docs/AGENT_HANDOFF.md`** — complete the "SP3 M1 deliverables" table (one row per task 1–22, already filled in-task; fill any `<line>` placeholders left by copying the real result lines from `git log --format=%B main..HEAD`); add:
  - **M1 status:** "SP3 M1 done on branch `sp3-api-streaming` (local, not pushed): accept thread + worker pool (default 8), FIFO generation gate (default queue 4, 300 s), header-first authentication, §4.2 parser, Host/Origin/CORS, §7.2 JSON errors, decisions B1/B2/B6/B10/B11/B12 implemented; B3/B4/B5 are M2." with the linux-gcc total, the wine total, and the list of `windows-runtime` cases "pending windows-msys2-clang".
  - **SP1 addendum A1–A3** rows (already added) plus A4: "SP1 spec §4 internal-constants list: the 1000 ms minimum send budget is deleted (replaced by `limits.http.write_stall_ms` and `limits.http.response_write_deadline_ms`); the 100 ms / 256 KiB half-close drain moved to `api_constants.hpp`."
  - **SP2 second-run follow-ups SP3 still depends on:** D-1 (lease-release notification; documented thread-safety of `acquire`/`snapshot`/`report_fatal` — SP3 serializes its own calls through `SupervisorPort`), D-2 (a drain failure after a client-requested `STOP` should not count toward the circuit; risk R4).
  - **Deferred to M2 (controller note 7):** SSE/chunked streaming, `next_event`, `Utf8StreamDecoder` (and the non-streaming decode via it), decision B4 (timeout → partial text with `length`; M1 answers `503 engine_error`), B3 (U+FFFD), B5 (chunked framing), keepalive, `stream_options`, `secure_frontend_api_client`, `strata-api-probe`, `docs/validation/sp3-api-kit.md`, the streaming DOS-02/WEB-04/EXT-01 cases, fake modes `--progress-records`, `--crash-mid-gen`, `--err-mid-gen` and the UTF-8 tokenizer ids.
  - **Known small gaps:** remove the SP0 bullets fixed here (`send_all` and `EINTR`, the S6 port range, the transient-accept backoff); add "the per-request log and every error are fixed literals; `--check` still prints the operator's own configured values, as SP1 designed".
  - **Current security behavior:** replace the serial-listener description with the pool/gate/early-auth description and the default numbers.
- [ ] **Step 3: `docs/security/threat-model.md`** (spec §13.2 item 6) — under "Browser and HTTP boundary" add: authentication happens when the header is complete, before any body byte; unauthenticated clients can hold a worker only in the header phase (`limits.http.header_deadline_ms`, oldest-first eviction); bodies are read only from token holders. Under "Out of scope and residual risks" add:
  - **Decision 12 residual (accepted, B12):** loopback peers are exempt from the per-address cap; another local user's connection flood can cause eviction churn that delays a local client's header by up to its own send time, and an authenticated local client can occupy every worker. A token holder is trusted with the API's capacity.
  - **R4:** a client that disconnects during a long real prefill may make `STOP` miss `limits.engine.stop_drain_timeout_s`; SP2 then kills the engine and counts a failure, so three such disconnects within 300 s can open the circuit. Mitigation: SP2 follow-up D-2; GPU-box measurement (M2 kit); the operator may raise the drain timeout.
  - **R5:** SDK retries amplify engine errors; `engine_error` carries `x-should-retry: false`, clients that ignore it remain a residual risk.
  - **R8 egress checks are tripwires, not proofs:** the import check misses `dlopen`/`GetProcAddress` and raw syscalls; the `LD_PRELOAD` interposer misses raw `syscall(2)` and static binaries (which the import check then fails on); the 50 ms table scan misses sockets that live less than 50 ms, and on Windows it is the only runtime check. Whole-machine monitoring is the SP7 validation-kit part of `PRIV-01`.
- [ ] **Step 4: `docs/security/engine-protocol-limits.md`** — append a section "SP3 M1": one generation at a time is now a FIFO queue (`limits.concurrency.max_queued_generations`, `queue_timeout_ms`); `GEN <max_new> [temperature=F] [top_p=F] [seed=N] <ids>` with the bounds of decision B6, rendered by the frontend from parsed numbers; the context rule `n + max_tokens + 8 ≤ ctx` is applied in the frontend before a queue place and again against the lease's READY; `max_tokens` is clamped (B2) and defaults to auto (B1); streaming lands in M2.
- [ ] **Step 5: `secure_frontend/README.md`** — routes (`GET /health`, `GET /v1/models`, `GET /v1/models/<id>`, `POST /v1/chat/completions` non-streaming in M1), the §7.2 error table (status, `code`, message), the security headers, the default limits, and under the generated key table a "Constants" note: "Internal constants live in `include/secure_frontend/api_constants.hpp` (SP3) and `config_defaults.hpp` (SP1). SP3 removed SP1's 1000 ms minimum send budget (SP1 addendum A4)." `docs/secure-frontend-ci.md`: add the `linux-clang-tsan` job (non-blocking) if Task 22 Step 3 was approved, and the new test binaries.
- [ ] **Step 6: Commit**

```bash
git -C "$(git rev-parse --show-toplevel)" add docs secure_frontend/README.md
git -C "$(git rev-parse --show-toplevel)" -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "docs: SP3 M1 handoff, threat-model residuals (B12, R4, R5, R8), protocol limits, README routes and errors" \
  -m "Final verification: linux-gcc <line>; wine <line>; windows-runtime cases pending windows-msys2-clang: <list>. Deferred to M2 listed in the handoff." \
  -m "Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>" -m "Claude-Session: <session link>"
```

---

## Self-review (done while writing)

**Spec coverage.** §2.1 threads → Task 12; §2.2 phases/deadlines → Tasks 8a, 8b, 9; early auth → Tasks 6b, 8b, 9; §2.3 admission/eviction/per-remote cap → Tasks 11, 12; §2.4 lingering close → Task 11; §2.5 port/gate → Tasks 13, 16; §2.6 cancellation (non-stream) → Tasks 8b, 15, 16, 17; §2.7 complete-response write bounds → Tasks 8a, 8b (streaming backpressure: M2); §3.11 streaming seam types and dispatch site → Tasks 6a, 8b (producers: M2); §2.8 shutdown → Tasks 12, 15, 16, 17; §3.1 schema → Task 7; §3.5 response → Tasks 15, 16; §3.6 routes → Tasks 6a, 6b; §3.7 `/health` unchanged → Task 6b; §3.8 Retry-After → Tasks 1, 9, 16; §3.9 one writer → Task 1; §4.1–§4.7 → Tasks 2, 3, 6a, 6b, 8b (bind rules unchanged, NET-01 re-proved in Task 12); §5.1 schema rows → Tasks 4, 5 (+A3 in Task 7); §5.2 limits table → Tasks 2, 4, 5, 8, 11, 13, 15, 21; §5.3 constants → Task 1; §5.4 token counting → Tasks 5, 16; §6.1–§6.4 → Tasks 18, 19, 20; §7 → Tasks 1, 6b, 9, 21; §8 → Task 21; §9.3 M1 rows → Tasks 2–22 by name; pre-flight rulings R-A, R-D, R-G … R-K applied (controller note 16); §9.4 changed tests → Tasks 5, 6a, 6b, 7, 8b, 9; §10 Windows → Tasks 8a, 8b, 12, 19, 22; §13.2 docs → Task 23. M2-only rows (SSE, UTF-8, finish mapping beyond stop/length, keepalive, client, probe, kit) are listed as deferred in Task 23.

**Placeholder scan.** `<line>`, `<N₀>`, `<list>`, `<names>` occur only inside commit-message and handoff templates and mean "paste the observed result"; no code step contains one.

**Type consistency.** `RouteDecision`, `RequestContext`, `RequestRouter::handle(head, body, decision, context)` are used with the same four parameters in Tasks 6a, 6b, 8b, 16, and `handle` returns `RouteResult` everywhere; `kNoResponseStatus` (Task 6a) is produced by `ChatRoute` (Task 16) and consumed by `deliver_route_result` (Task 8b); `HttpResponse::log_code` (Task 1) feeds `ConnectionResult::code` (Task 8b) and the logger (Task 21); `AdmissionState::admit/header_complete/finished` carry `(worker, connection)` in Tasks 11 and 12; `GenerationGate::admit(port, deadline, client_gone)` and `release()` match between Tasks 13 and 16; `resolve_max_tokens(requested, prompt, context, max_completion, default)` has the same order in Tasks 5 and 16; `run_generation(prompt_ids, settings, control, tokenizer)` matches Tasks 15 and 16.

**Review Focus.** Each of the five lines has its pinning test in its owning task (Tasks 3, 8b, 8b, 6b, 7).
