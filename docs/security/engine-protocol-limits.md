# Engine protocol limits (Strata v0.2.2, `upstream/src/program/generate.cpp`)

Facts the frontend relies on; verified against the engine source on 2026-10-13
(`v0.1.43`), re-verified for `v0.1.44.1` on 2026-10-14, for `v0.2.1.3` on
2026-10-20 and for `v0.2.2` on 2026-10-21 (the bump audits). The `qwen35moe` and `strata_kq` rows are new at
`v0.1.44.1`. The SP2 rows (Windows stdin writes, rewritten; READY wait, fatal
lines, exit codes, GPU selection) were checked against the submodule on
2026-10-14 and re-checked against `v0.2.1.3` on 2026-10-20 and `v0.2.2` on 2026-10-21; line numbers are
for `v0.2.2` unless stated otherwise. Rows new at `v0.2.1.x`: `DONE` tail,
HIP-Windows `rocm\bin`, batch telemetry, the version split.

| Fact | Engine behavior | Frontend consequence |
| --- | --- | --- |
| stdin line length | Unbounded `std::string`; no truncation, no error | Frontend caps its own GEN line at min(32 + 11 × ctx, 16 MiB `kProtocolMaxLineBytes`) bytes and never relies on the engine to reject |
| Prompt form | One line: `GEN <max_new> [k=v ...] <id,id,...>`; no multi-line form | One prompt line for complete and streaming responses alike; one generation in flight |
| Context rule | `n + max_new + 8 <= --max-context` else `ERR ...` and the engine stays alive | Frontend rejects with 400 before GEN using the same rule |
| READY | `READY <max_context> stop` | ctx parsed from READY sizes the outbound GEN cap (min(32 + 11 × ctx, 16 MiB)); the inbound record cap is a fixed 8192 bytes (`kMaxReadLineBytes`); a fixed 32768 context is never advertised |
| STOP | Honored only after the GEN has been picked up; always followed by `DONE ... cancel` | Frontend sends STOP only after a generation has started; drains to DONE |
| `PP` prefill progress | `generate.cpp` prints `PP <position reached> <prompt tokens> <ms> <fresh tok/s>` (`%lld %lld %.0f %.1f`) after every batched prompt chunk; `q36_engine.cu` (`qwen35moe`) prints `PP <done> <fresh> <ms> <tok/s>` where `<fresh>` excludes tokens reused from the held state; upstream's server accepts any `PP` with two decimal fields | Consumed silently (never client-visible) when bounded: 3–5 fields, two decimal counters (`parse_decimal`, 32-bit), optional non-negative metrics; no ordering between the counters is assumed. Anything else fails the request closed (`unexpected_line`). The GLM profile keeps its exact 3-/5-field record with `0 < total` and `read <= total`. Fixed 2026-10-14; not yet observed against a real engine |
| `nonfinite` | `DONE ... nonfinite`; next request restarts from token 0 (since `v0.2.2`, after a prompt-window `nonfinite` the next request reads its whole prompt through the batched path and the engine prints a `strata serve:` stderr notice, `generate.cpp:12399-12402`; protocol unchanged) | 503 for that request; protocol stays idle |
| `RESUME`/`REUSED` prompt progress | The main engine's serve loop prints `RESUME <n>` before reading the prompt and `REUSED <n>` once it is read, for every accepted GEN and unconditionally even when nothing is reused: `generate.cpp:11214`/`:11849` at `v0.1.44.1`, `:12809`/`:13910` at `v0.2.1.3`, `:12899`/`:14006` at `v0.2.2` (upstream's own server consumes them, `serve/server.py`). The `qwen35moe` engine prints neither; `YIELDED <slot> <tokens>` needs the `--batch`/`--slots` paths, which the frontend refuses | Consumed silently (never client-visible) when exactly two fields with a 32-bit decimal count; anything else fails the request closed (`unexpected_line`). The GLM profile refuses them (exact record kept). Found 2026-10-20 while re-auditing the protocol for the v0.2.1.x bump — before this fix every real-engine chat would have failed with `unexpected_line` at BOTH tags; not yet observed against a real engine |
| Model path | `--pack DIR` is the expert/weight directory, loaded unconditionally, default cwd-relative `pack/full`; `--native <shard1.gguf>` is a separate shard source; other weight flags: `--ple-gguf`, `--embd-gguf`, `--native-head-gguf`, `--native-dense-gguf`, `--mtp`; no positional args. Upstream (`setup.py`) launches `--pack <pack> --native <shard1> [--ple-gguf <ple>] ...` | Strata profile only: the frontend supplies shard 1 as `--native <models_root/artifact>` from the inventory (`engine_arguments`, appended last) and rejects `--native` from `--engine-arg`. The operator MUST pass `--pack` for a real run (otherwise the engine resolves `pack/full` against its cwd); `--pack` and the other weight flags pass through (as `engine.args` since SP1) until SP5 models the whole launch. The GLM-5.3 profile passes arguments through (next row) |
| GLM-5.3 profile | Engine CLI not characterized | Frontend passes arguments through unchanged (NUL/--serve rejected) and does not supply the model path; model-id linkage is Strata-only until SP5/SP6 |
| Network | None in the engine; no host/port/bind/http flags | Deny-list covers only frontend-owned flags |
| Unsupported modes | `--batch` and its alias `--slots` (BT/BDONE), `--vision` (GENI) | Rejected in `--engine-arg` until the frontend implements their protocol. Since `v0.2.1` the `qwen35moe` engine parses these too, so the frontend refusal is the only guard (see the `qwen35moe` row) |
| Windows stdin writes | The engine reads stdin on its own reader thread with `read(0)` / `_read(0)` (`generate.cpp:10783-10808`), which normally drains the pipe | Every engine-stdin write has two bounds. Per chunk: on Windows the frontend's end of the stdin pipe is overlapped and writes go in 64 KiB chunks, and a chunk that does not complete within min(1 s, the remaining deadline) is cancelled with `CancelIoEx`; on POSIX the stdin write end is `O_NONBLOCK` and each `poll` wait for writability is at most 1 s (a limit per wait, not strictly per 64 KiB). Overall (R1, owner ruling 2026-10-14): every chunk wait is also capped by the request's remaining deadline. A GEN write and the read to `DONE` share one generation deadline: the read gets only what is left of it. `STOP` and `QUIT` are each bounded by 1 s. A GEN write that misses either bound fails the request and the supervisor kills the engine (`killed_by_supervisor(write_timeout)`, counted). Windows behavior is confirmed only by the `windows-msys2-clang` job (pending) |
| `qwen35moe` family switch (new at `v0.1.44.1`, CUDA/HIP builds except gfx906 — `v0.2.1` HIP gfx906 rebuilds exclude the qwen35 engine, `CMakeLists.txt:749-770`, and `strata --families` then lists `qwen4exp` only) | When the GGUF named by `--native`/`--model` declares `general.architecture = qwen35moe`, `main` hands the whole argv to a separate engine (`upstream/src/qwen35/q36_engine.cu`, `strata::qwen35::run`) before normal parsing. Its serve loop differs: context rule `n + max_new > ctx` (no `+ 8`, `q36_engine.cu:3505`), the loop takes `GEN` only and answers any other line with `ERR expected: GEN …` and continues (`:3720`), no `nonfinite` finish, no `QUIT` handling (exits on stdin EOF), no `--pack`, and unknown `--flags` are skipped instead of fatal. Since `v0.2.1` it PARSES `--vision` and `--batch`/`--slots` (`:3775-3776`) and has the BT/BDONE/BGEN/BGENI/BADM batch protocol documented at `:3175-3177` | Our `+ 8` check is stricter, so still safe. The frontend argv deny-list is now the ONLY guard keeping `--vision`/`--batch`/`--slots` away from q36 — at `v0.1.44.1` the engine itself skipped them; the deny-list refusal stands. Not yet modeled: the frontend must not assume a `qwen35moe` model behaves as above (QUIT alone does not end it). Open until SP5 |
| `strata_kq` shared library (new at `v0.1.44.1`, CUDA builds only) | With `STRATA_MMQ_KQ=1` (set by default by the `qwen35moe` path unless already set — `q36_engine.cu:3801`/`:3803`, which also sets `STRATA_Q8_PACKED=1` at `:3800`) the engine opens `strata_kq.dll` / `libstrata_kq.so` by full path from its own executable's folder (`LoadLibraryA` / `dlopen`, `upstream/src/prefill/mmq_kq_loader.cpp`); missing file falls back to FP16. Its own DLL dependencies use the default Windows search order | Packaging/inventory must ship or exclude it explicitly and verify it like the engine binary; the engine folder must not be writable by the service user |
| `DONE` tail (changed at `v0.2.1`) | The main engine's `DONE` now ends with ` chunk=<tokens> experts_streamed=<blobs>` (`generate.cpp:15202-15219`); the `qwen35moe` engine's `DONE` did not gain it | The tail stays opaque: the client inspects only field 5 (the finish word), whose position is unchanged on both engine paths; nothing to change |
| HIP-Windows DLL search (new at `v0.2.1.2`) | `hipblas`, `libhipblaslt` and `rocblas` are delay-loaded and live in `<exe folder>\rocm\bin`, which the engine adds with `SetDllDirectoryW` (`generate.cpp:2297-2311`; `CMakeLists.txt:739-745`); CUDA-only Windows builds are unaffected | Same rule as `strata_kq`: packaging/inventory must name `rocm\bin` explicitly and the engine folder must not be writable by the service user (SP7 packaging) |
| Batch telemetry (`v0.2.1`) | `BSTAT`/`PSTAT` stdout records appear only when `STRATA_BATCH_TELEMETRY=1` AND `--batch` is active (`generate.cpp:11209` for the environment gate) | Unreachable for us: the engine environment allowlist keeps `STRATA_BATCH_TELEMETRY` out, and `--batch` is refused at argv level; keep it that way |
| Version numbering split (`v0.2.1`) | The app tag and the engine's CMake project version now differ: tag `v0.2.2` vs engine `0.1.47` (`CMakeLists.txt:11`); `strata --version` prints the engine version (`generate.cpp:2317`) | Owner ruling 2026-10-20: the TAG version governs our gates — `upstream.version` feeds `kEngineVersion`, and manifest `min_version` compares against the tag number; the engine's self-report is informational for the frontend |
| READY wait (SP2) | Upstream's service waits `STRATA_ENGINE_READY_S`, default 900 s (`serve/server.py:330`) | The supervisor reads READY as soon as the engine starts, within `limits.engine.ready_timeout_s` (default 900 s). `/health` is `503` `engine=starting` until then. A miss kills the engine (`killed_by_supervisor(ready_timeout)`) and counts a failure |
| Fatal lines (SP2) | `ERR verify: timed out at layer `, `ERR verify batch: timed out at layer `, `ERR verify: an earlier window never finished`, `ERR verify batch: an earlier window never finished` (upstream `FATAL_PREFIXES`, `serve/server.py:835-836`; upstream's `_pump` stops at such a line and never publishes the trailing `DONE`, `:1165-1184`). The engine lives on but is unusable | Before READY (no request exists yet): the supervisor kills the engine (`killed_by_supervisor(fatal_line)`), counts a failure and restarts it; `/health` stays `503`. After READY: the request fails with the generic `503`, and the engine is killed, counted and restarted the same way. A trailing `DONE` is never accepted |
| Exit codes (SP2) | `QUIT` ends the serve loop (`generate.cpp:11865`); an unrecoverable serve-loop `ERR` ends the loop with `return 1`, so the engine exits 1 (`:13955-13963`, `:13972-13974`, `:13981-13984`, `:13987-13990`), and a CUDA fault after such an `ERR` leaves at once with `_Exit(1)` (`:13957-13962`); the no-progress watchdog calls `abort()` (`:11010-11017`; SIGABRT on POSIX, exit 3 or `0xC0000409` on Windows). Exit codes are not part of the protocol | Classified only for the frontend's own log line (`engine generation <n> ended: <kind>`). Every end before shutdown counts as a failure, except, at most 3 times in a row with no `DONE` in between, a drain failure after a client-requested `STOP` (`client_stop_drain_failed`, SP3 D-2; nothing issues a client `STOP` before SP3) |
| GPU selection (SP2) | `--gpu LIST` makes the engine set `CUDA_DEVICE_ORDER`/`CUDA_VISIBLE_DEVICES` (or `HIP_VISIBLE_DEVICES`) itself (`generate.cpp:2357-2375`; usage `:801`) | The frontend renders `--gpu` from its typed `engine.gpu` option and refuses `--gpu` in `--engine-arg`. Strata family: no GPU variable is ever in the engine's environment. The SP6 `glm` family will render `CUDA_DEVICE_ORDER`/`CUDA_VISIBLE_DEVICES` from typed configuration (mechanism only today; not reachable from configuration). Not GPU-verified |
| CPU pool affinity (new at `v0.2.2`) | `--pool-cores LIST` pins the CPU expert workers to the listed logical processors (`generate.cpp:2512`; experimental, off by default, an unreadable list is ignored with a stderr warning); env twin `STRATA_POOL_CORES` | Not frontend-owned and not a protocol mode: it passes through `engine.args` like the other local tuning flags. The env twin never reaches the engine (closed environment allowlist) |

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

The chat `model` field bound (256 bytes, `chat_schema.cpp` `kMaximumModelBytes`)
is a parsing bound, not a second model-ID rule: the field must still equal the
configured `engine.model_id`, whose grammar caps it at 128 bytes, so a 129–256
byte value parses and is then rejected as a model mismatch.

### `EngineProtocolLimits` and the renderer caps: internal backstops

`EngineProtocolLimits` (`include/secure_frontend/engine_protocol_client.hpp`)
holds `max_prompt_tokens`, `max_generated_tokens` and `max_new_tokens`, each
262144, and `secure_server_main.cpp` builds the client with `{}` (these
defaults). They are classified as **internal protocol backstops**, not
configuration keys: they bound what the protocol client will put on the GEN
line or accumulate from `T` records no matter what the layers above decide, and
they are not in spec §4's list of keys. The operator-facing limits are the
`limits.chat.*` keys, enforced before `begin_generation` is called.

The Qwen and GLM prompt renderers (`qwen_prompt_renderer.cpp`,
`glm_prompt_renderer.cpp`) carry three more internal constants:
`kMaximumMessages` 128, `kMaximumMessageBytes` 64 KiB (65536) and
`kMaximumPromptBytes` 512 KiB, the cap on the rendered prompt text (template
markup plus every message) before tokenization. The rendered-prompt cap is an
internal constant, not a configuration key (controller ruling, SP1 final fix
wave): with the defaults a chat body is at most 49152 bytes, far below it. An
operator who raises `limits.http.max_request_bytes` and
`limits.chat.max_body_bytes` above roughly 512 KiB can send messages that pass
the per-message and message-count limits but whose rendered prompt exceeds
512 KiB; such a request is refused at planning (`503`), as before.

Since the SP1 final fix wave the configuration ranges match the enforced caps
(spec §9.5: every value the schema accepts is one the server can honour):

| Key | Range | Enforced cap it matches |
| --- | --- | --- |
| `limits.chat.max_messages` | 1–128 | renderer `kMaximumMessages` 128 |
| `limits.chat.max_message_bytes` | 1–65536 | renderer `kMaximumMessageBytes` 64 KiB |
| `limits.chat.max_prompt_tokens` | 1–262144 | `EngineProtocolLimits::max_prompt_tokens` 262144 |

`max_completion_tokens` (≤ 16384) is always below `max_new_tokens`, so that
pair cannot diverge. The defaults (128 messages, 65536 message bytes, 32768
prompt tokens) are inside these ranges. `cfg01_chat_ranges_match_enforced_caps`
pins each cap (the cap is accepted, one above it is `out_of_range`).

## SP3 M1 (frontend, branch `sp3-api-streaming`)

- One generation at a time is now a FIFO queue: `limits.concurrency.max_queued_generations`
  (derived default 4) with `queue_timeout_ms` (default 300 000). An arrival past
  the queue gets the fixed 503 `server_busy` + `Retry-After: 1` at once, before
  any engine byte; a waiter that leaves (disconnect or deadline) leaves the queue.
- `GEN <max_new> [temperature=F] [top_p=F] [seed=N] <ids>` — the sampling keys
  carry only schema-bounded values (temperature 0–2, top_p (0,1], seed
  0–2^63−1; decision 6/B6), rendered by the frontend from parsed numbers
  (shortest round-trip form), never from request bytes. An empty rendering is
  greedy, exactly as before SP3.
- The context rule `n + max_tokens + 8 ≤ ctx` is applied in the frontend
  **before** a queue place (against the last READY context) and **again**
  against the lease's READY context; both answers are fixed 400
  `context_length_exceeded` / `invalid_request` bodies with no `GEN` sent.
- `max_tokens` is clamped to `limits.chat.max_completion_tokens` (decision B2,
  never rejected for being above the limit) and defaults to auto —
  `min(max_completion, ctx − n − 8)` — when omitted (decision B1).
- `STOP` now carries the engine stop cause (`client_cancelled` for a client
  disconnect, `generation_timeout`/shutdown otherwise — SP2 D-2 ruling) and
  the drain follows the existing 1 s / `stop_drain_timeout_s` rules above.

### SP3 M2 (streaming, branch `sp3-api-streaming`)

- Streaming exists: `POST /v1/chat/completions` with `"stream": true` is served
  as SSE under HTTP/1.1 chunked framing (spec §3.2). The `GEN`/`DONE` protocol
  framing above is unchanged by it.
- The frontend reads `T` records one at a time (`next_event`, 20 ms slices —
  `kStreamPollSlice`) at engine speed whatever the client does (§2.7): a slow
  reader never slows the engine. Decoded text held for one streaming worker is
  bounded by `limits.chat.max_generated_bytes`; content deltas coalesce to at
  most `kMaxDeltaTextBytes` (16 KiB) of text each and no SSE event exceeds
  `kMaxSseEventBytes` (128 KiB).
- A client that disconnects, stalls for `limits.http.write_stall_ms` without
  progress, or a server shutdown sends `STOP` and drains to `DONE … cancel`
  within `limits.engine.stop_drain_timeout_s`, else SP2's `drain_failed` path
  (kill, count, restart) applies. Shutdown bounds its drain and tail by
  `kShutdownDrainBudget` (2 s) from the first shutdown sight instead of the
  60 s tail deadline (R-H).
- A generation timeout returns the text so far with `finish_reason: length`
  (decision 4; §8 log-only code `generation_timeout`) instead of the M1 503.
- One generation at a time, now queued (the M1 FIFO rules above unchanged).
