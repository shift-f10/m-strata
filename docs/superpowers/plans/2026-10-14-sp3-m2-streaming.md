# SP3 M2 — Streaming Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `POST /v1/chat/completions` with `"stream": true` returns OpenAI-compatible Server-Sent Events in an HTTP/1.1 chunked body — role delta, content deltas decoded incrementally from the engine's `T` records, a mapped `finish_reason`, optional usage, `data: [DONE]`, the terminal chunk — with cancellation on disconnect and write stall, a slow reader that never slows the engine, keepalive comments, in-stream error events, the loopback client library (`LoopbackAddress`, complete and streamed exchanges), the `strata-api-probe` kit tool, and every streaming `sse_*`/`utf8_*`/`DOS-02`/finish/client/EXT case of spec §9.3. The non-streaming path moves onto the same generation loop, so decisions B3 and B4 hold there too.

**Architecture:** Pure, separately tested layers stacked bottom-up: `TokenByteDecoder` (one token id → raw bytes) and `Utf8StreamDecoder` (held tail, U+FFFD); `EngineProtocolClient::next_event` (one record per call; `read_generation` rebuilt on it); `drive_generation` (the one generation loop: 20 ms slices, generation timeout → `STOP` → `length`, byte cap, cancel, shutdown drain bounded by `kShutdownDrainBudget`), on which M1's member `ChatExecutionService::run_generation` is rebuilt as `begin(prompt_ids, settings)` + loop; the SSE encoder (events, 16 KiB code-point deltas, chunk framing); `ChunkedStreamSink` over a small `StreamSocket` seam implemented with M1's `socket_io`; and `run_chat_stream`, which renders events from a pending decoded-text buffer only when the socket is writable and releases the engine at `DONE` before the tail is flushed. Integration: a stream branch in M1's `ChatRoute::handle` after the gate grants the `Lease` and `GEN` is written (sampling included), and M1's dispatch site `deliver_route_result`, which runs the handler against M2's non-blocking sink and passes the stream's log code to M1's request log.

**Tech Stack:** C++20, standard library and OS sockets only (through M1's `socket_io`); CMake + Ninja from `.venv-dev/`; presets `linux-gcc` and `windows-llvm-mingw-cross` with `-Werror`; wine is a smoke test; MSYS2 `windows-msys2-clang` CI is the Windows authority.

**Spec:** `docs/superpowers/specs/2026-10-14-sp3-api-concurrency-streaming-design.md` (APPROVED with the owner decisions of 2026-10-14 07:43 UTC: B1–B12 "approve all"; plans: two, M1 and M2). This plan is **M2**. M1's plan (`docs/superpowers/plans/2026-10-14-sp3-m1-concurrency-hardening.md`) is the source of every M1 name used here; controller rulings R-A…R-K of pre-flight review 1 are applied.

---

## Base, timing, where to work

- **M2 executes AFTER M1 is merged** to `main`; M1 itself follows the SP1 and SP2 merges. Do not start Task 1 before the owner has merged M1.
- **Execution worktree:** `/opt/projects/ai/.worktrees/sp3`, branch `sp3-api-streaming` — the same worktree M1 used. After M1's merge, M2 continues on this branch (the owner fast-forwards or re-creates it on the merged `main`; Task 1 Step 1 checks it). Never push, merge, rebase or switch branches; no `rm -rf`, no `git clean`; commits are local and the owner pushes.
- **Shell prefix for every command** (run from the frontend directory):

  ```bash
  cd /opt/projects/ai/.worktrees/sp3/secure_frontend && export PATH="$(git rev-parse --show-toplevel)/.venv-dev/bin:$PATH"
  ```

- **Build and test:**
  - Linux: `cmake --preset linux-gcc && cmake --build --preset linux-gcc && ctest --preset linux-gcc`
  - One case: `ctest --preset linux-gcc -R '^<binary>\.<case>$' --output-on-failure`
  - Windows cross build (must pass with `-Werror`): `cmake --preset windows-llvm-mingw-cross && cmake --build --preset windows-llvm-mingw-cross`
  - wine smoke (excludes `windows-runtime`): `WINEPREFIX=$PWD/out/wineprefix WINEDLLOVERRIDES=mscoree,mshtml= ctest --preset windows-llvm-mingw-cross`
  - One wine case: `WINEPREFIX=$PWD/out/wineprefix WINEDLLOVERRIDES=mscoree,mshtml= wine out/build/windows-llvm-mingw-cross/<binary>.exe --case <case>`
- **Commits (R-D):** one local commit per task, always with the literal trailer lines (no shell variable):

  ```bash
  git -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit -m "<subject>" -m "<body: every acceptance ID, CTest name, RED line, wine line>" \
    -m "Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>" -m "Claude-Session: <session link>"
  ```

- **Citations:** existing code is cited by quoted anchor text and function name, never by line number. M1 code is cited by the names of M1's plan.

## Global Constraints

- No third-party dependencies in `secure_frontend/`: standard library and OS APIs only (Windows: `ws2_32`).
- `-Werror` on every target: libraries via M1's `secure_frontend_add_library(<name> SOURCES … LINKS …)`, tests via `secure_frontend_add_test_executable`, the probe executable via `secure_frontend_set_warnings`.
- TDD: every §9 row is written first, run to fail for the stated reason (the RED line goes into the commit body and the handoff row), then fixed; full `ctest --preset linux-gcc` and the `windows-llvm-mingw-cross` build green before each local commit. A test that pins behavior an earlier task already delivered may pass on its first run; the task says so and the handoff row records it.
- No change under `upstream/` and none to the root `CMakeLists.txt`; stop and ask the owner first.
- The engine never gets network flags; M2 adds no engine input (decision B6's `GEN` keys are M1's; M2 passes them on the stream path).
- Configuration only through SP1's typed keys; M2 adds **no** schema row. Every M2 number is a named constant in M1's `include/secure_frontend/api_constants.hpp` (`secure_frontend::api_constants`: `kStreamPollSlice` 20 ms, `kMaxSendChunk` 64 KiB, `kMaxDeltaTextBytes` 16 KiB, `kMaxSseEventBytes` 128 KiB, `kShutdownDrainBudget` 2 s — all already there; M2 adds none). No environment variable is read.
- Fixed-reason rule: SSE error events, logs and errors carry only §7.2 literals (`api_error_row(ApiError)`) — never a token, prompt, completion, header or body value, option value, configured value, path, engine line or exit code.
- Thread rule: only the worker holding the single `Lease` touches the protocol client; every explicit call into SP2 (`snapshot`, `acquire`, `report_fatal`) goes through `SupervisorPort`. Shutdown (R-H) is `context.shutting_down->load() || shutdown_signalled()` (in the generation loop: `ExecutionControl::shutting_down` or `ExecutionControl::shutdown_requested()`, which `ChatRoute` fills with `shutdown_signalled()`); after it is true `report_fatal` is never called, and every worker-side wait (generation drain, stream tail flush, `~LeaseHold` drain) is bounded by the remaining `api_constants::kShutdownDrainBudget`, because SP2's 5 s `_Exit` timer starts only after the pool has joined.
- Framing (decision B5): every stream is `Transfer-Encoding: chunked`; every normal or error end sends `data: [DONE]` and the terminal chunk `0\r\n\r\n`; a cut (stall, client gone, tail deadline, crash) sends **no** terminal chunk. `[DONE]` is not a success marker. Complete responses keep `Content-Length`.
- UTF-8 (decision B3): invalid bytes mid-text → U+FFFD and continue; an incomplete tail is dropped on `length`/`cancel` ends and becomes one U+FFFD on a `stop` end — on both paths.
- Timeout (decision B4): generation timeout → `STOP`, drain, the text so far with `finish_reason:"length"` (log code `generation_timeout`) — on both paths.
- Headers of a stream are sent only after the gate granted the `Lease`, the context rule was re-checked and `GEN` was written; every refusal before that is a normal HTTP status.
- Windows truth: real Winsock behavior is claimed only from the MSYS2 job; every `secure_frontend_api_server_tests` case and every test that uses real sockets carries `LABELS ${SECURE_FRONTEND_WINDOWS_RUNTIME}` (R-J); pure unit cases are unlabeled and also run under wine. Winsock accepted a 32 MiB blocking `send` whole (SP0 S2) and `Sleep(1)` is about 15.6 ms: every send stays ≤ `kMaxSendChunk`, the stall rule measures progress, and write-stall tests are driven by volume, never by short per-token sleeps.
- Tests are framework-free `main()` programs using `secure_frontend_test::run_cases`, registered with `secure_frontend_add_cases(<binary> <case>... TIMEOUT 120 [LABELS ${SECURE_FRONTEND_WINDOWS_RUNTIME}])`; CTest names `<binary>.<case>` exactly as in spec §9.3; no full test name is defined twice across M1 and M2 (R-E).
- `docs/AGENT_HANDOFF.md` is updated in every task (its "SP3 M2 deliverables" table): the RED line, the linux-gcc `tests passed` line, the wine line.

## Controller notes (spec vs. code; read before Task 1)

1. **Seam types (R-A).** M1 (Task 6a, `request_router.hpp`) owns `HttpHead {status, headers}`, `StreamSink` (`send_headers`, `send_event`, `writable`, `client_gone`, `finish`), `StreamHandler = std::function<void(StreamSink&)>`, `RouteResult = std::variant<HttpResponse, StreamHandler>`, `kNoResponseStatus`; `serialize_head(const HttpHead&, system_clock::time_point)` (`http_wire.hpp`); and the dispatch site `deliver_route_result(NativeSocket, RouteResult, const RouteDecision&, const ConnectionLimits&, const WorkerSignals&)` (`connection_handler.cpp`, Task 8b), which runs a handler against M1's `SocketStreamSink`. **Conflict with spec §2.7:** M1's `SocketStreamSink::send_event` writes each chunk with the blocking `send_all_bounded` (up to `write_stall`), so a slow reader would hold the engine-reading loop for seconds. M2 keeps M1's types and dispatch site but, in Task 10, replaces `SocketStreamSink` inside `deliver_route_result` with M2's non-blocking `ChunkedStreamSink` (same CORS rule from `RouteDecision::allow_origin`, same `status()`/`bytes()`/`finished()`/`failed()` accessors), keeping M1's `stream_handler_dispatch_writes_head_chunks_and_terminal_chunk` green.
   **Log code:** `StreamHandler` returns `void`, so M2 adds one defaulted virtual to M1's `StreamSink`: `virtual void set_log_code(std::string_view) {}` (no-op for every other sink); `deliver_route_result` copies the sink's code into `ConnectionResult::code` (Task 11).
2. **M2 owns the client (R-B):** `LoopbackAddress`, the CMake target `secure_frontend_api_client` and the complete-response client are created in Task 9 (M1 does not build them).
3. **One generation loop (R-C):** M2's free loop is `drive_generation(EngineProtocolClient&, …)`; M1's member `ChatExecutionService::run_generation` is rebuilt on it as `begin(prompt_ids, settings)` + the loop (Task 5). `GenerationFailure::timed_out` is removed: a timeout is a `GenerationResult` with `FinishReason::length` and `timed_out = true` (B4), answered `200`. M1's shutdown drain `min(stop_drain_timeout, kShutdownDrainBudget)` is kept. SP2's legacy `ChatExecutionService::execute` (test-only since M1) is not changed.
4. **Test tokenizer pack (spec §9.2 "gains ids whose bytes are the lead and continuation bytes…")**: SP2's `test_support::byte_tokenizer()` already maps every byte to one id (`byte_for_id`: id k is byte (k+65) mod 256), so multi-byte and lone-continuation sequences need no new ids. M2 adds one **long** ordinary token (id 258, 1024 bytes of `a`) so the 1 MiB and write-stall cases reach their volume under `limits.chat.max_completion_tokens ≤ 16384`.
5. **Fake engine (R-F):** Task 2 is a diff on M1's `StdinReader`/`run_generation` fake: adds `--progress-records`, `--crash-mid-gen`, `--err-mid-gen`, and `--burst-tokens=<n>` (the first n tokens without the per-token pause, for volume-driven stall tests; not in spec §9.2); prefill and per-token sleeps react to `STOP`; `DONE` keeps M1's `<generated> <prompt> 0 0 <finish>` form. M1's `fake_engine_stream_modes_selftest` body is extended, not redefined (R-E).
6. **`stream: true` (C3):** M1's schema answers it `400 unsupported_parameter` (`stream`) and only notes `stream_options`. Task 10 deletes that line, validates `stream_options`, updates M1's `dos01_content_array_is_unsupported_parameter` assertion and M1's handoff "Deferred to M2" list.
7. **Logging (§8):** M1's request log has no `chat_stream` route. Task 11 adds `bool streamed{false}` to `ConnectionResult` and `RequestLogRecord`; `format_request_line` prints route `chat_stream` for a streamed chat; the stream handler's log code becomes `ConnectionResult::code`.
8. **PX `server.json`** lists pid, port, start time and program version — no host — and no writer exists yet. `run_refuses_non_loopback_server_json` needs a host: M2 defines the reader contract (`host` + `port` required, other keys ignored); the future writer must emit `host`.
9. **`decode_token_ids`** (`tokenizer_pack.cpp`, anchor `if (!valid_utf8(bytes)) return error(TokenizerPackErrorCode::decoded_text_not_utf8, "decoded token data is not valid UTF-8");`) rejects a whole response cut mid-character — the latent bug of spec §3.3. Both production paths leave it for `drive_generation`; it keeps its strict whole-text contract for SP2's `execute`.
10. **DONE forms:** existing tests use short forms (`"DONE 1"`, `"DONE stop"`). `next_event` reads `finish` from field 6 of a full record, from field 2 of a two-field record when it is a finish literal, and treats every other short form as `stop`; a full record with an unknown finish is `malformed_line`.
11. **Lease release before the tail (spec §2.5 + §2.7):** a streaming worker destroys its `Lease` and releases the gate as soon as the generation ends (DONE, failure or cancel), then flushes the tail under `response_write_deadline`. This is what lets `dos02_slow_reader_never_backpressures_engine` serve the next chat while the slow reader still drains.
12. **Undecodable output** (a generated id that is a control/user-defined token) has no row in spec §3.4. `drive_generation` sends `STOP`, drains, and returns `undecodable`; the non-stream path keeps M1's `500 internal_error` (`GenerationFailure::decode_failed`), the stream sends the `internal_error` event.

## Interfaces consumed from M1 (names from M1's plan; Task 1 greps each exact string and STOPS if one is missing)

| # | What (spec / M1 task) | Exact grep string(s) in `include/` `src/` `tests/` `CMakeLists.txt` |
| --- | --- | --- |
| A1 | Constants (§5.3, M1 T1) | `namespace secure_frontend::api_constants`, `kStreamPollSlice`, `kMaxSendChunk`, `kMaxDeltaTextBytes`, `kMaxSseEventBytes`, `kShutdownDrainBudget` |
| A2 | Error model (§7.2, M1 T1) | `enum class ApiError`, `const ApiErrorRow& api_error_row(ApiError)`, `struct ErrorOptions`, `HttpResponse make_error_response(ApiError`, `x-should-retry`, `std::string_view log_code{}`; target `secure_frontend_api_core`; helper `function(secure_frontend_add_library` |
| A3 | Router (§3.11, M1 T6a/T6b) | `enum class RouteId`, `struct RequestHead`, `struct RouteDecision`, `struct RequestContext`, `const std::atomic<bool>* shutting_down`, `class RequestRouter`, `kNoResponseStatus` |
| A4 | Seam (R-A, M1 T6a/T8b) | `struct HttpHead final`, `struct StreamSink`, `using StreamHandler = std::function<void(StreamSink&)>;`, `using RouteResult = std::variant<HttpResponse, StreamHandler>;`, `std::string serialize_head(const HttpHead& head, std::chrono::system_clock::time_point now);`, `ConnectionResult deliver_route_result(`, `class SocketStreamSink final : public StreamSink` |
| A5 | Socket I/O (M1 T8a) | `using NativeSocket`, `int poll_sockets(std::span<PollEntry>`, `IoResult send_some(NativeSocket`, `IoResult recv_some(NativeSocket`, `enum class IoStatus`, `PeerState probe_peer(NativeSocket`, `bool set_non_blocking(NativeSocket`, `void set_no_delay(NativeSocket`, `class Socket final`, `class WinsockGuard`; target `secure_frontend_socket_io` |
| A6 | Connection result (M1 T8b) | `struct ConnectionResult`, `ConnectionResult serve_connection(` |
| A7 | Port and gate (§2.5, M1 T13) | `class SupervisorPort`, `void report_fatal(const Lease&, KillReason)`, `class GenerationGate`, `enum class GateRefusal`, `void release() noexcept`; target `secure_frontend_generation_gate` |
| A8 | Generation (M1 T15) | `enum class FinishReason`, `struct GenerationSettings`, `struct ExecutionControl`, `std::function<bool()> shutdown_requested{}`, `struct GenerationResult`, `enum class GenerationFailure`, `GenerationOutcome run_generation(`, `std::size_t context_tokens()`, `render_sampling_keys(` |
| A9 | Chat route (M1 T16) | `struct ChatRouteConfig`, `std::optional<std::string> make_completion_id()`, `class ChatRoute`, file `src/chat_route.cpp`; target `secure_frontend_chat_route` |
| A10 | Request log (§8, M1 T21) | `struct RequestLogRecord`, `std::string format_request_line(`, `RequestLogRecord log_record_for(`; target `secure_frontend_request_log` |
| A11 | Schema (§3.1, M1 T7) | `return fail(ApiError::unsupported_parameter, "stream");`, `stream_options_seen = true;`, `dos01_content_array_is_unsupported_parameter` |
| A12 | Config (§5.1, M1 T4/T5) | `"limits.http.write_stall_ms"`, `"limits.http.response_write_deadline_ms"`, `"limits.http.stream_keepalive_ms"`, `"logging.requests"`, `"limits.chat.default_max_tokens"` (minimum 0); `HttpLimits` members `write_stall`, `response_write_deadline`, `stream_keepalive` |
| A13 | Test machinery (M1 T9, T14) | `ServerOptions` member `config_literals`; `secure_frontend_api_server_tests`; `fake_engine_stream_modes_selftest`; `class StdinReader`; `--token-interval-ms`; cases `web04_default_emits_no_cors_header_anywhere`, `dos02_resources_return_to_idle`, `auth01_every_route_rejects_missing_malformed_wrong_token` (in `tests/api_server_test.cpp`) |
| A14 | SP1/SP2 (consumed, not redesigned) | `void append_json_string(std::string* output, std::string_view value)`, `parse_json(`, `parse_listener_address(`, `class IpAddress`, `class Lease`, `kill_reason_for(`, `shutdown_signalled(`, `class ServerProcess`, `function(secure_frontend_add_cases` |

**M2 creates (R-B and its own pieces):** `LoopbackAddress` and the target `secure_frontend_api_client` (Task 9), `secure_frontend_api_client_tests`, `secure_frontend_sse_tests`, `strata-api-probe`, and every type in the File structure below.

## Review Focus

1. **A stream whose engine emits no token** (`DONE 0 … stop` right after `GEN`, e.g. a model that answers EOS at once) must still send the role chunk, a final chunk with `"stop"`, `[DONE]` and the terminal chunk — never an empty content chunk and never a cut. Pinned in Task 8 (`sse_tests.stream_with_no_tokens_has_role_final_done`).
2. **`max_generated_bytes` reached in the middle of a multi-byte character** (CJK or emoji output) must cut on a code-point boundary, send `STOP`, and end with `"length"` — never a split character and never U+FFFD from the cap itself. Pinned in Task 5 (`chat_execution_tests.drive_generation_bytes_cap_cuts_on_code_point`).
3. **A client that disconnects during prefill**, before any token (Ctrl-C in `strata run` while a long prompt is read), must cause `STOP` within one poll slice of the frontend seeing EOF and free the engine for the next request. Pinned in Task 11 (`api_server_tests.stream_cancel_during_prefill_frees_engine`).
4. **Wrong `stream` types and orphan options** — `"stream": "true"`, `"stream": 1`, `stream_options` without `"stream": true`, `include_usage` that is not a boolean, an unknown key inside `stream_options` — must be `400 invalid_request` before any queue place or stream header. Pinned in Task 10 (`chat_schema_tests.stream_flag_type_and_options_rules`).
5. **Keepalive while output is pending** must never be interleaved with, or sent instead of, pending content, and never sent while the sink cannot take a whole event. Pinned in Task 12 (`sse_tests.keepalive_never_splits_a_pending_event`).

## File structure

| File | Responsibility | Task |
| --- | --- | --- |
| `tests/supervision_fake_engine_main.cpp` (M1's fake, modify) | `--progress-records`, `--crash-mid-gen`, `--err-mid-gen`, `--burst-tokens`, STOP-aware sleeps | 2 |
| `tests/support/test_tokenizer.hpp` (modify) | long token id 258, `id_for_byte`, `token_ids_for` | 2 |
| `include/secure_frontend/utf8_stream_decoder.hpp`, `src/utf8_stream_decoder.cpp` | `Utf8StreamDecoder` | 3 |
| `include/secure_frontend/tokenizer_pack.hpp`, `src/tokenizer_pack.cpp` (modify) | `TokenByteDecoder`; `decode_token_ids` rebuilt on it | 3 |
| `include/secure_frontend/engine_protocol_client.hpp`, `src/engine_protocol_client.cpp` (modify) | `next_event`; `read_generation` rebuilt on it | 4 |
| `include/secure_frontend/generation_run.hpp`, `src/generation_run.cpp` | `drive_generation`, the one generation loop | 5 |
| `include/secure_frontend/chat_execution.hpp`, `src/chat_execution.cpp` (modify) | `begin`, `drive_limits`, M1's `run_generation` rebuilt | 5 |
| `src/chat_route.cpp` (M1's, modify) | timeout → `200 length`; stream branch | 5, 10, 14 |
| `tests/support/chat_json.hpp` | test JSON helpers, `start_stream_server`, `chat_body`, `file_has_line` | 5 |
| `include/secure_frontend/sse_encoder.hpp`, `src/sse_encoder.cpp` | events, delta split, chunk framing, error event | 6 |
| `include/secure_frontend/stream_socket.hpp`, `src/stream_socket.cpp` | `StreamSocket` seam, `AcceptedSocketStream` on `socket_io` | 7 |
| `include/secure_frontend/chunked_stream_sink.hpp`, `src/chunked_stream_sink.cpp` | `ChunkedStreamSink` (a `StreamSink`), `make_sse_head` | 7 |
| `include/secure_frontend/chat_stream.hpp`, `src/chat_stream.cpp` | `run_chat_stream` | 8, 11–14 |
| `include/secure_frontend/loopback_address.hpp`, `include/secure_frontend/api_client.hpp`, `src/loopback_address.cpp`, `src/api_client.cpp` | `LoopbackAddress`, `ChunkedSseReader`, `LoopbackStreamConnection`, `post_chat_completion` | 9 |
| `include/secure_frontend/lease_hold.hpp`, `src/lease_hold.cpp` | `LeaseHold` (report, stop/drain or abandon, destroy lease, release gate) | 10 |
| `tests/support/stream_client.hpp` | e2e wrappers over the client | 10 |
| `include/secure_frontend/request_router.hpp`, `src/connection_handler.cpp`, `include/secure_frontend/request_log.hpp`, `src/request_log.cpp` (M1's, modify) | `StreamSink::set_log_code`; `deliver_route_result` uses `ChunkedStreamSink`; `streamed` flag; `chat_stream` log route | 10, 11 |
| `include/secure_frontend/run_target.hpp`, `src/run_target.cpp` | `run_target_from_server_json` | 15 |
| `tests/api_probe_main.cpp`, `docs/validation/sp3-api-kit.md` | `strata-api-probe`, GPU kit procedure | 16 |
| `tests/sse_test.cpp` | binary `secure_frontend_sse_tests` | 3, 6–8, 11–14 |
| `tests/api_client_test.cpp` | binary `secure_frontend_api_client_tests` | 9, 15, 16 |
| `tests/api_server_test.cpp` (M1's; extend) | M2 end-to-end cases | 5, 10–14 |

---
## Tasks, in execution order

### Task 1: Verify M1's interfaces on the branch; baseline

**Files:**
- Modify: `docs/AGENT_HANDOFF.md` (new section "SP3 M2 deliverables")

**Interfaces:**
- Consumes: every row of "Interfaces consumed from M1".
- Produces: the handoff section with the baseline counts N₀ (linux-gcc) and W₀ (wine) and the M2 deliverables table.

- [ ] **Step 1: Check the base**

```bash
cd /opt/projects/ai/.worktrees/sp3/secure_frontend && export PATH="$(git rev-parse --show-toplevel)/.venv-dev/bin:$PATH"
git status --short && git branch --show-current && git merge-base --is-ancestor main HEAD && echo "HEAD contains main"
git log --oneline -25 | grep -i 'SP3' | head
```
Expected: clean tree, branch `sp3-api-streaming`, "HEAD contains main", M1's commits visible. If M1 is not merged: STOP and report.

- [ ] **Step 2: Grep every consumed name (exact strings)**

```bash
fail=0
check() { if grep -rqF -- "$1" include src tests CMakeLists.txt; then echo "OK   $1"; else echo "MISS $1"; fail=1; fi; }
for s in 'namespace secure_frontend::api_constants' 'kStreamPollSlice' 'kMaxSendChunk' 'kMaxDeltaTextBytes' 'kMaxSseEventBytes' \
         'kShutdownDrainBudget' 'enum class ApiError' 'const ApiErrorRow& api_error_row(ApiError)' 'struct ErrorOptions' \
         'HttpResponse make_error_response(ApiError' 'x-should-retry' 'std::string_view log_code{}' 'secure_frontend_api_core' \
         'function(secure_frontend_add_library' 'enum class RouteId' 'struct RequestHead' 'struct RouteDecision' 'struct RequestContext' \
         'const std::atomic<bool>* shutting_down' 'class RequestRouter' 'kNoResponseStatus' \
         'struct HttpHead final' 'struct StreamSink' 'using StreamHandler = std::function<void(StreamSink&)>;' \
         'using RouteResult = std::variant<HttpResponse, StreamHandler>;' \
         'std::string serialize_head(const HttpHead& head, std::chrono::system_clock::time_point now);' \
         'ConnectionResult deliver_route_result(' 'class SocketStreamSink final : public StreamSink' \
         'std::function<bool()> shutdown_requested{}' \
         'using NativeSocket' 'int poll_sockets(std::span<PollEntry>' 'IoResult send_some(NativeSocket' 'IoResult recv_some(NativeSocket' \
         'enum class IoStatus' 'PeerState probe_peer(NativeSocket' 'bool set_non_blocking(NativeSocket' 'void set_no_delay(NativeSocket' \
         'class Socket final' 'class WinsockGuard' 'secure_frontend_socket_io' 'struct ConnectionResult' 'ConnectionResult serve_connection(' \
         'class SupervisorPort' 'void report_fatal(const Lease&, KillReason)' 'class GenerationGate' 'enum class GateRefusal' \
         'void release() noexcept' 'secure_frontend_generation_gate' 'enum class FinishReason' 'struct GenerationSettings' \
         'struct ExecutionControl' 'struct GenerationResult' 'enum class GenerationFailure' 'GenerationOutcome run_generation(' \
         'std::size_t context_tokens()' 'render_sampling_keys(' 'struct ChatRouteConfig' 'std::optional<std::string> make_completion_id()' \
         'class ChatRoute' 'secure_frontend_chat_route' 'struct RequestLogRecord' 'std::string format_request_line(' \
         'RequestLogRecord log_record_for(' 'secure_frontend_request_log' 'return fail(ApiError::unsupported_parameter, "stream");' \
         'stream_options_seen = true;' 'dos01_content_array_is_unsupported_parameter' '"limits.http.write_stall_ms"' \
         '"limits.http.response_write_deadline_ms"' '"limits.http.stream_keepalive_ms"' '"logging.requests"' 'config_literals' \
         'secure_frontend_api_server_tests' 'fake_engine_stream_modes_selftest' 'class StdinReader' '--token-interval-ms' \
         'web04_default_emits_no_cors_header_anywhere' 'dos02_resources_return_to_idle' 'auth01_every_route_rejects_missing_malformed_wrong_token' \
         'void append_json_string(std::string* output, std::string_view value)' 'parse_json(' 'parse_listener_address(' 'class IpAddress' \
         'class Lease' 'kill_reason_for(' 'shutdown_signalled(' 'class ServerProcess' 'function(secure_frontend_add_cases'; do check "$s"; done
grep -n 'struct HttpLimits' -A16 include/secure_frontend/frontend_config.hpp | grep -E 'write_stall|response_write_deadline|stream_keepalive'
grep -rn '"limits.chat.default_max_tokens"' src | head -2   # minimum must be 0 (B1)
grep -rn 'class LoopbackAddress\|secure_frontend_api_client\b' include src CMakeLists.txt && echo "UNEXPECTED: M2 creates these (R-B)"
echo "result: $fail"
```
Note for the executor: M2 changes M1's `ChatRoute::handle` return type from `HttpResponse` to `RouteResult` (Task 10), adds `StreamSink::set_log_code` (Task 7), and `bool streamed` to `ConnectionResult`/`RequestLogRecord` (Task 11); the greps above check M1's names as M1 built them.

Expected: every line `OK`, the three `HttpLimits` members printed, `default_max_tokens` minimum `0`, no "UNEXPECTED" line, `result: 0`. Any `MISS` or unexpected hit: STOP and report the exact strings to the controller; do not continue.

- [ ] **Step 3: Baseline counts**

```bash
cmake --preset linux-gcc && cmake --build --preset linux-gcc && ctest --preset linux-gcc 2>&1 | tail -3
cmake --preset windows-llvm-mingw-cross && cmake --build --preset windows-llvm-mingw-cross && \
  WINEPREFIX=$PWD/out/wineprefix WINEDLLOVERRIDES=mscoree,mshtml= ctest --preset windows-llvm-mingw-cross 2>&1 | tail -3
```
Expected: `100% tests passed, 0 tests failed out of N₀` and the wine line `… out of W₀`. A red baseline: STOP and report.

- [ ] **Step 4: Write the handoff section** — append to `docs/AGENT_HANDOFF.md`:

```markdown
### SP3 M2 deliverables (branch `sp3-api-streaming`, after the M1 merge)

Plan: `docs/superpowers/plans/2026-10-14-sp3-m2-streaming.md`. Baseline after M1: linux-gcc N₀ = <n>, wine W₀ = <w>. Every M1 name M2 consumes was found (Task 1 Step 2).

| Task | Deliverable | RED | linux-gcc | wine | Status |
| --- | --- | --- | --- | --- | --- |
| 1 | M1 interfaces verified; baseline | — | N₀ | W₀ | done |
```

- [ ] **Step 5: Commit**

```bash
git add ../docs/AGENT_HANDOFF.md
git -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit -m "docs: SP3 M2 baseline and M1 interface check" \
  -m "Verifies the M1 names M2 consumes (spec §3.11, §5, §7; M1 plan Tasks 1-23 incl. 6a/6b, 8a/8b) and records the baseline counts." \
  -m "Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>" -m "Claude-Session: <session link>"
```

---

### Task 2: Fake engine — M2 modes on M1's reader, STOP-aware sleeps, the long test token (spec §9.2; R-F)

**Files:**
- Modify: `tests/supervision_fake_engine_main.cpp` (M1's `Options`, `parse_options`, `run_generation`), `tests/fake_engine_test.cpp` (extend M1's `fake_engine_stream_modes_selftest`), `tests/support/test_tokenizer.hpp`

**Interfaces:**
- Consumes: M1's fake (`StdinReader` with `next()` and `take_stop()`, `run_generation(const Options&, const std::string&, StdinReader&)`, `Options::{token_ids, tokens, token_interval_ms, finish, prefill_delay_ms, stop_delay_ms, stream_modes}`, `sleep_ms`, `value_of`), M1's selftest body and SP2's `Child` helper.
- Produces: fake flags `--progress-records` (one `PP <i> <steps>` per 100 ms of prefill), `--crash-mid-gen=<k>` (exit 1 after k tokens), `--err-mid-gen=<k>` (`ERR test` after k tokens, then idle), `--burst-tokens=<n>` (the first n tokens without the `--token-interval-ms` pause); `STOP` is acted on within about 1 ms during prefill and during every per-token pause; `DONE` stays `DONE <generated> <prompt> 0 0 <finish>`. Test helpers `test_support::kLongTokenId` (258), `kLongTokenBytes` (1024), `id_for_byte(unsigned char)`, `token_ids_for(std::string_view bytes)`. No second stdin reader is added.

- [ ] **Step 1: Extend M1's selftest (failing)** — in `tests/fake_engine_test.cpp`, inside `fake_engine_stream_modes_selftest`, after its last block (anchor `"without new modes the SP2 short form is unchanged");` and the `}` that closes that block), add:

```cpp
  {  // M2: STOP during prefill is acted on at once
    Child child({"--prefill-delay-ms=3000", "--tokens=5"});
    (void)child.line();
    child.send("GEN 5 1");
    std::this_thread::sleep_for(50ms);
    const auto sent = std::chrono::steady_clock::now();
    child.send("STOP");
    expect(child.line() == "DONE 0 1 0 0 cancel", "STOP during prefill: no token, cancel");
    expect(std::chrono::steady_clock::now() - sent < 1s, "not after the 3 s prefill");
  }
  {  // M2: STOP during a long per-token pause is acted on at once
    Child child({"--tokens=10", "--token-interval-ms=3000"});
    (void)child.line();
    child.send("GEN 10 1");
    expect(child.line() == "T 7", "first token");
    const auto sent = std::chrono::steady_clock::now();
    child.send("STOP");
    expect(child.line() == "DONE 1 1 0 0 cancel", "the emitted token counts");
    expect(std::chrono::steady_clock::now() - sent < 1s, "not after the 3 s pause");
  }
  {  // M2: --burst-tokens skips the pause for the first n tokens
    Child child({"--tokens=6", "--burst-tokens=4", "--token-interval-ms=400"});
    (void)child.line();
    child.send("GEN 6 1");
    const auto start = std::chrono::steady_clock::now();
    for (int index = 0; index < 4; ++index) expect(child.line() == "T 7", "burst token");
    expect(std::chrono::steady_clock::now() - start < 300ms, "four tokens without a pause");
    expect(child.line() == "T 7" && std::chrono::steady_clock::now() - start >= 350ms, "then the pause applies");
  }
  {  // M2: --progress-records during prefill
    Child child({"--prefill-delay-ms=300", "--progress-records", "--tokens=1"});
    (void)child.line();
    child.send("GEN 1 1");
    expect(child.line() == "PP 1 3" && child.line() == "PP 2 3" && child.line() == "PP 3 3" && child.line() == "T 7", "PP then the token");
  }
  {  // M2: --err-mid-gen
    Child child({"--tokens=5", "--err-mid-gen=2"});
    (void)child.line();
    child.send("GEN 5 1");
    const std::string a = child.line(), b = child.line(), err = child.line();
    expect(a == "T 7" && b == "T 7" && err == "ERR test", "ERR after two tokens: " + err);
  }
  {  // M2: --crash-mid-gen
    Child child({"--tokens=5", "--crash-mid-gen=1"});
    (void)child.line();
    child.send("GEN 5 1");
    expect(child.line() == "T 7", "one token before the crash");
    expect(child.kind(5s) == secure_frontend::EngineReadKind::closed, "--crash-mid-gen exits");
  }
```
(Add `#include <thread>` if missing.) The case is already registered by M1; no CMake change.

- [ ] **Step 2: Run to see it fail**

Run: `cmake --build --preset linux-gcc && ctest --preset linux-gcc -R '^secure_frontend_fake_engine_tests\.fake_engine_stream_modes_selftest$' --output-on-failure`
Expected: FAIL — `not after the 3 s prefill` (M1's prefill is one uninterruptible `sleep_ms`), then `four tokens without a pause` and `PP then the token` (unknown flags are ignored).

- [ ] **Step 3: Implement** in `tests/supervision_fake_engine_main.cpp`. `Options` gains `bool progress_records{false}; std::optional<unsigned long> crash_mid_gen; std::optional<unsigned long> err_mid_gen; unsigned long burst_tokens{0};`. `parse_options` gains, next to M1's `--token-ids` branch:

```cpp
    else if (argument == "--progress-records") { options.progress_records = true; options.stream_modes = true; }
    else if (value_of(argument, "--crash-mid-gen", value)) { options.crash_mid_gen = std::stoul(value); options.stream_modes = true; }
    else if (value_of(argument, "--err-mid-gen", value)) { options.err_mid_gen = std::stoul(value); options.stream_modes = true; }
    else if (value_of(argument, "--burst-tokens", value)) { options.burst_tokens = std::stoul(value); options.stream_modes = true; }
```

Add before M1's `run_generation` (fake):

```cpp
// Sleeps up to `milliseconds` in 1 ms steps; true (and STOP consumed) when STOP arrived meanwhile.
bool sleep_or_stop(StdinReader& reader, const int milliseconds) {
  const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
  do {
    if (reader.take_stop()) return true;
    sleep_ms(1);
  } while (std::chrono::steady_clock::now() < until);
  return reader.take_stop();
}
```

and replace M1's fake `run_generation` body after its prompt-count parse (anchor `const unsigned long total = options.tokens.value_or(options.token_ids.size());`, through the final `DONE` write) with:

```cpp
  const unsigned long total = options.tokens.value_or(options.token_ids.size());
  bool stopped = false;
  if (options.prefill_delay_ms > 0) {
    const int steps = options.progress_records ? std::max(1, options.prefill_delay_ms / 100) : 1;
    for (int step = 1; step <= steps && !stopped; ++step) {
      stopped = sleep_or_stop(reader, options.prefill_delay_ms / steps);
      if (options.progress_records && !stopped) std::cout << "PP " << step << ' ' << steps << '\n' << std::flush;
    }
  }
  unsigned long generated = 0;
  std::string finish = options.finish;
  for (; !stopped && generated < total; ++generated) {
    if (reader.take_stop()) { stopped = true; break; }
    if (generated >= max_new) { finish = "length"; break; }
    if (options.crash_mid_gen && generated == *options.crash_mid_gen) { std::cout << std::flush; std::_Exit(1); }
    if (options.err_mid_gen && generated == *options.err_mid_gen) { std::cout << "ERR test\n" << std::flush; return; }
    std::cout << "T " << options.token_ids[generated % options.token_ids.size()] << '\n' << std::flush;
    if (options.token_interval_ms > 0 && generated + 1 >= options.burst_tokens && sleep_or_stop(reader, options.token_interval_ms)) {
      ++generated;  // the token just written counts
      stopped = true;
      break;
    }
  }
  if (stopped) {
    finish = "cancel";
    if (options.stop_delay_ms > 0) sleep_ms(options.stop_delay_ms);
  }
  std::cout << "DONE " << generated << ' ' << prompt << " 0 0 " << finish << '\n' << std::flush;
```

- [ ] **Step 4: Long token and byte helpers** — in `tests/support/test_tokenizer.hpp`, after `byte_for_id`:

```cpp
// Inverse of byte_for_id: the byte pack's id for one byte.
inline std::size_t id_for_byte(const unsigned char byte) { return static_cast<std::size_t>((byte + 256U - 65U) & 0xffU); }

// An ordinary token of kLongTokenBytes 'a' bytes: volume tests reach 1 MiB within
// limits.chat.max_completion_tokens (spec §9.3 dos02_slow_* cases; plan Controller note 4).
inline constexpr std::size_t kLongTokenId = 258U;
inline constexpr std::size_t kLongTokenBytes = 1024U;

// The --token-ids value that makes the fake emit `bytes`, one byte per token.
inline std::string token_ids_for(const std::string_view bytes) {
  std::string csv;
  for (const char character : bytes) {
    if (!csv.empty()) csv += ',';
    csv += std::to_string(id_for_byte(static_cast<unsigned char>(character)));
  }
  return csv;
}
```

Add `#include <string_view>`. In `byte_tokenizer()`, after the `<|im_end|>` entry:

```cpp
  pack.tokens_by_id.emplace_back(kLongTokenBytes, 'a');  // id 258 ('a' is its own byte symbol)
  pack.token_types.push_back(1);
```

- [ ] **Step 5: Run the selftest and the full suite**

Run: `cmake --build --preset linux-gcc && ctest --preset linux-gcc -R 'fake_engine_tests|engine_supervisor_tests|server_tests|tokenizer' --output-on-failure && ctest --preset linux-gcc | tail -1`
Expected: PASS; total N₀ (no new CTest entry). Cross-build; wine total W₀.

- [ ] **Step 6: Handoff row and commit**

```bash
git add tests/supervision_fake_engine_main.cpp tests/support/test_tokenizer.hpp tests/fake_engine_test.cpp ../docs/AGENT_HANDOFF.md
git -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit -m "test: fake engine M2 modes, STOP-aware sleeps, long test token (SP3 §9.2)" \
  -m "Adds --progress-records, --crash-mid-gen, --err-mid-gen, --burst-tokens on M1's StdinReader; prefill and per-token pauses react to STOP; byte pack id 258. CTest: secure_frontend_fake_engine_tests.fake_engine_stream_modes_selftest (extended). RED: <line>. wine: <line>." \
  -m "Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>" -m "Claude-Session: <session link>"
```

---

### Task 3: `Utf8StreamDecoder` and `TokenByteDecoder` (spec §3.3, decision 3)

**Files:**
- Create: `include/secure_frontend/utf8_stream_decoder.hpp`, `src/utf8_stream_decoder.cpp`, `tests/sse_test.cpp`
- Modify: `include/secure_frontend/tokenizer_pack.hpp`, `src/tokenizer_pack.cpp` (`decode_token_ids`), `tests/tokenizer_pack_test.cpp`, `CMakeLists.txt`

**Interfaces:**
- Consumes: `TokenizerPack`, `TokenizerDecodeResult`, `TokenizerPackErrorCode` (existing); `test_support::byte_tokenizer`, `id_for_byte`, `kLongTokenId` (Task 2).
- Produces:

```cpp
namespace secure_frontend {
inline constexpr std::string_view kReplacementCharacter = "\xEF\xBF\xBD";
class Utf8StreamDecoder final {
 public:
  [[nodiscard]] std::string push(std::string_view bytes);          // complete UTF-8 released; invalid → U+FFFD
  [[nodiscard]] std::string finish(bool drop_incomplete_tail);     // held tail: "" (drop) or one U+FFFD
  [[nodiscard]] std::size_t held_bytes() const noexcept;
};
class TokenByteDecoder final {
 public:
  explicit TokenByteDecoder(const TokenizerPack& pack);            // pack must outlive the decoder
  [[nodiscard]] TokenizerDecodeResult bytes_for(std::size_t id) const;   // raw bytes, NOT UTF-8 validated
};
}
```
CMake: library `secure_frontend_utf8_stream`; test binary `secure_frontend_sse_tests`.

- [ ] **Step 1: Write the failing tests** — create `tests/sse_test.cpp`:

```cpp
#include "secure_frontend/utf8_stream_decoder.hpp"
#include "support/test_support.hpp"

#include <iostream>
#include <string>

namespace {

using secure_frontend::Utf8StreamDecoder;

int failures = 0;
void expect(const bool condition, const std::string& message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
  }
}
const std::string kR{secure_frontend::kReplacementCharacter};

void utf8_split_across_tokens_is_held_then_released() {
  Utf8StreamDecoder decoder;
  expect(decoder.push("A\xE2") == "A", "ASCII released, a lead byte held");
  expect(decoder.held_bytes() == 1U, "one byte held");
  expect(decoder.push("\x82").empty(), "still incomplete");
  expect(decoder.push("\xAC") == "\xE2\x82\xAC", "the euro sign released whole");
  expect(decoder.push("\xF0\x9F").empty() && decoder.push("\x98\x80!") == "\xF0\x9F\x98\x80!", "4-byte sequence across tokens");
  expect(decoder.finish(false).empty() && decoder.held_bytes() == 0U, "nothing held at the end");
}

void utf8_invalid_sequence_becomes_replacement_character() {
  Utf8StreamDecoder decoder;
  expect(decoder.push("a\x80" "b") == "a" + kR + "b", "lone continuation byte");
  expect(decoder.push("\xC0\xAF") == kR + kR, "overlong lead C0 and its continuation");
  expect(decoder.push("\xE2\x82" "A") == kR + "A", "truncated sequence then ASCII: one U+FFFD");
  expect(decoder.push("\xED\xA0\x80") == kR + kR + kR, "surrogate encoding");
  expect(decoder.push("\xF5\xFF") == kR + kR, "bytes that never start a sequence");
  expect(decoder.push("\xF4\x90\x80\x80") == kR + kR + kR + kR, "above U+10FFFF");
  expect(decoder.push("ok") == "ok", "the decoder continues after invalid input");
}

void utf8_incomplete_tail_dropped_on_length_and_cancel() {
  Utf8StreamDecoder decoder;
  expect(decoder.push("ok\xF0\x9F\x98") == "ok", "three bytes of an emoji held");
  expect(decoder.finish(true).empty(), "length/cancel end: the cut tail is dropped");
  expect(decoder.held_bytes() == 0U, "finish resets the decoder");
}

void utf8_incomplete_tail_on_stop_becomes_one_replacement() {
  Utf8StreamDecoder decoder;
  expect(decoder.push("ok\xE2\x82") == "ok", "two bytes held");
  expect(decoder.finish(false) == kR, "stop end: the tail is one U+FFFD");
}

}  // namespace

int main(int argc, char* argv[]) {
  return secure_frontend_test::run_cases(argc, argv,
                                         {
                                             {"utf8_split_across_tokens_is_held_then_released", utf8_split_across_tokens_is_held_then_released},
                                             {"utf8_invalid_sequence_becomes_replacement_character", utf8_invalid_sequence_becomes_replacement_character},
                                             {"utf8_incomplete_tail_dropped_on_length_and_cancel", utf8_incomplete_tail_dropped_on_length_and_cancel},
                                             {"utf8_incomplete_tail_on_stop_becomes_one_replacement", utf8_incomplete_tail_on_stop_becomes_one_replacement},
                                         },
                                         failures);
}
```

Add to `tests/tokenizer_pack_test.cpp` (include `"support/test_tokenizer.hpp"`):

```cpp
void token_byte_decoder_returns_raw_token_bytes() {
  const auto pack = test_support::byte_tokenizer();
  const secure_frontend::TokenByteDecoder decoder(pack);
  const auto lead = decoder.bytes_for(test_support::id_for_byte(0xE2U));
  expect(std::holds_alternative<std::string>(lead) && std::get<std::string>(lead) == "\xE2", "a lead byte alone is returned, not rejected");
  const auto long_token = decoder.bytes_for(test_support::kLongTokenId);
  expect(std::holds_alternative<std::string>(long_token) && std::get<std::string>(long_token) == std::string(1024U, 'a'), "multi-symbol token");
  const auto control = decoder.bytes_for(256U);
  expect(std::holds_alternative<secure_frontend::TokenizerPackError>(control) &&
             std::get<secure_frontend::TokenizerPackError>(control).code == secure_frontend::TokenizerPackErrorCode::undecodable_token,
         "control tokens are not ordinary output");
  const auto outside = decoder.bytes_for(100000U);
  expect(std::holds_alternative<secure_frontend::TokenizerPackError>(outside) &&
             std::get<secure_frontend::TokenizerPackError>(outside).code == secure_frontend::TokenizerPackErrorCode::token_id_out_of_range,
         "ids outside the vocabulary");
}
```
Register `{"token_byte_decoder_returns_raw_token_bytes", token_byte_decoder_returns_raw_token_bytes}` in its `main()` and append the case to `set(_tokenizer_cases …` in `CMakeLists.txt`.

CMake — before `if(STRATA_BUILD_SECURE_FRONTEND_TESTS)`:

```cmake
# SP3 M2 — streaming.
secure_frontend_add_library(secure_frontend_utf8_stream SOURCES src/utf8_stream_decoder.cpp)
```

and inside the tests block:

```cmake
    # SP3 M2: SSE encoder, UTF-8 stream decoder, sink and stream pump units. Pure: unlabeled, also run under wine.
    secure_frontend_add_test_executable(secure_frontend_sse_tests tests/sse_test.cpp secure_frontend_utf8_stream)
    set(SECURE_FRONTEND_SSE_CASES
        utf8_split_across_tokens_is_held_then_released utf8_invalid_sequence_becomes_replacement_character
        utf8_incomplete_tail_dropped_on_length_and_cancel utf8_incomplete_tail_on_stop_becomes_one_replacement)
    secure_frontend_add_cases(secure_frontend_sse_tests ${SECURE_FRONTEND_SSE_CASES} TIMEOUT 120)
```
(Later tasks append case names to the `set(SECURE_FRONTEND_SSE_CASES` line and libraries to the `secure_frontend_add_test_executable(secure_frontend_sse_tests` line.)

- [ ] **Step 2: Run to see it fail**

Run: `cmake --preset linux-gcc && cmake --build --preset linux-gcc`
Expected: build FAILS with `fatal error: secure_frontend/utf8_stream_decoder.hpp: No such file or directory` and `'TokenByteDecoder' is not a member of 'secure_frontend'`.

- [ ] **Step 3: Implement.** `include/secure_frontend/utf8_stream_decoder.hpp`:

```cpp
#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace secure_frontend {

inline constexpr std::string_view kReplacementCharacter = "\xEF\xBF\xBD";

// Incremental UTF-8 decoder for generated text (spec §3.3, decision 3).
// push() releases the longest complete prefix and holds an incomplete
// trailing sequence (at most 3 bytes) for the next token. A byte that can
// never start or continue a valid sequence becomes U+FFFD (maximal-subpart
// rule: one U+FFFD per broken sequence, then the offending byte is
// re-examined). finish() ends the text: the held tail is dropped for
// length/cancel ends and becomes one U+FFFD for a stop end.
class Utf8StreamDecoder final {
 public:
  [[nodiscard]] std::string push(std::string_view bytes);
  [[nodiscard]] std::string finish(bool drop_incomplete_tail);
  [[nodiscard]] std::size_t held_bytes() const noexcept { return held_size_; }

 private:
  std::array<unsigned char, 4> held_{};
  std::size_t held_size_{0};
  std::size_t needed_{0};
};

}  // namespace secure_frontend
```

`src/utf8_stream_decoder.cpp`:

```cpp
#include "secure_frontend/utf8_stream_decoder.hpp"

namespace secure_frontend {
namespace {

std::size_t sequence_length(const unsigned char lead) noexcept {
  if (lead >= 0xC2U && lead <= 0xDFU) return 2U;
  if (lead >= 0xE0U && lead <= 0xEFU) return 3U;
  if (lead >= 0xF0U && lead <= 0xF4U) return 4U;
  return 0U;
}

// RFC 3629 table 3-7: the second byte's range depends on the lead.
bool valid_second(const unsigned char lead, const unsigned char byte) noexcept {
  switch (lead) {
    case 0xE0U: return byte >= 0xA0U && byte <= 0xBFU;
    case 0xEDU: return byte >= 0x80U && byte <= 0x9FU;
    case 0xF0U: return byte >= 0x90U && byte <= 0xBFU;
    case 0xF4U: return byte >= 0x80U && byte <= 0x8FU;
    default: return byte >= 0x80U && byte <= 0xBFU;
  }
}

}  // namespace

std::string Utf8StreamDecoder::push(const std::string_view bytes) {
  std::string out;
  out.reserve(bytes.size() + held_size_);
  for (const char character : bytes) {
    const auto byte = static_cast<unsigned char>(character);
    if (held_size_ > 0U) {
      const bool continues = held_size_ == 1U ? valid_second(held_[0], byte) : (byte >= 0x80U && byte <= 0xBFU);
      if (continues) {
        held_[held_size_++] = byte;
        if (held_size_ == needed_) {
          out.append(reinterpret_cast<const char*>(held_.data()), held_size_);
          held_size_ = 0U;
          needed_ = 0U;
        }
        continue;
      }
      out += kReplacementCharacter;  // the broken sequence; `byte` is examined afresh below
      held_size_ = 0U;
      needed_ = 0U;
    }
    if (byte < 0x80U) {
      out.push_back(static_cast<char>(byte));
      continue;
    }
    const std::size_t length = sequence_length(byte);
    if (length == 0U) {
      out += kReplacementCharacter;
      continue;
    }
    held_[0] = byte;
    held_size_ = 1U;
    needed_ = length;
  }
  return out;
}

std::string Utf8StreamDecoder::finish(const bool drop_incomplete_tail) {
  const bool had_tail = held_size_ > 0U;
  held_size_ = 0U;
  needed_ = 0U;
  return had_tail && !drop_incomplete_tail ? std::string(kReplacementCharacter) : std::string();
}

}  // namespace secure_frontend
```

`tokenizer_pack.hpp` — add `#include <map>` and, after `decode_token_ids`:

```cpp
// Raw bytes of ONE ordinary generated token (SP3 streaming). Unlike
// decode_token_ids, the result is not UTF-8 validated: a byte-level token may
// end in the middle of a character, which Utf8StreamDecoder resolves.
// CONTROL (3) and USER_DEFINED (4) types fail closed as in decode_token_ids.
// The pack must outlive the decoder.
class TokenByteDecoder final {
 public:
  explicit TokenByteDecoder(const TokenizerPack& pack);
  [[nodiscard]] TokenizerDecodeResult bytes_for(std::size_t id) const;

 private:
  const TokenizerPack& pack_;
  std::map<std::string, unsigned char> reverse_;
};
```

`tokenizer_pack.cpp` — replace the whole function `decode_token_ids` (the one ending `if (!valid_utf8(bytes)) return error(TokenizerPackErrorCode::decoded_text_not_utf8, "decoded token data is not valid UTF-8");`) with:

```cpp
TokenByteDecoder::TokenByteDecoder(const TokenizerPack& pack) : pack_(pack) {
  const auto symbols = byte_symbols();
  for (unsigned int byte = 0U; byte < 256U; ++byte) reverse_.emplace(symbols[byte], static_cast<unsigned char>(byte));
}

TokenizerDecodeResult TokenByteDecoder::bytes_for(const std::size_t id) const {
  if (id >= pack_.tokens_by_id.size()) return error(TokenizerPackErrorCode::token_id_out_of_range, "token ID is outside the loaded vocabulary");
  if (id >= pack_.token_types.size() || pack_.token_types[id] == 3 || pack_.token_types[id] == 4) {
    return error(TokenizerPackErrorCode::undecodable_token, "control and user-defined token types are not ordinary decoded output");
  }
  const std::string_view token = pack_.tokens_by_id[id];
  std::string bytes;
  for (std::size_t offset = 0U; offset < token.size();) {
    const auto lead = static_cast<unsigned char>(token[offset]);
    const std::size_t width = lead <= 0x7fU ? 1U : lead <= 0xdfU ? 2U : lead <= 0xefU ? 3U : 4U;
    if (offset + width > token.size()) return error(TokenizerPackErrorCode::undecodable_token, "token is not composed of byte alphabet symbols");
    const auto it = reverse_.find(std::string{token.substr(offset, width)});
    if (it == reverse_.end()) return error(TokenizerPackErrorCode::undecodable_token, "token is not composed of byte alphabet symbols");
    bytes.push_back(static_cast<char>(it->second));
    offset += width;
  }
  return bytes;
}

TokenizerDecodeResult decode_token_ids(const TokenizerPack& pack, const std::vector<std::size_t>& ids) {
  const TokenByteDecoder decoder(pack);
  std::string bytes;
  for (const std::size_t id : ids) {
    auto piece = decoder.bytes_for(id);
    if (auto* failure = std::get_if<TokenizerPackError>(&piece)) return std::move(*failure);
    const auto& text = std::get<std::string>(piece);
    if (bytes.size() + text.size() > kMaximumDecodedBytes) return error(TokenizerPackErrorCode::decoded_text_not_utf8, "decoded token data exceeds fixed limit");
    bytes += text;
  }
  if (!valid_utf8(bytes)) return error(TokenizerPackErrorCode::decoded_text_not_utf8, "decoded token data is not valid UTF-8");
  return bytes;
}
```

- [ ] **Step 4: Run**

Run: `cmake --build --preset linux-gcc && ctest --preset linux-gcc -R '^secure_frontend_(sse|tokenizer_pack)_tests\.' --output-on-failure && ctest --preset linux-gcc`
Expected: PASS; total previous + 5. Cross-build; wine total previous + 5 (all unlabeled).

- [ ] **Step 5: Handoff row and commit**

```bash
git add include/secure_frontend/utf8_stream_decoder.hpp src/utf8_stream_decoder.cpp include/secure_frontend/tokenizer_pack.hpp src/tokenizer_pack.cpp \
  tests/sse_test.cpp tests/tokenizer_pack_test.cpp CMakeLists.txt ../docs/AGENT_HANDOFF.md
git -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit -m "feat: UTF-8 stream decoder and per-token byte decoding (SP3 §3.3)" \
  -m "Decision 3: invalid bytes become U+FFFD, the incomplete tail is dropped on length/cancel and is one U+FFFD on stop. CTest: secure_frontend_sse_tests.utf8_split_across_tokens_is_held_then_released, .utf8_invalid_sequence_becomes_replacement_character, .utf8_incomplete_tail_dropped_on_length_and_cancel, .utf8_incomplete_tail_on_stop_becomes_one_replacement; secure_frontend_tokenizer_pack_tests.token_byte_decoder_returns_raw_token_bytes RED: <line>. wine: <line>." -m "Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>" -m "Claude-Session: <session link>"
```

---

### Task 4: `EngineProtocolClient::next_event`; `read_generation` rebuilt on it (spec §3.3, §3.11)

**Files:**
- Modify: `include/secure_frontend/engine_protocol_client.hpp`, `src/engine_protocol_client.cpp` (`read_generation`)
- Test: `tests/engine_protocol_client_test.cpp`, `CMakeLists.txt`

**Interfaces:**
- Consumes: existing `EngineProtocolClient`, `EngineProtocolError`, `fail`, `validate_incoming`, `fields`, `parse_decimal`, `deadline_after`, `remaining_until`.
- Produces:

```cpp
struct TokenEvent final { std::uint32_t id; };
struct DoneEvent final { std::string_view finish; std::size_t generated; };  // finish: static literal stop|length|cancel|nonfinite
struct ProgressEvent final {};
using GenerationEvent = std::variant<TokenEvent, DoneEvent, ProgressEvent, EngineProtocolError>;
[[nodiscard]] GenerationEvent EngineProtocolClient::next_event(std::chrono::milliseconds timeout);
```
A timeout returns `EngineProtocolError{timeout}` and leaves the generation active; `DONE` (including `nonfinite`) returns the client to idle; every other failure closes the protocol exactly as `read_generation` did. `generated` counts this generation's `T` records.

- [ ] **Step 1: Write the failing tests** — in `tests/engine_protocol_client_test.cpp` (uses its `FakeTransport` and `line`):

```cpp
template <typename T>
const T* as(const secure_frontend::GenerationEvent& event) { return std::get_if<T>(&event); }

EngineProtocolClient generating_client(FakeTransport& transport, const secure_frontend::EngineProtocolFlavor flavor) {
  EngineProtocolClient client(transport, {}, flavor);
  secure_frontend::EngineReady ready{};
  (void)client.await_ready(std::chrono::seconds(1), ready);
  (void)client.begin_generation(4, {1});
  return client;
}

void next_event_yields_tokens_progress_and_done() {
  FakeTransport transport;
  transport.reads = {line("READY 64 stop"), line("T 5"), line("PP 1 2"), line("T 6"), line("DONE 2 1 1.0 1.0 length")};
  auto client = generating_client(transport, secure_frontend::EngineProtocolFlavor::lighttransport_glm53);
  const auto first = client.next_event(std::chrono::seconds(1));
  expect(as<secure_frontend::TokenEvent>(first) && as<secure_frontend::TokenEvent>(first)->id == 5U, "first token");
  expect(as<secure_frontend::ProgressEvent>(client.next_event(std::chrono::seconds(1))) != nullptr, "PP is a progress event");
  expect(as<secure_frontend::TokenEvent>(client.next_event(std::chrono::seconds(1))) != nullptr, "second token");
  const auto done = client.next_event(std::chrono::seconds(1));
  expect(as<secure_frontend::DoneEvent>(done) && as<secure_frontend::DoneEvent>(done)->finish == "length" &&
             as<secure_frontend::DoneEvent>(done)->generated == 2U, "DONE carries finish and count");
  expect(client.idle(), "DONE returns the client to idle");
}

void next_event_timeout_keeps_generation_active() {
  FakeTransport transport;
  transport.reads = {line("READY 64 stop")};
  auto client = generating_client(transport, secure_frontend::EngineProtocolFlavor::strata_0143);
  const auto waited = client.next_event(std::chrono::milliseconds(10));
  expect(as<secure_frontend::EngineProtocolError>(waited) &&
             as<secure_frontend::EngineProtocolError>(waited)->code == EngineProtocolErrorCode::timeout, "an empty slice is a timeout");
  expect(client.generating(), "the generation stays active");
  expect(!client.stop().has_value(), "STOP can still be sent");
  transport.reads.push_back(line("DONE 0 1 1.0 1.0 cancel"));
  const auto done = client.next_event(std::chrono::seconds(1));
  expect(as<secure_frontend::DoneEvent>(done) && as<secure_frontend::DoneEvent>(done)->finish == "cancel", "drained to cancel");
}

void next_event_finish_forms_and_nonfinite_stays_idle() {
  const auto finish_of = [](const std::string& done_line) -> std::string {
    FakeTransport transport;
    transport.reads = {line("READY 64 stop"), line(done_line)};
    auto client = generating_client(transport, secure_frontend::EngineProtocolFlavor::strata_0143);
    const auto event = client.next_event(std::chrono::seconds(1));
    if (const auto* done = as<secure_frontend::DoneEvent>(event)) return std::string(done->finish) + (client.idle() ? "/idle" : "/busy");
    return client.closed() ? "closed" : "other";
  };
  expect(finish_of("DONE 1") == "stop/idle", "legacy short form is an ordinary end");
  expect(finish_of("DONE stop") == "stop/idle", "two-field finish form");
  expect(finish_of("DONE 1 1 1.0 1.0 nonfinite") == "nonfinite/idle", "nonfinite keeps the engine idle (SP0 E3)");
  expect(finish_of("DONE 1 1 1.0 1.0 bogus") == "closed", "unknown finish in a full record is malformed");
}
```
Register the three in `main()` and append them to the `secure_frontend_add_cases(secure_frontend_engine_protocol_client_tests` call.

- [ ] **Step 2: Run to see it fail**

Run: `cmake --build --preset linux-gcc`
Expected: build FAILS: `'GenerationEvent' is not a member of 'secure_frontend'`.

- [ ] **Step 3: Implement.** Header: add `#include <variant>`; after `struct EngineProtocolError` add the four declarations of **Produces**; in the class, after `read_generation`:

```cpp
  // Reads at most one record within `timeout` (a per-call budget, SP3 §3.3).
  // Timeout: EngineProtocolError{timeout}, generation still active. DONE (also
  // DONE ... nonfinite) returns to idle. Any other failure closes the protocol.
  [[nodiscard]] GenerationEvent next_event(std::chrono::milliseconds timeout);
```

Source: in the anonymous namespace add:

```cpp
// DONE <generated> <prompt> <prompt_ms> <decode_ms> <finish> ... (generate.cpp);
// the two-field "DONE <finish>" form and older short forms ("DONE 1") are an ordinary stop.
std::optional<std::string_view> finish_literal(const std::vector<std::string_view>& done) {
  static constexpr std::array<std::string_view, 4> kFinishes{"stop", "length", "cancel", "nonfinite"};
  const auto match = [](const std::string_view word) -> std::optional<std::string_view> {
    for (const std::string_view literal : kFinishes) {
      if (literal == word) return literal;
    }
    return std::nullopt;
  };
  if (done.size() >= 6U) return match(done[5]);
  if (done.size() == 2U) {
    if (const auto literal = match(done[1])) return literal;
  }
  return std::string_view{"stop"};
}
```

Replace the whole body of `EngineProtocolClient::read_generation` and add `next_event`:

```cpp
GenerationEvent EngineProtocolClient::next_event(const std::chrono::milliseconds timeout) {
  if (state_ != State::generating) return EngineProtocolError{EngineProtocolErrorCode::invalid_state, "no generation is active"};
  const auto response = transport_.read_line(timeout);
  if (response.kind == EngineReadKind::timeout) {
    return EngineProtocolError{EngineProtocolErrorCode::timeout, "engine generation read timed out"};
  }
  if (response.kind == EngineReadKind::closed) return *fail(EngineProtocolErrorCode::closed, "engine closed during generation");
  if (response.kind == EngineReadKind::failure) {
    return *fail(EngineProtocolErrorCode::transport_read_failed,
                 response.message.empty() ? "engine generation read failed" : response.message);
  }
  if (const auto validation = validate_incoming(response.line); validation.has_value()) {
    return *fail(validation->code, validation->message);
  }
  if (is_engine_fatal_line(response.line)) {
    return *fail(EngineProtocolErrorCode::fatal_engine_line, "engine reported a fatal verification failure");
  }
  if (response.line.rfind("T ", 0) == 0) {
    std::uint32_t token = 0;
    if (!parse_decimal(std::string_view(response.line).substr(2), token)) {
      return *fail(EngineProtocolErrorCode::malformed_line, "engine token line is invalid");
    }
    if (active_generation_.token_ids.size() >= limits_.max_generated_tokens) {
      return *fail(EngineProtocolErrorCode::response_limit, "engine exceeded generated-token limit");
    }
    active_generation_.token_ids.push_back(token);
    return TokenEvent{token};
  }
  if (response.line.rfind("PP ", 0) == 0) {
    // PP is allowed only for the startup-selected GLM wire profile: exactly
    // `PP <read> <total>` or `PP <read> <total> <milliseconds> <tokens-per-second>`.
    const auto progress = fields(response.line);
    std::uint32_t read = 0;
    std::uint32_t total = 0;
    const bool extended = progress.size() == 5U && nonnegative_decimal_metric(progress[3]) && nonnegative_decimal_metric(progress[4]);
    if (flavor_ != EngineProtocolFlavor::lighttransport_glm53 || (progress.size() != 3U && !extended) ||
        !parse_decimal(progress[1], read) || !parse_decimal(progress[2], total) || total == 0U || read > total) {
      return *fail(EngineProtocolErrorCode::unexpected_line, "engine emitted an invalid or unsupported prefill progress line");
    }
    return ProgressEvent{};
  }
  if (response.line.rfind("DONE ", 0) == 0 && response.line.size() > 5) {
    const auto finish = finish_literal(fields(response.line));
    if (!finish) return *fail(EngineProtocolErrorCode::malformed_line, "engine DONE line is invalid");
    active_generation_.done_line = response.line;
    state_ = State::idle;
    return DoneEvent{*finish, active_generation_.token_ids.size()};
  }
  if (response.line.rfind("ERR", 0) == 0 && (response.line.size() == 3 || response.line[3] == ' ')) {
    return *fail(EngineProtocolErrorCode::engine_error, response.line);
  }
  return *fail(EngineProtocolErrorCode::unexpected_line, "engine emitted an unexpected generation line");
}

std::optional<EngineProtocolError> EngineProtocolClient::read_generation(const std::chrono::milliseconds timeout,
                                                                          EngineGeneration& generation) {
  if (state_ != State::generating) return EngineProtocolError{EngineProtocolErrorCode::invalid_state, "no generation is active"};
  const auto deadline = deadline_after(timeout);
  for (;;) {
    const auto remaining = remaining_until(deadline);
    if (remaining.count() == 0) {
      generation = active_generation_;
      return EngineProtocolError{EngineProtocolErrorCode::timeout, "engine generation read timed out"};
    }
    auto event = next_event(remaining);
    if (auto* error = std::get_if<EngineProtocolError>(&event)) {
      generation = active_generation_;
      return std::move(*error);
    }
    if (const auto* done = std::get_if<DoneEvent>(&event)) {
      generation = active_generation_;
      // A nonfinite finish fails the request but not the protocol (SP0 E3).
      if (done->finish == "nonfinite") return EngineProtocolError{EngineProtocolErrorCode::engine_error, "engine reported a non-finite result"};
      return std::nullopt;
    }
  }
}
```
(`fail` returns `std::optional<EngineProtocolError>` that is always set; `*fail(...)` converts it into the variant.)

- [ ] **Step 4: Run**

Run: `cmake --build --preset linux-gcc && ctest --preset linux-gcc -R '^secure_frontend_(engine_protocol_client|chat_execution|engine_supervisor)_tests\.' --output-on-failure && ctest --preset linux-gcc`
Expected: PASS, including every existing `read_generation` case (the parse is shared now); total previous + 3. Cross-build; wine previous + 3.

- [ ] **Step 5: Handoff row and commit**

```bash
git add include/secure_frontend/engine_protocol_client.hpp src/engine_protocol_client.cpp tests/engine_protocol_client_test.cpp CMakeLists.txt ../docs/AGENT_HANDOFF.md
git -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit -m "feat: incremental engine protocol reads (SP3 §3.3)" \
  -m "next_event returns one T/PP/DONE record or a failure; read_generation is rebuilt on it. CTest: secure_frontend_engine_protocol_client_tests.next_event_yields_tokens_progress_and_done, .next_event_timeout_keeps_generation_active, .next_event_finish_forms_and_nonfinite_stays_idle RED: <line>. wine: <line>." -m "Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>" -m "Claude-Session: <session link>"
```

---

### Task 5: `drive_generation` — one generation loop; M1's `run_generation` rebuilt on it; timeout → `200 length` (spec §3.3, §3.4, B3, B4; R-C)

**Files:**
- Create: `include/secure_frontend/generation_run.hpp`, `src/generation_run.cpp`, `tests/support/chat_json.hpp`
- Modify: `include/secure_frontend/chat_execution.hpp`, `src/chat_execution.cpp` (M1's `ChatExecutionService::run_generation`), `src/subsystem_limits.cpp` (`chat_execution_limits`), `src/chat_route.cpp` (M1's `ChatRoute::handle`), `tests/chat_execution_test.cpp`, `tests/api_server_test.cpp`, `CMakeLists.txt`

**Interfaces:**
- Consumes: `next_event`, `DoneEvent`, `TokenEvent`, `ProgressEvent` (Task 4); `TokenByteDecoder`, `Utf8StreamDecoder` (Task 3); M1's `GenerationSettings`, `ExecutionControl`, `GenerationResult`, `GenerationFailure`, `GenerationOutcome`, `FinishReason`, `ChatExecutionService::run_generation`, `api_constants::kStreamPollSlice`, `api_constants::kShutdownDrainBudget`; `ExecutionControl::shutdown_requested` (R-H); M1's harness `ServerOptions::config_literals`.
- Produces:

```cpp
namespace secure_frontend {
enum class RunCancel { none, client_gone, write_stall, shutting_down };
enum class RunEnd { stop, length, cancelled, engine_error, engine_unavailable, undecodable };
struct GenerationRunLimits final {
  std::chrono::milliseconds generation_timeout{std::chrono::seconds(120)};
  std::chrono::milliseconds stop_drain_timeout{std::chrono::seconds(5)};
  std::size_t max_generated_bytes{1048576U};
  std::size_t max_tokens{0};                      // the GEN max_new; 0 = unchecked
  std::chrono::milliseconds slice{api_constants::kStreamPollSlice};
  std::chrono::milliseconds shutdown_drain{api_constants::kShutdownDrainBudget};   // drain cap when cancel == shutting_down
};
struct GenerationRunResult final {
  RunEnd end{RunEnd::engine_unavailable};
  std::size_t completion_tokens{0};
  bool timed_out{false};
  bool bytes_capped{false};
  RunCancel cancel{RunCancel::none};
};
class GenerationObserver {
 public:
  virtual void on_text(std::string_view text) = 0;     // complete UTF-8, never empty
  virtual RunCancel on_tick(bool progress) = 0;        // at least once per slice, and after every PP
  virtual ~GenerationObserver() = default;
};
[[nodiscard]] GenerationRunResult drive_generation(EngineProtocolClient&, const TokenByteDecoder&,
                                                   const GenerationRunLimits&, GenerationObserver&);
// chat_execution.hpp (M1 types amended):
//   GenerationResult gains        bool timed_out{false};          (B4: a timeout is a result with FinishReason::length)
//   GenerationFailure loses       timed_out
//   ChatExecutionLimits gains     std::size_t max_generated_bytes
// ChatExecutionService gains (M1's run_generation keeps its signature and becomes begin + drive_generation):
[[nodiscard]] std::optional<GenerationFailure> begin(const std::vector<std::uint32_t>& prompt_ids, const GenerationSettings& settings);
[[nodiscard]] GenerationRunLimits drive_limits(std::size_t max_tokens) const noexcept;
[[nodiscard]] EngineProtocolClient& protocol() noexcept;
}
```
`begin` adopts/awaits READY and writes `GEN` with M1's sampling keys (B6) — the stream path (Task 10) calls it too. Test support `tests/support/chat_json.hpp`: `parse_test_json`, `json_member`, `complete_text`, `complete_finish`, `error_code_of`, `delta_content`, `chunk_finish`, `chat_body`, `start_stream_server`, `file_has_line` (named so they never collide with M1's local `start_server(literals, engine)` and `chat_body` in `api_server_test.cpp`; always called qualified as `test_support::…`).

- [ ] **Step 1: Write the failing unit tests** — in `tests/chat_execution_test.cpp` (include `"secure_frontend/generation_run.hpp"`, `"support/test_tokenizer.hpp"`, `<mutex>`):

```cpp
std::string byte_id(const unsigned char byte) { return std::to_string(test_support::id_for_byte(byte)); }

// Thread-safe scripted transport (mandatory: a test thread appends while the loop reads).
class LockedTransport final : public secure_frontend::EngineLineTransport {
 public:
  void push(std::string line) { std::lock_guard lock(mutex_); reads_.push_back(std::move(line)); }
  [[nodiscard]] std::size_t stops() const { std::lock_guard lock(mutex_); return static_cast<std::size_t>(std::count(writes_.begin(), writes_.end(), "STOP")); }
  bool write_line(const std::string_view line, std::string&) override { std::lock_guard lock(mutex_); writes_.emplace_back(line); return true; }
  secure_frontend::EngineReadResult read_line(const std::chrono::milliseconds timeout) override {
    {
      std::lock_guard lock(mutex_);
      if (!reads_.empty()) {
        std::string next = std::move(reads_.front());
        reads_.pop_front();
        return {secure_frontend::EngineReadKind::line, std::move(next), {}};
      }
    }
    std::this_thread::sleep_for(std::min(timeout, std::chrono::milliseconds(2)));
    return {secure_frontend::EngineReadKind::timeout, {}, {}};
  }
 private:
  mutable std::mutex mutex_;
  std::deque<std::string> reads_;
  std::vector<std::string> writes_;
};

struct Collector final : secure_frontend::GenerationObserver {
  std::string text;
  int cancel_after_ticks{-1};
  int ticks{0};
  void on_text(const std::string_view piece) override { text += piece; }
  secure_frontend::RunCancel on_tick(bool) override {
    ++ticks;
    return cancel_after_ticks >= 0 && ticks > cancel_after_ticks ? secure_frontend::RunCancel::client_gone : secure_frontend::RunCancel::none;
  }
};

// A protocol already past READY and GEN, reading `records`.
struct Running final {
  LockedTransport transport;
  secure_frontend::EngineProtocolClient protocol{transport};
  explicit Running(const std::vector<std::string>& records) {
    transport.push("READY 512 stop");
    secure_frontend::EngineReady ready{};
    (void)protocol.await_ready(std::chrono::seconds(1), ready);
    (void)protocol.begin_generation(16, {1});
    for (const auto& record : records) transport.push(record);
  }
};

void drive_generation_bytes_cap_cuts_on_code_point() {
  const auto pack = test_support::byte_tokenizer();
  const secure_frontend::TokenByteDecoder decoder(pack);
  Running run({"T " + byte_id('A'), "T " + byte_id(0xE2), "T " + byte_id(0x82), "T " + byte_id(0xAC), "T " + byte_id('B'),
               "DONE 5 1 0 0 cancel"});
  Collector collector;
  const auto result = secure_frontend::drive_generation(run.protocol, decoder, {.max_generated_bytes = 2U, .max_tokens = 16U}, collector);
  expect(collector.text == "A", "the euro sign does not fit in the 1 byte left and is not split: [" + collector.text + "]");
  expect(result.end == secure_frontend::RunEnd::length && result.bytes_capped, "the cap ends with length");
  expect(run.transport.stops() == 1U, "the cap sends STOP once");
  expect(run.protocol.idle(), "drained to idle");
}

void drive_generation_drops_tail_on_length_and_replaces_on_stop() {
  const auto pack = test_support::byte_tokenizer();
  const secure_frontend::TokenByteDecoder decoder(pack);
  Running cut({"T " + byte_id('A'), "T " + byte_id(0xE2), "DONE 2 1 0 0 length"});
  Collector at_length;
  expect(secure_frontend::drive_generation(cut.protocol, decoder, {}, at_length).end == secure_frontend::RunEnd::length && at_length.text == "A",
         "length: the held lead byte is dropped");
  Running eos({"T " + byte_id('A'), "T " + byte_id(0xE2), "DONE 2 1 0 0 stop"});
  Collector at_stop;
  expect(secure_frontend::drive_generation(eos.protocol, decoder, {}, at_stop).end == secure_frontend::RunEnd::stop &&
             at_stop.text == "A\xEF\xBF\xBD", "stop: the held tail becomes one U+FFFD");
}

void drive_generation_cancel_sends_stop_and_drains() {
  const auto pack = test_support::byte_tokenizer();
  const secure_frontend::TokenByteDecoder decoder(pack);
  Running run({"T " + byte_id('A')});
  Collector collector;
  collector.cancel_after_ticks = 1;
  std::thread late([&run] {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    run.transport.push("DONE 1 1 0 0 cancel");
  });
  const auto result = secure_frontend::drive_generation(run.protocol, decoder, {}, collector);
  late.join();
  expect(result.end == secure_frontend::RunEnd::cancelled && result.cancel == secure_frontend::RunCancel::client_gone, "cancel drains to cancelled");
  expect(run.transport.stops() == 1U, "one STOP");
}

void chat_max_tokens_cut_mid_character_returns_length() {
  const auto pack = test_support::byte_tokenizer();
  LockedTransport transport;
  transport.push("READY 512 stop");
  secure_frontend::EngineProtocolClient protocol(transport);
  secure_frontend::ChatExecutionService service(protocol);
  for (const auto& record : {"T " + byte_id('A'), "T " + byte_id(0xE2), "T " + byte_id(0x82), std::string("DONE 3 1 0 0 length")}) transport.push(record);
  const auto outcome = service.run_generation({1U, 2U}, {.max_tokens = 3U}, {}, pack);
  const auto* result = std::get_if<secure_frontend::GenerationResult>(&outcome);
  expect(result != nullptr, "a response cut mid-character is not decode_failed (spec §3.3 latent bug)");
  if (result != nullptr) {
    expect(result->text == "A" && result->finish == secure_frontend::FinishReason::length && result->completion_tokens == 3U, "text so far, length");
  }
}
```
Register the four in `main()` and the `secure_frontend_add_cases(secure_frontend_chat_execution_tests` call (none of these names exists in M1: R-E).

Update M1's `run_generation_timeout_stops_and_reports_timed_out` (B4) — replace its first `expect` with:

```cpp
  const auto* result = std::get_if<secure_frontend::GenerationResult>(&outcome);
  expect(result != nullptr && result->timed_out && result->finish == secure_frontend::FinishReason::length,
         "a timeout returns the text so far with length (B4), flagged timed_out for the log");
```

- [ ] **Step 2: Write the failing end-to-end tests** — create `tests/support/chat_json.hpp`:

```cpp
#pragma once

// SP3 M2 test helpers: JSON reading of chat responses and SSE payloads, the
// chat body, a server started with fake-engine flags and config keys (M1's `config_literals`).
#include "secure_frontend/json.hpp"
#include "support/server_harness.hpp"
#include "support/test_support.hpp"

#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace test_support {

inline std::optional<secure_frontend::JsonValue> parse_test_json(const std::string_view text) {
  auto parsed = secure_frontend::parse_json(text, {.max_bytes = 16U << 20U, .max_depth = 8U, .max_container_entries = 4096U, .max_string_bytes = 16U << 20U});
  if (auto* value = std::get_if<secure_frontend::JsonValue>(&parsed)) return std::move(*value);
  return std::nullopt;
}

inline const secure_frontend::JsonValue* json_member(const secure_frontend::JsonValue& object, const std::string_view key) {
  const auto* members = object.if_object();
  if (members == nullptr) return nullptr;
  for (const auto& member : *members) {
    if (member.key == key) return &member.value;
  }
  return nullptr;
}

inline const secure_frontend::JsonValue* first_choice(const secure_frontend::JsonValue& root) {
  const auto* choices = json_member(root, "choices");
  if (choices == nullptr || choices->if_array() == nullptr || choices->if_array()->empty()) return nullptr;
  return &choices->if_array()->front();
}

inline std::optional<std::string> string_at(const secure_frontend::JsonValue* object, const std::string_view key) {
  const auto* value = object != nullptr ? json_member(*object, key) : nullptr;
  if (value == nullptr || value->if_string() == nullptr) return std::nullopt;
  return *value->if_string();
}

// choices[0].message.content / choices[0].finish_reason of a complete response.
inline std::string complete_text(const std::string_view body) {
  const auto root = parse_test_json(body);
  const auto* choice = root ? first_choice(*root) : nullptr;
  const auto* message = choice != nullptr ? json_member(*choice, "message") : nullptr;
  return string_at(message, "content").value_or("<no content>");
}
inline std::string complete_finish(const std::string_view body) {
  const auto root = parse_test_json(body);
  return string_at(root ? first_choice(*root) : nullptr, "finish_reason").value_or("<no finish>");
}
// error.code of an error body or of an SSE error payload.
inline std::string error_code_of(const std::string_view body) {
  const auto root = parse_test_json(body);
  return string_at(root ? json_member(*root, "error") : nullptr, "code").value_or("<no error>");
}
// For one SSE data payload: choices[0].delta.content, choices[0].finish_reason (null → nullopt).
inline std::optional<std::string> delta_content(const std::string_view data) {
  const auto root = parse_test_json(data);
  const auto* choice = root ? first_choice(*root) : nullptr;
  return string_at(choice != nullptr ? json_member(*choice, "delta") : nullptr, "content");
}
inline std::optional<std::string> chunk_finish(const std::string_view data) {
  const auto root = parse_test_json(data);
  return string_at(root ? first_choice(*root) : nullptr, "finish_reason");
}

// {"model":"fake-model","messages":[{"role":"user","content":"<content>"}],<extra>}
inline std::string chat_body(const std::string_view extra = "", const std::string_view content = "Hi") {
  std::string body = R"({"model":"fake-model","messages":[{"role":"user","content":")";
  body += content;
  body += "\"}]";
  if (!extra.empty()) {
    body += ',';
    body += extra;
  }
  body += '}';
  return body;
}

// The server with fake-engine flags and config keys (key -> JSON literal), ready to serve.
inline std::unique_ptr<ServerProcess> start_stream_server(std::vector<std::string> engine_args, std::map<std::string, std::string> config = {}) {
  ServerOptions options;
  options.engine_args = std::move(engine_args);
  options.config_literals = std::move(config);
  auto server = ServerProcess::start(options);
  if (server && !server->wait_for_health(kHealthReady, std::chrono::seconds(20))) server.reset();
  return server;
}

// True once `path` contains `line` as a whole line, polled every 20 ms.
inline bool file_has_line(const std::filesystem::path& path, const std::string_view line, const std::chrono::milliseconds wait) {
  const auto until = std::chrono::steady_clock::now() + wait;
  do {
    std::error_code ignored;
    if (std::filesystem::exists(path, ignored)) {
      const std::string text = "\n" + secure_frontend_test::read_file(path);
      if (text.find("\n" + std::string(line) + "\n") != std::string::npos) return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  } while (std::chrono::steady_clock::now() < until);
  return false;
}

}  // namespace test_support
```

In M1's `tests/api_server_test.cpp` add (include `"support/chat_json.hpp"` and `"support/test_tokenizer.hpp"`):

```cpp
void utf8_non_stream_max_tokens_cut_mid_character_returns_length() {
  auto server = test_support::start_stream_server({"--token-ids=" + test_support::token_ids_for("A\xE2\x82\xAC")});
  expect(server != nullptr, "the server becomes ready");
  if (!server) return;
  const auto result = server->post_chat(test_support::chat_body(R"("max_tokens":3)"), 10s);
  expect(result && result->status == 200, "a max_tokens cut mid-character is a 200, not a decode failure");
  if (result) {
    expect(test_support::complete_text(result->body) == "A", "the held lead bytes are dropped: " + result->body);
    expect(test_support::complete_finish(result->body) == "length", "finish_reason length");
  }
}

void generation_timeout_returns_text_with_length() {
  auto server = test_support::start_stream_server({"--tokens=100", "--token-interval-ms=300"}, {{"limits.engine.generation_timeout_s", "1"}});
  expect(server != nullptr, "the server becomes ready");
  if (!server) return;
  const auto result = server->post_chat(test_support::chat_body(R"("max_tokens":100)"), 10s);
  expect(result && result->status == 200, "a timed-out generation is a 200 (decision 4)");
  if (result) {
    const auto text = test_support::complete_text(result->body);
    expect(!text.empty() && text.size() <= 5U && text.find_first_not_of('H') == std::string::npos, "the text so far: " + text);
    expect(test_support::complete_finish(result->body) == "length", "finish_reason length");
  }
}
```
Register both in `main()`. In `CMakeLists.txt`, after M1's registration of `secure_frontend_api_server_tests`:

```cmake
    # SP3 M2 end-to-end cases (real sockets, ServerProcess): windows-runtime on Windows (R-J).
    target_link_libraries(secure_frontend_api_server_tests PRIVATE secure_frontend_json)
    set(SECURE_FRONTEND_API_SERVER_M2_CASES
        utf8_non_stream_max_tokens_cut_mid_character_returns_length generation_timeout_returns_text_with_length)
    secure_frontend_add_cases(secure_frontend_api_server_tests ${SECURE_FRONTEND_API_SERVER_M2_CASES}
        TIMEOUT 120 LABELS ${SECURE_FRONTEND_WINDOWS_RUNTIME})
```
(Later tasks append to the `set(SECURE_FRONTEND_API_SERVER_M2_CASES` line.)

- [ ] **Step 3: Run to see it fail**

Run: `cmake --build --preset linux-gcc 2>&1 | grep -m1 error`
Expected: FAIL `secure_frontend/generation_run.hpp: No such file or directory`. With the header stubbed in Step 4 but before Step 5, the e2e cases fail at runtime: `a max_tokens cut mid-character is a 200, not a decode failure` (M1: `decode_failed` → `500`) and `a timed-out generation is a 200 (decision 4)` (M1: `503 engine_error`). Paste both lines into the commit body.

- [ ] **Step 4: Implement `drive_generation`.** `include/secure_frontend/generation_run.hpp` — the first block of **Produces** with includes `"secure_frontend/api_constants.hpp"`, `"secure_frontend/engine_protocol_client.hpp"`, `"secure_frontend/tokenizer_pack.hpp"`, `<chrono>`, `<cstddef>`, `<string_view>`, and these comments on `RunEnd`: `stop` = DONE … stop; `length` = DONE … length, the byte cap, or the generation timeout (B4); `cancelled` = the observer cancelled (client gone, stall or shutdown) and STOP drained; `engine_error` = DONE … nonfinite (engine idle), ERR/fatal line (protocol closed), or more tokens than GEN allowed (abandoned); `engine_unavailable` = exit, protocol failure, a failed STOP write or a drain that missed its deadline (protocol closed); `undecodable` = a generated id that is not ordinary output, STOP drained.

`src/generation_run.cpp`:

```cpp
```cpp
#include "secure_frontend/generation_run.hpp"

#include "secure_frontend/utf8_stream_decoder.hpp"

#include <algorithm>
#include <string>
#include <variant>

namespace secure_frontend {
namespace {

using Clock = std::chrono::steady_clock;

std::chrono::milliseconds left_until(const Clock::time_point deadline) {
  const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
  return left.count() > 0 ? left : std::chrono::milliseconds(0);
}

// Longest prefix of valid UTF-8 `text` of at most `limit` bytes ending on a code-point boundary.
std::size_t code_point_prefix(const std::string_view text, std::size_t limit) {
  if (limit >= text.size()) return text.size();
  while (limit > 0U && (static_cast<unsigned char>(text[limit]) & 0xC0U) == 0x80U) --limit;
  return limit;
}

enum class StopCause { none, timeout, bytes, cancel, undecodable };

}  // namespace

GenerationRunResult drive_generation(EngineProtocolClient& protocol, const TokenByteDecoder& decoder,
                                   const GenerationRunLimits& limits, GenerationObserver& observer) {
  GenerationRunResult result;
  Utf8StreamDecoder utf8;
  std::size_t released = 0;
  StopCause cause = StopCause::none;
  const auto generation_deadline = Clock::now() + limits.generation_timeout;
  Clock::time_point drain_deadline{};
  auto last_tick = Clock::now();

  // false: STOP could not be written (the protocol is abandoned).
  const auto send_stop = [&](const StopCause why) {
    cause = why;
    // Shutdown (R-H): the drain is bounded by min(stop_drain, kShutdownDrainBudget), as M1's loop did.
    const bool shutting = why == StopCause::cancel && result.cancel == RunCancel::shutting_down;
    drain_deadline = Clock::now() + (shutting ? std::min(limits.stop_drain_timeout, limits.shutdown_drain) : limits.stop_drain_timeout);
    if (protocol.stop().has_value()) {
      protocol.abandon();
      return false;
    }
    return true;
  };
  // Releases decoded text within the byte cap; on reaching it, cuts on a code
  // point and (while the engine still generates) sends STOP.
  const auto release = [&](std::string text) {
    if (cause != StopCause::none || text.empty()) return true;
    if (released + text.size() > limits.max_generated_bytes) {
      text.resize(code_point_prefix(text, limits.max_generated_bytes - released));
      result.bytes_capped = true;
      if (!text.empty()) {
        released += text.size();
        observer.on_text(text);
      }
      if (!protocol.generating()) {
        cause = StopCause::bytes;
        return true;
      }
      return send_stop(StopCause::bytes);
    }
    released += text.size();
    observer.on_text(text);
    return true;
  };

  for (;;) {
    if (cause == StopCause::none && Clock::now() >= generation_deadline) {
      result.timed_out = true;
      if (!send_stop(StopCause::timeout)) return result;  // engine_unavailable
    }
    if (cause != StopCause::none && Clock::now() >= drain_deadline) {
      protocol.abandon();  // the engine ignored STOP: SP2 drain_failed
      return result;       // engine_unavailable
    }
    const auto budget = std::min(limits.slice, left_until(cause == StopCause::none ? generation_deadline : drain_deadline));
    auto event = protocol.next_event(std::max(budget, std::chrono::milliseconds(1)));
    bool progress = false;
    bool idle_slice = false;
    if (const auto* failure = std::get_if<EngineProtocolError>(&event)) {
      if (failure->code != EngineProtocolErrorCode::timeout) {
        result.end = failure->code == EngineProtocolErrorCode::engine_error || failure->code == EngineProtocolErrorCode::fatal_engine_line
            ? RunEnd::engine_error
            : RunEnd::engine_unavailable;
        return result;
      }
      idle_slice = true;
    } else if (const auto* token = std::get_if<TokenEvent>(&event)) {
      ++result.completion_tokens;
      if (limits.max_tokens != 0U && result.completion_tokens > limits.max_tokens) {
        protocol.abandon();  // more tokens than GEN allowed: not trusted further (M1: engine_error)
        result.end = RunEnd::engine_error;
        return result;
      }
      if (cause == StopCause::none) {
        const auto bytes = decoder.bytes_for(token->id);
        const bool ok = std::holds_alternative<std::string>(bytes) ? release(utf8.push(std::get<std::string>(bytes)))
                                                                   : send_stop(StopCause::undecodable);
        if (!ok) return result;  // engine_unavailable
      }
    } else if (std::holds_alternative<ProgressEvent>(event)) {
      progress = true;
    } else {
      const auto& done = std::get<DoneEvent>(event);
      if (cause == StopCause::none) (void)release(utf8.finish(done.finish != "stop"));
      if (done.finish == "nonfinite") {
        result.end = RunEnd::engine_error;
        return result;
      }
      switch (cause) {
        case StopCause::none: result.end = done.finish == "stop" ? RunEnd::stop : RunEnd::length; break;
        case StopCause::timeout:
        case StopCause::bytes: result.end = RunEnd::length; break;
        case StopCause::cancel: result.end = RunEnd::cancelled; break;
        case StopCause::undecodable: result.end = RunEnd::undecodable; break;
      }
      return result;
    }
    const auto now = Clock::now();
    if (progress || idle_slice || now - last_tick >= limits.slice) {
      last_tick = now;
      const RunCancel cancel = observer.on_tick(progress);
      if (cancel != RunCancel::none && cause == StopCause::none) {
        result.cancel = cancel;
        if (!send_stop(StopCause::cancel)) return result;  // engine_unavailable
      }
    }
  }
}

}  // namespace secure_frontend
```
```

CMake (after M1's chat-execution library block, anchor `add_library(secure_frontend_chat_execution`):

```cmake
secure_frontend_add_library(secure_frontend_generation_run SOURCES src/generation_run.cpp
    LINKS secure_frontend_engine_protocol_client secure_frontend_tokenizer_pack secure_frontend_utf8_stream secure_frontend_api_core)
target_link_libraries(secure_frontend_chat_execution PUBLIC secure_frontend_generation_run)
```

- [ ] **Step 5: Rebuild M1's `ChatExecutionService::run_generation` on it.** In `chat_execution.hpp`: include `"secure_frontend/generation_run.hpp"`, `<optional>`; add `bool timed_out{false};` as the last member of `GenerationResult`; delete `timed_out` from `enum class GenerationFailure`; add `std::size_t max_generated_bytes{static_cast<std::size_t>(config_defaults::kChatMaxGeneratedBytes)};` as the last member of `ChatExecutionLimits`; declare `begin`, `drive_limits`, `protocol()` (second block of **Produces**).

In `chat_execution.cpp`, replace the whole body of `ChatExecutionService::run_generation` (the function that starts `using Clock = std::chrono::steady_clock;` and contains `EngineGeneration generation{};` and `decode_token_ids(tokenizer, ids)` — not SP2's `execute`, which keeps its own `EngineGeneration generation{};`) with the three functions below, and add the observer in the anonymous namespace:

```cpp
namespace {
// Non-streaming observer: collects text; cancels on a gone client or on shutdown (R-H).
struct ControlObserver final : GenerationObserver {
  explicit ControlObserver(const ExecutionControl& control) : control_(control) {}
  void on_text(const std::string_view piece) override { text += piece; }
  RunCancel on_tick(bool) override {
    if ((control_.shutting_down != nullptr && control_.shutting_down->load()) || (control_.shutdown_requested && control_.shutdown_requested())) {
      return RunCancel::shutting_down;
    }
    if (control_.cancelled && control_.cancelled()) return RunCancel::client_gone;
    return RunCancel::none;
  }
  std::string text;
  const ExecutionControl& control_;
};
}  // namespace

std::optional<GenerationFailure> ChatExecutionService::begin(const std::vector<std::uint32_t>& prompt_ids, const GenerationSettings& settings) {
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
  return std::nullopt;
}

GenerationRunLimits ChatExecutionService::drive_limits(const std::size_t max_tokens) const noexcept {
  GenerationRunLimits limits;
  limits.generation_timeout = limits_.generation_timeout;
  limits.stop_drain_timeout = limits_.stop_drain_timeout;
  limits.max_generated_bytes = limits_.max_generated_bytes;
  limits.max_tokens = max_tokens;
  return limits;
}

EngineProtocolClient& ChatExecutionService::protocol() noexcept { return protocol_; }

GenerationOutcome ChatExecutionService::run_generation(const std::vector<std::uint32_t>& prompt_ids, const GenerationSettings& settings,
                                                       const ExecutionControl& control, const TokenizerPack& tokenizer) {
  if (const auto failure = begin(prompt_ids, settings)) return *failure;
  ControlObserver observer(control);
  const TokenByteDecoder decoder(tokenizer);
  const GenerationRunResult run = drive_generation(protocol_, decoder, drive_limits(settings.max_tokens), observer);
  const bool shutting = (control.shutting_down != nullptr && control.shutting_down->load()) ||
                        (control.shutdown_requested && control.shutdown_requested());
  switch (run.end) {
    case RunEnd::stop:
    case RunEnd::length:
      return GenerationResult{std::move(observer.text), prompt_ids.size(), run.completion_tokens,
                              run.end == RunEnd::stop ? FinishReason::stop : FinishReason::length, run.timed_out};
    case RunEnd::cancelled: return run.cancel == RunCancel::shutting_down ? GenerationFailure::shutting_down : GenerationFailure::cancelled;
    case RunEnd::engine_error: return GenerationFailure::engine_error;
    case RunEnd::undecodable: return GenerationFailure::decode_failed;
    case RunEnd::engine_unavailable: break;
  }
  return shutting ? GenerationFailure::shutting_down : GenerationFailure::engine_unavailable;
}
```
(Include `<limits>`. The shutdown check uses M1's `ExecutionControl::shutdown_requested`, which `ChatRoute` fills with `shutdown_signalled()` — R-H.)

`src/subsystem_limits.cpp`, function `chat_execution_limits`: add `limits.max_generated_bytes = c.limits.chat.max_generated_bytes;` next to the line that sets `default_max_tokens` (use that function's own variable names).

`src/chat_route.cpp`, in `ChatRoute::handle`: delete the whole `case GenerationFailure::timed_out: { … }` block (anchor `HttpResponse response = make_error_response(ApiError::engine_error);   // M1; decision B4 lands in M2`), and replace the final `return HttpResponse{.status = 200, .headers = {{"Content-Type", "application/json; charset=utf-8"}}, .body = *rendered};` with:

```cpp
  HttpResponse response{.status = 200, .headers = {{"Content-Type", "application/json; charset=utf-8"}}, .body = *rendered};
  if (result.timed_out) response.log_code = "generation_timeout";   // §8 log-only code; the client sees 200 length (B4)
  return response;
```

- [ ] **Step 6: Run**

Run: `cmake --build --preset linux-gcc && ctest --preset linux-gcc -R '^secure_frontend_(chat_execution|api_server|engine_supervisor|server)_tests\.' --output-on-failure && ctest --preset linux-gcc | tail -1`
Expected: PASS, including M1's six `run_generation_*` cases (the timeout case as amended) and SP2's `proc04_*`; total previous + 6. Cross-build; wine previous + 4 (the two e2e cases are `windows-runtime`).

- [ ] **Step 7: Handoff row and commit**

```bash
git add include/secure_frontend/generation_run.hpp src/generation_run.cpp include/secure_frontend/chat_execution.hpp src/chat_execution.cpp \
  src/subsystem_limits.cpp src/chat_route.cpp tests/support/chat_json.hpp tests/chat_execution_test.cpp tests/api_server_test.cpp CMakeLists.txt ../docs/AGENT_HANDOFF.md
git -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit -m "feat: one generation loop with timeout-to-length and byte cap; M1's run_generation rebuilt on it (SP3 §3.3, §3.4, B3, B4)" \
  -m "drive_generation: 20 ms slices, STOP on timeout/cap/cancel/shutdown/undecodable, drain (shutdown capped by kShutdownDrainBudget), UTF-8 tail rules; fixes the mid-character max_tokens cut; a timeout is 200 length. CTest: secure_frontend_chat_execution_tests.drive_generation_bytes_cap_cuts_on_code_point, .drive_generation_drops_tail_on_length_and_replaces_on_stop, .drive_generation_cancel_sends_stop_and_drains, .chat_max_tokens_cut_mid_character_returns_length; amended .run_generation_timeout_stops_and_reports_timed_out; secure_frontend_api_server_tests.utf8_non_stream_max_tokens_cut_mid_character_returns_length, .generation_timeout_returns_text_with_length. RED: <line>. wine: <line>." \
  -m "Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>" -m "Claude-Session: <session link>"
```

---

### Task 6: SSE encoder — events, code-point deltas, event cap, error event (spec §3.2, §3.4, §3.9)

**Files:**
- Create: `include/secure_frontend/sse_encoder.hpp`, `src/sse_encoder.cpp`
- Modify: `tests/sse_test.cpp`, `CMakeLists.txt`

**Interfaces:**
- Consumes: `append_json_string` (SP1, the one writer of §3.9), M1's `ApiError`/`api_error_row`, `api_constants::kMaxDeltaTextBytes`/`kMaxSseEventBytes` (M1 already defines both; M2 adds none). The completion id is M1's `make_completion_id()` (`chat_route.hpp`); the encoder only receives it in `SseIdentity`.
- Produces:

```cpp
namespace secure_frontend {
inline constexpr std::string_view kSseDoneEvent = "data: [DONE]\n\n";
inline constexpr std::string_view kSseKeepAliveComment = ": keep-alive\n\n";
inline constexpr std::string_view kChunkedTerminator = "0\r\n\r\n";
struct SseIdentity final { std::string id; std::int64_t created{0}; std::string model; };
[[nodiscard]] std::string render_role_event(const SseIdentity&);
[[nodiscard]] std::size_t next_delta_size(std::string_view text) noexcept;   // ≤ api_constants::kMaxDeltaTextBytes, code-point boundary
[[nodiscard]] std::string render_content_event(const SseIdentity&, std::string_view text);
[[nodiscard]] std::string render_final_event(const SseIdentity&, std::string_view finish_reason);
[[nodiscard]] std::string render_usage_event(const SseIdentity&, std::size_t prompt_tokens, std::size_t completion_tokens);
[[nodiscard]] std::string render_error_event(ApiError code);
[[nodiscard]] std::string frame_chunk(std::string_view bytes);              // "<hex>\r\n<bytes>\r\n"
}
```
CMake: library `secure_frontend_sse` (this task: `src/sse_encoder.cpp`).

- [ ] **Step 1: Write the failing tests** — add to `tests/sse_test.cpp` (include `"secure_frontend/api_constants.hpp"`, `"secure_frontend/sse_encoder.hpp"`, `<vector>`):

```cpp
const secure_frontend::SseIdentity kIdentity{"chatcmpl-0123456789abcdef0123456789abcdef", 1760000000, "fake-model"};

// True when the only LF bytes are the event's own "\n\n" terminator.
bool single_line_event(const std::string& event) {
  return event.size() >= 2U && event.compare(event.size() - 2U, 2U, "\n\n") == 0 && event.find('\n') == event.size() - 2U;
}

void sse_events_never_contain_raw_newline() {
  const std::string hostile = "a\nb\r\nc\x01\"\\\xE2\x80\xA8 end";
  const std::vector<std::string> events{
      secure_frontend::render_role_event(kIdentity), secure_frontend::render_content_event(kIdentity, hostile),
      secure_frontend::render_final_event(kIdentity, "stop"), secure_frontend::render_usage_event(kIdentity, 3, 4),
      secure_frontend::render_error_event(secure_frontend::ApiError::engine_error)};
  for (const auto& event : events) {
    expect(single_line_event(event), "one line per event: " + event);
    expect(event.rfind("data: {", 0) == 0, "every event is a data line");
    expect(event.find('\r') == std::string::npos, "no raw CR either");
  }
  expect(events[1].find(R"("content":"a\nb\r\nc\u0001\"\\)") != std::string::npos, "escaped by the shared writer: " + events[1]);
  expect(events[4] == R"(data: {"error":{"message":")" + std::string(secure_frontend::api_error_row(secure_frontend::ApiError::engine_error).message) +
                          R"(","type":"server_error","param":null,"code":"engine_error"}})" "\n\n",
         "OpenAI error shape with the fixed §7.2 message");
}

void sse_coalesced_control_heavy_text_respects_event_cap() {
  const std::string pattern = std::string("\"\\\x01\x1f") + "\xC3\xA9" + "\xF0\x9F\x98\x80" + "x";  // 4 escapes, 2-byte, 4-byte, ASCII
  std::string text;
  while (text.size() < 1048576U) text += pattern;
  std::string rebuilt;
  std::size_t events = 0;
  for (std::string_view rest = text; !rest.empty();) {
    const std::size_t size = secure_frontend::next_delta_size(rest);
    expect(size > 0U && size <= secure_frontend::api_constants::kMaxDeltaTextBytes, "delta within 16 KiB");
    secure_frontend::Utf8StreamDecoder check;
    const std::string_view delta = rest.substr(0, size);
    expect(check.push(delta) == delta && check.held_bytes() == 0U, "delta ends on a code-point boundary");
    const auto event = secure_frontend::render_content_event(kIdentity, delta);
    expect(event.size() <= secure_frontend::api_constants::kMaxSseEventBytes, "event within 128 KiB: " + std::to_string(event.size()));
    rebuilt += delta;
    rest.remove_prefix(size);
    ++events;
  }
  expect(rebuilt == text, "deltas concatenate to the text");
  expect(events >= 64U, "1 MiB is split into at least 64 deltas");
  const std::string worst(secure_frontend::api_constants::kMaxDeltaTextBytes, '\x01');
  expect(secure_frontend::render_content_event(kIdentity, worst).size() <= secure_frontend::api_constants::kMaxSseEventBytes, "6x escape growth fits the cap");
}

void sse_chunk_framing_units() {
  expect(secure_frontend::frame_chunk("data: [DONE]\n\n") == "e\r\ndata: [DONE]\n\n\r\n", "hex size, CRLF, bytes, CRLF");
  expect(secure_frontend::kChunkedTerminator == "0\r\n\r\n", "terminal chunk");
}
```
Register the three; append them to `set(SECURE_FRONTEND_SSE_CASES`; add `secure_frontend_sse` to the `secure_frontend_add_test_executable(secure_frontend_sse_tests` line.

- [ ] **Step 2: Run to see it fail**

Run: `cmake --build --preset linux-gcc`
Expected: build FAILS: `secure_frontend/sse_encoder.hpp: No such file or directory`.

- [ ] **Step 3: Implement.** `include/secure_frontend/sse_encoder.hpp` — the **Produces** block with includes `"secure_frontend/api_constants.hpp"`, `"secure_frontend/api_errors.hpp"`, `<cstddef>`, `<cstdint>`, `<optional>`, `<string>`, `<string_view>`, and the comment: "Every event is `data: <one-line JSON>\n\n`; text is escaped by SP1's `append_json_string` (spec §3.9), so no raw LF/CR appears inside an event. `id`, `created` and `model` are fixed per stream (§3.2)."

`src/sse_encoder.cpp`:

```cpp
#include "secure_frontend/sse_encoder.hpp"

#include "secure_frontend/json.hpp"

#include <algorithm>

namespace secure_frontend {
namespace {

constexpr std::string_view kChoiceTail = R"(,"logprobs":null,"finish_reason":null}]})" "\n\n";

std::string envelope(const SseIdentity& identity) {
  std::string out = R"(data: {"id":)";
  append_json_string(&out, identity.id);
  out += R"(,"object":"chat.completion.chunk","created":)";
  out += std::to_string(identity.created);
  out += R"(,"model":)";
  append_json_string(&out, identity.model);
  return out;
}

}  // namespace

std::string render_role_event(const SseIdentity& identity) {
  std::string out = envelope(identity);
  out += R"(,"choices":[{"index":0,"delta":{"role":"assistant","content":""})";
  out += kChoiceTail;
  return out;
}

std::size_t next_delta_size(const std::string_view text) noexcept {
  std::size_t size = std::min(text.size(), api_constants::kMaxDeltaTextBytes);
  if (size == text.size()) return size;
  while (size > 0U && (static_cast<unsigned char>(text[size]) & 0xC0U) == 0x80U) --size;
  return size;
}

std::string render_content_event(const SseIdentity& identity, const std::string_view text) {
  std::string out = envelope(identity);
  out += R"(,"choices":[{"index":0,"delta":{"content":)";
  append_json_string(&out, text);
  out += '}';
  out += kChoiceTail;
  return out;
}

std::string render_final_event(const SseIdentity& identity, const std::string_view finish_reason) {
  std::string out = envelope(identity);
  out += R"(,"choices":[{"index":0,"delta":{},"logprobs":null,"finish_reason":)";
  append_json_string(&out, finish_reason);
  out += "}]}\n\n";
  return out;
}

std::string render_usage_event(const SseIdentity& identity, const std::size_t prompt_tokens, const std::size_t completion_tokens) {
  std::string out = envelope(identity);
  out += R"(,"choices":[],"usage":{"prompt_tokens":)" + std::to_string(prompt_tokens) + R"(,"completion_tokens":)" +
         std::to_string(completion_tokens) + R"(,"total_tokens":)" + std::to_string(prompt_tokens + completion_tokens) + "}}\n\n";
  return out;
}

std::string render_error_event(const ApiError code) {
  const auto& row = api_error_row(code);
  std::string out = R"(data: {"error":{"message":)";
  append_json_string(&out, row.message);
  out += R"(,"type":)";
  append_json_string(&out, row.type);
  out += R"(,"param":null,"code":)";
  append_json_string(&out, row.code);
  out += "}}\n\n";
  return out;
}

std::string frame_chunk(const std::string_view bytes) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string size;
  for (std::size_t value = bytes.size(); value != 0U || size.empty(); value >>= 4U) size.insert(size.begin(), kHex[value & 0x0fU]);
  std::string out;
  out.reserve(size.size() + bytes.size() + 4U);
  out += size;
  out += "\r\n";
  out += bytes;
  out += "\r\n";
  return out;
}

}  // namespace secure_frontend
```

CMake:

```cmake
secure_frontend_add_library(secure_frontend_sse SOURCES src/sse_encoder.cpp
    LINKS secure_frontend_json secure_frontend_api_core secure_frontend_utf8_stream)
```


- [ ] **Step 4: Run** — `cmake --build --preset linux-gcc && ctest --preset linux-gcc -R '^secure_frontend_sse_tests\.' --output-on-failure && ctest --preset linux-gcc`. Expected: PASS; total previous + 3; wine previous + 3.

- [ ] **Step 5: Handoff row and commit**

```bash
git add include/secure_frontend/sse_encoder.hpp src/sse_encoder.cpp tests/sse_test.cpp CMakeLists.txt ../docs/AGENT_HANDOFF.md
git -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit -m "feat: SSE encoder with code-point deltas and event cap (SP3 §3.2)" \
  -m "CTest: secure_frontend_sse_tests.sse_events_never_contain_raw_newline, .sse_coalesced_control_heavy_text_respects_event_cap, .sse_chunk_framing_units. RED: <line>. wine: <line>." -m "Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>" -m "Claude-Session: <session link>"
```

---

### Task 7: `StreamSocket` seam, `ChunkedStreamSink`, `AcceptedSocketStream` (spec §2.7, §3.2, §10)

**Files:**
- Create: `include/secure_frontend/stream_socket.hpp`, `src/stream_socket.cpp`, `include/secure_frontend/chunked_stream_sink.hpp`, `src/chunked_stream_sink.cpp`
- Modify: `include/secure_frontend/request_router.hpp` (M1's `StreamSink`: `set_log_code`), `tests/sse_test.cpp`, `tests/http_socket_server_test.cpp`, `CMakeLists.txt`

**Interfaces:**
- Consumes: `StreamSink`, `HttpHead`, `serialize_head` (R-A); M1's `socket_io` (`NativeSocket`, `send_some`, `IoResult`/`IoStatus`, `poll_sockets`/`PollEntry`, `probe_peer`/`PeerState`, `set_non_blocking`, `set_no_delay`); `api_constants::kMaxSendChunk`, `api_constants::kStreamPollSlice`; `frame_chunk`, `kChunkedTerminator` (Task 6).
- Produces:

```cpp
namespace secure_frontend {
enum class SendStatus { progressed, would_block, failed };
struct SendStep final { SendStatus status; std::size_t sent; };
class StreamSocket {                                   // one connected socket, never blocking
 public:
  virtual SendStep send_some(std::string_view bytes) = 0;            // one send of ≤ api_constants::kMaxSendChunk bytes
  virtual bool wait_writable(std::chrono::milliseconds timeout) = 0; // poll for writability
  virtual bool peer_gone() = 0;                                      // peek: EOF, reset, or any stray byte
  virtual ~StreamSocket() = default;
};
class AcceptedSocketStream final : public StreamSocket {
 public:
  explicit AcceptedSocketStream(NativeSocket socket);   // sets non-blocking + TCP_NODELAY (M1 socket_io); does not own the socket
  ...overrides
};
struct StreamSinkTimeouts final {
  std::chrono::milliseconds write_stall{10000};
  std::function<bool()> shutting_down{};   // R-H: finish() stops waiting kShutdownDrainBudget after shutdown is seen
};
class ChunkedStreamSink final : public StreamSink {
 public:
  using Clock = std::chrono::steady_clock;
  ChunkedStreamSink(StreamSocket& socket, StreamSinkTimeouts timeouts, std::optional<std::string> allow_origin = std::nullopt,
                    std::function<Clock::time_point()> now = &Clock::now);
  bool send_headers(const HttpHead& head) override;   // adds the request's CORS headers (M1 rule), buffers the head, tries to send
  bool send_event(std::string_view event) override;   // frames one chunk, tries to send; false once failed
  bool writable(std::chrono::milliseconds wait) override;  // flushes up to `wait`; true when nothing is pending and not failed
  bool client_gone() override;                        // failed (send error or stall) or the peer probe says gone
  void finish() override;                             // terminal chunk, flushed under the stall rule
  [[nodiscard]] bool failed() const noexcept;
  [[nodiscard]] bool stalled() const noexcept;
  [[nodiscard]] std::size_t pending_bytes() const noexcept;
  void set_log_code(std::string_view code) override;  // M2's defaulted StreamSink virtual (Controller note 1)
  // The accessors M1's deliver_route_result reads from its sink:
  [[nodiscard]] int status() const noexcept;           // 0 until the head was queued
  [[nodiscard]] std::size_t bytes() const noexcept;    // bytes actually sent
  [[nodiscard]] bool finished() const noexcept;        // the terminal chunk was sent completely
  [[nodiscard]] std::string_view log_code() const noexcept;
};
[[nodiscard]] HttpHead make_sse_head();   // 200; Content-Type: text/event-stream; charset=utf-8; Transfer-Encoding: chunked; X-Accel-Buffering: no
}
```
The stall clock starts when pending output appears and restarts on every byte of progress; no progress for `write_stall` fails the sink (spec §2.7). Memory: at most one head or one event (≤ 128 KiB) plus framing is buffered; the pump never calls `send_event` unless `writable` returned true.

- [ ] **Step 1: Write the failing tests** — add to `tests/sse_test.cpp` (include `"secure_frontend/chunked_stream_sink.hpp"`, `<chrono>`, `<limits>`, `<optional>`):

```cpp
using namespace std::chrono_literals;

class FakeStreamSocket final : public secure_frontend::StreamSocket {
 public:
  std::string wire;
  std::size_t accept_budget{std::numeric_limits<std::size_t>::max()};
  bool fail_sends{false};
  bool gone{false};
  std::chrono::steady_clock::time_point clock{};
  secure_frontend::SendStep send_some(const std::string_view bytes) override {
    if (fail_sends) return {secure_frontend::SendStatus::failed, 0U};
    const std::size_t n = std::min(bytes.size(), accept_budget);
    if (n == 0U) return {secure_frontend::SendStatus::would_block, 0U};
    if (accept_budget != std::numeric_limits<std::size_t>::max()) accept_budget -= n;
    wire.append(bytes.substr(0, n));
    return {secure_frontend::SendStatus::progressed, n};
  }
  bool wait_writable(const std::chrono::milliseconds timeout) override {
    clock += timeout;
    return accept_budget > 0U;
  }
  bool peer_gone() override { return gone; }
};

secure_frontend::ChunkedStreamSink make_sink(FakeStreamSocket& socket, const std::chrono::milliseconds stall = 500ms) {
  return secure_frontend::ChunkedStreamSink(socket, {stall}, std::nullopt, [&socket] { return socket.clock; });
}

void sse_chunked_framing_and_terminal_chunk() {
  FakeStreamSocket socket;
  auto sink = make_sink(socket);
  expect(sink.send_headers(secure_frontend::make_sse_head()), "head sent");
  const auto head_end = socket.wire.find("\r\n\r\n");
  const std::string head = socket.wire.substr(0, head_end + 4U);
  expect(head.rfind("HTTP/1.1 200 ", 0) == 0, "status line: " + head);
  expect(head.find("\r\nContent-Type: text/event-stream; charset=utf-8\r\n") != std::string::npos, "SSE content type");
  expect(head.find("\r\nTransfer-Encoding: chunked\r\n") != std::string::npos, "chunked (decision 5)");
  expect(head.find("\r\nX-Accel-Buffering: no\r\n") != std::string::npos, "proxy buffering off");
  expect(head.find("Content-Length") == std::string::npos, "a stream has no Content-Length");
  expect(sink.send_event("data: a\n\n") && sink.send_event(secure_frontend::kSseDoneEvent), "events sent");
  sink.finish();
  expect(socket.wire.substr(head.size()) == "9\r\ndata: a\n\n\r\ne\r\ndata: [DONE]\n\n\r\n0\r\n\r\n", "one chunk per event, then 0\\r\\n\\r\\n");
  expect(sink.writable(0ms) && !sink.client_gone(), "a finished stream is clean");
  expect(sink.finished() && sink.status() == 200 && sink.bytes() == socket.wire.size(), "accessors deliver_route_result reads");
  FakeStreamSocket cors_socket;
  secure_frontend::ChunkedStreamSink cors(cors_socket, {500ms}, std::string("http://app.example:3000"), [&cors_socket] { return cors_socket.clock; });
  (void)cors.send_headers(secure_frontend::make_sse_head());
  expect(cors_socket.wire.find("\r\nAccess-Control-Allow-Origin: http://app.example:3000\r\n") != std::string::npos &&
             cors_socket.wire.find("\r\nVary: Origin\r\n") != std::string::npos, "the decision's CORS origin reaches the stream head");
  expect(socket.wire.find("Access-Control-Allow-Origin") == std::string::npos, "no CORS header without an allowed origin");
}

void sink_buffers_on_would_block_and_flushes_later() {
  FakeStreamSocket socket;
  auto sink = make_sink(socket);
  socket.accept_budget = 5U;
  expect(sink.send_event("data: hello\n\n"), "a partial send is not a failure");
  expect(!sink.writable(0ms) && sink.pending_bytes() > 0U, "the rest is pending");
  socket.accept_budget = std::numeric_limits<std::size_t>::max();
  expect(sink.writable(20ms) && sink.pending_bytes() == 0U, "flushed once the socket drains");
  expect(socket.wire == "d\r\ndata: hello\n\n\r\n", "bytes are complete and in order");
}

void sink_write_stall_marks_client_gone() {
  FakeStreamSocket socket;
  auto sink = make_sink(socket, 500ms);
  socket.accept_budget = 0U;
  (void)sink.send_event("data: x\n\n");
  for (int poll = 0; poll < 40 && !sink.failed(); ++poll) (void)sink.writable(20ms);
  expect(sink.failed() && sink.stalled() && sink.client_gone(), "no progress for write_stall fails the sink");
  const auto before = socket.wire.size();
  socket.accept_budget = std::numeric_limits<std::size_t>::max();
  sink.finish();
  expect(socket.wire.size() == before, "a failed sink sends no terminal chunk");
}

void sink_send_failure_and_peer_probe_are_gone() {
  FakeStreamSocket failing;
  auto broken = make_sink(failing);
  failing.fail_sends = true;
  expect(!broken.send_event("data: x\n\n") && broken.client_gone() && !broken.stalled(), "a send error is gone, not a stall");
  FakeStreamSocket closed;
  auto sink = make_sink(closed);
  closed.gone = true;
  expect(sink.client_gone(), "peer EOF, reset or a stray byte is gone");
}
```
Register the four; append to `set(SECURE_FRONTEND_SSE_CASES`.

OS seam test — add to `tests/http_socket_server_test.cpp` (it already opens loopback sockets; reuse its helpers if present, otherwise this self-contained version):

```cpp
// SP3 M2: AcceptedSocketStream over a real loopback connection (Winsock semantics are authoritative in MSYS2).
void stream_socket_peek_and_nonblocking_send() {
  using Native = secure_frontend::NativeSocket;
  const auto close_native = [](Native s) { secure_frontend::close_native_socket(s); };
  // listener on 127.0.0.1:0, one client, one accepted socket.
  Native listener = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  expect(bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 && listen(listener, 4) == 0, "listen");
  socklen_t length = sizeof(address);
  (void)getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length);
  const auto connected_pair = [&] {
    Native client = socket(AF_INET, SOCK_STREAM, 0);
    int small = 4096;
    (void)setsockopt(client, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&small), sizeof(small));
    expect(connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "connect");
    Native server = accept(listener, nullptr, nullptr);
    return std::pair<Native, Native>{client, server};
  };
  {
    auto [client, server] = connected_pair();
    secure_frontend::AcceptedSocketStream stream(server);
    expect(!stream.peer_gone(), "a quiet client is alive");
    expect(send(client, "x", 1, 0) == 1, "client sends a stray byte");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    expect(stream.peer_gone(), "a byte after the request is treated as gone (spec §2.6)");
    close_native(client);
    close_native(server);
  }
  {
    auto [client, server] = connected_pair();
    secure_frontend::AcceptedSocketStream stream(server);
    close_native(client);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    expect(stream.peer_gone(), "peer EOF or reset is gone");
    close_native(server);
  }
  {
    auto [client, server] = connected_pair();
    secure_frontend::AcceptedSocketStream stream(server);
    const std::string block(64U * 1024U, 'z');
    bool blocked = false;
    for (int round = 0; round < 4096 && !blocked; ++round) {
      const auto step = stream.send_some(block);
      expect(step.status != secure_frontend::SendStatus::failed, "send never fails on a live, non-reading peer");
      expect(step.sent <= block.size(), "at most api_constants::kMaxSendChunk per call");
      blocked = step.status == secure_frontend::SendStatus::would_block;
    }
    expect(blocked, "a non-reading peer makes a non-blocking send report would_block, never block");
    expect(!stream.wait_writable(std::chrono::milliseconds(50)), "not writable while the peer reads nothing");
    close_native(client);
    close_native(server);
  }
  close_native(listener);
}
```
(Use the file's existing platform includes; on Windows `socklen_t` is `int` — add `using socklen_t = int;` under `_WIN32` if the file lacks it.) Register it in `main()` and add a separate registration: `secure_frontend_add_cases(secure_frontend_http_socket_server_tests stream_socket_peek_and_nonblocking_send TIMEOUT 120 LABELS ${SECURE_FRONTEND_WINDOWS_RUNTIME})`; link `secure_frontend_sse` into `secure_frontend_http_socket_server_tests`.

- [ ] **Step 2: Run to see it fail** — `cmake --build --preset linux-gcc`. Expected: build FAILS: `secure_frontend/chunked_stream_sink.hpp: No such file or directory`.

- [ ] **Step 3: Implement.** `include/secure_frontend/stream_socket.hpp`: the `SendStatus`/`SendStep`/`StreamSocket`/`AcceptedSocketStream` declarations of **Produces** (includes `"secure_frontend/socket_io.hpp"`, `<chrono>`, `<cstddef>`, `<string_view>`; private member `NativeSocket socket_;`).

`src/stream_socket.cpp` — a thin adapter over M1's `socket_io` (no second copy of the OS calls):

```cpp
#include "secure_frontend/stream_socket.hpp"

#include "secure_frontend/api_constants.hpp"

#include <array>

namespace secure_frontend {

AcceptedSocketStream::AcceptedSocketStream(const NativeSocket socket) : socket_(socket) {
  (void)set_non_blocking(socket_);  // M1 already makes accepted sockets non-blocking; idempotent
  set_no_delay(socket_);            // spec §3.2: TCP_NODELAY for streams
}

SendStep AcceptedSocketStream::send_some(const std::string_view bytes) {
  const IoResult result = secure_frontend::send_some(socket_, bytes);  // ≤ kMaxSendChunk, EINTR retried (M1)
  switch (result.status) {
    case IoStatus::done: return {result.bytes > 0U ? SendStatus::progressed : SendStatus::would_block, result.bytes};
    case IoStatus::would_block: return {SendStatus::would_block, 0U};
    case IoStatus::closed:
    case IoStatus::failed: break;
  }
  return {SendStatus::failed, 0U};
}

bool AcceptedSocketStream::wait_writable(const std::chrono::milliseconds timeout) {
  std::array<PollEntry, 1> entry{PollEntry{socket_, false, true}};
  return poll_sockets(entry, timeout) > 0 && entry[0].writable && !entry[0].failed;
}

bool AcceptedSocketStream::peer_gone() { return probe_peer(socket_) == PeerState::gone; }  // EOF, reset or any byte (spec §2.6)

}  // namespace secure_frontend
```

In M1's `include/secure_frontend/request_router.hpp`, add to `struct StreamSink` (after `finish`): `// §8 log-only code of the stream (a fixed literal); M2. Default: ignored.` and `virtual void set_log_code(std::string_view /*code*/) {}`.

`include/secure_frontend/chunked_stream_sink.hpp`: the `StreamSinkTimeouts`/`ChunkedStreamSink`/`make_sse_head` declarations of **Produces** with includes `"secure_frontend/request_router.hpp"` (R-A seam types), `"secure_frontend/stream_socket.hpp"`, `<chrono>`, `<functional>`, `<optional>`, `<string>`, `<string_view>`, and private members:

```cpp
  bool flush(std::chrono::milliseconds wait);
  void append(std::string bytes);
  StreamSocket& socket_;
  StreamSinkTimeouts timeouts_;
  std::function<Clock::time_point()> now_;
  std::string outbound_;
  std::size_t offset_{0};
  Clock::time_point last_progress_{};
  bool failed_{false};
  bool stalled_{false};
  bool headers_sent_{false};
  std::size_t bytes_sent_{0};
  std::optional<std::string> allow_origin_;
  int status_{0};
  bool finished_{false};
  std::string_view log_code_{};
```

`src/chunked_stream_sink.cpp`:

```cpp
#include "secure_frontend/chunked_stream_sink.hpp"

#include "secure_frontend/api_constants.hpp"
#include "secure_frontend/sse_encoder.hpp"

#include <algorithm>
#include <utility>

namespace secure_frontend {

HttpHead make_sse_head() {
  HttpHead head;
  head.status = 200;
  head.headers = {{"Content-Type", "text/event-stream; charset=utf-8"}, {"Transfer-Encoding", "chunked"}, {"X-Accel-Buffering", "no"}};
  return head;  // Cache-Control, Connection: close and the §4.6 headers come from serialize_head
}

ChunkedStreamSink::ChunkedStreamSink(StreamSocket& socket, const StreamSinkTimeouts timeouts, std::optional<std::string> allow_origin,
                                     std::function<Clock::time_point()> now)
    : socket_(socket), timeouts_(timeouts), now_(std::move(now)), allow_origin_(std::move(allow_origin)) {}

void ChunkedStreamSink::append(std::string bytes) {
  if (outbound_.size() == offset_) last_progress_ = now_();  // the stall clock starts with new pending output
  outbound_ += bytes;
}

bool ChunkedStreamSink::flush(const std::chrono::milliseconds wait) {
  const auto until = now_() + wait;
  while (!failed_ && offset_ < outbound_.size()) {
    const auto chunk = std::string_view(outbound_).substr(offset_, std::min(api_constants::kMaxSendChunk, outbound_.size() - offset_));
    const SendStep step = socket_.send_some(chunk);
    if (step.status == SendStatus::progressed && step.sent > 0U) {
      offset_ += step.sent;
      bytes_sent_ += step.sent;
      last_progress_ = now_();
      continue;
    }
    if (step.status == SendStatus::failed) {
      failed_ = true;
      break;
    }
    const auto now = now_();
    if (now - last_progress_ >= timeouts_.write_stall) {
      failed_ = true;
      stalled_ = true;
      break;
    }
    if (now >= until) break;
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - now);
    (void)socket_.wait_writable(std::max(std::min(left, std::chrono::milliseconds(api_constants::kStreamPollSlice)), std::chrono::milliseconds(1)));
  }
  if (offset_ == outbound_.size()) {
    outbound_.clear();
    offset_ = 0U;
  }
  return !failed_ && outbound_.empty();
}

bool ChunkedStreamSink::send_headers(const HttpHead& head) {
  if (failed_ || headers_sent_) return false;
  headers_sent_ = true;
  HttpHead with_cors = head;  // the same CORS rule as M1's SocketStreamSink (spec §4.4: SSE follows complete responses)
  if (allow_origin_) {
    with_cors.headers.emplace_back("Access-Control-Allow-Origin", *allow_origin_);
    with_cors.headers.emplace_back("Vary", "Origin");
  }
  status_ = head.status;
  append(serialize_head(with_cors, std::chrono::system_clock::now()));
  (void)flush(std::chrono::milliseconds(0));
  return !failed_;
}

bool ChunkedStreamSink::send_event(const std::string_view event) {
  if (failed_) return false;
  append(frame_chunk(event));
  (void)flush(std::chrono::milliseconds(0));
  return !failed_;
}

bool ChunkedStreamSink::writable(const std::chrono::milliseconds wait) { return flush(wait); }

bool ChunkedStreamSink::client_gone() {
  if (!failed_ && socket_.peer_gone()) failed_ = true;
  return failed_;
}

void ChunkedStreamSink::finish() {
  if (failed_) return;
  append(std::string(kChunkedTerminator));
  std::optional<Clock::time_point> shutdown_seen;
  while (!failed_ && !outbound_.empty()) {  // bounded by the stall rule ...
    if (timeouts_.shutting_down && timeouts_.shutting_down()) {  // ... and, once shutdown is seen, by kShutdownDrainBudget (R-H)
      if (!shutdown_seen) shutdown_seen = now_();
      if (now_() >= *shutdown_seen + api_constants::kShutdownDrainBudget) return;  // cut: finished_ stays false
    }
    (void)flush(std::chrono::milliseconds(api_constants::kStreamPollSlice));
  }
  finished_ = !failed_ && outbound_.empty();
}

void ChunkedStreamSink::set_log_code(const std::string_view code) { log_code_ = code; }  // fixed literals only

bool ChunkedStreamSink::failed() const noexcept { return failed_; }
bool ChunkedStreamSink::stalled() const noexcept { return stalled_; }
std::size_t ChunkedStreamSink::pending_bytes() const noexcept { return outbound_.size() - offset_; }
int ChunkedStreamSink::status() const noexcept { return status_; }
std::size_t ChunkedStreamSink::bytes() const noexcept { return bytes_sent_; }
bool ChunkedStreamSink::finished() const noexcept { return finished_; }
std::string_view ChunkedStreamSink::log_code() const noexcept { return log_code_; }

}  // namespace secure_frontend
```

CMake: add `src/chunked_stream_sink.cpp src/stream_socket.cpp` to the `SOURCES` of `secure_frontend_add_library(secure_frontend_sse` and `secure_frontend_socket_io` to its `LINKS` (`secure_frontend_api_core`, which holds the head serializer, is already linked).

- [ ] **Step 4: Run** — `cmake --build --preset linux-gcc && ctest --preset linux-gcc -R '^secure_frontend_(sse|http_socket_server)_tests\.' --output-on-failure && ctest --preset linux-gcc`. Expected: PASS; total previous + 5. Cross-build; wine previous + 4 (the socket case is `windows-runtime`); also run it once under wine directly and record the line (smoke only).

- [ ] **Step 5: Handoff row and commit**

```bash
git add include/secure_frontend/request_router.hpp include/secure_frontend/stream_socket.hpp src/stream_socket.cpp include/secure_frontend/chunked_stream_sink.hpp src/chunked_stream_sink.cpp \
  tests/sse_test.cpp tests/http_socket_server_test.cpp CMakeLists.txt ../docs/AGENT_HANDOFF.md
git -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit -m "feat: chunked SSE sink with write-stall rule over non-blocking sockets (SP3 §2.7, §3.2)" \
  -m "CTest: secure_frontend_sse_tests.sse_chunked_framing_and_terminal_chunk, .sink_buffers_on_would_block_and_flushes_later, .sink_write_stall_marks_client_gone, .sink_send_failure_and_peer_probe_are_gone; secure_frontend_http_socket_server_tests.stream_socket_peek_and_nonblocking_send (windows-runtime). RED: <line>. wine: <line>." -m "Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>" -m "Claude-Session: <session link>"
```

---

### Task 8: `run_chat_stream` — the normal stream, coalescing and the tail deadline (spec §2.7, §3.2)

**Files:**
- Create: `include/secure_frontend/chat_stream.hpp`, `src/chat_stream.cpp`
- Modify: `tests/sse_test.cpp`, `CMakeLists.txt`

**Interfaces:**
- Consumes: `drive_generation`, `GenerationObserver`, `GenerationRunLimits`, `GenerationRunResult`, `RunEnd`, `RunCancel` (Task 5); encoder (Task 6); `make_sse_head` (Task 7); `StreamSink` (R-A); `api_constants::kStreamPollSlice`.
- Produces:

```cpp
namespace secure_frontend {
struct ChatStreamSettings final {
  GenerationRunLimits run{};
  std::chrono::milliseconds write_stall{10000};        // limits.http.write_stall_ms
  std::chrono::milliseconds tail_deadline{60000};      // limits.http.response_write_deadline_ms
  std::chrono::milliseconds keepalive{15000};          // limits.http.stream_keepalive_ms
  bool include_usage{false};
  std::size_t prompt_tokens{0};
};
struct ChatStreamOutcome final {
  RunEnd end{RunEnd::engine_unavailable};
  bool terminal_chunk_sent{false};
  std::string_view log_code{"-"};    // §8: "-", a §7.2 code, client_gone, write_stall or generation_timeout
};
// Called once, as soon as the engine finished (DONE, failure or cancel), before the tail is flushed.
// The protocol must not be used after it returns (the caller destroys its Lease there).
using GenerationEnded = std::function<void(const GenerationRunResult&)>;
[[nodiscard]] ChatStreamOutcome run_chat_stream(StreamSink& sink, EngineProtocolClient& protocol, const TokenByteDecoder& decoder,
                                                const SseIdentity& identity, const ChatStreamSettings& settings,
                                                const std::function<bool()>& shutting_down, const GenerationEnded& generation_ended);
}
```
Precondition: `GEN` was written (`ChatExecutionService::begin` succeeded). This task delivers the `stop`/`length` ends; other ends return without writing (a cut) until Tasks 11, 13 and 14.

- [ ] **Step 1: Write the failing tests** — add to `tests/sse_test.cpp` (include `"secure_frontend/chat_stream.hpp"`, `"support/test_tokenizer.hpp"`, `<algorithm>`, `<atomic>`, `<deque>`, `<mutex>`, `<thread>`; link `secure_frontend_chat_stream`):

```cpp
// Thread-safe (mandatory: test threads append while the pump reads). "<close>" reads as an engine exit.
class ScriptedTransport final : public secure_frontend::EngineLineTransport {
 public:
  void push(std::string line) {
    std::lock_guard lock(mutex_);
    lines_.push_back(std::move(line));
  }
  [[nodiscard]] std::size_t stop_count() const {
    std::lock_guard lock(mutex_);
    return static_cast<std::size_t>(std::count(writes_.begin(), writes_.end(), "STOP"));
  }
  bool write_line(const std::string_view line, std::string&) override {
    std::lock_guard lock(mutex_);
    writes_.emplace_back(line);
    return true;
  }
  secure_frontend::EngineReadResult read_line(const std::chrono::milliseconds timeout) override {
    std::string line;
    {
      std::lock_guard lock(mutex_);
      if (!lines_.empty()) {
        line = std::move(lines_.front());
        lines_.pop_front();
      }
    }
    if (line.empty()) {
      std::this_thread::sleep_for(std::min(timeout, std::chrono::milliseconds(5)));
      return {secure_frontend::EngineReadKind::timeout, {}, {}};
    }
    if (line == "<close>") return {secure_frontend::EngineReadKind::closed, {}, {}};
    return {secure_frontend::EngineReadKind::line, std::move(line), {}};
  }

 private:
  mutable std::mutex mutex_;
  std::deque<std::string> lines_;
  std::vector<std::string> writes_;
};

// Records accepted events; `blocked_polls` makes writable() report false that many times.
class RecordingSink final : public secure_frontend::StreamSink {
 public:
  std::vector<std::string> events;
  bool headers{false};
  bool finished{false};
  std::atomic<int> blocked_polls{0};
  std::atomic<bool> gone{false};
  bool violated{false};      // send_event after writable() returned false
  bool last_writable{true};
  bool send_headers(const secure_frontend::HttpHead& head) override { headers = head.status == 200; return true; }
  bool send_event(const std::string_view event) override {
    if (!last_writable) violated = true;
    events.emplace_back(event);
    return !gone;
  }
  bool writable(const std::chrono::milliseconds wait) override {
    if (blocked_polls > 0) {
      --blocked_polls;
      std::this_thread::sleep_for(wait);
      return last_writable = false;
    }
    return last_writable = !gone;
  }
  bool client_gone() override { return gone; }
  void finish() override { finished = true; }
};

struct StreamRun final {
  ScriptedTransport transport;
  secure_frontend::EngineProtocolClient protocol{transport};
  secure_frontend::TokenizerPack pack = test_support::byte_tokenizer();
  RecordingSink sink;
  int ended_calls{0};
  explicit StreamRun(std::vector<std::string> records) {
    transport.push("READY 4096 stop");
    secure_frontend::EngineReady ready{};
    (void)protocol.await_ready(std::chrono::seconds(1), ready);
    (void)protocol.begin_generation(64, {1, 2, 3});
    for (auto& record : records) transport.push(std::move(record));
  }
  secure_frontend::ChatStreamOutcome run(secure_frontend::ChatStreamSettings settings = {},
                                         std::function<bool()> shutting_down = [] { return false; }) {
    const secure_frontend::TokenByteDecoder decoder(pack);
    return secure_frontend::run_chat_stream(sink, protocol, decoder, kIdentity, settings, shutting_down,
                                            [this](const secure_frontend::GenerationRunResult&) { ++ended_calls; });
  }
  [[nodiscard]] std::string text() const {
    std::string all;
    for (const auto& event : sink.events) {
      const auto marker = event.find(R"("delta":{"content":")");
      if (marker == std::string::npos) continue;
      const auto start = marker + 20U;
      all += event.substr(start, event.find('"', start) - start);  // the tests use plain ASCII text
    }
    return all;
  }
};

std::string tok(const unsigned char byte) { return "T " + std::to_string(test_support::id_for_byte(byte)); }

void sse_role_content_final_done_sequence() {
  StreamRun stream({tok('H'), tok('i'), "DONE 2 3 1.0 1.0 stop"});
  const auto outcome = stream.run();
  const auto& events = stream.sink.events;
  expect(stream.sink.headers && events.size() >= 4U, "head, role, content, final, DONE");
  expect(events.front() == secure_frontend::render_role_event(kIdentity), "role chunk first");
  expect(events[events.size() - 2U] == secure_frontend::render_final_event(kIdentity, "stop"), "final chunk with stop");
  expect(events.back() == secure_frontend::kSseDoneEvent, "[DONE] last");
  expect(stream.text() == "Hi", "content deltas in order: " + stream.text());
  expect(stream.sink.finished && outcome.terminal_chunk_sent && outcome.end == secure_frontend::RunEnd::stop, "terminal chunk");
  expect(stream.ended_calls == 1 && !stream.sink.violated, "engine released once; never sent while unwritable");
}

void sse_include_usage_adds_usage_chunk() {
  StreamRun stream({tok('H'), tok('i'), "DONE 2 3 1.0 1.0 length"});
  secure_frontend::ChatStreamSettings settings;
  settings.include_usage = true;
  settings.prompt_tokens = 3U;
  (void)stream.run(settings);
  const auto& events = stream.sink.events;
  expect(events.size() >= 5U && events[events.size() - 2U] == secure_frontend::render_usage_event(kIdentity, 3U, 2U), "usage before [DONE]");
  expect(events[events.size() - 3U] == secure_frontend::render_final_event(kIdentity, "length"), "final chunk before usage");
  expect(events[events.size() - 2U].find(R"("choices":[],"usage":{"prompt_tokens":3,"completion_tokens":2,"total_tokens":5})") != std::string::npos,
         "OpenAI usage chunk shape");
}

void sse_ids_and_created_constant_per_stream() {
  StreamRun stream({tok('a'), tok('b'), tok('c'), "DONE 3 3 1.0 1.0 stop"});
  (void)stream.run();
  const std::string id = R"("id":")" + kIdentity.id + R"(","object":"chat.completion.chunk","created":1760000000,"model":"fake-model")";
  for (std::size_t index = 0; index + 1U < stream.sink.events.size(); ++index) {
    expect(stream.sink.events[index].find(id) != std::string::npos, "same id, created and model in every chunk");
  }
}

void stream_with_no_tokens_has_role_final_done() {
  StreamRun stream({"DONE 0 3 1.0 1.0 stop"});
  const auto outcome = stream.run();
  const auto& events = stream.sink.events;
  expect(events.size() == 3U && events[0] == secure_frontend::render_role_event(kIdentity) &&
             events[1] == secure_frontend::render_final_event(kIdentity, "stop") && events[2] == secure_frontend::kSseDoneEvent,
         "role, final stop, [DONE]; no empty content chunk");
  expect(outcome.terminal_chunk_sent, "a zero-token stream is a normal end");
}

void stream_slow_reader_coalesces_deltas() {
  std::vector<std::string> records;
  for (int index = 0; index < 200; ++index) records.push_back(tok('a'));
  records.emplace_back("DONE 200 3 1.0 1.0 stop");
  StreamRun stream(std::move(records));
  stream.sink.blocked_polls = 1000000;  // the reader takes nothing while the engine runs ...
  std::thread reader([&stream] {
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    stream.sink.blocked_polls = 0;    // ... then drains
  });
  const auto outcome = stream.run();
  reader.join();
  std::size_t content = 0;
  for (const auto& event : stream.sink.events) content += event.find(R"("delta":{"content")") != std::string::npos ? 1U : 0U;
  expect(stream.text() == std::string(200U, 'a'), "all text delivered");
  expect(content <= 2U, "200 tokens coalesce into at most 2 content events: " + std::to_string(content));
  expect(stream.ended_calls == 1 && outcome.terminal_chunk_sent && !stream.sink.violated, "normal end");
}

void stream_tail_deadline_cuts_without_terminal_chunk() {
  StreamRun stream({tok('a'), "DONE 1 3 1.0 1.0 stop"});
  stream.sink.blocked_polls = 1000000;
  secure_frontend::ChatStreamSettings settings;
  settings.tail_deadline = std::chrono::milliseconds(100);
  const auto outcome = stream.run(settings);
  expect(!outcome.terminal_chunk_sent && !stream.sink.finished, "tail deadline passed: no terminal chunk");
  expect(stream.ended_calls == 1, "the engine was released before the tail");
}
```
Register all six; append to `set(SECURE_FRONTEND_SSE_CASES`.

- [ ] **Step 2: Run to see it fail** — `cmake --build --preset linux-gcc`. Expected: build FAILS: `secure_frontend/chat_stream.hpp: No such file or directory`.

- [ ] **Step 3: Implement.** `include/secure_frontend/chat_stream.hpp`: the **Produces** block (includes `"secure_frontend/chunked_stream_sink.hpp"`, `"secure_frontend/generation_run.hpp"`, `"secure_frontend/sse_encoder.hpp"`, `<chrono>`, `<functional>`, `<string_view>`).

`src/chat_stream.cpp`:

```cpp
#include "secure_frontend/chat_stream.hpp"

#include "secure_frontend/api_constants.hpp"

#include <optional>
#include <string>

namespace secure_frontend {
namespace {

using Clock = std::chrono::steady_clock;

// Holds decoded text and renders events only when the sink can take one
// without blocking: the engine is read at engine speed (spec §2.7).
class StreamObserver final : public GenerationObserver {
 public:
  StreamObserver(StreamSink& sink, const SseIdentity& identity, const ChatStreamSettings& settings,
                 const std::function<bool()>& shutting_down)
      : sink_(sink), identity_(identity), settings_(settings), shutting_down_(shutting_down) {}

  void on_text(const std::string_view text) override { pending_ += text; }

  RunCancel on_tick(bool /*progress*/) override {
    flush_ready();
    return RunCancel::none;
  }

  // Sends content events while the sink is writable now.
  void flush_ready() {
    while (!pending_.empty() && sink_.writable(std::chrono::milliseconds(0))) send_one();
  }

  // Waits (in poll slices) until the sink can take an event; false on deadline or gone.
  bool wait_writable(const Clock::time_point until) {
    while (!sink_.writable(api_constants::kStreamPollSlice)) {
      if (Clock::now() >= until || sink_.client_gone()) return false;
    }
    return true;
  }

  bool flush_all(const Clock::time_point until) {
    while (!pending_.empty()) {
      if (!wait_writable(until)) return false;
      send_one();
    }
    return true;
  }

  bool send(const std::string_view event, const Clock::time_point until) { return wait_writable(until) && sink_.send_event(event); }

 private:
  void send_one() {
    const std::size_t size = next_delta_size(pending_);
    (void)sink_.send_event(render_content_event(identity_, std::string_view(pending_).substr(0, size)));
    pending_.erase(0, size);
  }

  StreamSink& sink_;
  const SseIdentity& identity_;
  const ChatStreamSettings& settings_;
  const std::function<bool()>& shutting_down_;
  std::string pending_;  // decoded text, bounded by limits.chat.max_generated_bytes
};

}  // namespace

ChatStreamOutcome run_chat_stream(StreamSink& sink, EngineProtocolClient& protocol, const TokenByteDecoder& decoder,
                                  const SseIdentity& identity, const ChatStreamSettings& settings,
                                  const std::function<bool()>& shutting_down, const GenerationEnded& generation_ended) {
  ChatStreamOutcome outcome;
  StreamObserver observer(sink, identity, settings, shutting_down);
  (void)sink.send_headers(make_sse_head());
  (void)sink.send_event(render_role_event(identity));
  const GenerationRunResult run = drive_generation(protocol, decoder, settings.run, observer);
  generation_ended(run);  // the Lease is gone from here on: the next generation may start
  outcome.end = run.end;
  if (run.timed_out) outcome.log_code = "generation_timeout";
  if (run.end != RunEnd::stop && run.end != RunEnd::length) return outcome;  // other ends: Tasks 11, 13, 14
  const auto until = Clock::now() + settings.tail_deadline;
  if (!observer.flush_all(until)) return outcome;
  if (!observer.send(render_final_event(identity, run.end == RunEnd::stop ? "stop" : "length"), until)) return outcome;
  if (settings.include_usage && !observer.send(render_usage_event(identity, settings.prompt_tokens, run.completion_tokens), until)) return outcome;
  if (!observer.send(kSseDoneEvent, until) || !observer.wait_writable(until)) return outcome;
  sink.finish();
  outcome.terminal_chunk_sent = sink.writable(std::chrono::milliseconds(0));
  return outcome;
}

}  // namespace secure_frontend
```

CMake:

```cmake
secure_frontend_add_library(secure_frontend_chat_stream SOURCES src/chat_stream.cpp
    LINKS secure_frontend_sse secure_frontend_generation_run)
```
Add `secure_frontend_chat_stream` to the `secure_frontend_add_test_executable(secure_frontend_sse_tests` line.

- [ ] **Step 4: Run** — `cmake --build --preset linux-gcc && ctest --preset linux-gcc -R '^secure_frontend_sse_tests\.' --output-on-failure && ctest --preset linux-gcc`. Expected: PASS; total previous + 6; wine previous + 6.

- [ ] **Step 5: Handoff row and commit**

```bash
git add include/secure_frontend/chat_stream.hpp src/chat_stream.cpp tests/sse_test.cpp CMakeLists.txt ../docs/AGENT_HANDOFF.md
git -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit -m "feat: chat stream pump with coalescing and tail deadline (SP3 §2.7, §3.2)" \
  -m "CTest: secure_frontend_sse_tests.sse_role_content_final_done_sequence, .sse_include_usage_adds_usage_chunk, .sse_ids_and_created_constant_per_stream, .stream_with_no_tokens_has_role_final_done, .stream_slow_reader_coalesces_deltas, .stream_tail_deadline_cuts_without_terminal_chunk RED: <line>. wine: <line>." -m "Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>" -m "Claude-Session: <session link>"
```

---

### Task 9: Loopback client library — `LoopbackAddress`, complete and streamed exchanges, chunked SSE reader with an event cap (spec §3.10; R-B)

**Files:**
- Create: `include/secure_frontend/loopback_address.hpp`, `src/loopback_address.cpp`, `include/secure_frontend/api_client.hpp`, `src/api_client.cpp`, `tests/api_client_test.cpp`
- Modify: `CMakeLists.txt` (new library `secure_frontend_api_client`, new test binary `secure_frontend_api_client_tests`)

**Interfaces:**
- Consumes: SP1's `IpAddress` (`family()`, `bytes()`, `scope()`, `canonical_text()`), `AddressScope::loopback`; M1's `socket_io` (`Socket`, `NativeSocket`, `set_non_blocking`, `poll_sockets`/`PollEntry`, `send_some`, `recv_some`, `IoStatus`, `WinsockGuard`); `api_constants::kMaxSseEventBytes`.
- Produces (M2 owns these — R-B; the spec §6.1 rule "the only `connect()` in the frontend sources is in `secure_frontend_api_client`" holds: M1 has none):

```cpp
namespace secure_frontend {
// Built only from an SP1 IpAddress the classifier reports as loopback (127.0.0.0/8, ::1, ::ffff:127.0.0.0/104).
// No conversion from IpAddress exists, so passing a remote address to the client is a compile error at the call site.
class LoopbackAddress final {
 public:
  [[nodiscard]] static std::optional<LoopbackAddress> from(const IpAddress& address);
  [[nodiscard]] const IpAddress& ip() const noexcept { return ip_; }
 private:
  explicit LoopbackAddress(IpAddress ip) : ip_(std::move(ip)) {}
  IpAddress ip_;
};
}
```
- Produces:

```cpp
namespace secure_frontend {
enum class SseItemKind { data, comment };
struct SseItem final { SseItemKind kind; std::string text; };   // data payload after "data: ", or comment text after ':'
enum class SseReadState { need_more, item, error };
class ChunkedSseReader final {                                   // pure: fed the raw body after the head
 public:
  explicit ChunkedSseReader(std::size_t max_event_bytes = api_constants::kMaxSseEventBytes);
  void feed(std::string_view bytes);
  [[nodiscard]] SseReadState next(SseItem& item);
  [[nodiscard]] bool terminal_chunk_seen() const noexcept;
  [[nodiscard]] bool done_seen() const noexcept;
};
struct ResponseHead final {
  int status{0};
  std::vector<std::pair<std::string, std::string>> headers;
  [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const;   // ASCII case-insensitive
};
[[nodiscard]] std::optional<ResponseHead> parse_response_head(std::string_view head);   // through the blank line
class LoopbackStreamConnection final {
 public:
  [[nodiscard]] static std::optional<LoopbackStreamConnection> open(const LoopbackAddress& address, std::uint16_t port,
                                                                   std::chrono::milliseconds timeout, int receive_buffer_bytes = 0);
  LoopbackStreamConnection(LoopbackStreamConnection&&) noexcept; LoopbackStreamConnection& operator=(LoopbackStreamConnection&&) noexcept;
  ~LoopbackStreamConnection();
  [[nodiscard]] bool send_all(std::string_view bytes, std::chrono::milliseconds timeout);
  [[nodiscard]] std::optional<std::string> receive_some(std::chrono::milliseconds timeout);   // nullopt: timeout or error; "" + eof(): EOF
  [[nodiscard]] bool eof() const noexcept;
  void close() noexcept;
};
struct ChatRequestOptions final {
  std::chrono::milliseconds timeout{std::chrono::seconds(30)};
  std::size_t max_event_bytes{api_constants::kMaxSseEventBytes};
  std::function<bool(const SseItem&)> on_item;                          // false → close now (cancel)
  std::vector<std::pair<std::string, std::string>> extra_headers;
  int receive_buffer_bytes{0};
};
enum class ChatExchangeEnd { complete, cut, cancelled, protocol_error, timeout, connect_failed };
struct ChatExchangeResult final {
  ChatExchangeEnd end{ChatExchangeEnd::connect_failed};
  ResponseHead head;
  bool streamed{false};                  // 200 + text/event-stream + chunked
  std::vector<SseItem> items;
  bool done_seen{false};
  bool terminal_chunk_seen{false};
  std::string body;                      // Content-Length body of a complete response
};
[[nodiscard]] std::string render_chat_request(const LoopbackAddress& address, std::uint16_t port, std::string_view bearer_token,
                                              std::string_view json_body,
                                              const std::vector<std::pair<std::string, std::string>>& extra_headers = {});
[[nodiscard]] ChatExchangeResult read_chat_exchange(LoopbackStreamConnection& connection, const ChatRequestOptions& options = {});
[[nodiscard]] ChatExchangeResult post_chat_completion(const LoopbackAddress& address, std::uint16_t port, std::string_view bearer_token,
                                                      std::string_view json_body, const ChatRequestOptions& options = {});
}
```
A stream is `complete` only with `[DONE]` **and** the terminal chunk; EOF before both is `cut`; a framing error or an event over `max_event_bytes` is `protocol_error`. A non-stream response is read by its `Content-Length` (the complete-response client).

- [ ] **Step 1: Write the failing tests** — `tests/api_client_test.cpp` (if M1 created it, add these cases to it):

```cpp
#include "secure_frontend/api_client.hpp"
#include "secure_frontend/config.hpp"
#include "support/test_support.hpp"

#include <atomic>
#include <cstdio>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <type_traits>

#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"
#include <winsock2.h>
#include <ws2tcpip.h>
using NativeSocket = SOCKET;
using socklen_t = int;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using NativeSocket = int;
#endif

namespace {

using namespace std::chrono_literals;
using secure_frontend::ChatExchangeEnd;
using secure_frontend::ChunkedSseReader;
using secure_frontend::SseItem;
using secure_frontend::SseReadState;

int failures = 0;
void expect(const bool condition, const std::string& message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
  }
}

void close_native(const NativeSocket socket) {
#if defined(_WIN32)
  closesocket(socket);
#else
  close(socket);
#endif
}

std::string chunk(const std::string& bytes) {
  char size[16];
  std::snprintf(size, sizeof(size), "%zx", bytes.size());
  return std::string(size) + "\r\n" + bytes + "\r\n";
}

const std::string kHead = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream; charset=utf-8\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n";
const std::string kGoodBody = chunk("data: {\"a\":1}\n\n") + chunk(": keep-alive\n\n") + chunk("data: [DONE]\n\n") + "0\r\n\r\n";

// One canned HTTP response on 127.0.0.1:<port>: reads the request head, writes `response`, closes.
struct CannedServer final {
  NativeSocket listener;
  std::uint16_t port{0};
  std::thread thread;
  explicit CannedServer(std::string response) {
    listener = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    (void)bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    (void)listen(listener, 1);
    socklen_t length = sizeof(address);
    (void)getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length);
    port = ntohs(address.sin_port);
    thread = std::thread([this, response = std::move(response)] {
      const NativeSocket client = accept(listener, nullptr, nullptr);
      std::string request;
      char buffer[4096];
      while (request.find("\r\n\r\n") == std::string::npos) {
        const auto got = recv(client, buffer, sizeof(buffer), 0);
        if (got <= 0) break;
        request.append(buffer, static_cast<std::size_t>(got));
      }
      (void)send(client, response.data(), static_cast<int>(response.size()), 0);
      close_native(client);
    });
  }
  ~CannedServer() {
    thread.join();
    close_native(listener);
  }
};

secure_frontend::LoopbackAddress loopback() {
  return *secure_frontend::LoopbackAddress::from(*secure_frontend::parse_listener_address("127.0.0.1"));
}

void client_reads_chunked_stream_and_requires_done_and_terminal_chunk() {
  {  // pure reader, fed one byte at a time
    ChunkedSseReader reader;
    std::vector<SseItem> items;
    for (const char byte : kGoodBody) {
      reader.feed(std::string_view(&byte, 1));
      SseItem item{};
      SseReadState state;
      while ((state = reader.next(item)) == SseReadState::item) items.push_back(item);
      expect(state != SseReadState::error, "valid framing never errors");
    }
    expect(items.size() == 3U && items[0].text == R"({"a":1})" && items[1].kind == secure_frontend::SseItemKind::comment &&
               items[2].text == "[DONE]", "data, comment, [DONE] in order");
    expect(reader.done_seen() && reader.terminal_chunk_seen(), "both end markers seen");
  }
  {
    ChunkedSseReader reader;
    reader.feed("zz\r\n");
    SseItem item{};
    expect(reader.next(item) == SseReadState::error, "a non-hex chunk size is a framing error");
  }
  const auto exchange = [](const std::string& body) {
    CannedServer server(kHead + body);
    return secure_frontend::post_chat_completion(loopback(), server.port, "token", "{}", {.timeout = 5s});
  };
  expect(exchange(kGoodBody).end == ChatExchangeEnd::complete, "[DONE] + terminal chunk = complete");
  expect(exchange(chunk("data: {\"a\":1}\n\n") + chunk("data: [DONE]\n\n")).end == ChatExchangeEnd::cut, "no terminal chunk = cut");
  expect(exchange(chunk("data: {\"a\":1}\n\n") + "0\r\n\r\n").end == ChatExchangeEnd::cut, "terminal chunk without [DONE] = cut");
  const auto plain = [] {
    CannedServer server("HTTP/1.1 503 Service Unavailable\r\nContent-Type: application/json\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}");
    return secure_frontend::post_chat_completion(loopback(), server.port, "token", "{}", {.timeout = 5s});
  }();
  expect(plain.end == ChatExchangeEnd::complete && !plain.streamed && plain.head.status == 503 && plain.body == "{}", "complete responses are read too");
}

void client_rejects_event_over_cap() {
  ChunkedSseReader reader(1024U);
  reader.feed(chunk("data: " + std::string(2000U, 'x') + "\n\n"));
  SseItem item{};
  expect(reader.next(item) == SseReadState::error, "one chunk larger than the cap");
  ChunkedSseReader split(1024U);
  for (int index = 0; index < 4; ++index) split.feed(chunk(index == 0 ? "data: " + std::string(400U, 'y') : std::string(400U, 'y')));
  expect(split.next(item) == SseReadState::error, "an event growing over the cap across chunks");
  CannedServer server(kHead + chunk("data: " + std::string(2000U, 'x') + "\n\n") + "0\r\n\r\n");
  const auto result = secure_frontend::post_chat_completion(loopback(), server.port, "token", "{}", {.timeout = 5s, .max_event_bytes = 1024U});
  expect(result.end == ChatExchangeEnd::protocol_error, "the client refuses an oversized event");
}

void client_accepts_only_loopback_address() {
  using secure_frontend::LoopbackAddress;
  using secure_frontend::parse_listener_address;
  expect(LoopbackAddress::from(*parse_listener_address("127.0.0.1")).has_value(), "127.0.0.1");
  expect(LoopbackAddress::from(*parse_listener_address("::1")).has_value(), "::1");
  expect(!LoopbackAddress::from(*parse_listener_address("172.16.1.10")).has_value(), "a LAN address is refused");
  expect(!LoopbackAddress::from(*parse_listener_address("0.0.0.0")).has_value(), "the wildcard is refused");
  static_assert(!std::is_invocable_v<decltype(&secure_frontend::post_chat_completion), const secure_frontend::IpAddress&, std::uint16_t,
                                     std::string_view, std::string_view, const secure_frontend::ChatRequestOptions&>,
                "a plain IpAddress is a compile error at the call site");
}

}  // namespace

int main(int argc, char* argv[]) {
#if defined(_WIN32)
  WSADATA data{};
  (void)WSAStartup(MAKEWORD(2, 2), &data);
#endif
  return secure_frontend_test::run_cases(argc, argv,
                                         {
                                             {"client_reads_chunked_stream_and_requires_done_and_terminal_chunk", client_reads_chunked_stream_and_requires_done_and_terminal_chunk},
                                             {"client_rejects_event_over_cap", client_rejects_event_over_cap},
                                             {"client_accepts_only_loopback_address", client_accepts_only_loopback_address},
                                         },
                                         failures);
}
```
CMake, in the tests block:

```cmake
    secure_frontend_add_test_executable(secure_frontend_api_client_tests tests/api_client_test.cpp secure_frontend_api_client)
    if(WIN32)
        target_link_libraries(secure_frontend_api_client_tests PRIVATE ws2_32)
    endif()
    # Real loopback sockets: windows-runtime on Windows.
    set(SECURE_FRONTEND_API_CLIENT_CASES client_reads_chunked_stream_and_requires_done_and_terminal_chunk client_rejects_event_over_cap
        client_accepts_only_loopback_address)
    secure_frontend_add_cases(secure_frontend_api_client_tests ${SECURE_FRONTEND_API_CLIENT_CASES} TIMEOUT 120 LABELS ${SECURE_FRONTEND_WINDOWS_RUNTIME})
```

- [ ] **Step 2: Run to see it fail** — `cmake --build --preset linux-gcc`. Expected: build FAILS: `secure_frontend/api_client.hpp: No such file or directory`.

- [ ] **Step 3: Implement.** Header `include/secure_frontend/api_client.hpp`: the **Produces** block, includes `"secure_frontend/loopback_address.hpp"`, `"secure_frontend/api_constants.hpp"`, `<chrono>`, `<cstdint>`, `<functional>`, `<optional>`, `<string>`, `<string_view>`, `<utility>`, `<vector>`; `ChunkedSseReader` private members:

```cpp
  bool decode_chunks();
  std::size_t max_event_bytes_;
  std::string raw_;
  std::string events_;
  std::size_t chunk_left_{0};
  bool need_crlf_{false};
  bool awaiting_terminal_crlf_{false};
  bool terminal_{false};
  bool done_{false};
  bool failed_{false};
```
`LoopbackStreamConnection` private: `explicit LoopbackStreamConnection(Socket socket) noexcept; Socket socket_; bool eof_{false};` (M1's RAII `Socket`, include `"secure_frontend/socket_io.hpp"`).

`src/api_client.cpp`:

```cpp
#include "secure_frontend/api_client.hpp"

#include "secure_frontend/socket_io.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>

#if defined(_WIN32)
#include <ws2tcpip.h>
using socklen_t = int;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace secure_frontend {
namespace {

using Clock = std::chrono::steady_clock;
constexpr std::size_t kMaxHeadBytes = 16U * 1024U;
constexpr std::size_t kMaxCompleteBodyBytes = 16U * 1024U * 1024U;

bool equals_ascii_ci(const std::string_view a, const std::string_view b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
           return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
         });
}

int hex_value(const char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Winsock stays started for the process (WSAStartup is reference-counted); a no-op on POSIX.
void ensure_winsock() {
  static const WinsockGuard guard;
  (void)guard;
}

bool wait_for(const NativeSocket socket, const bool read, const std::chrono::milliseconds timeout) {
  std::array<PollEntry, 1> entry{PollEntry{socket, read, !read}};
  return poll_sockets(entry, timeout) > 0 && (read ? (entry[0].readable || entry[0].failed) : entry[0].writable);
}

}  // namespace

// ---- ChunkedSseReader ----------------------------------------------------

ChunkedSseReader::ChunkedSseReader(const std::size_t max_event_bytes) : max_event_bytes_(max_event_bytes) {}

void ChunkedSseReader::feed(const std::string_view bytes) { raw_ += bytes; }

bool ChunkedSseReader::terminal_chunk_seen() const noexcept { return terminal_; }
bool ChunkedSseReader::done_seen() const noexcept { return done_; }

// Moves complete chunk payload bytes from raw_ to events_; false on a framing error.
bool ChunkedSseReader::decode_chunks() {
  for (;;) {
    if (terminal_) return raw_.empty();  // nothing may follow 0\r\n\r\n
    if (chunk_left_ > 0U) {
      const std::size_t take = std::min(chunk_left_, raw_.size());
      events_.append(raw_, 0, take);
      raw_.erase(0, take);
      chunk_left_ -= take;
      if (chunk_left_ > 0U) return true;
      need_crlf_ = true;
    }
    if (need_crlf_ || awaiting_terminal_crlf_) {
      if (raw_.size() < 2U) return true;
      if (raw_.compare(0, 2, "\r\n") != 0) return false;
      raw_.erase(0, 2);
      if (awaiting_terminal_crlf_) terminal_ = true;
      need_crlf_ = false;
      awaiting_terminal_crlf_ = false;
      continue;
    }
    const auto end = raw_.find("\r\n");
    if (end == std::string::npos) return raw_.size() <= 10U;
    if (end == 0U || end > 8U) return false;
    std::size_t size = 0U;
    for (std::size_t index = 0; index < end; ++index) {
      const int digit = hex_value(raw_[index]);
      if (digit < 0) return false;
      size = size * 16U + static_cast<std::size_t>(digit);
    }
    if (size > max_event_bytes_) return false;
    raw_.erase(0, end + 2U);
    if (size == 0U) {
      awaiting_terminal_crlf_ = true;  // no trailers
      continue;
    }
    chunk_left_ = size;
  }
}

SseReadState ChunkedSseReader::next(SseItem& item) {
  if (failed_) return SseReadState::error;
  if (!decode_chunks()) {
    failed_ = true;
    return SseReadState::error;
  }
  const auto end = events_.find("\n\n");
  if (end == std::string::npos) {
    if (events_.size() > max_event_bytes_ || (terminal_ && !events_.empty())) failed_ = true;
    return failed_ ? SseReadState::error : SseReadState::need_more;
  }
  if (end + 2U > max_event_bytes_) {
    failed_ = true;
    return SseReadState::error;
  }
  const std::string event = events_.substr(0, end);
  events_.erase(0, end + 2U);
  if (!event.empty() && event.front() == ':') {
    item = {SseItemKind::comment, event.substr(event.size() > 1U && event[1] == ' ' ? 2U : 1U)};
    return SseReadState::item;
  }
  if (done_ || event.rfind("data: ", 0) != 0 || event.find('\n') != std::string::npos) {
    failed_ = true;
    return SseReadState::error;
  }
  item = {SseItemKind::data, event.substr(6)};
  if (item.text == "[DONE]") done_ = true;
  return SseReadState::item;
}

// ---- head ------------------------------------------------------------------

std::optional<std::string_view> ResponseHead::header(const std::string_view name) const {
  for (const auto& [key, value] : headers) {
    if (equals_ascii_ci(key, name)) return std::string_view(value);
  }
  return std::nullopt;
}

std::optional<ResponseHead> parse_response_head(const std::string_view head) {
  if (head.rfind("HTTP/1.1 ", 0) != 0 || head.size() < 12U) return std::nullopt;
  ResponseHead parsed;
  for (std::size_t index = 9; index < 12; ++index) {
    if (!std::isdigit(static_cast<unsigned char>(head[index]))) return std::nullopt;
    parsed.status = parsed.status * 10 + (head[index] - '0');
  }
  std::size_t line = head.find("\r\n");
  while (line != std::string_view::npos) {
    const std::size_t start = line + 2U;
    const std::size_t end = head.find("\r\n", start);
    if (end == std::string_view::npos || end == start) break;
    const auto field = head.substr(start, end - start);
    const auto colon = field.find(':');
    if (colon == std::string_view::npos) return std::nullopt;
    auto value = field.substr(colon + 1U);
    while (!value.empty() && value.front() == ' ') value.remove_prefix(1);
    parsed.headers.emplace_back(std::string(field.substr(0, colon)), std::string(value));
    line = end;
  }
  return parsed;
}

// ---- connection --------------------------------------------------------------

LoopbackStreamConnection::LoopbackStreamConnection(Socket socket) noexcept : socket_(std::move(socket)) {}
LoopbackStreamConnection::LoopbackStreamConnection(LoopbackStreamConnection&&) noexcept = default;
LoopbackStreamConnection& LoopbackStreamConnection::operator=(LoopbackStreamConnection&&) noexcept = default;
LoopbackStreamConnection::~LoopbackStreamConnection() = default;
void LoopbackStreamConnection::close() noexcept { socket_.reset(); }
bool LoopbackStreamConnection::eof() const noexcept { return eof_; }

std::optional<LoopbackStreamConnection> LoopbackStreamConnection::open(const LoopbackAddress& address, const std::uint16_t port,
                                                                       const std::chrono::milliseconds timeout, const int receive_buffer_bytes) {
  ensure_winsock();
  const IpAddress& ip = address.ip();
  sockaddr_storage storage{};
  socklen_t length = 0;
  if (ip.family() == IpFamily::v4) {
    auto* v4 = reinterpret_cast<sockaddr_in*>(&storage);
    v4->sin_family = AF_INET;
    v4->sin_port = htons(port);
    std::memcpy(&v4->sin_addr, ip.bytes().data(), 4);
    length = sizeof(sockaddr_in);
  } else {
    auto* v6 = reinterpret_cast<sockaddr_in6*>(&storage);
    v6->sin6_family = AF_INET6;
    v6->sin6_port = htons(port);
    std::memcpy(&v6->sin6_addr, ip.bytes().data(), 16);
    length = sizeof(sockaddr_in6);
  }
  Socket socket(::socket(storage.ss_family, SOCK_STREAM, IPPROTO_TCP));
  if (!socket.valid() || !set_non_blocking(socket.get())) return std::nullopt;
  if (receive_buffer_bytes > 0) {
    (void)setsockopt(socket.get(), SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&receive_buffer_bytes), sizeof(receive_buffer_bytes));
  }
  // The only connect() in the frontend sources (spec §6.1); the address is loopback by construction.
  (void)::connect(socket.get(), reinterpret_cast<const sockaddr*>(&storage), length);  // completes asynchronously
  int error = 0;
  socklen_t error_length = sizeof(error);
  if (!wait_for(socket.get(), false, timeout) ||
      getsockopt(socket.get(), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &error_length) != 0 || error != 0) {
    return std::nullopt;
  }
  return LoopbackStreamConnection(std::move(socket));
}

bool LoopbackStreamConnection::send_all(std::string_view bytes, const std::chrono::milliseconds timeout) {
  const auto until = Clock::now() + timeout;
  while (!bytes.empty()) {
    const IoResult sent = send_some(socket_.get(), bytes);  // M1: ≤ kMaxSendChunk, EINTR retried
    if (sent.status == IoStatus::done && sent.bytes > 0U) {
      bytes.remove_prefix(sent.bytes);
      continue;
    }
    if (sent.status != IoStatus::would_block && sent.status != IoStatus::done) return false;
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - Clock::now());
    if (left.count() <= 0) return false;
    (void)wait_for(socket_.get(), false, std::min(left, std::chrono::milliseconds(50)));
  }
  return true;
}

std::optional<std::string> LoopbackStreamConnection::receive_some(const std::chrono::milliseconds timeout) {
  if (eof_) return std::string();
  if (!wait_for(socket_.get(), true, timeout)) return std::nullopt;
  char buffer[65536];
  const IoResult got = recv_some(socket_.get(), buffer, sizeof(buffer));
  if (got.status == IoStatus::closed) {
    eof_ = true;
    return std::string();
  }
  if (got.status != IoStatus::done) return std::nullopt;
  return std::string(buffer, got.bytes);
}

// ---- exchange ----------------------------------------------------------------

std::string render_chat_request(const LoopbackAddress& address, const std::uint16_t port, const std::string_view bearer_token,
                                const std::string_view json_body, const std::vector<std::pair<std::string, std::string>>& extra_headers) {
  const IpAddress& ip = address.ip();
  const std::string host = ip.family() == IpFamily::v6 ? "[" + ip.canonical_text() + "]" : ip.canonical_text();
  std::string request = "POST /v1/chat/completions HTTP/1.1\r\nHost: " + host + ":" + std::to_string(port) + "\r\n";
  request += "Authorization: Bearer ";
  request += bearer_token;
  request += "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(json_body.size()) + "\r\n";
  for (const auto& [name, value] : extra_headers) request += name + ": " + value + "\r\n";
  request += "\r\n";
  request += json_body;
  return request;
}

ChatExchangeResult read_chat_exchange(LoopbackStreamConnection& connection, const ChatRequestOptions& options) {
  ChatExchangeResult result;
  result.end = ChatExchangeEnd::timeout;
  const auto until = Clock::now() + options.timeout;
  std::string head;
  std::string body;
  std::optional<ChunkedSseReader> reader;
  std::size_t content_length = 0U;
  for (;;) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - Clock::now());
    if (left.count() <= 0) return result;
    const auto bytes = connection.receive_some(std::min(left, std::chrono::milliseconds(50)));
    if (!bytes) continue;
    if (bytes->empty() && connection.eof()) {
      if (reader) {
        result.end = reader->done_seen() && reader->terminal_chunk_seen() ? ChatExchangeEnd::complete : ChatExchangeEnd::cut;
      } else {
        result.end = !head.empty() && head.find("\r\n\r\n") != std::string::npos && body.size() == content_length ? ChatExchangeEnd::complete
                                                                                                                 : ChatExchangeEnd::cut;
        result.body = std::move(body);
      }
      return result;
    }
    std::string_view incoming = *bytes;
    if (result.head.status == 0) {
      head += incoming;
      const auto end = head.find("\r\n\r\n");
      if (end == std::string::npos) {
        if (head.size() > kMaxHeadBytes) { result.end = ChatExchangeEnd::protocol_error; return result; }
        continue;
      }
      const auto parsed = parse_response_head(std::string_view(head).substr(0, end + 4U));
      if (!parsed) { result.end = ChatExchangeEnd::protocol_error; return result; }
      result.head = *parsed;
      const auto type = result.head.header("Content-Type");
      const auto coding = result.head.header("Transfer-Encoding");
      result.streamed = result.head.status == 200 && type && type->rfind("text/event-stream", 0) == 0 && coding && equals_ascii_ci(*coding, "chunked");
      if (result.streamed) {
        reader.emplace(options.max_event_bytes);
      } else {
        const auto length = result.head.header("Content-Length");
        if (!length || length->empty() || length->size() > 9U || length->find_first_not_of("0123456789") != std::string_view::npos) {
          result.end = ChatExchangeEnd::protocol_error;
          return result;
        }
        content_length = static_cast<std::size_t>(std::stoul(std::string(*length)));
        if (content_length > kMaxCompleteBodyBytes) { result.end = ChatExchangeEnd::protocol_error; return result; }
      }
      const std::string rest = head.substr(end + 4U);
      head.resize(end + 4U);
      if (rest.empty()) continue;
      body.clear();
      incoming = std::string_view();
      if (reader) reader->feed(rest); else body += rest;
    }
    if (reader) {
      reader->feed(incoming);
      SseItem item{};
      SseReadState state;
      while ((state = reader->next(item)) == SseReadState::item) {
        if (item.kind == SseItemKind::data && item.text == "[DONE]") result.done_seen = true;
        result.items.push_back(item);
        if (options.on_item && !options.on_item(item)) {
          connection.close();
          result.end = ChatExchangeEnd::cancelled;
          return result;
        }
      }
      result.terminal_chunk_seen = reader->terminal_chunk_seen();
      if (state == SseReadState::error) { result.end = ChatExchangeEnd::protocol_error; return result; }
    } else {
      body += incoming;
      if (body.size() > content_length) { result.end = ChatExchangeEnd::protocol_error; return result; }
    }
  }
}

ChatExchangeResult post_chat_completion(const LoopbackAddress& address, const std::uint16_t port, const std::string_view bearer_token,
                                        const std::string_view json_body, const ChatRequestOptions& options) {
  auto connection = LoopbackStreamConnection::open(address, port, std::min(options.timeout, std::chrono::milliseconds(5000)),
                                                   options.receive_buffer_bytes);
  if (!connection) return {};
  if (!connection->send_all(render_chat_request(address, port, bearer_token, json_body, options.extra_headers), options.timeout)) {
    ChatExchangeResult failed;
    failed.end = ChatExchangeEnd::cut;
    return failed;
  }
  return read_chat_exchange(*connection, options);
}

}  // namespace secure_frontend
```
Note: in the `rest` branch, the `rest` bytes are fed and `incoming` cleared so they are not fed twice; for a `rest` that contains the whole stream, the `if (reader)` block below still runs with an empty `incoming` and drains the items. On Windows `POLLHUP`/`POLLERR` exist in `winsock2.h`.

`include/secure_frontend/loopback_address.hpp`: the `LoopbackAddress` block of **Produces** (includes `"secure_frontend/config.hpp"` for `IpAddress`, `<optional>`, `<utility>`). `src/loopback_address.cpp`:

```cpp
#include "secure_frontend/loopback_address.hpp"

namespace secure_frontend {

std::optional<LoopbackAddress> LoopbackAddress::from(const IpAddress& address) {
  if (address.scope() != AddressScope::loopback) return std::nullopt;  // SP1 classifies from the bytes (§3.7)
  return LoopbackAddress(address);
}

}  // namespace secure_frontend
```

CMake (library block):

```cmake
# SP3 M2 (R-B): loopback-only client for strata run, the tests and strata-api-probe. Never linked into the serve path.
secure_frontend_add_library(secure_frontend_api_client SOURCES src/loopback_address.cpp src/api_client.cpp
    LINKS secure_frontend_config secure_frontend_socket_io secure_frontend_api_core)
```
(`secure_frontend_config` is SP1's target holding `IpAddress`; check with `grep -n 'src/config.cpp' CMakeLists.txt`.)

- [ ] **Step 4: Run** — `cmake --build --preset linux-gcc && ctest --preset linux-gcc -R '^secure_frontend_api_client_tests\.' --output-on-failure && ctest --preset linux-gcc`. Expected: PASS; total previous + 3. Cross-build; wine total unchanged (labeled); run `client_reads_chunked_stream_and_requires_done_and_terminal_chunk` once under wine directly and record it.

- [ ] **Step 5: Handoff row and commit**

```bash
git add include/secure_frontend/loopback_address.hpp src/loopback_address.cpp include/secure_frontend/api_client.hpp src/api_client.cpp tests/api_client_test.cpp CMakeLists.txt ../docs/AGENT_HANDOFF.md
git -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit -m "feat: loopback client library — LoopbackAddress, complete and chunked SSE exchanges with an event cap (SP3 §3.10)" \
  -m "complete requires [DONE] and the terminal chunk; cut otherwise. CTest: secure_frontend_api_client_tests.client_reads_chunked_stream_and_requires_done_and_terminal_chunk, .client_rejects_event_over_cap, .client_accepts_only_loopback_address. RED: <line>. wine: <line>." -m "Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>" -m "Claude-Session: <session link>"
```

---

### Task 10: Wire `stream: true` end to end — schema, `ChatRoute` stream branch, `LeaseHold`, the non-blocking sink in `deliver_route_result` (spec §2.5, §3.1, §3.2, §6.4; C3, R-H)

**Files:**
- Create: `include/secure_frontend/lease_hold.hpp`, `src/lease_hold.cpp`, `tests/support/stream_client.hpp`
- Modify: `src/chat_schema.cpp` (M1's `parse_chat_request`), `include/secure_frontend/chat_route.hpp`, `src/chat_route.cpp`, `src/secure_server_main.cpp`, `src/connection_handler.cpp` (M1's `deliver_route_result`, `SocketStreamSink`), `tests/http_parser_test.cpp`, `tests/api_server_test.cpp`, `docs/AGENT_HANDOFF.md` (M1's "Deferred to M2" list), `CMakeLists.txt`

**Interfaces:**
- Consumes: Tasks 5–9; M1's `ChatRoute`, `ChatRouteConfig`, `make_completion_id`, `SupervisorPort`, `GenerationGate`, `RequestContext`, `StreamHandler`, `RouteResult`, `deliver_route_result`, `ConnectionLimits::write_stall`, `RouteDecision::allow_origin`; SP2's `Lease`, `kill_reason_for`, `shutdown_signalled()`.
- Produces:

```cpp
namespace secure_frontend {
// Owns the granted Lease of one streamed chat (spec §2.5 order: report, destroy the Lease, then release the gate).
class LeaseHold final {
 public:
  LeaseHold(Lease lease, GenerationGate& gate, SupervisorPort& port, std::function<bool()> shutting_down);
  // If the handler never ran (the Lease is still held with GEN written): STOP and drain within
  // stop_drain_timeout — or, during shutdown, abandon at once (R-H) — then end_generation().
  ~LeaseHold();
  LeaseHold(const LeaseHold&) = delete;
  LeaseHold& operator=(const LeaseHold&) = delete;
  [[nodiscard]] Lease& lease() noexcept;
  // Idempotent: report a closed protocol through the port (never while shutting down), destroy the Lease, release the gate.
  void end_generation();
};
// ChatRouteConfig gains (after queue_timeout): std::chrono::milliseconds write_stall, response_write_deadline, stream_keepalive;
// ChatRoute::handle now returns RouteResult (was HttpResponse in M1).
// ChatRoute gains: private RouteResult stream_response(std::optional<Lease>& lease, const std::vector<std::uint32_t>& prompt_ids,
//                                                      std::size_t max_tokens, const ChatCompletionRequest& request, const RequestContext& context);
}
```
A streamed chat: P4 and the gate exactly as M1's non-streaming path; after the context re-check `begin(prompt_ids, {max_tokens, request.sampling})` writes `GEN` (B6 sampling on the stream path); any failure up to here is a normal HTTP status; then `ChatRoute::handle` returns a `StreamHandler` that runs `run_chat_stream` and calls `hold->end_generation()` from `generation_ended`. `deliver_route_result` runs it against M2's `ChunkedStreamSink` (Controller note 1). Test helpers in `tests/support/stream_client.hpp`: `loopback_v4()`, `chat(const ServerProcess&, std::string_view body, ChatRequestOptions = {})`, `open_raw(const ServerProcess&, std::string_view body, int receive_buffer_bytes = 0)`, `stream_text`, `stream_finish`, `stream_error_code`, `content_event_count`, `comments_before_content`.

- [ ] **Step 1: Write the failing schema test** (Review Focus 4) — in `tests/http_parser_test.cpp` (M1's helpers `parse`, `ok`, `fails_with`, `body_with`):

```cpp
void stream_flag_type_and_options_rules() {
  const auto* streaming = ok(parse(body_with(R"("stream":true)")));
  expect(streaming != nullptr && streaming->stream, "stream:true is accepted (M2)");
  expect(ok(parse(body_with(R"("stream":false)"))) != nullptr && !ok(parse(body_with(R"("stream":false)")))->stream, "stream:false is a complete response");
  for (const char* bad : {R"("stream":"true")", R"("stream":1)", R"("stream":null)"}) {
    expect(fails_with(parse(body_with(bad)), ApiError::invalid_request), std::string{"stream must be a boolean: "} + bad);
  }
  const auto* usage = ok(parse(body_with(R"("stream":true,"stream_options":{"include_usage":true})")));
  expect(usage != nullptr && usage->include_usage, "include_usage parsed");
  expect(ok(parse(body_with(R"("stream":true,"stream_options":{})"))) != nullptr, "empty stream_options is a no-op");
  for (const char* bad : {R"("stream_options":{"include_usage":true})", R"("stream":false,"stream_options":{"include_usage":true})",
                          R"("stream":true,"stream_options":{"include_usage":"yes"})", R"("stream":true,"stream_options":{"other":true})",
                          R"("stream":true,"stream_options":[])"}) {
    expect(fails_with(parse(body_with(bad)), ApiError::invalid_request), std::string{"invalid stream_options: "} + bad);
  }
}
```
Register it in `main()` and in the `secure_frontend_add_cases(secure_frontend_http_parser_tests` call. In M1's `dos01_content_array_is_unsupported_parameter` (C3), replace the line `expect(fails_with(parse(body_with(R"("stream":true)")), ApiError::unsupported_parameter, "stream"), "stream:true is M2");` with:

```cpp
  expect(ok(parse(body_with(R"("stream":true)"))) != nullptr, "stream:true is a streamed response since M2");
```

- [ ] **Step 2: Write the failing end-to-end tests.** Create `tests/support/stream_client.hpp`:

```cpp
#pragma once

#include "secure_frontend/api_client.hpp"
#include "secure_frontend/config.hpp"
#include "support/chat_json.hpp"

#include <string>
#include <string_view>

namespace test_support {

inline secure_frontend::LoopbackAddress loopback_v4() {
  return *secure_frontend::LoopbackAddress::from(*secure_frontend::parse_listener_address("127.0.0.1"));
}

inline secure_frontend::ChatExchangeResult chat(const ServerProcess& server, const std::string_view body,
                                                const secure_frontend::ChatRequestOptions& options = {}) {
  return secure_frontend::post_chat_completion(loopback_v4(), server.port(), kHarnessToken, body, options);
}

// A connection that has sent one chat request and has read nothing.
inline std::optional<secure_frontend::LoopbackStreamConnection> open_raw(const ServerProcess& server, const std::string_view body,
                                                                        const int receive_buffer_bytes = 0) {
  auto connection = secure_frontend::LoopbackStreamConnection::open(loopback_v4(), server.port(), std::chrono::seconds(5), receive_buffer_bytes);
  if (connection && !connection->send_all(secure_frontend::render_chat_request(loopback_v4(), server.port(), kHarnessToken, body),
                                          std::chrono::seconds(5))) {
    connection.reset();
  }
  return connection;
}

inline std::string stream_text(const secure_frontend::ChatExchangeResult& result) {
  std::string text;
  for (const auto& item : result.items) {
    if (item.kind != secure_frontend::SseItemKind::data || item.text == "[DONE]") continue;
    if (const auto content = delta_content(item.text)) text += *content;
  }
  return text;
}

// finish_reason of the final chunk, or "<none>".
inline std::string stream_finish(const secure_frontend::ChatExchangeResult& result) {
  for (const auto& item : result.items) {
    if (item.kind != secure_frontend::SseItemKind::data || item.text == "[DONE]") continue;
    if (const auto finish = chunk_finish(item.text)) return *finish;
  }
  return "<none>";
}

// error.code of the first in-stream error event, or "<none>".
inline std::string stream_error_code(const secure_frontend::ChatExchangeResult& result) {
  for (const auto& item : result.items) {
    if (item.kind == secure_frontend::SseItemKind::data && item.text.rfind(R"({"error")", 0) == 0) return error_code_of(item.text);
  }
  return "<none>";
}

inline std::size_t content_event_count(const secure_frontend::ChatExchangeResult& result) {
  std::size_t count = 0;
  for (const auto& item : result.items) count += item.kind == secure_frontend::SseItemKind::data && item.text.find(R"("delta":{"content")") != std::string::npos;
  return count;
}

inline std::size_t comments_before_content(const secure_frontend::ChatExchangeResult& result) {
  std::size_t count = 0;
  for (const auto& item : result.items) {
    if (item.kind == secure_frontend::SseItemKind::comment) ++count;
    else if (item.text.find(R"("delta":{"content")") != std::string::npos) break;
  }
  return count;
}

}  // namespace test_support
```

Add to M1's `tests/api_server_test.cpp` (include `"support/stream_client.hpp"`):

```cpp
using secure_frontend::ChatExchangeEnd;

std::string long_token_ids() { return "--token-ids=" + std::to_string(test_support::kLongTokenId); }

void stream_matches_non_stream_text() {
  const std::string text = "Hi \xE2\x82\xAC \xF0\x9F\x98\x80!";
  auto server = test_support::start_stream_server({"--token-ids=" + test_support::token_ids_for(text)});
  expect(server != nullptr, "server ready");
  if (!server) return;
  const auto complete = test_support::chat(*server, test_support::chat_body(R"("max_tokens":64)"));
  const auto streamed = test_support::chat(*server, test_support::chat_body(R"("max_tokens":64,"stream":true)"));
  expect(complete.end == ChatExchangeEnd::complete && complete.head.status == 200 && !complete.streamed, "complete response");
  expect(streamed.end == ChatExchangeEnd::complete && streamed.streamed && streamed.done_seen && streamed.terminal_chunk_seen,
         "stream with [DONE] and the terminal chunk");
  expect(test_support::complete_text(complete.body) == text, "non-stream text");
  expect(test_support::stream_text(streamed) == text, "stream text equals non-stream text: " + test_support::stream_text(streamed));
  expect(test_support::stream_finish(streamed) == "stop" && test_support::complete_finish(complete.body) == "stop", "both stop");
  const auto head = streamed.head;
  expect(head.header("Cache-Control") == "no-store" && head.header("Connection") == "close" && !head.header("Content-Length"),
         "stream headers (§3.2, §4.6)");
}

void dos02_queue_full_with_streams_is_503_before_headers() {
  auto server = test_support::start_stream_server({"--tokens=100000", "--token-interval-ms=20"});
  expect(server != nullptr, "server ready");
  if (!server) return;
  std::vector<secure_frontend::LoopbackStreamConnection> held;
  for (int index = 0; index < 5; ++index) {  // 1 running + max_queued_generations (4, derived at 8 workers)
    auto connection = test_support::open_raw(*server, test_support::chat_body(R"("stream":true)"));
    expect(connection.has_value(), "held stream " + std::to_string(index));
    if (connection) held.push_back(std::move(*connection));
  }
  std::this_thread::sleep_for(500ms);
  const auto refused = test_support::chat(*server, test_support::chat_body(R"("stream":true)"));
  expect(refused.end == ChatExchangeEnd::complete && refused.head.status == 503 && !refused.streamed, "a plain 503, never a stream");
  expect(test_support::error_code_of(refused.body) == "server_busy" && refused.head.header("Retry-After") == "1", "server_busy + Retry-After: 1");
  const auto type = refused.head.header("Content-Type");
  expect(type && type->rfind("application/json", 0) == 0, "no text/event-stream header before admission");
  held.clear();  // every held client goes away: the running stream is cancelled, the queue empties
  bool served = false;
  for (int attempt = 0; attempt < 50 && !served; ++attempt) {
    served = test_support::chat(*server, test_support::chat_body(R"("max_tokens":1)")).head.status == 200;
    if (!served) std::this_thread::sleep_for(100ms);
  }
  expect(served, "the server serves again once the held clients left");
}

void ext01_extension_fields_rejected_on_stream_path() {
  const auto dir = test_support::make_temp_dir("ext01s");
  const auto record = dir / "record.txt";
  auto server = test_support::start_stream_server({"--record-file=" + test_support::utf8_path(record)});
  expect(server != nullptr, "server ready");
  if (!server) return;
  const std::vector<std::string> bodies{
      test_support::chat_body(R"("stream":true,"tools":[])"), test_support::chat_body(R"("stream":true,"tool_choice":"auto")"),
      test_support::chat_body(R"("stream":true,"functions":[])"), test_support::chat_body(R"("stream":true,"function_call":"auto")"),
      test_support::chat_body(R"("stream":true,"parallel_tool_calls":false)"), test_support::chat_body(R"("stream":true,"mcp_servers":[])"),
      R"({"model":"fake-model","stream":true,"messages":[{"role":"tool","content":"x"}]})",
      R"({"model":"fake-model","stream":true,"messages":[{"role":"function","content":"x"}]})"};
  for (const auto& body : bodies) {
    const auto result = test_support::chat(*server, body);
    expect(result.head.status == 400 && !result.streamed && test_support::error_code_of(result.body) == "extension_disabled",
           "400 extension_disabled before any stream header: " + body);
  }
  const std::string recorded = "\n" + secure_frontend_test::read_file(record);
  expect(recorded.find("\nGEN ") == std::string::npos, "no GEN reached the engine");
}

void dos02_slow_reader_never_backpressures_engine() {
  auto server = test_support::start_stream_server({long_token_ids(), "--tokens=1024", "--ready-context=65536"},
                                           {{"limits.chat.max_completion_tokens", "8192"}});
  expect(server != nullptr, "server ready");
  if (!server) return;
  auto slow = test_support::open_raw(*server, test_support::chat_body(R"("stream":true,"max_tokens":1024)"), 4096);
  expect(slow.has_value(), "slow client connected; it reads nothing for now");
  if (!slow) return;
  const auto start = std::chrono::steady_clock::now();
  const auto second = test_support::chat(*server, test_support::chat_body(R"("max_tokens":1)"), {.timeout = 5s});
  const auto waited = std::chrono::steady_clock::now() - start;
  expect(second.end == ChatExchangeEnd::complete && second.head.status == 200, "a second chat is served while the first client reads nothing");
  expect(waited < 2s, "1 MiB reached DONE at engine speed (< 2 s): " +
                          std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(waited).count()) + " ms");
  const auto first = secure_frontend::read_chat_exchange(*slow, {.timeout = 20s});
  expect(first.end == ChatExchangeEnd::complete && test_support::stream_text(first).size() == 1048576U, "the slow reader then gets all 1 MiB");
  expect(test_support::content_event_count(first) <= 80U, "coalesced into 16 KiB deltas, not 1024 small events");
}

void finish_stop_length_and_generated_bytes_cap() {
  {
    auto server = test_support::start_stream_server({"--tokens=3"});
    expect(server != nullptr, "server A ready");
    if (!server) return;
    for (const bool stream : {false, true}) {
      const std::string mode = stream ? R"(,"stream":true)" : "";
      const auto stop = test_support::chat(*server, test_support::chat_body(R"("max_tokens":3)" + mode));
      const auto length = test_support::chat(*server, test_support::chat_body(R"("max_tokens":2)" + mode));
      const auto finish = [stream](const secure_frontend::ChatExchangeResult& r) {
        return stream ? test_support::stream_finish(r) : test_support::complete_finish(r.body);
      };
      const auto text = [stream](const secure_frontend::ChatExchangeResult& r) {
        return stream ? test_support::stream_text(r) : test_support::complete_text(r.body);
      };
      expect(finish(stop) == "stop" && text(stop) == "HHH", "EOS: stop" + mode);
      expect(finish(length) == "length" && text(length) == "HH", "max_tokens: length" + mode);
    }
  }
  auto server = test_support::start_stream_server({long_token_ids(), "--tokens=4", "--ready-context=65536"},
                                           {{"limits.chat.max_generated_bytes", "2048"}});
  expect(server != nullptr, "server B ready");
  if (!server) return;
  const auto complete = test_support::chat(*server, test_support::chat_body(R"("max_tokens":4)"));
  const auto streamed = test_support::chat(*server, test_support::chat_body(R"("max_tokens":4,"stream":true)"));
  expect(test_support::complete_text(complete.body) == std::string(2048U, 'a') && test_support::complete_finish(complete.body) == "length",
         "generated-bytes cap: 2048 bytes, length");
  expect(test_support::stream_text(streamed) == std::string(2048U, 'a') && test_support::stream_finish(streamed) == "length" &&
             streamed.end == ChatExchangeEnd::complete, "the same on the stream");
}

void utf8_timeout_cut_mid_character_returns_length() {
  // 'A' at once, then E2, 82 at 400 ms steps; the 1 s timeout falls before the final AC (1200 ms).
  auto server = test_support::start_stream_server({"--token-ids=" + test_support::token_ids_for("A\xE2\x82\xAC"), "--token-interval-ms=400"},
                                           {{"limits.engine.generation_timeout_s", "1"}});
  expect(server != nullptr, "server ready");
  if (!server) return;
  const auto streamed = test_support::chat(*server, test_support::chat_body(R"("max_tokens":4,"stream":true)"), {.timeout = 10s});
  expect(streamed.end == ChatExchangeEnd::complete && test_support::stream_text(streamed) == "A", "the cut lead bytes are dropped");
  expect(test_support::stream_finish(streamed) == "length", "timeout is length (decision 4)");
}
```
Extend M2's `generation_timeout_returns_text_with_length` (Task 5) with the stream half at its end:

```cpp
  const auto streamed = test_support::chat(*server, test_support::chat_body(R"("max_tokens":100,"stream":true)"), {.timeout = 10s});
  expect(streamed.end == ChatExchangeEnd::complete && test_support::stream_finish(streamed) == "length" &&
             !test_support::stream_text(streamed).empty(), "stream: text so far, final chunk length");
```
Extend M1's `web04_default_emits_no_cors_header_anywhere` in `tests/api_server_test.cpp` (its `ServerProcess`, called `server` below; a same-origin `Origin`, which M1's policy allows without CORS headers):

```cpp
  const auto sse = test_support::chat(*server, test_support::chat_body(R"("stream":true)"),
                                      {.extra_headers = {{"Origin", "http://127.0.0.1:" + std::to_string(server->port())}}});
  expect(sse.streamed && !sse.head.header("Access-Control-Allow-Origin") && !sse.head.header("Vary"), "no CORS header on an SSE response");
```
Extend `auth01_every_route_rejects_missing_malformed_wrong_token` in `tests/api_server_test.cpp` (the `api_server_tests` copy, not the `api_router_tests` one; its `ServerProcess` is called `server` below):

```cpp
  for (const std::string_view token : {std::string_view(""), std::string_view("wrong-token-wrong-token-wrong-token-xx")}) {
    const auto result = secure_frontend::post_chat_completion(test_support::loopback_v4(), server->port(), token,
                                                              test_support::chat_body(R"("stream":true)"));
    expect(result.head.status == 401 && !result.streamed, "stream:true without a valid token is 401, never a stream");
  }
```
Register the new cases in `main()`; append `stream_matches_non_stream_text dos02_queue_full_with_streams_is_503_before_headers ext01_extension_fields_rejected_on_stream_path dos02_slow_reader_never_backpressures_engine finish_stop_length_and_generated_bytes_cap utf8_timeout_cut_mid_character_returns_length` to `set(SECURE_FRONTEND_API_SERVER_M2_CASES`; link `secure_frontend_api_client` into `secure_frontend_api_server_tests`.

Every `secure_frontend_api_server_tests` case carries the `windows-runtime` label (R-J).

- [ ] **Step 3: Run to see them fail**

Run: `cmake --build --preset linux-gcc && ctest --preset linux-gcc -R 'stream|dos02_slow_reader|finish_stop|utf8_timeout|ext01_extension_fields_rejected_on_stream_path|web04|auth01|dos01_content_array' --output-on-failure`
Expected: FAIL — `stream:true is accepted (M2)` (M1: `400 unsupported_parameter`), `stream with [DONE] and the terminal chunk`, and every other stream assertion. `ext01_extension_fields_rejected_on_stream_path` passes already (M1 checks extension keys before every other member); record that in the commit body.

- [ ] **Step 4: Schema (C3).** In `src/chat_schema.cpp`, `parse_chat_request`, replace the two lines

```cpp
      if (*flag) return fail(ApiError::unsupported_parameter, "stream");   // M1; M2 implements streaming
      request.stream = false;
```
with `request.stream = *flag;`, and replace the `stream_options` branch body (`stream_options_seen = true;`) with:

```cpp
      const auto* options = value.if_object();
      if (options == nullptr) return fail(ApiError::invalid_request);
      for (const auto& option : *options) {
        const bool* include_usage = option.value.if_boolean();
        if (option.key != "include_usage" || include_usage == nullptr) return fail(ApiError::invalid_request);
        request.include_usage = *include_usage;
      }
      stream_options_seen = true;
```
(M1's check after the loop, `if (stream_options_seen && !request.stream) return fail(ApiError::invalid_request);`, stays.) In `docs/AGENT_HANDOFF.md`, M1's "Deferred to M2" bullet: append "— streaming (`stream: true`, `stream_options`) delivered by M2 Task 10".

- [ ] **Step 5: `LeaseHold`.** `include/secure_frontend/lease_hold.hpp` — the `LeaseHold` block of **Produces** (includes `"secure_frontend/engine_supervisor.hpp"`, `"secure_frontend/generation_gate.hpp"`, `<functional>`, `<optional>`; private `std::optional<Lease> lease_; GenerationGate& gate_; SupervisorPort& port_; std::function<bool()> shutting_down_;`). `src/lease_hold.cpp`:

```cpp
#include "secure_frontend/lease_hold.hpp"

#include "secure_frontend/api_constants.hpp"

#include <algorithm>
#include <chrono>
#include <variant>

namespace secure_frontend {

LeaseHold::LeaseHold(Lease lease, GenerationGate& gate, SupervisorPort& port, std::function<bool()> shutting_down)
    : gate_(gate), port_(port), shutting_down_(std::move(shutting_down)) {
  lease_.emplace(std::move(lease));
}

LeaseHold::~LeaseHold() {
  if (!lease_) return;
  EngineProtocolClient& protocol = lease_->protocol();
  if (protocol.generating()) {  // GEN was written but the handler never ran
    if (shutting_down_ && shutting_down_()) {
      protocol.abandon();  // R-H: no drain wait; the supervisor tears this generation down
    } else if (protocol.stop().has_value()) {
      protocol.abandon();
    } else {
      const auto started = std::chrono::steady_clock::now();
      auto until = started + lease_->chat().drive_limits(0U).stop_drain_timeout;
      while (protocol.generating() && std::chrono::steady_clock::now() < until) {
        if (shutting_down_ && shutting_down_()) {  // shutdown began during the drain: cap it (R-H)
          until = std::min(until, std::chrono::steady_clock::now() + api_constants::kShutdownDrainBudget);
        }
        if (std::holds_alternative<EngineProtocolError>(protocol.next_event(api_constants::kStreamPollSlice)) && protocol.closed()) break;
      }
      if (protocol.generating()) protocol.abandon();  // drain failed: SP2 drain_failed
    }
  }
  end_generation();
}

Lease& LeaseHold::lease() noexcept { return *lease_; }

void LeaseHold::end_generation() {
  if (!lease_) return;
  const bool shutting = shutting_down_ && shutting_down_();
  // A closed protocol leaves this engine generation unusable (SP2: kill, restart, count) — never reported during shutdown (R-H).
  if (!shutting && lease_->protocol().closed()) port_.report_fatal(*lease_, kill_reason_for(lease_->protocol()));
  lease_.reset();   // SP2: generating -> ready
  gate_.release();  // only now may the next head acquire (spec §2.5)
}

}  // namespace secure_frontend
```
CMake: add `src/lease_hold.cpp` to the `SOURCES` of `secure_frontend_add_library(secure_frontend_chat_stream` and `secure_frontend_engine_supervisor secure_frontend_generation_gate secure_frontend_chat_execution` to its `LINKS`; `target_link_libraries(secure_frontend_chat_route PUBLIC secure_frontend_chat_stream)`.

The destructor's never-ran path needs a live `EngineSupervisor` for its `Lease`, so it is covered end to end: `dos02_shutdown_during_stream_ends_with_error_event` (Task 14) and `dos02_queue_full_with_streams_is_503_before_headers` exercise release and reuse of the engine after streams end in every state.

- [ ] **Step 6: `ChatRoute` stream branch.** `include/secure_frontend/chat_route.hpp` and `src/chat_route.cpp`: `ChatRoute::handle` returns `RouteResult` instead of `HttpResponse` — declaration `[[nodiscard]] RouteResult handle(std::string_view body, const RequestContext& context);` and the matching definition (include `"secure_frontend/request_router.hpp"`); every existing `return` of an `HttpResponse` converts implicitly, and the `.chat` lambda in `src/secure_server_main.cpp` deduces the new type (M1's `ApiRouter` already forwards a `RouteResult`, R-A). Also add to `ChatRouteConfig`, after `queue_timeout`, `std::chrono::milliseconds write_stall; std::chrono::milliseconds response_write_deadline; std::chrono::milliseconds stream_keepalive;`, and the private `stream_response` declaration of **Produces**. `src/secure_server_main.cpp`, in the `ChatRouteConfig` designated initializer (anchor `.queue_timeout = config.limits.concurrency.queue_timeout},`), make the tail
`.queue_timeout = config.limits.concurrency.queue_timeout, .write_stall = config.limits.http.write_stall, .response_write_deadline = config.limits.http.response_write_deadline, .stream_keepalive = config.limits.http.stream_keepalive},`.

`src/chat_route.cpp`, in `ChatRoute::handle`, directly after the second `resolve_max_tokens` call (anchor `const auto budget = resolve_max_tokens(request.max_tokens, prompt_ids.size(), context_tokens, config_.max_completion_tokens,` and its continuation line), insert:

```cpp
  if (budget && request.stream) return stream_response(lease, prompt_ids, *budget, request, context);
```
and add (includes `"secure_frontend/chat_stream.hpp"`, `"secure_frontend/lease_hold.hpp"`, `"secure_frontend/shutdown_signal.hpp"`, `<memory>`):

```cpp
RouteResult ChatRoute::stream_response(std::optional<Lease>& lease, const std::vector<std::uint32_t>& prompt_ids, const std::size_t max_tokens,
                                       const ChatCompletionRequest& request, const RequestContext& context) {
  const std::atomic<bool>* flag = context.shutting_down;
  std::function<bool()> shutting_down = [flag] { return (flag != nullptr && flag->load()) || shutdown_signalled(); };   // R-H
  auto hold = std::make_shared<LeaseHold>(std::move(*lease), gate_, port_, shutting_down);
  lease.reset();
  auto id = make_completion_id();
  if (!id) {
    hold->end_generation();
    return make_error_response(ApiError::internal_error);
  }
  if (const auto failure = hold->lease().chat().begin(prompt_ids, {.max_tokens = max_tokens, .sampling = request.sampling})) {
    hold->end_generation();  // reports a closed protocol through the port
    return *failure == GenerationFailure::engine_error ? make_error_response(ApiError::engine_error) : engine_unavailable();
  }
  const auto created = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  SseIdentity identity{std::move(*id), static_cast<std::int64_t>(created), config_.model_id};
  ChatStreamSettings settings;
  settings.run = hold->lease().chat().drive_limits(max_tokens);
  settings.write_stall = config_.write_stall;
  settings.tail_deadline = config_.response_write_deadline;
  settings.keepalive = config_.stream_keepalive;
  settings.include_usage = request.include_usage;
  settings.prompt_tokens = prompt_ids.size();
  const TokenizerPack* tokenizer = &tokenizer_;
  return StreamHandler{[hold, identity = std::move(identity), settings, tokenizer, shutting_down](StreamSink& sink) {
    const TokenByteDecoder decoder(*tokenizer);
    const ChatStreamOutcome outcome = run_chat_stream(sink, hold->lease().protocol(), decoder, identity, settings, shutting_down,
                                                      [&hold](const GenerationRunResult&) { hold->end_generation(); });
    sink.set_log_code(outcome.log_code);
  }};
}
```

- [ ] **Step 7: The non-blocking sink at the dispatch site.** In `src/connection_handler.cpp`, `deliver_route_result`: replace `SocketStreamSink sink(socket, limits, signals, decision.allow_origin);` with

```cpp
  AcceptedSocketStream channel(socket);
  ChunkedStreamSink sink(channel,
                         StreamSinkTimeouts{limits.write_stall, [&signals] { return signals.shutting_down != nullptr && signals.shutting_down->load(); }},
                         decision.allow_origin);   // spec §2.7: never blocks the engine loop
```
and delete the class `SocketStreamSink` (its `write` helper went with it; `respond_complete` and `write_deadline` stay). Include `"secure_frontend/chunked_stream_sink.hpp"`; link `secure_frontend_sse` into the target that compiles `src/connection_handler.cpp` (`grep -n 'connection_handler.cpp' CMakeLists.txt`). M1's `http_socket_server_tests.stream_handler_dispatch_writes_head_chunks_and_terminal_chunk` must stay green unchanged (it checks the bytes on the wire, which are identical).

- [ ] **Step 8: Run** — `cmake --build --preset linux-gcc && ctest --preset linux-gcc --output-on-failure`. Expected: PASS everywhere (M1's suite included); total previous + 7. Cross-build; wine previous + 1 (only the parser case is unlabeled).

- [ ] **Step 9: Handoff row and commit**

```bash
git add include/secure_frontend/lease_hold.hpp src/lease_hold.cpp src/chat_schema.cpp include/secure_frontend/chat_route.hpp src/chat_route.cpp \
  src/secure_server_main.cpp src/connection_handler.cpp tests/support/stream_client.hpp tests/http_parser_test.cpp tests/api_server_test.cpp \
  CMakeLists.txt ../docs/AGENT_HANDOFF.md
git status --short   # only the files named above may be staged
git -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit -m "feat: stream:true serves SSE end to end (SP3 §3.2, DOS-02, EXT-01, WEB-04)" \
  -m "Headers only after the lease, the context re-check and GEN (with sampling); the engine is released at DONE; deliver_route_result uses the non-blocking ChunkedStreamSink. CTest: secure_frontend_http_parser_tests.stream_flag_type_and_options_rules; secure_frontend_api_server_tests.stream_matches_non_stream_text, .dos02_queue_full_with_streams_is_503_before_headers, .ext01_extension_fields_rejected_on_stream_path, .dos02_slow_reader_never_backpressures_engine, .finish_stop_length_and_generated_bytes_cap, .utf8_timeout_cut_mid_character_returns_length; amended: http_parser_tests.dos01_content_array_is_unsupported_parameter; extended: .generation_timeout_returns_text_with_length, .web04_default_emits_no_cors_header_anywhere, .auth01_every_route_rejects_missing_malformed_wrong_token. RED: <line>. wine: <line>." \
  -m "Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>" -m "Claude-Session: <session link>"
```

---

### Task 11: Cancellation — client gone, write stall, the cut stream, log codes (spec §2.6, §2.7, §8)

**Files:**
- Modify: `src/chat_stream.cpp` (`StreamObserver::on_tick`, `run_chat_stream`), M1's `include/secure_frontend/connection_handler.hpp` (`ConnectionResult`), `src/connection_handler.cpp` (`deliver_route_result`), `include/secure_frontend/request_log.hpp`, `src/request_log.cpp` (`RequestLogRecord`, `format_request_line`, `log_record_for`), `tests/sse_test.cpp`, `tests/api_server_test.cpp`, `tests/concurrency_test.cpp`, `CMakeLists.txt`

**Interfaces:**
- Consumes: Task 8's `StreamObserver`; `ChatStreamSettings::write_stall`; `StreamSink::set_log_code` and `ChunkedStreamSink::log_code()` (Task 7); M1's `ConnectionResult`, `RequestLogRecord`, `format_request_line`, `log_record_for`.
- Produces: `on_tick` returns `RunCancel::client_gone` or `RunCancel::write_stall` when `sink.client_gone()`; the stream then ends with nothing more written and `ChatStreamOutcome::log_code` `"client_gone"` or `"write_stall"`. `write_stall` is chosen when the sink has refused output continuously for at least `settings.write_stall`. Log path (§8, Controller note 1): `ConnectionResult` and `RequestLogRecord` gain `bool streamed{false}` (last member, default initializer); `deliver_route_result` sets `streamed = true` and `code = sink.log_code()` for a `StreamHandler`; `format_request_line` prints route `chat_stream` for a streamed chat; `log_record_for` no longer replaces a non-empty code with `write_stall`.

- [ ] **Step 1: Write the failing tests.** `tests/sse_test.cpp`:

```cpp
void stream_client_gone_sends_stop() {
  std::vector<std::string> records;
  for (int index = 0; index < 5; ++index) records.push_back(tok('a'));
  StreamRun stream(std::move(records));   // no DONE until STOP
  std::thread closer([&stream] {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    stream.sink.gone = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    stream.transport.push("DONE 5 3 1.0 1.0 cancel");
  });
  const auto outcome = stream.run();
  closer.join();
  expect(stream.transport.stop_count() == 1U, "client gone sends STOP");
  expect(outcome.end == secure_frontend::RunEnd::cancelled && outcome.log_code == "client_gone" && !outcome.terminal_chunk_sent,
         "cancelled, logged as client_gone, no terminal chunk");
  expect(!stream.sink.finished && stream.ended_calls == 1, "nothing more written; engine released");
}
```
Register; append to `set(SECURE_FRONTEND_SSE_CASES`.

`tests/api_server_test.cpp`:

```cpp
void dos02_stream_disconnect_sends_stop_and_frees_engine() {
  const auto dir = test_support::make_temp_dir("dos02d");
  const auto record = dir / "record.txt";
  auto server = test_support::start_stream_server({"--tokens=10000", "--token-interval-ms=5", "--ready-context=65536",
                                            "--record-file=" + test_support::utf8_path(record)});
  expect(server != nullptr, "server ready");
  if (!server) return;
  int contents = 0;
  secure_frontend::ChatRequestOptions options;
  options.on_item = [&contents](const secure_frontend::SseItem& item) {
    contents += item.text.find(R"("delta":{"content")") != std::string::npos;
    return contents < 3;
  };
  const auto cancelled = test_support::chat(*server, test_support::chat_body(R"("stream":true,"max_tokens":4096)"), options);
  expect(cancelled.end == ChatExchangeEnd::cancelled, "the client closed after 3 content chunks");
  expect(test_support::file_has_line(record, "STOP", 2s), "the frontend sent STOP to the engine");
  const auto start = std::chrono::steady_clock::now();
  const auto next = test_support::chat(*server, test_support::chat_body(R"("max_tokens":1)"), {.timeout = 3s});
  expect(next.head.status == 200 && std::chrono::steady_clock::now() - start < 1s, "the next request is served within 1 s");
}

// A client that never reads (4 KiB receive buffer) while the engine streams 16 MiB.
struct StalledStream final {
  std::filesystem::path record;
  std::unique_ptr<test_support::ServerProcess> server;
  std::optional<secure_frontend::LoopbackStreamConnection> slow;
  StalledStream() {
    const auto dir = test_support::make_temp_dir("stall");
    record = dir / "record.txt";
    // Volume, not time (Windows Sleep(1) is ~15.6 ms; SP0 S2 saw Winsock accept one 32 MiB *blocking* send whole): 15 MiB at once, then
    // 1024 more tokens at 50 ms keep the generation running for ~50 s while the unread socket fills.
    server = test_support::start_stream_server({long_token_ids(), "--tokens=16384", "--burst-tokens=15360", "--token-interval-ms=50", "--ready-context=65536",
                                         "--record-file=" + test_support::utf8_path(record)},
                                        {{"limits.http.write_stall_ms", "500"}, {"limits.chat.max_completion_tokens", "16384"},
                                         {"limits.chat.max_generated_bytes", "16777216"}});
    if (server) slow = test_support::open_raw(*server, test_support::chat_body(R"("stream":true,"max_tokens":16384)"), 4096);
  }
};

void dos02_slow_stream_reader_hits_write_stall() {
  StalledStream stalled;
  expect(stalled.server && stalled.slow, "server ready, slow client connected");
  if (!stalled.server || !stalled.slow) return;
  expect(test_support::file_has_line(stalled.record, "STOP", 45s), "no progress for write_stall_ms (500) cancels with STOP");
  const auto next = test_support::chat(*stalled.server, test_support::chat_body(R"("max_tokens":1)"), {.timeout = 5s});
  expect(next.head.status == 200, "the worker and the engine are free again");
}

void stream_cut_has_no_terminal_chunk() {
  StalledStream stalled;
  expect(stalled.server && stalled.slow, "server ready, slow client connected");
  if (!stalled.server || !stalled.slow) return;
  expect(test_support::file_has_line(stalled.record, "STOP", 45s), "the stall cut the stream");
  const auto read = secure_frontend::read_chat_exchange(*stalled.slow, {.timeout = 30s});
  expect(read.streamed && read.end == ChatExchangeEnd::cut && !read.terminal_chunk_seen && !read.done_seen,
         "a cut stream ends without [DONE] and without the terminal chunk, so SDKs raise instead of accepting it");
}

void stream_cancel_during_prefill_frees_engine() {
  const auto dir = test_support::make_temp_dir("prefill");
  const auto record = dir / "record.txt";
  auto server = test_support::start_stream_server({"--prefill-delay-ms=5000", "--record-file=" + test_support::utf8_path(record)});
  expect(server != nullptr, "server ready");
  if (!server) return;
  secure_frontend::ChatRequestOptions options;
  options.on_item = [](const secure_frontend::SseItem&) { return false; };  // close right after the role chunk
  const auto start = std::chrono::steady_clock::now();
  (void)test_support::chat(*server, test_support::chat_body(R"("stream":true)"), options);
  expect(test_support::file_has_line(record, "STOP", 1s), "STOP during prefill");
  expect(std::chrono::steady_clock::now() - start < 2s, "seen within a poll slice, not after the 5 s prefill");
  const auto next = test_support::chat(*server, test_support::chat_body(R"("max_tokens":1)"), {.timeout = 10s});
  expect(next.head.status == 200, "the engine serves the next request");
}

void log_stream_outcomes_use_fixed_codes() {
  auto server = test_support::start_stream_server({"--tokens=200", "--token-interval-ms=5"}, {{"logging.requests", "true"}});
  expect(server != nullptr, "server ready");
  if (!server) return;
  (void)test_support::chat(*server, test_support::chat_body(R"("stream":true,"max_tokens":2)"));
  secure_frontend::ChatRequestOptions options;
  options.on_item = [](const secure_frontend::SseItem& item) { return item.text.find(R"("delta":{"content")") == std::string::npos; };
  (void)test_support::chat(*server, test_support::chat_body(R"("stream":true,"max_tokens":200)"), options);
  std::this_thread::sleep_for(500ms);
  const auto log = server->stderr_text();
  expect(log.find(" chat_stream 200 - ") != std::string::npos, "a normal stream logs '-': " + log);
  expect(log.find(" chat_stream 200 client_gone ") != std::string::npos, "a cancelled stream logs client_gone");
  expect(log.find("fake-model") == std::string::npos && log.find("Hi") == std::string::npos, "no model field or prompt text in the log");
}
```
Extend M1's `dos02_resources_return_to_idle` before its final comparison:

```cpp
  for (int index = 0; index < 20; ++index) {
    secure_frontend::ChatRequestOptions cancel_early;
    cancel_early.on_item = [](const secure_frontend::SseItem&) { return false; };
    (void)test_support::chat(*server, test_support::chat_body(R"("stream":true)"), cancel_early);
  }
```
Register; append `dos02_stream_disconnect_sends_stop_and_frees_engine dos02_slow_stream_reader_hits_write_stall stream_cut_has_no_terminal_chunk stream_cancel_during_prefill_frees_engine log_stream_outcomes_use_fixed_codes` to `set(SECURE_FRONTEND_API_SERVER_M2_CASES`.

- [ ] **Step 2: Run to see them fail** — `ctest --preset linux-gcc -R 'stream_client_gone|dos02_stream_disconnect|dos02_slow_stream|stream_cut|cancel_during_prefill|log_stream|resources_return' --output-on-failure`. Expected: FAIL — `client gone sends STOP`, `the frontend sent STOP to the engine`, `no progress for write_stall_ms (500) cancels with STOP` (Task 8's observer never cancels, so generations run to their end).

- [ ] **Step 3: Implement.** In `StreamObserver` (`src/chat_stream.cpp`) add members `std::optional<Clock::time_point> blocked_since_;` and replace `on_tick` and `flush_ready`:

```cpp
  RunCancel on_tick(bool /*progress*/) override {
    if (sink_.client_gone()) return gone_reason();
    flush_ready();
    if (sink_.client_gone()) return gone_reason();
    return RunCancel::none;
  }

  void flush_ready() {
    while (!pending_.empty()) {
      if (!sink_.writable(std::chrono::milliseconds(0))) {
        if (!blocked_since_) blocked_since_ = Clock::now();
        return;
      }
      blocked_since_.reset();
      send_one();
    }
  }

  // write_stall when output was refused for the whole stall window, else client_gone (§8 log codes).
  [[nodiscard]] RunCancel gone_reason() const {
    return blocked_since_ && Clock::now() - *blocked_since_ >= settings_.write_stall ? RunCancel::write_stall : RunCancel::client_gone;
  }
```
In `run_chat_stream`, after `if (run.timed_out) …`:

```cpp
  if (run.end == RunEnd::cancelled && (run.cancel == RunCancel::client_gone || run.cancel == RunCancel::write_stall)) {
    outcome.log_code = run.cancel == RunCancel::write_stall ? "write_stall" : "client_gone";
    return outcome;  // nothing more is written to a gone client (spec §2.6)
  }
```

Log path. `connection_handler.hpp`: add `bool streamed{false};` as the last member of `ConnectionResult`. `deliver_route_result`, after the handler ran (anchor `connection.bytes_out = sink.bytes();`), add:

```cpp
  connection.streamed = true;
  connection.code = sink.log_code();   // "-" is rendered for an empty code; fixed literals only
```
`request_log.hpp`: add `bool streamed{false};` as the last member of `RequestLogRecord`. `request_log.cpp`: in `log_record_for`, change `else if (f.result.outcome == ConnectionOutcome::client_gone && f.result.status != 0) code = "write_stall";` to `else if (f.result.outcome == ConnectionOutcome::client_gone && f.result.status != 0 && code.empty()) code = "write_stall";` and return `{f.result.route, f.result.status, code, f.elapsed, f.result.bytes_out, f.peer, f.result.streamed}`; in `format_request_line`, replace `line += route_name(r.route);` with `line += r.streamed && r.route == RouteId::chat ? std::string_view{"chat_stream"} : route_name(r.route);`; a stream that ran the handler reports `-` for an empty code (`ChatStreamOutcome::log_code` is `"-"` already for a normal end, so map `"-"` to empty: in `deliver_route_result` use `connection.code = sink.log_code() == "-" ? std::string_view{} : sink.log_code();`).

Unit case in `tests/concurrency_test.cpp` (M1's `secure_frontend_concurrency_tests`; add it to `main()` and to M1's `secure_frontend_add_cases(secure_frontend_concurrency_tests` call):

```cpp
void request_log_names_streamed_chats() {
  secure_frontend::RequestLogRecord stream{secure_frontend::RouteId::chat, 200, "client_gone", std::chrono::milliseconds(7), 99U,
                                           secure_frontend::AddressScope::loopback, true};
  expect(secure_frontend::format_request_line(3, stream) == "req 3 chat_stream 200 client_gone 7 99 loopback", "streamed chat line");
  stream.streamed = false;
  expect(secure_frontend::format_request_line(4, stream).find(" chat 200 ") != std::string::npos, "complete chat line unchanged");
}
```

- [ ] **Step 4: Run** — `cmake --build --preset linux-gcc && ctest --preset linux-gcc --output-on-failure`. Expected: PASS, including M1's `request_log_format_and_token_bucket` and `log_line_format_and_rate_limit`; total previous + 7. Cross-build; wine previous + 2.

- [ ] **Step 5: Handoff row and commit** — **Windows risk (record it in the row, do not assume it away):** the stall cases are capped at 16 MiB of output (SP1's `limits.chat.max_generated_bytes` ceiling) with a 45 s wait for `STOP`. If Winsock absorbs the whole 16 MiB into its buffers, or the stall only happens in the post-DONE tail (no `STOP` then), `dos02_slow_stream_reader_hits_write_stall` and `stream_cut_has_no_terminal_chunk` fail on `windows-msys2-clang`; the row says "Linux green; MSYS2 pending, may fail on buffer volume".

```bash
git add src/chat_stream.cpp include/secure_frontend/connection_handler.hpp src/connection_handler.cpp include/secure_frontend/request_log.hpp src/request_log.cpp tests/sse_test.cpp tests/api_server_test.cpp tests/concurrency_test.cpp CMakeLists.txt ../docs/AGENT_HANDOFF.md
git -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit -m "feat: streams cancel on disconnect and write stall (SP3 §2.6, §2.7, DOS-02)" \
  -m "CTest: secure_frontend_sse_tests.stream_client_gone_sends_stop; secure_frontend_api_server_tests.dos02_stream_disconnect_sends_stop_and_frees_engine, .dos02_slow_stream_reader_hits_write_stall, .stream_cut_has_no_terminal_chunk, .stream_cancel_during_prefill_frees_engine, .log_stream_outcomes_use_fixed_codes; secure_frontend_concurrency_tests.request_log_names_streamed_chats; extended: .dos02_resources_return_to_idle RED: <line>. wine: <line>." -m "Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>" -m "Claude-Session: <session link>"
```

---

### Task 12: Keepalive comments (spec §3.2)

**Files:**
- Modify: `src/chat_stream.cpp` (`StreamObserver`), `tests/sse_test.cpp`, `tests/api_server_test.cpp`, `CMakeLists.txt`

**Interfaces:**
- Consumes: `ChatStreamSettings::keepalive`, `kSseKeepAliveComment`.
- Produces: while no content chunk has been sent for `keepalive`, nothing is pending and the sink is writable now, the pump sends `: keep-alive\n\n`; the timer restarts at every content event or comment, and first starts after the role chunk.

- [ ] **Step 1: Write the failing tests.** `tests/sse_test.cpp`:

```cpp
void keepalive_never_splits_a_pending_event() {
  {  // long prefill: comments, then content
    StreamRun stream({});
    std::thread engine([&stream] {
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
      stream.transport.push(tok('a'));
      stream.transport.push("DONE 1 3 1.0 1.0 stop");
    });
    secure_frontend::ChatStreamSettings settings;
    settings.keepalive = std::chrono::milliseconds(100);
    (void)stream.run(settings);
    engine.join();
    std::size_t comments = 0;
    for (const auto& event : stream.sink.events) {
      if (event == secure_frontend::kSseKeepAliveComment) ++comments;
      if (event.find(R"("delta":{"content")") != std::string::npos) break;
    }
    expect(comments >= 2U, "comments while no content flows: " + std::to_string(comments));
  }
  {  // blocked reader with pending text: no comment is queued or interleaved
    std::vector<std::string> records{tok('a')};
    StreamRun stream(std::move(records));
    stream.sink.blocked_polls = 1000000;
    std::thread engine([&stream] {
      std::this_thread::sleep_for(std::chrono::milliseconds(400));
      stream.sink.blocked_polls = 0;
      stream.transport.push("DONE 1 3 1.0 1.0 stop");
    });
    secure_frontend::ChatStreamSettings settings;
    settings.keepalive = std::chrono::milliseconds(50);
    (void)stream.run(settings);
    engine.join();
    expect(!stream.sink.violated, "nothing is sent while the sink is not writable");
    const auto content = std::find_if(stream.sink.events.begin(), stream.sink.events.end(),
                                      [](const std::string& e) { return e.find(R"("delta":{"content")") != std::string::npos; });
    expect(content != stream.sink.events.end() && std::find(stream.sink.events.begin() + 1, content, std::string(secure_frontend::kSseKeepAliveComment)) == content,
           "pending content goes out before any comment");
  }
}
```
`tests/api_server_test.cpp`:

```cpp
void stream_keepalive_comment_during_prefill() {
  auto server = test_support::start_stream_server({"--prefill-delay-ms=2500"}, {{"limits.http.stream_keepalive_ms", "1000"}});
  expect(server != nullptr, "server ready");
  if (!server) return;
  const auto streamed = test_support::chat(*server, test_support::chat_body(R"("stream":true)"), {.timeout = 15s});
  expect(streamed.end == ChatExchangeEnd::complete, "the stream completes");
  expect(test_support::comments_before_content(streamed) >= 2U,
         "at least 2 ': keep-alive' comments during a 2.5 s prefill: " + std::to_string(test_support::comments_before_content(streamed)));
}
```
Register both; append to the two case lists.

- [ ] **Step 2: Run to see them fail** — expected FAIL: `comments while no content flows: 0` and `at least 2 ': keep-alive' comments during a 2.5 s prefill: 0`.

- [ ] **Step 3: Implement.** In `StreamObserver`: add `Clock::time_point last_activity_{Clock::now()};` (the observer is constructed after the role chunk is queued: construct it after `sink.send_event(render_role_event(identity))` in `run_chat_stream`, moving the two `(void)sink...` lines above `StreamObserver observer(...)`), set `last_activity_ = Clock::now();` at the end of `send_one()`, and in `on_tick` after `flush_ready();`:

```cpp
    if (pending_.empty() && Clock::now() - last_activity_ >= settings_.keepalive && sink_.writable(std::chrono::milliseconds(0))) {
      (void)sink_.send_event(kSseKeepAliveComment);
      last_activity_ = Clock::now();
    }
```

- [ ] **Step 4: Run** — full suite. Expected: PASS; total previous + 2; wine previous + 1.

- [ ] **Step 5: Handoff row and commit**

```bash
git add src/chat_stream.cpp tests/sse_test.cpp tests/api_server_test.cpp CMakeLists.txt ../docs/AGENT_HANDOFF.md
git -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit -m "feat: SSE keepalive comments while no content flows (SP3 §3.2)" \
  -m "CTest: secure_frontend_sse_tests.keepalive_never_splits_a_pending_event; secure_frontend_api_server_tests.stream_keepalive_comment_during_prefill RED: <line>. wine: <line>." -m "Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>" -m "Claude-Session: <session link>"
```

---

### Task 13: Engine failures — in-stream error events, `x-should-retry`, report and restart (spec §3.4)

**Files:**
- Modify: `src/chat_stream.cpp` (`run_chat_stream`), `tests/sse_test.cpp`, `tests/api_server_test.cpp`, `CMakeLists.txt`

**Interfaces:**
- Consumes: `render_error_event`, M1's `ApiError`/`api_error_row`, `LeaseHold::end_generation` (already reports a closed protocol), Task 5's non-stream mapping.
- Produces: `RunEnd::engine_error` → error event `engine_error`; `engine_unavailable` → `engine_unavailable`; `undecodable` → `internal_error`. Pending text is flushed first (under the tail deadline); then the error event, `[DONE]`, the terminal chunk. No `finish_reason` after an error. `log_code` = the §7.2 code.

- [ ] **Step 1: Write the failing tests.** `tests/sse_test.cpp`:

```cpp
void stream_error_events_follow_text_and_end_with_done() {
  const auto check = [](std::vector<std::string> records, const secure_frontend::ApiError code, const std::string& name) {
    StreamRun stream(std::move(records));
    const auto outcome = stream.run();
    const auto& events = stream.sink.events;
    expect(events.size() >= 3U && events[events.size() - 2U] == secure_frontend::render_error_event(code), name + ": error event");
    expect(events.back() == secure_frontend::kSseDoneEvent && stream.sink.finished && outcome.terminal_chunk_sent, name + ": [DONE] + terminal");
    for (const auto& event : events) expect(event.find(R"("finish_reason":")") == std::string::npos, name + ": no finish_reason after an error");
    expect(stream.text() == "a", name + ": text before the failure was delivered");
  };
  check({tok('a'), "ERR boom"}, secure_frontend::ApiError::engine_error, "ERR");
  check({tok('a'), "DONE 1 3 1.0 1.0 nonfinite"}, secure_frontend::ApiError::engine_error, "nonfinite");
  check({tok('a'), "<close>"}, secure_frontend::ApiError::engine_unavailable, "engine exit");
  check({tok('a'), "T 256", "DONE 2 3 1.0 1.0 cancel"}, secure_frontend::ApiError::internal_error, "control token in output");
}
```
`tests/api_server_test.cpp`:

```cpp
void err_mid_stream_is_error_event_then_done() {
  auto server = test_support::start_stream_server({"--tokens=5", "--err-mid-gen=2"});
  expect(server != nullptr, "server ready");
  if (!server) return;
  const auto streamed = test_support::chat(*server, test_support::chat_body(R"("stream":true)"));
  expect(test_support::stream_text(streamed) == "HH" && test_support::stream_error_code(streamed) == "engine_error", "text, then engine_error");
  expect(streamed.done_seen && streamed.terminal_chunk_seen && streamed.end == ChatExchangeEnd::complete, "[DONE] and the terminal chunk follow");
  expect(test_support::stream_finish(streamed) == "<none>", "no finish_reason after an error");
}

void crash_mid_stream_is_engine_unavailable_event() {
  auto server = test_support::start_stream_server({"--tokens=10", "--crash-mid-gen=3", "--token-interval-ms=10"});
  expect(server != nullptr, "server ready");
  if (!server) return;
  const auto streamed = test_support::chat(*server, test_support::chat_body(R"("stream":true)"));
  expect(test_support::stream_error_code(streamed) == "engine_unavailable" && streamed.terminal_chunk_seen, "engine_unavailable event, ended normally");
  bool restarting = false;
  for (int poll = 0; poll < 100 && !restarting; ++poll) {
    const auto health = server->get("/health");
    restarting = health && health->body.find(R"("recovery":"restarting")") != std::string::npos;
    if (!restarting) std::this_thread::sleep_for(20ms);
  }
  expect(restarting, "/health follows SP2: recovery=restarting");
}

void nonfinite_is_engine_error_and_engine_stays_ready() {
  auto server = test_support::start_stream_server({"--finish=nonfinite"});
  expect(server != nullptr, "server ready");
  if (!server) return;
  const auto engines_before = server->descendants();
  const auto complete = test_support::chat(*server, test_support::chat_body(""));
  expect(complete.head.status == 503 && test_support::error_code_of(complete.body) == "engine_error", "non-stream: 503 engine_error");
  expect(complete.head.header("x-should-retry") == "false", "SDK retries suppressed");
  const auto streamed = test_support::chat(*server, test_support::chat_body(R"("stream":true)"));
  expect(test_support::stream_error_code(streamed) == "engine_error" && streamed.done_seen && streamed.terminal_chunk_seen, "stream: error event");
  expect(server->wait_for_health(test_support::kHealthReady, 2s), "the engine stays ready (SP0 E3)");
  expect(server->descendants() == engines_before, "no restart");
}

void engine_error_carries_should_retry_false() {
  auto server = test_support::start_stream_server({"--tokens=3", "--err-mid-gen=0"});
  expect(server != nullptr, "server ready");
  if (!server) return;
  const auto result = test_support::chat(*server, test_support::chat_body(""));
  expect(result.head.status == 503 && test_support::error_code_of(result.body) == "engine_error", "ERR is 503 engine_error");
  expect(result.head.header("x-should-retry") == "false", "x-should-retry: false (risk R5)");
}
```
Register; append to the case lists.

- [ ] **Step 2: Run to see them fail** — expected FAIL: `ERR: error event` (Task 8 ends other outcomes as a cut) and `text, then engine_error`. `nonfinite_is_engine_error_and_engine_stays_ready` and `engine_error_carries_should_retry_false` non-stream halves pass since M1 Task 16 and this plan's Task 5 (record it).

- [ ] **Step 3: Implement.** In `run_chat_stream`, replace the line `if (run.end != RunEnd::stop && run.end != RunEnd::length) return outcome;  // other ends: Tasks 11, 13, 14` with:

```cpp
  const auto until = Clock::now() + settings.tail_deadline;
  if (run.end == RunEnd::engine_error || run.end == RunEnd::engine_unavailable || run.end == RunEnd::undecodable) {
    const ApiError code = run.end == RunEnd::engine_error ? ApiError::engine_error
                              : run.end == RunEnd::engine_unavailable ? ApiError::engine_unavailable
                                                                      : ApiError::internal_error;
    outcome.log_code = api_error_row(code).code;
    if (!observer.flush_all(until) || !observer.send(render_error_event(code), until) || !observer.send(kSseDoneEvent, until) ||
        !observer.wait_writable(until)) {
      return outcome;
    }
    sink.finish();
    outcome.terminal_chunk_sent = sink.writable(std::chrono::milliseconds(0));
    return outcome;
  }
  if (run.end != RunEnd::stop && run.end != RunEnd::length) return outcome;  // shutdown: Task 14
```
and delete the later duplicate `const auto until = …` line.

- [ ] **Step 4: Run** — full suite. Expected: PASS; total previous + 5; wine previous + 1.

- [ ] **Step 5: Handoff row and commit**

```bash
git add src/chat_stream.cpp tests/sse_test.cpp tests/api_server_test.cpp CMakeLists.txt ../docs/AGENT_HANDOFF.md
git -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit -m "feat: in-stream error events for engine failures (SP3 §3.4)" \
  -m "CTest: secure_frontend_sse_tests.stream_error_events_follow_text_and_end_with_done; secure_frontend_api_server_tests.err_mid_stream_is_error_event_then_done, .crash_mid_stream_is_engine_unavailable_event, .nonfinite_is_engine_error_and_engine_stays_ready, .engine_error_carries_should_retry_false RED: <line>. wine: <line>." -m "Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>" -m "Claude-Session: <session link>"
```

---

### Task 14: Shutdown during a stream (spec §2.8; R-H)

**Files:**
- Modify: `src/chat_stream.cpp`, `tests/sse_test.cpp`, `tests/api_server_test.cpp`, `CMakeLists.txt`

**Interfaces:**
- Consumes: the `shutting_down` probe `context.shutting_down->load() || shutdown_signalled()` built by `ChatRoute::stream_response` (Task 10); `LeaseHold::end_generation()` never reports while it is true; `api_constants::kShutdownDrainBudget`; `GenerationRunLimits::shutdown_drain` (Task 5).
- Produces: `on_tick` returns `RunCancel::shutting_down` first when the probe is true; whatever the run ended with, if the probe is true after the run the stream ends with the `server_shutting_down` error event, `[DONE]`, the terminal chunk (pending text flushed first), `log_code` `"server_shutting_down"`. Every wait after the probe first turned true — the generation drain (already capped by `min(stop_drain, kShutdownDrainBudget)` in `drive_generation`) and the tail flush — ends at `shutdown_seen + kShutdownDrainBudget` (R-H: SP2's 5 s `_Exit` timer starts only after the pool has joined), never at the 60 s tail deadline.

- [ ] **Step 1: Write the failing tests.** `tests/sse_test.cpp`:

```cpp
void stream_shutdown_sends_error_event_and_done() {
  std::vector<std::string> records{tok('a')};
  StreamRun stream(std::move(records));
  std::atomic<bool> stopping{false};
  std::thread signal([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    stopping = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    stream.transport.push("DONE 1 3 1.0 1.0 cancel");
  });
  const auto outcome = stream.run({}, [&stopping] { return stopping.load(); });
  signal.join();
  const auto& events = stream.sink.events;
  expect(events.size() >= 3U && events[events.size() - 2U] == secure_frontend::render_error_event(secure_frontend::ApiError::server_shutting_down),
         "server_shutting_down event");
  expect(events.back() == secure_frontend::kSseDoneEvent && outcome.terminal_chunk_sent, "[DONE] and the terminal chunk");
  expect(stream.transport.stop_count() == 1U, "the generation was stopped");
  expect(outcome.log_code == "server_shutting_down", "log code");
}
```
`tests/api_server_test.cpp`:

```cpp
void dos02_shutdown_during_stream_ends_with_error_event() {
  auto server = test_support::start_stream_server({"--tokens=100000", "--token-interval-ms=10"});
  expect(server != nullptr, "server ready");
  if (!server) return;
  int contents = 0;
  bool stopped = false;
  secure_frontend::ChatRequestOptions options;
  options.timeout = 10s;
  options.on_item = [&](const secure_frontend::SseItem& item) {
    contents += item.text.find(R"("delta":{"content")") != std::string::npos;
    if (contents == 2 && !stopped) {
      stopped = true;
      server->stop(test_support::ServerProcess::Stop::term);
    }
    return true;
  };
  const auto streamed = test_support::chat(*server, test_support::chat_body(R"("stream":true)"), options);
  expect(test_support::stream_error_code(streamed) == "server_shutting_down", "in-stream server_shutting_down");
  expect(streamed.done_seen && streamed.terminal_chunk_seen && streamed.end == ChatExchangeEnd::complete, "[DONE] and the terminal chunk");
  expect(server->wait_exit(5s) == 0, "exit 0 within SP2's 5 s bound");
}
```
Register; append to the case lists.

- [ ] **Step 2: Run to see them fail** — expected FAIL: `server_shutting_down event` (the probe is ignored; the stream ends as a cut or runs on).

- [ ] **Step 3: Implement.** In `StreamObserver` add the member `std::optional<Clock::time_point> shutdown_seen_;` and the public accessor `[[nodiscard]] std::optional<Clock::time_point> shutdown_seen() const { return shutdown_seen_; }`; the first lines of `on_tick` become:

```cpp
    if (shutting_down_()) {
      if (!shutdown_seen_) shutdown_seen_ = Clock::now();
      return RunCancel::shutting_down;
    }
```
In `run_chat_stream`, directly after `generation_ended(run);` and the `timed_out` line:

```cpp
  if (run.cancel == RunCancel::shutting_down || shutting_down()) {
    outcome.log_code = "server_shutting_down";
    // R-H: bounded by what is left of kShutdownDrainBudget since shutdown was first seen, never by the 60 s tail deadline.
    const auto seen = observer.shutdown_seen().value_or(Clock::now());
    const auto until = std::min(seen + api_constants::kShutdownDrainBudget, Clock::now() + settings.tail_deadline);
    if (!observer.flush_all(until) || !observer.send(render_error_event(ApiError::server_shutting_down), until) ||
        !observer.send(kSseDoneEvent, until) || !observer.wait_writable(until)) {
      return outcome;
    }
    sink.finish();
    outcome.terminal_chunk_sent = sink.writable(std::chrono::milliseconds(0));
    return outcome;
  }
```
(Task 10's `generation_ended` lambda calls `hold->end_generation()`, which checks the same probe, so no `report_fatal` happens after the flag — the spec §2.8 rule.)

A shutdown that begins while a normal or error end is still flushing its tail is bounded the same way: in `StreamObserver::wait_writable`, inside its loop, add before the deadline check

```cpp
      if (shutting_down_()) {
        if (!shutdown_seen_) shutdown_seen_ = Clock::now();
        if (Clock::now() >= *shutdown_seen_ + api_constants::kShutdownDrainBudget) return false;  // R-H: cut, no terminal chunk
      }
```


- [ ] **Step 4: Run** — full suite, plus SP2's `proc05_sigterm_during_generation_exits_cleanly` and M1's `shutdown_errors_never_report_fatal` stay green. Expected: PASS; total previous + 2; wine previous + 1.

- [ ] **Step 5: Handoff row and commit**

```bash
git add src/chat_stream.cpp tests/sse_test.cpp tests/api_server_test.cpp CMakeLists.txt ../docs/AGENT_HANDOFF.md
git -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit -m "feat: shutdown ends streams with an error event, [DONE] and the terminal chunk (SP3 §2.8)" \
  -m "CTest: secure_frontend_sse_tests.stream_shutdown_sends_error_event_and_done; secure_frontend_api_server_tests.dos02_shutdown_during_stream_ends_with_error_event RED: <line>. wine: <line>." -m "Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>" -m "Claude-Session: <session link>"
```

---

### Task 15: `strata run` target from `server.json` — loopback only (spec §3.10, §12.1)

**Files:**
- Create: `include/secure_frontend/run_target.hpp`, `src/run_target.cpp`
- Modify: `tests/api_client_test.cpp`, `CMakeLists.txt`

**Interfaces:**
- Consumes: `parse_json`, `JsonValue` (SP1); `parse_listener_address` (SP1); `LoopbackAddress::from` (Task 9).
- Produces:

```cpp
namespace secure_frontend {
inline constexpr std::string_view kRunNeedsLoopbackMessage =
    "strata run needs a server on a loopback address; set listener.host to 127.0.0.1 or ::1.";
inline constexpr std::string_view kRunServerFileInvalidMessage = "The server file is not valid.";
struct RunTarget final { LoopbackAddress address; std::uint16_t port; };
struct RunTargetError final { std::string_view message; };   // one of the two literals above
// Reads PX's server.json: "host" (IP literal, required), "port" (1-65535, required); pid, start time,
// version and any other keys are ignored. Never echoes the file's values.
[[nodiscard]] std::variant<RunTarget, RunTargetError> run_target_from_server_json(std::string_view json);
}
```
Contract note (Controller note 4): PX's `server.json` has no host today and no writer exists; the future writer (PX/SP2 follow-up) must emit `host`.

- [ ] **Step 1: Write the failing test** — `tests/api_client_test.cpp`:

```cpp
void run_refuses_non_loopback_server_json() {
  using secure_frontend::RunTarget;
  using secure_frontend::RunTargetError;
  const auto message = [](const std::string& json) -> std::string {
    const auto target = secure_frontend::run_target_from_server_json(json);
    return std::holds_alternative<RunTargetError>(target) ? std::string(std::get<RunTargetError>(target).message) : "ok";
  };
  expect(message(R"({"pid":12,"host":"127.0.0.1","port":8080,"version":"0.1"})") == "ok", "loopback IPv4");
  expect(message(R"({"host":"::1","port":8080})") == "ok", "loopback IPv6");
  const auto lan = message(R"({"host":"172.16.1.5","port":8080})");
  expect(lan == secure_frontend::kRunNeedsLoopbackMessage, "a LAN-only server is refused with the fixed message");
  expect(lan.find("listener.host") != std::string::npos && lan.find("192.168") == std::string::npos, "names the key, never the value");
  expect(message(R"({"host":"0.0.0.0","port":8080})") == secure_frontend::kRunNeedsLoopbackMessage, "the wildcard is not loopback");
  expect(message(R"({"host":"localhost","port":8080})") == "ok", "localhost maps to 127.0.0.1 without a resolver (SP1)");
  for (const std::string bad : {R"({"port":8080})", R"({"host":"127.0.0.1"})", R"({"host":"127.0.0.1","port":0})",
                                R"({"host":"127.0.0.1","port":70000})", R"({"host":"example.com","port":8080})", "not json", "[]"}) {
    expect(message(bad) == secure_frontend::kRunServerFileInvalidMessage, "invalid server file: " + bad);
  }
  const auto target = secure_frontend::run_target_from_server_json(R"({"host":"127.0.0.1","port":8080})");
  expect(std::holds_alternative<RunTarget>(target) && std::get<RunTarget>(target).port == 8080U, "port carried");
}
```
Register; append to `set(SECURE_FRONTEND_API_CLIENT_CASES` (the case is pure, but the binary's other cases are labeled; keep it in the same call).

- [ ] **Step 2: Run to see it fail** — build FAILS: `secure_frontend/run_target.hpp: No such file or directory`.

- [ ] **Step 3: Implement.** Header: the **Produces** block (includes `"secure_frontend/loopback_address.hpp"`, `<cstdint>`, `<string_view>`, `<variant>`). `src/run_target.cpp`:

```cpp
#include "secure_frontend/run_target.hpp"

#include "secure_frontend/config.hpp"
#include "secure_frontend/json.hpp"

namespace secure_frontend {

std::variant<RunTarget, RunTargetError> run_target_from_server_json(const std::string_view json) {
  const RunTargetError invalid{kRunServerFileInvalidMessage};
  const auto parsed = parse_json(json, {.max_bytes = 4096U, .max_depth = 2U, .max_container_entries = 16U, .max_string_bytes = 256U});
  const auto* root = std::get_if<JsonValue>(&parsed);
  const auto* members = root != nullptr ? root->if_object() : nullptr;
  if (members == nullptr) return invalid;
  const std::string* host = nullptr;
  const std::int64_t* port = nullptr;
  for (const auto& member : *members) {
    if (member.key == "host") host = member.value.if_string();
    if (member.key == "port") port = member.value.if_integer();
  }
  if (host == nullptr || port == nullptr || *port < 1 || *port > 65535) return invalid;
  const auto address = parse_listener_address(*host);
  if (!address) return invalid;
  auto loopback = LoopbackAddress::from(*address);
  if (!loopback) return RunTargetError{kRunNeedsLoopbackMessage};
  return RunTarget{std::move(*loopback), static_cast<std::uint16_t>(*port)};
}

}  // namespace secure_frontend
```
CMake: `target_sources(secure_frontend_api_client PRIVATE src/run_target.cpp)` and link `secure_frontend_json` into `secure_frontend_api_client` if M1 did not.

- [ ] **Step 4: Run** — full suite. Expected: PASS; total previous + 1.

- [ ] **Step 5: Handoff row and commit**

```bash
git add include/secure_frontend/run_target.hpp src/run_target.cpp tests/api_client_test.cpp CMakeLists.txt ../docs/AGENT_HANDOFF.md
git -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit -m "feat: run target from server.json is loopback-only (SP3 §3.10)" \
  -m "Defines the reader contract (host + port) the future server.json writer must meet. CTest: secure_frontend_api_client_tests.run_refuses_non_loopback_server_json RED: <line>. wine: <line>." -m "Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>" -m "Claude-Session: <session link>"
```

---

### Task 16: `strata-api-probe` and the GPU validation kit procedure (spec §9.5)

**Files:**
- Create: `tests/api_probe_main.cpp`, `docs/validation/sp3-api-kit.md`
- Modify: `tests/api_client_test.cpp`, `CMakeLists.txt`

**Interfaces:**
- Consumes: `post_chat_completion`, `read_chat_exchange`, `LoopbackStreamConnection`, `ChunkedSseReader` items (Task 9); `Utf8StreamDecoder` (Task 3); `parse_listener_address`, `LoopbackAddress::from`; `chat_json.hpp` helpers are test-only and **not** used by the probe (it parses with SP1's `parse_json` directly).
- Produces: test-only executable `strata-api-probe` (built in every preset, never installed):
  `strata-api-probe --port <n> --token-file <path> --model <id> [--host 127.0.0.1|::1] <mode>`; modes `stream`, `nonstream`, `cancel-after=<n>`, `cancel-during-prefill`, `concurrent=<k>`, `utf8`, `seed`. Prints one line `PASS <mode> <facts>` or `FAIL <mode> <reason>`; exit 0 on PASS, 1 on FAIL, 2 on usage.

- [ ] **Step 1: Write the failing test** — `tests/api_client_test.cpp` (link `secure_frontend_test_support`, `secure_frontend_engine_process`; `secure_frontend_test_paths(secure_frontend_api_client_tests)`; define `STRATA_API_PROBE_PATH="$<TARGET_FILE:strata-api-probe>"` and `add_dependencies(secure_frontend_api_client_tests strata-api-probe)`):

```cpp
void probe_modes_pass_against_fake_engine() {
  auto server = test_support::start_stream_server({"--tokens=200", "--token-interval-ms=5"});
  expect(server != nullptr, "server ready");
  if (!server) return;
  const auto dir = test_support::make_temp_dir("probe");
  const auto token_file = dir / "token";
  secure_frontend_test::write_file(token_file, test_support::kHarnessToken, 0600);
  for (const std::string mode : {"stream", "nonstream", "cancel-after=2", "utf8", "seed", "concurrent=3"}) {
    secure_frontend::EngineProcess probe;
    const bool started = !probe.start({.executable = STRATA_API_PROBE_PATH,
                                       .arguments = {"--port", std::to_string(server->port()), "--token-file", test_support::utf8_path(token_file),
                                                     "--model", "fake-model", mode},
                                       .environment = {}}).has_value();
    expect(started && probe.wait_for(60s) == 0, "strata-api-probe " + mode + " passes against the fake engine");
  }
}
```
(Include `"support/chat_json.hpp"`, `"support/test_paths.hpp"`, `"secure_frontend/engine_process.hpp"`.) Register; append to the API client case list.

- [ ] **Step 2: Run to see it fail** — CMake configure/build FAILS: no target `strata-api-probe`.

- [ ] **Step 3: Implement** `tests/api_probe_main.cpp`:

```cpp
// strata-api-probe: test-only client for the SP3 GPU validation kit (docs/validation/sp3-api-kit.md).
// Built in every preset, never installed. Talks only to a loopback address.
#include "secure_frontend/api_client.hpp"
#include "secure_frontend/config.hpp"
#include "secure_frontend/json.hpp"
#include "secure_frontend/utf8_stream_decoder.hpp"

#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
using secure_frontend::ChatExchangeEnd;
using secure_frontend::ChatExchangeResult;

struct Probe final {
  secure_frontend::LoopbackAddress address;
  std::uint16_t port;
  std::string token;
  std::string model;
};

long long ms_since(const Clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
}

std::string json_text(const std::string& text) {
  std::string out;
  secure_frontend::append_json_string(&out, text);
  return out;
}

std::string body(const Probe& probe, const std::string& prompt, const std::string& extra) {
  return R"({"model":)" + json_text(probe.model) + R"(,"messages":[{"role":"user","content":)" + json_text(prompt) + "}]" +
         (extra.empty() ? "" : "," + extra) + "}";
}

std::optional<secure_frontend::JsonValue> parse(const std::string& text) {
  auto parsed = secure_frontend::parse_json(text, {.max_bytes = 16U << 20U, .max_depth = 8U, .max_container_entries = 4096U, .max_string_bytes = 16U << 20U});
  if (auto* value = std::get_if<secure_frontend::JsonValue>(&parsed)) return std::move(*value);
  return std::nullopt;
}

const secure_frontend::JsonValue* member(const secure_frontend::JsonValue* object, const std::string_view key) {
  const auto* members = object != nullptr ? object->if_object() : nullptr;
  if (members == nullptr) return nullptr;
  for (const auto& entry : *members) {
    if (entry.key == key) return &entry.value;
  }
  return nullptr;
}

const secure_frontend::JsonValue* first_choice(const secure_frontend::JsonValue* root) {
  const auto* choices = member(root, "choices");
  return choices != nullptr && choices->if_array() != nullptr && !choices->if_array()->empty() ? &choices->if_array()->front() : nullptr;
}

std::string string_of(const secure_frontend::JsonValue* value) { return value != nullptr && value->if_string() ? *value->if_string() : ""; }

struct StreamFacts final {
  std::string text;
  std::string finish;
  std::size_t content_events{0};
  long long first_content_ms{-1};
};

StreamFacts facts_of(const ChatExchangeResult& result) {
  StreamFacts facts;
  for (const auto& item : result.items) {
    if (item.kind != secure_frontend::SseItemKind::data || item.text == "[DONE]") continue;
    const auto root = parse(item.text);
    const auto* choice = first_choice(root ? &*root : nullptr);
    const auto* content = member(member(choice, "delta"), "content");
    if (content != nullptr && content->if_string() && !content->if_string()->empty()) {
      facts.text += *content->if_string();
      ++facts.content_events;
    }
    const auto finish = string_of(member(choice, "finish_reason"));
    if (!finish.empty()) facts.finish = finish;
  }
  return facts;
}

std::string complete_text(const ChatExchangeResult& result) {
  const auto root = parse(result.body);
  return string_of(member(member(first_choice(root ? &*root : nullptr), "message"), "content"));
}

ChatExchangeResult send(const Probe& probe, const std::string& json, secure_frontend::ChatRequestOptions options = {}) {
  if (options.timeout == std::chrono::seconds(30)) options.timeout = 600s;  // real prefill can be long
  return secure_frontend::post_chat_completion(probe.address, probe.port, probe.token, json, options);
}

int pass(const std::string& mode, const std::string& facts) {
  std::cout << "PASS " << mode << ' ' << facts << '\n';
  return 0;
}
int fail(const std::string& mode, const std::string& reason) {
  std::cout << "FAIL " << mode << ' ' << reason << '\n';
  return 1;
}

bool complete_stream(const ChatExchangeResult& result) {
  return result.end == ChatExchangeEnd::complete && result.streamed && result.done_seen && result.terminal_chunk_seen;
}

// After a cancel: how long until a one-token request is served (STOP -> DONE ... cancel -> next lease).
long long next_request_ms(const Probe& probe) {
  const auto start = Clock::now();
  const auto next = send(probe, body(probe, "Say OK.", R"("max_tokens":1)"));
  return next.head.status == 200 ? ms_since(start) : -1;
}

int run_mode(const Probe& probe, const std::string& mode) {
  if (mode == "stream") {
    const auto start = Clock::now();
    const auto result = send(probe, body(probe, "Say hello in five words.", R"("max_tokens":64,"stream":true)"));
    const auto facts = facts_of(result);
    if (!complete_stream(result) || facts.finish.empty()) return fail(mode, "stream not complete (needs [DONE] and the terminal chunk)");
    return pass(mode, "events=" + std::to_string(facts.content_events) + " bytes=" + std::to_string(facts.text.size()) +
                          " finish=" + facts.finish + " total_ms=" + std::to_string(ms_since(start)));
  }
  if (mode == "nonstream") {
    const auto result = send(probe, body(probe, "Say hello in five words.", R"("max_tokens":64)"));
    if (result.end != ChatExchangeEnd::complete || result.head.status != 200) return fail(mode, "status " + std::to_string(result.head.status));
    return pass(mode, "bytes=" + std::to_string(complete_text(result).size()));
  }
  if (mode.rfind("cancel-after=", 0) == 0) {
    const int after = std::stoi(mode.substr(13));
    int seen = 0;
    secure_frontend::ChatRequestOptions options;
    options.on_item = [&seen, after](const secure_frontend::SseItem& item) {
      seen += item.text.find(R"("delta":{"content")") != std::string::npos;
      return seen < after;
    };
    const auto result = send(probe, body(probe, "Count from 1 to 500.", R"("max_tokens":2048,"stream":true)"), options);
    if (result.end != ChatExchangeEnd::cancelled) return fail(mode, "the stream ended before the cancel");
    const auto next = next_request_ms(probe);
    if (next < 0) return fail(mode, "the next request failed after the cancel");
    return pass(mode, "next_request_ms=" + std::to_string(next) + " (compare with limits.engine.stop_drain_timeout_s)");
  }
  if (mode == "cancel-during-prefill") {
    std::string prompt;
    while (prompt.size() < 40000U) prompt += "Summarize this sentence carefully. ";
    secure_frontend::ChatRequestOptions options;
    options.on_item = [](const secure_frontend::SseItem&) { return false; };  // close at the role chunk
    (void)send(probe, body(probe, prompt, R"("max_tokens":16,"stream":true)"), options);
    const auto next = next_request_ms(probe);
    if (next < 0) return fail(mode, "the next request failed after a cancel during prefill");
    return pass(mode, "next_request_ms=" + std::to_string(next) + " (risk R4: STOP latency during prefill)");
  }
  if (mode.rfind("concurrent=", 0) == 0) {
    const int count = std::stoi(mode.substr(11));
    std::mutex lock;
    std::vector<int> finished;
    std::vector<std::thread> clients;
    for (int index = 0; index < count; ++index) {
      clients.emplace_back([&, index] {
        const auto result = send(probe, body(probe, "Say OK.", R"("max_tokens":16)"));
        std::lock_guard guard(lock);
        finished.push_back(result.head.status == 200 ? index : -1 - index);
      });
      std::this_thread::sleep_for(100ms);  // admission order = start order
    }
    for (auto& client : clients) client.join();
    for (int index = 0; index < count; ++index) {
      if (finished[static_cast<std::size_t>(index)] != index) return fail(mode, "completion order is not admission order (FIFO)");
    }
    return pass(mode, "fifo=yes clients=" + std::to_string(count));
  }
  if (mode == "utf8") {
    const std::string prompt = "Repeat exactly: 東京 и 🌍 — ok";
    const auto streamed = send(probe, body(probe, prompt, R"("max_tokens":64,"seed":7,"temperature":0,"stream":true)"));
    const auto complete = send(probe, body(probe, prompt, R"("max_tokens":64,"seed":7,"temperature":0)"));
    const auto facts = facts_of(streamed);
    secure_frontend::Utf8StreamDecoder check;
    const bool valid = check.push(facts.text) == facts.text && check.finish(false).empty();
    if (!complete_stream(streamed) || !valid) return fail(mode, "stream incomplete or not valid UTF-8");
    if (facts.text != complete_text(complete)) return fail(mode, "stream text differs from non-stream text at a fixed seed");
    return pass(mode, "bytes=" + std::to_string(facts.text.size()));
  }
  if (mode == "seed") {
    const auto first = send(probe, body(probe, "Write one sentence about rivers.", R"("max_tokens":48,"seed":42,"temperature":0.7)"));
    const auto second = send(probe, body(probe, "Write one sentence about rivers.", R"("max_tokens":48,"seed":42,"temperature":0.7)"));
    if (first.head.status != 200 || complete_text(first) != complete_text(second)) return fail(mode, "the same seed gave different text");
    return pass(mode, "same_text=yes");
  }
  return -1;
}

}  // namespace

int main(int argc, char* argv[]) {
  std::optional<std::uint16_t> port;
  std::string token_file;
  std::string model;
  std::string host = "127.0.0.1";
  std::string mode;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    const bool has_value = index + 1 < argc;
    if (argument == "--port" && has_value) port = static_cast<std::uint16_t>(std::stoi(argv[++index]));
    else if (argument == "--token-file" && has_value) token_file = argv[++index];
    else if (argument == "--model" && has_value) model = argv[++index];
    else if (argument == "--host" && has_value) host = argv[++index];
    else mode = argument;
  }
  const auto ip = secure_frontend::parse_listener_address(host);
  auto address = ip ? secure_frontend::LoopbackAddress::from(*ip) : std::nullopt;
  std::ifstream file(token_file, std::ios::binary);
  std::string token((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  while (!token.empty() && (token.back() == '\n' || token.back() == '\r')) token.pop_back();
  if (!port || !address || token.empty() || model.empty() || mode.empty()) {
    std::cerr << "usage: strata-api-probe --port <n> --token-file <path> --model <id> [--host 127.0.0.1|::1] "
                 "<stream|nonstream|cancel-after=<n>|cancel-during-prefill|concurrent=<k>|utf8|seed>\n";
    return 2;
  }
  const Probe probe{std::move(*address), *port, std::move(token), std::move(model)};
  const int result = run_mode(probe, mode);
  if (result < 0) {
    std::cerr << "unknown mode\n";
    return 2;
  }
  return result;
}
```
On Windows the probe needs Winsock started: `post_chat_completion` calls `ensure_winsock()` (Task 9). CMake (tests block):

```cmake
    # SP3 §9.5: test-only probe for the offline GPU kit. Never installed.
    add_executable(strata-api-probe tests/api_probe_main.cpp)
    target_link_libraries(strata-api-probe PRIVATE secure_frontend_api_client secure_frontend_json secure_frontend_utf8_stream)
    target_compile_features(strata-api-probe PRIVATE cxx_std_20)
    secure_frontend_set_warnings(strata-api-probe)
```

`docs/validation/sp3-api-kit.md` — write the procedure (one section per box, Linux and Windows), with exactly these parts:
1. **Purpose and scope** — what CI cannot prove (spec §9.5 items 1–4): real tokenizer splits of multi-byte UTF-8, `STOP` latency during long prefill (risk R4), whether the engine applies `GEN` `temperature`/`top_p`/`seed`, real clients against a real model. Results go to the release record, not to CI.
2. **Setup** — build `strata-api-probe` from the release commit (`cmake --build --preset linux-gcc --target strata-api-probe`; Windows MSYS2 `windows-msys2-clang`); start `strata serve` with a real model; note the port; the token file path (owner-only).
3. **Egress snapshot before** — Linux `ss -tunap`, Windows `netstat -abno`, saved to the record.
4. **Probe runs** — the table:

| Command | Pass criterion | Record |
| --- | --- | --- |
| `strata-api-probe --port P --token-file T --model M stream` | `PASS`; `[DONE]` and the terminal chunk seen | events, bytes, total_ms |
| `… nonstream` | `PASS` | bytes |
| `… cancel-after=5` | `PASS` | next_request_ms vs `limits.engine.stop_drain_timeout_s` |
| `… cancel-during-prefill` | `PASS` | next_request_ms (risk R4); a value near the drain timeout means raise `stop_drain_timeout_s` |
| `… concurrent=3` | `PASS` (FIFO) | — |
| `… utf8` | `PASS`: stream text equals non-stream text at seed 7, temperature 0, valid UTF-8 | bytes |
| `… seed` | `PASS`: same text twice at seed 42 | — |

5. **Real clients** — `openai-python` (`client.chat.completions.create(..., stream=True)`, iterate to the end; then the same with the server stopped mid-stream: the SDK must raise, not return a truncated answer) and `curl -N -H "Authorization: Bearer $(cat T)" -H 'Content-Type: application/json' -d '{"model":"M","messages":[{"role":"user","content":"hi"}],"stream":true}' http://127.0.0.1:P/v1/chat/completions` (events print as they arrive; the output ends with `data: [DONE]`).
6. **Egress snapshot during and after** — the same commands while a stream runs and after `strata stop`; the only TCP sockets of the server are the listener and its accepted connections; no UDP.
7. **Record template** — date, commit, box, OS, GPU, model, every probe line verbatim, the three egress snapshots, the client outcomes.

- [ ] **Step 4: Run** — full suite. Expected: PASS; total previous + 1. Cross-build (the probe builds with `-Werror`); wine unchanged.

- [ ] **Step 5: Handoff row and commit**

```bash
git add tests/api_probe_main.cpp tests/api_client_test.cpp CMakeLists.txt ../docs/validation/sp3-api-kit.md ../docs/AGENT_HANDOFF.md
git -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit -m "test: strata-api-probe and the SP3 GPU kit procedure (SP3 §9.5)" \
  -m "CTest: secure_frontend_api_client_tests.probe_modes_pass_against_fake_engine RED: <line>. wine: <line>." -m "Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>" -m "Claude-Session: <session link>"
```

---

### Task 17: Documentation truth pass and the M2 handoff (spec §13.2 item 6)

**Files:**
- Modify: `docs/AGENT_HANDOFF.md`, `docs/security/engine-protocol-limits.md`, `docs/security/threat-model.md`, `secure_frontend/README.md`

**Interfaces:**
- Consumes: every earlier task's handoff row.
- Produces: accurate docs; the M2 section marked complete with the final counts.

- [ ] **Step 1: Check the names the docs must cite exist** (the docs claim only what is built):

```bash
ctest --preset linux-gcc -N | grep -cE 'secure_frontend_(sse|api_server|api_client)_tests\.' 
for case in sse_role_content_final_done_sequence sse_include_usage_adds_usage_chunk sse_ids_and_created_constant_per_stream \
  sse_events_never_contain_raw_newline sse_coalesced_control_heavy_text_respects_event_cap sse_chunked_framing_and_terminal_chunk \
  stream_matches_non_stream_text stream_cut_has_no_terminal_chunk utf8_split_across_tokens_is_held_then_released \
  utf8_invalid_sequence_becomes_replacement_character utf8_incomplete_tail_dropped_on_length_and_cancel \
  utf8_incomplete_tail_on_stop_becomes_one_replacement utf8_timeout_cut_mid_character_returns_length \
  utf8_non_stream_max_tokens_cut_mid_character_returns_length finish_stop_length_and_generated_bytes_cap \
  generation_timeout_returns_text_with_length nonfinite_is_engine_error_and_engine_stays_ready err_mid_stream_is_error_event_then_done \
  crash_mid_stream_is_engine_unavailable_event engine_error_carries_should_retry_false stream_keepalive_comment_during_prefill \
  client_reads_chunked_stream_and_requires_done_and_terminal_chunk client_rejects_event_over_cap client_accepts_only_loopback_address \
  run_refuses_non_loopback_server_json dos02_queue_full_with_streams_is_503_before_headers dos02_stream_disconnect_sends_stop_and_frees_engine \
  dos02_slow_stream_reader_hits_write_stall dos02_slow_reader_never_backpressures_engine dos02_shutdown_during_stream_ends_with_error_event \
  ext01_extension_fields_rejected_on_stream_path fake_engine_stream_modes_selftest; do
  ctest --preset linux-gcc -N | grep -q "\.${case}\$" && echo "OK   $case" || echo "MISS $case"; done
```
Expected: every line `OK` (spec §13.2 item 4: each §9.3 M2 name is listed individually). Any `MISS`: fix the registration before writing docs.

- [ ] **Step 2: `docs/security/engine-protocol-limits.md`** — replace the statement that the frontend only reads whole generations with: streaming exists (`stream: true`); the frontend reads `T` records one at a time (`next_event`, 20 ms slices) at engine speed whatever the client does; a client that disconnects, stalls for `limits.http.write_stall_ms`, or a server shutdown sends `STOP` and drains to `DONE … cancel` within `limits.engine.stop_drain_timeout_s`, else SP2's `drain_failed`; generation timeout returns the text so far with `length` (decision 4); one generation at a time, now queued (M1).
- [ ] **Step 3: `docs/security/threat-model.md`** — add under the API section: chunked framing and why a cut stream has no terminal chunk (truncation is visible to SDKs; `[DONE]` is not a success marker); `x-should-retry: false` on `engine_error` and the residual for clients that ignore it (R5); no bytes while queued (R6); the slow-reader bound (pending text ≤ `limits.chat.max_generated_bytes` + one 128 KiB event per streaming worker); R4 measured only on the GPU box (`docs/validation/sp3-api-kit.md`, `cancel-during-prefill`).
- [ ] **Step 4: `secure_frontend/README.md`** — routes section: `POST /v1/chat/completions` with `"stream": true` (SSE, chunked, role/content/final/usage/`[DONE]`, error events with the §7.2 codes, `stream_options.include_usage`); the keys that govern streams (`limits.http.write_stall_ms`, `limits.http.response_write_deadline_ms`, `limits.http.stream_keepalive_ms`, `limits.chat.max_generated_bytes`); the constants note gains `api_constants::kMaxDeltaTextBytes` 16 KiB and `api_constants::kMaxSseEventBytes` 128 KiB; `strata-api-probe` is test-only.
- [ ] **Step 5: `docs/AGENT_HANDOFF.md`** — mark "SP3 M2 deliverables" complete: final linux-gcc and wine `tests passed` lines (N₀ + every task's k), the list of W/`windows-runtime` cases "authoritative in MSYS2 — pending the owner's push" (naming `dos02_slow_stream_reader_hits_write_stall` and `stream_cut_has_no_terminal_chunk` as at risk on Winsock buffer volume, Task 11), Controller notes 2, 4, 7, 8 restated as decisions taken, the open follow-ups (server.json writer must emit `host`; SP2 second-run D-1/D-2 unchanged; GPU kit run pending on the owner's boxes), and "nothing is claimed for a GPU or for Windows beyond CI logs".
- [ ] **Step 6: Final run** — `ctest --preset linux-gcc` and the cross build + wine smoke; paste both result lines into the handoff.
- [ ] **Step 7: Commit**

```bash
git add ../docs/AGENT_HANDOFF.md ../docs/security/engine-protocol-limits.md ../docs/security/threat-model.md README.md
git -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit -m "docs: SP3 M2 streaming — limits, threat model, README, handoff" \
  -m "States only what is built and tested; Windows rows pending the MSYS2 run; GPU kit pending the owner's boxes." -m "Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>" -m "Claude-Session: <session link>"
```

---

## Self-review (done while writing)

**Spec coverage (M2 scope of §1.3 and §13.1):** incremental protocol reads → Task 4; UTF-8 stream decoder → Task 3; SSE encoder, chunked framing, event cap, 16 KiB code-point deltas → Tasks 6–7; streaming path and backpressure (pending decoded text, render only when writable, release at DONE, tail deadline) → Tasks 8, 10; keepalive → Task 12; finish mapping incl. timeout → `length`, byte cap, nonfinite, ERR, crash → Tasks 5, 10, 13; error events + `x-should-retry` → Task 13; shutdown during stream → Task 14; cancellation on EOF/reset/stray byte and `write_stall` → Tasks 7, 11; loopback client with event-size cap → Task 9; `strata run` loopback rule → Task 15; `strata-api-probe` + kit → Task 16; every §9.3 M2 name: `sse_*` (Tasks 6–8), `utf8_*` (Tasks 3, 5, 10), finish rows (Tasks 5, 10, 13), keepalive (12), client rows (9, 15), `dos02_*` M2 rows (10, 11, 14), `stream_matches_non_stream_text` and `stream_cut_has_no_terminal_chunk` (10, 11), `ext01_extension_fields_rejected_on_stream_path` (10), WEB-04 SSE and AUTH-01 `stream:true` extensions (10), `dos02_resources_return_to_idle` stream half (11), `fake_engine_stream_modes_selftest` (2); docs of §13.2 item 6 → Task 17. The latent `decode_token_ids` failure → Task 5 (Controller note 9). Pre-flight rulings: R-A (seam names consumed exactly, Tasks 7, 10, 11), R-B (client owned here, Task 9), R-C (`drive_generation`, M1's `run_generation` rebuilt, timeout → `200 length`, sampling on the stream path, Tasks 5, 10), R-D (literal trailers in every commit step), R-E (`drive_generation_*` names; M1's selftest extended, Task 2), R-F (Task 2 is a diff on M1's fake), R-H (shutdown bound in Tasks 5, 10, 14), R-J (labels).

**Placeholder scan:** no TBD/TODO and no unresolved M1 seam marker; every M1 name is taken from M1's amended plan (e55648d6) and grepped verbatim by Task 1. The only `<line>` markers are the RED/wine result lines an executor pastes into commit bodies.

**Type consistency:** `drive_generation(EngineProtocolClient&, const TokenByteDecoder&, const GenerationRunLimits&, GenerationObserver&)` is used identically in Tasks 5 and 8; `RunCancel`/`RunEnd` members match across Tasks 5, 8, 11, 13, 14; `run_chat_stream(StreamSink&, EngineProtocolClient&, const TokenByteDecoder&, const SseIdentity&, const ChatStreamSettings&, const std::function<bool()>&, const GenerationEnded&)` is called the same way in Task 8's tests and Task 10's router; `ChatExchangeResult` fields (`end`, `head`, `streamed`, `items`, `done_seen`, `terminal_chunk_seen`, `body`) match across Tasks 9–16; `LeaseHold::end_generation()` and its stored shutdown probe match Tasks 10 and 14; `ChunkedStreamSink`'s `status()`/`bytes()`/`finished()`/`failed()`/`log_code()` are what `deliver_route_result` reads (Tasks 7, 10, 11).

**Review Focus:** each of the five lines has its pinning test in the owning task (Tasks 8, 5, 11, 10, 12).
