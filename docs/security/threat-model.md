# Portable frontend threat model

## Status and scope

This document defines the release gate for the Strata-compatible portable
frontend. It applies to the code that starts and supervises the inference
engine, exposes HTTP APIs, reads configuration, and acquires model artifacts on
Windows and Linux. It does **not** change the upstream inference engine or make
claims about the safety of untrusted model weights.

The default deployment is one user running a local service on a workstation.
Remote or shared-network operation is a deliberate opt-in and has a separate
acceptance gate below.

## Assets and trust boundaries

Assets requiring protection:

- API tokens and credentials supplied to the process.
- Prompts, responses, uploaded images, model-selection data, and usage data.
- The host filesystem, including model directories and configuration files.
- GPU/CPU, memory, and the availability of the local inference service.
- Integrity of executable, engine, and model artifacts.
- Requests from clients, browsers, LAN peers, and child-process output.

Trust boundaries:

1. HTTP clients and browser-originated requests are untrusted.
2. Config files, environment variables, CLI values, and model manifests are
   untrusted input until validated.
3. Download servers and downloaded bytes are untrusted until their expected
   digest is verified.
4. The inference engine is a child process with a constrained, explicit
   argument and environment contract.
5. A model file is data, not executable code; loading untrusted weights is not
   a security boundary and must be disclosed to the operator.

## Security contract

### Network and authentication

- Default bind addresses are loopback only (`127.0.0.1` and `::1` where
  available). Wildcard, LAN, and public binding require an explicit
  `--listen`/configuration opt-in.
- Wildcard addresses (`0.0.0.0` and `::`) require a second, distinct opt-in;
  selecting a remote listener alone must never expose every interface.
- Every API route, including health and model discovery, requires a non-empty
  bearer token by default. A token must be generated with a cryptographically
  secure RNG, never logged, and compared in constant time.
- An unauthenticated mode is permitted only with an explicit development-only
  switch; it must refuse non-loopback binds and emit a high-severity warning.
  Implemented 2026-10-21 (owner decision) as `listener.loopback_without_key`:
  config file only (no flag, no environment variable), refused for a remote or
  wildcard bind at load and again by the listener's own validation at bind,
  refused next to a `token.source`, and announced by a `strata: warning:` line
  on stderr and in the ready line. Host, Origin and Content-Type checks still
  apply (residual risk below).
- Rate/size/concurrency limits are configured with safe finite defaults. The defaults
  stayed small when the schema maxima were raised to upstream's scale for long contexts
  (2026-10-21: 256 MiB request/body/message, 1,048,576 prompt and output tokens, 65,536
  messages). An operator who raises them accepts that each worker may hold one request
  that large in memory (bounded by `limits.concurrency.max_concurrent_requests`).
  One remote address may hold at most `limits.concurrency.max_connections_per_remote_address`
  workers: derived min(4, workers), and min(6, workers) with `listener.web_ui` (owner,
  2026-10-23: a browser fetches the page's files over up to six connections, and a phone lost
  `app.js` to the cap of 4). At the default 8 workers one page client can then hold 6 of them;
  a header-phase connection is still evictable, and an explicit value is used as written.
  Streaming requests must be cancellable on client disconnect and shutdown.

### Privacy and offline operation

- The distributed binary has no analytics, telemetry, crash-report upload,
  update check, remote configuration, advertising, or usage-reporting client.
  It must not contact an external service during installation, startup,
  inference, shutdown, or failure handling.
- Prompts, completions, images, API metadata, and local model information stay
  on the operator's machine. Request logging is disabled by default; any
  operator-enabled diagnostic log must redact credentials and use a bounded,
  documented local retention policy.
- The networked operations are the explicit, foreground `strata pull` (acquire),
  `strata update`, and — only for a model that is not installed and only after
  the same confirmation — `strata run`. Each runs only after the operator
  confirms the manifest-pinned hosts and digests it displays. `strata import`,
  `export` and `verify` are local. None can be invoked by the HTTP API, a model,
  a plugin, configuration or automatic logic — with one owner-accepted exception
  (2026-10-23, model downloads D1): with `models.downloads` set, a key holder on
  the Models tab may start a `strata pull` of a catalog card or a confirmed
  Hugging Face `.gguf` link. The server itself still holds no network code; the
  pull runs as a separate confined `strata` child (below, "Model downloads").
  A second owner-accepted exception (2026-10-27): a server **built with**
  `SECURE_FRONTEND_EMAIL_ALERTS` (off by default) and with `alerts.email.enabled`
  set connects to the configured SMTP host itself to send alerts (below, "GUI-first
  start-up"). A default build has no mail code, and no build connects anywhere
  until alerts are enabled in config.
  A third owner-accepted exception (2026-10-27): a server **built with**
  `SECURE_FRONTEND_WEB_SEARCH` (off by default) and with `search.enabled` set runs
  `strata-search`, a separate program beside it, which connects to the configured
  SearXNG instance for the model's searches (below, "Web search"). The server process
  itself gains no resolver or outbound connection for it.
- Privacy-relevant behavior is build-time auditable: the release manifest lists
  enabled network-capable components, and CI tests prove that a normal service
  lifecycle makes no outbound network connection.

### Browser and HTTP boundary

- Reject `Host` headers not matching the configured loopback host/port or an
  explicit allowlist. Reject ambiguous, malformed, duplicate, or control-byte
  host values.
- Reject browser `Origin` values unless the origin is same-origin or exactly in
  an explicit allowlist. Missing `Origin` is allowed for non-browser API
  clients after token authentication.
- The built-in chat page (`listener.web_ui`, off by default, 2026-10-21; upstream's
  look 2026-10-22) is the only unauthenticated content: bundled files (ASCII text,
  plus the WOFF2 font) embedded at build time, served at exact `GET` targets after
  the Host and Origin checks, under their own CSP (`script-src`/`style-src`/
  `connect-src`/`img-src`/`font-src 'self'`, no inline code, `form-action 'none'`,
  `frame-ancestors 'none'`) and
  `Cross-Origin-Opener-Policy: same-origin`. They hold no secret and reach no
  route. The page keeps the key in `sessionStorage` (that tab), sends it only as
  a same-origin bearer header with cookies off, and renders model output as text
  only. Tests pin those properties of the files (no inline script or handler, no
  HTML sinks, no other host, no `localStorage` in the chat script; the theme
  script stores only `strata.theme`).
- The Monitor (`listener.monitor`, off by default, 2026-10-22; session log step
  16) is one more keyed route, `GET /metrics`, answering 404 while off. It tells a
  key holder what the server is doing — the engine state, the queue, the last 100
  generations' token counts, finish and the engine's own timings, and this
  machine's CPU, RAM and GPU readings — never a prompt, an answer, a client
  address or a key. Everything is in memory and bounded (100 records, 60 samples a
  series) and gone at exit. A key holder already sees the model and can time its
  own requests; the new disclosure is other clients' request sizes and times, and
  the hardware's state. The GPU is read by NVIDIA's `nvidia-smi`, started by the
  server as its own child from the system directory only (Windows
  `GetSystemDirectoryW`, Linux `/usr/bin`), with a fixed argument list, no shell,
  a fixed environment (Windows: `ProgramFiles` alone, which NVML needs, read from
  HKLM's `ProgramFilesDir` — never from the server's own environment, so whoever
  starts the server cannot point NVML elsewhere) and the engine's Job/process-group
  handling, and killed on
  shutdown. Its stdout is parsed strictly (ten fields, bounded, fail-closed per
  line); no vendor library is loaded into the server. (2026-10-28) The About's
  list of GPUs runs the same tool once, on the first About request, the same
  way (`--query-gpu=index,name,memory.total`, ten seconds at most), parsed as
  strictly (three fields, an index 0-63, a printable name, at most 16 GPUs). A
  card's GPUs choice is typed: `"auto"` or GPU numbers 0-63, never free text,
  written only to `engine.gpu` or `glm.gpu`. Residual: that tool is the
  driver vendor's code, trusted like the driver the engine already uses.
- CORS is disabled by default: no `Access-Control-Allow-Origin` header is
  emitted. When configured, emit only exact allowed origins; never reflect an
  arbitrary origin and never combine `*` with credentials.
- State-changing routes reject cross-origin browser requests even if a token is
  present, unless the origin is explicitly trusted. Preflight responses are
  restricted to allowed methods/headers and do not disclose credentials.
- Parse HTTP and JSON with bounded headers, body size, nesting, request time,
  and streaming buffer limits. Return generic client errors; details belong in
  protected server logs.
- (SP3) Authentication happens when the header is complete, before any body
  byte: an unauthenticated client never has a body read and can hold a worker
  only in the header phase (`limits.http.header_deadline_ms`, oldest-first
  eviction of the connection longest in that phase). Bodies are read only from
  token holders.
- (SP3) One accept thread plus a fixed worker pool (`max_concurrent_requests`,
  default 8) with a per-address cap for non-loopback peers; over-admission and
  an over-full generation queue (`max_queued_generations`, default 4) get one
  fixed 503 `server_busy` + `Retry-After: 1` read through a lingering close
  (never an RST the client's stack could cache as an attack signature).
- (SP3) Every error body and the optional per-request log line are fixed
  literals — never a token, prompt, configured path or client payload
  (`err_no_token_prompt_or_path_in_any_body_or_log`).
- (SP3 M2) Streaming responses are SSE under HTTP/1.1 chunked framing. A stream
  cut before its end carries neither the terminal chunk nor `data: [DONE]`: the
  truncation is visible to SDKs and to `curl -N` rather than silently accepted,
  and `[DONE]` is never a success marker — an in-stream error event is followed
  by `[DONE]` after the text already sent has been flushed
  (`stream_cut_has_no_terminal_chunk`, `err_mid_stream_is_error_event_then_done`).
- (SP3 M2) No bytes while queued (R6): a request waiting on the generation slot
  has written nothing to its socket, and a full queue is refused with a plain
  503 `server_busy` before any SSE header (`dos02_queue_full_with_streams_is_503_before_headers`).
- (SP3 M2) Slow-reader bound: a non-reading client never backpressures the
  engine (the pump reads at engine speed); unread decoded text is bounded per
  streaming worker by `limits.chat.max_generated_bytes` plus one pending SSE
  event of at most 128 KiB in the socket sink, and after
  `limits.http.write_stall_ms` without progress the stream is cancelled with
  `STOP` (`dos02_slow_reader_never_backpressures_engine`,
  `dos02_slow_stream_reader_hits_write_stall`).
- (SP3 M2) R4's real numbers are only measurable on the owner's GPU boxes:
  `docs/validation/sp3-api-kit.md` with the `cancel-during-prefill` probe mode
  is a release-record step, not a CI proof (see R4 under residual risks).
- (SP5 M2) GLM option-surface parity is a config/launch-surface claim and is
  attested only by the owner-box acceptance run recorded in `docs/PROVENANCE.md`
  (`docs/validation/sp5-parity-kit.md`, corpus `sp5-parity-corpus.txt`): until
  that entry exists — command lines, engine versions, pack and results hashes —
  no GLM parity claim is made anywhere in this repository.
- (SP5 M1) Image input exists only for a vision-capable model (`vision.enabled`
  plus the manifest `mmproj` artifact): the OpenAI content-parts grammar opens
  only for that model, an image URL must be a `data:` URL — an http(s) image
  URL is refused by name and never fetched, so no image path reaches any
  resolver or socket — and every cap (images per request, base64 length, 32 MiB
  decoded, the side-length probe) is answered before any scratch file is
  written; the refusal is the fixed 400 naming `image_url`.
- (2026-10-28) A model card reads pictures only with its own encoder file
  (`vision.mmproj`, a card row the main config refuses): absolute, a regular
  file and not a link, in a folder that passes the model folders' rule (no
  other user may change it, unless the owner set `roots.allow_untrusted_media`),
  checked at each launch. Only the qwen profile opens the content-parts gate;
  every other card's launch refuses `vision.enabled` by name. The same `data:`
  URL rule and caps as above hold, and the page sends pictures only while the
  running card says it reads them.

### Configuration and filesystem

- Configuration is schema-validated before start. Unknown keys are errors;
  security-sensitive values cannot be silently ignored or coerced.
- Paths are converted to canonical/absolute paths and must remain below an
  explicitly configured root after symlink/reparse-point resolution. Traversal,
  NUL/control characters, device paths, and paths outside the root are rejected.
- On Windows, apply the same policy to drive-relative, UNC, extended-length,
  alternate-data-stream, and junction/reparse-point paths. On Linux, cover
  symlinks, bind mounts where observable, and `/proc`/device paths.
- Secrets come from a protected file or environment at startup and are never
  copied into logs, command lines, telemetry, crash reports, or child arguments.
  A Windows implementation may use DPAPI for at-rest local token storage.
- Long-lived parsed configuration stores only secret-source metadata and
  validation state, never a plaintext API token. The authentication component
  owns its short-lived secret representation.
- The service creates data files with user-only permissions where the platform
  supports them and rejects configuration files writable by untrusted users.
- (SP5 M1) Upstream-style environment configuration knobs (`STRATA_*`,
  `MAYA_*`) never configure the frontend — the typed config file is the only
  source — and `strata doctor` warns when such variables sit in the
  environment, so a user who expects the pinned server's behaviour sees that
  they are ignored.
- (SP5 M1) `strata serve` is foreground-only: it never daemonizes and never
  writes a lifecycle file (no pidfile, no `server.json`, no lock). The pin has
  none of these, and our lifecycle writer has never existed.
- (SP5 M1) Vision staging and encoded scratch files are 0600 regular files
  under `<state_dir>/vision` (0700), and every request deletes its own files
  when its generation ends, by success or failure. The pin's
  `session.keep_files` LRU has no consumer in M1 (recorded gap); the
  per-request cleanup bounds what the directory holds. Boot with
  `vision.enabled` fails when the state path contains whitespace — the
  encoder's positional argv cannot carry it safely (pin issue #480).

### Artifact acquisition

- Runtime networking is off by default. No model, executable, dependency, or
  plugin is downloaded implicitly on startup or request handling. The one
  request that can start a transfer is a model download route, off unless
  `models.downloads` (below).
- An explicit acquire command accepts a manifest containing HTTPS source,
  artifact name/version, byte limit, and SHA-256 digest. The artifact is written
  to a temporary file, size-checked, hashed, then atomically moved into the
  configured model root.
- Redirects must remain HTTPS and obey an allowlist/policy. Archive extraction,
  if introduced, must defend against traversal, links, decompression bombs, and
  unexpected file types. Initial releases should distribute verified files, not
  arbitrary archives.
- Build dependencies and release artifacts are pinned, provenance recorded, and
  accompanied by an SBOM. Verification failure leaves no accepted artifact.
- Every file is SHA-256-pinned by a manifest that is compiled into the release
  or pinned by an operator-typed digest; redirects are followed only by the
  frontend, only over HTTPS and only to hosts the manifest names.
- **Parallel ranged downloads (2026-10-23, `pull.connections` > 1).** A large file may be fetched
  as 32 MiB pieces over several confined downloader children at once (Hugging Face's CDN limits
  each connection). The trust boundary is unchanged: one probe follows the manifest's redirects to
  the final URL (host in the manifest's hosts/redirect-hosts, DNS resolved and the address pinned,
  as every hop); each piece is a bounded range request to that URL, `--max-redirs 0`, pinned per
  connection, and its 206 must carry the exact Content-Range for the piece and the object's
  unchanged ETag (a changed object fails closed). Bytes are bounded per piece; the manifest still
  pins only the **whole-file** SHA-256, verified in one pass over the reassembled file, so a wrong
  offset, a changed object or any corruption leaves no accepted file. Confinement is unchanged
  (one `curl` child per connection, same Job/Low-integrity/Landlock); the signed CDN URL's query is
  never logged; the `.part.chunks` resume sidecar holds only sizes, an ETag and a bitmap. A server
  without range support, or a proxy, falls back to one stream. The
  downloader runs confined (on Linux with Landlock: no file writes and reads
  only below system directories; on Windows: Low integrity, which blocks writes
  but not reads of the user's files). Confinement limits but does not remove
  what a compromised downloader can do: it can open network connections to any
  host, including LAN and loopback services (Landlock restricts only ports and
  only from ABI 4; Low integrity restricts no network), read and send the
  user's files on Windows, and on Linux change modes, owners and timestamps and
  — below Landlock ABI 3 — truncate files. Digest verification still prevents
  it from planting a model.

### Engine process isolation

- The frontend starts the engine without a shell. It passes a fixed executable
  path and an argument vector built from validated typed values; user strings
  never form shell syntax.
- The child gets a fixed, frontend-rendered environment and nothing from the
  parent: POSIX `PATH=/usr/bin:/bin`, `LD_LIBRARY_PATH` (the engine library
  directories), `TMPDIR`, `CUDA_CACHE_PATH`; Windows `PATH`, `TEMP`, `TMP`,
  `CUDA_CACHE_PATH`, `SystemRoot`, `WINDIR`, `SystemDrive`,
  `NUMBER_OF_PROCESSORS`. The renderer also supports optional
  `STRATA_HIPBLASLT_TUNING`, `STRATA_WATCHDOG_S` and `STRATA_WATCHDOG_IO_S`
  rows, but no configuration key sets them yet. Never the token variable,
  `STRATA_API_KEY` or other `STRATA_*`, `LD_PRELOAD`, `LD_AUDIT`,
  `CUDA_LAUNCH_BLOCKING`, `HOME`/`USERPROFILE`/`APPDATA`, proxies, or
  GPU-selection variables (strata family: GPUs are the typed `--gpu` argv; the
  `glm` family — SP5 M2, `glm53` manifests — instead renders a **closed typed
  child environment**: the GLM parity table's typed `engine_env` set is the
  100-name allowlist `kFamilyForcibleEnvironment`, the two GPU selectors
  `CUDA_DEVICE_ORDER`/`CUDA_VISIBLE_DEVICES` come from typed `glm.gpu`, the
  SP6 §4.9 safety values are forced defaults a typed row may move, and no
  other `STRATA_*`/`MAYA_*` name reaches that child at all
  (`glm_child_env_is_exactly_the_named_typed_set`); the VLEND row
  `STRATA_GLM_VISION_LEND_MB` renders only with a configured vision turn). Not
  yet verified with a real CUDA/ROCm engine. The child inherits no listening
  socket, credential descriptor/handle or token. On POSIX it starts with
  default signal dispositions and an empty signal mask; on both platforms its
  cwd is `<state_dir>/engine`; on Linux `PR_SET_NO_NEW_PRIVS` is set by default
  (`engine.allow_new_privileges` turns it off for a GPU stack that needs it).
  Its stdout is the bounded strict protocol pipe; stderr is discarded to the
  platform null device so diagnostics cannot corrupt protocol framing or leak
  prompts into logs.
- The child is reached only by private stdio in the current profile; clients
  never receive an engine address. Any future private loopback/IPC bridge must
  be authenticated and blocked from non-frontend peers.
- (SP5 M2) The engine wire protocol is a recorded per-manifest **dialect**
  (`strata` or `maya_118`): a `glm53` manifest's child speaks Maya's framing
  (NOTE/STAT records, 3–6-field PP progress, STOP only when READY advertises
  it; since Maya v1.1.21 also bounded `VRAM key=value …` and non-empty `NOTE`
  records before READY), a qwen manifest's keeps SP2's strict dialect; in
  either dialect an unknown, out-of-order or unrequested record kills the
  generation or the start (`fake_maya_dialect_round_trips_only_under_the_maya_dialect`,
  `fake_maya_vram_records_reach_ready_only_under_the_maya_dialect`) — a dialect
  relaxes framing, never what a confused or hostile child may do. Maya v1.1.21
  spells its user settings `MAYA_*` and copies each `MAYA_X` over `STRATA_X` at
  engine start, so a `MAYA_*` name reaching the glm child would override a
  forced `STRATA_*` safety value; the closed environment allowlist keeps every
  `MAYA_*` name out (`glm_child_env_is_exactly_the_named_typed_set`).
- (Gemma 4 plan 1) A third chat profile, `gemma4`, adds no new control or surface: its child
  is the strata family (the same closed environment, the `strata_0143` dialect, private
  stdio); its argv is the `strata_gemma4` allowlist (unknown flags refused without being
  echoed); its prompt is a fixed renderer of the GGUF's template that refuses every one of the
  vocabulary's 24 control literals in client text; its tokenizer mode comes from the profile,
  never the files; and the thought channel's name line must be exactly `thought` or the reply
  ends `undecodable`. Managed `gemma4` manifests stay refused by the engine gate until plan 4.
- (flint, 2026-10-27) The `flint` chat profile adds no new control or surface either: its child is the strata family
  (closed environment, `strata_0143` dialect, private stdio, the one `multiengine.exe` with `multiengine_flint.dll`);
  its argv is the `strata_flint` allowlist (test-only flags and file paths refused by name, unknown flags unechoed);
  its prompt is a fixed renderer of the GGUF's template whose `flint_text` escapes write the framing literals as
  `&lt;|...|&gt;`, and client text that would still hold one of the five markers after the escapes (or an earlier
  answer holding one) is refused, as is any other special literal of the vocabulary (the encoder takes the markers only
  from the renderer); its tokenizer mode comes from the profile; the four end-of-generation ids end the reply and are
  never output.
- (GLM-5.3 replies, 2026-10-27) A `glm53` reply now starts as reasoning (the template's prompt ends in `<think>`) and
  its `</think>` moves the reply to the answer; `<|endoftext|>`, `<|user|>` and `<|observation|>`, each only as one
  CONTROL token of the pack, end it. Those tags are never output, and every other special token in a reply still ends
  it `undecodable`. Client text still cannot carry any of them: the GLM encoder takes its markers only from the
  renderer.
- (DeepSeek-V4.1, 2026-10-27, entry p) The `deepseek` profile adds no new control or surface: its child is Maya's
  maya-ds4 behind the one `multiengine.exe` (`multiengine_maya_ds4.dll`), the strata family's closed environment,
  private stdio and Maya's `maya_118` dialect; its argv is the `maya_ds4` allowlist with `--pack` and `--vision off`
  composed (the weight source, one-shot modes, vision, drafter and expert-profile path refused by name). Its prompt is a
  fixed renderer of DeepSeek's encoder that refuses DeepSeek's framing literals, `｜DSML｜` and control bytes in client
  text; its encoder takes the seven markers only from the renderer, each one CONTROL or USER_DEFINED token. One new
  behaviour: maya-ds4 stops only on its EOS, so `<｜User｜>` (and the EOS itself) are server stops - the reply ends
  there as stop and the frontend sends one STOP and drains to DONE within the usual drain bound, as Maya's server
  does. A STOP the engine reads after its own DONE is cleared at its next GEN. The tokenizer's classes are a generated
  table (no regex engine at run time).
- (MiniMax-M3.1-Flash, 2026-10-28, entry v) The `minimax` profile adds no new control or surface either: Maya's
  maya-mm3 behind `multiengine.exe` (`multiengine_maya_mm3.dll`), the strata family's closed environment, private stdio,
  `maya_118`; its argv is the `maya_mm3` allowlist with `--pack`, `--kv-streaming on`, the shipped expert profile,
  `--gpus` and `--vision off` composed (one-shot modes, vision, drafter, `--tp` and every path refused by name). Its
  prompt writes the template's literals only as the renderer's markers; a message's literals are plain text (Maya's
  marking), so client text cannot open a turn. `[e~[` and `]~b]` are server stops, as DeepSeek's. The pack step reads
  the shipped template, a file of the install, never one a request names.
- (Gemma 4 plans 2-3) The Gemma engine is our own code, reviewed like the frontend: until 2026-10-26 the
  standalone `strata-gemma4-engine` (`engines/gemma4/`), now the `strata_gemma4` family library of
  `multiengine.exe` (`engine/src/gemma4/`; `main()` hands it a `gemma4` GGUF), so its process, import list and
  launch are the one engine's. It compiles nothing from `upstream/`, writes no file in serve mode (`--dump-*` and
  `--tokens` are refused with `--serve`), reads only the model named by the frontend-owned `--native`, and fails
  closed on a malformed GGUF (bounded parser; every tensor's type, shape, offset and size checked) or GEN line (ids
  in range, the context bound). It pins about 23.5 GiB of host RAM for the experts (the box's 64 GB is the limit,
  not a frontend control) plus about 100 KiB of mapped memory its CPU-lane thread and the GPU share inside
  the process (the decode doorbell, flags, plan and rows); nothing of it leaves the process.
- PROC-03 on Linux means "other user context": another uid cannot open the
  engine's stdio (`proc03_other_uid_cannot_open_engine_fd`; it runs only in CI,
  which has not yet run SP2 — pending the owner's push). The
  **same** uid can reopen the engine's stdin through `/proc/<pid>/fd/0` and
  inject protocol lines; the upstream engine cannot be made non-dumpable after
  exec and its protocol carries no nonce without engine changes. This is an
  **accepted risk** for SP2 and must appear in the release record
  (`proc03_same_uid_reopen_is_accepted_risk` pins today's behavior). On Windows
  the stdin pipe is owner-only, single-instance and client-verified (N6, R1)
  (implemented; authoritative in the MSYS2 CI job — pending the owner's push).
- Windows uses restricted handle inheritance plus a Job Object so all child
  processes terminate with the service. Linux uses close-on-exec descriptors
  plus a descriptor sweep before `execve`; the listening and accepted sockets
  are not close-on-exec (no `SOCK_CLOEXEC`), so the sweep is what closes them
  in the engine child (the fallback sweep's 65536 cap is a recorded gap,
  `docs/AGENT_HANDOFF.md`, Known small gaps), a dedicated process group, and
  reliable `waitpid`/signal cleanup. A single supervisor thread owns the
  engine: it restarts a failed engine after 1 s, 2 s, … (at most 60 s), opens
  a circuit on the third consecutive failure (closed only by restarting the
  frontend), and resets the count after 300 s of healthy running. Because an
  `ERR` answer leaves the generation unusable, three requests in a row whose
  input makes the engine answer `ERR` open the circuit until the frontend is
  restarted; this is accepted (Review Focus 6,
  `proc04_client_err_lines_open_circuit_until_restart`). A drain failure after
  a client-requested `STOP` (SP3) restarts the engine without counting, at most
  3 times in a row with no `DONE` in between; after that it counts. Linux
  cleanup is process-group SIGKILL, a child-subreaper sweep after every reaped
  generation, `PR_SET_PDEATHSIG`, and `cgroup.kill` when a delegated cgroup v2
  is detected (the delegated path is verified by reading only, not run).
  **Residual risk (PROC-05):** a frontend killed with SIGKILL on Linux takes
  the engine leader with it (`PR_SET_PDEATHSIG`), but the engine's own
  descendants survive. Without a delegated cgroup nothing reaches them; with
  one they stay in the generation's cgroup until the operator's scope is
  killed, because nothing writes `cgroup.kill` once the frontend is dead. The
  upstream engine is not known to spawn any. This must appear in the release
  record.
- Privilege elevation is forbidden. The service and child run as the invoking
  user (or a documented dedicated low-privilege account); writable roots are
  narrowly scoped. The frontend refuses to start as root or with an elevated
  Windows token; an engine that requires elevation is a spawn failure and is
  never started via `runas` (G3 is verified only on a non-elevated Windows
  prompt, a validation-kit step; CI runners are elevated).
- (SP5 M1) The vision encoder is a second supervised child (`strata-vision`, a
  sibling of the engine executable): the same fixed environment allowlist, cwd
  `<state_dir>/vision`, private stdio only, READY/ENC/QUIT role protocol with
  typed ready/encode timeouts, and a resident encoder that fails the boot if it
  cannot reach READY. **Recorded gap:** in M1 the encoder role receives no
  per-generation cgroup delegation (the PROC-05 residual above applies to it
  too, one role smaller). (2026-10-28) A model card's encoder is the same
  confined child, owned by that card's runtime: started after the engine's
  READY, stopped with the engine (on a stop, a swap and the server's exit), and
  a card whose encoder fails to start does not start. It is upstream's
  `strata-vision` from the out folder's `engine/` (beside the launcher), never a
  path the config names. Lifecycle timers are typed frontend decisions —
  `engine.lazy_load` defers the engine start, `engine.idle_unload_s` reaps an
  idle engine child, the MTP rows compose argv — they hand no control to the
  engine.
- (2026-10-27) Our engine executable `multiengine.exe` is a launcher: it loads
  one family library (`multiengine_strata.dll`, `multiengine_maya.dll`,
  `multiengine_maya_ds4.dll`, `multiengine_maya_mm3.dll`, `multiengine_raon1.dll`,
  `multiengine_gemma4.dll` or `multiengine_flint.dll`) by absolute path from its own folder, chosen from
  the command line: the source GGUF a `--glm-pack` names in its
  `native_experts.txt` (first line, at most 4096 bytes), the GGUF header of
  `--native` / `--model`, or the presence of `ds4.txt` / `mm3.txt` in a lone
  `--pack` folder. Every read is bounded (lengths capped before use) and
  read-only; a file it cannot read goes to a library that reports it.
  **Recorded gap:** the frontend's executable checks cover the `.exe` only;
  the libraries beside it (as `strata_kq.dll` already was) are trusted with
  that folder and not verified separately, and their own dependencies (the
  CUDA libraries) resolve by the system's usual DLL search. Project Maya's
  libraries also load `advapi32.dll`, `dxgi.dll` and `dbghelp.dll` by bare name
  (as Maya's executables do), which Windows looks for in the engine's folder
  first: one more reason that folder must stay owner-only. Maya's programs open
  no network connection and start no process (docs/PROVENANCE.md, the Maya
  supply-chain read); a decode stall in the GLM program writes a minidump
  `maya-stall-<pid>.dmp` into the engine's working directory.

### Model launcher (models.cards, 2026-10-23)

Spec `docs/superpowers/specs/2026-10-23-model-launcher-design.md` §7; the owner accepted
D1-D10.

- The page and the API never supply a path, an executable, an environment variable or a
  free-form argument. A start names a **model card** (an owner-only file the operator wrote,
  read like the config file: trusted directory, no link followed, not writable by others) and
  typed option values. Each value is checked twice: against the card's bounds (inside the
  engine's own, model_options.cpp) and by the profile's argument rules when the config is
  validated (the `strata_gemma4` allowlist, the frontend-owned flags). A card may not fix a
  flag one of its listed options sets, so the page's value is the only one.
- Model control (`/v1/strata/models*`) is off unless `models.control`, keyed like every route,
  and for loopback peers only unless `models.control_remote` (D2); the peer's class comes from
  the accepted socket's address, never from a header. Since 2026-10-27 (owner: "add
  172.16.1.0/24 and 172.16.2.0/24 to the control_remote", "it should allow hosts list or
  subnets") `models.control_remote_networks` narrows remote control to listed IP addresses and
  CIDR subnets, matched against that same socket address. Names are never resolved, and a
  subnet with host bits set is refused rather than widened. An empty list keeps D2's rule (any
  key holder). The page's model-file routes (this PC's folders, a card from a file, a card's
  folder, the danger-zone delete) also answer clients in a listed network (owner, 2026-10-27:
  "Allow all from subnets"), never through `control_remote` alone. The delete keeps its code, key
  and confirmation checks. The alert routes stay loopback-only. Every start and stop is logged on stderr
  with the card, the option values and the client's class, never the key.
- A start validates and composes the new model (its tokenizer loads) before the running engine
  is touched, so a bad card or option never takes the running model down. One start or stop at a
  time; during a switch chat answers 503 instead of queueing. The generation in flight finishes
  for up to 60 s and is then ended through the same stop flag as a shutdown (D3). The old engine
  goes through the supervisor's orderly shutdown before the new one spawns: one supervised engine
  at a time, the same environment, confinement and Job/process group as a single-model server.
- Each start gets its own supervisor, port, gate and chat route bound to that card's tokenizer
  and profile (ModelRuntime). A request holds the runtime it was validated against until its
  response or stream ends, so it never runs on another model's engine.
- The remembered options (D7) live in `<roots.state>/model-options.json`, written with the
  owner-only atomic writer; a file of another shape is ignored (the card's defaults apply).

### Model downloads (models.downloads, 2026-10-23)

Spec `docs/superpowers/specs/2026-10-23-model-downloads-design.md`; the owner's rules O1-O4
(O4: "make sure there is a safety in the download that only downloads from huggingface and a
gguf") and decisions D1-D8; pause and resume added the same day.

- **Off by default; the same gate as model control.** The download routes
  (`/v1/strata/downloads*`) are no routes unless `models.downloads`, keyed like every route,
  and for loopback peers only unless `models.control_remote`. The page and the API name a
  catalog card's id or a link; never a path, a host, a file name of the store, an executable or
  an argument.
- **No network code in the server.** Every transfer is a `strata --config <the server's config>
  pull --manifest <m> --trust-manifest sha256:<d> --yes --progress-lines` child, every link
  lookup a `strata ... hf-info <link>` child: no shell, a fixed argv, a closed environment (the
  system's variables, a private temp folder, `STRATA_GGUF_PY` when the server has it), the
  working folder `<roots.state>/downloads`, the Job / process group. Inside it SP4's pull runs
  unchanged: the confined downloader child, HTTPS only, every file's size and SHA-256 fixed in
  the manifest before a byte moves, and the manifest itself trusted by its digest. The server
  reads only the child's fixed progress words (`MANIFEST`, `FILE`, `BYTES`, `FINISHED`,
  `RESULT`).
- **O4, three times.** The page pre-checks a pasted link (convenience only). The server's link
  grammar (`hf_source`) accepts only `https://huggingface.co/<owner>/<repo>/(resolve|blob)/<rev>/
  <path>.gguf`: no other host, no port, no user info, no `..` or empty segment, no datasets or
  spaces, a path ending `.gguf`. The manifest the server writes (profile `gguf`, or `glm53` for
  Maya's GLM files) lists only `weights`/`mmproj` roles with `.gguf` paths, names the host
  `huggingface.co` and allows redirects only to `*.huggingface.co`, `*.hf.co`,
  `*.xethub.hf.co`, `*.cdn.hf.co` and `*.aws.cdn.hf.co` (Hugging Face's own storage and CDN; SP4's
  wildcard covers one label, so each level is named); the engine gate refuses a `gguf` manifest as a
  launch, so a downloaded file runs only through a card.
- **What is trusted.** Catalog cards carry the commit, size and SHA-256 that upstream's
  `setup.py` pins (Maya's: SHA-256 at `main`; the commit and sizes are looked up at start and
  every reported SHA-256 must equal the pinned one). A pasted link is trust on first use: the
  commit and each file's LFS SHA-256 and size come from Hugging Face's tree listing at the moment
  of the check (one answer, 200 only, no redirect, 1 MiB cap), are shown to the person, and are
  then fixed in the manifest; the confirmation is valid for that link for ten minutes.
- **Space (O2).** A start that does not leave 2 GiB free on the store's drive is refused (507);
  Maya's are checked again once their sizes are known; the pull checks while it writes. Pause
  keeps the partial files in the store's owner-only staging; Cancel deletes them (`strata rm
  --partials`).
- **The native prepare step (2026-10-27, owner: "executables to handle what needs to be done so we
  don't have any other dependencies").** Where the server names an engine for new cards and
  `strata-prepare` is installed beside it, a Qwen3.8-family catalog download is packed by that
  program instead of Python.
  - It runs as a child with a fixed argv: `download`, the installed first shard, and the pack
    folder `<the download's store>/packs/<id>`. The pack flags come from the embedded catalog
    (only `--compat-bf16` and `--experts-bin` are accepted). The child gets the same closed
    environment and Job / process group as the other download children.
  - It has no network code. It reads the GGUF files the store verified and writes only inside
    the pack folder (temporary names, then renames).
  - Its card names only paths the server made: the pack, the installed folder, its PLE shard,
    and `engine/profiles/<profile>`.
  - Deleting the model removes that pack folder. The folder is named by the manager from the
    catalog id, never taken from the card.
  - Qwen3.6 needs no prepare step at all: its card is built from the GGUF header.
  - **GLM-5.3 downloads (2026-10-27)** are packed the same way with `download --maya` (Maya's
    step; no pack flag accepted). Maya's engine reads the shards at `<pack>/..`, so the manager
    makes `<store>/packs/<id>/` owner-only, puts **hard links** to the store's installed shards in
    it (same drive; no copy; an existing name that is not the same file is replaced) and the
    pack in `pack/`; the store's version folder is never written. A pack that inherited its ACL
    is made owner-only (`make_tree_owner_only`) before the card names it as `roots.models`. The
    marker file `glm-5.3-flash.gguf` is written into that trusted folder. Deleting the model
    removes `<store>/packs/<id>` - its links, never the store's files (those are `strata rm`'s).
  - **DeepSeek-V4.1 downloads (2026-10-27)** go through the same `download --maya`, which follows
    Maya's DeepSeek step for a `deepseek41` GGUF. Its pack names the model folder by absolute path,
    so the pack is `<store>/packs/<id>` itself and reads the store's version folder in place (no
    links; the version folder is still never written). The only extra argument is
    `--expert-profile`, the bundled `engine/profiles/expert-profile-ds4.bin` beside the engine;
    strata-prepare copies it into the pack as `expert-profile.bin` only when the pack has none and
    the file is the pack's geometry (the engine then saves what it learns there, inside the pack).
- **"Add a model" with a pack (2026-10-27, owner: "Then work on the add model").** For a Qwen3.8
  (`qwen4exp`) or GLM-5.3 GGUF picked in a folder the server trusts (owner-only, as every model
  folder), the card route runs `strata-prepare` (the one beside `models.default_engine`) with a
  fixed argv built from that folder and the file name the route checked (a name, never a path;
  a later shard refused by name), in the downloads' closed environment, before writing the card.
  - Qwen3.8: the pack goes in `<roots.state>/packs/<card name>` (owner-only, the server's own);
    the model folder is only read.
  - GLM-5.3: the pack is written **into the user's model folder**, `pack/` (or `pack-<name>/`
    when `pack/` indexes another model), as Maya's own setup does - the only route that writes
    beside a user's model files. A pack of this model that still matches its shards (Maya's
    `sources.json` rule) is reused unchanged; a new one is made owner-only when it inherited its
    ACL; an existing private pack's permissions are never touched. The marker file is written
    only when it is missing.
  - DeepSeek-V4.1: likewise into the user's model folder, `maya-ds4/` (or `maya-ds4-<name>/` when
    its `sources.json` lists another model's shards), as Maya's setup does, with the bundled
    DeepSeek profile handed over as above.
  - The route waits for the child (a pack takes seconds to minutes); the card routes' lock is
    not held meanwhile, so a second add of the same name is caught again before the card is
    written.
- **The page and `reasoning_effort` (item 46, 2026-10-27).** The launcher's card list now says per
  card whether its profile takes `reasoning_effort` (`per_request_reasoning_effort_for`, the
  schema's own rule); the page leaves the field out for a card that says false. The server's
  schema still refuses the field for glm53; deepseek takes it since entry p (DeepSeek's effort line).
- **The prepare step (D7).** Only for catalog cards upstream packs (never a pasted link): the
  script the server was built with (`tools/secure/prepare_download.py`, written owner-only to
  `<roots.state>/downloads/` at boot) runs under the operator's `models.prepare_python` with
  `-I` and calls upstream's pack tools read-only from `models.prepare_tools`. The card the server
  then writes copies only the engine settings of the operator's `models.download_engine_card`
  and the paths it made; it is read back through the card reader (owner-only, validated with the
  main config) before the launcher adds it.

### GUI-first start-up (2026-10-27)

Spec `docs/superpowers/specs/2026-10-27-gui-first-install-design.md` §3-§6; the owner's
decisions D1 (first login by a one-time link), D2 (the data folder beside the binaries) and the
asks quoted there.

- **First start.** Started without `--config`, the server keeps everything in
  `<install>/data` (config, `secret/token`, state, cards). Each folder is created owner-only and
  nothing existing is overwritten. The install folder itself is made owner-only (Windows: a
  protected DACL for the current user; POSIX: no group or other write) only when it is not
  already, so a copied install never runs an engine others may write.
- **The one-time login link** (`listener.login_link`): the only route served without the key,
  `POST /v1/strata/login-link`, loopback peers only, a 512-byte body. The code is 32 random
  bytes, printed on the console at each start as `http://127.0.0.1:<port>/#login=<code>` (after
  `#`, so a browser never sends it in a request line, Referer or log), valid 10 minutes, used
  once, spent after 10 wrong tries, compared in constant time; a wrong code answers
  `login_link_refused` (403). It hands out the key once, to whoever holds the console's link.
- **Cards the page writes.** A card is built only from a GGUF file's header (flint and Gemma 4,
  the families that run from one GGUF): the page names a file in a folder, never an engine, a
  flag or a free-form argument. The card is written owner-only to `<roots.state>/cards`, then
  read back through the card reader and checked with the main config before the launcher adds
  it. Operator cards (`models.cards`) come first and stay read-only to the page. A page card can
  be replaced or removed only while it is not starting, calibrating or (for a remove) running.
- **Paths from the page** (D4) are accepted only on the keyed control routes, from this PC or (since
  the owner's 2026-10-27 choice) a client in a host or subnet `models.control_remote_networks` lists,
  and only as local folders of the server: valid UTF-8, absolute, no `.`/`..` part; on Windows a drive-letter path on a fixed or
  removable drive (no UNC, device, network, optical or RAM drive). The model folder is then
  opened like any model root (`open_root`: owner-trusted, no link followed). Errors are fixed
  text and never echo a path. Listing shows only sub-folders and `.gguf` files.
- **"Make this folder private"** changes permissions only on a flat model folder (not a drive's
  root, at least one `.gguf`, no sub-folder or link, at most 256 entries): Windows, a protected
  DACL for the user, SYSTEM and Administrators that everything below inherits; POSIX, no group
  or other write. A **new download folder** is owner-only from its creation (Windows: created
  with that protected DACL; POSIX: mode 0700, refused inside a world-writable folder without
  the sticky bit). Its name is 1-64 plain characters.
- **Model folders as they are** (owner opt-in `roots.allow_untrusted_media`, off by default; widened 2026-10-27 at the
  owner's "an option to not change the permissions when browsing and using existing models ... ignore that other users
  can access the files locally"): the main config's `roots.models` and `roots.tokenizer_pack`, every card's, and the
  folders the page names for Add a model, a card's new folder and the Danger zone open with `RootPolicy::external` -
  still local, canonical and held open, links below still refused, but neither the owner nor others' write access is
  checked - and a pack strata-prepare builds beside such files is left as it came out. The page never offers or makes
  a folder private then. Unchanged: the server's own folders (`data/`, the secret folder, state, cards, packs in
  state), the config file and its folder, the install folder, and downloads (still into a private folder). The switch
  (Models > Settings) is saved by `POST /v1/strata/folders/sharing`, keyed, for the clients the file routes admit (this
  PC, and since 2026-10-28 the hosts and subnets `models.control_remote_networks` lists: the page's "make it private"
  question names the switch, and such a client can already add and delete models), and applies at the next start. It
  already relaxed the engine executable's check (`open_executable`, for a build folder others may write) before
  2026-10-27; that is unchanged.
- **Make private with a model's pack** (2026-10-28, the owner's raon1 and Maya folders): the folder may hold the
  folders a model's tools put beside its files (`pack`, `pack-<name>`, `maya-ds4`, `maya-ds4-<name>`, `maya-mm3`,
  `maya-mm3-<name>`, Hugging Face's `.cache`); each is walked without following anything (a link anywhere refuses the folder; at most 4096 entries in
  all) before the whole tree is made owner-only. Any other sub-folder still refuses it, so a general folder (Documents,
  a drive's root) cannot be locked by mistake.
- **The settings from the LAN** (2026-10-28, owner: the email and MCP sections were missing on a page opened from
  another PC): the email alerts and web search settings routes follow the same rule as the file routes -
  `model_files_allowed`: this PC, or a listed host or subnet with `models.control_remote`, never `control_remote`
  alone. A listed client can therefore change where alerts go and which SearXNG answers searches, and set the SMTP
  password (written to the secret folder, never shown back). That password then crosses the LAN in the request body,
  as the API key already does on a plain-HTTP listener.
- **About** (`GET /v1/strata/about`, 2026-10-28): keyed like every API route. It tells a key holder the engine folder's
  build record (`engine/build.json`, read once, schema 1, at most 64 KiB, known fields only, printable text) and this
  PC's CPU, RAM, GPU and operating system - no path, user or host name.
- **Downloads into a chosen folder** keep the downloads rules above. The folder must pass the
  same local-folder and `open_root` checks; the `strata` child gets it as `--store <folder>`
  after `--config` (a fixed argv). The 2 GiB margin is checked on that folder's volume.
- **The danger zone's delete** needs three server-side checks in one request: a one-time code
  the page showed (XXXX-XXXX, 5 minutes, one card, spent by any try, right or wrong), the API
  key typed again (checked by the server's own verifier, so a stolen page session alone cannot
  delete), and `"permanent": true`. Only a page card built from a GGUF file can be deleted,
  never one that is running. Only that model's files go: its GGUF (every shard of a split model,
  each checked to be a regular file, not a link, before the first is removed), then the card, its
  options and its calibration. The folder is removed only when left empty. A downloaded model's
  store entry then goes through the store's own `strata rm <id>:download` child (its lock, its
  index, the version folder inside the store), never a path from the page.
- **Calibration** starts the card's own engine with candidate values of a fixed per-profile
  list of allowlisted flags (`--cpu-lane`, `--pcie-frac`, `--cpu-threads`); nothing comes from
  the page but the card's name and its typed options. It runs on the launcher's single job
  thread, so it is never concurrent with a start or a stop. Results are owner-only files in
  `<roots.state>/calibration`. A result read back is checked value by value against the same
  list before it is composed at a start.
- **Email alerts (built only with `SECURE_FRONTEND_EMAIL_ALERTS`).** The server's only
  outbound connection, to the configured SMTP host and port, made only when
  `alerts.email.enabled`. TLS is mandatory (implicit TLS on 465, or STARTTLS on 587 required
  before AUTH; AUTH is never sent in clear), TLS 1.2 at least, the certificate checked against
  the system's trust store and the host name (Windows: SChannel; elsewhere: OpenSSL). The SMTP
  password is a secret like the API key (`roots.secret/smtp-password`: DPAPI on Windows, 0600
  elsewhere), set on the page, never shown, logged or returned again. Messages are short plain
  text naming the event and the card, never a path, prompt or key. Header values have line
  breaks removed and the body is dot-stuffed. Sending happens on a background queue: at most
  16 waiting, one per kind per minute, no retry. A build without the option refuses every
  `alerts.*` key by name and serves no alert route. The static import check knows the choice: a
  default build imports no resolver at all; an email build imports `getaddrinfo` and nothing else
  on the forbidden list.

### Web search (2026-10-27; built only with `SECURE_FRONTEND_WEB_SEARCH`)

Spec `docs/superpowers/specs/2026-10-27-web-search-design.md`.

- **One tool, the server's own.** The model may call `web_search` and nothing else; the
  tool is offered by the server, never by a request (client-sent `tools` stay refused), a
  model, a manifest or a card. Calls are read only from qwen, glm53, raon1, flint and gemma4
  replies (2026-10-28: raon1 in Qwen's form, flint in GLM's, Gemma 4 in its own
  `<|tool_call>call:NAME{...}<tool_call|>` read by llama.cpp's grammar, each as its own
  template writes it), as upstream, Maya and llama.cpp read them, and only for that tool;
  any other call, and one the grammar refuses, is text. The results and the model's own
  words go back into the prompt as plain spans (their special literals are text, never a
  token), so a page that writes `<|tool_call>` or `<turn|>` cannot open a call or a turn.
- **The MTP draft layer (2026-10-28).** A Qwen3.8 card made by Add a model runs upstream's
  draft layer (`mtp/rt`) only when it passes the model folders' rule (private, unless model
  folders are used as they are); a draft layer other users of the PC could change is left
  out and the log says so.
- **Fewer searches, never more (2026-10-28).** A card's Web search checkbox (`web_search`,
  on by default, shown only while the server's search is on) starts its model without the
  tool, and a request's `"strata_search": false` serves that chat without it. Neither can
  turn search on where the server's settings have it off, offer another tool, or reach
  another program; anything but a boolean is refused by name. Past `search.max_rounds` the
  calls are not run: the model gets a fixed note in their place and one round to answer,
  and a call in that round ends the answer.
- **One program, one destination.** The server runs only the bundled `strata-search`
  (beside its own executable, never a configured command), as a child with the downloads'
  closed environment (system variables and an owner-only temp folder under
  `roots.state`) and the instance's URL and limits as its only arguments. It is the
  install's only search connection: the one origin from `search.url` (http or https; https
  checked as the email client checks TLS), `GET <url>/search?q=..&format=json` only, no
  redirect followed, a 4 MiB reply, `search.timeout_s` per step. The static import check
  knows the split: the server binary keeps its rule, `strata-search` imports `getaddrinfo`
  and no HTTP client library.
- **What leaves the PC.** The model's queries (derived from the conversation) go to the
  SearXNG instance and from it to the search engines it asks. Nothing else is sent: no
  conversation text beyond the query, no key, no path.
- **Results are untrusted text.** Only titles, URLs, dates and snippets reach the model
  (capped; no page bodies), encoded as ordinary text even where they spell a special token,
  so a page cannot open a turn, end a result or forge a call. A steered model can only
  search again (at most `search.max_rounds` rounds) or answer.
- **Settings.** `search.*` rows, refused by name in a build without the option. The page's
  routes (`/v1/strata/search`, `/settings`, `/test`) need the key, model control and a
  loopback client; settings apply at the next start; a test search runs only when the
  operator presses it.

### Extensions and MCP

- MCP, tool execution, arbitrary plugins, and remote command execution are
  disabled and absent from the default build/runtime configuration. The one exception is
  the web search build's own MCP server and tool (above), which no request, model or
  configuration can extend.
- Enabling an extension requires a future, separately reviewed capability model;
  it cannot be enabled through a model manifest, HTTP request, or unrecognized
  configuration key.

## Out of scope and residual risks

- A valid token grants the API permissions implemented by the service. Operators
  must treat it as a password.
- Local users with the same OS account can generally read that account's files
  and are outside this service's isolation boundary.
- **Keyless loopback residual (accepted by the owner, 2026-10-21):** with
  `listener.loopback_without_key`, every program and every user on the machine
  can use the model. Browsers cannot: DNS rebinding fails the Host check, a page
  on another origin fails the Origin check, and a plain form post fails the
  Content-Type check. Use it only on a single-user machine.
- **Model launcher residual (2026-10-23):** a key holder with model control can stop the running
  model, switch models (each switch reloads tens of GB) and pick any option value the card
  allows; a card whose bounds let a context's KV cache exceed the machine's memory can make a
  start fail or slow the box. Mitigations: control is off by default and loopback-only unless
  `models.control_remote`; the operator narrows each card's bounds; one switch at a time. On
  Windows, after a switch the console handler's completion event is already set, so a console
  *close* (not Ctrl+C/Ctrl+Break) no longer waits for the orderly shutdown; the Job still kills
  the engine with the server.
- **Model downloads residual (2026-10-23):** a key holder with downloads can fill the store's
  drive down to the 2 GiB margin and spend the line's bandwidth; a pasted link downloads
  whatever `.gguf` file Hugging Face serves at the confirmed commit (its content is not vetted,
  only fixed by its SHA-256). The prepare step runs upstream's Python tools (with numpy) over a
  pinned catalog file as the server's user; a flaw in them meets only files whose SHA-256
  upstream or Maya pinned. Mitigations: off by default, loopback-only unless
  `models.control_remote`, one download at a time, pasted links are files only.
- **GUI-first residual (2026-10-27):** whoever can read the server's console can sign in once
  per start with the login link (as they could read `secret/token`). A key holder on a control
  route can list this PC's local folders and `.gguf` file names, make a flat model folder
  owner-only, create a private folder, start a calibration (minutes of GPU work) and, by typing
  the key again with a fresh code, delete a page card's model files. Mitigations: control is
  off unless configured (the first start turns it on for the loopback page), loopback-only
  unless `models.control_remote`, and paths are never echoed.
- **Email alerts residual (2026-10-27, email builds only):** alerts reveal to the mail
  provider and the recipients when downloads, calibrations and engine stops happen, and the
  card names involved. A compromised SMTP account can read them. The SMTP password sits beside
  the API key and is as exposed as it is. A mail host the operator mistypes receives the login
  attempt (TLS and host-name checks still apply). The server process now carries TLS and
  resolver code (the owner chose this over a curl child).
- **Web search residual (2026-10-27, search builds only):** the SearXNG instance, and the
  engines it asks, see what the model searches for, which comes from the conversation; an
  http URL to another host exposes the queries on the network (loopback or https is the
  safe choice). A hostile or compromised instance, or a page in its results, can feed the
  model false or steering text (prompt injection); the model has no tool but the search, so
  the effect is a wrong or slanted answer, not an action. A mistyped URL sends the queries
  elsewhere.
- **Model folders as they are residual (2026-10-27, accepted by the owner, opt-in):** with
  `roots.allow_untrusted_media` on, another user of this PC who can write a model folder can
  replace or change its GGUF, tokenizer or pack files, between the page's checks or before a
  start, and the frontend's GGUF reader, the tokenizer loader and the engine then parse those
  files as the server's user; a parser flaw in any of them becomes that user's code. Other users
  can also read the model files. Links below the folder are still refused and the server's own
  folders keep their checks, so the API key, cards and config are not exposed by the switch.
  Mitigations: off by default, set only by the owner (config file, or the switch on this PC's
  page) and in force only after a restart.
- Model outputs can be harmful or unreliable. Prompt injection is not solved by
  transport security; the default build has no tool execution surface.
- GPU drivers, inference libraries, and upstream engine vulnerabilities remain
  supply-chain and patch-management risks. Releases must document versions and
  security updates.
- **Decision 12 residual (accepted, SP3 B12):** loopback peers are exempt from
  the per-address cap; another local user's connection flood can cause
  eviction churn that delays a local client's header by up to its own send
  time, and an authenticated local client can occupy every worker. A token
  holder is trusted with the API's capacity.
- **R4 (SP3):** a client that disconnects during a long real prefill may make
  `STOP` miss `limits.engine.stop_drain_timeout_s`; SP2 then kills the engine
  and counts a failure, so three such disconnects within 300 s can open the
  circuit. Mitigation: SP2 follow-up D-2 (a client-requested `STOP` drain
  failure does not count; bounded at 3 uncounted restarts in a row); GPU-box
  measurement (M2 kit); the operator may raise the drain timeout.
- **R5 (SP3):** SDK retries amplify engine errors; `engine_error` carries
  `x-should-retry: false`, clients that ignore it remain a residual risk.
- **R8 (SP3) — egress checks are tripwires, not proofs:** the import check
  misses `dlopen`/`GetProcAddress` and raw syscalls; the `LD_PRELOAD`
  interposer misses raw `syscall(2)` and static binaries (which the import
  check then fails on); the 50 ms connection-table scan misses sockets that
  live less than 50 ms, and on Windows it is the only runtime check.
  Whole-machine monitoring is the SP7 validation-kit part of `PRIV-01`.

## Release decision

No release may claim the portable frontend is security-hardened unless every
MUST test in `tests/security/README.md` passes on supported Windows and Linux
targets, dependencies have been scanned, and a reviewer has assessed changes
to this document or the security boundary.
