# Secure frontend scaffold

This is a dependency-free C++20 foundation for the portable Strata frontend.
It currently provides configuration validation, secret-source metadata,
protected-file token reading on Linux and Windows, exact Host/Origin/CORS policy checks, a
bounded in-memory HTTP request gate, and a portable engine-process supervisor.
It includes a deliberately limited socket-listener executable for smoke testing
and a native, serial inference listener. It does not connect to the legacy
service, download artifacts, or open outbound sockets (one exception, in a build
with the optional email alerts: the SMTP connection, made only once
`alerts.email.enabled` is set; see "Install" below; a build with the optional web
search runs `strata-search` beside it, which alone connects to the SearXNG
instance, only once `search.enabled` is set).

## Token-source metadata

`SecretSource` represents only an explicit environment-variable name or a
protected-file path. It deliberately cannot contain a resolved API token, so
long-lived configuration never retains plaintext credentials.

`read_token()` resolves a source only when required and returns the token as
`SecretBytes` (wiped on destruction). On Linux it opens the file below the
canonical secret root with `openat2(RESOLVE_BENEATH|RESOLVE_NO_SYMLINKS|RESOLVE_NO_XDEV)`
(or an `openat(O_NOFOLLOW)` walk where `openat2` is unavailable) and accepts
only a current-user-owned regular file with no group/world permissions. On
Windows it opens every component with `READ_CONTROL` without following reparse
points, checks the final path stays beneath the root, and requires a
current-user-owned protected DACL; a `STRATAK1` file is unwrapped with DPAPI,
anything else is read as a plaintext token under the same rules. The token may
end in one LF or CRLF and must be at least 32 bytes. `secure_frontend_token_reader_tests`
now builds and runs on every platform; its Windows file cases carry the
`windows-runtime` label and are validated only by the `windows-msys2-clang` CI
job (CI run `38540719307`: `windows-msys2-clang` 193/193 on SP1, before
the SP2 merge).

## Security contract

- The default bind is the IPv4 loopback literal `127.0.0.1:8080`.
- Every listener requires a bearer token, including on loopback — with one
  owner-approved exception: `listener.loopback_without_key = true` (config file
  only; no flag, no environment variable) runs a loopback listener with no key.
  It is refused for a remote or wildcard bind, at load and again at bind, and
  refused next to a `token.source`; see "Built-in chat page and the keyless
  loopback mode" below. `init`
  generates it from the OS random generator (CSPRNG, base64url, 43
  characters). An operator-made token must be at least 32 bytes (at most 256);
  a token file may end in one LF or CRLF. Tokens are compared in a fixed
  256-byte window, so the comparison time does not reveal the length.
- Ports must be TCP ports in the inclusive range 1–65535.
- The listener address is classified from its bytes: it must be an IPv4/IPv6
  literal or `localhost` (ASCII case-insensitive), which maps to `127.0.0.1`
  without a resolver call. Every other hostname is rejected, never resolved.
  CIDR notation, bracketed IPv6 literals and whitespace are rejected.
- A non-loopback bind additionally requires `listener.allow_remote = true`
  (`--allow-remote`); it defaults to `false` and cannot be enabled implicitly
  by selecting a remote host.
- Wildcard binds (`0.0.0.0`, `::` and every other spelling of the unspecified
  address) additionally require `listener.allow_wildcard_bind = true`
  (`--allow-wildcard-bind`); a regular remote-listening opt-in is not enough.
  A wildcard listener also needs a non-empty `listener.host_aliases` (config
  file): those are the only `Host` values it accepts.
- The whole configuration is schema-validated before the listener binds or the
  engine starts; `--check` opens and inspects but never creates, writes,
  renames or deletes anything. The server binds before it spawns the engine.
- An environment token (`token.source = env:NAME`) is wiped and removed from
  the process environment after it is read (on Windows the CRT copies are
  wiped and the Win32 environment entry is removed). The server is
  non-dumpable with a zero core-file limit on Linux.
- Protected-file token sources are portable relative paths only; traversal,
  absolute/UNC/device/drive/ADS forms and Windows reserved device names are
  rejected before a platform reader resolves the path below its canonical root.
- HTTP messages are bounded, require exactly one trusted `Host`, reject
  untrusted or ambiguous browser `Origin`, keep CORS off by default, and require
  exactly one bearer token before reaching route dispatch.
- Since SP3 the server is concurrent: one accept thread plus
  `limits.concurrency.max_concurrent_requests` workers (default 8). Each
  connection gets a header deadline (`limits.http.header_deadline_ms`, 10 s)
  before authentication completes and a total request deadline
  (`limits.http.request_deadline_ms`, 30 s) with a body cap
  (`limits.http.max_request_bytes`, 64 KiB); chat bodies are capped at 49152
  bytes. Unauthenticated clients can hold a worker only in the header phase
  (oldest-P1 eviction); bodies are read only after the bearer token checks.
  Over-admission (per-remote cap `max_connections_per_remote_address`, derived
  min(4, workers), or min(6, workers) with `listener.web_ui`; loopback exempt) and an
  over-full generation queue
  (`limits.concurrency.max_queued_generations`, derived default 4,
  `queue_timeout_ms` default 300 s) answer one fixed 503 `server_busy` +
  `Retry-After: 1` read through a lingering close. Response writes are bounded
  by the stall rule (`limits.http.write_stall_ms` 10 s measured on progress,
  `limits.http.response_write_deadline_ms` 60 s). `::` binds IPv6-only, and
  the listener stops only when the socket itself fails.
- Engine children are started directly without a shell, with a minimal
  allowlisted environment. Embedded NUL bytes in the executable path, argv, or
  environment are rejected before the platform process API can truncate them.
  On Linux, argv/env are built before `fork`, pipes are `O_CLOEXEC`, every
  descriptor above stdio is closed in the child, the child runs in its own
  process group, and a killed child is reaped. On Windows the child starts
  suspended, is assigned to a Job Object, and only then resumes.
- The engine protocol has a total generation deadline, recovers from a timed-out
  generation with `STOP` and a bounded drain (or marks the engine failed),
  sizes the outbound `GEN` cap from the engine's `READY` context, and reports
  its state through `/health` as `{"status":"ok"|"unavailable","engine":"<state>"}`:
  `starting` → `503` (since SP2 a supervisor thread reads READY as soon as
  the engine starts), `ready`/`generating` → `200`, `failed` → `503` with an
  optional `recovery` of `restarting` or `exhausted`. The supervisor restarts
  a failed engine with backoff and opens a circuit after three consecutive
  failures. The engine
  facts these limits rely on are in
  [`docs/security/engine-protocol-limits.md`](../docs/security/engine-protocol-limits.md).

## Build

No packages are fetched during CMake configuration or testing. On Linux, create
the tool environment once (no root needed). It installs `-r
scripts/dev/requirements-dev.txt` into `.venv-dev`: cmake, ninja, regex, pytest
and pytest's dependencies are pinned there. Offline use (`STRATA_WHEEL_DIR`
set) needs every wheel that file names **plus** a pip wheel (`pip-*.whl`): on a
fresh venv the pip bootstrap takes pip from `STRATA_WHEEL_DIR` as well.

```sh
scripts/dev/bootstrap-linux.sh                # from the repository root
export PATH=$PWD/.venv-dev/bin:$PATH
cd secure_frontend
cmake --preset linux-gcc && cmake --build --preset linux-gcc && ctest --preset linux-gcc
```

`CMakePresets.json` has five presets:

| Preset | Where it runs |
| --- | --- |
| `linux-gcc` | Linux, GCC, Ninja |
| `linux-clang` | Linux, Clang, Ninja |
| `windows-llvm-mingw-cross` | Linux host, cross-compiles Windows x86_64 `.exe` files with llvm-mingw (`scripts/dev/bootstrap-llvm-mingw.sh` fetches the pinned, sha256-checked toolchain); `ctest` runs them under wine |
| `windows-msys2-clang` | Windows, MSYS2 CLANG64 clang |
| `windows-msvc` | Windows, Visual Studio 2022 (informational only) |

Warnings are errors by default (`SECURE_FRONTEND_WERROR`, which follows
`STRATA_WERROR` when the parent project sets it). The top-level Strata build
includes this directory only when configured with
`-DSTRATA_BUILD_SECURE_FRONTEND=ON` (default `OFF`). `ws2_32` is linked
automatically on Windows. See [`docs/secure-frontend-ci.md`](../docs/secure-frontend-ci.md)
for the CI jobs.

Windows binaries are cross-compiled with llvm-mingw and smoke-tested under
wine on Linux. wine does not implement Job Objects or protected-file DACLs
faithfully; Windows runtime behavior is validated only by the `windows-msys2-clang`
CI job on a real Windows runner. The `windows-msvc` job is informational.

Each test case is its own CTest test (`<binary>.<case>`). Cases that need real
Windows semantics (DACLs, junctions, DPAPI, Job Objects, private roots) carry
the label `windows-runtime`: the `windows-llvm-mingw-cross` test preset
excludes them under wine, and `windows-msys2-clang` runs everything. Every test
binary, including `secure_frontend_token_reader_tests`, is built on Windows;
the socket-server tests run on Windows too. `engine_process_tests` and `engine_stdio_process_tests` start and
terminate through the Job on MSYS2; the grandchild assertion
(`job_object_terminates_grandchild`) runs only there (wine passes it but is not
authoritative for Job Objects). Both Windows presets link with `-static`: the
engine launchers give the child only the allowlisted environment, with no
`PATH`, so a dynamically linked MSYS2 binary could not find its runtime DLLs
when it is started as an engine.

**Email alerts are a build option**, `SECURE_FRONTEND_EMAIL_ALERTS`, **off by
default**. A build without it has no SMTP or TLS code, refuses every `alerts.*`
config key by name and serves no alert route. The `linux-gcc`, `linux-clang`
and `windows-msvc` presets (our own builds) turn it on; `make.py --with email`
builds it into an out folder. On Linux it links the system's OpenSSL
(`find_package(OpenSSL)`); on Windows it uses SChannel.

**Web search is a build option**, `SECURE_FRONTEND_WEB_SEARCH`, **off by default**
(spec [`docs/superpowers/specs/2026-10-27-web-search-design.md`](../docs/superpowers/specs/2026-10-27-web-search-design.md)).
With it, a qwen, GLM-5.3, raon1, flint or Gemma 4 model may call one tool, `web_search`, when it is unsure
of a fact or the answer needs recent information: the server runs `strata-search`
(an MCP server on stdio, beside the server) and talks to it with upstream's MCP
client; `strata-search` asks one SearXNG instance's JSON API and the model answers
from the titles, URLs and snippets, with the links. The answer streams across the
search rounds (at most `search.max_rounds`), each search announced as a
`strata_search` chunk. A build without it has no MCP or search code, refuses every
`search.*` key by name and serves no search route. The same three presets turn it
on; `make.py --with search` builds it into an out folder (`strata-search` beside
the server). The instance must answer JSON (SearXNG's `settings.yml`:
`search: formats:` lists `json`).

### Web search from the API

A client of the OpenAI-compatible `POST /v1/chat/completions` gets web search with no change to its requests: the
server offers the tool to the model, runs the searches itself and returns the answer.

**Turn it on, once, on the server.** A build with web search (above), then `search.enabled` on and `search.url` naming
the SearXNG instance: on the page (Settings > Web search) or in `data/config.json`. Saved settings apply at the
next start, so restart the server. "Test search" there shows what a model would read; the server log prints
`[strata] search: strata-search is running` when the first chat starts it.

**What the model is offered.** Every text chat to a qwen (Qwen3.6, Qwen3.8), GLM-5.3, raon1, flint or Gemma 4 model gets one
tool, each in its own template's form (raon1 writes Qwen's calls, flint GLM's, Gemma 4 its own
`<|tool_call>call:web_search{query:<|"|>...<|"|>}<tool_call|>`, read as llama.cpp reads it; the server stops a Gemma 4
reply at the `<|tool_response>` it writes after its calls),
`web_search(query, max_results?)`, described to the model as: "Search the web through a SearXNG instance and get the top
results (title, URL, snippet). Call it when you are not sure of a fact, when the answer depends on recent events or
anything after your training data, or when the user asks you to look something up. Then answer from the results and
cite the URLs you used.", followed by today's date on this PC. The model decides when to call it. A chat with an image,
and the other profiles (DeepSeek, MiniMax), are served without it, and so is a model whose card's **Web search** option
(`web_search`, on by default, listed while the server's search is on) was off when it started.

**Getting the model to search.**

- Ask what needs current or exact information: a version, a date, an event, a lookup ("What is MITRE ATT&CK technique
  T1059.001?", "Which group is ATT&CK G0016?"). A model that is unsure searches on its own.
- Ask it to: "Search the web for ... and cite your sources" in the message, or once for the whole conversation in a
  system message, for example
  `{"role": "system", "content": "Before you answer anything about current events, versions, dates or other facts that change, search the web and cite the URLs you used."}`.
- A broad question can take several searches, at most `search.max_rounds` (8 by default) per answer. A search the model
  asks for past that limit is not run: the model reads "Not run: this answer has made all the searches it may. Answer
  the user now from the results above, without searching again." in its place and gets one more round to answer (upstream
  ends the answer there instead); a search it asks for in that round ends the answer.
- A search that finds nothing says why when SearXNG reports it: "No results: the search engines did not answer (...)",
  naming the engines that refused (CAPTCHAs, rate limits). Turn on more engines in the instance's `settings.yml`.
- `max_tokens` is each round's budget (as upstream's server spends it), reasoning included: a thinking model that
  searches several times can spend a round's tokens reasoning and end with `finish_reason` `length` and no answer. Give
  it room (8192 or more), or leave `max_tokens` out for the server's automatic budget.
- What the model searches for comes from the conversation and goes to the instance (`search.url`), which passes it on
  to the search engines it uses.

**Do not send** `tools`, `tool_choice`, `functions`, `function_call`, `parallel_tool_calls` or `mcp_servers`: the server
refuses them (`extension_disabled`). `"strata_search": false` serves that one request without the tool (any profile;
`true` or leaving it out keeps the server's and the card's choice; anything else is a 400 naming it); it cannot turn
search on. `search.enabled` turns it off for all.

**What comes back.**

- A complete reply (`"stream": false`): `choices[0].message.content` is the answer, `reasoning_content` the reasoning of
  every round, and `message.strata_search` lists what happened in order: `{"event": "call", "round": 1, "name":
  "web_search", "arguments": {"query": "..."}}`, then `{"event": "result", "round": 1, "ok": true, "text": "...",
  "chars": ..., "truncated": false, "ms": ...}` (`text` is what the model read), and past the limit `{"event": "limit",
  "max_rounds": 8}` with the calls not run as results with `"skipped": true`. `usage` counts the last round's prompt
  and every round's completion tokens.
- A streamed reply: the reasoning and the answer stream as usual. Each search event is a chunk with an empty `delta` and
  a `strata_search` object, so a client that ignores unknown fields shows only the answer.
  `"stream_options": {"include_usage": true}` adds the usage chunk at the end.

```sh
curl -s http://127.0.0.1:8080/v1/chat/completions \
  -H "Authorization: Bearer $STRATA_API_KEY" -H "Content-Type: application/json" \
  -d '{"model": "Qwen3.6-35B-A3B-Q8_0", "max_tokens": 8192,
       "messages": [{"role": "user", "content": "What is MITRE ATT&CK technique T1059.001, and which tactic does it belong to?"}]}'
```

`model` is the model card's name. A search usually adds a few seconds; an answer that needs several searches and long
reasoning can take a minute or two, so give the client a long enough read timeout.

## Install: one folder, then the page (GUI-first start-up, 2026-10-27)

Spec: [`docs/superpowers/specs/2026-10-27-gui-first-install-design.md`](../docs/superpowers/specs/2026-10-27-gui-first-install-design.md).

**Build an out folder** from the repository root. It holds only what runs, so it can be copied anywhere:

```sh
make.bat --out D:\SecureStrata --ggml-dir C:\src\llama.cpp --with email,search     # Windows
./make.sh --out /opt/secure-strata --ggml-dir ~/llama.cpp --with email,search      # Linux
```

`--with email,search` adds email alerts and web search (each is off otherwise). `--without maya,raon1` leaves those
engine families out (strata, Gemma 4 and flint are always built). `--no-cuda-runtime` skips bundling cuBLAS; `--jobs N`
sets the compile jobs; `--plan` prints the commands only. The folder gets `strata-secure-server`, `strata`,
`strata-search` (with `search`), `engine/` (the `multiengine` launcher, one library per family, `strata-prepare`, the
expert profiles, MiniMax's template in `templates/minimax-m3.1/` and, by default, NVIDIA's cuBLAS), `README.txt`,
`config.example.json`, `LICENSE` and `NOTICE.md`, and it is made owner-only.

**First start.** Run `strata-secure-server` with no arguments. It creates `data/` beside itself (config, API key,
cards, state, owner-only), serves `http://127.0.0.1:8080`, and prints a one-time login link
(`http://127.0.0.1:8080/#login=...`, valid 10 minutes, once, from this PC only). Opening the link shows the API key
once and signs the page in. Every start prints a fresh link. `--data-dir DIR` keeps the data elsewhere (relative to
the current folder), and `--config FILE` runs from a JSON config as before (`data/config.json` is one; every key is in
the table below); the two exclude each other. Other options (`--port 9090`, `--host`, `--token-env`, ...) apply to
that start on top of the data folder's config (`uses_data_folder`); only a bring-up start that names its model by
flags (`--expert`, `--models-root`, `--engine`, `--model-id`, `--profile`, `--tokenizer-pack`) reads no data folder.

**The Models tab** (model control on, loopback clients only):

- **Add a model** from files already on this PC: pick a drive and folder (free space shown), then a GGUF file (a split
  model: its first file). Gemma 4, flint and Qwen3.6 GGUFs become cards; the tokenizer is read from the GGUF. A
  Qwen3.8 or GLM-5.3 GGUF has its pack built first by `strata-prepare` (beside the engine; seconds to minutes): a
  Qwen3.8 pack in the server's `data/state/packs/`, the model folder left as it is (and when the model folder, its parent
  or its grandparent holds upstream's MTP draft layer, `mtp/rt` with `dense.bin`, `experts.bin` and `draft_vocab.bin`,
  private by the folders' rule, the card runs it as upstream's setup does, `--mtp <rt>` with `--spec 4 --spec-min-p 0.5`,
  and lists `mtp`, `spec` and `spec_min_p`; GLM-5.3's Maya engine drafts with its pack's MTP block by itself, with no
  setting); a GLM-5.3 pack in `pack/` beside
  the shards, where Maya's engine reads them (as Maya's own setup puts it; an up-to-date pack there is reused); a
  DeepSeek-V4.1 pack in `maya-ds4/` beside the shards, Maya's shipped DeepSeek profile seeded into it; a MiniMax-M3.1
  pack in `maya-mm3/` with Maya's chat template; a Raon-1-mini pack in `pack/` beside its GGUF, as GLM-5.3's. A
  folder other users may write is offered "Make this folder private" first, unless Settings > Model folders has "Use
  model folders as they are" on (`roots.allow_untrusted_media`): then every model folder, and a pack built beside its
  files, is used as it is and its permissions are never changed.
- **Download** into a drive or folder you choose. Its free space must cover the files plus a 2 GiB margin, and "New
  private folder" makes an owner-only folder on a shared drive. A downloaded Gemma 4, flint or Qwen3.6 GGUF becomes a
  card when it is verified; a Qwen3.8, GLM-5.3, DeepSeek-V4.1 or MiniMax-M3.1 download is packed by `strata-prepare`
  first (a GLM pack beside hard links to the store's shards in the store's `packs/`, a DeepSeek or MiniMax pack there
  reading the store's files in place). A model that has a card no longer shows in the Download list.
- **GLM-5.3 cards** take Maya's options (context 8K-128K, the KV format, sampling) and Maya's shipped expert profile;
  the page sends no `reasoning_effort` to them (the profile has no thinking switch).
- **DeepSeek-V4.1 cards** run Maya's DeepSeek engine (`multiengine --pack`) with Maya's options (context 32K-1M, 64K;
  thinking none/low/medium/high, high; temperature 1.0, top_p 1.0) and Maya's one-GPU launch (`--tp 1 --threads on`).
  The prompt is DeepSeek's own encoder's; a request's `reasoning_effort` sets its effort line (low 50, medium and high
  75), `none` is chat mode.
- Each page card shows its **folder**. After moving the files, change it there and the card follows at the next start.
- **Calibrate** sweeps the engine settings that depend on this PC (flint: `--cpu-lane`, `--pcie-frac`,
  `--cpu-threads`; Gemma 4: `--cpu-threads`). Each candidate is one engine start, scored by the median decode speed;
  a setting is kept only when more than 3% faster. The result applies at each start until the GPU, CPU, RAM, engine,
  model file or the card's settings change. A card that was never calibrated calibrates on its first start; "Recalibrate
  when settings change" does the same after a settings change.
- **Danger zone**, for a page card that is not running: a one-time code to type back, the API key again, then a final
  confirmation that the delete is permanent. The server checks all three and deletes only that model's files (and a
  download's store entry), the card and its calibration.

**The Settings tab** (every build with model control; 2026-10-28 a tab of its own, it was a sub-tab of Models) holds
the API key, the server's own settings, and the sections below. **API key:** the key the page signed in with (the page
holds it; no route returns a key), Show and Copy; "Rotate the key", typed ROTATE first and from this PC only
(`POST /v1/strata/key/rotate`, `{"confirm":"ROTATE"}`), makes a new key in the key file as `strata key rotate` does and
answers it once; the running server keeps the old key until it restarts. A `--token-env` start has no key file, so
rotation is refused there. **Server and network**, **Limits and timeouts** and **Folders** list 14 config rows
(`listener.host`, `.port`, `.allow_remote`, `.allow_wildcard_bind`, `.host_aliases`, `models.control_remote`,
`.control_remote_networks`, `limits.engine.generation_timeout_s`, `.ready_timeout_s`, `limits.chat.max_prompt_tokens`,
`.max_completion_tokens`, `pull.connections`, `models.downloads_root`, `.default_root`) from `POST /v1/strata/config`
(each with its type, value, default, bounds and the schema's description) and save them through
`POST /v1/strata/config/settings` (`{"values":{key: value or null}}`; null or an empty text is the default). A save is
held to the server's start-up rules on the whole file: a problem the change brings is refused by name and nothing is
written. Saved settings apply at the next start; a command-line option (`--port`, `--host`, ...) still overrides the
file for the start that names it.

**Model folders** (every build with model control): "Use model folders as they are, even if other
users of this PC can access them" saves `roots.allow_untrusted_media` to the config file; it applies at the next start.
Other users of this PC can then read or change those model files, and the engine reads them as you; the server's own
folders (`data/`, the API key, cards, state) keep their checks, and a download still goes into a private folder.
The Settings tab's sections (all but the key's rotation) answer the clients the model-file routes
serve: a browser on this PC, and the hosts and subnets `models.control_remote_networks` lists; from any other PC they
do not show.

**Email alerts** (builds with email alerts only): the SMTP server, port and security (SSL/TLS on 465 or STARTTLS
on 587; TLS 1.2 or later, certificate checked, never plain text), the user name, From and To (1-5 addresses), and
which alerts to send (downloads finished or failed, calibrations finished or failed, the engine stopped unexpectedly,
the server started). "IONOS" fills in `smtp.ionos.com`, 465 SSL/TLS and the mailbox address as the user name. The
password is saved on its own into the secret folder (DPAPI on Windows, 0600 elsewhere) and never shown again. "Send a
test email" reports the mail server's answer. Saved settings are written to the config file and apply at the next start.
In a build with web search the tab also holds **Web search**: on/off, the SearXNG address, results per search,
searches per answer and the time limit, Save, and a test search through the saved settings that shows what a model
reads. A chat reply notes what it searched for.

## Runnable smoke server

`secure-frontend-smoke` is a local, privacy-preserving test server. It serves
authenticated `GET /health` and `GET /v1/models` only; it does not start the
inference engine and intentionally does not implement chat completion yet.

```sh
export STRATA_SMOKE_API_KEY='choose-a-long-random-value-of-32-bytes-or-more'
./secure-frontend-smoke --api-key-env STRATA_SMOKE_API_KEY --models-root /path/to/local/models
curl -H "Authorization: Bearer $STRATA_SMOKE_API_KEY" http://127.0.0.1:8080/health
curl -H "Authorization: Bearer $STRATA_SMOKE_API_KEY" http://127.0.0.1:8080/v1/models
```

## Run the secure smoke server

`strata-secure-smoke-server` is an authenticated, offline-only smoke-test
server. It inventories pre-existing local `.gguf` files and exposes only
`GET /health` and `GET /v1/models`. It does **not** perform inference; chat
routes deliberately return `404` until the engine integration is complete.

Build it with the commands above, then set a token only in the process
environment and run it on loopback:

```sh
export STRATA_SMOKE_TOKEN='replace-with-a-long-random-token'
./out/build/linux-gcc/strata-secure-smoke-server \
  --models-root /absolute/path/to/local/models \
  --token-env STRATA_SMOKE_TOKEN
```

In another terminal, use the exact configured `Host` value and bearer token:

```sh
curl --fail --header 'Host: 127.0.0.1:8080' \
  --header "Authorization: Bearer $STRATA_SMOKE_TOKEN" \
  http://127.0.0.1:8080/health
curl --fail --header 'Host: 127.0.0.1:8080' \
  --header "Authorization: Bearer $STRATA_SMOKE_TOKEN" \
  http://127.0.0.1:8080/v1/models
```

For Linux protected-file tokens, create an owner-only file below a dedicated
secret root and pass `--token-file token.txt --secret-root /path/to/root`.
The relative token-file name is validated before the secure reader opens it.
Windows protected-file tokens require an explicitly protected owner-only DACL;
the implementation is fail-closed, and its runtime tests (`windows-runtime`)
passed in the `windows-msys2-clang` CI job (run `38540719307`). Tokens shorter than 32 bytes are
rejected.

Remote/wildcard binds are rejected unless the corresponding explicit flags are
provided. CORS remains disabled, and the executable neither sends outbound
traffic nor starts MCP, telemetry, analytics, update checks, or downloads
(downloads run as `strata` children; a build with email alerts sends mail to the
configured SMTP server only when they are enabled; a build with web search runs its
own `strata-search` for it, the one MCP server and tool there is).

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
        --models-root ~/models --engine ~/strata/strata --model-id NAME:TAG
    strata-secure-server --check --config ~/.config/strata/config.json
    strata-secure-server --config ~/.config/strata/config.json

`engine.model_id` names a model installed in `roots.models` (`strata pull` or
`strata import`; `strata list` shows them). Its manifest supplies the engine
profile, `--native`, `--pack`, `--ple-gguf` and the tokenizer, so
`engine.profile`, `roots.tokenizer_pack` and those weight flags in
`engine.args` are configuration errors that `--check` and serve both report. A
model this release cannot run stops serve with exit 11. `--expert` (below)
is the operator bring-up path instead.

`init` writes the config (owner-only, atomic). For a file token source
(`file:…`, the default) it also writes a 43-character token generated from the
OS random generator (`0600` on Linux; DPAPI-wrapped with a private ACL on
Windows); for an `env:NAME` source it writes no token, and you set that variable
yourself (at least 32 bytes). It refuses to replace an existing file without
`--force`. Where the files live by
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
| `listener.cors_origins` | string array | `[]` | - | - | Exact browser origins allowed CORS: http:// or https://, a lower-case host, an optional port, no path; never * or null; at most 16. |
| `listener.host_aliases` | string array | `[]` | - | - | Extra exact Host values (host or host:port) clients use; required when the bind is a wildcard; at most 16. |
| `listener.web_ui` | boolean | `false` | - | - | Serve the built-in chat page at / (bundled files only). The page asks for the API key and keeps it in that tab; every API route still needs the key. |
| `listener.loopback_without_key` | boolean | `false` | - | - | Answer the API and the page without the API key: only with a loopback listener.host, and with no token.source; Host and Origin checks still apply. |
| `listener.login_link` | boolean | `false` | - | - | Print a one-time login link at every start (GUI-first start-up): opened from this PC (loopback only, within 10 minutes, once), it hands the page the API key. Needs listener.web_ui and a token.source. |
| `listener.monitor` | boolean | `false` | - | - | Serve GET /metrics (with the key) for the page's Monitor tab: the engine's state, the last 100 requests' token counts and timings (never their text), and this machine's CPU, RAM and GPU readings (the GPU through the system's nvidia-smi, a separate process). |
| `limits.http.request_deadline_ms` | integer | `30000` | 1-3600000 | - | Total time to read one request, header and body. |
| `limits.http.max_request_bytes` | integer | `65536` | 4096-268435456 | - | Largest request (header and body) the listener and parser accept. |
| `limits.http.max_header_bytes` | integer | `16384` | 1024-65536 | - | Largest header block; at most limits.http.max_request_bytes. |
| `limits.http.max_headers` | integer | `64` | 8-256 | - | Most header fields. |
| `limits.http.max_request_line_bytes` | integer | `4096` | 256-16384 | - | Longest request line; at most limits.http.max_header_bytes. |
| `limits.http.header_deadline_ms` | integer | `10000` | 1-3600000 | - | Time from accept to the end of the request header; at most limits.http.request_deadline_ms. |
| `limits.http.write_stall_ms` | integer | `10000` | 100-300000 | - | Longest send without progress before the client is treated as gone. |
| `limits.http.response_write_deadline_ms` | integer | `60000` | 1000-3600000 | - | Time to send a whole complete response, or a stream's tail after DONE; at least limits.http.write_stall_ms. |
| `limits.http.stream_keepalive_ms` | integer | `15000` | 1000-300000 | - | SSE comment interval while no content flows (streaming). |
| `limits.chat.max_body_bytes` | integer | `49152` | 1-268435456 | - | Largest chat request body; at most max_request_bytes minus max_header_bytes. |
| `limits.chat.max_messages` | integer | `128` | 1-65536 | - | Most messages in one chat request. |
| `limits.chat.max_message_bytes` | integer | `65536` | 1-268435456 | - | Largest single message content. |
| `limits.chat.max_prompt_tokens` | integer | `32768` | 1-1048576 | - | Largest tokenized prompt sent to the engine. |
| `limits.chat.max_completion_tokens` | integer | `4096` | 1-1048576 | - | Largest max_tokens a request may ask for. |
| `limits.chat.default_max_tokens` | integer | `0` | 0-1048576 | - | max_tokens when a request omits it; 0 = auto: min(max_completion_tokens, context - prompt - 8); at most limits.chat.max_completion_tokens. |
| `limits.chat.max_generated_bytes` | integer | `1048576` | 1-16777216 | - | Largest generated text returned. |
| `limits.engine.ready_timeout_s` | integer | `900` | 1-86400 | - | How long the supervisor waits for the engine's READY after each start; a miss kills and restarts it (upstream STRATA_ENGINE_READY_S). |
| `limits.engine.generation_timeout_s` | integer | `120` | 1-86400 | - | Total time for one generation before STOP. |
| `limits.engine.stop_drain_timeout_s` | integer | `5` | 1-600 | - | Time to drain to DONE after STOP before the engine is marked failed. |
| `limits.concurrency.max_concurrent_requests` | integer | `8` | 1-256 | - | Worker threads: connections read or answered at once; 1 = serial. |
| `limits.concurrency.max_queued_generations` | integer | `derived: min(4, max(0, workers-2))` | 0-254 | - | Chats waiting behind the running generation; 0 when max_concurrent_requests is 1, otherwise at most max_concurrent_requests - 2. |
| `limits.concurrency.queue_timeout_ms` | integer | `300000` | 1000-3600000 | - | Longest wait for the engine before 503 server_busy. |
| `limits.concurrency.max_connections_per_remote_address` | integer | `derived: min(4, workers); min(6, workers) with listener.web_ui` | 1-256 | - | Workers one non-loopback address may hold; at most max_concurrent_requests. Loopback is exempt. |
| `roots.models` | string | required | - | `--models-root` | Absolute directory of .gguf files. The root itself may be a symlink; nothing below it may be. |
| `roots.tokenizer_pack` | string | unset | - | `--tokenizer-pack` | Absolute tokenizer directory; only with strata-secure-server --expert (a managed model brings its own). |
| `roots.secret` | string | unset | - | `--secret-root` | Absolute directory holding the token file. Default: the config file's directory. |
| `roots.logs` | string | unset | - | - | Absolute log directory (created 0700; unused in SP1). |
| `roots.cache` | string | unset | - | - | Absolute cache directory (created 0700); when set, it holds the engine's CUDA cache in its cuda subdirectory. |
| `roots.allow_untrusted_media` | boolean | `false` | - | `--allow-untrusted-media` | Use model folders as they are, even where other users of this PC can read or change them (shared folders, exFAT/FAT media whose permissions the OS ignores): roots.models and roots.tokenizer_pack open with RootPolicy::external (no owner-only / not-shared-writable check), and the page adds, moves and packs models in such folders without changing their permissions (the Settings tab's Model folders switch). Owner opt-in; links below the root are still refused; the server's own folders keep their checks. |
| `roots.state` | string | unset | - | `--state-dir` | Absolute, existing owner-only directory for engine state: the engine's cwd, per-generation temp and (without roots.cache) the CUDA cache. Required to serve. |
| `token.source` | string | `file:token` | - | `--token-file, --token-env` | file:<relative path below roots.secret> or env:<NAME>. Metadata only; the token is never in the config. |
| `engine.profile` | string | unset | - | `--profile` | qwen, glm53 or gemma4; only with --expert (a managed model's manifest sets it). |
| `engine.executable` | string | required | - | `--engine` | Absolute path of the engine; a regular file not writable by group or others. |
| `engine.model_id` | string | required | - | `--model-id` | Default model reference name[:tag] from the model index (or, with --expert, an inventory ID). |
| `engine.args` | string array | `[]` | - | `--engine-arg (repeatable)` | Extra engine arguments; frontend-owned and unsupported flags are rejected. |
| `engine.lib_dirs` | string array | `[]` | - | `--engine-lib-dir (repeatable)` | Absolute library directories for the engine (default: the engine executable's directory). |
| `engine.gpu` | string | unset | - | `--gpu` | GPU device indices for the engine, e.g. 0 or 0,1 (distinct, 0-255, at most 16); rendered as the engine's --gpu argument (strata family: no GPU variable). Not supported with engine.profile glm53. |
| `engine.allow_new_privileges` | boolean | `false` | - | `--allow-new-privileges` | Linux: do not set PR_SET_NO_NEW_PRIVS on the engine (default: set). Only for hosts whose NVIDIA driver must run setuid nvidia-modprobe to create /dev/nvidia-uvm on first use. |
| `pull.downloader` | string | unset | - | - | Absolute path of curl for pull/update. Default /usr/bin/curl (Windows: System32\curl.exe). Validated when a transfer or doctor --downloader runs, never at serve start. |
| `pull.proxy` | string | unset | - | - | http://host:port or https://host:port; absent = --noproxy *. No credentials. Environment proxies are never read. |
| `pull.allow_private_addresses` | boolean | `false` | - | - | Allow sources that resolve to private or special-use addresses. |
| `pull.max_total_bytes` | integer | `1099511627776` | 1048576-17592186044416 | - | Largest manifest total a pull accepts (bytes). |
| `pull.max_duration_s` | integer | `172800` | 60-604800 | - | Time budget for a whole pull run. |
| `pull.stall_timeout_s` | integer | `120` | 10-3600 | - | Abort a transfer slower than 1024 B/s for this long. |
| `pull.connect_timeout_s` | integer | `30` | 1-300 | - | TCP/TLS connect timeout per request. |
| `pull.connections` | integer | `16` | 1-32 | - | Connections for a large download (parallel ranged, 32 MiB pieces; 1 = one stream). Hugging Face's CDN limits each connection, so several are much faster. |
| `update.source` | string array | `[]` | - | - | HTTPS release-mirror URLs that 'strata update' fetches the signed release envelope from. |
| `logging.requests` | boolean | `false` | - | - | One line per request to stderr: route, status, code, time, bytes, loopback or remote; never paths, headers, tokens or text. |
| `defaults.sampling.temperature` | number | unset | 0-2 | - | Request default temperature (pin: 0..2); unset keeps the engine default. |
| `defaults.sampling.top_p` | number | unset | >0-1 | - | Request default top_p (pin: 0 < top_p <= 1); unset keeps the engine default. |
| `defaults.sampling.top_k` | integer | unset | 1-64 | - | Request default top_k candidates (pin: an integer 1..64); unset keeps the engine default. |
| `defaults.sampling.min_p` | number | unset | 0-1 | - | Request default min_p (pin wire: only 0 < min_p <= 1 is sent; 0 stays off). |
| `defaults.sampling.presence_penalty` | number | unset | 0-2 | - | Request default presence penalty (0 off; cap 2 added, C-1). |
| `defaults.sampling.repetition_penalty` | number | unset | >0-2 | - | Request default repetition penalty (pin wire: only != 1.0 is sent; cap 2 added, C-1). |
| `defaults.sampling.frequency_penalty` | number | unset | 0-2 | - | Request default frequency penalty (0 off; cap 2 added, C-1). |
| `defaults.sampling.penalty_last_n` | integer | unset | 0-8192 | - | Penalty window; unset means the pin default of 64 when any penalty is on (cap added, C-1). |
| `defaults.sampling.seed` | integer | unset | 1-4294967295 | - | Request default sampling seed (pin: a positive integer; 32-bit cap added, C-1). |
| `reasoning.effort` | string | unset | - | - | Thinking level for requests that name none: none, low, medium or high (pin levels); unset is the template's default, high. Qwen packs with <think>: none closes the think block; low and high write the template's effort sentence, medium none. |
| `reasoning.budget_tokens` | integer | `0` | 0-4294967295 | - | Default thinking budget in tokens (0 = none, the pin default); bounds added, C-1. |
| `reasoning.close_retry` | boolean | `false` | - | - | Close an unfinished thinking block once when the reply ends inside it (pin opt-in, off). |
| `reasoning.loop_recovery` | string | `off` | - | - | A reasoning loop ends the reply (off, the pin default), stops (stop) or continues once (recover). |
| `reasoning.effort_position` | string | `start` | - | - | Where a non-default effort sits in the prompt: start (the pin default) or end; end adds the engine's --tail-role-token. |
| `reasoning.anthropic_thinking` | string | `model` | - | - | Thinking default for /v1/messages-shaped clients: model (the pin default) or on_request. |
| `engine.fit_max_tokens` | boolean | `true` | - | - | Fit an over-room max_tokens to the context room (true, the pin default); false answers the §7.2 400 instead. |
| `engine.lazy_load` | boolean | `false` | - | - | Start with the engine unloaded; the first request loads it (the pin's lazy_load, text only). |
| `engine.idle_unload_s` | integer | `0` | 0-86400 | - | Seconds without a request before the engine is unloaded (0: never, the pin default); cap added, C-1. |
| `engine.mtp` | boolean | `true` | - | - | Draft layer (MTP): on unless explicitly false; false drops --mtp/--spec-min-p/--batch-mtp at every start. |
| `engine.vram_reserve_mib` | integer | unset | 0-1048576 | - | VRAM MiB the engine leaves free (renders --vram-reserve-mib; unset: the engine's own 700). |
| `engine.min_free_vram_mib` | integer | `0` | 0-1048576 | - | Refuse to load when free VRAM is below this (0: never; the driver check runs on the owner's GPU box). |
| `engine.vram_elastic` | boolean | `false` | - | - | Segment the expert cache so VRAM can be given back and taken back (renders --vram-elastic, #533). |
| `engine.vram_segment_mib` | integer | unset | 1-1048576 | - | Segment size for vram_elastic (renders --vram-segment-mib; unset: the engine's own 512). |
| `engine.elastic` | boolean | `false` | - | - | The image encoder starts with the first picture and stops when idle (needs vram_elastic; the vision turn). |
| `engine.elastic_idle_s` | integer | `120` | 1-86400 | - | Seconds without a picture before the elastic encoder stops (pin default 120); whole seconds, cap added. |
| `engine.elastic_mib` | integer | `2048` | 1-1048576 | - | VRAM MiB the expert cache keeps free while the encoder runs (pin default 2048). |
| `engine.gpu_order` | string | `auto` | - | - | GPU order: auto (reorder by card speed, where the driver can tell) or as_given (keep the configured order). |
| `engine.gpu_policy` | string | `auto` | - | - | GPU turn for a picture: auto, keep, shrink or unload the engine's VRAM first (the POLICIES set). |
| `engine.layer_split` | string | `auto` | - | - | First layer of each later GPU (rising numbers from 2), or auto; renders --layer-split with several GPUs. |
| `engine.split_skip_if_fits` | boolean | `false` | - | - | Let an auto split run on the first card alone when it holds every expert and the KV (renders --split-skip-if-fits). |
| `engine.hip_ordinal` | integer | unset | 0-127 | - | The HIP number of the discrete card (setup-recorded); wins over one (or zero) configured GPUs. |
| `engine.uma` | boolean | `false` | - | - | An APU: engine and image turn share the RAM either way, so a picture never needs the engine moved. |
| `engine.silence_s` | integer | `300` | 0-86400 | - | End a request when the engine says nothing valid for this long (pin default 300 s; 0: wait, still bounded by the total generation timeout). |
| `engine.answer_room` | boolean | `true` | - | - | Close a long un-budgeted thinking reply early enough to leave the answer room (true, the pin default). |
| `engine.repeat_stop_tokens` | integer | `256` | 0-65536 | - | One token this many times in a row ends a reply as length (pin default 256; 0: off). Cap added, C-1. |
| `engine.max_tokens` | integer | unset | 1-1048576 | - | Ceiling on a request's output budget (unset: the completion cap; an added frontend bound, C-1). |
| `engine.min_tokens` | integer | unset | 1-1048576 | - | Floor on a request's output budget (unset: none; an added frontend bound, C-1). |
| `model.aliases` | string array | `[]` | - | - | Other model names the server lists and answers to (exact match only, like the pin's set_aliases). |
| `model.name` | string | unset | - | - | The name the server serves the model as: /v1/models lists it, the chat page shows it, requests name it (like the pin's model_name). Unset: the model's own id (its inventory file name, or name:tag). |
| `session.slot_save_path` | string | unset | - | - | Directory for engine slot session save/restore files (absolute; the /slots route is not served today). |
| `session.expert_profile_save` | string | unset | - | - | Where the engine re-saves its learned expert profile (absolute path; --expert-profile-save). |
| `session.expert_profile_save_every` | integer | `10` | 0-1440 | - | Re-save the learned expert profile every N minutes (pin default 10; 0: at engine exit only). Cap added, C-1. |
| `session.tool_call_recovery` | boolean | `false` | - | - | Attempt to recover malformed tool-call output (the pin's opt-in; stored for parity today). |
| `session.env_to_user` | boolean | `false` | - | - | Pass configured environment hints to the user prompt path (the pin's opt-in; stored for parity today). |
| `session.codex_thread_titles` | boolean | `false` | - | - | Codex thread titles (#923; the pin's opt-in; the route is not served here). |
| `session.codex_compaction_cache` | boolean | `false` | - | - | Codex compaction cache (#924; the pin's opt-in; the route is not served here). |
| `session.keep_files` | integer | `64` | 1-1024 | - | How many image-scratch files to retain (pin default 64; bounds added, C-1; the consumer is out per D-9). |
| `replicas.parallel` | integer | `1` | 1-1024 | - | Decode up to N requests together inside the one engine (1: one at a time; the N-child scheduler is not implemented). |
| `vision.enabled` | boolean | `false` | - | - | Read images: a supervised maya-vision child encodes image_url parts for the engine. |
| `vision.max_side` | integer | `2048` | 1-16384 | - | Largest image side accepted before decoding (pin default 2048; cap added, C-1). |
| `vision.max_n` | integer | `4` | 1-64 | - | Most images one request may carry (pin default 4; cap added, C-1). |
| `vision.need_mib` | integer | unset | 1-1048576 | - | Override the free-VRAM figure the encoder is checked against (unset: computed; bounds added, C-1). |
| `vision.no_flash_attn` | boolean | `false` | - | - | Run the encoder without flash attention (added, spec §5.1; the encoder's --no-flash-attn). |
| `vision.resident` | boolean | `true` | - | - | Keep the encoder running from boot (the pin's eager resident encoder; false: the first image starts it). |
| `vision.threads` | integer | `0` | 0-256 | - | Encoder CPU threads (0: the tool's own default; cap added, C-1). |
| `vision.ready_timeout_s` | integer | `300` | 0-86400 | - | Seconds the encoder gets to print READY (0: wait for ever, as at the pin). |
| `vision.encode_timeout_s` | integer | `300` | 0-86400 | - | Seconds one image encoding may take (0: wait for ever, as at the pin). |
| `vision.mmproj` | string | unset | - | - | Absolute path of the picture encoder (an mmproj GGUF) a model card's engine reads images with; a card row. |
| `vision.gpu` | boolean | `false` | - | - | Encode pictures on the GPU (the encoder's --gpu; off: the CPU); a card row. |
| `glm.thinking_budget` | integer | `32768` | 0-1000000000 | - | Thinking-token budget when a request names none (Maya's default 32768; 0 disables the budget). |
| `glm.reasoning_effort` | string | unset | - | - | Thinking level when a request names none: none, low, high or max (Maya's canonical levels; Maya's aliases such as "medium" are not accepted here). |
| `glm.fit_max_tokens` | boolean | `false` | - | - | Fit a request's output budget to the context room on the GLM side (Maya's default: false). |
| `glm.experimental_speed_projection` | boolean | `false` | - | - | GLM speed projection (Maya's default: false). |
| `glm.gpu` | string | unset | - | - | GPU device indices for the GLM engine, e.g. 0 or 0,1 (distinct, 0-255, at most 16). |
| `glm.layer_split` | string | `auto` | - | - | First layer of each later GPU's share (rising numbers, e.g. 18 or 16,32), or auto; the GLM engine's --layer-split. |
| `glm.tokenizer` | string | unset | - | - | Absolute tokenizer-pack path for the GLM engine (default: the pack named by the model). |
| `glm.model_name` | string | unset | - | - | Served model id for the GLM engine (1-256 characters; a managed model takes its identity from the manifest). |
| `glm.max_context` | integer | `32768` | 1-1048576 | - | GLM context length in tokens the engine is started with (Maya's default 32768). |
| `glm.kv` | string | `fp16` | - | - | KV cache precision the GLM engine starts with: fp16 (default; raon1 keeps its exact F32 cache), int8, or q8_0 / q4_0 (raon1's quantized cache). |
| `glm.ram_headroom_gb` | integer | `16` | 6-1024 | - | Gigabytes of RAM the GLM engine leaves free when sizing its pinned tier (6-1024; default 16). |
| `glm.idle_unload_s` | integer | `0` | 0-86400 | - | Seconds a GLM engine idles unloaded before it gives back the memory (0 = never; under glm53 this wins over engine.idle_unload_s). |
| `models.cards` | string array | `[]` | - | - | Absolute paths of model card files (at most 16): the page's Models tab starts and stops them. Each card holds its model's engine.*, model.aliases, roots.models, roots.tokenizer_pack, defaults.sampling.*, reasoning.* and limits.chat token rows, which the main config then may not set. |
| `models.start` | string | unset | - | - | The card (by name) started at boot; unset: the server boots with no engine and waits for the page. Needs models.cards. |
| `models.control` | boolean | `false` | - | - | The page and API may start and stop model cards (keyed; loopback clients only unless models.control_remote). Needs models.cards. |
| `models.control_remote` | boolean | `false` | - | - | Model control also from non-loopback clients. Needs models.control. |
| `models.control_remote_networks` | string array | `[]` | - | - | With models.control_remote: the hosts (an IP address) and subnets (CIDR, e.g. 172.16.1.0/24) other machines may control models from; empty: every client with the key. Listed clients may also use the page's model-file actions (this PC's folders, add a model, a card's folder, delete), which otherwise stay this-PC-only. IPv4 or IPv6, never a name; at most 16. |
| `models.downloads` | boolean | `false` | - | - | The Models tab's downloads: Hugging Face .gguf files only, fetched by strata pull as a separate confined process (the server itself has no network code). Keyed; loopback clients only unless models.control_remote. Needs models.cards and models.downloads_root. |
| `models.downloads_root` | string | unset | - | - | Absolute, existing owner-only directory the downloads are installed in (strata's model store layout); strata's own verbs use it as roots.models with a models.cards config. |
| `models.prepare_python` | string | unset | - | - | Absolute path of the Python that prepares a downloaded Qwen model (upstream's pack tools; they need numpy and regex). Unset: Qwen downloads stay files only. Needs models.prepare_tools. |
| `models.prepare_tools` | string | unset | - | - | Absolute directory of upstream Strata's tools (iq_pack.py, strata_tokenizer.py, gguf_reader.py), read only. Needs models.prepare_python. |
| `models.download_engine_card` | string | unset | - | - | The qwen card whose engine (executable, library folders, GPU, options) a prepared download's card uses. |
| `models.default_engine` | string | unset | - | - | Absolute path of the engine (multiengine) the cards the page writes use; with it the server also serves with no card yet. The first start sets it to the engine beside the server. |
| `models.default_root` | string | unset | - | - | Absolute folder the page offers first for a download or a model's files (the first start's data/models). |
| `alerts.email.enabled` | boolean | `false` | - | - | Send email alerts (a build with SECURE_FRONTEND_EMAIL_ALERTS only; any other build refuses alerts.*). The password is kept in roots.secret (smtp-password), never in this file. |
| `alerts.email.host` | string | unset | - | - | The SMTP server, a DNS name (IONOS: smtp.ionos.com, or the account's regional host). Needed with alerts.email.enabled. |
| `alerts.email.port` | integer | `465` | 1-65535 | - | The SMTP port: 465 with ssl (IONOS's primary), 587 with starttls. |
| `alerts.email.security` | string | `ssl` | - | - | ssl (TLS from the first byte) or starttls (STARTTLS before AUTH); never plain text; TLS 1.2 or later. |
| `alerts.email.username` | string | unset | - | - | The SMTP user name (IONOS: the full mailbox address). Needed with alerts.email.enabled. |
| `alerts.email.from` | string | unset | - | - | The From address (IONOS: the mailbox the user name logs in to). Needed with alerts.email.enabled. |
| `alerts.email.to` | string array | `[]` | - | - | 1-5 addresses the alerts go to. Needed with alerts.email.enabled. |
| `alerts.email.events` | string array | `[]` | - | - | Which alerts are sent: download_finished, download_failed, calibration_finished, calibration_failed, engine_stopped, server_started (absent: all). |
| `search.enabled` | boolean | `false` | - | - | Let the model search the web through a SearXNG instance when it is unsure or the answer needs recent information (a build with SECURE_FRONTEND_WEB_SEARCH only; any other build refuses search.*). The searches leave this PC: what the model searches for comes from the chat. |
| `search.url` | string | unset | - | - | The SearXNG instance, http(s)://host[:port][/path], with the JSON format on (its settings.yml: search.formats lists json). Needed with search.enabled. |
| `search.results` | integer | `5` | 1-10 | - | Results the model gets per search (1-10). |
| `search.max_rounds` | integer | `8` | 1-8 | - | Searches one answer may make (upstream's mcp.max_rounds, 1-8). |
| `search.timeout_s` | integer | `30` | 5-120 | - | Seconds each step of a search may take: the connection, the request, each read of the reply (5-120). |
<!-- config-keys:end -->

`roots.secret` defaults to the config file's directory only for a `file:` token
source; an `env:` source never opens it. `roots.logs` and `roots.cache` may be
absent if their parent is a private directory; only serving and `init` create
them. `--check` creates nothing.

**Replicas (SP5 narrowing, spec C-4).** N-child replica scheduling is not
implemented in SP5; `--parallel` within one engine is. The pinned scheduler's
`replicas` entry and its `replica_*` members are rejected at load, naming the
key; what stands is engine-internal parallelism: `replicas.parallel` decodes up
to N requests together inside the one engine child (rendered to the engine as
`--batch N`, exactly like the pinned server; 1, the default, is one request at
a time, and the engine's batch window is 8).

**Vision (SP5 M1, spec D-9).** Image *input* is typed and plumbed; image
*generation* stays out (its routes answer 404, parity table). `vision.enabled`
needs the manifest artifact `launch.mmproj` — the files[] role `mmproj` is
listed like any other file, at most one — and the engine then receives
`--vision`, like the pinned server's section gate (serve/server.py:2926-2931).
The encoder is a supervised `strata-vision` child, a sibling of the engine
executable (the pin's config-named vision exe stays rejected, spec §4); it
must print READY within `vision.ready_timeout_s` (300 s, the pin's
VISION_READY_S; 0 waits for ever), and a resident encoder that cannot start
fails the boot — `vision.enabled` promises images. `vision.resident` false
waits for the first picture (the pin's laziness, :2646). On the request path
images arrive as `data:` URLs only — an http(s) image URL is refused 400 by
name, never fetched — `vision.max_side` and `vision.max_n` cap them before
any decode, `vision.encode_timeout_s` (the pin's VISION_ENCODE_S, :328)
bounds one ENC round trip, and every staged or encoded scratch file lives
under `<state>/vision` (owner-private) and is deleted when the request ends.
`engine.max_tokens`/
`engine.min_tokens` also reach the child as `--max-tokens`/`--min-tokens`
exactly as at the pin (:2564-2567), and `vision.no_flash_attn` is the added
spec §5.1 family flag. Real encode behaviour on real images is proven only by
the owner-box acceptance kit.

**Pictures on model cards (2026-10-28, spec
`docs/superpowers/specs/2026-10-28-vision-for-cards-design.md`).** A qwen card
(a Qwen3.8 pack or a Qwen3.6 GGUF) reads pictures with its own encoder file:
the card rows `vision.mmproj` (absolute; it must pass the model folders' rule:
a regular file, not a link, in a folder no other user may change unless
`roots.allow_untrusted_media`) and `vision.gpu`, and the card option `vision`
(Pictures: `gpu`, the default upstream's setup gives, `cpu` or `off`), which
writes `vision.enabled` and `vision.gpu`. The launch adds `--vision` to the
engine; the per-card runtime starts upstream's `strata-vision` beside the
engine (`--mmproj`, `--model` the model's first GGUF, `--gpu`, and
`vision.no_flash_attn` as `--flash-attn off`) with the model, and stops it
with the model; a card whose encoder does not reach READY does not start. The
other profiles read no pictures (their launch refuses `vision.enabled`). A
download with the **Vision files** box ticked (`POST /v1/strata/downloads/start`
with `"vision": true`; the list's `vision_bytes` is the encoder's size, 0
where there is none) pulls the catalog's pinned `mmproj` as the manifest's
`mmproj` file, from its own repository and revision, and the card names it;
**Add a model** gives a qwen card the folder's one `mmproj*.gguf` (none, or
two: none). The page attaches PNG, JPEG and WebP pictures while the running
card's Pictures option is on, sent as `image_url` data URLs.

**Several GPUs (2026-10-28, spec `docs/superpowers/specs/2026-10-28-multi-gpu-design.md`).** Every card has a GPUs
choice (the option `gpus`, kind `devices`, offered by the launcher after Web search): `"auto"` or GPU numbers 0-63 as
nvidia-smi numbers them, such as `"0,1"`, none repeated, at most 16 (one on Gemma 4 and flint, whose engines use the
first). It writes `engine.gpu` (qwen: `--gpu` and, with two or more, upstream's `--layer-split auto` unless the card pins
`engine.layer_split`; deepseek and minimax: `--gpus`; gemma4 and flint: `--gpu`) or `glm.gpu` (glm53 and raon1:
`CUDA_VISIBLE_DEVICES`); auto writes nothing, so the card's own rows stand. With several GPUs named and `glm.layer_split`
at auto, the GLM family's environment no longer forces `STRATA_GLM_SPLIT=0`: Maya's engine splits the layers across them
by default (one GPU or none keeps the single device; a pinned split wins). DeepSeek's `--tp` and `--threads` are composed
as Maya's `write_config_ds4` writes them, `--tp min(2, GPUs)` and `--threads on` on one GPU, unless the card names them
(cards from this build fix neither). `GET /v1/strata/about` lists `hardware.gpus` (`index`, `name`, `mem_bytes`), read
once by `nvidia-smi --query-gpu=index,name,memory.total` on the first request; the page draws one box per GPU and hides
the choice on a one-GPU PC. A GPU picture encoder adds upstream's `--vram-reserve-mib 700` to a qwen launch without a
reserve of its own, and upstream's encoder reads up to 1,024 image tokens on the GPU, 300 on the CPU.

**Constants.** Internal constants live in
`include/secure_frontend/api_constants.hpp` (SP3) and `config_defaults.hpp`
(SP1). SP3 removed SP1's 1000 ms minimum send budget (SP1 addendum A4); the
half-close drain (100 ms / 256 KiB) moved to `api_constants.hpp`; M2 adds the
streaming bounds `kMaxDeltaTextBytes` (16 KiB of text per content delta) and
`kMaxSseEventBytes` (128 KiB per SSE event).

## Strata option-surface parity (SP5 M1)

The pinned upstream Strata (`upstream.version`, tag `v0.2.2`) reads 88 config
keys and 32 environment knobs (the verbatim grep in
`secure_frontend/tests/parity_keys_upstream.hpp`); the parity table
(`secure_frontend/src/parity_table.cpp`) gives each one — plus a few rule rows
the flat grep cannot see — exactly one disposition, cited to the governing
read site. The three tables below mirror that table for humans, one block per
disposition; the C++ table stays the authority and the pytest gate
`tools/secure/test_parity_readme_coverage.py` re-checks these blocks against
it. Typed options live in the config table above (each note names the wiring
task); rejected options are refused **at config load, by name**, and
`strata doctor` warns about ignored `STRATA_*`/`MAYA_*` environment variables.
Narrowings: `replicas.parallel` is typed but renders `--batch N` inside the
one child (C-4: the pin's N-child replica scheduler is not implemented);
`session.answer_room` is typed only (the pin's formula sits in its thinking
parser, not in our §7.2 room rule); `lazy_load` is typed but has no flag (the
pin has none either); the vision encoder gets no per-generation cgroup in M1
(recorded gap); `session.keep_files` keeps the pin's default but its LRU
consumer is out of M1 — every request cleans up after itself instead.

### Kept as typed options

<!-- parity-typed:begin -->
| Upstream option | Destination | Disposition note and the pin read site |
| --- | --- | --- |
| `aliases` | frontend behaviour | serve/server.py:8493, set_aliases :3951-3964; serve/runconfig.py:34 (M1 Task 8: model.aliases — /v1/models lists them, chat matches exactly) |
| `model_name` | frontend behaviour | serve/server.py:8074 default_model_name, :3966 model_names (session log step 13: model.name — the served name /v1/models lists and requests name; unset keeps the inventory or manifest id) |
| `allowed_hosts` | frontend behaviour | already typed (SP1/SP3 listen); serve/server.py:8513 |
| `answer_room` | typed config | serve/server.py:8461 (`is not False`), formula :4893-4905; typed only: the pin's formula sits in its thinking parser, not in our §7.2 room rule (Ruling T8-d, Task 15) |
| `anthropic_thinking` | typed config | serve/server.py:8549 (M1 Task 5: reasoning.anthropic_thinking) |
| `codex_compaction_cache` | typed config | serve/server.py:8491 (M1 Task 9: session.codex_compaction_cache) |
| `codex_thread_titles` | typed config | serve/server.py:8490 (M1 Task 9: session.codex_thread_titles) |
| `cors_origins` | frontend behaviour | already typed (SP3 B12: listener.cors_origins); serve/server.py:8510 |
| `effort_position` | engine argv | serve/server.py:2979 (M1 Task 5: reasoning.effort_position composes the engine's --tail-role-token; session log step 16: then a low or medium level's sentence goes right before the answer, :4557 render_prompt) |
| `elastic` | frontend behaviour | serve/server.py:2618, :8138 (M1 Task 7: engine.elastic; Tasks 11-12 consume it) |
| `elastic_idle_s` | frontend behaviour | serve/server.py:2620, :4041 (M1 Task 7: engine.elastic_idle_s) |
| `elastic_mib` | frontend behaviour | serve/server.py:2619, :4019 (M1 Task 7: engine.elastic_mib; /v1/vram headroom) |
| `engine_silence_s` | frontend behaviour | serve/server.py:3002, default 300 at :277, engine_silence_s() :2905-2912; serve/runconfig.py:39 (M1 Task 8: the pump's silence watchdog) |
| `env_to_user` | typed config | serve/server.py:8557 (M1 Task 9: session.env_to_user) |
| `expert_profile_save` | engine argv | serve/server.py:3203 (M1 Task 9: --expert-profile-save; an operator-owned flag wins, :3071) |
| `expert_profile_save_every` | engine argv | serve/server.py:3207, default 10 documented at :3066 (M1 Task 9: --expert-profile-save-every; rides the save, :3073) |
| `fit_max_tokens` | frontend behaviour | serve/server.py:8442 (M1 Task 5: engine.fit_max_tokens) |
| `gpu` | engine argv | already typed (SP2 engine.gpu; compose renders --gpu); serve/server.py:2918 (gpu_list :2819-2826: a set `gpu` wins; unset, a saved `gpu_roles` picks the cards, Main last, :2822-2825 — see the gpu_roles row) |
| `gpu_order` | engine argv | serve/server.py:3357 (M1 Task 7: `as_given` keeps the order; at v0.2.2 active `gpu_roles` keep it too and a HIP built-in Radeon goes first before the speed sort, :3230-3247 — the typed auto or as_given vocabulary is unchanged) |
| `gpu_policy` | typed config | serve/images.py:174 (M1 Task 7: engine.gpu_policy, re-scoped to the vision child's GPU turn; the pin's use is the image-generation turn) |
| `hip_ordinal` | engine argv | serve/server.py:3227 (M1 Task 7) |
| `host` | frontend behaviour | already typed (SP1 listen.host); serve/server.py:8288 |
| `idle_unload_s` | frontend behaviour | serve/server.py:8528, :8298 (M1 Task 6: supervisor idle timer) |
| `keep_files` | typed config | serve/images.py:189 (`or 64`); typed only: the consumer is the pin's ImageService and D-9 keeps image generation out (Ruling T9-e, M1 Task 9) |
| `layer_split` | engine argv | serve/server.py:3015 (M1 Task 7) |
| `lazy_load` | frontend behaviour | serve/server.py:8356, :967-994 (M1 Task 6: the first request spawns the same composed launch) |
| `max_n` | frontend behaviour | serve/images.py:103 (M1 Tasks 11-12: vision.max_n; the pin's cap lives on the images block) |
| `max_side` | frontend behaviour | serve/images.py:102 (M1 Task 12: vision.max_side pre-decode cap) |
| `max_tokens` | request rule | serve/server.py:2632 (vision encoder at the pin); engine.max_tokens re-scoped as the request output ceiling (added, C-1; Ruling T8-e: caps at the §7.2 resolve site) |
| `min_free_vram_mib` | frontend behaviour | serve/server.py:8530 (M1 Task 7: engine.min_free_vram_mib) |
| `min_tokens` | request rule | serve/server.py:2650 (vision encoder at the pin); engine.min_tokens re-scoped as the request output floor (added, C-1; Ruling T8-e: lifts at the §7.2 resolve site) |
| `mtp` | engine argv | strata_control/draft_layer.py is_off; serve/server.py:2966-2969 (M1 Task 6: engine.mtp drops --mtp/--spec-min-p/--batch-mtp at every start) |
| `need_mib` | frontend behaviour | serve/images.py:157 (M1 Task 11: vision.need_mib; added security bound) |
| `parallel` | engine argv | serve/server.py:3141 (replicas.parallel renders --batch N, :3020; window PARALLEL_MAX 8 at :2999; cap 1024 from the pin's own bound :8054, M1 Task 10) |
| `reasoning_budget_tokens` | typed config | serve/server.py:8567 (M1 Task 5: reasoning.budget_tokens) |
| `reasoning_close_retry` | typed config | serve/server.py:8462 (M1 Task 5: reasoning.close_retry) |
| `reasoning_effort` | frontend behaviour | serve/server.py:7856 (M1 Task 5: reasoning.effort; the qwen think tail, issue 19: none writes the closed block, unset/low/medium/high open it; session log step 14: the same words per request on the qwen profile; step 16: low and high (unset) write the template's effort sentence, medium none) |
| `reasoning_loop_recovery` | typed config | serve/server.py:8577 (M1 Task 5: reasoning.loop_recovery) |
| `repeat_stop_tokens` | frontend behaviour | serve/server.py:8562 (REPEAT_STOP_TOKENS 256 at :198, the pin's detector is frontend-side at :5098, notice :5278, 0 = off at :8334; Ruling T8-f: no GEN key) |
| `sampling` | typed config | serve/server.py:7905 (M1 Task 4: defaults.sampling.*) |
| `slot_save_path` | typed config | serve/server.py:8541 + slot_save_dir :3656-3668; the pin uses it FRONTEND-side for its /slots file store (:3885-3902) — no argv append (Ruling T9-a, M1 Task 9) |
| `split_skip_if_fits` | engine argv | serve/server.py:3118 (M1 Task 7) |
| `threads` | engine argv | serve/server.py:2645 (M1 Task 11: vision.threads; the only pin read of `threads`, inside the vision reader_args — the flag lands on the encoder child's argv) |
| `tool_call_recovery` | typed config | serve/server.py:8553 (M1 Task 9: session.tool_call_recovery) |
| `trusted_origins` | frontend behaviour | already typed (SP3 origins family); serve/server.py:8511 |
| `uma` | frontend behaviour | serve/images.py:23, :335 (M1 Task 7: engine.uma; Tasks 11-12 consume it) |
| `update.source` | typed config | our key (PX §4 reserved; SP7 M2 D-10): the HTTPS mirror list read only by 'strata update' |
| `vision` | typed config | serve/server.py:3107 (M1 Task 11: the vision.* block) |
| `vram_elastic` | engine argv | serve/server.py:3122 (M1 Task 7) |
| `vram_segment_mib` | engine argv | serve/server.py:3124 (M1 Task 7) |
| `STRATA_ALLOWED_HOSTS` | frontend behaviour | same semantics as allowed_hosts, already typed; serve/server.py:8513 |
| `STRATA_BATCH_MTP` | engine argv | serve/server.py:3150; engine.mtp (M1 Task 6) |
| `STRATA_ENGINE_READY_S` | frontend behaviour | already typed: ready_timeout (SP1); serve/server.py:336 |
| `STRATA_ENGINE_STALL_S` | frontend behaviour | serve/server.py:315; engine.silence_s (M1 Task 8) |
| `STRATA_MAX_BODY_MIB` | frontend behaviour | already typed: limits.chat.max_body_bytes (SP1); serve/server.py:6086 |
| `STRATA_VISION_ENCODE_S` | frontend behaviour | serve/server.py:335 (M1 Task 12: vision encode-timeout row) |
| `STRATA_VISION_READY_S` | frontend behaviour | serve/server.py:334 (M1 Task 11: vision.ready_timeout_s; 0 = wait for ever, :292) |
| `vram_reserve_mib` | engine argv | serve/runconfig.py:42 renders --vram-reserve-mib (M1 Task 7) |
<!-- parity-typed:end -->

### Mapped to a Strata mechanism instead

<!-- parity-mapped:begin -->
| Upstream option | Where it lives in Strata instead (and the pin read site) |
| --- | --- |
| `api_key` | SP1 token store is the only auth source (CFG-06: no secret in config); serve/server.py:8295 |
| `arch` | model identity comes from the SP4 manifest/GGUF inventory; serve/server.py:8074 |
| `backend` | engine identity: SP4 manifest + engine gate; serve/server.py:8106 |
| `engine_ready_s` | ready_timeout (SP1) already carries engine readiness — see the STRATA_ENGINE_READY_S env row; serve/server.py:3047 |
| `gpu_none` | strata_control/gpu_roles.py saved_roles :399; serve/server.py:2841 (gpu_all_off), :3353-3355 (every card Off: empty CUDA/HIP_VISIBLE_DEVICES), :2854-2859 (refused at start for a GPU engine); card choice is the typed engine.gpu (SP2 §3.9: rendered as --gpu, never a GPU variable), so the all-Off run has no counterpart |
| `gpu_roles` | strata_control/gpu_roles.py saved_roles :386-405 (:390); serve/server.py:2822-2825 (gpu_list: the cards, Main last, when `gpu` is unset), :3224 (keeps the given order); card choice and order are the typed engine.gpu (SP2 §3.9: rendered as --gpu), the Main/Helper/Off roles have no typed form |
| `images` | spec D-9: image generation is out, the route answers 404 like an unknown route; serve/server.py:8463 |
| `max_queue` | the SP3 row limits.concurrency.max_queued_generations already carries queue admission (pin bound 0..4096 at serve/server.py:8259, derived default :3789-3791; Ruling T8-a) |
| `native` | the SP4 manifest composes --native (engine_arguments); serve/images.py:126 |
| `out_dir` | spec D-9 image generation is out; serve/images.py:186 |
| `pool_cores` | the operator's engine.args carries the engine's own --pool-cores LIST (not frontend-owned and not a mode: engine_arguments.cpp passes it through; no typed row); serve/server.py:3164, pool_cores_args :3026-3046 (experimental, off by default; a --pool-cores already in the args wins, :3034), composed after parallel at :2995 |
| `pre` | the SP4 manifest carries the tokenizer pack (the tokenizer row's rule); serve/server.py:8328 |
| `quant` | quant identity comes from the SP4 manifest; serve/images.py:283 |
| `queue_timeout` | the SP3 row limits.concurrency.queue_timeout_ms already carries admission waiting (pin bound 0.1..600 at serve/server.py:8263; Ruling T8-a) |
| `reload` | image-generation card reload mode (spec D-9); the vision child's lifecycle follows the vision/elastic rows; serve/images.py:177 |
| `sigmas` | spec D-9 image generation is out; serve/images.py:120 |
| `special_ids` | the SP4 manifest carries the tokenizer pack (the tokenizer row's rule); serve/server.py:8328 |
| `steps` | spec D-9 image generation is out; serve/images.py:104 |
| `timeout_s` | the SP3 row limits.engine.generation_timeout_s already carries the generation deadline (pin: generation job timeout at serve/images.py:227; Ruling T8-b) |
| `tokenizer` | the SP4 manifest carries the tokenizer; serve/server.py:8309 |
| `STRATA_ADMIT_MAX_WAIT_S` | serve/server.py:324; admission waiting is already carried by limits.concurrency.queue_timeout_ms (Ruling T8-a) |
| `STRATA_ADMIT_SHORT_FIRST` | SP3 admission ordering is the pool policy (B8); serve/server.py:321 |
| `STRATA_API_KEY` | SP1 token store is the only auth source; serve/server.py:8184 |
| `STRATA_DROP_CACHE_ON_EXIT` | the SP2 shutdown order (§3.7) is fixed by the supervisor; serve/cache_drop.py:1 |
| `STRATA_ENGINE_NICE` | the SP2 child pre-exec state (§3.12) is fixed by the supervisor, which sets no engine priority; serve/server.py:3294 (engine_nice :3154-3168: Linux only, +5 by default beside a built-in Radeon), applied at spawn :1017-1022 |
| `STRATA_HTTP_BACKLOG` | the listener backlog is fixed by the SP3 poll design (128 entries); serve/server.py:7445 |
| `STRATA_KEEP_EMPTY_TURNS` | the frontend serves stateless requests; conversation history is the client's; serve/frontend.py:60 |
| `STRATA_KEEP_LEFTOVERS` | lifecycle is teardown + respawn (M1 Task 6); serve/server.py:133 |
| `STRATA_LIFELINE` | SP2 supervision replaces the lifeline watcher (pdeathsig, monitor handle); serve/lifeline.py:33 |
| `STRATA_NO_PRESET` | launch is SP4 manifest-composed; serve/server.py:7873 |
| `STRATA_OFFLINE` | PRIV-01: the frontend never reaches the network; enforced by design and the DL-01 proof; serve/server.py:3525 |
| `STRATA_REQUEST_LINES` | the wire record is fixed by docs/security/engine-protocol-limits.md; serve/server.py:1021 |
| `STRATA_RELEASE_PUBKEY_HEX` | compile definition pinning the Ed25519 release public key (SP7 M2 D-2); read by the release-manifest verifier and reported by 'strata update'; empty refuses updates |
| `STRATA_START_RETRIES` | respawn retries belong to the SP2 RestartTracker (restart_policy.hpp); serve/server.py:630 |
| `STRATA_TOKENIZER` | the SP4 manifest carries the tokenizer; serve/test_detok.py:21 |
<!-- parity-mapped:end -->

### Excluded keys — refused at config load

<!-- parity-rejected:begin -->
| Upstream option | Excluded by (and the pin read site) |
| --- | --- |
| `api_monitor` | spec §4 web/desktop/telemetry; serve/server.py:8519 |
| `args` | spec §4 free-form process control; serve/server.py:3359 |
| `batch_telemetry` | spec §4 telemetry endpoints; serve/server.py:916 |
| `before_load` | spec §4 command execution; serve/server.py:8330 |
| `before_load_ignore_failure` | spec §4 command execution; serve/server.py:8537 |
| `before_load_strict` | spec §4 command execution; serve/server.py:8538 |
| `before_load_timeout_s` | spec §4 command execution; serve/server.py:8533 |
| `command` | spec §4 MCP command; serve/mcp.py:610 |
| `cwd` | spec §4 free-form process control; serve/mcp.py:106 |
| `env` | spec §4 raw env dict; serve/server.py:3511 |
| `exe` | spec §4 free-form process control (engine identity comes from the SP4 manifest); serve/server.py:8100 |
| `headers` | spec §4 MCP headers; serve/mcp.py:257 |
| `lib_dirs` | spec §4 free-form process control; serve/images.py:135 |
| `log` | spec §4 free-form process control; serve/replicas.py:99 |
| `mcp` | spec §4 MCP; serve/mcp.py:632 |
| `mcp_servers` | spec §4 MCP; serve/mcp.py:651 |
| `open_browser` | spec §4 web/desktop; serve/server.py:8639 |
| `prevent_sleep` | spec §4 desktop; serve/server.py:8478 |
| `replica_cpus` | spec C-4: N-child replica scheduling is the accepted narrowing; serve/server.py:7972 |
| `replica_load_tokens` | spec C-4: N-child replica scheduling is the accepted narrowing; serve/server.py:8006 |
| `replicas` | spec C-4: N-child replica scheduling is the accepted narrowing; serve/replicas.py:44 |
| `telemetry_read_key` | spec §4 telemetry endpoints; serve/server.py:8500 |
| `type` | spec §4 MCP entry type; serve/mcp.py:604 |
| `url` | spec §4 MCP url; serve/mcp.py:384 |
| `user_args` | spec §4 free-form process control; serve/server.py:3084 |
| `STRATA_BATCH_TELEMETRY` | spec §4 telemetry endpoints; serve/server.py:918 |
| `STRATA_BATCH_TRACE` | spec §4 no new web/diagnostics surface; serve/server.py:840 |
| `STRATA_DEBUG` | spec §4 no new web/diagnostics surface; serve/mcp.py:130 |
| `STRATA_FAIL_ONCE` | spec §4 free-form process control (fault-injection knob); serve/test_server.py:4742 |
| `STRATA_OWNER_ATTEST_KEY` | spec §4 signing material, deferred to SP7; serve/server.py:8246 |
| `STRATA_PARALLEL_SOLO` | spec C-4: N-child replica scheduling is the accepted narrowing; serve/server.py:1758 |
| `STRATA_REPLICA_START` | spec C-4: N-child replica scheduling is the accepted narrowing; serve/server.py:7988 |
| `STRATA_RUNTIME_PROFILE` | spec §4 no new web/diagnostics surface; serve/lifeline.py:265 |
| `STRATA_SYCL_BIN` | spec §4 free-form process control (external binary path); serve/server.py:2966 |
| `STRATA_TELEMETRY_KEY` | spec §4 telemetry endpoints; serve/server.py:8500 |
| `STRATA_VERIFY_TRACE` | spec §4 no new web/diagnostics surface; serve/server.py:1242 |
| `mcpServers` | spec §4 MCP (Claude Desktop block); serve/mcp.py:648 |
<!-- parity-rejected:end -->

### Out of scope — no channel, reaches nothing (SP5-P3 refresh: the DS4-path knobs and the drafting-card logic)

The pin reads these, but they reach nothing in our frontend: the DeepSeek DS4 path is out of scope (plan 2026-10-22-sp5-p2 §Non-goals), the drafting-card suggestion logic is upstream's own (the frontend composes MTP from the typed engine.mtp row), and a prefix fragment or test-harness gate is not a knob. Every row still carries its citation.

<!-- parity-out:begin -->
| Upstream name | Where it is read and why it reaches nothing |
| --- | --- |
| `STRATA_DS4_` | a prefix fragment in the pin's own DS4.1 test script — not a knob name; the full names carry their own rows (SP5-P2 §Non-goals: the DeepSeek DS4 path); serve/test_dsv41.py:616 |
| `STRATA_DS4_AUTO_PIN` | the DeepSeek DS4 engine path (SP5-P2 §Non-goals: DS4-path knobs get out-of-scope rows, not typed config): system-prompt auto-pin on the DS4 card; serve/server.py:4934 |
| `STRATA_DS4_SLOT_DIR` | the DeepSeek DS4 engine path's slot-folder feature (SP5-P2 §Non-goals); serve/server.py:3490 |
| `STRATA_DS4_SLOT_GB` | the DeepSeek DS4 engine path's slot-size cap (SP5-P2 §Non-goals); serve/server.py:3490 |
| `STRATA_DS4_SLOT_KEEP` | the DeepSeek DS4 engine path's slot retention (SP5-P2 §Non-goals); serve/server.py:3488 |
| `STRATA_DSV41_PACK` | the pin's own tokenizer test harness (skipUnless gate naming the DS4.1 pack folder); serve/test_dsv41.py:126 |
| `drafting` | the pin's own drafting-card suggestion logic (drafting.suggest picks the card class); the frontend composes MTP from the typed engine.mtp row instead — no drafting-card surface in the SP5 spec; serve/server.py:3088 |
<!-- parity-out:end -->


## MAYA v1.1.21 option-surface parity (SP5 M2)

The vendored MAYA reference (`engines/maya-ref/`, tag `v1.1.21`, read-only —
nothing of it is compiled by our presets; `docs/PROVENANCE.md`) is the
`glm53` profile's reference surface. Its parity table
(`secure_frontend/src/parity_table.cpp`, `kMayaRows`) gives all 730 options —
183 typed, 50 rejected, 33 mapped, 464 out of scope — exactly one disposition,
cited to a read site live at the pin. The option inventory was generated at
`v1.1.21` (`tests/parity_keys_maya.hpp`: 38 config keys, 575 env names) and the
drift tripwires `secure_frontend_parity_table_tests.scan_*` re-check it. That tag's `docs/SETTINGS.md` spells every
setting `MAYA_*`; the rows keep the `STRATA_*` names the engine reads and each
documented `MAYA_*` spelling carries its own alias row naming the `STRATA_*`
row behind it (`include/strata/platform/env_alias.hpp` maps `MAYA_X` to
`STRATA_X` at engine start). The blocks below mirror that
table for humans; the pytest gate
`tools/secure/test_parity_readme_coverage.py` re-checks them against it. The
typed **child-env** rows form a closed 146-name allowlist for the glm-family
child (`kFamilyForcibleEnvironment`): no other `STRATA_*`/`MAYA_*` name can
reach that child at all (D-7). Free-form launch is retired: a `glm53`
manifest refuses any `engine.args` (D-8) — the launch is composed from the
manifest and this table. Declared gaps the acceptance kit runs without, on
both sides: `--expert-profile`, `--expert-profile-save(-every)` and
`--kv-resident` have no typed value source yet (`--spec*` was retired by the
engine at v1.1.21), and `--parallel` is bounded 1..8 at composition (`docs/validation/sp5-parity-kit.md` §0).

### Kept as typed options

<!-- maya-typed:begin -->
| Maya option | Destination | Disposition note and the MAYA v1.1.27 read site |
| --- | --- | --- |
| `cors_origins` | frontend behaviour | already typed (SP3 B12: listener.cors_origins); serve/server.py:4476 |
| `experimental_speed_projection` | typed config | serve/server.py:3819 (shared settings, bool); Task 3: glm.experimental_speed_projection |
| `fit_max_tokens` | typed config | serve/server.py:4454; Task 3: glm.fit_max_tokens (M1 engine.fit_max_tokens pattern) |
| `gpu` | engine argv | already typed (SP2 engine.gpu renders --gpu); serve/server.py:1185, child_env :1280-1288, maya.py:2440; Task 7 |
| `host` | frontend behaviour | already typed (SP1 listen.host); serve/server.py:4360, maya.py:2855 |
| `idle_unload_s` | typed config | serve/server.py:4471 (the v1.1.21 Maya server gained the idle-unload timer, default 0 = never at :3753; typed as glm.idle_unload_s — it wins over engine.idle_unload_s for a glm53 profile, R-T4-b) |
| `layer_split` | engine argv | serve/server.py:1484-1277 renders --layer-split; docs/SETTINGS.md:54; Task 7 |
| `max_tokens` | request rule | serve/server.py:1189, :1213 (the vision block), shared settings :3815; engine.max_tokens stays the request output ceiling (M1 Ruling T8-e pattern) |
| `min_p` | typed config | serve/server.py:3808 (shared settings: 0 <= min_p < 1), sampling-block validator :3848; defaults.sampling.min_p |
| `model_name` | typed config | serve/server.py:4452, maya.py:2870; Task 3: glm.model_name (Ruling T2-c: managed launches still take served identity from the SP4 manifest) |
| `no_flash_attn` | frontend behaviour | serve/server.py:1191, :1222 (--no-flash-attn on the encoder child); Task 8: vision.no_flash_attn |
| `port` | frontend behaviour | already typed (SP1 listen port); maya.py:2854, :3132 |
| `reasoning_effort` | typed config | serve/server.py:4458, shared settings :3798 (none|low|high|max); Task 3: glm.reasoning_effort |
| `resident` | typed config | serve/server.py:4392 (on-demand vs resident encoder); Task 8: vision.resident |
| `sampling` | typed config | serve/server.py:4142-3836 (supported keys, validators from :3840); maya.py:2243 (SAMPLING defaults); defaults.sampling.* (M1 Task 4) |
| `seed` | typed config | serve/server.py:3815 (shared settings: positive integer), :265; defaults.sampling.seed (SP3 B9 neutral seed) |
| `temperature` | typed config | serve/server.py:3802 (shared settings: 0..2), config-block validator :3840 (>= 0, 0 = greedy); defaults.sampling.temperature |
| `thinking_budget` | typed config | serve/server.py:4470 (default 32768); C-3 keeps Maya's name: Task 3 glm.thinking_budget; STRATA_GLM_THINK_RESERVE rides it (docs/SETTINGS.md:165) |
| `threads` | engine argv | serve/server.py:1187 (the vision block; the flag lands on the encoder child :1221-1222); Task 8 (M1 Task 11 vision.threads pattern) |
| `tokenizer` | typed config | serve/server.py:4366, maya.py:2823, :3144; Task 3: glm.tokenizer — managed launches still compose the tokenizer from the SP4 manifest (Ruling T2-c) |
| `top_k` | typed config | serve/server.py:3811 (shared settings: integer 1..64), validator :3852; defaults.sampling.top_k |
| `top_p` | typed config | serve/server.py:3805 (shared settings: 0 < top_p <= 1), validator :3844; defaults.sampling.top_p |
| `vision` | typed config | serve/server.py:4385, :4011, :4015; the vision.* block (M1 Task 11); Task 8 the GLM side |
| `mmproj` | manifest artifact | serve/server.py:1221 (the vision child's --mmproj); M1 Task 11: manifest files[] role mmproj (at most one entry) |
| `model` | manifest artifact | serve/server.py:1221 (the vision child's --model file); the SP4 manifest composes the vision model artifact (Task 8) |
| `presence_penalty` | typed config | serve/server.py:263-265 (supported sampling fields), block docstring :3829-3834, validator from :3856; defaults.sampling.presence_penalty |
| `repetition_penalty` | typed config | serve/server.py:263-265, :274-279 (llama.cpp repeat_penalty is taken as this one); defaults.sampling.repetition_penalty |
| `frequency_penalty` | typed config | serve/server.py:263-265, :3829-3834; defaults.sampling.frequency_penalty |
| `penalty_last_n` | typed config | serve/server.py:263-265, :280-283, :305-306 (repeat_last_n maps; 0 = no penalties, -1 = the whole context up to PENALTY_WINDOW_MAX 4096 :260, below -1 refused); defaults.sampling.penalty_last_n |
| `STRATA_ENGINE_STALL_S` | frontend behaviour | serve/server.py:397, docs/SETTINGS.md:166 (the pump's silence watchdog — M1 engine.silence_s pattern) |
| `STRATA_GLM_AHEAD` | engine env | docs/SETTINGS.md:109 |
| `STRATA_GLM_CKPTS` | engine env | docs/SETTINGS.md:80 |
| `STRATA_GLM_CKPT_EVERY` | engine env | docs/SETTINGS.md:81 |
| `STRATA_GLM_COMPLETE_POOL` | engine env | src/core/glm_fast_path.cu:1207 (docs/SETTINGS.md:50 documents it as MAYA_GLM_COMPLETE_POOL: typed per D-7 onto the closed child-env allowlist — a value reaches the child only through a forced row this frontend composes) |
| `STRATA_GLM_CPU_1T` | engine env | docs/SETTINGS.md:126 |
| `STRATA_GLM_CPU_CONCURRENT` | engine env | src/core/glm_fast_path.cu:2686 (docs/SETTINGS.md:60 documents it as MAYA_GLM_CPU_CONCURRENT: typed per D-7 onto the closed child-env allowlist — a value reaches the child only through a forced row this frontend composes) |
| `STRATA_GLM_CPU_LANE` | engine env | docs/SETTINGS.md:39 |
| `STRATA_GLM_CPU_LANE0` | engine env | src/core/glm_fast_path.cu:2735 (the per-GPU CPU-lane form: the engine getenvs the constructed name STRATA_GLM_CPU_LANE+<dev> at this line; documented as MAYA_GLM_CPU_LANE0/1, docs/SETTINGS.md:47) |
| `STRATA_GLM_CPU_LANE1` | engine env | src/core/glm_fast_path.cu:2735 (the per-GPU CPU-lane form: the engine getenvs the constructed name STRATA_GLM_CPU_LANE+<dev> at this line; documented as MAYA_GLM_CPU_LANE0/1, docs/SETTINGS.md:47) |
| `STRATA_GLM_CPU_MT` | engine env | docs/SETTINGS.md:126 |
| `STRATA_GLM_CPU_PIN` | engine env | docs/SETTINGS.md:58 |
| `STRATA_GLM_CPU_PLAN` | engine env | docs/SETTINGS.md:64 |
| `STRATA_GLM_CPU_RETIME` | engine env | docs/SETTINGS.md:65 |
| `STRATA_GLM_CPU_SHARED` | engine env | docs/SETTINGS.md:58 |
| `STRATA_GLM_CPU_SPLIT` | engine env | docs/SETTINGS.md:63 |
| `STRATA_GLM_DISK_QD` | engine env | docs/SETTINGS.md:119 |
| `STRATA_GLM_DISK_READERS` | engine env | docs/SETTINGS.md:119 |
| `STRATA_GLM_DISK_SKIP_FROM` | engine env | docs/SETTINGS.md:109 |
| `STRATA_GLM_DISK_SKIP_W` | engine env | docs/SETTINGS.md:109 |
| `STRATA_GLM_DROP_CACHE` | engine env | docs/SETTINGS.md:143 |
| `STRATA_GLM_DSA_ALL` | engine env | src/kernels/cuda/glm_fast.cu:5572 (docs/SETTINGS.md:132 documents it as MAYA_GLM_DSA_ALL: typed per D-7 onto the closed child-env allowlist — a value reaches the child only through a forced row this frontend composes) |
| `STRATA_GLM_DSA_SCORE` | engine env | docs/SETTINGS.md:131 |
| `STRATA_GLM_DUAL_PREFILL` | engine env | src/core/glm_prefill.cu:4263 (docs/SETTINGS.md:66 documents it as MAYA_GLM_DUAL_PREFILL: typed per D-7 onto the closed child-env allowlist — a value reaches the child only through a forced row this frontend composes) |
| `STRATA_GLM_EVEN_GROUPS` | engine env | src/program/generate.cpp:2387 (docs/SETTINGS.md:86 documents it as MAYA_GLM_EVEN_GROUPS: typed per D-7 onto the closed child-env allowlist — a value reaches the child only through a forced row this frontend composes) |
| `STRATA_GLM_EXL3_DECODE` | engine env | src/kernels/cuda/exl3.cu:1853 (docs/SETTINGS.md:116 documents it as MAYA_GLM_EXL3_DECODE: typed per D-7 onto the closed child-env allowlist — a value reaches the child only through a forced row this frontend composes) |
| `STRATA_GLM_EXL3_MMA_ROWS` | engine env | docs/SETTINGS.md:98 |
| `STRATA_GLM_EXL3_MMA_ROWS_SHORT` | engine env | src/core/glm_prefill.cu:526 (docs/SETTINGS.md:139 documents it as MAYA_GLM_EXL3_MMA_ROWS_SHORT: typed per D-7 onto the closed child-env allowlist — a value reaches the child only through a forced row this frontend composes) |
| `STRATA_GLM_EXL3_ROT` | engine env | src/core/glm_prefill.cu:434 (docs/SETTINGS.md:117 documents it as MAYA_GLM_EXL3_ROT: typed per D-7 onto the closed child-env allowlist — a value reaches the child only through a forced row this frontend composes) |
| `STRATA_GLM_EXPERT_CACHE` | engine env | src/core/glm_fast_path.cu:5888 (docs/SETTINGS.md:217 documents it as MAYA_GLM_EXPERT_CACHE: typed per D-7 onto the closed child-env allowlist — a value reaches the child only through a forced row this frontend composes) |
| `STRATA_GLM_EXPERT_HELPER` | engine env | src/core/glm_fast_path.cu:1897 (docs/SETTINGS.md:47 documents it as MAYA_GLM_EXPERT_HELPER: typed per D-7 onto the closed child-env allowlist — a value reaches the child only through a forced row this frontend composes) |
| `STRATA_GLM_FLUSH` | engine env | src/core/glm_fast_state.hpp:79 (docs/SETTINGS.md:52 documents it as MAYA_GLM_FLUSH: typed per D-7 onto the closed child-env allowlist — a value reaches the child only through a forced row this frontend composes) |
| `STRATA_GLM_GROUP_EXPERTS` | engine env | src/core/glm_memory.hpp:33 (docs/SETTINGS.md:76 documents it as MAYA_GLM_GROUP_EXPERTS: typed per D-7 onto the closed child-env allowlist — a value reaches the child only through a forced row this frontend composes) |
| `STRATA_GLM_GU_STAGE` | engine env | src/kernels/cuda/glm_fast.cu:1943 (docs/SETTINGS.md:136 documents it as MAYA_GLM_GU_STAGE: typed per D-7 onto the closed child-env allowlist — a value reaches the child only through a forced row this frontend composes) |
| `STRATA_GLM_HEAD_SPLIT` | engine env | src/core/glm_model.cu:1242 (docs/SETTINGS.md:148 documents it as MAYA_GLM_HEAD_SPLIT: typed per D-7 onto the closed child-env allowlist — a value reaches the child only through a forced row this frontend composes) |
| `STRATA_GLM_HEAD_SPLIT_CHECK` | engine env | src/core/glm_window.cu:1366 (docs/SETTINGS.md:124 documents it as MAYA_GLM_HEAD_SPLIT_CHECK: typed per D-7 onto the closed child-env allowlist — a value reaches the child only through a forced row this frontend composes) |
| `STRATA_GLM_KDA_GRAPH` | engine env | docs/SETTINGS.md:62 |
| `STRATA_GLM_KQ` | engine env | docs/SETTINGS.md:125 |
| `STRATA_GLM_KV_INT8` | engine env | docs/SETTINGS.md:129 |
| `STRATA_GLM_KV_PREFETCH` | engine env | docs/SETTINGS.md:130 |
| `STRATA_GLM_KV_RESIDENT` | engine env | docs/SETTINGS.md:130 |
| `STRATA_GLM_LEND_PARK_LRU` | engine env | src/core/glm_prefill.cu:1741 (docs/SETTINGS.md:73 documents it as MAYA_GLM_LEND_PARK_LRU: typed per D-7 onto the closed child-env allowlist — a value reaches the child only through a forced row this frontend composes) |
| `STRATA_GLM_LOOP_ANSWER` | engine env | docs/SETTINGS.md:145 (the loop guard: SP6 §4.9 keeps the guards a forced default the table may move; Task 4) |
| `STRATA_GLM_LOOP_THINK` | engine env | docs/SETTINGS.md:145 (the loop guard: SP6 §4.9 keeps the guards a forced default the table may move; Task 4) |
| `STRATA_GLM_MISS_SKIP_FROM` | engine env | src/core/glm_fast_path.cu:1798 (docs/SETTINGS.md:110 documents it as MAYA_GLM_MISS_SKIP_FROM: typed per D-7 onto the closed child-env allowlist — a value reaches the child only through a forced row this frontend composes) |
| `STRATA_GLM_MISS_SKIP_W` | engine env | src/core/glm_fast_path.cu:1797 (docs/SETTINGS.md:110 documents it as MAYA_GLM_MISS_SKIP_W: typed per D-7 onto the closed child-env allowlist — a value reaches the child only through a forced row this frontend composes) |
| `STRATA_GLM_MLA_MMA` | engine env | src/kernels/cuda/glm_fast.cu:5640 (docs/SETTINGS.md:133 documents it as MAYA_GLM_MLA_MMA: typed per D-7 onto the closed child-env allowlist — a value reaches the child only through a forced row this frontend composes) |
| `STRATA_GLM_MLA_V70` | engine env | src/kernels/cuda/glm_batch.cu:2431 (docs/SETTINGS.md:134 documents it as MAYA_GLM_MLA_V70: typed per D-7 onto the closed child-env allowlist — a value reaches the child only through a forced row this frontend composes) |
| `STRATA_GLM_MLA_V70_SPLITS` | engine env | src/kernels/cuda/glm_batch.cu:2457 (docs/SETTINGS.md:134 documents it as MAYA_GLM_MLA_V70_SPLITS: typed per D-7 onto the closed child-env allowlist — a value reaches the child only through a forced row this frontend composes) |
| `STRATA_GLM_MMQ_GLU` | engine env | docs/SETTINGS.md:142 |
| `STRATA_GLM_MMQ_RESERVE_MB` | engine env | src/core/glm_fast_path.cu:5915 (docs/SETTINGS.md:123 documents it as MAYA_GLM_MMQ_RESERVE_MB: typed per D-7 onto the closed child-env allowlist — a value reaches the child only through a forced row this frontend composes) |
| `STRATA_GLM_MTP_DECODE` | engine env | src/core/glm_mtp.cu:83 (docs/SETTINGS.md:85 documents it as MAYA_GLM_MTP_DECODE: typed per D-7 onto the closed child-env allowlist — a value reaches the child only through a forced row this frontend composes) |
| `STRATA_GLM_MTP_KEEP` | engine env | docs/SETTINGS.md:106 |
| `STRATA_GLM_MTP_MISS` | engine env | docs/SETTINGS.md:106 |
| `STRATA_GLM_MULTI_BENCH` | engine env | docs/SETTINGS.md:164 |
| `STRATA_GLM_MULTI_BOUNDARY` | engine env | docs/SETTINGS.md:99 |
| `STRATA_GLM_MULTI_DUMP` | engine env | src/program/generate.cpp:3681 (docs/SETTINGS.md:164 documents it as MAYA_GLM_MULTI_DUMP — an embedded `MAYA_X=value` span the pin scan missed at regeneration; typed per D-7 onto the closed child-env allowlist — a value reaches the child only through a forced row this frontend composes) |
| `STRATA_GLM_MULTI_MTP` | engine env | src/core/glm_mtp_rows.cu:93 (docs/SETTINGS.md:90 documents it as MAYA_GLM_MULTI_MTP: typed per D-7 onto the closed child-env allowlist — a value reaches the child only through a forced row this frontend composes) |
| `STRATA_GLM_MULTI_MTP_DRAFT_ROWS` | engine env | src/core/glm_mtp_rows.cu:766 (docs/SETTINGS.md:92 documents it as MAYA_GLM_MULTI_MTP_DRAFT_ROWS: typed per D-7 onto the closed child-env allowlist — a value reaches the child only through a forced row this frontend composes) |
| `STRATA_GLM_MULTI_MTP_MAX` | engine env | src/program/generate.cpp:3047 (docs/SETTINGS.md:94 documents it as MAYA_GLM_MULTI_MTP_MAX: typed per D-7 onto the closed child-env allowlist — a value reaches the child only through a forced row this frontend composes) |
| `STRATA_GLM_MULTI_MTP_ROWS` | engine env | src/core/glm_mtp_rows.cu:101 (docs/SETTINGS.md:93 documents it as MAYA_GLM_MULTI_MTP_ROWS: typed per D-7 onto the closed child-env allowlist — a value reaches the child only through a forced row this frontend composes) |
| `STRATA_GLM_MULTI_MTP_VOCAB` | engine env | src/core/glm_mtp_rows.cu:745 (docs/SETTINGS.md:95 documents it as MAYA_GLM_MULTI_MTP_VOCAB: typed per D-7 onto the closed child-env allowlist — a value reaches the child only through a forced row this frontend composes) |
| `STRATA_GLM_MULTI_ROOM` | engine env | docs/SETTINGS.md:99 |
| `STRATA_GLM_MULTI_SPEC` | engine env | docs/SETTINGS.md:92 |
| `STRATA_GLM_MV_P100` | engine env | src/kernels/cuda/glm_fast.cu:1961 (docs/SETTINGS.md:135 documents it as MAYA_GLM_MV_P100: typed per D-7 onto the closed child-env allowlist — a value reaches the child only through a forced row this frontend composes) |
| `STRATA_GLM_MV_P100_R` | engine env | src/kernels/cuda/glm_fast.cu:1962 (docs/SETTINGS.md:135 documents it as MAYA_GLM_MV_P100_R: typed per D-7 onto the closed child-env allowlist — a value reaches the child only through a forced row this frontend composes) |
| `STRATA_GLM_MV_RDNA` | engine env | docs/SETTINGS.md:104 |
| `STRATA_GLM_NO_MTP` | engine env | docs/SETTINGS.md:92 |
| `STRATA_GLM_NO_PREFILL` | engine env | src/core/glm_prefill.cu:1047 (docs/SETTINGS.md:272 documents it as MAYA_GLM_NO_PREFILL — an embedded `MAYA_X=value` span the pin scan missed at regeneration; typed per D-7 onto the closed child-env allowlist — a value reaches the child only through a forced row this frontend composes) |
| `STRATA_GLM_NO_SPEC` | engine env | docs/SETTINGS.md:111 |
| `STRATA_GLM_NUMA` | engine env | docs/SETTINGS.md:69 |
| `STRATA_GLM_PARALLEL_LOAD` | engine env | docs/SETTINGS.md:55 |
| `STRATA_GLM_PCIE_SHARE` | engine env | docs/SETTINGS.md:64 |
| `STRATA_GLM_POOL_CHECK` | engine env | src/core/glm_fast_path.cu:5956 (docs/SETTINGS.md:222 documents it as MAYA_GLM_POOL_CHECK — an embedded `MAYA_X=value` span the pin scan missed at regeneration; typed per D-7 onto the closed child-env allowlist — a value reaches the child only through a forced row this frontend composes) |
| `STRATA_GLM_POOL_STATS` | engine env | docs/SETTINGS.md:161 |
| `STRATA_GLM_PREFETCH_AT` | engine env | docs/SETTINGS.md:68 |
| `STRATA_GLM_PREFETCH_BLOCKS` | engine env | docs/SETTINGS.md:68 |
| `STRATA_GLM_PREFETCH_N` | engine env | docs/SETTINGS.md:68 |
| `STRATA_GLM_PREFETCH_RANK` | engine env | docs/SETTINGS.md:68 |
| `STRATA_GLM_PREFILL_ATTN` | engine env | docs/SETTINGS.md:134 |
| `STRATA_GLM_PREFILL_ATTN_CHECK` | engine env | docs/SETTINGS.md:137 |
| `STRATA_GLM_PREFILL_CALC` | engine env | src/core/glm_prefill.cu:4355 (docs/SETTINGS.md:67 documents it as MAYA_GLM_PREFILL_CALC: typed per D-7 onto the closed child-env allowlist — a value reaches the child only through a forced row this frontend composes) |
| `STRATA_GLM_PREFILL_CALC_CHECK` | engine env | src/core/glm_prefill.cu:3894 (docs/SETTINGS.md:67 documents it as MAYA_GLM_PREFILL_CALC_CHECK — an embedded `MAYA_X=value` span the pin scan missed at regeneration; typed per D-7 onto the closed child-env allowlist — a value reaches the child only through a forced row this frontend composes) |
| `STRATA_GLM_PREFILL_CALC_SHARE` | engine env | src/core/glm_prefill.cu:4362 (docs/SETTINGS.md:67 documents it as MAYA_GLM_PREFILL_CALC_SHARE — an embedded `MAYA_X=value` span the pin scan missed at regeneration; typed per D-7 onto the closed child-env allowlist — a value reaches the child only through a forced row this frontend composes) |
| `STRATA_GLM_PREFILL_CHUNK` | engine env | docs/SETTINGS.md:71 |
| `STRATA_GLM_PREFILL_CPU` | engine env | docs/SETTINGS.md:117 |
| `STRATA_GLM_PREFILL_CPU_ROW_MS` | engine env | docs/SETTINGS.md:117 |
| `STRATA_GLM_PREFILL_FLUSH` | engine env | src/core/glm_fast_state.hpp:93 (docs/SETTINGS.md:52 documents it as MAYA_GLM_PREFILL_FLUSH: typed per D-7 onto the closed child-env allowlist — a value reaches the child only through a forced row this frontend composes) |
| `STRATA_GLM_PREFILL_FUSED` | engine env | src/prefill/moe_fused_rdna4.cu:665 (docs/SETTINGS.md:65 documents it as MAYA_GLM_PREFILL_FUSED: typed per D-7 onto the closed child-env allowlist — a value reaches the child only through a forced row this frontend composes) |
| `STRATA_GLM_PREFILL_GAPS` | engine env | src/program/generate.cpp:1153 (docs/SETTINGS.md:97 documents it as MAYA_GLM_PREFILL_GAPS: typed per D-7 onto the closed child-env allowlist — a value reaches the child only through a forced row this frontend composes) |
| `STRATA_GLM_PREFILL_GAP_CREDIT` | engine env | src/program/generate.cpp:2768 (docs/SETTINGS.md:97 documents it as MAYA_GLM_PREFILL_GAP_CREDIT: typed per D-7 onto the closed child-env allowlist — a value reaches the child only through a forced row this frontend composes) |
| `STRATA_GLM_PREFILL_INTERLEAVE` | engine env | src/program/generate.cpp:1148 (docs/SETTINGS.md:96 documents it as MAYA_GLM_PREFILL_INTERLEAVE: typed per D-7 onto the closed child-env allowlist — a value reaches the child only through a forced row this frontend composes) |
| `STRATA_GLM_PREFILL_INTERLEAVE_ALWAYS` | engine env | src/program/generate.cpp:2891 (docs/SETTINGS.md:96 documents it as MAYA_GLM_PREFILL_INTERLEAVE_ALWAYS: typed per D-7 onto the closed child-env allowlist — a value reaches the child only through a forced row this frontend composes) |
| `STRATA_GLM_PREFILL_KEEP` | engine env | src/core/glm_fast_path.cu:6105 (docs/SETTINGS.md:122 documents it as MAYA_GLM_PREFILL_KEEP: typed per D-7 onto the closed child-env allowlist — a value reaches the child only through a forced row this frontend composes) |
| `STRATA_GLM_PREFILL_MB` | engine env | docs/SETTINGS.md:72 |
| `STRATA_GLM_PREFILL_MIN_CHUNKS` | engine env | src/core/glm_prefill.cu:4189 (docs/SETTINGS.md:97 documents it as MAYA_GLM_PREFILL_MIN_CHUNKS: typed per D-7 onto the closed child-env allowlist — a value reaches the child only through a forced row this frontend composes) |
| `STRATA_GLM_PREFILL_STAGES` | engine env | src/core/glm_prefill.cu:1405 (docs/SETTINGS.md:121 documents it as MAYA_GLM_PREFILL_STAGES: typed per D-7 onto the closed child-env allowlist — a value reaches the child only through a forced row this frontend composes) |
| `STRATA_GLM_PREFILL_SUB` | engine env | docs/SETTINGS.md:128 |
| `STRATA_GLM_PREFILL_TAIL_SKIP` | engine env | docs/SETTINGS.md:74 |
| `STRATA_GLM_PREFILL_TRACE` | engine env | docs/SETTINGS.md:120 |
| `STRATA_GLM_PREFILL_WINDOW` | engine env | docs/SETTINGS.md:75 |
| `STRATA_GLM_PREFIX_CUT` | engine env | src/program/generate.cpp:1814 (docs/SETTINGS.md:80 documents it as MAYA_GLM_PREFIX_CUT: typed per D-7 onto the closed child-env allowlist — a value reaches the child only through a forced row this frontend composes) |
| `STRATA_GLM_PRESTAGE` | engine env | docs/SETTINGS.md:118 |
| `STRATA_GLM_PRESTAGE_ADAPT` | engine env | docs/SETTINGS.md:118 |
| `STRATA_GLM_PROFILE_WEIGHT` | engine env | docs/SETTINGS.md:144 |
| `STRATA_GLM_PROMOTE` | engine env | docs/SETTINGS.md:100 |
| `STRATA_GLM_PROMOTE_FILL` | engine env | docs/SETTINGS.md:102 |
| `STRATA_GLM_PROMOTE_MIN` | engine env | docs/SETTINGS.md:101 |
| `STRATA_GLM_RAM_GB` | engine env | docs/SETTINGS.md:49 |
| `STRATA_GLM_RAM_HEADROOM_GB` | engine env | docs/SETTINGS.md:48 (typed value source glm.ram_headroom_gb, 6..1024 default 16 per the SP6 §4.9 sheet M2; always projected by glm_environment_knobs) |
| `STRATA_GLM_RAM_RESIDENT` | engine env | docs/SETTINGS.md:53 |
| `STRATA_GLM_RAM_SLACK` | engine env | docs/SETTINGS.md:53 |
| `STRATA_GLM_READ_CHUNKS` | engine env | docs/SETTINGS.md:70 |
| `STRATA_GLM_RESERVE_LATER_MB` | engine env | src/core/glm_fast_path.cu:1178 (docs/SETTINGS.md:218 documents it as MAYA_GLM_RESERVE_LATER_MB: typed per D-7 onto the closed child-env allowlist — a value reaches the child only through a forced row this frontend composes) |
| `STRATA_GLM_RESERVE_MB` | engine env | maya.py:2252 (value from hip_reserve_mib, :642-648; the HIP calibration writes it into the config env); docs/SETTINGS.md:218-193 now documents MAYA_GLM_RESERVE_MB; the calibration flow is docs/SETTINGS.md:122 |
| `STRATA_GLM_RESIDENT_SKIP` | engine env | src/core/glm_fast_state.hpp:544 (docs/SETTINGS.md:51 documents it as MAYA_GLM_RESIDENT_SKIP: typed per D-7 onto the closed child-env allowlist — a value reaches the child only through a forced row this frontend composes) |
| `STRATA_GLM_ROWS_ATTN` | engine env | src/core/glm_rows.cu:68 (docs/SETTINGS.md:90 documents it as MAYA_GLM_ROWS_ATTN: typed per D-7 onto the closed child-env allowlist — a value reaches the child only through a forced row this frontend composes) |
| `STRATA_GLM_ROWS_BATCH` | engine env | src/core/glm_rows.cu:54 (docs/SETTINGS.md:89 documents it as MAYA_GLM_ROWS_BATCH: typed per D-7 onto the closed child-env allowlist — a value reaches the child only through a forced row this frontend composes) |
| `STRATA_GLM_ROWS_SMALL` | engine env | src/core/glm_rows.cu:61 (docs/SETTINGS.md:91 documents it as MAYA_GLM_ROWS_SMALL: typed per D-7 onto the closed child-env allowlist — a value reaches the child only through a forced row this frontend composes) |
| `STRATA_GLM_SCORE` | engine env | docs/SETTINGS.md:163 |
| `STRATA_GLM_SCORE_PREFILL` | engine env | docs/SETTINGS.md:163 |
| `STRATA_GLM_SEQS` | engine env | docs/SETTINGS.md:84 |
| `STRATA_GLM_SEQ_CTX` | engine env | docs/SETTINGS.md:84 |
| `STRATA_GLM_SERVICE_IDLE_MS` | engine env | docs/SETTINGS.md:116 |
| `STRATA_GLM_SKIP_EMPTY` | engine env | src/core/glm_memory.hpp:41 (docs/SETTINGS.md:75 documents it as MAYA_GLM_SKIP_EMPTY: typed per D-7 onto the closed child-env allowlist — a value reaches the child only through a forced row this frontend composes) |
| `STRATA_GLM_SLOTS` | engine env | docs/SETTINGS.md:79 (SP6 §4.9: slots off is the forced default — the table may move it, nothing unknown is ever sent; Task 4) |
| `STRATA_GLM_SLOT_DAYS` | engine env | docs/SETTINGS.md:83 |
| `STRATA_GLM_SLOT_GB` | engine env | docs/SETTINGS.md:82 |
| `STRATA_GLM_SLOT_KEEP` | engine env | docs/SETTINGS.md:80 |
| `STRATA_GLM_SLOT_MIN` | engine env | docs/SETTINGS.md:80 |
| `STRATA_GLM_SPEC_CANCEL` | engine env | docs/SETTINGS.md:112 |
| `STRATA_GLM_SPEC_FOLLOW` | engine env | src/program/generate.cpp:1028 (docs/SETTINGS.md:114 documents it as MAYA_GLM_SPEC_FOLLOW: typed per D-7 onto the closed child-env allowlist — a value reaches the child only through a forced row this frontend composes) |
| `STRATA_GLM_SPEC_FOLLOW_SAVE` | engine env | src/program/generate.cpp:1029 (docs/SETTINGS.md:115 documents it as MAYA_GLM_SPEC_FOLLOW_SAVE: typed per D-7 onto the closed child-env allowlist — a value reaches the child only through a forced row this frontend composes) |
| `STRATA_GLM_SPEC_HEAD` | engine env | docs/SETTINGS.md:111 |
| `STRATA_GLM_SPEC_SAMPLE` | engine env | docs/SETTINGS.md:112 |
| `STRATA_GLM_SPLIT` | engine env | docs/SETTINGS.md:54 (SP6 §4.9: the forced split; glm.layer_split also renders --layer-split, the engine getenvs the name at src/core/glm_model.cu:1953; Task 4/7) |
| `STRATA_GLM_SPLIT_LAYER_MS` | engine env | docs/SETTINGS.md:54 |
| `STRATA_GLM_SPLIT_LOG` | engine env | docs/SETTINGS.md:54 |
| `STRATA_GLM_SPLIT_ROWS` | engine env | src/core/glm_rows.cu:79 (docs/SETTINGS.md:85 documents it as MAYA_GLM_SPLIT_ROWS: typed per D-7 onto the closed child-env allowlist — a value reaches the child only through a forced row this frontend composes) |
| `STRATA_GLM_THINK_RESERVE` | typed config | docs/SETTINGS.md:165, serve/server.py:2576 (server-side reserve; rides glm.thinking_budget — C-3; Task 3) |
| `STRATA_GLM_TIER_DIAG` | engine env | src/core/glm_fast_path.cu:5096 (v1.1.27 documents it as MAYA_GLM_TIER_DIAG at docs/SETTINGS.md:73: was the undocumented internal of v1.1.21, typed per D-7 onto the closed child-env allowlist — a value reaches the child only through a forced row this frontend composes) |
| `STRATA_GLM_TIER_DIAG_EVERY` | engine env | src/core/glm_fast_path.cu:5097 (docs/SETTINGS.md:73 documents it as MAYA_GLM_TIER_DIAG_EVERY: typed per D-7 onto the closed child-env allowlist — a value reaches the child only through a forced row this frontend composes) |
| `STRATA_GLM_TIMING` | engine env | docs/SETTINGS.md:161 |
| `STRATA_GLM_TRUNK_KEEP` | engine env | docs/SETTINGS.md:108 |
| `STRATA_GLM_TRUNK_KEEP_MAP` | engine env | docs/SETTINGS.md:108 |
| `STRATA_GLM_VISION_LEND_MB` | engine env | serve/server.py:4403 (the vision turn lends MB from the expert pool; the flow is docs/SETTINGS.md:29) |
| `STRATA_GLM_VRAM_EVICT` | engine env | docs/SETTINGS.md:103 |
| `STRATA_GLM_WARM_PROMPT` | engine env | src/program/generate.cpp:3824 (docs/SETTINGS.md:124 documents it as MAYA_GLM_WARM_PROMPT: typed per D-7 onto the closed child-env allowlist — a value reaches the child only through a forced row this frontend composes) |
| `STRATA_GLM_WIN_ALLCPU` | engine env | docs/SETTINGS.md:154 |
| `STRATA_GLM_WIN_BATCH` | engine env | src/core/glm_window.cu:585 (docs/SETTINGS.md:131 documents it as MAYA_GLM_WIN_BATCH: typed per D-7 onto the closed child-env allowlist — a value reaches the child only through a forced row this frontend composes) |
| `STRATA_GLM_WIN_COMMIT_WAIT` | engine env | src/core/glm_dflash.cu:2007 (docs/SETTINGS.md:128 documents it as MAYA_GLM_WIN_COMMIT_WAIT: typed per D-7 onto the closed child-env allowlist — a value reaches the child only through a forced row this frontend composes) |
| `STRATA_GLM_WIN_CPU` | engine env | docs/SETTINGS.md:154 |
| `STRATA_GLM_WIN_LANES` | engine env | docs/SETTINGS.md:154 |
| `STRATA_GLM_WIN_MULTI` | engine env | src/core/glm_window.cu:593 (docs/SETTINGS.md:132 documents it as MAYA_GLM_WIN_MULTI: typed per D-7 onto the closed child-env allowlist — a value reaches the child only through a forced row this frontend composes) |
| `STRATA_GLM_WIN_PIPE` | engine env | docs/SETTINGS.md:146 |
| `STRATA_GLM_WIN_REPLAY` | engine env | src/core/glm_window.cu:1753 (docs/SETTINGS.md:129 documents it as MAYA_GLM_WIN_REPLAY: typed per D-7 onto the closed child-env allowlist — a value reaches the child only through a forced row this frontend composes) |
| `STRATA_GLM_WIN_SKIP` | engine env | docs/SETTINGS.md:147 |
| `STRATA_GLM_WIN_STAGGER` | engine env | docs/SETTINGS.md:154 |
| `STRATA_GLM_WIN_THREADS` | engine env | docs/SETTINGS.md:147 |
| `STRATA_GLM_WIN_X3` | engine env | docs/SETTINGS.md:157 |
| `STRATA_GLM_WIN_X3_POOLS` | engine env | docs/SETTINGS.md:157 |
| `STRATA_MAX_BODY_MIB` | frontend behaviour | already typed: limits.chat.max_body_bytes (SP1); serve/server.py:3221, docs/SETTINGS.md:168 |
| `STRATA_NO_IQ512` | engine env | src/kernels/cpu/native_expert.cpp:113 (docs/SETTINGS.md:127 documents it as MAYA_NO_IQ512 — an embedded `MAYA_X=value` span the pin scan missed at regeneration; typed per D-7 onto the closed child-env allowlist — a value reaches the child only through a forced row this frontend composes) |
| `STRATA_PF_WMMA` | engine env | src/core/glm_prefill.cu:189 (docs/SETTINGS.md:77 documents it as MAYA_PF_WMMA: typed per D-7 onto the closed child-env allowlist — a value reaches the child only through a forced row this frontend composes) |
| `STRATA_PREFILL_LEND_PCT` | engine env | src/core/glm_prefill.cu:1161 (docs/SETTINGS.md:72 documents it as MAYA_PREFILL_LEND_PCT: typed per D-7 onto the closed child-env allowlist — a value reaches the child only through a forced row this frontend composes) |
| `STRATA_SPLIT_COVER_B` | engine env | src/core/glm_model.cu:1675 (docs/SETTINGS.md:54 documents it as MAYA_SPLIT_COVER_B: typed per D-7 onto the closed child-env allowlist — a value reaches the child only through a forced row this frontend composes) |
| `STRATA_VIDEO_MAX_FRAMES` | engine env | serve/server.py:1366 (docs/SETTINGS.md:172 documents it as MAYA_VIDEO_MAX_FRAMES: typed per D-7 onto the closed child-env allowlist — a value reaches the child only through a forced row this frontend composes) |
| `STRATA_WATCHDOG_S` | engine env | docs/SETTINGS.md:143, src/program/generate.cpp:747 (engine-side watchdog; SP6 §4.9/M5 forces 0 pending the owner's sheet; Task 4) |
| `--expert-profile-save` | engine argv | maya.py:2296 |
| `--expert-profile-save-every` | engine argv | maya.py:2296 |
| `--kv-resident` | engine argv | maya.py:2297 |
| `--kv-streaming` | engine argv | maya.py:106 |
| `--max-context` | engine argv | serve/server.py:588 |
| `--parallel` | engine argv | maya.py:2296 |
| `--prefill` | engine argv | maya.py:2240 |
<!-- maya-typed:end -->

### Mapped to a Strata mechanism instead

<!-- maya-mapped:begin -->
| Maya option | Destination | Disposition note and the MAYA v1.1.27 read site |
| --- | --- | --- |
| `api_key` | SP1 token store is the only auth source (CFG-06); serve/server.py:4474, maya.py:2856 |
| `backend` | engine identity: the SP4 manifest + engine gate; serve/server.py:1492, maya.py:2817 |
| `STRATA_API_KEY` | SP1 token store is the only auth source (CFG-06); serve/server.py:4337 |
| `STRATA_ENGINE_QUIT_S` | the SP2 shutdown order (§3.7) is fixed by the supervisor; serve/server.py:1145, docs/SETTINGS.md:169 |
| `STRATA_ENGINE_WRAP_S` | the SP3 shutdown drain is fixed in the frontend (no per-request wrap-up); serve/server.py:4552, docs/SETTINGS.md:170 |
| `STRATA_FAIR_SLICE_S` | SP3 admission/pool scheduling (B8/B9) is the frontend's policy; serve/server.py:288, docs/SETTINGS.md:171 |
| `STRATA_HTTP_BACKLOG` | the listener backlog is fixed by the SP3 poll design (128 entries); serve/server.py:3212, docs/SETTINGS.md:168 |
| `STRATA_TOKENIZER` | serve/test_detok.py:21 (the SP4 manifest carries the tokenizer (the T2-c pattern); Maya passes it to its own tools — the engine reads no STRATA_TOKENIZER) |
| `--bench` | maya.py:4045 (the engine's own argv; the SP4 manifest-composed launch at v1.1.21 is SP5-P2 Task 6) |
| `--check` | maya.py:3984 (the engine's own argv; the SP4 manifest-composed launch at v1.1.21 is SP5-P2 Task 6) |
| `--cpu-threads` | maya.py:3838 (the engine's own argv; the SP4 manifest-composed launch at v1.1.21 is SP5-P2 Task 6) |
| `--dir` | maya.py:3553 (the engine's own argv; the SP4 manifest-composed launch at v1.1.21 is SP5-P2 Task 6) |
| `--expert-cache` | maya.py:2239 (the engine's own argv; SP5-P2 Task 6 landed: the glm53 composed base renders --expert-cache auto at 451f9cb6) |
| `--expert-profile` | maya.py:2291 |
| `--gguf` | maya.py:2075 (the engine's own argv; the SP4 manifest-composed launch at v1.1.21 is SP5-P2 Task 6) |
| `--glm-pack` | serve/server.py:4487 |
| `--glm-ram-resident` | docs/SETTINGS.md:53 (the engine's own argv; the SP4 manifest-composed launch at v1.1.21 is SP5-P2 Task 6) |
| `--gpus` | maya.py:2996 (the engine's own argv; the SP4 manifest-composed launch at v1.1.21 is SP5-P2 Task 6) |
| `--layer-split` | serve/server.py:1483 (the engine's own argv; the SP4 manifest-composed launch at v1.1.21 is SP5-P2 Task 6) |
| `--mmap-experts` | maya.py:3841 (the engine's own argv: --mmap-experts auto|on|off kept across setup by the config's args list, declared :4027; the SP4 manifest-composed launch at v1.1.27 is SP5-P2 Task 6) |
| `--native` | serve/server.py:445 (the engine's own argv; the SP4 manifest-composed launch at v1.1.21 is SP5-P2 Task 6) |
| `--out` | maya.py:2075 (the engine's own argv; the SP4 manifest-composed launch at v1.1.21 is SP5-P2 Task 6) |
| `--pack` | serve/server.py:508 (the engine's own argv; the SP4 manifest-composed launch at v1.1.21 is SP5-P2 Task 6) |
| `--prefill-chunk` | maya.py:3837 (the engine's own argv; the SP4 manifest-composed launch at v1.1.21 is SP5-P2 Task 6) |
| `--reserve-mib` | maya.py:3837 (the engine's own argv; the SP4 manifest-composed launch at v1.1.21 is SP5-P2 Task 6) |
| `--serve` | serve/server.py:517 |
| `--threads` | serve/server.py:1188 (the engine's own argv; the SP4 manifest-composed launch at v1.1.21 is SP5-P2 Task 6) |
| `--tokens` | docs/SETTINGS.md:163 (the engine's own argv; the SP4 manifest-composed launch at v1.1.21 is SP5-P2 Task 6) |
| `--tokens-file` | docs/SETTINGS.md:114 (the engine's own argv; the SP4 manifest-composed launch at v1.1.21 is SP5-P2 Task 6) |
| `--tp` | maya.py:3834 (the engine's own argv; the SP4 manifest-composed launch at v1.1.21 is SP5-P2 Task 6) |
| `--vision` | maya.py:3803 (the engine's own argv; SP5-P2 Task 6 truth: the launch composition emits it when vision.enabled, and maya serve reads it at src/program/generate.cpp:4419, :430 at 451f9cb6) |
| `--vram-experts` | maya.py:3837 (the engine's own argv; the SP4 manifest-composed launch at v1.1.21 is SP5-P2 Task 6) |
| `--vram-reserve-later-mib` | maya.py:2295 (the engine's own argv; the SP4 manifest-composed launch at v1.1.21 is SP5-P2 Task 6) |
| `--vram-reserve-mib` | maya.py:2295 (the engine's own argv; the SP4 manifest-composed launch at v1.1.21 is SP5-P2 Task 6) |
<!-- maya-mapped:end -->

### Excluded keys — refused at config load

<!-- maya-rejected:begin -->
| Maya option | Excluded by (and the MAYA v1.1.21 read site) |
| --- | --- |
| `args` | spec §4 free-form process control (raw engine argv); serve/server.py:4486, engine_args :1274-1277 (Task 7 retires the glm53 pass-through) |
| `command` | spec §4 MCP command; serve/mcp.py:604 |
| `cwd` | spec §4 free-form process control; serve/server.py:4035, serve/mcp.py:104 |
| `env` | spec §4 raw env dict (Maya carries engine settings as raw env with no allowlist — the closed typed table replaces it); serve/server.py:1497, :3729 |
| `exe` | spec §4 free-form process control (engine identity from the SP4 manifest); serve/server.py:4412, maya.py:2822 |
| `headers` | spec §4 MCP headers; serve/mcp.py:251, :601 |
| `installer` | spec §4 PRIV-01 forbids update checks and self-update (the Maya updater block, serve/update.py); maya.py:2443, :2541, :2560; the block's sub-keys (models_dir, quant, gguf, draft, data_dir, speed_calibration) ride the parent reject |
| `lib_dirs` | spec §4 free-form process control (library search paths); serve/server.py:1499 |
| `log` | spec §4 free-form process control (the child's log path); serve/server.py:4355, :4035, maya.py:2519 |
| `mcp` | spec §4 MCP; serve/mcp.py:626 |
| `mcp_servers` | spec §4 MCP; serve/mcp.py:645 |
| `type` | spec §4 MCP entry type; serve/mcp.py:598 |
| `url` | spec §4 MCP url; serve/mcp.py:378, :597 |
| `mcpServers` | spec §4 MCP (Claude Desktop block); serve/mcp.py:643, serve/server.py:3970 (--mcp-config) |
| `MAYA_GLM_DFLASH` | spec C-5: the DFlash2 drafter and its knobs stay rejected on the A7 licence ground (the drafter weights are CC BY-NC-ND 4.0); docs/SETTINGS.md:347 |
| `MAYA_GLM_DFLASH_AHEAD` | spec C-5: the DFlash2 drafter and its knobs stay rejected on the A7 licence ground (the drafter weights are CC BY-NC-ND 4.0); docs/SETTINGS.md:150 |
| `MAYA_GLM_DFLASH_HEAD_SPLIT` | spec C-5: the DFlash2 drafter and its knobs stay rejected on the A7 licence ground (the drafter weights are CC BY-NC-ND 4.0); docs/SETTINGS.md:149 |
| `MAYA_GLM_DFLASH_INJ_OVERLAP` | spec C-5: the DFlash2 drafter and its knobs stay rejected on the A7 licence ground (the drafter weights are CC BY-NC-ND 4.0); docs/SETTINGS.md:151 |
| `MAYA_GLM_DFLASH_MIN` | spec C-5: the DFlash2 drafter and its knobs stay rejected on the A7 licence ground (the drafter weights are CC BY-NC-ND 4.0); docs/SETTINGS.md:353 |
| `MAYA_GLM_DFLASH_PRIO` | spec C-5: the DFlash2 drafter and its knobs stay rejected on the A7 licence ground (the drafter weights are CC BY-NC-ND 4.0); docs/SETTINGS.md:160 |
| `MAYA_GLM_DFLASH_PROF` | spec C-5: the DFlash2 drafter and its knobs stay rejected on the A7 licence ground (the drafter weights are CC BY-NC-ND 4.0); docs/SETTINGS.md:158 |
| `MAYA_GLM_DFLASH_STATS` | spec C-5: the DFlash2 drafter and its knobs stay rejected on the A7 licence ground (the drafter weights are CC BY-NC-ND 4.0); docs/SETTINGS.md:158 |
| `MAYA_GLM_DFLASH_TL` | spec C-5: the DFlash2 drafter and its knobs stay rejected on the A7 licence ground (the drafter weights are CC BY-NC-ND 4.0); docs/SETTINGS.md:159 |
| `MAYA_RESTART_ON_UPDATE` | spec §4 PRIV-01 forbids the self-updater restart; maya.py:2874 |
| `MAYA_UPDATE_CHECK` | spec §4 PRIV-01 forbids update checks (the Maya self-updater block); serve/update.py:75 |
| `STRATA_DEBUG` | spec §4 no new diagnostics surface (debug logging switch); serve/server.py:2727, :2465 |
| `STRATA_GLM_CPU_CAL` | spec §4 free-form process control (the `log` rule: a timing cache in an operator-named file); docs/SETTINGS.md:61 |
| `STRATA_GLM_DFLASH` | spec C-5: the DFlash2 drafter and its knobs stay rejected on the A7 licence ground (the drafter weights are CC BY-NC-ND 4.0, docs/SETTINGS.md:347) — not a knob-visibility call; docs/SETTINGS.md:347 |
| `STRATA_GLM_DFLASH_AHEAD` | spec C-5: the DFlash2 drafter and its knobs stay rejected on the A7 licence ground (the drafter weights are CC BY-NC-ND 4.0); src/core/glm_dflash.cu:1820 |
| `STRATA_GLM_DFLASH_ATTN` | spec C-5: the DFlash2 drafter and its knobs stay rejected on the A7 licence ground (the drafter weights are CC BY-NC-ND 4.0); src/core/glm_dflash.cu:1250 |
| `STRATA_GLM_DFLASH_CTX` | spec C-5: the DFlash2 drafter and its knobs stay rejected on the A7 licence ground (the drafter weights are CC BY-NC-ND 4.0); src/core/glm_dflash.cu:844 |
| `STRATA_GLM_DFLASH_CUM` | spec C-5: the DFlash2 drafter and its knobs stay rejected on the A7 licence ground (the drafter weights are CC BY-NC-ND 4.0); src/core/glm_dflash.cu:1795 |
| `STRATA_GLM_DFLASH_DUMP` | spec C-5: the DFlash2 drafter and its knobs stay rejected on the A7 licence ground (the drafter weights are CC BY-NC-ND 4.0); src/core/glm_dflash.cu:590 |
| `STRATA_GLM_DFLASH_EXPLORE` | spec C-5: the DFlash2 drafter and its knobs stay rejected on the A7 licence ground (the drafter weights are CC BY-NC-ND 4.0); src/core/glm_dflash.cu:1774 |
| `STRATA_GLM_DFLASH_HEAD_MMA` | spec C-5: the DFlash2 drafter and its knobs stay rejected on the A7 licence ground (the drafter weights are CC BY-NC-ND 4.0); src/core/glm_dflash.cu:1356 |
| `STRATA_GLM_DFLASH_HEAD_SPLIT` | spec C-5: the DFlash2 drafter and its knobs stay rejected on the A7 licence ground (the drafter weights are CC BY-NC-ND 4.0); src/core/glm_dflash.cu:1463 |
| `STRATA_GLM_DFLASH_INJ_OVERLAP` | spec C-5: the DFlash2 drafter and its knobs stay rejected on the A7 licence ground (the drafter weights are CC BY-NC-ND 4.0); src/core/glm_dflash.cu:1124 |
| `STRATA_GLM_DFLASH_MIN` | spec C-5: the DFlash2 drafter and its knobs stay rejected on the A7 licence ground (the drafter weights are CC BY-NC-ND 4.0, docs/SETTINGS.md:353) — not a knob-visibility call; docs/SETTINGS.md:353 |
| `STRATA_GLM_DFLASH_PRIO` | spec C-5: the DFlash2 drafter and its knobs stay rejected on the A7 licence ground (the drafter weights are CC BY-NC-ND 4.0); src/core/glm_dflash.cu:733 |
| `STRATA_GLM_DFLASH_PROF` | spec C-5: the DFlash2 drafter and its knobs stay rejected on the A7 licence ground (the drafter weights are CC BY-NC-ND 4.0, docs/SETTINGS.md:158) — not a knob-visibility call; docs/SETTINGS.md:158 |
| `STRATA_GLM_DFLASH_STATS` | spec C-5: the DFlash2 drafter and its knobs stay rejected on the A7 licence ground (the drafter weights are CC BY-NC-ND 4.0, docs/SETTINGS.md:158) — not a knob-visibility call; docs/SETTINGS.md:158 |
| `STRATA_GLM_DFLASH_TL` | spec C-5: the DFlash2 drafter and its knobs stay rejected on the A7 licence ground (the drafter weights are CC BY-NC-ND 4.0); src/core/glm_dflash.cu:1721 |
| `STRATA_GLM_MTP_GGUF` | spec §4 model files come from the SP4 manifest (a draft GGUF picked by free path); docs/SETTINGS.md:105 |
| `STRATA_GLM_ROUTE_LOG` | spec §4 free-form process control (the `log` rule: every route written to an operator-named prefix); docs/SETTINGS.md:162 |
| `STRATA_GLM_SLOT_DIR` | spec §4 free-form process control (the `cwd` rule: the slot folder moves off the pack root); docs/SETTINGS.md:82 |
| `STRATA_GLM_USAGE` | spec §4 free-form process control (the `log` rule: an operator-named file the engine writes after every request); docs/SETTINGS.md:78 |
| `STRATA_MOCK_RELOAD_S` | spec §4 the mock engine is excluded (serve/server.py:1800 --engine mock); serve/server.py:1800 |
| `--dflash` | spec C-5: DFlash2 drafter rejected on the A7 licence ground (CC BY-NC-ND weights, docs/SETTINGS.md:347); engine-parsed at src/program/generate.cpp:4415 |
| `--engine` | serve/server.py:4315 |
| `--mcp-config` | spec §4 MCP (Maya's own MCP loader flag); serve/server.py:4340 |
<!-- maya-rejected:end -->

### Out of scope — never reachable in a managed launch

The engine reads each of these via `getenv`, but none is part of the parity surface we ship: the DeepSeek DS4 path is out of scope (plan 2026-10-22-sp5-p2 §Non-goals); a `MAYA_*` row is the SETTINGS.md spelling of the `STRATA_*` row that carries the knob (`include/strata/platform/env_alias.hpp` maps it at engine start); and an engine-internal switch is undocumented — the closed child-env allowlist never hands its name to the child. Every row still carries its citation and disposition.

<!-- maya-out:begin -->
| Maya knob | Read site and why it never renders |
| --- | --- |
| `DS4_AHEAD` | src/ds4/ds4_model.cu:1184 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_AHEAD_EXTRA` | src/ds4/ds4_model.cu:1185 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_AHEAD_ORDER` | src/ds4/ds4_model.cu:1187 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_APU_DEDICATED_GB` | src/ds4/ds4_model.cu:108 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_AR_DMA` | src/ds4/ds4_model.cu:9439 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_AR_F16` | src/ds4/ds4_kernels.cu:3492 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_AR_HOST` | src/ds4/ds4_model.cu:2863 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_AR_HOST_PIECES` | src/ds4/ds4_kernels.cu:3616 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_AR_HOST_PIN` | src/ds4/ds4_kernels.cu:3740 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_AR_HOST_STATS` | src/ds4/ds4_kernels.cu:3760 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_AR_HOST_THREADS` | src/ds4/ds4_model.cu:2866 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_AR_OVERLAP` | src/ds4/ds4_model.cu:9659 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_AR_ZC` | src/ds4/ds4_kernels.cu:3532 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_ASYNC_T` | src/ds4/ds4_model.cu:1161 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_ATTN_MMA` | src/ds4/ds4_kernels.cu:3294 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_ATTN_SLAB_MB` | src/ds4/ds4_model.cu:3388 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_ATTN_V7` | src/ds4/ds4_kernels.cu:3248 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_ATTN_WMMA` | src/ds4/ds4_kernels.cu:3271 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_BENCH_REPEAT` | src/ds4/ds4_main.cpp:1301 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_BORROW` | src/ds4/ds4_model.cu:3331 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_CACHE_BONUS` | src/ds4/ds4_model.cu:1208 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_CACHE_DECAY` | src/ds4/ds4_model.cu:1209 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_CACHE_INFLIGHT` | src/ds4/ds4_model.cu:1210 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_CACHE_MARGIN` | src/ds4/ds4_model.cu:1208 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_CACHE_THREADS` | src/ds4/ds4_model.cu:3242 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_CHECK_LEND` | src/ds4/ds4_model.cu:10692 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_CHECK_SWAPS` | src/ds4/ds4_model.cu:1461 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_CHUNK` | src/ds4/ds4_model.cu:2481 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_CHUNK_MIN` | src/ds4/ds4_model.cu:2484 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_COMMIT_CAP_GB` | src/ds4/ds4_model.cu:1371 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_COPY_GB` | src/ds4/ds4_model.cu:5220 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_COPY_IDLE` | src/ds4/ds4_model.cu:3230 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_COPY_NUMA` | src/ds4/ds4_model.cu:5234 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_CPU_STATS` | src/ds4/ds4_model.cu:6448 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_DEBUG_DROP_BEFORE_REQ` | src/ds4/ds4_model.cu:1272 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_DSPARK_CONF` | src/ds4/ds4_model.cu:1021 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_DSPARK_DUMP` | src/ds4/ds4_model.cu:1048 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_DSPARK_MAX` | src/ds4/ds4_model.cu:1047 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_DSPARK_QTEMP` | src/ds4/ds4_model.cu:1046 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_DSPARK_SAMPLE` | src/ds4/ds4_model.cu:1045 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_DUMP_H` | src/ds4/ds4_model.cu:1464 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_DUMP_L` | src/ds4/ds4_model.cu:1467 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_DUMP_LAST` | src/ds4/ds4_main.cpp:1332 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_DUMP_R` | src/ds4/ds4_model.cu:1472 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_DUMP_Y` | src/ds4/ds4_model.cu:1476 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_EG_CHECK` | src/ds4/ds4_engram_io.hpp:282 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_EG_DIRECT` | src/ds4/ds4_model.cu:2987 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_EG_SHARE` | src/ds4/ds4_model.cu:10827 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_ENGRAM_CACHE_MB` | src/ds4/ds4_model.cu:2977 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_ENGRAM_RAM` | src/ds4/ds4_model.cu:3002 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_ENGRAM_THREADS` | src/ds4/ds4_model.cu:2974 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_FAT_PIECE` | src/ds4/ds4_model.cu:3518 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_FAT_ROWS` | src/ds4/ds4_model.cu:3502 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_FLOW` | src/ds4/ds4_model.cu:3129 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_FLOW_BUFFERED` | src/ds4/ds4_model.cu:10068 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_FLOW_PUMP_WAIT` | src/ds4/ds4_model.cu:9752 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_FLOW_STATS` | src/ds4/ds4_model.cu:11060 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_FLOW_TP` | src/ds4/ds4_model.cu:3133 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_HC_STREAM` | src/ds4/ds4_model.cu:1176 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_HIP_ATTN_SWIZZLE` | src/ds4/ds4_kernels.cu:3370 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_HOST_FREE` | src/ds4/ds4_model.cu:1165 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_INDEX_CHECK` | src/ds4/ds4_model.cu:1057 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_INDEX_TC` | src/ds4/ds4_kernels.cu:2877 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_IQ_AHEAD` | src/ds4/ds4_iq.cu:1090 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_IQ_AVX2` | src/ds4/ds4_iq_cpu.cpp:490 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_IQ_GATHER` | src/ds4/ds4_iq_cpu.cpp:640 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_IQ_OWN` | src/ds4/ds4_iq_cpu.cpp:636 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_IQ_PF_ALL` | src/ds4/ds4_model.cu:6587 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_IQ_Q8K` | src/ds4/ds4_gguf.cu:290 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_IQ_RP` | src/ds4/ds4_iq.cu:1089 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_IQ_VERBOSE` | src/ds4/ds4_iq_cpu.cpp:676 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_LANE_CAL` | src/ds4/ds4_model.cu:8144 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_LANE_CHECK` | src/ds4/ds4_model.cu:8842 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_LANE_FREE` | src/ds4/ds4_model.cu:5597 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_LAST_WINDOW` | src/ds4/ds4_main.cpp:871 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_LAYER_CACHE_MB` | src/ds4/ds4_model.cu:10571 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_LAYER_MAJOR` | src/ds4/ds4_model.cu:10548 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_LT` | src/ds4/ds4_lt.cu:83 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_LT_STORE` | src/ds4/ds4_model.cu:2332 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_LT_VERBOSE` | src/ds4/ds4_lt.cu:146 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_MK_SPLIT` | src/ds4/ds4_model.cu:2902 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_MOE_TOKEN` | src/ds4/ds4_model.cu:1160 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_MX_PF` | src/ds4/ds4_gguf_cpu.cpp:322 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_MX_TIER` | src/ds4/ds4_gguf_cpu.cpp:267 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_PATH_PICK` | src/ds4/ds4_model.cu:10272 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_PF_ALL` | src/ds4/ds4_os.hpp:321 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_PLACE_DEBUG` | src/ds4/ds4_model.cu:4773 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_PLACE_UNIFORM` | src/ds4/ds4_model.cu:4675 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_PREFILL_STATS` | src/ds4/ds4_model.cu:3718 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_PROF` | src/ds4/ds4_model.cu:1486 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_PROFILE` | src/ds4/ds4_main.cpp:1348 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_PROFILE_PROMPT` | src/ds4/ds4_main.cpp:1313 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_Q8_BATCH` | src/ds4/ds4_gguf.cu:1100 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_Q8_CHECK` | src/ds4/ds4_model.cu:2376 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_READAHEAD_MB` | src/ds4/ds4_model.cu:3734 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_READERS` | src/ds4/ds4_model.cu:3127 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_RECENT` | src/ds4/ds4_model.cu:2485 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_RECENT_FILLS` | src/ds4/ds4_model.cu:1397 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_RECENT_NEED` | src/ds4/ds4_model.cu:1401 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_RECENT_WIN` | src/ds4/ds4_model.cu:1402 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_REQUIRE_ALL_RAM` | src/ds4/ds4_model.cu:5194 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_RING_MB` | src/ds4/ds4_model.cu:3436 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_ROUTER_ROWS` | src/ds4/ds4_kernels.cu:2845 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_ROUTE_TRACE` | src/ds4/ds4_model.cu:7808 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_SERVE_DUMP` | src/ds4/ds4_main.cpp:374 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_SPEC_Q` | src/ds4/ds4_model.cu:3940 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_SPEC_WASTE` | src/ds4/ds4_model.cu:3959 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_SPLIT` | src/ds4/ds4_model.cu:2607 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_STREAM` | src/ds4/ds4_model.cu:8105 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_STREAM_DIRECT` | src/ds4/ds4_model.cu:8804 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_STREAM_FORCE` | src/ds4/ds4_model.cu:8146 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_STREAM_THREADS` | src/ds4/ds4_model.cu:8706 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_STREAM_TOKEN_AB` | src/ds4/ds4_model.cu:8420 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_SYNC` | src/ds4/ds4_model.cu:1157 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_TOPK_CHECK` | src/ds4/ds4_model.cu:1053 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_TP_CHECK` | src/ds4/ds4_model.cu:3220 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_TP_GGUF_UNEVEN` | src/ds4/ds4_model.cu:2387 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_W16_MB` | src/ds4/ds4_model.cu:3389 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_WARM_GB` | src/ds4/ds4_model.cu:10753 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_WARM_MIN` | src/ds4/ds4_model.cu:10751 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_WIN_SUBMIT` | src/ds4/ds4_kernels.cu:3462 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_YOCO` | src/ds4/ds4_model.cu:10817 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `DS4_YOCO_KEEP` | src/ds4/ds4_model.cu:10893 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `MAYA_DS4_CPU_LANE` | docs/SETTINGS.md:513 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_DS4_DISK_SKIP_AUTO` | docs/SETTINGS.md:444 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_DS4_DISK_SKIP_BUDGET` | docs/SETTINGS.md:444 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_DS4_DISK_SKIP_FROM` | docs/SETTINGS.md:443 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_DS4_DISK_SKIP_W` | docs/SETTINGS.md:436 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_DS4_DSPARK_CUM` | docs/SETTINGS.md:431 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_DS4_KEEP_THINKING` | docs/SETTINGS.md:581 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_DS4_LT_STORE` | docs/SETTINGS.md:555 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_DS4_PROMPT_SKIP` | docs/SETTINGS.md:430 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_DS4_Q8_BATCH` | docs/SETTINGS.md:519 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_DS4_Q8_CHECK` | docs/SETTINGS.md:520 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_DS4_SLOT_CAP` | docs/SETTINGS.md:582 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_DS4_SLOT_DAYS` | docs/SETTINGS.md:586 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_DS4_SLOT_DIR` | docs/SETTINGS.md:584 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_DS4_SLOT_GB` | docs/SETTINGS.md:583 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_DS4_SLOT_KEEP` | docs/SETTINGS.md:580 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_DS4_SLOT_MIN` | docs/SETTINGS.md:585 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_DS4_STREAM` | docs/SETTINGS.md:412 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_DS4_TP_CHECK` | docs/SETTINGS.md:538 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_DS4_TP_GGUF_UNEVEN` | docs/SETTINGS.md:532 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_ENGINE_QUIT_S` | docs/SETTINGS.md:169 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_ENGINE_STALL_S` | docs/SETTINGS.md:166 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_ENGINE_WRAP_S` | docs/SETTINGS.md:170 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_FAIR_SLICE_S` | docs/SETTINGS.md:171 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_AHEAD` | docs/SETTINGS.md:109 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_CKPTS` | docs/SETTINGS.md:80 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_CKPT_EVERY` | docs/SETTINGS.md:81 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_COMPLETE_POOL` | docs/SETTINGS.md:50 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_CPU_1T` | docs/SETTINGS.md:126 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_CPU_CAL` | docs/SETTINGS.md:61 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_CPU_CONCURRENT` | docs/SETTINGS.md:60 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_CPU_LANE` | docs/SETTINGS.md:29 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_CPU_MT` | docs/SETTINGS.md:126 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_CPU_PIN` | docs/SETTINGS.md:58 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_CPU_PLAN` | docs/SETTINGS.md:64 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_CPU_RETIME` | docs/SETTINGS.md:65 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_CPU_SHARED` | docs/SETTINGS.md:58 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_CPU_SPLIT` | docs/SETTINGS.md:63 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_DISK_QD` | docs/SETTINGS.md:119 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_DISK_READERS` | docs/SETTINGS.md:119 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_DISK_SKIP_FROM` | docs/SETTINGS.md:109 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_DISK_SKIP_W` | docs/SETTINGS.md:109 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_DROP_CACHE` | docs/SETTINGS.md:143 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_DSA_ALL` | docs/SETTINGS.md:132 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_DSA_SCORE` | docs/SETTINGS.md:131 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_DUAL_PREFILL` | docs/SETTINGS.md:66 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_EVEN_GROUPS` | docs/SETTINGS.md:86 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_EXL3_DECODE` | docs/SETTINGS.md:140 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_EXL3_MMA_ROWS` | docs/SETTINGS.md:98 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_EXL3_MMA_ROWS_SHORT` | docs/SETTINGS.md:139 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_EXL3_ROT` | docs/SETTINGS.md:141 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_EXPERT_CACHE` | docs/SETTINGS.md:217 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_EXPERT_HELPER` | docs/SETTINGS.md:47 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_FLUSH` | docs/SETTINGS.md:52 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_GROUP_EXPERTS` | docs/SETTINGS.md:88 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_GU_STAGE` | docs/SETTINGS.md:136 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_HEAD_SPLIT` | docs/SETTINGS.md:148 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_HEAD_SPLIT_CHECK` | docs/SETTINGS.md:148 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_KDA_GRAPH` | docs/SETTINGS.md:62 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_KQ` | docs/SETTINGS.md:125 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_KV_INT8` | docs/SETTINGS.md:129 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_KV_RESIDENT` | docs/SETTINGS.md:130 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_LEND_PARK_LRU` | docs/SETTINGS.md:73 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_LOOP_ANSWER` | docs/SETTINGS.md:145 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_LOOP_THINK` | docs/SETTINGS.md:145 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_MISS_SKIP_FROM` | docs/SETTINGS.md:110 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_MISS_SKIP_W` | docs/SETTINGS.md:110 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_MLA_MMA` | docs/SETTINGS.md:133 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_MLA_V70` | docs/SETTINGS.md:134 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_MLA_V70_SPLITS` | docs/SETTINGS.md:134 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_MMQ_GLU` | docs/SETTINGS.md:142 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_MMQ_RESERVE_MB` | docs/SETTINGS.md:123 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_MTP_DECODE` | docs/SETTINGS.md:107 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_MTP_GGUF` | docs/SETTINGS.md:105 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_MTP_KEEP` | docs/SETTINGS.md:106 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_MULTI_BENCH` | docs/SETTINGS.md:164 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_MULTI_BOUNDARY` | docs/SETTINGS.md:99 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_MULTI_MTP` | docs/SETTINGS.md:90 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_MULTI_MTP_MAX` | docs/SETTINGS.md:94 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_MULTI_MTP_ROWS` | docs/SETTINGS.md:93 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_MULTI_MTP_VOCAB` | docs/SETTINGS.md:95 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_MULTI_ROOM` | docs/SETTINGS.md:99 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_MULTI_SPEC` | docs/SETTINGS.md:92 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_MV_P100` | docs/SETTINGS.md:135 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_MV_P100_R` | docs/SETTINGS.md:135 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_MV_RDNA` | docs/SETTINGS.md:104 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_NO_MTP` | docs/SETTINGS.md:92 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_NUMA` | docs/SETTINGS.md:69 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_PARALLEL_LOAD` | docs/SETTINGS.md:55 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_PCIE_SHARE` | docs/SETTINGS.md:29 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_POOL_STATS` | docs/SETTINGS.md:161 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_PREFETCH_AT` | docs/SETTINGS.md:68 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_PREFETCH_BLOCKS` | docs/SETTINGS.md:68 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_PREFETCH_N` | docs/SETTINGS.md:68 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_PREFETCH_RANK` | docs/SETTINGS.md:68 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_PREFILL_ATTN` | docs/SETTINGS.md:134 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_PREFILL_CALC` | docs/SETTINGS.md:67 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_PREFILL_CHUNK` | docs/SETTINGS.md:71 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_PREFILL_CPU` | docs/SETTINGS.md:117 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_PREFILL_CPU_ROW_MS` | docs/SETTINGS.md:117 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_PREFILL_FLUSH` | docs/SETTINGS.md:52 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_PREFILL_FUSED` | docs/SETTINGS.md:76 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_PREFILL_GAPS` | docs/SETTINGS.md:97 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_PREFILL_INTERLEAVE` | docs/SETTINGS.md:96 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_PREFILL_KEEP` | docs/SETTINGS.md:122 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_PREFILL_MB` | docs/SETTINGS.md:72 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_PREFILL_MIN_CHUNKS` | docs/SETTINGS.md:97 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_PREFILL_STAGES` | docs/SETTINGS.md:121 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_PREFILL_SUB` | docs/SETTINGS.md:128 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_PREFILL_TAIL_SKIP` | docs/SETTINGS.md:74 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_PREFILL_TRACE` | docs/SETTINGS.md:120 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_PREFILL_WINDOW` | docs/SETTINGS.md:75 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_PREFIX_CUT` | docs/SETTINGS.md:80 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_PRESTAGE` | docs/SETTINGS.md:118 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_PROFILE_WEIGHT` | docs/SETTINGS.md:144 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_PROMOTE` | docs/SETTINGS.md:100 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_PROMOTE_FILL` | docs/SETTINGS.md:102 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_PROMOTE_MIN` | docs/SETTINGS.md:101 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_RAM_GB` | serve/test_dashboard.py:116 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_RAM_HEADROOM_GB` | docs/SETTINGS.md:48 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_RAM_RESIDENT` | docs/SETTINGS.md:53 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_RAM_SLACK` | docs/SETTINGS.md:53 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_READ_CHUNKS` | docs/SETTINGS.md:29 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_RESERVE_LATER_MB` | docs/SETTINGS.md:218 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_RESERVE_MB` | docs/SETTINGS.md:218 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_RESIDENT_SKIP` | docs/SETTINGS.md:51 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_RESPONSES_CACHE_ENTRIES` | serve/test_response_store.py:93 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_ROUTE_LOG` | docs/SETTINGS.md:162 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_ROWS_ATTN` | docs/SETTINGS.md:90 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_ROWS_BATCH` | docs/SETTINGS.md:89 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_ROWS_SMALL` | docs/SETTINGS.md:91 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_SCORE_PREFILL` | docs/SETTINGS.md:163 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_SEQS` | docs/SETTINGS.md:84 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_SEQ_CTX` | docs/SETTINGS.md:84 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_SERVICE_IDLE_MS` | docs/SETTINGS.md:116 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_SKIP_EMPTY` | docs/SETTINGS.md:87 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_SLOTS` | docs/SETTINGS.md:79 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_SLOT_DAYS` | docs/SETTINGS.md:83 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_SLOT_DIR` | docs/SETTINGS.md:82 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_SLOT_GB` | docs/SETTINGS.md:82 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_SLOT_KEEP` | docs/SETTINGS.md:80 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_SLOT_MIN` | docs/SETTINGS.md:80 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_SPEC_CANCEL` | docs/SETTINGS.md:112 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_SPEC_FOLLOW` | docs/SETTINGS.md:114 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_SPEC_FOLLOW_SAVE` | docs/SETTINGS.md:115 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_SPEC_HEAD` | docs/SETTINGS.md:111 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_SPEC_SAMPLE` | docs/SETTINGS.md:112 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_SPLIT` | docs/SETTINGS.md:47 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_SPLIT_LAYER_MS` | docs/SETTINGS.md:54 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_SPLIT_ROWS` | docs/SETTINGS.md:85 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_THINK_RESERVE` | docs/SETTINGS.md:165 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_TIER_DIAG_EVERY` | docs/SETTINGS.md:73 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_TIMING` | docs/SETTINGS.md:161 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_TRUNK_KEEP` | docs/SETTINGS.md:108 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_TRUNK_KEEP_MAP` | docs/SETTINGS.md:108 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_USAGE` | docs/SETTINGS.md:27 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_VRAM_EVICT` | docs/SETTINGS.md:103 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_WARM_PROMPT` | docs/SETTINGS.md:124 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_WIN_ALLCPU` | docs/SETTINGS.md:154 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_WIN_BATCH` | docs/SETTINGS.md:155 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_WIN_COMMIT_WAIT` | docs/SETTINGS.md:152 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_WIN_CPU` | docs/SETTINGS.md:154 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_WIN_LANES` | docs/SETTINGS.md:154 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_WIN_MULTI` | docs/SETTINGS.md:156 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_WIN_PIPE` | docs/SETTINGS.md:146 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_WIN_REPLAY` | docs/SETTINGS.md:153 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_WIN_SKIP` | docs/SETTINGS.md:147 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_WIN_STAGGER` | docs/SETTINGS.md:154 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_WIN_THREADS` | docs/SETTINGS.md:147 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_WIN_X3` | docs/SETTINGS.md:157 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_GLM_WIN_X3_POOLS` | docs/SETTINGS.md:157 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_HTTP_BACKLOG` | docs/SETTINGS.md:168 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_MAX_BODY_MIB` | docs/SETTINGS.md:168 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_PACK_TEMPLATE` | serve/server.py:4447 (launcher internal: the pack-template path override (R-T2-b lineage)) |
| `MAYA_PF_WMMA` | docs/SETTINGS.md:77 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_PREFILL_LEND_PCT` | docs/SETTINGS.md:72 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_SH` | maya.py:4073 (launcher internal: the maya.sh self-detection marker (R-T2-b lineage); never an engine or server knob) |
| `MAYA_SPLIT_COVER_B` | docs/SETTINGS.md:54 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_VIDEO_MAX_FRAMES` | docs/SETTINGS.md:172 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `MAYA_WATCHDOG_S` | docs/SETTINGS.md:167 (the MAYA_ spelling of the target row: include/strata/platform/env_alias.hpp maps MAYA_X to STRATA_X at engine start — the target row carries the knob) |
| `STRATA_ARENA_LOCK` | src/core/pinned.cu:175 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_CKPT_REREAD` | src/program/generate.cpp:7221 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_COMMIT_GROWTH` | include/strata/platform/commit.hpp:33 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_CPU_YMM` | src/kernels/cpu/expert.cpp:155 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_DBG_NAN` | src/core/verify.cpp:1097 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_DECODE_TIMING` | src/program/generate.cpp:7478 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_DEC_BATCH` | src/core/verify.cpp:338 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_DRAIN_S` | serve/server.py:423 (maya's own serve/server.py: the launcher the SP4 manifest + supervision replace (T2-b); undocumented in SETTINGS.md) |
| `STRATA_DS4_CPU_LANE` | src/ds4/ds4_model.cu:5621 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_DS4_DISK_SKIP_AUTO` | src/ds4/ds4_model.cu:1268 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_DS4_DISK_SKIP_BUDGET` | src/ds4/ds4_model.cu:1271 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_DS4_DISK_SKIP_FROM` | src/ds4/ds4_model.cu:1242 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_DS4_DISK_SKIP_SETTLE` | src/ds4/ds4_model.cu:1270 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_DS4_DISK_SKIP_W` | src/ds4/ds4_model.cu:1238 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_DS4_DISK_SKIP_WINDOW` | src/ds4/ds4_model.cu:1269 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_DS4_DSPARK_CUM` | src/ds4/ds4_main.cpp:197 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_DS4_KEEP_THINKING` | serve/frontend.py:164 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_DS4_LANE_FREE` | src/ds4/ds4_model.cu:5595 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_DS4_LT_STORE` | src/ds4/ds4_model.cu:2331 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_DS4_NO_LIGHT` | src/ds4/ds4_iq.cu:1177 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_DS4_NO_TC` | src/ds4/ds4_gguf.cu:1035 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_DS4_PROMPT_SKIP` | src/ds4/ds4_model.cu:10818 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_DS4_Q8_BATCH` | src/ds4/ds4_gguf.cu:1099 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_DS4_Q8_CHECK` | src/ds4/ds4_model.cu:2376 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_DS4_RAM_TIER` | src/ds4/ds4_model.cu:1230 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_DS4_SLOT_CAP` | src/ds4/ds4_main.cpp:401 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_DS4_SLOT_DAYS` | src/ds4/ds4_main.cpp:403 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_DS4_SLOT_DIR` | src/ds4/ds4_main.cpp:677 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_DS4_SLOT_GB` | src/ds4/ds4_main.cpp:404 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_DS4_SLOT_KEEP` | src/ds4/ds4_main.cpp:400 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_DS4_SLOT_MIN` | src/ds4/ds4_main.cpp:402 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_DS4_STREAM` | src/ds4/ds4_model.cu:8106 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_DS4_TP_CHECK` | src/ds4/ds4_model.cu:3220 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_DS4_TP_GGUF_UNEVEN` | src/ds4/ds4_model.cu:2386 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_FORCE_AVX2` | src/kernels/cpu/exl3_cpu.cpp:1178 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GDN_CONV_SERIAL` | src/prefill/kernels.cu:629 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GDN_REC_HEADS` | src/prefill/kernels.cu:642 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_AHEAD_READ` | src/core/glm_fast_path.cu:1805 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_APU_DEDICATED_GB` | src/core/glm_fast_path.cu:5944 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_BOUNCE` | src/core/glm_model.cu:3577 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_CKPT_TURN` | src/program/generate.cpp:1758 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_COMMIT_CAP_GB` | src/core/glm_fast_path.cu:5996 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_CPU_COLD` | src/core/glm_fast_path.cu:3147 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_CPU_EXPERTS` | src/core/glm_model.cu:3491 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_CPU_LANE_CHECK` | src/core/glm_fast_path.cu:2886 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_CPU_LANE_VERIFY` | src/core/glm_fast_path.cu:2638 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_DEFER_CPU` | src/core/glm_fast_path.cu:5449 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_DEV1` | src/core/glm_model.cu:1995 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_DEVS` | src/core/glm_model.cu:1979 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_EMB_PROBE` | src/core/glm_model.cu:2076 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_HC1` | src/kernels/cuda/glm_fast.cu:5399 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_HC2` | src/kernels/cuda/glm_fast.cu:5400 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_KDA_SPLIT` | src/core/glm_prefill.cu:2504 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_KDA_SPLIT_CHECK` | src/core/glm_prefill.cu:2510 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_KEEP_MAP` | src/core/glm_model.cu:2798 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_KL_DUMP` | src/program/generate.cpp:1091 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_LEND_DROP` | src/core/glm_prefill.cu:1722 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_MLA1` | src/kernels/cuda/glm_fast.cu:5603 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_MLA_ATTN1` | src/kernels/cuda/glm_fast.cu:5641 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_MLA_F16` | src/core/glm_prefill.cu:162 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_MLA_WMMA` | src/core/glm_prefill.cu:158 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_MTP` | src/core/glm_model.cu:3351 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_MTP_ACC_WINDOW` | src/core/glm_mtp.cu:579 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_MTP_ADAPT` | src/core/glm_mtp.cu:464 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_MTP_CHAIN` | src/core/glm_mtp.cu:70 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_MTP_DRAFT` | src/core/glm_mtp.cu:57 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_MTP_DRAFT_VOCAB` | src/core/glm_mtp.cu:151 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_MTP_FIXED` | src/core/glm_mtp.cu:470 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_MTP_START` | src/core/glm_mtp.cu:474 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_MTP_TRACE` | src/core/glm_mtp.cu:551 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_MV_APPROX` | src/kernels/cuda/glm_fast.cu:1809 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_MV_GENERIC` | src/kernels/cuda/glm_fast.cu:5056 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_MV_MMA` | src/kernels/cuda/glm_fast.cu:1829 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_MV_TILED` | src/kernels/cuda/glm_fast.cu:5101 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_NO_REUSE` | src/program/generate.cpp:1290 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_NO_STAGE` | src/core/glm_model.cu:3583 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_NSYS` | src/core/glm_fast_path.cu:6590 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_PIN_CAP_MB` | src/core/glm_fast_path.cu:460 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_PIN_PIECE_MB` | src/core/glm_fast_path.cu:1646 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_PIPE` | src/core/glm_model.cu:1010 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_POOL_CHECK_TEST_MB` | src/core/glm_fast_path.cu:6060 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_POOL_GB` | src/core/glm_fast_path.cu:1142 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_POOL_SLOTS0` | src/core/glm_model.cu:3530 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_PREFETCH` | src/core/glm_model.cu:2664 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_PREFETCH_RANKS` | src/core/glm_model.cu:2666 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_PREFILL` | src/core/glm_prefill.cu:978 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_PREFILL_BLAS` | src/core/glm_prefill.cu:3375 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_PREFILL_LAND` | src/core/glm_prefill.cu:1305 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_PREFILL_LIGHT` | src/core/glm_prefill.cu:2820 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_PREFILL_LIGHT_CHECK` | src/core/glm_prefill.cu:3499 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_PREFILL_LIGHT_LAYER` | src/core/glm_prefill.cu:2824 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_PREFILL_MIN` | src/core/glm_prefill.cu:4211 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_PREFILL_PRED_T` | src/core/glm_prefill.cu:2240 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_PREFILL_PROF` | src/core/glm_prefill.cu:1115 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_PREFILL_SERIAL` | src/core/glm_prefill.cu:4474 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_PREFILL_SH_SUB` | src/core/glm_prefill.cu:342 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_PREFILL_VERBOSE` | src/core/glm_prefill.cu:4803 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_PROF` | src/core/glm_fast_path.cu:643 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_RAM_EVICT` | src/core/glm_fast_path.cu:4695 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_RAM_FREE` | src/core/glm_fast_path.cu:4907 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_RAM_PROTECT` | src/core/glm_fast_path.cu:4699 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_RESPONSES_` | serve/response_config.py:13 (the Maya Responses-API store settings, docs/RESPONSES.md; the Responses route is not part of the frontend surface (spec C-8)) |
| `STRATA_GLM_RESPONSES_CACHE_BYTES` | serve/test_response_store.py:89 (the Maya Responses-API store settings, docs/RESPONSES.md; the Responses route is not part of the frontend surface (spec C-8)) |
| `STRATA_GLM_RESPONSES_CACHE_ENTRIES` | serve/test_response_store.py:94 (the Maya Responses-API store settings, docs/RESPONSES.md; the Responses route is not part of the frontend surface (spec C-8)) |
| `STRATA_GLM_RESPONSES_OWNER_ENTRIES` | serve/test_response_store.py:86 (the Maya Responses-API store settings, docs/RESPONSES.md; the Responses route is not part of the frontend surface (spec C-8)) |
| `STRATA_GLM_ROUTE_AFFINITY` | src/kernels/cuda/glm_fast.cu:5806 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_SCORE_DUMP` | src/program/generate.cpp:3713 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_SCORE_REF` | src/program/generate.cpp:3714 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_SCORE_TOPK` | src/program/generate.cpp:3715 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_SLOT_RAM_GB` | src/program/generate.cpp:1296 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_SLOT_ROUNDTRIP` | src/program/generate.cpp:1772 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_SLOT_TIMING` | src/core/glm_fast_path.cu:7042 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_SLOW` | src/core/glm_model.cu:1490 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_SPEC_PROF` | src/core/glm_fast_path.cu:6249 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_STALL_TEST` | src/core/glm_prefill.cu:4234 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_SVC_TRACE` | src/core/glm_fast_path.cu:4359 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_TEST_PIPE` | src/core/glm_pack_test.cpp:36 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_TIER_GC` | src/core/glm_fast_path.cu:4724 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_TRACE` | src/core/glm_model.cu:2562 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_UNIFORM_SLOTS` | src/core/glm_fast_path.cu:1265 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_VRAM_GB` | src/core/glm_fast_path.cu:1146 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_WARM` | src/core/glm_fast_path.cu:1816 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_WIN_L3` | src/core/glm_fast_path.cu:2968 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_WIN_MLA` | src/core/glm_window.cu:431 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_WIN_PROF` | src/core/glm_window.cu:379 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GLM_WIN_UNION` | src/core/glm_window.cu:276 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GROUP_COPY` | src/prefill/prefill.cpp:549 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_GR_UNFUSED` | src/prefill/prefill.cpp:92 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_HIPBLASLT_TUNING` | maya.py:2276 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_HIPBLASLT_TUNING_V1` | src/prefill/hipblaslt_tuning.hpp:47 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_HIPBLASLT_VERBOSE` | src/prefill/gemm.cu:77 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_HIP_EXPERTS_LEGACY` | src/kernels/cuda/glm_fast.cu:5890 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_IDX_FP16_CHECK` | src/prefill/prefill.cpp:1440 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_INDEXER_PER_TOKEN` | src/prefill/prefill.cpp:1386 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_IO_THREADS` | src/platform/direct_file.cpp:63 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_IQ_GATHER` | src/kernels/cpu/iq_avx512.cpp:37 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_IQ_PREFETCH` | src/kernels/cpu/iq_avx512.cpp:41 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_KV_MAIN_OFF` | src/core/layer.cpp:501 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_KV_RING_OFF` | src/core/layer.cpp:500 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_KV_STAGE_OWN` | src/prefill/prefill.cpp:334 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_MTP_BATCH` | src/prefill/prefill.cpp:721 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_MTP_BATCH_F16` | src/prefill/prefill.cpp:753 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_MTP_PREFILL_SYNC` | src/core/mtp.cpp:591 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_NO_IQ256` | src/kernels/cpu/native_expert.cpp:114 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_NO_IQ4NL` | src/kernels/cpu/native_expert.cpp:354 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_NO_LARGEPAGES` | src/core/pinned.cu:61 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_PLE_BATCH` | src/prefill/prefill.cpp:1000 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_PLE_GGUF` | src/kernels/ple_parity.cpp:255 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_POOL_SPIN_US` | src/kernels/cpu/pool.cpp:177 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_POOL_TRACE` | src/core/expert_source.cpp:864 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_PREFILL_DUMP_R` | src/prefill/prefill.cpp:1860 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_PREFILL_ISSUER` | src/prefill/prefill.cpp:1174 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_PREFILL_MMQ` | src/prefill/prefill.cpp:427 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_PREFILL_RING` | src/prefill/prefill.cpp:83 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_PREFILL_TIMING` | src/prefill/prefill.cpp:935 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_PROMPT_ATTN_OLD` | src/prefill/prefill.cpp:1508 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_PROMPT_ATTN_V1` | src/kernels/cuda/qsa_prompt_attn.cu:709 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_QSA_DUMP` | src/prefill/prefill.cpp:1486 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_QSA_DUMP_LAST` | src/prefill/prefill.cpp:1487 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_REFILL_BLOCKING` | src/program/generate.cpp:104 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_REMOTE_SPIN` | src/core/remote_experts.cpp:279 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_REMOTE_ZEROCOPY` | src/core/remote_experts.cpp:404 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_REQUEST_QUIET_S` | serve/server.py:411 (maya's own serve/server.py: the launcher the SP4 manifest + supervision replace (T2-b); undocumented in SETTINGS.md) |
| `STRATA_SELECT_OLD` | src/prefill/prefill.cpp:1407 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_SEL_OVERLAP` | src/prefill/prefill.cpp:1418 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_SERVER_LOG` | serve/server.py:4025 (maya's own serve/server.py: the launcher the SP4 manifest + supervision replace (T2-b); undocumented in SETTINGS.md) |
| `STRATA_SPLIT_MISS_MS` | src/program/generate.cpp:5342 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_SPLIT_TIMING` | src/program/generate.cpp:7722 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_STAGER_RING` | src/prefill/prefill.cpp:153 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_STAGER_THREADS` | src/prefill/prefill.cpp:541 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_STATE_HASH` | src/program/generate.cpp:7616 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_STATE_HASH_GDN` | src/prefill/prefill.cpp:1934 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_TEST_CACHE_FAIL` | src/program/generate.cpp:5530 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_TG_FLUSH_US` | src/core/session.cpp:884 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_TOPK_OLD` | src/kernels/cuda/qsa_select.cu:510 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_TRACE` | src/prefill/prefill.cpp:1026 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_VERIFY_DEBUG` | src/core/verify.cpp:57 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_VERIFY_DEVICE_PLAN` | src/core/verify.cpp:292 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_VERIFY_NODES` | src/core/verify.cpp:840 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `STRATA_VERIFY_PROFILE` | src/core/verify.cpp:269 (undocumented engine-internal knob; it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `--allow-empty` | serve/test_update.py:132 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
| `--api-key` | serve/server.py:4337 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
| `--backend` | maya.py:2995 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
| `--bin` | maya.py:697 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
| `--build` | maya.py:1391 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
| `--calibrate` | maya.py:4048 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
| `--compat-bf16` | maya.py:2076 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
| `--config` | serve/server.py:4316 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
| `--connect-timeout` | maya.py:1817 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
| `--context` | maya.py:4019 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
| `--crash-at-start` | serve/mcp_fake_server.py:165 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
| `--disable-pip-version-check` | maya.py:736 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
| `--download-model` | maya.py:3990 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
| `--draft` | maya.py:4004 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
| `--drafter` | maya.py:3805 (the DS4 pack-build tool's own arg inside maya.py's CLI: the launcher the SP4 manifest + supervision replace (T2-b); SP5-P2 §Non-goals: the DeepSeek DS4 path) |
| `--drafting` | maya.py:4057 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
| `--ds4-quant` | maya.py:3998 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `--dspark` | maya.py:3838 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `--dspark-conf` | maya.py:3838 (out of scope: the DeepSeek DS4 engine path is not part of the GLM parity surface (plan 2026-10-22-sp5-p2 §Non-goals); it never renders: the closed child-env allowlist takes its names from the typed rows) |
| `--engram` | maya.py:3804 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
| `--env` | maya.py:4034 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
| `--equal` | docs/SETTINGS.md:195 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
| `--fail` | maya.py:1838 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
| `--ff-only` | serve/update.py:163 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
| `--find-links` | maya.py:3333 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
| `--fit-max-tokens` | serve/server.py:4334 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
| `--gguf-dir` | maya.py:4009 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
| `--gpu` | serve/server.py:1186 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
| `--host` | serve/server.py:4318 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
| `--host-compiler` | maya.py:4037 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
| `--http` | serve/mcp_fake_server.py:168 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
| `--idle-unload` | serve/server.py:4330 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
| `--index-url` | maya.py:736 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
| `--json` | maya.py:3060 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
| `--llama-dir` | maya.py:4039 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
| `--max-tokens` | serve/server.py:1190 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
| `--measure` | serve/server.py:1430 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
| `--mmproj` | serve/server.py:1184 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
| `--model` | serve/server.py:1184 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
| `--model-dir` | maya.py:3995 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
| `--models-dir` | maya.py:4013 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
| `--no-flash-attn` | serve/server.py:1192 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
| `--no-index` | maya.py:3332 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
| `--no-speed-calibration` | maya.py:4061 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
| `--no-start` | maya.py:3985 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
| `--no-vision` | maya.py:4007 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
| `--no-warmup` | serve/server.py:1208 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
| `--nvcc` | maya.py:4036 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
| `--nvml-child` | serve/telemetry.py:52 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
| `--open` | serve/server.py:4329 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
| `--page` | serve/mcp_fake_server.py:163 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
| `--parent-pipe` | serve/server.py:4343 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
| `--plain` | maya.py:3988 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
| `--porcelain` | serve/update.py:117 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
| `--port` | serve/server.py:4324 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
| `--quiet` | maya.py:3332 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
| `--rebuild` | maya.py:4040 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
| `--repack` | maya.py:4041 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
| `--report` | maya.py:4042 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
| `--retry` | maya.py:1888 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
| `--root` | maya.py:695 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
| `--script` | serve/server.py:4321 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
| `--setup` | maya.py:3983 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
| `--shards` | maya.py:3806 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
| `--showdriverversion` | maya.py:3060 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
| `--showmeminfo` | maya.py:3060 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
| `--showproductname` | maya.py:3060 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
| `--skip` | serve/telemetry.py:90 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
| `--speed-calibration` | maya.py:4052 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
| `--speed-limit` | maya.py:1817 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
| `--speed-time` | maya.py:1817 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
| `--tags` | serve/update.py:160 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
| `--target` | maya.py:546 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
| `--tokenizer` | serve/server.py:4327 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
| `--version` | maya.py:552 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
| `--yes` | maya.py:2992 (maya.py's own CLI: the launcher the SP4 manifest + supervision replace (T2-b)) |
<!-- maya-out:end -->


## Run the native inference server

`strata-secure-server` is the first runnable native chat path. It starts one
local Strata engine as a direct child (never through a shell), communicates
only through private stdio, and exposes authenticated `GET /health`, `GET
/v1/models`, `GET /v1/models/<id>`, and text-only `POST /v1/chat/completions`
— complete JSON or streamed SSE (`"stream": true`, SP3 M2) — plus `GET /metrics`
when `listener.monitor` is on (below). Its
default fixed native profile uses the local GPT-2-byte-BPE tokenizer pack and
the fixed Qwen ChatML renderer; the other profiles (GLM-5.3, Gemma 4, flint,
MiniMax, ...) are documented below. It refuses a client's own tools and MCP servers
(its web search is the server's, above), and a request whose `model` is not the
running model. Downloads are the Models tab's (`models.downloads`, below), run by
`strata pull` as a separate process. Images are an
input path only for a vision-capable model (`vision.enabled` plus the
manifest `mmproj` artifact, SP5 M1) and only as `data:` URLs — an http(s)
image URL is refused by name, never fetched.

`strata serve` (SP5 M1) runs exactly this boot in the foreground from the
config file alone — no daemonization, no `server.json`, no lock (the pin has
none, and the PX lifecycle writer has never existed); it serves the installed
managed model, and `--expert` stays a `strata-secure-server` flag. `strata
version` prints the frontend and pin versions — the tag version governs, and
the engine's own self-version is informational only. `strata key show [--env]`
prints the locally generated API token (only when the store is readable by
this uid — the SP1 store rules), and `strata key rotate` replaces it atomically;
after a rotate, restart the server and reconfigure every app that used the old
token.

### Built-in chat page and the keyless loopback mode (owner decisions 2026-10-21)

`listener.web_ui = true` serves a chat page at `/` with upstream Strata's look:
its UI kit (tokens, components, icon sprite, favicon; MIT, adapted) and its Outfit
font (SIL OFL), from `secure_frontend/web/`. The licence texts are
`web/LICENSE.strata-ui.txt` and `web/fonts/OFL.txt`, with provenance in
docs/PROVENANCE.md. The files are embedded in the binary at configure time
(`cmake/EmbedWebUi.cmake`: text files ASCII with LF, the fonts as they are; nothing
read from disk at run time) and served only at their exact `GET` targets (`/` and
`/ui/...`), after the `Host` and `Origin` checks. They hold no secret, load nothing
from any other server, and reach no route.

- **The login is the API key.** The page asks for it, checks it against
  `/v1/models` and keeps it in that browser tab only (`sessionStorage`). It sends it
  only as `Authorization: Bearer` to this server, with cookies off. Every API route
  still requires the key. The key field has no form name, so no submission can put
  it in a URL.
- **Upstream's chat:** your messages in green bubbles and the model's in cards, with
  Markdown (headings, lists, tables, quotes, code blocks with a copy button), the
  reasoning in a collapsible "Thinking" block, and per reply the model, tokens,
  speed and time.
  - **Header:** Chat, Monitor, Models and About tabs, a New chat button, a status pill
    (polls `/health`) and a light/dark switch.
  - **Composer:** attach text files, save the chat as Markdown, Stop, Send.
  - **Sampling drawer:** Thinking Off, Low, Medium or High (upstream's four levels,
    sent as `reasoning_effort`; High is the default; a note under it says Gemma models think
    or answer directly, Low, Medium and High all meaning thinking on), temperature, top-p,
    max tokens, seed, show thinking.
  - **Monitor** (with `listener.monitor`, below): upstream's dashboard — the model
    state, speed (decode and prefill), GPU load, VRAM, GPU temperature, power, PCIe and
    CPU cards with a minute of history, the context fill, GPU memory, RAM and
    temperature bars, and the recent requests with their totals. Without the switch
    the tab says the Monitor is off.
  - **About:** the model, the engine state, and the base URL for other clients.
  - **Storage:** the chat and the settings live in that tab; the theme choice is in
    `localStorage` (`strata.theme`, nothing else).
  - **Left out:** upstream's Images and MCP views, its conversation-cache card and its
    server-side settings need features this server does not have.
  - **Phones** (owner, 2026-10-23): below 640 px the header takes two rows (the brand and
    the controls, then the tabs across the width), and every view fits 320 px without
    sideways scrolling. A browser loads the page's files over up to six connections at once;
    an over-cap connection is answered 503 without a log line, and when that was `app.js` an
    iPhone never showed the sign-in (the cap was 4). With `listener.web_ui` the cap
    `limits.concurrency.max_connections_per_remote_address` now derives min(6, workers) (owner,
    same day); an explicit value is used as written; loopback clients are exempt.
- **Its own policy:** the page's responses carry `Content-Security-Policy:
  default-src 'none'; script-src 'self'; style-src 'self'; connect-src 'self';
  img-src 'self' data:; font-src 'self'; base-uri 'none'; form-action 'none';
  frame-ancestors 'none'` (no inline script, style or handler; tests pin that) and
  `Cross-Origin-Opener-Policy: same-origin`. Every other response keeps
  `default-src 'none'`.

`reasoning_effort` is also accepted per request on the qwen and gemma4 profiles (`none`,
`low`, `medium`, `high`, the `reasoning.effort` words; for gemma4 see its section below:
`none` is off, the three levels all mean thinking on). glm53 refuses it by name. The
levels write what the model's own template writes (session log step 16): `none` closes the
think block; `high` (also a request with no level and no `reasoning.effort`) puts the
template's "Reasoning effort is set to xhigh..." sentence at the top of the system turn,
`low` its low sentence, `medium` none. With `reasoning.effort_position = "end"` and an
engine that takes `--tail-role-token`, the conversation is written as `high` and a `low` or
`medium` sentence goes in a short system turn right before the answer, as upstream does.

### The Monitor (listener.monitor, session log step 16)

`listener.monitor = true` (config file only, off by default) serves `GET /metrics` for the
page's Monitor tab, in upstream's `/metrics` shape. Like every route it needs the key; with
the switch off it answers 404. It reports:

- the served model name, the context from the engine's `READY`, and the engine's state
  (`starting`, `idle`, `reading`, `generating`, `error`) with the queue length;
- the last 100 generations: when each ended, how (`stop`, `length`, `cancel`,
  `disconnect`, `error`), the prompt, reused and output token counts and the engine's own
  prompt and decode times (from its `DONE` record), with totals since the start. Never a
  prompt, an answer, a client address or a key. All of it is in memory and goes when the
  server stops;
- this machine's CPU load and RAM (from the operating system) and GPU 0's load, memory,
  temperature, power and PCIe link, read once a second with a minute of history. The GPU
  readings come from NVIDIA's `nvidia-smi`, run as its own child process from the system
  directory only (`System32` on Windows, `/usr/bin` on Linux), with a fixed argument list,
  no shell and a fixed environment (on Windows only `ProgramFiles`, which NVML needs to
  start, read from the machine registry — nothing is inherited from the server's
  environment), and stopped with the server. No NVIDIA library is
  loaded into the server. Without `nvidia-smi` the GPU cards stay empty and say why;
- for an expert (MoE) model, the number of experts loaded, beside the memory on the VRAM and
  RAM cards (the engine's `expert_slots` total, with `expert_slots_primary` the VRAM tier and
  the rest the RAM tier). These come from the engine's own startup `INFO`, captured when the
  model reaches `READY` (owner, 2026-10-23), so a just-started model shows them without a first
  chat; the GLM engine's live `STAT` cadence updates them during generation. GB-only when the
  engine reports no expert figures.

`listener.loopback_without_key = true` answers the API and the page with no key
at all. On the page, the login is skipped. It is accepted only with a loopback
`listener.host` (`127.0.0.1`, `::1`, `localhost`), and refused next to a
`token.source` (a key that would never be read). `--check` reports `token: not
used`, and the boot prints a warning line on stderr. `Host` and `Origin` are
still checked, and chat still needs `Content-Type: application/json`. So
another web site cannot use it from a browser: DNS rebinding fails the Host
check, a page on another origin fails the Origin check, and a plain form post
fails the Content-Type check. Any program or user on the same machine can
use it.

### Model launcher: choose and start models from the page (models.cards)

Owner request 2026-10-23; spec `docs/superpowers/specs/2026-10-23-model-launcher-design.md`
(decisions D1-D10 accepted), plan `docs/superpowers/plans/2026-10-23-model-launcher.md`. With
`models.cards` set, the server binds no fixed model. Each **model card** is one owner-only JSON
file naming a model, its engine profile, the card-owned config rows and the options the page may
change; the page's **Models** tab lists the cards, sets a card's options and starts or stops it.
One model runs at a time.

```json
{
  "schema_version": 1,
  "name": "Gemma-4-26B-A4B-it",
  "profile": "gemma4",
  "config": {
    "engine": {"executable": "C:\\...\\strata-gemma4-engine.exe", "model_id": "gemma-4-26B-A4B-it-UD-Q8_K_XL",
               "gpu": "0", "lib_dirs": ["C:\\...\\CUDA\\v13.0\\bin\\x64"]},
    "roots": {"models": "C:\\Strata-data\\models\\gemma4_q8", "tokenizer_pack": "C:\\...\\gemma4-pack\\tokenizer"},
    "limits": {"chat": {"max_prompt_tokens": 262144, "max_completion_tokens": 262144}}
  },
  "options": {
    "max_context": {"default": 262144, "min": 4096, "max": 262144},
    "expert_vram": {"default": "auto"},
    "thinking": {"default": "off"},
    "temperature": {"default": 1.0}, "top_p": {"default": 0.95}, "top_k": {"default": 64}, "min_p": {"default": 0.05}
  }
}
```

- **The card:** `name` is the served name (the alias grammar); `profile` is `qwen`, `glm53`, `gemma4`, `deepseek`,
  `raon1`, `flint` or `minimax` (GLM-5.3 cards follow the page's GLM chat fix, D10); `config` holds only card-owned rows of the
  table above, read by the config file's own reader: `engine.*` (not `engine.profile`, which the
  profile sets, nor `engine.lazy_load`/`engine.idle_unload_s`), `model.aliases`, `roots.models`,
  `roots.tokenizer_pack`, `defaults.sampling.*`, `reasoning.*`, and
  `limits.chat.max_prompt_tokens`, `max_completion_tokens`, `default_max_tokens`. Cards always
  launch the expert way (an inventory model and its tokenizer pack). The main config then holds
  the server (listener, token, roots.state/cache, the other limits, `models.*`) and may not set a
  card-owned row (refused by name). A bad card is refused at boot by its place and name and the
  others load (D9); `--check` lists each card's verdict.
- **Options:** each option maps to one engine flag or config row, with the engine's own bounds,
  narrowed by the card's `min`/`max`/`choices`; an option the card does not list is not shown.
  The `qwen` engine (upstream Strata): `max_context` (`--max-context`; a dropdown of 4K, 8K, 16K,
  32K, 64K, 128K, 256K, 384K, 512K, 768K and 1M; past the trained 256K the start adds
  `--rope-scaling yarn --rope-scale <context / 262,144>`, upstream's own setup rule and
  experimental there), `kv` (`--kv` fp16|int8|q4_0|k8v4), `kv_resident` (**KV streaming**,
  `--kv-resident`; a dropdown of Off, 20K, 32K, 64K, 128K, 256K and 512K resident tokens, never
  more than the context), `kv_grow` (`--kv-grow`), `mtp` (off = `engine.mtp` false: the draft
  layer's flags are dropped; `--spec` stays for prompt lookup), `spec` (2-8), `spec_min_p`,
  `expert_cache` (`auto` or a count), `prefill` (`auto` or a chunk), `thinking`
  (none|low|medium|high) and the sampling defaults. The `gemma4` engine: `max_context` (the same
  dropdown up to its trained 256K), `chunk`, `expert_vram` (GiB or `auto`), `thinking` (off|on)
  and the sampling defaults (it has no KV streaming yet, D8). A listed value off the dropdown is
  refused (400 `invalid_option`); the card's `min`/`max` trim the dropdown. A card's fixed
  `engine.args` may not hold a flag one of its listed options sets (the context option's
  includes the rope flags). The page's last options per card are remembered in
  `<roots.state>/model-options.json` (owner-only) and offered at the next start (D7).
- **Control:** `models.control = true` serves `GET /v1/strata/models` (the cards, their state
  and options), `POST /v1/strata/models/start` (`{"name": "...", "options": {...}}`, 202) and
  `POST /v1/strata/models/stop` (202), keyed like every route, for loopback clients; remote
  clients get 403 `control_not_allowed` unless `models.control_remote = true` (D2; the page
  then shows the Models tab with that reason instead of the cards). Each start
  and stop is logged on stderr (card, option values, loopback or remote; never the key).
  `models.start` names the card started at boot; unset, the server boots with no engine (D5).
- **A start:** the card, the option values and the card's config are validated at once (a
  refusal names the option: 400 `invalid_option`); the model is then composed and its tokenizer
  loaded before the running model is touched, so a bad start never takes it down. Then new chat
  requests get 503 `model_switching` (Retry-After 5), queued ones are refused, the generation in
  flight may finish for up to 60 s and is then cut like a shutdown (D3), the old engine stops,
  and the new one starts. An engine that ends before READY leaves its card `failed` with a fixed
  reason; nothing restarts the old model (D4). One start or stop at a time (409
  `switch_in_progress`).
- **Chat and the OpenAI surface:** chat uses the running card; a request naming another card gets
  409 `model_not_running` (D6), and with none running 503 `no_model_running`. `/v1/models` lists
  the running card and its aliases and adds `strata_models` (every card's id and state);
  `/health` answers `{"status":"ok","engine":"none"}` with nothing running.
- **Not with cards:** `vision.*`, `engine.lazy_load`, `engine.idle_unload_s` (refused by name);
  the `strata` CLI's verbs read a single-model config only, except the store verbs below.

### Model downloads: GGUF models from Hugging Face on the Models tab (models.downloads)

Owner requests 2026-10-23 (O1-O4: a card per model Strata and Project Maya support with its
download status on the Models tab; the free space of the data drive, refusing a download that
does not fit; a pasted Hugging Face link; Hugging Face and `.gguf` files only), then "Have a
download pause and resume functionality". Spec
`docs/superpowers/specs/2026-10-23-model-downloads-design.md`, plan
`docs/superpowers/plans/2026-10-23-model-downloads.md`.

```json
"models": {
  "cards": ["C:\\...\\cards\\qwen-iq3s.json"],
  "downloads": true,
  "downloads_root": "C:\\Strata-data\\downloads",
  "prepare_python": "C:\\multi-strata\\.venv-dev\\Scripts\\python.exe",
  "prepare_tools": "C:\\multi-strata\\upstream\\tools",
  "download_engine_card": "Qwen3.8-Flash-Next"
}
```

- **What runs:** the server holds no download code (and, unless built with email alerts, no network client at
  all). Every transfer is a child,
  `strata --config <the server's config> pull --manifest <m> --trust-manifest sha256:<d> --yes
  --progress-lines` (SP4's confined pull: the downloader child, HTTPS, the host rules, every
  file's size and SHA-256 fixed before a byte moves), and every link lookup
  `strata --config <cfg> hf-info <link>` (one bounded answer, no redirect). `strata` must sit
  beside `strata-secure-server`. Each child has no shell, a fixed argv, a closed environment
  (the system's own variables, a private temp folder under `<roots.state>/downloads/tmp`, and
  `STRATA_GGUF_PY` when the server has it), its working folder `<roots.state>/downloads` and the
  Job. One download runs at a time; the others queue.
- **The cards:** the catalog (`catalog/downloads.json`, generated by
  `tools/secure/download_catalog.py` from upstream's `setup.py` and Maya's `maya.py` pins, checked
  with `--check`, embedded at build) has one card per GGUF variant: upstream's with commit, size
  and SHA-256 per file; Maya's with SHA-256 per shard at `main`, resolved at start (the commit
  and sizes come from Hugging Face, and every reported SHA-256 must equal the pinned one). A
  pasted link (`https://huggingface.co/<owner>/<repo>/resolve|blob/<revision>/<path>.gguf`, split
  files expanded to the whole set) is looked up, shown for confirmation (commit, files, sizes,
  total, whether it fits) and becomes a "Your downloads" card (kept in
  `<roots.state>/downloads/custom.json`). Anything else is refused: other hosts, ports, user
  info, datasets and spaces, traversal, files that are not `.gguf` (O4; the page checks first,
  the server again, the manifest profile `gguf` a third time).
- **Space (O2):** the page shows the free and total space of `models.downloads_root`'s drive; a
  start needs the missing bytes plus 2 GiB, otherwise 507 `insufficient_storage` (the card says
  how much is needed and how much is free). Maya's cards are checked again once their sizes are
  known; the pull checks while it writes.
- **Pause, Resume, Cancel:** Pause stops the transfer and keeps its partial files; Resume is a
  start of the same manifest (the same digest, so the same staging) and continues from them.
  Cancel stops it and deletes the partial files (`strata rm --partials <id>:download`).
- **After the download (D7):** a card upstream packs (Qwen3.8-Flash-Next and its Coder, Swift
  and Gyro variants) is prepared: the server
  writes the script it was built with (`tools/secure/prepare_download.py`) to
  `<roots.state>/downloads/`, runs it with `models.prepare_python -I`, which calls upstream's
  `iq_pack.py` and `strata_tokenizer.py` read-only from `models.prepare_tools` (they need numpy
  and regex; `--compat-bf16` variants also gguf-py through `STRATA_GGUF_PY`) into
  `<downloads_root>/packs/<id>/`, then writes a model card (`<roots.state>/cards/<id>.json`,
  owner-only) with the engine settings of `models.download_engine_card`; the launcher adds it at
  once, and at every boot. GLM-5.3 and Qwen3.6 files and pasted links are downloaded only (their
  card says so).
- **Routes** (off unless `models.downloads`; keyed; loopback clients unless
  `models.control_remote`, else 403 `control_not_allowed`): `GET /v1/strata/downloads` (the disk,
  the running transfer, every card with its state: available, queued, downloading, verifying,
  preparing, paused, installed, ready, failed, cancelled), `POST /v1/strata/downloads/start`
  `{"id"}` or `{"url", "confirm": true}` (202; 404, 409 `download_in_progress`, 507),
  `POST .../pause` and `.../cancel` `{"id"}` (202; 409 `download_not_active` when there is nothing
  to pause or cancel),
  `POST .../link` `{"url"}` (200 `{"confirm": {...}}`; 400 with a fixed `reason`, 502
  `upstream_unavailable`).
- **The store:** `models.downloads_root` is an SP4 store (`<id>/download/v<seq>-<hex>/`); the
  `strata` verbs (`list`, `verify`, `rm`, ...) read a `models.cards` config with it as the store.
- **Speed (parallel ranged downloads):** Hugging Face's CDN limits throughput per connection, so a
  single stream is slow. With `pull.connections` > 1 (1-32, 16 by default as upstream's; 1 = one stream) a file of at least 256
  MiB is fetched as 32 MiB pieces over that many confined `curl` children at once, each a bounded
  `--range lo-hi` to the resolved CDN URL, written at its offset; a `.part.chunks` sidecar records
  finished pieces so a pause resumes only what is missing, and the whole file is hashed in one pass
  at the end. A server that does not honour ranges, or a proxy, falls back to one stream. The same
  key is read by the CLI `strata pull`. Mirrors upstream Strata's downloader
  (`upstream/setup.py`, `DL_CONNECTIONS` default 16).

### Long contexts and model names (owner requests, session log step 13)

The defaults keep a request small (48 KiB body, 32K-token prompt). For long contexts, raise
them in the config file, up to upstream's scale:

- `limits.chat.max_prompt_tokens` and `limits.chat.max_completion_tokens` take up to
  1,048,576. `limits.http.max_request_bytes`, `limits.chat.max_body_bytes` and
  `limits.chat.max_message_bytes` take up to 256 MiB (upstream's body limit), and
  `limits.chat.max_messages` up to 65,536.
- Set `limits.engine.generation_timeout_s` and `limits.http.request_deadline_ms` for the
  longer prefill and upload.
- The engine's own context is `--max-context N` in `engine.args`, with `--kv-resident 32768`
  (KV streaming, as upstream's setup does at 64K and more) on a GPU with less memory than
  the KV cache needs.
- Past the trained 262,144 positions, add upstream's YaRN rope scaling: `--rope-scaling yarn
  --rope-scale F` with F = context / 262,144 (2 at 512K, 4 at 1M). This is experimental
  upstream, needs proportionally more RAM for the KV cache (~13 KB a token), and is fixed for
  the whole run.

`model.name` sets the name the server serves the model as: `/v1/models` lists it, the chat
page shows it, and requests name it (upstream serves `qwen3.8-flash-next` by default, set by
its `model_name`). Unset, the served id is the inventory file name (expert mode) or
`name:tag` (a managed model).

### Routes, errors, headers (SP3)

Every route requires `Authorization: Bearer <token>` (except the built-in
page's files and the keyless loopback mode, above); authentication happens
when the request header is complete, before any body byte. Unknown paths are
404 `not_found` and wrong methods 405 `method_not_allowed` (with `Allow`) only
after authentication — unauthenticated requests get 401 on every path, so the
route table is not observable without a token. Errors are fixed JSON bodies
`{"error":{"message","type","param","code"}}` — the message is a compile-time
literal, `param` names at most a fixed-list key, never request bytes:

| Status | `code` | Message |
| --- | --- | --- |
| 400 | `invalid_request` | The request is malformed or not supported. |
| 400 | `invalid_json` | The request body is not valid JSON within the allowed limits. |
| 400 | `unsupported_parameter` | This parameter is not supported by this server. |
| 400 | `extension_disabled` | Tools, functions and extensions are disabled on this server. |
| 400 | `context_length_exceeded` | The prompt exceeds the model context or the configured prompt limit. |
| 401 | `invalid_api_key` | Missing or invalid API key. (`WWW-Authenticate: Bearer`) |
| 403 | `forbidden` | The request origin or host is not allowed. |
| 404 | `not_found` / `model_not_found` | Not found. / The requested model is not served here. |
| 405 | `method_not_allowed` | Method not allowed. (`Allow`) |
| 408 | `request_timeout` | The request was not received in time. |
| 411 | `length_required` | A Content-Length body is required; chunked bodies are not accepted. |
| 413 | `request_too_large` | The request body is too large. |
| 414 | `request_line_too_long` | The request line is too long. |
| 415 | `unsupported_media_type` | Send application/json. |
| 417 | `expectation_failed` | Unsupported Expect header. |
| 431 | `header_too_large` | The request header is too large. |
| 500 | `internal_error` | Internal error. |
| 503 | `server_busy` | The server is busy; retry later. (`Retry-After: 1`) |
| 503 | `engine_unavailable` | The local engine is not available. (`Retry-After: 5` or none) |
| 503 | `engine_error` | The local engine could not complete the request. (`x-should-retry: false`) |
| 503 | `server_shutting_down` | The server is shutting down. |
| 505 | `http_version_not_supported` | Only HTTP/1.1 is supported. |

Every response carries `Cache-Control: no-store`,
`X-Content-Type-Options: nosniff`,
`Content-Security-Policy: default-src 'none'; frame-ancestors 'none'` (the
built-in page's files carry their own, above),
`Referrer-Policy: no-referrer` and `Date`. `Vary: Origin` and (only for
configured origins) `Access-Control-Allow-Origin` appear as per §4.4.

`POST /v1/chat/completions` with `"stream": true` (SP3 M2) is served as SSE:
`Content-Type: text/event-stream` under HTTP/1.1 chunked framing,
`Cache-Control: no-store`, no `Content-Length`. The event sequence is a role
chunk, content chunks coalesced to at most 16 KiB of text each
(`api_constants::kMaxDeltaTextBytes`) with no SSE event above
`api_constants::kMaxSseEventBytes` (128 KiB), a `: keep-alive` comment every
`limits.http.stream_keepalive_ms` while no content flows, an optional usage
chunk when `stream_options.include_usage` is true, a final chunk with
`finish_reason` (`stop` or `length`), `data: [DONE]`, then the terminal chunk.
Headers go out only after the engine lease, the context re-check and `GEN`.
After the headers, engine failures arrive as in-stream error events with the
§7.2 codes (`engine_error`, `engine_unavailable`, `internal_error`,
`server_shutting_down`), each followed by `data: [DONE]` and the terminal
chunk. A stream cancelled by the client, by `limits.http.write_stall_ms` or by
a missed shutdown budget gets neither `data: [DONE]` nor the terminal chunk, so
SDKs see the truncation. Keys that govern streams:
`limits.http.write_stall_ms` (no progress → cancel + `STOP`),
`limits.http.response_write_deadline_ms` (the tail once the generation ended),
`limits.http.stream_keepalive_ms`, and `limits.chat.max_generated_bytes` (the
pump's held-text bound). The loopback-only client library used by the tests
(spec §6.1, the only `connect()` in the frontend) also serves the future
`strata run`; its command-line form, `strata-api-probe`, is test-only and
never installed.

Default limits (all config keys; see the table below): 8 workers, 4 queued
generations (queue timeout 300 s), 4 connections per remote address (loopback
exempt), 10 s header / 30 s request deadlines, 64 KiB request cap (49152-byte
chat body), 10 s write stall / 60 s response-write deadline, `logging.requests`
off (when on: one fixed-field stderr line per request under a token bucket —
never tokens, prompts, paths or payload text; a reply cut by a token the
frontend refuses to output answers the generic `500 internal_error` and logs
the log-only code `undecodable_token`).

The launch described in this section is the `--expert` bring-up mode: the
operator supplies `engine.profile`, `roots.tokenizer_pack` (required) and the
weight flags other than `--native` in `engine.args`, and `engine.model_id`
names a `.gguf` in the local inventory (no managed model; `/v1/models` lists
only `engine.model_id` and its `model.aliases`, as for a managed model — SP3
E9). Pass `--expert` when serving and with `--check` (which
then validates this mode); `init` accepts it and ignores it.

The model directory must already contain the selected `.gguf`; the tokenizer
pack must already contain `vocab.json`, `merges.txt`, and `token_type.json`.
The server adds `--serve` and, for this default profile, supplies shard 1 via
`--native <model>` built from the local inventory entry named by `--model-id`
(appended last); never pass `--native` yourself. The expert pack (`--pack DIR`)
and the other weight sources (`--ple-gguf`, `--embd-gguf`, `--native-head-gguf`,
`--native-dense-gguf`, `--mtp`) are operator-supplied with `--engine-arg`
(`engine.args` in the config file) until SP5 models the whole launch. With
`engine.mtp` set to `false` the draft layer's own options (`--mtp`,
`--spec-min-p`, `--batch-mtp`) are dropped from the engine's argv at every
start — the pinned `mtp: false` behaviour (`strata_control/draft_layer.py`);
prompt-lookup `--spec` stays. The
engine loads `--pack` unconditionally and its default is the cwd-relative
`pack/full`, so a real run must pass it. Give every engine switch and value as
its own `--engine-arg` (its own array element in `engine.args`). With the
`qwen` profile, `--native`, `--serve`, `--batch`, `--slots` and `--vision` are
rejected there, in both `--flag value` and `--flag=value` forms, and
`--check` names `engine.args` as the key at fault.
The engine child receives a fixed environment rendered by the frontend (see
`docs/security/threat-model.md`, Engine process isolation); nothing is
inherited from the server's environment, so pass every engine setting as a
typed option or `--engine-arg`. The engine path (`engine.executable`)
must be absolute and name a regular file not writable by group or others; the
engine receives the canonical models root, never the configured spelling.

The flags below can equally live in the config file (see
[Configuration](#configuration)); the token must be at least 32 bytes.

```sh
export STRATA_API_TOKEN='replace-with-a-long-random-token-of-32-bytes-or-more'
./out/build/linux-gcc/strata-secure-server --expert \
  --models-root /absolute/path/to/models \
  --model-id local-model-name \
  --tokenizer-pack /absolute/path/to/tokenizer-pack \
  --engine /absolute/path/to/strata-engine \
  --engine-arg --pack --engine-arg /absolute/path/to/pack \
  --state-dir /absolute/path/to/owner-only-state-dir \
  --token-env STRATA_API_TOKEN
```

For example, make a request without putting the token into a URL or request
body:

```sh
curl --fail --header 'Host: 127.0.0.1:8080' \
  --header "Authorization: Bearer $STRATA_API_TOKEN" \
  --header 'Content-Type: application/json' \
  --data '{"model":"local-model-name","messages":[{"role":"user","content":"Hello"}],"max_tokens":64}' \
  http://127.0.0.1:8080/v1/chat/completions
```

This is not yet a release claim for every upstream model/template or vision
profile. The native server fails closed for profiles it cannot represent; do
not substitute the legacy Python service to bypass that boundary.

### GLM-5.3 Flash profile

Select `--profile glm53` to use the native, fixed GLM-5.3 text profile. It is
text-only, non-streaming, and greedy through the private `GEN`/`T`/`DONE`
stdio protocol. The only additional engine record accepted in this mode is its
strictly validated `PP` prefill-progress heartbeat. Tools, images, remote URLs,
templates, downloads, analytics, and MCP remain disabled.

This profile emits fixed GLM framing
`[gMASK]<sop>...<|assistant|><think>` and rejects message text that contains
any framing literal. It does not build, download, or embed the LightTransport
fork. Build and audit a compatible `strata-glm-decode` executable separately,
then pass that build's required positional engine arguments unchanged with
repeated `--engine-arg`. The server adds only `--serve` immediately after the
executable; it does not reinterpret or synthesize positional arguments, does
not add `--native`, and rejects only `--serve` and NUL bytes in them. With
`glm53`, `engine.args` must not be empty (`--check --expert` reports it as required). The
tokenizer pack must match that exact engine/model build and provide the fixed
GLM framing markers as CONTROL tokens.

```sh
./out/build/linux-gcc/strata-secure-server --expert \
  --profile glm53 \
  --models-root /absolute/path/to/models \
  --model-id glm53-local \
  --tokenizer-pack /absolute/path/to/glm53-tokenizer-pack \
  --engine /absolute/path/to/strata-glm-decode \
  --engine-arg /absolute/path/to/models/glm53-local.gguf \
  --engine-arg /absolute/path/to/fork-required-config \
  --token-env STRATA_API_TOKEN
```

The positional-argument example is illustrative. Use the separately audited
engine's local documentation for its exact required order and values; do not
expose its own listener or run its network-facing service beside this server.

### Gemma 4 profile

Select `--profile gemma4` (`engine.profile = "gemma4"`) to serve a Gemma 4 26B-A4B GGUF with
our own `strata-gemma4-engine` (spec `docs/superpowers/specs/2026-10-22-gemma4-engine-design.md`;
the engine is a separate build, `engines/gemma4/` — see its README; it is not in this tree's
frontend). It is text only (no
images, D2), streamed or not, on the same private `strata_0143` stdio protocol as the
default profile, with the strata family's closed child environment.

- **Launch (expert mode):** `engine.executable` names the Gemma engine; `engine.model_id` is the
  `.gguf` stem in `roots.models`. The server composes `--serve` (inserted by the launcher), the
  allowlisted `engine.args` (`--max-context N`, `--chunk N`, `--kv f16|f32`, `--no-graph`,
  `--expert-vram GiB`, `--expert-slots N`, `--hybrid-rows N`, `--cpu-threads N`; anything else is
  refused by name or, if unknown, without being echoed), then `--gpu` from `engine.gpu`, then
  `--native <models_root/file>` last. The qwen-only rows (`engine.vram_*`, `engine.layer_split`,
  `engine.split_skip_if_fits`, `session.expert_profile_save*`, `replicas.parallel`,
  `engine.mtp`, `reasoning.effort_position`) are refused by name on this profile.
- **Tokenizer pack (`spm_bpe`):** `roots.tokenizer_pack` holds `vocab.json`, `merges.json`,
  `token_type.json` (and `tokenizer.json`), written from the GGUF's metadata by
  `tools/secure/secure_tokenizer.py --profile gemma4 --gguf <file> --out <dir>` (header only;
  it refuses any other tokenizer model). `merges.json` is a JSON array of `[left, right]`
  pairs in rank order, because Gemma's merges hold newlines and tabs; a pack with a
  `merges.txt` is refused in this mode, and a qwen/glm pack with `merges.json` is refused in
  theirs. The mode comes from the profile, never from the files. On Windows the pack folder
  must be owner-only like every root (see the Windows notes).
- **Prompt:** the GGUF's template, byte for byte: `<bos>`, a system turn only for a leading
  system message or thinking on (`<|think|>` first), `<|turn>user\n…<turn|>\n`,
  `<|turn>model\n…<turn|>\n` for earlier answers, and the generation prompt `<|turn>model\n`
  plus the closed thought channel `<|channel>thought\n<channel|>` when thinking is off. Text is
  trimmed as Jinja's `trim` does; a Gemma control literal in any message, an empty message, a
  non-leading system message or a broken user/assistant order is refused (400).
- **Thinking (`reasoning_effort`):** accepted per request: `none` is thinking off, `low`,
  `medium` and `high` all mean thinking on (the template has no levels). A request without it
  takes `reasoning.effort` (`none` or unset: off; any level: on). The thought channel comes
  back as `reasoning_content`, the answer as `content`; `<turn|>` and `<eos>` end the reply.
- **Sampling:** set `defaults.sampling` to the model card's values (temperature 1.0, top_p 0.95,
  top_k 64; the GGUF's `general.sampling`) plus `min_p` 0.05. A request without a temperature is
  otherwise decoded greedily, and the model's thinking then tends to run on for thousands of
  tokens. Sampled at the model card's values the owner's (abliterated) file fell into a repetition
  loop in about 1 answer in 30 on the box, and in none of 32 with `min_p` 0.05 (session log item 55).
- **Not yet:** managed Gemma models (a `gemma4` manifest installs, but `run`/`serve` refuse it
  with "the Gemma 4 engine is not installed" until plan 4 opens the gate) and the
  per-family `engine.executables.gemma4` key (plan 4).

```sh
./out/build/linux-gcc/strata-secure-server --expert \
  --profile gemma4 \
  --models-root /absolute/path/to/models \
  --model-id gemma-4-26B-A4B-it-UD-Q8_K_XL \
  --tokenizer-pack /absolute/path/to/gemma4-pack/tokenizer \
  --engine /absolute/path/to/strata-gemma4-engine \
  --engine-arg --max-context --engine-arg 32768 \
  --engine-arg --kv --engine-arg f16 \
  --gpu 0 \
  --state-dir /absolute/path/to/owner-only-state-dir \
  --token-env STRATA_API_TOKEN
```

### flint profile

Select `profile: "flint"` on a model card (or `engine.profile = "flint"` with `--expert`) to serve RecursiveSI's
flint (`F:\models\flint-Q8_0\flint-Q8_0.gguf` on the owner's box) with our `multiengine_flint` library of
`multiengine.exe` (design `docs/superpowers/specs/2026-10-27-flint-engine-design.md`). It is text only, streamed or
not, on the same private `strata_0143` stdio protocol as Gemma 4 (flint's serve loop is Gemma 4's), with the strata
family's closed child environment.

- **Launch:** `engine.executable` names `multiengine.exe` beside `multiengine_flint.dll` (the launcher picks the library
  from the GGUF's arch); `engine.model_id` is the `.gguf` stem in `roots.models`. The server composes the allowlisted
  `engine.args` (`--max-context N` 1-262144, `--chunk N` 1-8192, `--no-graph`, `--expert-vram GiB`, `--expert-slots N`
  0-256, `--cpu-threads N`, `--pcie-frac` 0-1, `--cpu-lane on|off`, `--no-prompt-borrow`; the engine's test-only flags,
  Gemma's `--kv`/`--hybrid-rows` and the expert-profile paths are refused by name, anything unknown without being
  echoed), then `--gpu` from `engine.gpu`, then `--native <models_root/file>` last. The qwen-only rows are refused by
  name as on gemma4. Card options: `max_context`, `chunk`, `expert_vram`, `thinking` (off|on) and the sampling defaults.
- **Tokenizer pack (`flint_bpe`):** byte_bpe's files (`vocab.json`, `merges.txt`, `token_type.json`), written by
  `tools/secure/secure_tokenizer.py --gguf <file> --out <dir>` (header only; its `tokenizer.json` records flint's
  three pre-tokenizer regexes as `pre_patterns`). The frontend tokenizes exactly as llama.cpp master (`e86f5fd1`) does:
  its three flint regexes run in turn over llama.cpp's collapsed text (every `\p{L*}` class is the one letter class
  there, so a case change never splits a word; digits in threes from the left; CJK/kana runs apart), then byte-level
  BPE by rank; no normalization, no BOS. `tests/fixtures/flint_tokenizer_texts.json` holds 38 texts and 9 chats with
  llama.cpp's ids (`SECURE_FRONTEND_FLINT_PACK` names the pack for the real-pack cases). The pack folder must be
  owner-only (the Windows notes).
- **Prompt:** the GGUF's template byte for byte (llama.cpp's own Jinja rendering is the fixture): `<|system|>{text}`,
  `<|user|>{text}`, `<|assistant|>{answer}` for earlier answers, the generation prompt `<|assistant|>`; no newline, no
  trim, no BOS. System and user text go through the template's `flint_text` (`\0`, `<think>` and `</think>` removed,
  the seven framing literals written as `&lt;|...|&gt;`). Messages may come in any order, the last one the user's.
  Refused (400): an escaped text that still holds a marker (the escapes can rebuild `<think>`), an earlier answer
  holding one, any other special literal of the vocabulary (`<tool_call>`, `<|reserved_...|>`, ...), an empty text, a
  control byte.
- **Thinking (`reasoning_effort`):** the template has no switch: the model opens `<think>` itself, and its reasoning
  comes back as `reasoning_content`. `none` writes the closed block `<think></think>` after the generation prompt (a
  direct answer); any level, or none given, leaves the template's form. A request without it takes
  `reasoning.effort` (`none`: off; unset or a level: on). `<|end_of_text|>`, `<|user|>`, `<|observation|>` and
  `<|developer|>` end the reply (the GGUF's end-of-generation ids, the engine's own stops).
- **Sampling:** the card sets the GGUF's `general.sampling` (temperature 1.0, top_p 0.95, top_k 20).
- **Served:** `engine/fe-config/cards/flint-q8.json` in `engine/fe-config/multi-test.json`
  (`engine/serve-frontend-multi-test.bat`): prompt token counts equal llama.cpp's for the fixture chats; thinking on
  reasons then answers, `none` answers directly, a `<tool_call>` in user text is a 400, a stream sends reasoning then
  content.

### MiniMax profile

Select `profile: "minimax"` on a model card to serve MiniMax-M3.1-Flash (Maya's GGUF quants Maya-XS and Maya-S) with
Project Maya's `multiengine_maya_mm3` library of `multiengine.exe` (owner 2026-10-28; design
`docs/superpowers/specs/2026-10-27-strata-prepare-design.md` §7.3). Text only, streamed or not, on Maya's `maya_118`
stdio protocol (as DeepSeek's), with the strata family's closed child environment.

- **Licence:** the model and its chat template are under the MiniMax Community License (Copyright (c) 2026 MiniMax;
  `engine/third_party/maya/LICENSE.minimax-m31`, shipped beside the template in `engine/templates/minimax-m3.1/`):
  non-commercial use and redistribution with the notice kept; commercial use needs "Built with MiniMax M3.1-Flash" shown
  and a one-time notice to MiniMax (written authorization above USD 20 million a year in revenue).
- **Pack:** Maya's (maya.py `mm3_pack_step`): `strata-prepare download --maya --gguf <first shard> --out <pack>
  --template engine/templates/minimax-m3.1/chat_template.jinja` builds `mm3.txt`, `tensors.txt` and `tokenizer/` over the
  shards and the set's `*-maya-indexers.gguf` (the one whose `maya.indexers.for` names the set), read in place. Add a
  model puts it in `<folder>/maya-mm3` (`maya-mm3-<name>/` when that one indexes another model), a download in
  `<store>/packs/<id>`; the marker `minimax-m3.1-flash.gguf` is the card's model.
- **Launch:** `--pack <pack>`, the card's allowlisted `engine.args` (`--max-context` 1-1048576, `--kv-cache`
  bf16|int8|fp8|fp4, `--kv-streaming on|off`, `--kv-resident N`, `--chunk N`, `--cpu-threads N`, `--reserve-gb GiB`; the
  one-shot modes, the vision tower, the drafter, `--tp` and the paths are refused by name), then `--kv-streaming on`
  unless the card sets it, `--profile engine/profiles/expert-profile-mm3.bin` when the install has it, `--gpus` from
  `engine.gpu`, and `--vision off` (Maya's write_config_mm3 for a GGUF quant). Card options: `max_context` (Maya's
  32K-1M, 64K), `kv` (int8), `thinking` (auto, none, low, medium, high; auto), `temperature` 1.0, `top_p` 0.95.
- **Tokenizer (`minimax_bpe`):** the pack's byte_bpe files; each plain span NFC-normalized, then split by Maya's
  `MINIMAX_PATTERN` (GPT-4o's o200k regex, on the Python regex module's classes), then byte-level BPE by rank; no BOS.
- **Prompt:** Maya's template (MiniMax's own plus `thinking_mode` "disabled"), byte for byte against Maya's own
  renderer: the system turn (MiniMax's default text, the thinking instructions and mode, `<model_id>8bb8149fe</model_id>`,
  an optional `<effort>`), a developer turn (the first system message, or "You are a helpful assistant."), the turns as
  `]~b]user` / `]~b]ai` / `]~b]developer` with `[e~[`, earlier answers after `</mm:think>`, the generation prompt
  `]~b]ai\n`. The template's five literals are special tokens where the pack has them (Maya's parse_special); a
  message's are text.
- **Thinking (`reasoning_effort`):** `none` is thinking off (the template's "disabled": the reply is the answer); `low`,
  `medium`, `high` are MiniMax's `<effort>`; none given takes the card's `thinking` (`auto`: MiniMax decides, no effort
  named). The reply begins as reasoning, `<mm:think>` dropped and `</mm:think>` ending it (the newlines after it kept,
  as Maya's parser keeps them); `[e~[` and `]~b]` end the reply (server stops: STOP, then the drain).
- **Not yet:** an end-to-end run (no MiniMax GGUF on the development PC), MiniMax's tool calls (no web search) and
  pictures.
