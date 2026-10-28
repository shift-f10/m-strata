# Sub-project 3 — API concurrency, streaming, and the `NET-*`/`WEB-*`/`DOS-*`/`EXT-*` acceptance tests

Status: APPROVED 2026-10-14 — owner accepted every recommendation (see §11 "Decisions taken"). Every value below is the
recommended option of the matching decision; a different choice changes only
the rows that cite it.

Date: 2026-10-14 (fix round 1 after review `sp3-spec-review-1`). Parent
effort: Secure Strata full release path (SP0–SP7), see
`docs/AGENT_HANDOFF.md`. Base: branch `specs/sp3-api-streaming` at
`1d549f69`. Binding inputs (approved; consumed here, not redesigned):

- **PX** — [`2026-10-13-product-experience-and-upstream-tracking-design.md`](2026-10-13-product-experience-and-upstream-tracking-design.md)
  (`strata serve/run`, mandatory token on loopback, OpenAI-compatible base URL,
  no telemetry; §11 gives SP3 "streaming and cancellation needed by interactive
  `run`; concurrency limits; NET/WEB/DOS/EXT tests").
- **SP1** — [`2026-10-13-sp1-typed-config-paths-tokens-design.md`](2026-10-13-sp1-typed-config-paths-tokens-design.md)
  and its plan (`docs/superpowers/plans/2026-10-13-sp1-typed-config-paths-tokens.md`,
  README block `config-keys`): the typed schema, `secure_frontend_json`,
  `TokenVerifier` (fixed 256-byte window), `secure_frontend_add_cases`,
  `<binary>.<case>` test names, the `windows-runtime` label.
  `limits.chat.max_body_bytes` defaults to **49152** (= 65536 − 16384): the
  SP1 spec §4 lists 262144, but its own cross-field rule rejects that value,
  and the SP1 plan (Global Constraints) ships 49152. SP3 follows the plan.
- **SP2** — [`2026-10-13-sp2-portable-supervision-design.md`](2026-10-13-sp2-portable-supervision-design.md),
  its plan (`docs/superpowers/plans/2026-10-13-sp2-portable-supervision.md`,
  cited as **SP2 plan**) and the shipped code: `EngineSupervisor`
  (`acquire()` only in `ready`, `Lease`, `report_fatal`, `snapshot()`; one
  serving thread borrows the generation, `engine_supervisor.hpp:53, 94-95`),
  the eight-state machine and `/health` bodies, restart 3 / 1 s → 60 s /
  300 s window, stdin writes in 64 KiB chunks with a 1 s no-progress deadline
  (SP2 plan Task 22), `strata-supervision-fake-engine` and its mode flags, the
  server harness (`test_support::ServerProcess`), the `ext01_*`/`ext02_*`
  cases in `secure_frontend_server_tests`. Where the SP2 spec and the SP2 plan
  differ, the plan is what shipped: client-visible `ERR` lines count toward the
  circuit (SP2 plan Review Focus 6; `proc04_client_err_lines_open_circuit_until_restart`),
  which supersedes the SP2 spec §3.5 sentence "Other `ERR` lines keep SP0
  behavior".

**Configuration key names.** SP1's implemented data model is the truth:
`listener.*`, `limits.http.*`, `limits.chat.*`, `limits.engine.*`,
`limits.concurrency.*`, `roots.*`, `engine.*`, `token.source`. The product
spec's older sketch (`listen.*`, `http.allowed_hosts`, `http.cors_origins`,
flat `limits.*`, `token_source`) is superseded; SP3 uses SP1's names only
(`listener.cors_origins`, `listener.host_aliases`, …). Updating the product
spec text is a later docs pass, not SP3 work.

Upstream parity reference (read-only): `upstream/serve/server.py` at `v0.1.43`
(cited as `server.py:<line>`).

## 1. Goals, non-goals, scope, global constraints

### 1.1 Goals

1. **Concurrency.** Replace the one-request-at-a-time `serve_one` loop with a
   bounded accept thread plus a fixed worker pool, so `/health`, `/v1/models`
   and queued chats are answered while one generation runs, and excess load is
   refused predictably (`503` + `Retry-After`) instead of piling up in the
   kernel backlog.
2. **One generation at a time, fairly.** The engine protocol runs one
   generation per engine process (`engine-protocol-limits.md`). SP3 serializes
   chats behind a bounded FIFO gate that owns the single running slot in front
   of SP2's `acquire()`, with a queue timeout and cancellation on client
   disconnect.
3. **Streaming.** `POST /v1/chat/completions` with `"stream": true` returns
   OpenAI-compatible Server-Sent Events: role delta, content deltas decoded
   incrementally from `T` records, a final `finish_reason`, optional usage,
   `data: [DONE]`, in an HTTP/1.1 chunked body. A disconnect sends `STOP` and
   drains. This is what `strata run` needs (PX §2, §11).
4. **HTTP hardening and DoS bounds** for every byte the listener reads or
   writes, each with a config key or internal constant, a default, a range, an
   enforcement point and a status code (§5). Authentication happens as soon as
   the header is complete, before any body byte is read.
5. **Acceptance tests by ID** for every `NET-*`, `WEB-*`, `DOS-*`, `EXT-*` row
   of `tests/security/README.md` on Linux and Windows (§9), plus the runtime
   egress checks the frontend can give (§6).

### 1.2 Non-goals

Sampling beyond decision 6's subset, `stop` sequences, multi-part content,
`tools`, vision, reasoning fields, other model profiles and differential tests
against upstream (SP5); the GLM engine (SP6); `pull`/`import`/`update`, TLS and
where their networking code lives (SP4); log files on disk and their retention
(SP7); keep-alive, pipelining, HTTP/2, TLS on the listener; more than one
engine process (PX §4 "replicas: deferred"); any engine change; any change to
SP1's or SP2's shipped code beyond the SP1 schema addendum (§5.1), the
header-first split of the request handler (§4.1) and the test updates of
§9.4.

### 1.3 Scope and plans

One spec, **two plans**:

- **M1 — concurrency and hardening:** JSON writer and error table, early
  authentication, parser rules, Host/Origin/CORS, schema rows, connection
  pool with eviction and lingering close, generation gate, disconnect
  cancellation for non-streaming chats, egress checks, EXT amendments,
  logging, Windows cases. Every `NET-*`, `WEB-*`, `DOS-*`, `EXT-*` case that
  does not need streaming lands here.
- **M2 — streaming:** incremental protocol reads, UTF-8 stream decoder, SSE
  encoder with chunked framing, backpressure, keepalive, finish mapping and
  error events, the loopback client library and `strata-api-probe`, the GPU
  kit procedure, and the streaming `DOS-02` cases.

The error model, route table and limits are shared and are designed once
here; M2 starts after M1 is merged.

### 1.4 Fit between sub-projects

| From | SP3 consumes | SP3 gives |
| --- | --- | --- |
| SP1 | Typed `FrontendConfig` and its schema mechanism (SP3 **adds rows** and extends the mechanism, §5.1); `secure_frontend_json` with a per-call depth limit; `TokenVerifier`; canonical-root model inventory; `IpAddress` classification; test registration helpers. | New schema rows; `max_concurrent_requests` range widened from `1-1` to `1-256` (§5.1). |
| SP2 | `EngineSupervisor::acquire/report_fatal/snapshot`, `Lease`, `KillReason`, `HealthSnapshot`, the `/health` body table, shutdown order and 5 s bound, `shutdown_listener()`, the fake engine and server harness. | `SupervisorPort`, which serializes SP3's calls into the supervisor, and the gate that owns the running slot (§2.5); a streaming execution path over a `Lease`; new fake-engine modes (§9.2); amended `ext01` case (§6.4). |
| SP4 | — | The egress constraint for `serve` and its tripwire tests (§6.3). |
| SP5 | — | The request schema, error model, SSE encoder and route table SP5 extends. |
| PX `strata run` | — | `secure_frontend_api_client` (loopback-only SSE client, §3.10). |

### 1.5 Dependencies on SP2's second run

These are SP2 follow-ups SP3 relies on; they are not SP3 tasks. Until they
land, SP3 works around them as stated.

| Follow-up | Why SP3 needs it | Until then |
| --- | --- | --- |
| D-1. Lease-release notification, and documented thread-safety of `acquire`/`snapshot`/`report_fatal` | SP3 has many workers; SP2 was built for one serving thread | **Landed (SP2 run 2, main 8333eccf).** `acquire`/`snapshot`/`report_fatal`/`release` are documented thread-safe (one internal mutex), and `wait_for_change(from, timeout)` returns when the supervisor STATE differs from `from` (it observes states, not transitions: `wait_for_change(generating, t)` wakes on lease release → `ready`). SP3's `SupervisorPort` (§2.5) may still serialize its own calls; the gate still learns of release from the holder's `gate.release()`. |
| D-2. A drain failure after a client-requested `STOP` restarts the engine but does not count toward the circuit | A client that disconnects during a long real prefill could otherwise open the circuit (risk R4) | **Landed (SP2 run 2, main 8333eccf).** SP3 sends the cancel as `stop(EngineStopCause::client_cancelled)`; the supervisor derives the reason through `kill_reason_for(protocol)` (`KillReason::client_stop_drain_failed`), restarts at once with a zero-length `backoff`, and leaves the failure count unchanged. Bound: at most `config_defaults::kMaxConsecutiveUncountedRestarts` (3) such restarts in a row with no successful `DONE` between them; later ones count like `drain_failed`. |

### 1.6 Global constraints (binding on both plans)

- **No third-party dependencies** in `secure_frontend/`: standard library and
  OS APIs only (Windows: `ws2_32`; `iphlpapi` in one test only).
- **`-Werror`** on every target (`secure_frontend_set_warnings`, through
  `secure_frontend_add_test_executable` for tests).
- **TDD**: every row of §9 is written first, run to fail for the stated
  reason, then fixed; full `ctest --preset linux-gcc` and the
  `windows-llvm-mingw-cross` build green before each local commit.
- **No change under `upstream/`** and none to our root `CMakeLists.txt`; stop
  and ask the owner first.
- **The engine never gets network flags.** SP0's deny-list and SP2's
  allowlist are unchanged; the only engine input SP3 adds is the bounded
  `GEN` `k=v` subset of decision 6.
- **Configuration only through SP1's typed keys.** Every SP3 tunable is a
  schema row (§5.1); every other number is a named internal constant in one
  header (`secure_frontend/include/secure_frontend/api_constants.hpp`, §5.3).
  No environment variable is read.
- **Fixed-reason rule.** Errors, logs and SSE error events never contain a
  token, a prompt or completion, a request header or body value, an option
  value, a configured value, a path, an engine line, or an exit code. Each is
  one of the fixed literals of §7.
- **Thread rule (SP2 §3.1 kept).** Only SP2's supervisor thread forks, reaps
  and kills engines. Every explicit SP3 call into `EngineSupervisor`
  (`snapshot`, `acquire`, `report_fatal`) goes through `SupervisorPort` (one
  mutex, one call at a time); `Lease` destruction and `main`'s `shutdown()`
  are the two exceptions (§2.5). Only the worker holding the single `Lease`
  touches the protocol client.
- **SP1 and SP2 merge first.** SP3 tasks start on `main` after both have
  merged; the SP3 plans re-anchor on their files. SP1 is not edited by SP3
  except through the schema rows and mechanism extensions of §5.1, each
  recorded as an SP1 addendum in the handoff.
- Commits are local and the owner pushes. `docs/AGENT_HANDOFF.md` is updated
  in every task.

## 2. Request lifecycle and concurrency model

### 2.1 Threads

| Thread | Count | Does | Never does |
| --- | --- | --- | --- |
| Accept | 1 | `poll`/`WSAPoll` the listener in 100 ms slices (`kAcceptPollSlice`), `accept`, classify the peer, apply the connection caps (§2.3), hand the socket to an idle worker or refuse it, service the lingering-close list (§2.4). | Read a request byte; call the router or the supervisor. |
| Worker | `limits.concurrency.max_concurrent_requests` (default 8), created at start, joined at shutdown | One connection at a time: read the header, authenticate, route, read the body, respond (complete or streaming), finish, close. | Spawn threads; call `acquire()` unless it is the gate head (§2.5). |
| Supervisor | 1 (SP2) | Everything in SP2 §3.1. | — |
| `main` | 1 | Validate config, start the supervisor, bind, start accept + workers, wait for the SP2 shutdown signal, run §2.8. | Serve requests. |

A fixed pool keeps the thread count constant under any load, which makes
`DOS-02`'s "resources are released" testable as "thread and descriptor counts
return to their idle values" (engineering choice E1, §11.2). With
`max_concurrent_requests = 1` the server is serial, as today.

### 2.2 Per-connection phases and their bounds

Times run from `accept()`. All deadlines use `std::chrono::steady_clock`.
Every read and wait in P1–P5 is a non-blocking `recv` after `poll` in 20 ms
slices (`kStreamPollSlice`), so a worker sees its abort flag (eviction,
shutdown) within one slice; SP0's blocking `SO_RCVTIMEO` reads are replaced.

| Phase | Ends when | Bound | On expiry |
| --- | --- | --- | --- |
| P1 header | `\r\n\r\n` received | `limits.http.header_deadline_ms` (10 000) | `408 request_timeout`, close |
| P2 authorize | request line + header parsed, Host/Origin, preflight, bearer token, route, method, `Content-Type`, `Content-Length` limits (§4.1 steps 2–8) | CPU only | the §4.1 status; the unread body is drained (§2.4) |
| P3 body | `Content-Length` bytes received (authenticated clients only) | `limits.http.request_deadline_ms` (30 000) total since accept | `408 request_timeout`, close |
| P4 validate | schema, tokenize, context rule (§5.4) | CPU only; bounded by `limits.chat.max_body_bytes` | — |
| P5 queue (chat only) | the gate grants the running slot and `acquire()` returns a lease | `limits.concurrency.queue_timeout_ms` (300 000) | `503 server_busy` + `Retry-After: 1` |
| P6 generate | `DONE`, `ERR`, failure, or timeout | `limits.engine.generation_timeout_s` (120) then `STOP` + `limits.engine.stop_drain_timeout_s` (5) | §3.4 |
| P7 write | last byte sent | per send: `limits.http.write_stall_ms` (10 000) with no progress; whole complete response, or the stream tail after `DONE`: `limits.http.response_write_deadline_ms` (60 000) | connection closed; a running generation is stopped (§2.6) |
| P8 finish | peer EOF or 100 ms / 256 KiB drained (SP0) | SP0 constants | close |

There is no keep-alive (§3.2, engineering choice E7): every response carries
`Connection: close`, so there is no idle phase between requests and P1 is
also the idle timeout.

**Authentication before the body (fixes the slow-body starvation).** The
token is checked at the end of P1, before any body byte is read, so an
unauthenticated client can hold a worker only in P1, where it is bounded by
`header_deadline_ms` and evictable (§2.3). A client in P3 has presented a
valid token; the threat model already treats a token holder as able to use
the API's capacity, and P3 stays bounded by `request_deadline_ms`. No minimum
body rate is added: it would only constrain trusted clients further.
`Expect: 100-continue` is answered with `HTTP/1.1 100 Continue` at the end of
P2, when every header-level check has passed.

### 2.3 Admission at accept time

1. **Peer class.** The accept thread reads the peer address once:
   *loopback* (`127.0.0.0/8`, `::1`, `::ffff:127.0.0.0/104`, classified from
   bytes as SP1 §3.7 does) or *remote*.
2. **Per-remote-address cap.** A remote address may hold at most
   `limits.concurrency.max_connections_per_remote_address` (4: derived; at the default 8 workers)
   workers at once. Loopback peers are exempt: every local client shares one
   address, so a per-address cap there would limit `strata run` and the user's
   own apps together (residual: decision 12). Over the cap → refusal (step 4).
3. **Idle worker.** If a worker is idle it gets the socket.
4. **No idle worker.** If at least one worker is still in P1 (not yet
   authenticated), the accept thread **evicts** the worker that has been in P1
   the longest: it sets that worker's abort flag; the worker sees it within
   one 20 ms slice, closes its connection without a response, and takes the
   new socket from the accept thread's hand-off slot. A legitimate client
   sends its header in one write and leaves P1 within milliseconds, so it is
   almost never the oldest P1 connection; a slow-header client must open
   connections faster than it is evicted to hold a worker. If no worker is in
   P1, the new connection is **refused**: the accept thread writes the fixed
   `503 server_busy` response (`Retry-After: 1`, ≤ 512 bytes) with one
   non-blocking `send`, half-closes, and puts the socket on the
   lingering-close list (§2.4). It never reads the request.

The accept thread never blocks on a client: every call it makes on an
accepted socket is non-blocking. Authenticated connections are never evicted.

### 2.4 Lingering close

Closing a socket with unread request bytes makes the kernel send RST, which on
Windows can destroy the response before the client reads it (SP0 S4). Workers
keep SP0's half-close + 100 ms / 256 KiB drain (also after an early `401`,
whose body was never read). The accept thread cannot spend 100 ms per refused
client, so it keeps up to 64 refused sockets (`kLingerListMax`) on a list it
polls with the listener; each is closed on peer EOF, on error, after 256 KiB
drained, or 100 ms after refusal. When the list is full the oldest entry is
closed at once.

### 2.5 The generation gate and `SupervisorPort`

**`SupervisorPort`** wraps the one `EngineSupervisor`. Every call into it
(`snapshot`, `acquire`, `report_fatal`) takes one mutex for the duration of the
call, so SP3's own calls reach SP2 one at a time (SP2 was built for a single
serving thread; D-1).
`/health` workers call `snapshot()` through the port; the lease holder calls
`report_fatal()` through the port. Two entries into SP2 do not pass through the
port: `Lease::~Lease()` (which calls the supervisor's release on the holder's
thread) and `EngineSupervisor::shutdown()` (called by `main`, §2.8). Both rely
on SP2's internal mutex (`engine_supervisor.cpp:106-145`: `acquire`,
release, `report_fatal` and `snapshot` already lock it), which is
implementation behavior, not a documented contract, until D-1 documents it.
The port therefore serializes SP3's own calls; it does not claim that SP2 sees
only one caller at a time. The gate's job is different: it decides *who* may
call `acquire()` (§2.5 "Granting the slot").

**`GenerationGate`** is a FIFO of tickets that owns the single *running* slot:

- **Admission.** A validated chat request (P4 passed) asks for a ticket. If
  `limits.concurrency.max_queued_generations` (4: derived; at the default 8 workers) tickets already wait,
  it gets `503 server_busy` + `Retry-After: 1` at once, before any stream
  header is sent.
- **Fast fail on engine state.** If `snapshot()` is not `ready` or
  `generating`, the request gets `503 engine_unavailable` at once (SP2 V11:
  chats never wait for `starting`/`backoff`/`circuit_open`); `Retry-After: 5`
  unless `recovery=exhausted`, which carries none (§3.8).
- **Granting the slot.** Only when the slot is free does the head proceed. It
  calls `snapshot()`: `ready` → it calls `acquire()`; `starting`, `stopping`,
  `backoff`, `circuit_open`, `shutting_down`, `stopped` → `503
  engine_unavailable` (or `server_shutting_down`), and the next ticket becomes
  head; `generating` with a free slot (a release still settling inside SP2) →
  the head re-checks every 20 ms until its queue deadline. `acquire()` returning
  empty after `ready` (the engine died between the two calls) → `503
  engine_unavailable`. A lease → the slot is taken, P6 begins. The context rule
  is re-checked against the lease's READY context, because a restarted engine
  may advertise another one.
- **Release.** The holder destroys its `Lease` (SP2 moves `generating` →
  `ready`), then calls `gate.release()`, which wakes the next head. The gate
  never wakes a head while a `Lease` exists.
- **While waiting**, each waiter polls its own socket every 20 ms for
  disconnect (§2.6). A gone client leaves the queue silently. Queue timeout →
  `503 server_busy`.
- **Fairness.** Strict FIFO by admission time across all peers. A remote
  address can hold at most `max_connections_per_remote_address` workers and
  therefore at most that many of the generation places.
- **Spare workers.** With `max_concurrent_requests ≥ 2`, validation enforces
  `max_queued_generations + 2 ≤ max_concurrent_requests`, so with every
  generation place taken at least one worker remains for `/health`,
  `/v1/models` and refusals. With `max_concurrent_requests = 1` (serial),
  `max_queued_generations` must be 0. Default: 8 ≥ 4 + 2.

The non-streaming and streaming paths share the gate; `/health` and
`/v1/models` never enter it.

### 2.6 Cancellation

A chat is cancelled when its client is gone (engineering choice E5):

| Signal | Detected by |
| --- | --- |
| Peer EOF (`recv(MSG_PEEK)` returns 0) or reset, polled every 20 ms in P5/P6 | queue waiter, generating worker (stream and non-stream) |
| Any byte received after the body in P5/P6 (pipelining is not supported, and a stray byte would hide a later EOF from the peek probe) | same; treated as gone |
| A `send` fails or makes no progress for `write_stall_ms` | streaming worker; non-streaming worker in P7 |
| Shutdown (§2.8) | every worker |

Bytes beyond `Content-Length` that arrive together with the request are a
`400 invalid_request` at P3. Cancel in P5: leave the queue; nothing is sent.
Cancel in P6: send `STOP` with `EngineStopCause::client_cancelled`, drain to
`DONE … cancel` within `stop_drain_timeout_s`, destroy the lease, release the
slot. A drain that fails is reported through the port with the reason
`kill_reason_for(protocol)` returns (`client_stop_drain_failed`): the engine is
killed and restarted at once, and the failure is NOT counted toward the circuit
(D-2, landed in SP2 run 2) — up to `kMaxConsecutiveUncountedRestarts` (3)
consecutive times with no successful `DONE` in between; past that bound it
counts like `drain_failed`. Nothing more is written to a gone client.

Peer EOF is treated as "gone" only for chat requests. A client that
half-closes after sending a `GET` still receives its answer, so SP0's
`socket_server_responds_after_client_half_close` keeps passing for `GET`.

### 2.7 Backpressure: a slow reader never slows the engine

During P6 the generating worker loops: read one engine record with a 20 ms
budget through the lease; decode; append the text to the pending buffer; if
the socket is writable, render and send events without blocking (at most
64 KiB per `send`, `kMaxSendChunk`). The engine's stdout is always read at
engine speed, so a slow client cannot stall the engine's stdout pipe (which
would stall the GPU loop and risk the engine's watchdog abort). The pending
buffer holds **decoded text**, at most `limits.chat.max_generated_bytes`
(1 MiB) per stream; events are rendered from it only when the socket is
writable, one event of at most `kMaxSseEventBytes` (128 KiB) at a time (§3.2),
so memory per streaming worker is bounded by 1 MiB + 128 KiB.

Write deadlines (engineering choice E4):

- **Stall:** pending output that has made no progress for `write_stall_ms`
  (10 s) means the client is gone: cancel (§2.6).
- **Tail:** after `DONE`, the remaining bytes must be sent within
  `response_write_deadline_ms` (60 s); otherwise close without the terminal
  chunk.
- **Complete responses** (non-streaming chat, `/health`, `/v1/models`,
  errors): stall rule per send, whole response within
  `response_write_deadline_ms`. This replaces SP0's `kMinimumSendBudget`
  (1000 ms) and the reuse of the request deadline for sending.

SP2's 1 s per 64 KiB chunk is for a local pipe into a process we own, where 1 s
without progress means a hung engine. A client on a LAN or a busy desktop can
pause for seconds without being gone; the two rules protect different peers
and stay separate.

### 2.8 Shutdown

On SP2's shutdown signal: the accept thread stops accepting and closes the
listener (`shutdown_listener()`); the lingering list is closed; every worker's
abort flag is set; queued tickets get `503 server_shutting_down`; a streaming
worker sends the in-stream error `server_shutting_down`, `data: [DONE]`, the
terminal chunk, and closes; a non-streaming generating worker answers `503
server_shutting_down`; the lease holder sends `STOP` and drains only within the
remaining SP2 bound. After the shutdown flag is set, every worker-side failure
(drain timeout, protocol error, write failure) is reported to the client as
`server_shutting_down` and **never** as `report_fatal` — the supervisor is
already tearing the generation down. Workers are joined; anything still
running when SP2's 5 s bound ends is covered by SP2's `_exit(1)`. SP2 plan
Review Focus 3 (`proc05_sigterm_during_generation_exits_cleanly`) stays green.

## 3. Streaming and response shapes

### 3.1 Request schema (`POST /v1/chat/completions`)

Strict, parsed with `secure_frontend_json` at depth ≤ 4 (`kChatMaxJsonDepth`;
the deepest legal value is `messages[i].content` at depth 3), body ≤
`limits.chat.max_body_bytes`, duplicate keys rejected (SP1 C1).

| Field | Type / rule | SP3 behavior |
| --- | --- | --- |
| `model` | string, SP0 grammar, ≤ 256 bytes | must equal `engine.model_id`, else `404 model_not_found` |
| `messages` | array, 1..`limits.chat.max_messages`; each `{role, content}` | `role` ∈ `system`, `user`, `assistant`; `content` string, UTF-8, ≤ `limits.chat.max_message_bytes`; SP0 control-byte and remote-scheme rules |
| `stream` | boolean | `true` → SSE (§3.2); `false` or absent → complete response (§3.5) |
| `stream_options` | object `{ "include_usage": boolean }`, only with `stream: true` | usage chunk (§3.2) |
| `max_tokens` | integer ≥ 1 | above `limits.chat.max_completion_tokens` or above the room left in the context → clamped (decision 2); absent → decision 1 |
| `max_completion_tokens` | same as `max_tokens` (OpenAI's newer name) | both present → `400 invalid_request` |
| `n` | integer | `1` accepted; other → `400 unsupported_parameter` |
| `user` | string ≤ 256 bytes | accepted, ignored, never logged |
| `temperature`, `top_p`, `seed` | bounded numbers (decision 6) | passed to the engine as `GEN` `k=v` |
| `presence_penalty`, `frequency_penalty` | number | `0` accepted as a no-op; other → `400 unsupported_parameter` until SP5 |
| `logit_bias` | object | `{}` accepted as a no-op; other → `400 unsupported_parameter` |
| `stop` | string or array | `null` or `[]` accepted as a no-op; other → `400 unsupported_parameter` |
| `logprobs` | boolean | `false` accepted; `true` → `400 unsupported_parameter` |
| `top_k`, `min_p`, `top_logprobs`, `response_format`, `content` as an array | — | `400 unsupported_parameter` until SP5 |
| `tools`, `tool_choice`, `functions`, `function_call`, `parallel_tool_calls`, `mcp_servers`, role `tool`/`function` | — | `400 extension_disabled` (EXT-01; §6.4) |
| any other key | — | `400 invalid_request` |

For `unsupported_parameter` and `extension_disabled` the error's `param`
field carries the matched key, taken from the fixed lists above (never request
bytes); the `message` stays the fixed literal of §7.2.

### 3.2 SSE response

Status line and headers, in this order, then the event stream:

```
HTTP/1.1 200 OK
Content-Type: text/event-stream; charset=utf-8
Transfer-Encoding: chunked
Cache-Control: no-store
Connection: close
X-Accel-Buffering: no
<security headers, §4.6>
```

**Framing (decision 5).** The body uses HTTP/1.1 chunked transfer coding: each
SSE event or comment is one chunk (`<hex size>\r\n<bytes>\r\n`), and a stream
that ends normally — including one that ends with an in-stream error event —
ends with the terminal chunk `0\r\n\r\n` before the half-close. A stream cut
off by a stall, a client-gone cancel, or a frontend crash has no terminal
chunk, so `httpx`/`openai-python` raise a protocol error instead of treating
the truncated stream as complete. `[DONE]` is not a success marker: it also
follows error events.

Headers are sent only after the lease is acquired and the context rule
re-checked (§2.5), so every refusal before generation (`server_busy`,
`engine_unavailable`, validation errors) is a normal HTTP status.

Each event is `data: <one-line JSON>\n\n`. Generated text is JSON-escaped by
the shared writer (§3.9), so a raw `\n` never appears inside an event. The
sequence is:

1. Role chunk:
   `{"id":"chatcmpl-<32 hex>","object":"chat.completion.chunk","created":<unix s>,"model":"<engine.model_id>","choices":[{"index":0,"delta":{"role":"assistant","content":""},"logprobs":null,"finish_reason":null}]}`
2. Zero or more content chunks: same envelope, `"delta":{"content":"<text>"}`.
3. Final chunk: `"delta":{}`, `"finish_reason":"stop"|"length"` (§3.4).
4. Only if `stream_options.include_usage` is `true` and the stream finished
   normally: a usage chunk with `"choices":[]` and
   `"usage":{"prompt_tokens":n,"completion_tokens":k,"total_tokens":n+k}`
   (OpenAI semantics; upstream instead puts usage in the final chunk of a
   normally finished stream, `server.py:4377-4385`).
5. `data: [DONE]\n\n`, the terminal chunk, then half-close, drain, close.

`id` is `chatcmpl-` plus 16 bytes from SP1's CSPRNG in lowercase hex, the
same in every chunk of one response. `created` is wall-clock seconds,
identical across chunks. `model` is the configured `engine.model_id` (the
request's value already equals it, so nothing is reflected).

**Flush policy and event cap.** Text decoded since the last content chunk is
emitted when the socket is writable, as content chunks of at most
`kMaxDeltaTextBytes` (16 KiB) of text each, split only at UTF-8 code-point
boundaries. Escaping grows text by at most 6× (a C0 byte becomes `\u00XX`), so
one event is at most 6 × 16 KiB + the envelope (< 1 KiB) ≤ `kMaxSseEventBytes`
(128 KiB). On a fast reader that is one chunk per `T` record that produced
text; on a slow reader deltas coalesce up to the cap, so framing overhead does
not grow with backpressure and no event exceeds the client's limit. Sockets
get `TCP_NODELAY`.

**Keepalive.** After the headers and while no content chunk has been sent
for `limits.http.stream_keepalive_ms` (15 000) — long prefill, or the GLM
flavor's `PP` progress records — the worker sends the SSE comment
`: keep-alive\n\n` (upstream sends the same comment, `server.py:5382`).
Before the headers (queue wait) nothing is sent (risk R6).

### 3.3 From `T` records to deltas

`EngineProtocolClient` gains an incremental reader (`next_event(timeout)` →
token, done, progress, or error; §3.11); `read_generation` is rebuilt on it, so
both paths parse identically. Each token id is mapped to its bytes by the
loaded tokenizer pack. `Utf8StreamDecoder` appends the bytes and releases the
longest prefix that is complete UTF-8; an incomplete trailing sequence (≤ 3
bytes) is held for the next token.

- **Invalid sequence in the middle of the text** (a byte that can never
  start or continue a valid sequence): replaced by U+FFFD (decision 3), and the
  decoder continues.
- **Incomplete tail at the end** (held bytes when the generation ends):
  dropped for `length` and `cancel` endings (the cut is ours or the limit's,
  not the model's); for a `stop` ending it is handled like an invalid sequence
  (one U+FFFD).
- The non-streaming path uses the same decoder, which also fixes today's
  latent failure: `decode_token_ids` (`tokenizer_pack.cpp:308`) rejects a
  whole response whose last token was cut mid-character by `max_tokens`.

Released text counts against `limits.chat.max_generated_bytes`; reaching it
sends `STOP`, drains, and ends with `finish_reason:"length"`.

### 3.4 `finish_reason` and terminal outcomes

`DONE` field 6 is one of `stop`, `length`, `cancel`, `nonfinite`
(`upstream/src/program/generate.cpp:6203`); the fake engine's short
`DONE stop` form is accepted as today.

| Engine / frontend outcome | Non-streaming | Streaming |
| --- | --- | --- |
| `DONE … stop` (EOS) | `200`, `"stop"` | final chunk `"stop"` |
| `DONE … length` (`max_tokens` reached, or context end) | `200`, `"length"` | final chunk `"length"` |
| `max_generated_bytes` reached → `STOP` → `DONE … cancel` | `200`, `"length"` | final chunk `"length"` |
| generation timeout → `STOP` → `DONE … cancel` within drain (decision 4) | `200`, text so far, `"length"` | final chunk `"length"` |
| client gone → `STOP` → `DONE … cancel` | nothing sent | nothing sent |
| shutdown | `503 server_shutting_down` | error event `server_shutting_down` |
| `DONE … nonfinite` (engine stays idle, SP0 E3) | `503 engine_error` | error event `engine_error` |
| `ERR …` or a fatal prefix (SP2 kills, restarts, counts — SP2 plan Review Focus 6) | `503 engine_error` | error event `engine_error` |
| engine exit, protocol failure, drain failure (SP2 states `stopping`/`backoff`) | `503 engine_unavailable` | error event `engine_unavailable` |

`engine_error` responses carry `x-should-retry: false`, which `openai-python`
honors, so an SDK's automatic retries (default 2) do not turn one client-caused
`ERR` into three counted failures and an open circuit (risk R5).

The streaming error event uses OpenAI's documented error shape:
`data: {"error":{"message":"<fixed>","type":"server_error","param":null,"code":"<code>"}}\n\n`
followed by `data: [DONE]\n\n`, the terminal chunk, then close. Upstream sends
a subset of this shape (`{"error":{"type","message"}}`, a `code` only for
structured-output errors, `server.py:5391-5402`); `openai-python` raises
`APIError` for both. A `finish_reason` is never sent after an error event.

### 3.5 Non-streaming response

```
{"id":"chatcmpl-<32 hex>","object":"chat.completion","created":<unix s>,"model":"<engine.model_id>",
 "choices":[{"index":0,"message":{"role":"assistant","content":"<text>"},"logprobs":null,"finish_reason":"stop"|"length"}],
 "usage":{"prompt_tokens":n,"completion_tokens":k,"total_tokens":n+k}}
```

`Content-Type: application/json; charset=utf-8`, `Content-Length`,
`Connection: close`. Replaces today's `chatcmpl-local`, `created:0`, fixed
`"stop"` and zero usage (`chat_schema.cpp:337-341`). All complete responses
(including errors) use `Content-Length`; only streams are chunked.

### 3.6 Routes

The router owns one table, `kRouteTable`, which the auth and EXT tests
iterate.

| Method | Path (exact bytes) | Response |
| --- | --- | --- |
| `GET` | `/health` | SP2 body unchanged: `{"status","engine"[,"recovery"]}`; `200`/`503` per SP2 §3.8; never queued |
| `GET` | `/v1/models` | `{"object":"list","data":[{"id":"<engine.model_id>","object":"model","created":0,"owned_by":"<owned_by>"}]}` — only the served model (E9) |
| `GET` | `/v1/models/<id>` | the same object when `<id>` equals `engine.model_id`, else `404 model_not_found` |
| `POST` | `/v1/chat/completions` | §3.1–§3.5 |

Everything else after authentication is `404 not_found` (unknown path) or
`405 method_not_allowed` with `Allow` (known path, other method). Not in SP3:
`/v1/completions` (decision 7; upstream has none either), `/v1/messages`
(decision 8; upstream routes it at `server.py:5084-5085`), `/v1/responses`,
Ollama-native `/api/*` (decision 9), and upstream's control routes
(`/v1/load`, `/settings`, `/config`, `/slots`, `/metrics`, `/props`, `/mcp`),
which stay absent (EXT-01).

### 3.7 `/health` and the queue

`/health` does not report queue depth: SP2 froze its body ("no `/health`
change beyond adding `recovery`"), and queue length is load information an
authenticated client can infer from `503 server_busy` anyway.

### 3.8 Retry-After

Fixed integers (seconds), never computed from load:
`server_busy` → `1`; `engine_unavailable` with `engine=starting` or
`recovery=restarting` → `5`; with `recovery=exhausted` → none (an operator
must restart); `server_shutting_down` → none.

### 3.9 One JSON writer

All responses (router, chat, SSE, errors) use one escaping writer: `"` `\`
and C0 controls escaped, ` `/` ` passed through, UTF-8 copied
unchanged after validation. This replaces the two copies in
`api_router.cpp` and `chat_schema.cpp`; the router copy escapes every byte
≥ 0x80 as `\u00XX`, which corrupts non-ASCII model ids (fixed, with test
`models_list_keeps_utf8_model_id`).

### 3.10 Loopback client for `strata run` and the tests

`secure_frontend_api_client` (library) connects only to a `LoopbackAddress`,
a new type that can be built only from an SP1 `IpAddress` the SP1 classifier
reports as loopback (a remote address is a compile error at the call site and
a runtime refusal at construction). It sends one request with the bearer token
from SP1's token store and parses either a complete response or the chunked
SSE stream above (events ≤ `kMaxSseEventBytes`, `[DONE]` and the terminal
chunk required). `strata run` takes the server address from PX's
`server.json`; if that address is not loopback (an operator bound the only
listener to a LAN address), `run` stops with a fixed message naming
`listener.host` — a LAN-only server is not reachable by `run` in SP3 (§12.1).
`run` prints content deltas as they arrive; Ctrl-C closes the connection,
which cancels (§2.6). The tests use the same library as their fake client,
and the test-only `strata-api-probe` executable wraps it for the GPU
validation kit (§9.5).

### 3.11 Interface sketch (exact names fixed in the plans)

```cpp
// http_socket_server.hpp — replaces serve_one-in-a-loop; serve_one stays as a test seam.
struct ServerLimits final { HttpLimits http; ConcurrencyLimits concurrency; };   // from FrontendConfig
class ConnectionPool final {           // accept thread + fixed workers
 public:
  ConnectionPool(HttpSocketServer& listener, ServerLimits, RequestRouter&);
  void run_until(const std::atomic<bool>& stop);   // returns after §2.8
};
// Header-first routing: authorize() runs at the end of P1 (P2), handle() after the body (P3).
struct RequestHead final { std::string_view method, target; /* parsed headers */ };
struct RouteDecision final { std::optional<HttpResponse> early;   // set → answer now, body unread
                             bool expect_continue{false}; std::size_t body_limit{0}; };
class RequestRouter { public: virtual RouteDecision authorize(const RequestHead&) = 0;
  virtual RouteResult handle(const RequestHead&, std::string_view body) = 0; virtual ~RequestRouter() = default; };
// Router results: complete or streaming.
struct StreamSink { virtual bool send_headers(const HttpHead&) = 0; virtual bool send_event(std::string_view) = 0;
  virtual bool writable(std::chrono::milliseconds) = 0; virtual bool client_gone() = 0;
  virtual void finish() = 0;                      // [DONE] already sent → terminal chunk
  virtual ~StreamSink() = default; };
using StreamHandler = std::function<void(StreamSink&)>;
using RouteResult = std::variant<HttpResponse, StreamHandler>;
// One mutex around SP3's explicit calls (snapshot/acquire/report_fatal); ~Lease and shutdown() use SP2's own mutex (§2.5, D-1).
class SupervisorPort final { public: HealthSnapshot snapshot(); std::optional<Lease> acquire();
  void report_fatal(const Lease&, KillReason); };
// Generation gate: owns the running slot.
enum class GateRefusal { busy, timeout, cancelled, engine_unavailable, shutting_down };
class GenerationGate final { public:
  std::variant<Lease, GateRefusal> admit(SupervisorPort&, std::chrono::steady_clock::time_point deadline,
                                         const std::function<bool()>& client_gone);
  void release() noexcept; };      // called by the holder after its Lease is destroyed
// Incremental protocol reads.
struct TokenEvent { std::uint32_t id; }; struct DoneEvent { std::string_view finish; std::size_t generated; };
struct ProgressEvent {};
using GenerationEvent = std::variant<TokenEvent, DoneEvent, ProgressEvent, EngineProtocolError>;
GenerationEvent EngineProtocolClient::next_event(std::chrono::milliseconds timeout);
class Utf8StreamDecoder final { public: std::string push(std::string_view bytes);   // invalid → U+FFFD
  std::string finish(bool drop_incomplete_tail); };
class LoopbackAddress final { public: static std::optional<LoopbackAddress> from(const IpAddress&); };
```

## 4. HTTP hardening (`WEB-*`, `NET-*`)

### 4.1 Order of checks

The header is handled completely before any body byte is read (§2.2).

1. Listener bounds while reading the header (§2.2, §5): `408`, `414`, `431`.
2. Request syntax (§4.2): `400`, `411`, `505`, `417`, and the whole-request
   `413` (`Content-Length` over `max_request_bytes` − header bytes; a global
   limit, so answering it before authentication reveals nothing).
3. Host and Origin policy (§4.3): `400` (malformed, duplicate) or
   `403 forbidden` (untrusted).
4. CORS preflight (§4.4), before authentication, as today.
5. Bearer token (SP1 `TokenVerifier`): `401 invalid_api_key` with
   `WWW-Authenticate: Bearer`.
6. Route and method: `404`, `405`.
7. Route header checks: `415` (`Content-Type`), `413` (`Content-Length` over
   the route's own body limit, e.g. `limits.chat.max_body_bytes`; route
   specific, so after authentication).
8. `100 Continue` if requested; then the body (P3): `408`; stray bytes `400`.
9. Body checks: schema `400`, `404 model_not_found`.

Nothing about routes, models or the engine is revealed before step 5, so an
unauthenticated client sees the same answers on every path (AUTH-01). SP1's
`handle_http_request(whole message)` is split into a header stage and a body
stage to implement this order; the AUTH-01..04 tests stay green.

### 4.2 Request syntax

| Rule | Result |
| --- | --- |
| Request line `METHOD SP target SP HTTP/1.1`, CRLF line ends, single spaces | else `400 invalid_request` |
| Version `HTTP/1.0` or `HTTP/2.0` | `505 http_version_not_supported`; any other version string `400` |
| Method is an RFC 9110 token; served methods `GET`, `POST`, `OPTIONS` (preflight only) | another token → after auth `405` (unauthenticated: `401`) |
| Target is origin-form: starts with `/`, bytes `0x21–0x7E`, no `?`, `#`, `%`, `\`; length ≤ `limits.http.max_request_line_bytes` | else `400`; over length `414 request_line_too_long` |
| Path matched byte-for-byte against `kRouteTable`; no percent-decoding, no dot-segment removal, no `//` collapsing, no trailing-`/` stripping, no case folding | `404` |
| Header lines `name: value`; name an RFC 9110 token; value bytes `0x20–0x7E` or tab; no obs-fold, no bare LF, no NUL | else `400` |
| Header count ≤ `limits.http.max_headers`; header block ≤ `limits.http.max_header_bytes` | `431 header_too_large` |
| Exactly one `Host` (§4.3) | else `400` |
| `Transfer-Encoding` present (any value, any case, any spacing) | `411 length_required` (E3) |
| `Content-Length`: at most one, 1–10 ASCII digits | duplicate/malformed `400`; over `max_request_bytes − header bytes` `413` at §4.1 step 2, before authentication and before reading the body |
| Bytes received beyond `Content-Length` with the request | `400 invalid_request` |
| `GET` with `Content-Length` > 0 | `400` |
| `POST` without `Content-Length` | `411 length_required` |
| `Expect: 100-continue` | `100 Continue` sent after step 7 of §4.1; any other `Expect` → `417 expectation_failed` |
| `POST /v1/chat/completions` `Content-Type` not `application/json` (optional `; charset=utf-8`, case-insensitive) | `415 unsupported_media_type` |
| `Connection`, `Upgrade`, `TE`, `Keep-Alive` | ignored; the response is always `Connection: close`; no upgrade is ever performed |

### 4.3 Host

The allowed `Host` values are computed once at startup from the validated
listener (SP1 canonical text form): the bound authority (`127.0.0.1:8080`,
`[::1]:8080`); for a loopback bind also `localhost:<port>` (E10); plus each
entry of `listener.host_aliases` (§5.1, exact `host[:port]` authorities, e.g. a
LAN DNS name the operator's clients use). Comparison is byte-for-byte after
one rule: ASCII case is folded for names (not for the port). An IP literal
other than the bound one, an omitted or different port, a trailing dot,
userinfo, a control byte, or a second `Host` is rejected (WEB-01). A wildcard
bind (`0.0.0.0`, `::`) has no bound authority a client could use, and
interface addresses are not enumerated: its allowlist is exactly
`listener.host_aliases`, which must then be non-empty (validation error naming
the key otherwise), so a wildcard listener never trusts a Host the operator did
not name. DNS rebinding stays blocked: an attacker's name is never `localhost`
and never an alias the operator configured.

### 4.4 Origin and CORS (decision 10)

- No `Origin` → allowed (non-browser clients), then authentication.
- `Origin` equal to the same-origin value (`http://` + an allowed Host
  value) → allowed, no CORS headers.
- `Origin` in `listener.cors_origins` (default `[]`) → allowed, and the
  response carries `Access-Control-Allow-Origin: <that exact origin>` and
  `Vary: Origin`. Never `*`, never `Access-Control-Allow-Credentials`, never a
  reflected value.
- Any other `Origin` (including `null`, case variants, default-port variants,
  lookalike subdomains, duplicates) → `403 forbidden`, no CORS headers
  (WEB-03).
- Preflight (`OPTIONS` + `Origin` + `Access-Control-Request-Method`): only
  for an origin in `cors_origins`, method `GET` or `POST`, and
  `Access-Control-Request-Headers` a subset of the fixed list
  `authorization, content-type, x-stainless-arch, x-stainless-lang,
  x-stainless-os, x-stainless-package-version, x-stainless-retry-count,
  x-stainless-runtime, x-stainless-runtime-version, x-stainless-timeout`
  (case-insensitive; the `x-stainless-*` names are what the official
  `openai-node` SDK sends from a browser) → `204` with
  `Access-Control-Allow-Origin`, `Access-Control-Allow-Methods: GET, POST`,
  `Access-Control-Allow-Headers: <the fixed list>`,
  `Access-Control-Max-Age: 600`, `Vary: Origin`. Anything else `403`
  (WEB-06). A preflight never reaches authentication or a route.
- `cors_origins` entries are validated as `http://` or `https://`, a host
  (lower-case), an optional port, no path; `*`, `null` and duplicates are
  validation errors (WEB-05).
- SSE responses follow the same rules as complete responses.

### 4.5 Authentication

SP1's `TokenVerifier` (fixed 256-byte window, no length oracle) is the only
comparison; `constant_time_token_equals` in `http_request_handler.cpp` is
deleted with SP1. Missing header, a scheme other than `Bearer` (exact case,
one space), an empty token or a wrong token → `401`; more than one
`Authorization` → `400`. `x-api-key` is not accepted (SP1 §7 defers it). The
token is never logged, echoed, or put in an error.

### 4.6 Headers on every response

`Connection: close`, `Cache-Control: no-store`,
`X-Content-Type-Options: nosniff`,
`Content-Security-Policy: default-src 'none'; frame-ancestors 'none'`,
`Referrer-Policy: no-referrer`, `Cross-Origin-Resource-Policy: same-origin`,
and `Date` (IMF-fixdate; RFC 9110 §6.6.1 requires it of an origin server with
a clock). No `Server`, no `X-Powered-By`, no version anywhere. Response header
names and values come from code, never from the request; the SP0 CR/LF guard
in `serialize_response` stays.

### 4.7 Bind rules

Unchanged from SP1 (§3.7 classification, `allow_remote`,
`allow_wildcard_bind`, hostnames rejected, `localhost` → `127.0.0.1`). SP3
adds no listener: the workers use accepted sockets only (NET-01 re-proved with
workers running, §9.3).

## 5. DoS limits (`DOS-*`)

### 5.1 Schema rows SP3 adds or changes (SP1 addendum)

Added through SP1's schema mechanism (same validator, same `--check` output,
same README table). The mechanism needs four extensions, each with its own
hand-written CFG-01 cases (they are **not** covered automatically by SP1's
table-driven cases): string-array rows with an entry-count bound, a per-entry
grammar validator for arrays, the cross-field rules below, and **defaults
derived from the worker count** (used only when the key is absent; an
explicitly set value is validated as written and never adjusted). SP1's
`init` omits `max_queued_generations` and `max_connections_per_remote_address`
from the config it writes, so the derivation applies, and `--check` prints
their effective values marked `derived`. None of
the rows has a command-line option ("Option" `-`): they are config-file
settings, and SP1's flag table is not extended.

| Key | Type | Default | Range | Option | Cross-field rule | Meaning |
| --- | --- | --- | --- | --- | --- | --- |
| `limits.concurrency.max_concurrent_requests` (range changed) | integer | `8` | 1-256 | - | 1 = serial | Worker threads: connections read or answered at once |
| `limits.concurrency.max_queued_generations` | integer | `min(4, max(0, workers − 2))` — `4` at the default 8 workers, `0` at 1 or 2 | 0-254 | - | `= 0` when workers = 1; `+ 2 ≤ max_concurrent_requests` when workers ≥ 2 | Chats waiting behind the running generation |
| `limits.concurrency.queue_timeout_ms` | integer | `300000` | 1000-3600000 | - | — | Longest wait for the engine |
| `limits.concurrency.max_connections_per_remote_address` | integer | `min(4, workers)` — `4` at 8 workers, `1` at 1 | 1-256 | - | `≤ max_concurrent_requests` | Workers one non-loopback address may hold |
| `limits.http.header_deadline_ms` | integer | `10000` | 1-3600000 | - | `≤ request_deadline_ms` | Time from accept to end of header |
| `limits.http.write_stall_ms` | integer | `10000` | 100-300000 | - | — | Longest send without progress |
| `limits.http.response_write_deadline_ms` | integer | `60000` | 1000-3600000 | - | `≥ write_stall_ms` | Whole complete response, or stream tail after DONE |
| `limits.http.stream_keepalive_ms` | integer | `15000` | 1000-300000 | - | — | SSE comment interval while no content flows |
| `listener.cors_origins` | string array | `[]` | 0-16 entries | - | §4.4 grammar, no duplicates | Exact browser origins allowed CORS |
| `listener.host_aliases` | string array | `[]` | 0-16 entries | - | `host[:port]` grammar; non-empty when the bind is a wildcard | Extra exact `Host` values |
| `logging.requests` | boolean | `false` | — | - | — | One line per request to stderr (§8) |

The SP1 default of `max_concurrent_requests` changes from 1 to 8 and its range
from `1-1` to `1-256`. Every config valid under SP1 stays valid: such a file
sets `max_concurrent_requests` to `1` or omits it, and never sets the two new
concurrency keys, so their derived defaults (`0` queued, `1` per remote
address) satisfy the cross-field rules and the file gets the serial server it
asked for. Decision 1 may add a schema
change to `limits.chat.default_max_tokens` (value `0` = auto); that is the only
other SP1 row SP3 could touch.

### 5.2 Every limit, where it is enforced, what it yields

| Limit | Key / constant | Default | Enforced at | Status / effect |
| --- | --- | --- | --- | --- |
| Workers | `limits.concurrency.max_concurrent_requests` | 8 | accept thread | eviction of oldest P1, else `503 server_busy` |
| Per remote address | `…max_connections_per_remote_address` | 4 (derived; at the default 8 workers) | accept thread | `503 server_busy` |
| Listen backlog | `listener.backlog` (SP1) | 16 | kernel | SYNs beyond it dropped/refused by the OS |
| Header time | `limits.http.header_deadline_ms` | 10 s | worker P1 | `408` |
| Request time | `limits.http.request_deadline_ms` (SP1) | 30 s | worker P1–P3 | `408` |
| Request line | `limits.http.max_request_line_bytes` (SP1) | 4096 | header reader | `414` |
| Header bytes | `limits.http.max_header_bytes` (SP1) | 16384 | header reader | `431` |
| Header count | `limits.http.max_headers` (SP1) | 64 | parser | `431` |
| Whole request | `limits.http.max_request_bytes` (SP1) | 65536 | P2, from `Content-Length` | `413` |
| Chat body | `limits.chat.max_body_bytes` (SP1 plan) | 49152 | P2, from `Content-Length` | `413 request_too_large` |
| JSON depth | `kChatMaxJsonDepth` | 4 | `secure_frontend_json` | `400 invalid_json` |
| Messages | `limits.chat.max_messages` (SP1) | 128 | schema | `400 invalid_request` |
| Message bytes | `limits.chat.max_message_bytes` (SP1) | 65536 | schema | `400 invalid_request` |
| Prompt tokens | `limits.chat.max_prompt_tokens` (SP1) | 32768 | after frontend tokenization | `400 context_length_exceeded` |
| Output tokens | `limits.chat.max_completion_tokens` (SP1) | 4096 | P4 | clamped (decision 2) |
| Context rule | engine READY `<ctx>`: `n + max_tokens + 8 ≤ ctx` | from READY | P4 (last READY ctx) and again after the lease | `max_tokens` clamped to `ctx − n − 8`; `400 context_length_exceeded` when that is < 1; no `GEN` written |
| Generated bytes | `limits.chat.max_generated_bytes` (SP1) | 1 MiB | decoder | `STOP`, `finish_reason:"length"` |
| Queue places | `limits.concurrency.max_queued_generations` | 4 (derived; at the default 8 workers) | gate | `503 server_busy` |
| Queue wait | `limits.concurrency.queue_timeout_ms` | 300 s | gate | `503 server_busy` |
| Generation time | `limits.engine.generation_timeout_s` (SP1) | 120 s | P6 | `STOP`; `finish_reason:"length"` (decision 4) |
| Drain after STOP | `limits.engine.stop_drain_timeout_s` (SP1) | 5 s | P6 | SP2 `drain_failed`; `503`/error event `engine_unavailable` |
| Send stall | `limits.http.write_stall_ms` | 10 s | P7 / streaming | cancel + close |
| Response / tail write | `limits.http.response_write_deadline_ms` | 60 s | P7 | close |
| Delta text per event | `kMaxDeltaTextBytes` | 16 KiB | SSE encoder | split into more events |
| SSE event size | `kMaxSseEventBytes` | 128 KiB | SSE encoder, API client | encoder cannot exceed it (6 × 16 KiB + envelope); client rejects larger |
| Pending stream text | `limits.chat.max_generated_bytes` | 1 MiB | streaming worker | bounded by the generated-bytes limit |
| Refused sockets kept | `kLingerListMax` | 64 | accept thread | oldest closed |
| Log lines | `kLogBurst` 100, `kLogRatePerSecond` 20 | — | logger | dropped and counted (§8) |
| Engine stdin chunk | SP2 `kStdinWriteChunk`/`kStdinWriteDeadline` | 64 KiB / 1 s | SP2 | SP2 `write_timeout` |

### 5.3 Internal constants (`api_constants.hpp`)

`kAcceptPollSlice` 100 ms; `kStreamPollSlice` 20 ms; `kLingerListMax` 64;
`kChatMaxJsonDepth` 4; `kMaxDeltaTextBytes` 16 KiB; `kMaxSseEventBytes`
128 KiB; `kMaxSendChunk` 64 KiB (SP0, moved here); the half-close drain
100 ms / 256 KiB (SP0, moved here); `kRetryAfterBusyS` 1;
`kRetryAfterEngineS` 5; `kLogBurst` 100; `kLogRatePerSecond` 20; the refusal
response body; the `GEN` sampling bounds of decision 6. SP0's
`kMinimumSendBudget` is deleted (replaced by `write_stall_ms` and
`response_write_deadline_ms`). SP1 spec §4's internal-constants list still
names "send minimum budget 1000 ms" and "half-close drain"; SP3 records the
change as an **SP1 addendum** in the handoff (and in the frontend README's
constants note) rather than editing the approved SP1 spec silently.

### 5.4 Token counting stays in the frontend (engineering choice E6)

The frontend already renders the prompt template and tokenizes with the
offline tokenizer pack (`chat_execution.cpp`: `make_qwen_chat_generation_plan`
/ `make_glm53_chat_generation_plan`), so the prompt length `n` is known exactly
before any engine command. SP3 keeps that and applies three checks in P4,
before the request takes a queue place: `n ≤ max_prompt_tokens`; `max_tokens`
resolved (decisions 1 and 2) and `n + max_tokens + 8 ≤ ctx` with the last
READY context (re-checked against the lease's READY). Reasons: an engine `ERR`
costs a whole generation and counts toward the circuit (SP2 plan Review Focus
6, as shipped), so the engine must never see a request it will refuse; and a
request that cannot run must not occupy a queue place.

## 6. Egress and extension exclusion (`EXT-*`; frontend part of `PRIV-01`, `PRIV-03`, `DL-01`)

### 6.1 Rule

The `serve` code path (today `strata-secure-server`) opens exactly one kind of
socket: the listener and the sockets it accepts. It makes no outbound
connection, no DNS lookup, no UDP socket, and no Unix-domain socket. Pipes to
the engine are the only other IPC. The only `connect()` in the frontend
sources is in `secure_frontend_api_client` (§3.10), whose address parameter is
a `LoopbackAddress`; the serve path never calls it. Because the product's one
`strata` binary will contain both `serve` and `run`, the binary as a whole
imports `connect`; "serve never connects" is shown at runtime (§6.2), not from
the import table.

### 6.2 How tests check it — tripwires, not proofs

No single check proves the absence of egress; three independent checks each
catch a different regression, and their limits are recorded in the threat
model (§13.2).

| Check | Linux | Windows | Catches | Does not catch (recorded) |
| --- | --- | --- | --- | --- |
| Static import tripwire | Test-support reader of the ELF dynamic section (std only) asserts the undefined symbols contain none of `getaddrinfo`, `gethostbyname`, `gethostbyname2`, `gethostbyaddr`, `getnameinfo`, `res_query`, `res_search`, `res_nquery`. The test **fails** when the binary has no dynamic section (a static build), so it never passes vacuously; a static release must replace it deliberately. | Test-support reader of the PE import directory asserts no `winhttp.dll`, `wininet.dll`, `urlmon.dll`, `dnsapi.dll`, and no `getaddrinfo`, `GetAddrInfoW`, `GetAddrInfoExW`, `gethostbyname`, `WSAConnect` by name **or by their `ws2_32` ordinals**. The MSYS2 preset links `-static`, but system DLLs stay imported, so the directory exists; a missing import directory fails the test. | a new dependency or code path that links a resolver or HTTP client | `dlopen`/`dlsym`, `LoadLibrary`/`GetProcAddress`, raw syscalls |
| Runtime interposer (Linux) | Test-only `libstrata_egress_tripwire.so` (std + `dlfcn`), loaded with `LD_PRELOAD` into the server under test only (SP2 keeps `LD_PRELOAD` out of the engine's environment): wraps `connect`, `sendto`, `sendmsg`, `getaddrinfo`, `gethostbyname`, and `socket` for `AF_UNIX`, `AF_NETLINK` and `SOCK_DGRAM`; every call is appended to a record file and fails with `EACCES`. The record must be empty after the lifecycle below. | — | any libc-level outbound attempt, however short-lived, including through `dlsym` of libc symbols | raw `syscall(2)`; a static binary (then the static tripwire already failed) |
| Connection table scan | Sample every 50 ms: map `/proc/<pid>/fd` socket inodes to `/proc/<pid>/net/{tcp,tcp6,udp,udp6,unix}`; every TCP socket has local port = listener port; no UDP, no Unix socket; same scan for the fake engine pid | `GetExtendedTcpTable`/`GetExtendedUdpTable` (`TCP_TABLE_OWNER_PID_ALL`, `UDP_TABLE_OWNER_PID`) filtered by pid, every 50 ms: only the listener and connections whose local port is the listener port; no UDP endpoint | long-lived sockets on both OSes | sockets that live < 50 ms (on Windows this is the only runtime check) |

The lifecycle exercised under the interposer and the scan: start, `/health`,
`/v1/models`, chat, stream, cancel, engine crash and restart, refusal flood,
the SP2 `ext01`/`ext02` cases and the SP3 EXT cases, API values resembling
commands (`PRIV-03` frontend part: model `pull:x`, content with `https://…`,
`mcp://…`, `file://…`, `data:` — rejected or treated as text), shutdown.

Not used: seccomp. A `PR_SET_NO_NEW_PRIVS` + filter needs no privileges and
could deny `connect` outright, but it would have to be installed in the
production binary to test anything real, which is SP7 hardening, not an SP3
test. Whole-machine monitoring (tcpdump/ETW, DNS on the box) is the SP7
validation-kit part of `PRIV-01`.

### 6.3 Constraint for SP4

SP3 states only the constraint: the `serve` code path must keep passing §6.2,
so no resolver, TLS or HTTP-client code may be reachable from it, and the
static tripwire must stay meaningful for the binary that runs `serve`. Where
SP4 puts `pull`/`update` networking (a separate executable, or one binary with
a different static check) is SP4's decision.

### 6.4 Extension exclusion

SP2 owns `ext01_extension_fields_spawn_nothing` and the `ext02_*` cases. SP3:

- **Amends** `ext01_extension_fields_spawn_nothing`: the
  `{"stream":true}` body is removed from its rejected list (streaming is now
  a feature), and `parallel_tool_calls`, `function_call` and role `function`
  are added (array `content` is `unsupported_parameter`, not an extension, and
  is tested in the parser matrix instead). The case keeps asserting "no `GEN`
  in the `--record-file`, descendant count unchanged", and now also runs under
  the §6.2 interposer and scan.
- **Adds** the stream path, the routes, and the fixed body: each extension
  field with `"stream":true` gets `400 extension_disabled` before any header
  of a stream is sent; upstream's extension and control routes are `404` after
  auth; HTTP headers that look like switches (`X-Strata-Enable-Tools`,
  `X-Strata-Mcp`) change nothing.
- SP3's new config keys add no extension surface; unknown keys such as
  `mcp_servers` stay SP1 `CFG-01` errors (EXT-02).

## 7. Error model

### 7.1 Body

Every error, before or after authentication, is JSON in OpenAI's documented
shape:

```
{"error":{"message":"<fixed sentence>","type":"<type>","param":null,"code":"<code>"}}
```

`param` is `null` except for `unsupported_parameter` and
`extension_disabled`, where it is the matched key from the fixed lists of
§3.1; the message is always the fixed sentence. `Content-Type:
application/json; charset=utf-8` plus §4.6 headers. This replaces SP0's
`text/plain` bodies and the `{"error":"<phrase>"}` bodies of SP0/SP2; the
SP1/SP2 tests that assert those exact bodies are updated in the same SP3 task,
not deleted (§9.4). `/health` keeps SP2's body, which is a status document, not
an error.

### 7.2 Status and code table

| Status | `type` | `code` | Message (fixed) | Extra headers |
| --- | --- | --- | --- | --- |
| 400 | `invalid_request_error` | `invalid_request` | The request is malformed or not supported. | — |
| 400 | `invalid_request_error` | `invalid_json` | The request body is not valid JSON within the allowed limits. | — |
| 400 | `invalid_request_error` | `unsupported_parameter` | This parameter is not supported by this server. | — |
| 400 | `invalid_request_error` | `extension_disabled` | Tools, functions and extensions are disabled on this server. | — |
| 400 | `invalid_request_error` | `context_length_exceeded` | The prompt exceeds the model context or the configured prompt limit. | — |
| 401 | `authentication_error` | `invalid_api_key` | Missing or invalid API key. | `WWW-Authenticate: Bearer` |
| 403 | `permission_error` | `forbidden` | The request origin or host is not allowed. | — |
| 404 | `invalid_request_error` | `not_found` | Not found. | — |
| 404 | `invalid_request_error` | `model_not_found` | The requested model is not served here. | — |
| 405 | `invalid_request_error` | `method_not_allowed` | Method not allowed. | `Allow` |
| 408 | `invalid_request_error` | `request_timeout` | The request was not received in time. | — |
| 411 | `invalid_request_error` | `length_required` | A Content-Length body is required; chunked bodies are not accepted. | — |
| 413 | `invalid_request_error` | `request_too_large` | The request body is too large. | — |
| 414 | `invalid_request_error` | `request_line_too_long` | The request line is too long. | — |
| 415 | `invalid_request_error` | `unsupported_media_type` | Send application/json. | — |
| 417 | `invalid_request_error` | `expectation_failed` | Unsupported Expect header. | — |
| 431 | `invalid_request_error` | `header_too_large` | The request header is too large. | — |
| 500 | `server_error` | `internal_error` | Internal error. | — |
| 503 | `server_error` | `server_busy` | The server is busy; retry later. | `Retry-After: 1` |
| 503 | `server_error` | `engine_unavailable` | The local engine is not available. | `Retry-After: 5` or none (§3.8) |
| 503 | `server_error` | `engine_error` | The local engine could not complete the request. | `x-should-retry: false` |
| 503 | `server_error` | `server_shutting_down` | The server is shutting down. | — |
| 505 | `invalid_request_error` | `http_version_not_supported` | Only HTTP/1.1 is supported. | — |

Every message is a compile-time literal in one table (`api_errors.hpp`); a
test asserts the rendered body of every row is byte-identical to the table and
that no response body or log line in the suite contains the harness token, a
prompt marker string, or a configured path (§9.3 `err_*` cases).

### 7.3 401 vs 403 vs 404

Unauthenticated: `401` on every path and method, including unknown ones, so
the route table is not observable without a token. Host/Origin failures are
`400`/`403` before authentication; they reveal only that the Host or Origin
is not allowed, which the client already knows. `403` is never used for a
wrong token.

## 8. Observability without telemetry

- No metrics endpoint, no counters route, no outbound reporting of any kind
  (PX §1 non-goals). `/metrics` and `/props` stay absent.
- Log output is stderr only (the service manager or the terminal keeps it);
  files under `roots.logs` with retention are SP7.
- **Always logged** (fixed literals, not per request): startup summary (bound
  authority, worker count, queue size — the operator's own settings, already
  printed by `--check`), listener failure, SP2's
  `engine generation <n> ended: <kind>`, shutdown.
- **Per request, only when `logging.requests = true`** (default `false`):
  `req <seq> <route> <status> <code|-> <ms> <bytes_out> <peer>` where
  `<seq>` is a process-local counter, `<route>` one of `health`, `models`,
  `model`, `chat`, `chat_stream`, `other`, `<code>` the §7.2 code or one of
  the log-only outcomes `generation_timeout`, `client_gone`, `write_stall`,
  `evicted` (never sent to a client), `<peer>` `loopback` or `remote`. Never:
  path bytes, headers, the token, the model field, prompt or completion text,
  token counts per message, peer address.
- **Rate limit:** a token bucket of `kLogBurst` (100) lines refilled at
  `kLogRatePerSecond` (20/s) shared by all request lines; drops are counted
  and reported as one fixed line `log: <n> request lines suppressed` when the
  bucket refills.
- The logger is one mutex-guarded writer with a fixed-size line buffer; a
  write that fails is dropped, never retried, never blocks a worker beyond the
  mutex.

## 9. Testing strategy

### 9.1 Framework and labels

Framework-free `main()` test programs with `secure_frontend_test::run_cases`,
registered with `secure_frontend_add_cases(<binary> <case>… TIMEOUT 120
[LABELS ${SECURE_FRONTEND_WINDOWS_RUNTIME}])`; CTest names
`<binary>.<case>`. Process-level tests use SP2's `test_support::ServerProcess`
harness and `strata-supervision-fake-engine`. wine is a smoke test; Windows
claims come only from the MSYS2 `windows-msys2-clang` job. Tests that depend
on native Winsock behavior (RST vs FIN timing, `WSAPoll`, Winsock send
buffering, `GetExtendedTcpTable`) carry `windows-runtime`. Allowed skips (exit
77 with a printed reason) added by SP3: `net01_ipv6_loopback_listener_only`
when the host has no IPv6 loopback.

New binaries: `secure_frontend_http_parser_tests` (parser and policy units),
`secure_frontend_concurrency_tests` (pool, eviction, gate with an injected
clock), `secure_frontend_sse_tests` (encoder, UTF-8 decoder, chunk shapes),
`secure_frontend_api_server_tests` (end to end over TCP with the fake
engine), `secure_frontend_egress_tests` (plus the test-only
`libstrata_egress_tripwire.so` on Linux), `secure_frontend_api_client_tests`.
Existing binaries extended: `secure_frontend_http_policy_tests`,
`secure_frontend_http_socket_server_tests`, `secure_frontend_server_tests`
(SP2), SP1's config and AUTH-01 binaries.

### 9.2 Fake engine additions (`strata-supervision-fake-engine`)

The fake reads stdin on a thread so `STOP` is seen while it emits. New
`=value` modes: `--token-ids=<csv>` (ids emitted in order, default `7`),
`--token-interval-ms=<n>` (pause between `T` records), `--tokens=<n>`
(repeat the id list until n), `--finish=<stop|length|nonfinite>` (default
`stop`), `--prefill-delay-ms=<n>` (pause after `GEN` before the first `T`),
`--progress-records` (GLM flavor `PP` lines during prefill), `--crash-mid-gen=<k>`
(exit 1 after k tokens), `--err-mid-gen=<k>` (`ERR test` after k tokens),
`--stop-delay-ms=<n>` (delay between seeing `STOP` and `DONE … cancel`).
`DONE` uses the full six-field form when any new mode is set. The test
tokenizer pack gains ids whose bytes are the lead and continuation bytes of a
3-byte and a 4-byte UTF-8 sequence, and an id for a lone continuation byte. A
`fake_engine_stream_modes_selftest` case covers the modes.

### 9.3 Acceptance mapping

Platform: **B** = Linux CTest + MSYS2; **L** = Linux only; **W** = MSYS2 only
(`windows-runtime`). Binary prefix `secure_frontend_` omitted in the table:
`api_server_tests.x` means `secure_frontend_api_server_tests.x`. Plan: M1 or M2.

| ID | Requirement (README) | Test cases | Plat | Plan |
| --- | --- | --- | --- | --- |
| NET-01 | Only loopback listens by default | SP1 `net01_*` unchanged; `api_server_tests.net01_workers_add_no_listener` (after 20 mixed requests: exactly one listening socket, on loopback); `api_server_tests.net01_ipv6_loopback_listener_only` (bind `::1`, skip without IPv6) | B | M1 |
| NET-02 | Remote addresses need opt-in | SP1 `net02_*` (run in MSYS2 too) | B | M1 |
| NET-03 | Wildcard needs its own opt-in | SP1 `net03_*` (MSYS2 too) | B | M1 |
| WEB-01 | Bad `Host` rejected | `http_policy_tests.web01_host_matrix` (unknown, duplicate, malformed, other IP literal, other port, no port, trailing dot, userinfo, control byte, IPv6 unbracketed); `api_server_tests.web01_bad_host_rejected_on_every_route` (iterates `kRouteTable`, valid token, `400`/`403`, no route data); `api_server_tests.web01_ipv6_bound_host_rules` (`[::1]:p` ok, `::1` and `[::1]` rejected) | B | M1 |
| WEB-02 | Same-origin allowed Host works | `api_server_tests.web02_bound_authority_and_localhost_alias_succeed`; `api_server_tests.web02_host_alias_from_config_succeeds` | B | M1 |
| WEB-03 | Untrusted `Origin` rejected | `http_policy_tests.web03_origin_matrix` (`null`, lookalike subdomain, mixed case, `:80` default-port variant, `https` vs `http`, trailing `/`, duplicate); `api_server_tests.web03_untrusted_origin_no_cors_header` | B | M1 |
| WEB-04 | Default: no ACAO | `api_server_tests.web04_default_emits_no_cors_header_anywhere` (every route, every §7.2 status reachable over TCP; the SSE case is added in M2) | B | M1, M2 |
| WEB-05 | One configured origin, exact only | `api_server_tests.web05_only_exact_configured_origin_allowed`; `config_tests.web05_cors_origin_config_rejects_wildcard_null_path_duplicate_and_17_entries` | B | M1 |
| WEB-06 | Preflight / cross-origin state change | `api_server_tests.web06_preflight_matrix` (allowed origin × method × headers incl. `x-stainless-*`; disallowed each → `403`; preflight needs no token and reaches no route); `api_server_tests.web06_cross_origin_post_without_trust_rejected` (valid token, foreign `Origin`, `403`, fake `--record-file` has no `GEN`) | B | M1 |
| DOS-01 | Oversize, nesting, chunking, slow body | `http_parser_tests.dos01_request_line_over_limit_is_414`; `…dos01_header_block_over_limit_is_431`; `…dos01_too_many_headers_is_431`; `…dos01_status_matrix_415_417_505` (`text/plain` body → 415; `Expect: foo` → 417; `HTTP/1.0` and `HTTP/2.0` → 505); `…dos01_transfer_encoding_variants_are_411` (`chunked`, `Chunked`, ` chunked`, `gzip, chunked`, TE + CL; `Transfer-Encoding :` with space before the colon → `400`); `…dos01_content_length_variants` (duplicate, `+1`, `0x10`, 11 digits, mismatch, stray bytes after the body → `400`); `…dos01_json_depth_5_is_400`; `…dos01_json_limits` (messages, message bytes, `max_tokens` 0, number forms); `…dos01_content_array_is_unsupported_parameter`; `api_server_tests.dos01_body_over_limit_is_413_before_reading` (valid token, `Content-Length` 49153: over `max_body_bytes`, under the request limit → `413` at step 7); `…dos01_content_length_over_request_limit_is_413_before_auth` (no token, `Content-Length: 70000`, no body sent → `413` within 100 ms, not `401`); `…dos01_unauthenticated_gets_401_before_body_is_read` (header with a wrong token and `Content-Length: 40000`, no body sent: `401` within 100 ms); `…dos01_expect_continue_after_auth_only`; `…dos01_slow_header_is_408_at_header_deadline`; `…dos01_slow_body_is_408_at_request_deadline` (valid token); `…dos01_incomplete_body_then_close_frees_worker`; `…dos01_context_rule_is_400_without_gen` (fake `--ready-context=64`, no `GEN` recorded); `…dos01_service_healthy_after_each_attack` (after each case: `/health` `200` and a chat succeeds); `concurrency_tests.dos01_oldest_header_phase_connection_is_evicted` (seam); `api_server_tests.dos01_eviction_over_tcp_admits_new_client` (8 workers held in P1 by slow headers; a 9th client completes; one P1 client sees close); `api_server_tests.dos01_slowloris_cannot_starve_a_real_client` (16 one-byte-per-second header clients, a real chat still completes); `api_server_tests.dos01_slow_body_clients_cannot_starve_a_real_client` (16 clients send a complete header without a valid token and then dribble a body: each gets `401` at once and is closed; a real chat completes); `concurrency_tests.dos01_remote_address_cap` (peer classifier seam); `concurrency_tests.dos01_linger_list_overflow_closes_oldest` (65 refusals, at most 64 kept) | B | M1 |
| DOS-02 | Concurrency limits, cancel, release | `api_server_tests.dos02_excess_connections_get_503_retry_after` (all workers held in P5/P6, next gets `503 server_busy`, `Retry-After: 1`, valid JSON); `…dos02_queue_full_is_503`; `…dos02_queue_full_with_streams_is_503_before_headers` (M2: streaming requests fill the queue; the next stream request gets a plain `503`, no `text/event-stream` header); `…dos02_queue_timeout_is_503` (config 1000 ms); `…dos02_queue_is_fifo` (five clients, completion order = admission order); `…dos02_health_answers_while_generation_places_full`; `…dos02_serial_mode_one_worker` (`max_concurrent_requests` 1, queue 0: second chat gets `503 server_busy`); `…dos02_queued_client_disconnect_leaves_queue`; `…dos02_non_stream_disconnect_cancels`; `…dos02_stream_disconnect_sends_stop_and_frees_engine` (M2; fake `--tokens=10000 --token-interval-ms=5`; client closes after 3 chunks; `--record-file` has `STOP`; next request served within 1 s); `…dos02_slow_stream_reader_hits_write_stall` (M2; client stops reading; `write_stall_ms` 500; `STOP` recorded; worker free); `…dos02_slow_reader_never_backpressures_engine` (M2; fake emits 1 MiB of text in < 1 s while the client reads nothing; `DONE` reaches the frontend in < 2 s); `…dos02_resources_return_to_idle` (100 cancelled requests, streaming in M2: thread count and open fds/handles equal the values recorded at first idle, ± 0); `…dos02_shutdown_during_stream_ends_with_error_event` (M2; event, `[DONE]`, terminal chunk) | B | M1, M2 |
| EXT-01 | Extension fields/routes absent | SP2 `server_tests.ext01_extension_fields_spawn_nothing` (amended, §6.4, under the egress interposer and scan); `api_server_tests.ext01_extension_fields_rejected_on_stream_path` (M2); `api_server_tests.ext01_upstream_control_routes_are_404_after_auth` | B | M1, M2 |
| EXT-02 | Enabling via env/config/HTTP fails | SP2 `ext02_*` unchanged (under the egress interposer and scan); `api_server_tests.ext02_http_headers_cannot_enable_extensions` | B | M1 |

Configuration (SP1 addendum, M1): `config_tests.cfg01_sp3_rows_type_and_range`
(every §5.1 row through SP1's table-driven cases); `config_tests.cfg01_queue_plus_two_le_workers`;
`config_tests.cfg01_serial_requires_zero_queue`;
`config_tests.cfg01_remote_cap_le_workers`;
`config_tests.cfg01_header_deadline_le_request_deadline`;
`config_tests.cfg01_response_deadline_ge_write_stall`;
`config_tests.cfg01_wildcard_bind_requires_host_aliases` (also asserts nothing
spawned); `config_tests.cfg01_host_alias_grammar_and_count`;
`config_tests.cfg01_sp1_config_with_one_worker_still_valid` (an SP1-era file with `max_concurrent_requests: 1` and no SP3 keys validates, with queue 0 and remote cap 1); `config_tests.cfg01_derived_defaults_follow_worker_count` (workers 1, 2, 3, 6, 8, 256 → queue 0, 0, 1, 4, 4, 4 and cap 1, 2, 3, 4, 4, 4); `config_tests.cfg01_explicit_values_are_not_adjusted` (workers 1 with explicit queue 4 → error naming the key).

Streaming, errors, egress and Windows-specific cases:

| Area | Test cases | Plat | Plan |
| --- | --- | --- | --- |
| SSE shape | `sse_tests.sse_role_content_final_done_sequence`; `…sse_include_usage_adds_usage_chunk`; `…sse_ids_and_created_constant_per_stream`; `…sse_events_never_contain_raw_newline`; `…sse_coalesced_control_heavy_text_respects_event_cap` (1 MiB of `"`/`\`/C0-heavy text coalesced: every event ≤ 128 KiB, deltas ≤ 16 KiB, split on code points); `…sse_chunked_framing_and_terminal_chunk`; `api_server_tests.stream_matches_non_stream_text`; `api_server_tests.stream_cut_has_no_terminal_chunk` (stall cancel) | B | M2 |
| UTF-8 | `sse_tests.utf8_split_across_tokens_is_held_then_released`; `…utf8_invalid_sequence_becomes_replacement_character`; `…utf8_incomplete_tail_dropped_on_length_and_cancel`; `…utf8_incomplete_tail_on_stop_becomes_one_replacement`; `api_server_tests.utf8_timeout_cut_mid_character_returns_length` (multi-byte fake ids, timeout 1 s); `api_server_tests.utf8_non_stream_max_tokens_cut_mid_character_returns_length` | B | M2 |
| Finish mapping | `api_server_tests.finish_stop_length_and_generated_bytes_cap`; `…generation_timeout_returns_text_with_length`; `…nonfinite_is_engine_error_and_engine_stays_ready`; `…err_mid_stream_is_error_event_then_done`; `…crash_mid_stream_is_engine_unavailable_event` (fake `--crash-mid-gen=3`; then `/health` follows SP2 `recovery=restarting`); `…engine_error_carries_should_retry_false` | B | M2 |
| Max tokens | `api_server_tests.max_tokens_above_limit_is_clamped`; `…max_tokens_clamped_to_context_room`; `…prompt_alone_over_context_is_400`; `…absent_max_tokens_uses_auto` (decision 1) | B | M1 |
| Sampling | `http_parser_tests.sampling_neutral_values_are_no_ops`; `…sampling_bounds_rejected`; `api_server_tests.sampling_passthrough_reaches_gen` (`--record-file` shows `temperature=`, `top_p=`, `seed=` in the `GEN` line) | B | M1 |
| Keepalive | `api_server_tests.stream_keepalive_comment_during_prefill` (`--prefill-delay-ms=2500`, keepalive 1000 ms → ≥ 2 comments before content) | B | M2 |
| Errors | `http_parser_tests.err_every_code_renders_fixed_body`; `…err_param_is_key_only`; `api_server_tests.err_no_token_prompt_or_path_in_any_body_or_log` (harness token, marker prompt `ZZPROMPTMARKERZZ`, scratch path searched in every body, SSE byte and captured stderr with `logging.requests=true`); `…err_unauthenticated_is_401_on_every_path_and_method`; `…shutdown_errors_never_report_fatal` (SIGTERM during generation: `generations_started()` unchanged, no failure counted) | B | M1 |
| Auth | SP1 `auth01_every_route_rejects_missing_malformed_wrong_token` extended to iterate `kRouteTable` incl. `/v1/models/<id>` and `stream:true` | B | M1, M2 |
| Headers | `api_server_tests.security_headers_on_every_response` | B | M1 |
| Egress | `egress_tests.egress_server_imports_no_resolver_or_http_client` (ELF on Linux, PE on Windows); `egress_tests.egress_static_tripwire_fails_on_binary_without_dynamic_section` (a test-built static helper must make the reader report failure); `egress_tests.egress_interposer_records_nothing_during_lifecycle` (L); `egress_tests.egress_interposer_self_test` (a helper that calls `getaddrinfo` and `connect` is recorded and denied; L); `egress_tests.egress_table_scan_during_lifecycle` (Linux `/proc`, Windows tables with `windows-runtime`); `egress_tests.egress_command_like_values_trigger_nothing` | B / L / W | M1 |
| Client | `api_client_tests.client_accepts_only_loopback_address`; `…client_reads_chunked_stream_and_requires_done_and_terminal_chunk`; `…client_rejects_event_over_cap`; `…run_refuses_non_loopback_server_json` | B | M2 |
| Windows | `api_server_tests.win_refusal_is_read_not_reset` (refused client reads the full `503` body under a flood); `…win_eviction_wakes_worker_within_one_slice`; `…win_disconnect_detected_by_peek`; `http_socket_server_tests.win_send_chunks_respect_write_stall` | W | M1 |
| Logging | `api_server_tests.log_line_format_and_rate_limit` (300 requests in 1 s → ≤ 100 + 20 lines + one suppression line) | B | M1 |

### 9.4 Existing tests that change

| Test | Change |
| --- | --- |
| SP0/SP1/SP2 cases asserting `text/plain` or `{"error":"…"}` bodies (`http_request_handler_tests`, `api_router_tests`, SP2 `proc04_abort_in_generation_is_generic_503`, `proc04_chat_during_restart_is_generic_503`) | assert the §7.2 body for the same status |
| `socket_server_rejects_oversize_request_over_tcp` | `413` body now JSON; over-long header now `431` (new case), body `413` unchanged |
| `socket_server_responds_after_client_half_close` | unchanged for `GET` (§2.6) |
| SP2 `ext01_extension_fields_spawn_nothing` | §6.4 amendment |
| SP1 config tests for `max_concurrent_requests` | range `1-256`, default 8; `1` still valid |
| SP0 handoff gaps: `send_all` returns `false` on `EINTR`; S6 port range 20000–20099; transient-accept backoff | fixed in SP3 with `send_retries_on_eintr`, the S6 test binding port 0, and a 50 ms backoff on every transient accept failure (`accept_backs_off_on_transient_failure`) |

### 9.5 What cannot run on the dev host, and the offline GPU kit

The dev host has no GPU and no clang; every SP3 test above uses the fake
engine and runs on it (L/B) or in the MSYS2 job (W). Not provable here:

1. Real tokenizer splits of multi-byte UTF-8 over real vocabularies, at real
   token rates.
2. `STOP` latency during long prefill on a real engine (risk R4).
3. Whether the real engine applies `GEN` `temperature=`/`top_p=`/`seed=`
   (decision 6) as documented.
4. Real clients against a real model.

SP3 ships, for the SP7 offline kit, the test-only `strata-api-probe`
(built in every preset, not installed) and a written procedure
(`docs/validation/sp3-api-kit.md`) for the Linux and Windows boxes: start
`strata serve` with a real model; `strata-api-probe stream`, `nonstream`,
`cancel-after=<n>`, `cancel-during-prefill`, `concurrent=3`, `utf8` (CJK +
emoji prompt), `seed` (same seed twice → same text), each with pass criteria
(`[DONE]` and the terminal chunk seen; stream text equal to non-stream text at
a fixed seed; `STOP` → `DONE … cancel` measured and recorded against
`stop_drain_timeout_s`; FIFO order); plus `openai-python` and `curl -N`
against the base URL; and the egress check (`ss -tunap` / `netstat -abno`
snapshots before, during and after). Results go to the release record, not to
CI.

## 10. Windows parity

| Topic | POSIX | Windows | Decision |
| --- | --- | --- | --- |
| Init | — | `WSAStartup` once per process (today per `HttpSocketServer`); `WSACleanup` at exit | one RAII guard in `main` |
| Readiness | `poll` | `WSAPoll` (its known bug concerns failed `connect` notification, which the server never uses) | one `poll_sockets` wrapper |
| Non-blocking | `fcntl(O_NONBLOCK)` | `ioctlsocket(FIONBIO)` | every accepted socket is non-blocking; every read and write follows a `poll` in 20 ms slices (§2.2), so eviction and shutdown are seen within one slice on both OSes |
| Send bounds | 64 KiB chunks | Winsock accepted one 32 MiB blocking `send` whole (SP0 S2) → 64 KiB chunks kept; non-blocking `send` returns `WSAEWOULDBLOCK` at the buffer limit (SP0 measured 128 KiB default `SO_SNDBUF`) | stall rule measured on progress, not on call return |
| SIGPIPE | ignored + `MSG_NOSIGNAL` (SP0 S1) | n/a | unchanged |
| `EINTR` | retried | n/a | `send_all` fix (§9.4) |
| Half-close | `shutdown(SHUT_WR)` | `shutdown(SD_SEND)`; closing with unread data sends RST and can destroy the response | SP0 drain for workers; lingering list for refusals (§2.4) |
| Disconnect probe | `recv(MSG_PEEK)` = 0 → EOF; error → reset | `recv(MSG_PEEK)` on a non-blocking socket: 0 → EOF, `WSAECONNRESET` → reset, `WSAEWOULDBLOCK` → alive | same code, `windows-runtime` test |
| Waking the accept thread | `poll` timeout slice (100 ms) | same; never a loopback socket pair (that would be an outbound `connect`, §6.1) | slice + atomic flag on both |
| `TCP_NODELAY` | `setsockopt(IPPROTO_TCP)` | same | both |
| Port reuse | `SO_REUSEADDR` | `SO_EXCLUSIVEADDRUSE` (SP0 S7) | unchanged |
| Threads | `std::thread`, `std::mutex`, `std::condition_variable` | same (libc++ in llvm-mingw / MSYS2 CLANG64, static) | no Win32 thread API |
| Chunked SSE | same bytes | same bytes | one encoder |
| Egress checks | interposer + `/proc` scan + ELF tripwire | table scan + PE tripwire (no interposer) | limits recorded (§6.2) |
| Connection tables | `/proc` | `GetExtendedTcpTable`/`GetExtendedUdpTable` (`iphlpapi`, test only) | `iphlpapi` linked into the egress test only |

## 11. Decisions

### 11.1 Owner decisions

Each lists options, the recommendation (marked **R**) and the cost.

1. **`max_tokens` when the request omits it.** (a) **R** auto:
   `min(max_completion_tokens, ctx − n − 8)`, as upstream does (rest of the
   context, `server.py:5350`, `prepare()` 3516-3521) and as Ollama users
   expect; needs an SP1 schema addendum (`limits.chat.default_max_tokens`
   range `0-16384`, `0` = auto, new default `0`). (b) keep SP1's `256`; no
   schema change, but most chat answers are cut at 256 tokens with
   `finish_reason:"length"`. Cost of (a): an unbounded-looking request can run
   up to `generation_timeout_s`.
    **Decision (2026-10-14): (a) auto: `min(max_completion_tokens, ctx − n − 8)`, with the SP1 schema addendum (0 = auto, range 0–16384).**
2. **`max_tokens` above the limit.** (a) **R** clamp to
   `min(max_completion_tokens, ctx − n − 8)`; the answer ends with `"length"`
   if the clamp is hit; `400 context_length_exceeded` only when the prompt
   alone leaves no room. Clients that send `8192` or `16384` by default work.
   (b) reject with `400` (OpenAI behavior); such clients fail on every
   request. Cost of (a): a client gets fewer tokens than it asked for without
   an error.
    **Decision (2026-10-14): (a) clamp; `400` only when the prompt alone fills the context.**
3. **Invalid UTF-8 in model output.** (a) **R** replace each invalid sequence
   with U+FFFD and continue; drop an incomplete tail on `length`/`cancel`
   endings (§3.3). (b) fail on an invalid sequence: before stream headers
   (non-streaming) a `500 internal_error` response, today's
   `decode_token_ids` behavior; after stream headers the worker sends
   `STOP`, drains, and ends the stream with the `internal_error` error event,
   `data: [DONE]` and the terminal chunk (text already sent stays sent). One
   bad byte discards the rest of a generation. In both options the
   incomplete-tail drop on `length`/`cancel` endings applies, because a tail
   cut by a limit is not invalid output.
   Cost of (a): a client may see U+FFFD where a broken model emitted garbage.
    **Decision (2026-10-14): (a) replace invalid sequences with U+FFFD; incomplete tail dropped on length/cancel.**
4. **Generation timeout result.** (a) **R** `STOP`, drain, return the text so
   far with `finish_reason:"length"`. (b) an error (`503 engine_error`);
   output is discarded and OpenAI SDKs retry the whole request (unless
   `x-should-retry: false`). Cost of (a): a client cannot tell a timeout from
   `max_tokens`; the request log records `generation_timeout`.
    **Decision (2026-10-14): (a) partial text with `finish_reason:"length"`.**
5. **Stream framing.** (a) close-delimited body, `[DONE]` as the only end
   marker; simplest, but a truncated stream looks complete at the HTTP layer
   and `[DONE]` also follows error events. (b) **R** HTTP/1.1 chunked transfer
   coding for streams, terminal chunk on every normal or error end, none on a
   cut; truncation is visible to `httpx`/`openai-python`; about 80 lines more.
   Upstream answers as HTTP/1.0 with a close-delimited body
   (`server.py:4585`, `4743-4749`), so (b) differs from upstream on the wire
   but not in what an SSE client parses. Complete responses keep
   `Content-Length` in both options.
    **Decision (2026-10-14): (b) chunked transfer coding.**
6. **Sampling parameters.** (a) `400 unsupported_parameter` for all until SP5;
   fails most apps (LangChain, Open WebUI, Continue send `temperature`,
   `top_p`, and neutral `presence_penalty: 0` by default), against PX §1's
   "as easy as Ollama". (b) accept and silently ignore all; apps work but get
   behavior they did not ask for. (c) full pass-through now; pulls SP5 forward.
   (d) accept neutral values as no-ops (`presence_penalty`/`frequency_penalty`
   0, `logit_bias {}`, `stop` null/`[]`, `n` 1, `logprobs` false), reject
   non-neutral ones. (e) (d) plus bounded pass-through of `temperature`
   (0–2), `top_p` (0 < p ≤ 1) and `seed` (0–2⁶³−1) as `GEN` `k=v` (the
   engine parses these keys, `generate.cpp:10045-10053`); `stop` strings,
   `top_k`, `min_p`, non-zero penalties stay SP5. **R** (e); at minimum (d).
   Cost of (e): three bounded keys and a `GEN` change verified only on the GPU
   box (§9.5 item 3).
    **Decision (2026-10-14): (e) neutral values are no-ops; bounded `temperature`/`top_p`/`seed` passed to `GEN`.**
7. **Legacy `/v1/completions`.** **R** out of scope (upstream has none; PX
   targets chat clients). Cost: tools that only speak the legacy route do not
   work.
    **Decision (2026-10-14): out of scope.**
8. **Anthropic `/v1/messages`.** **R** not in SP3; consider in SP5 (upstream
   routes it, `server.py:5084-5085`, and project-maya has one too) behind the
   same gate, error model and SSE encoder with Anthropic event names. Cost:
   Claude-API clients need a translation proxy until SP5.
    **Decision (2026-10-14): not in SP3 (SP5 candidate).**
9. **Ollama-native `/api/chat`, `/api/generate`, `/api/tags`.** **R** out;
   PX's banner and clients use the OpenAI-compatible base URL, and upstream
   has none. Cost: tools that speak only the Ollama API need their OpenAI
   mode.
    **Decision (2026-10-14): out.**
10. **CORS default.** (a) **R** none by default; `listener.cors_origins`
    exact-origin opt-in, no `*`. (b) no CORS at all, even opt-in; browser apps
    can never call the API directly. (c) a built-in allow for
    `http://localhost:*` dev servers; any local web page on any port gets a
    CORS grant (the token is still required). Cost of (a): one key and its
    tests.
    **Decision (2026-10-14): (a) off by default; exact-origin opt-in via `listener.cors_origins`.**
11. **Concurrency defaults.** `max_concurrent_requests` 8,
    `max_queued_generations` 4, `queue_timeout_ms` 300 000,
    `max_connections_per_remote_address` 4. **R** these. A queue of 1 refuses
    a second `strata run` window plus an app quickly; 16+ lets one client park
    many requests behind a 120 s generation. Upstream queues without bound and
    never answers busy.
    **Decision (2026-10-14): 8 workers; queue derived min(4, max(0, workers−2)); 300 s queue timeout; per-remote-address cap derived min(4, workers).**
12. **Loopback residual after the slow-body fix.** Unauthenticated clients
    can now hold a worker only in P1 and are evicted oldest-first, but loopback
    peers stay exempt from the per-address cap. Residual: (i) another local
    user's high-rate connection flood causes eviction churn that can delay a
    legitimate local client's header by up to its own send time; (ii) an
    authenticated local client can occupy every worker. (a) **R** accept both
    and record them in the threat model; a token holder is trusted with the
    API's capacity. (b) also cap loopback at `max_concurrent_requests − 1` per
    address, keeping one worker for anyone else; since all local clients share
    one address, this only reserves a worker for remote peers and does not help
    local ones. (c) reserve one worker for `/health` only; `strata run`'s
    readiness polls never starve, at the cost of one idle thread. Cost of (a):
    none in code.
    **Decision (2026-10-14): (a) accept the loopback residual and record it in the threat model.**

**Decisions taken.** On 2026-10-14 the owner accepted every recommendation: (1) auto `max_tokens`; (2) clamp above the limit; (3) U+FFFD for invalid UTF-8; (4) partial text with `"length"` on timeout; (5) chunked transfer coding; (6) option (e) sampling; (7) `/v1/completions` out; (8) `/v1/messages` not in SP3; (9) Ollama `/api/*` out; (10) CORS off by default, exact-origin opt-in; (11) defaults 8 / 4 / 300 s / 4 as derived; (12) loopback residual accepted. Two plans: M1 (concurrency and hardening), M2 (streaming).

### 11.2 Settled engineering choices (recorded, not for the owner)

E1 fixed worker pool + accept thread (not thread-per-connection, not an event
loop). E2 busy is `503 server_busy` + `Retry-After` (not `429`; nothing
per-client was exceeded). E3 chunked **request** bodies rejected with `411`
(every client named by PX sends `Content-Length`; upstream accepts them,
`server.py:4604-4651`). E4 streaming never back-pressures the engine; stall
10 s, tail 60 s (not SP2's 1 s per chunk). E5 peer EOF or reset cancels a chat
at once (upstream does the same, `server.py:4751-4774`); `GET` keeps half-close
support. E6 tokens are counted in the frontend. E7 no keep-alive. E8 egress
checks are recorded against `PRIV-01`/`PRIV-03`/`DL-01` as their frontend part;
SP2 keeps the base `ext01`/`ext02` cases, which SP3 amends and extends. E9
`/v1/models` lists only the served model. E10 a loopback bind also accepts
`localhost:<port>` as `Host`.

## 12. Out of scope, deferred, and risks

### 12.1 Out of scope / deferred

Sampling beyond decision 6, `stop` strings, multi-part text content, `tools`,
vision, reasoning fields and other profiles (SP5); `/v1/messages`,
`/v1/responses`, `/v1/completions`, `/v1/embeddings`, Ollama-native routes
(decisions 7–9); keep-alive, HTTP/2, TLS on the listener; replicas or several
engines; per-token rate limits or per-client quotas; a metrics endpoint; log
files and retention (SP7); seccomp in the production binary (SP7); `x-api-key`
and multiple keys (SP1 §7); the CLI verbs themselves (PX/SP1/SP2), except the
loopback client library they call; `strata run` against a server whose only
listener is a LAN address (product follow-up: a second loopback listener or a
remote-capable client); changes to SP2's restart policy or `/health` body
(§1.5 lists what SP3 needs from SP2's second run).

### 12.2 Risks

| # | Risk | Mitigation / owner |
| --- | --- | --- |
| R1 | Slow clients hold workers. | Authentication before the body (§2.2) leaves unauthenticated clients only P1, bounded by the 10 s header deadline and oldest-first eviction (§2.3); tested by `dos01_slowloris_cannot_starve_a_real_client`, `dos01_slow_body_clients_cannot_starve_a_real_client` and `dos01_eviction_over_tcp_admits_new_client`. This bound holds only with both mechanisms in place. Residual on loopback: decision 12. |
| R2 | Thread-safety faults in the new pool, gate, port and logger. | One owner per object (§1.6 thread rule), one mutex around SP3's explicit calls (snapshot/acquire/report_fatal) in `SupervisorPort`, `std::mutex`/`condition_variable` only; load tests; a non-blocking `linux-clang` CI job with `-fsanitize=thread` is recommended in the M1 plan (no dependency). |
| R3 | Behavior changes for existing clients and tests: JSON error bodies, `415`, `411`, `/v1/models` contents, default worker count, `max_tokens` clamping. | Listed in §9.4; each change has a test; README and handoff state them. |
| R4 | `STOP` during a long real prefill may not reach `DONE … cancel` within `stop_drain_timeout_s` (5 s). SP2 kills and restarts the engine; with D-2 landed (§1.5, §2.6) the first 3 consecutive such client-STOP drain failures are not counted, so client disconnects during long prompts no longer open the circuit by themselves; a 4th in a row with no `DONE` between counts like `drain_failed`. | D-2 landed in SP2 run 2; residual: measure `STOP` latency on the GPU box (§9.5 item 2) and, if needed, the owner raises `limits.engine.stop_drain_timeout_s` (SP1 range allows 600 s). |
| R5 | Client SDK retries amplify engine-side failures: `openai-python` retries `5xx` twice by default, so one client-caused `ERR` can become three counted failures and an open circuit (SP2 plan Review Focus 6). | `engine_error` carries `x-should-retry: false` (§3.4); clients that ignore the header remain a residual risk, recorded with SP2's. |
| R6 | No bytes are sent while a stream waits in the queue (headers come after the lease), so a client with a short read timeout may give up during a long wait. | Queue length 4 bounds the wait to about 4 generations; `queue_timeout_ms` configurable; headers-first was rejected because every early refusal would become a `200` with an error event. |
| R7 | Proxies that buffer SSE. | `X-Accel-Buffering: no`, `Cache-Control: no-store`; loopback use has no proxy. |
| R8 | Egress checks are tripwires: `dlopen`, `GetProcAddress`, raw syscalls and sub-50 ms sockets on Windows escape them. | Three independent checks (§6.2); limits recorded in the threat model; whole-machine monitoring in the SP7 kit. |
| R9 | SP1/SP2 land with interface details different from their specs. | §1.6: SP3 starts after both merge and re-anchors; any conflict stops for the owner. |

## 13. Execution and acceptance

### 13.1 Order

**M1 plan:** JSON writer and error table → header-first parser split and early
authentication (§4.1) → parser rules (§4.2) → Host/Origin/CORS keys and policy
→ schema rows and mechanism extensions (§5.1) → connection pool with polled
reads, eviction, lingering close → `SupervisorPort` and the gate → disconnect
cancel for non-streaming chats → `max_tokens` resolution and sampling subset
(decisions 1, 2, 6) → egress tripwires, interposer and scans → EXT amendments
→ logging → Windows cases → handoff.
**M2 plan:** `next_event` and `Utf8StreamDecoder` → SSE encoder with chunked
framing and event cap → streaming path and backpressure → keepalive → finish
mapping and error events → shutdown during stream → API client and
`strata-api-probe` → kit procedure → handoff.
Each task: test first, full `ctest --preset linux-gcc`, cross build, one local
commit naming its IDs and test names, handoff row updated.

### 13.2 Acceptance for SP3

1. `ctest --preset linux-gcc` green with every L and B case of §9.3, skips
   only as listed in §9.1.
2. `windows-llvm-mingw-cross` builds with `-Werror`; wine smoke with
   `-LE windows-runtime` passes and is not cited for W rows.
3. After the owner's push: `windows-msys2-clang` green for every W case and
   every B case; `linux-gcc`, `linux-clang`, cross, `top-level-integration`
   green; `windows-msvc` informational.
4. `ctest -N` lists every case name of §9.3 individually.
5. No tuning number outside SP1's schema or `api_constants.hpp`.
6. `docs/AGENT_HANDOFF.md` (including the SP1 addendum and the SP2 second-run
   follow-ups D-1, D-2), `docs/security/engine-protocol-limits.md` (one
   generation at a time, now queued; streaming exists; `GEN` sampling keys),
   `docs/security/threat-model.md` (decision 12 residual, R4, R5 and the
   egress-check limits of R8), `secure_frontend/README.md` (routes, errors,
   keys) are accurate; nothing is claimed for a GPU or for Windows beyond CI
   logs.
