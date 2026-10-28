# Product experience and upstream tracking — design

Status: APPROVED by the owner 2026-10-13 19:03 UTC (all ten recommendations accepted)

Date: 2026-10-13. Sits between SP0 and SP1 (owner decision 4 in
[`docs/AGENT_HANDOFF.md`](../../AGENT_HANDOFF.md)). Inputs:
[`research/product-experience.md`](../research/product-experience.md) (cited
below as **PX**), [`research/sp1-config-paths-tokens.md`](../research/sp1-config-paths-tokens.md)
(**R1**), [`security/threat-model.md`](../../security/threat-model.md) (**TM**),
[`PROVENANCE.md`](../../PROVENANCE.md), [`tests/security/README.md`](../../../tests/security/README.md).

## 1. Purpose, the bar, non-goals

The owner's bar: "as easy as Ollama; not disjointed, not half-baked", with
launch, install and configuration both user-friendly and secure. Concretely: one
`strata` binary; `strata run <model>` works on a fresh install with nothing
configured; the server listens on loopback only, with an API token that is
always required but that the local user never types; one install per OS
(Linux, Windows; NVIDIA first); network use only when the user runs `pull` or
`update`. This spec defines the user-visible surface and the on-disk layout
that SP1–SP7 build toward, and the rules that keep upstream
`Niko1221/Strata` mergeable. It keeps every rule in TM; where it needs a TM
change (§7) it says so.

Where Ollama conflicts with our contract (no auth, auto-update, unverified
`curl | sh`, driver installs, cloud on by default, mutable tags — PX §1), we do
not follow it. Non-goals: cloud features, accounts or sign-in; `push`; automatic updates or
update checks; telemetry of any kind; MCP, tools, plugins or command execution
(TM "Extensions and MCP", EXT-01/02); a GUI or tray app; AMD/HIP and SYCL
packaging (later, same design); macOS.

## 2. The `strata` CLI surface

One binary, `strata` (`strata.exe`). "Server" below means the local `strata
serve` process, reached over loopback HTTP with the token read from the
protected store (§3). Only `pull`, `update` (and nothing else) may open a
network connection; `import` reads a local file.

| Verb | What it does | Talks to | Exit behavior | SP |
|---|---|---|---|---|
| `serve` | Runs the frontend + one supervised engine child in the foreground. Replaces today's `strata-secure-server` flag set (`secure_frontend/src/secure_server_main.cpp`) with config-file defaults. | Binds listener; filesystem (config, token, models) | Runs until signal/`stop`; non-zero on bad config before bind (CFG-01) | SP1 (config), SP2 (supervision) |
| `run <model> [prompt]` | With prompt: one completion to stdout, then exit. Without: interactive chat (line in, streamed text out; `/bye` or EOF ends). Starts a background server if none (§3). | Server over loopback + token | 0 on success; non-zero if model missing and user declines, server fails READY, or request fails | SP1 skeleton, SP2 auto-start, SP3 streaming |
| `pull <model>` | Fetches a model through the pinned manifest (§5). | **Network** (HTTPS manifest + artifacts), filesystem | Non-zero on any verification failure; nothing published (DL-03) | SP4 (after `import`) |
| `import <bundle>` | Verifies and publishes a pre-downloaded model bundle offline (§5). | Filesystem only | Same as `pull` | SP4 (first) |
| `list` | Models in the local index: name, tag, size, verified state. | Filesystem only (works with no server) | 0; non-zero if index is corrupt | SP1 (scan), SP4 (index) |
| `rm <model>` | Removes a model; refuses if the running server has it loaded unless `--force` after `stop`. | Filesystem; server (to ask what is loaded) | 0 / non-zero | SP4 |
| `show <model>` | Manifest entry: files, digests, sizes, profile, engine arguments it implies. Never prints paths outside the data dir. | Filesystem only | 0 / non-zero if unknown | SP4 |
| `ps` / `status` | Whether a server runs, its address, engine state (`/health`), loaded model. | Server; pid/port file | 0 if running, distinct non-zero if not | SP2 |
| `stop` | Asks the server to shut down cleanly (engine child and descendants terminated, PROC-05). | Server (authenticated shutdown route) | 0 when gone; non-zero after timeout | SP2 |
| `key show` | Prints the token; `--env` prints `OPENAI_API_KEY=…` lines for the shell. | Protected store | 0 / non-zero if none | SP1 |
| `key rotate` | New token, atomically replaced; tells the user to restart the server and reconfigure apps. | Protected store | 0 / non-zero | SP1 |
| `service enable\|disable\|status` | Opt-in per-user auto-start (§6). | systemd user / Task Scheduler | 0 / non-zero; never elevates | SP7 |
| `update` | Verified, confirmed program update (§7). | **Network** | Non-zero on any failure, previous version kept | SP4 mechanism, SP7 release side |
| `uninstall` | Stops server, removes service unit/task, token, config; asks "keep models?" (default keep). | Filesystem, service manager | 0 / non-zero | SP7 |
| `version` | Program, engine and manifest-format versions; build commit. | Nothing | 0 | SP1 |
| `doctor` | Offline diagnostics: GPU/driver/CUDA runtime presence, config validity, token file permissions, data-dir permissions, port free, model digests (on request). Prints what the user should install; installs nothing. | Filesystem, OS queries; **no network** | 0 if all pass; non-zero lists failures | SP7 (base checks in SP1) |

Deferred: `create`, `cp`, `push` (`push` needs an account, a non-goal). Bare
`strata` prints short help plus the `pull`/`import` hint if no model exists.
Every client verb attaches the token itself; no verb takes the token as an
argument (shell history, process list — CFG-05); no server code path reaches
`pull`/`update` networking (PRIV-03).

## 3. First run (zero-config)

On `strata run qwen3.8-flash-next` with nothing configured:

1. **Paths.** Resolve the per-user directories (table below). Create missing
   directories with user-only permissions (0700 on Linux; on Windows a protected
   DACL for the user and SYSTEM, matching `has_private_owner_dacl` in
   `secure_frontend/src/token_reader.cpp`).
2. **Config.** If `config.json` is absent, write it with built-in defaults
   (§4): host `127.0.0.1`, port `8080` (the frontend default today,
   `config.hpp` `kDefaultPort`), roots under the data dir, `token_source =
   "file:token"`. Written via temp file + atomic rename (R1 §5).
3. **Token.** If the token file is absent: 32 bytes from the OS CSPRNG
   (`getrandom` / `BCryptGenRandom`, R1 §5), encoded base64url without padding,
   written once with `O_CREAT|O_EXCL|O_NOFOLLOW`, mode 0600, `fsync`, rename
   (Linux) or `CreateFileW(CREATE_NEW)` with SDDL
   `D:P(A;;FA;;;<UserSID>)(A;;FA;;;SY)` (Windows) — PX §4 option A. DPAPI
   wrapping on Windows is an open question (§12). The writer adds no trailing
   newline and the reader accepts one (fixes R1 defect c).
4. **Find or start the server.** Read `server.json` (pid, port, start time,
   program version) from the runtime location. If the pid is alive and
   `GET /health` with the token answers, reuse it. Otherwise spawn `strata
   serve` detached (same user, no shell, no elevation), which writes
   `server.json` atomically after `bind()` succeeds and removes it on clean
   exit. A stale file (dead pid, or health fails) is replaced. Exactly one
   server per user: `serve` takes an exclusive lock on `server.lock` before
   binding.
5. **Wait for READY.** `run` polls `/health` until `engine` is `ready` (bounded
   by `ready_timeout`; SP2 makes READY eager — today it is consumed on the first
   request and `starting` returns 200, AGENT_HANDOFF "Known small gaps"). Each
   poll is authenticated; a 401 means a token mismatch and is reported as such.
6. **Model missing.** If the model is not in the local index: print the
   manifest source, the file list with sizes and SHA-256 digests, and total
   bytes, then ask `Download 87.4 GB from <source>? [y/N]`. Default is No.
   Non-interactive stdin means No and exit non-zero with the `strata pull`
   command to run. Until networked `pull` ships, the hint is `strata import`.
7. **Banner, once.** After the first successful start, print to the terminal
   only (never to a log file):
   `Strata is running at http://127.0.0.1:8080/v1 (OpenAI-compatible).`
   `Connect an app: use that base URL and the key from 'strata key show'.`
   The token itself is not printed (PX §4 option C, modified: base URL plus hint
   only). A marker `banner_shown` in the state dir suppresses it afterwards.

How the mandatory token stays invisible: the CLI reads it from the protected
file for every request; the server reads the same file at start (existing
`--token-file`/`--secret-root` path in `secure_server_main.cpp`); third-party
apps get it once via `strata key show`. The user never types it, and it never
appears in config, argv, environment of the child, logs, `/config`-style
routes or URLs (CFG-05, CFG-06).

### Where things live

| Item | Linux | Windows |
|---|---|---|
| Config | `$XDG_CONFIG_HOME/strata/config.json` (default `~/.config/strata/`) | `%APPDATA%\Strata\config.json` |
| Token | `$XDG_CONFIG_HOME/strata/token` (0600, dir 0700) | `%APPDATA%\Strata\token` (ACL user + SYSTEM) |
| Models + index | `$XDG_DATA_HOME/strata/models/` (default `~/.local/share/strata/models/`) | `%LOCALAPPDATA%\Strata\models\` |
| Download staging | `$XDG_DATA_HOME/strata/staging/` (same filesystem as models, for atomic rename) | `%LOCALAPPDATA%\Strata\staging\` |
| Logs (off by default) | `$XDG_STATE_HOME/strata/logs/` (default `~/.local/state/strata/`) | `%LOCALAPPDATA%\Strata\logs\` |
| Runtime (`server.json`, `server.lock`) | `$XDG_RUNTIME_DIR/strata/`, else the state dir | `%LOCALAPPDATA%\Strata\run\` |
| Program | `~/.local/lib/strata/<version>/`, `~/.local/bin/strata` → current | `%LOCALAPPDATA%\Programs\Strata\<version>\` |

Models stay out of the roaming `%APPDATA%`. `STRATA_CONFIG` (§4) can point at
another config file, where every root can be changed. Upstream keeps models next to the program folder and a
pointer in `%APPDATA%\Strata\settings.json` / `~/.config/strata/settings.json`
(PX §2); we do not read those files.

## 4. Configuration model (the contract SP1 implements)

**Format: JSON**, strict. Reasons: the frontend is dependency-free and already
has a bounded JSON reader for chat requests (`secure_frontend/src/chat_schema.cpp`);
upstream's run config is JSON too (R1 §6), which keeps mapping simple. Cost: no
comments — mitigated by a documented schema shipped with the program and by
`doctor` naming the offending key. TOML is the alternative (§12).
No YAML.

Rules: one file; schema-validated before any bind or spawn; unknown keys,
duplicate keys, wrong types and out-of-range values are errors (CFG-01); bounded
size (64 KiB) and nesting; file must be owned by the user and not writable by
group/other (TM "Configuration and filesystem"). The file holds no secret:
only `token_source` metadata (`"file:token"` relative to the config dir, or
`"env:NAME"` for operator setups), which maps onto the existing `SecretSource`
(`secure_frontend/include/secure_frontend/secret_source.hpp`). A `schema_version`
integer is required; `update` migrates it explicitly, never silently.

**Precedence:** CLI flag > config file > built-in default. The only environment
variable read is `STRATA_CONFIG` (path of the config file). Reason: env vars are
inherited by every child and visible in `/proc/<pid>/environ` (R1 §2); one
documented source per value makes `doctor` output trustworthy. Upstream's
`STRATA_*` env knobs (R1 §6) are not honored; `doctor` warns if any are set.

### Key mapping from upstream's run config (R1 §6)

| Upstream key / env | Ours | Decision |
|---|---|---|
| `host` / `--host` (127.0.0.1) | `listen.host` | Kept. Non-loopback needs `listen.allow_remote`; wildcard needs `listen.allow_wildcard_bind` too (existing `validate_config`, fix R1 defect a in SP1) |
| `port` / `--port` (8095) | `listen.port` | Kept; default stays 8080 (current native default) |
| `api_key` / `STRATA_API_KEY` | `token_source` | Renamed; value never in config |
| `allowed_hosts`, `cors_origins`, `trusted_origins` | `http.allowed_hosts`, `http.cors_origins` | Kept, exact match only; CORS empty by default; never `*` (WEB-04/05) |
| `exe`, `args`, `cwd`, `env`, `log` | — | Excluded as free-form. Engine path is fixed by the install; engine arguments come from the model manifest (§5); child env is a typed allowlist (SP2) |
| `tokenizer` | from model manifest | Moved into the manifest |
| `model_name`, `aliases` | `default_model`; manifest `name[:tag]` | Renamed |
| `gpu`, `backend`, `threads` | `engine.gpu`, `engine.threads` | Kept as typed values; backend fixed by the installed build |
| `sampling.*` | `defaults.sampling.*` | Kept (request may override within bounds) |
| `reasoning_*`, `anthropic_thinking`, `fit_max_tokens` | — | Deferred to SP5 parity |
| `parallel`, `replicas` | `limits.concurrency` | Concurrency in SP3; replicas deferred |
| `lazy_load`, `idle_unload_s`, `min_free_vram_mib`, `vram_reserve_mib` | `engine.idle_unload_s`, … | Deferred to SP2/SP5 |
| `vision.*` | — | Deferred (native rejects vision today) |
| `before_load` | — | **Excluded** (runs a command) |
| `mcp_servers` / `--mcp-config` | — | **Excluded** (EXT-02: present key = error) |
| `slot_save_path` | — | Deferred |
| `api_monitor`, `open_browser`, `prevent_sleep` | — | Excluded (no web UI in this spec) |
| `engine_silence_s`, `STRATA_ENGINE_READY_S`, `STRATA_ENGINE_STALL_S` | `limits.generation_timeout_s`, `limits.ready_timeout_s` | Kept as bounded typed values (native defaults 120 s / 30 s; upstream 300 s / 900 s — SP2 picks) |
| `STRATA_MAX_BODY_MIB`, `STRATA_HTTP_BACKLOG` | `limits.max_request_bytes`, `limits.backlog` | Kept, clamped; SP1 must make the parser honor them (R1 defect d) |
| `STRATA_DEBUG`, trace env vars | `logging.level` | Replaced; logging off by default, redacting (TM, PRIV-02) |

New keys of ours: `paths.models`, `paths.logs`, `paths.staging` (each must be
canonical and below an allowed root, CFG-02/03/04); `pull.manifest_sources`
(list of HTTPS URLs plus pinned digest of the signing key, §5); `update.source`.

## 5. Model management

**Identity.** A model is `name[:tag]` (tag defaults to `default`). Names follow
the existing `validate_model_id` grammar (`secure_frontend/src/model_inventory.cpp`).
A name resolves to a **manifest entry**: profile (`qwen` / `glm53`), engine
build compatibility, and a file list where each file has role, relative path,
byte size and SHA-256. Roles cover what the engine loads: `native` shard(s),
`pack` (expert pack directory contents), `ple_gguf`, `embd_gguf`,
`native_head_gguf`, `native_dense_gguf`, `mtp`, `tokenizer_pack`.

**Local index.** `models/index.json`, written atomically, lists installed
entries with the manifest they came from. The existing `.gguf` scan
(`enumerate_local_models`) stays as a consistency check: a file present but not
indexed is reported by `list --all` and `doctor`, never loaded.

**Launch composition (fixes the SP0 review's Critical).** Today
`build_engine_arguments` (`secure_frontend/src/engine_arguments.cpp`) supplies
only `--native` from the inventory; `--pack` (default cwd-relative `pack/full`)
and the other weight flags are operator-supplied pass-through (AGENT_HANDOFF M1/M2
row). Under this spec the manifest entry supplies every weight source: the
frontend maps each role to its flag with a canonical path below `paths.models`,
and the pass-through for weight flags is removed for the managed path.
Operator-supplied `--engine-arg` remains only under an explicit `serve
--expert` mode for SP5/SP6 bring-up, and never for `run`.

**`pull` semantics** (TM "Artifact acquisition"; DL-01..04):

- Manifest sources are pinned in config; HTTPS only; no `latest`, no mutable
  tag lookup at runtime — the manifest itself carries digests.
- Before transfer: print source, files, sizes, digests; require confirmation
  (`--yes` for scripts).
- Each file: stream into `staging/<entry>/<file>.part`, enforce the manifest
  byte limit, resume by HTTP range when the partial prefix is still consistent,
  SHA-256 over the whole file at the end.
- Redirects stay HTTPS and on the allowlist; no archives in the first release
  (TM).
- All files verified → atomic rename of the staging directory into
  `models/<name>/<tag>/`, then index update. Any failure → nothing published,
  previous version intact (DL-03); interrupted staging is never selected (DL-04).
- `pull` is never reachable from the HTTP API or from `run` without the
  confirmation (PRIV-03).

**`import <bundle>`** — offline first. A bundle is a directory (or USB copy)
containing a signed manifest and the files. `import` verifies signature,
sizes and SHA-256 locally, copies into staging, and publishes atomically.
Same code path as `pull` after the transfer step, and DL-02..04 apply with a
local fixture. It serves the offline Linux and Windows test boxes directly.

**Open design question for SP4: TLS without third-party dependencies.** The
frontend is dependency-free (`docs/security/deployment.md`). Windows has
Schannel/WinHTTP in the OS. Linux has no OS TLS API. Options: (a) bundle one
vetted TLS library (a change to the dependency policy, needs SBOM/review);
(b) `pull` from a local/LAN mirror only and rely on `import` for everything
else; (c) ship no networked `pull` on Linux at first and use `import` plus
an out-of-band download verified by `strata import`. **Recommendation:** ship
`strata import` before networked `pull` on both OSes; decide (a) vs (b) in the
SP4 spec.

`list`/`show` read the index only; `rm` updates the index, then deletes.

## 6. Service and auto-start

Default: none. The server runs when `run` or `serve` starts it (PX §5).

`strata service enable`:

- Linux: writes `~/.config/systemd/user/strata.service` (`ExecStart=<program>
  serve`, `NoNewPrivileges=yes`, `ProtectSystem=strict`, `ProtectHome=read-only`,
  `ReadWritePaths=` the data, state and runtime dirs, `PrivateTmp=yes`,
  `RestrictAddressFamilies=AF_UNIX AF_INET AF_INET6`, bounded `Restart=on-failure`
  with `RestartSec`), then `systemctl --user enable --now`. Boot-time start needs
  `loginctl enable-linger`, which `service enable` explains and does not run.
  `LimitMEMLOCK` may need raising for pinned memory (PX §5); `doctor` checks it.
- Windows: a Task Scheduler task "At log on" for the current user, not "run with
  highest privileges", action `strata.exe serve`; set the two task settings
  upstream warns about (PX §5, upstream `docs/INSTALL.md`:46-47).
- Never elevates; no system unit or Windows Service by default (a different
  identity breaks per-user ACLs and DPAPI; PX §5). `service disable` removes the
  unit/task; `uninstall` calls it first.

## 7. Update

`strata update`:

1. Fetch the release manifest from `update.source` (HTTPS) and its signature;
   verify the signature against the public key pinned in the installed program.
2. Show current vs offered version, changed components (frontend, engine, CUDA
   runtime), sizes, digests and the release notes link; ask to confirm.
3. Download into staging; verify every SHA-256.
4. Install side by side into `<program>/<new-version>/`; switch the `current`
   pointer atomically (symlink rename on Linux; a pointer file read by a tiny
   launcher, or a versioned Start-menu/PATH entry, on Windows — SP7 picks).
5. Keep the previous version; `strata update --rollback` switches back. Running
   servers are told to restart; nothing restarts on its own.

Never at start, never in the background, never prompted by the server.
No `latest` fallback: no signed manifest, no update (PX §3).

**TM contradiction to resolve under review:** TM "Privacy and offline operation"
says "The sole future networked operation is an explicit, foreground `acquire`
command". The owner decided on `strata update`. TM must be amended, with
review, to list `pull` (the product name for `acquire`) and `update` as the two
foreground operations, both manifest-verified and both unreachable from the
HTTP API, models, plugins or any automatic logic. AGENT_HANDOFF already records
this gap. PRIV-03 and PRIV-04 then cover `update` as well.

## 8. Install per OS

| | Linux | Windows |
|---|---|---|
| Artifact | Tarball (and a `.deb`) holding `strata`, the engine, the CUDA runtime libraries it needs, `SHA256SUMS` and signature | Per-user installer (`PrivilegesRequired=lowest` equivalent), signed |
| Location | User level by default (`~/.local/...`, §3); `.deb` is the admin path | `%LOCALAPPDATA%\Programs\Strata\` and user PATH |
| Network during install | None. No post-install script fetches anything | None |
| Auto-start | No | No, unless the user ticks "start at logon" (→ `service enable`) |
| Models | Not bundled; `import`/`pull` after install | Same |

The installer must NOT: install GPU drivers or DKMS modules, add package
repositories, run `pip`, install Python, pipe anything to a shell, phone home,
create a system service, or request elevation. (Each of these is what Ollama's
Linux script or upstream's `setup.py` does today, PX §1–2.) The source tree's
upstream Python service, installer and update scripts are quarantined at package
time by `scripts/secure_distribution_check.py --release`.

CUDA driver: the package ships the CUDA runtime but not the kernel driver.
`strata doctor` (and `run` on engine start failure) reports the detected driver
version, the minimum needed, and the vendor's install instruction for the OS;
it does not download or install anything.

## 9. Upstream tracking

> **Superseded (2026-10-13)** by
> [`2026-10-13-upstream-submodule-layout-design.md`](2026-10-13-upstream-submodule-layout-design.md):
> upstream is the pinned, unmodified submodule `upstream/`. The approved-edit
> list, `scripts/dev/upstream-delta.sh`, the `upstream-delta` CI job, the merge
> runbook, the ledger and the tokenizer exception below no longer apply; an
> upstream update is a submodule bump plus the gates (`docs/AGENT_HANDOFF.md`,
> "Update upstream"). The "Remotes" mechanics bullet is superseded too: the
> `upstream` git remote may still exist in a dev clone, but tracking is by the
> submodule pointer, never by merging that remote. The GLM-engine and
> SP5-parity bullets still hold, with `src/` read as `upstream/src/`.

Rules (owner decision 6): our code in our directories; upstream files touched
only from the PROVENANCE allowlist (root `CMakeLists.txt` integration hunk,
`.gitignore` appends, `requirements.txt` if needed); upstream `serve/`,
`setup.py`, `src/`, `include/` untouched; the Python service stays in-tree and
is quarantined at package time; the GLM engine lives in its own directory; SP5
parity wraps the unmodified engine.

Mechanics:

- Remotes: `upstream` = `https://github.com/Niko1221/Strata.git` (exists);
  `glm-upstream` = `https://github.com/lighttransport/Strata.git`, branch
  `glm53f` (to add in SP6); `maya-upstream` =
  `https://github.com/mw00/project-maya.git` (owner-added second GLM-5.3
  source, 2026-10-14; pinned in `docs/PROVENANCE.md`, to add in SP6).
- Pinned tag: PROVENANCE "Baseline" (today `v0.1.43` / `7695859a`); a merge
  updates it in the same commit.
- **`scripts/dev/upstream-delta.sh`** (new, SP1 or tooling): runs `git diff
  --name-only <pinned-upstream-commit> HEAD`, removes paths under our
  directories (the PROVENANCE list), and fails if any remaining path is not on
  the allowlist or the recorded-exception list. For allowlisted
  `CMakeLists.txt`, it also fails if the diff touches lines outside the
  integration hunk. Output names each offending file.
- **CI job** `upstream-delta` in `.github/workflows/secure-frontend.yml`,
  blocking, runs the script on every push (needs `fetch-depth: 0` and the pinned
  commit reachable).
- **Merge runbook:** `git fetch upstream`; `git merge <new tag>` on a branch;
  resolve conflicts only in allowlisted files; run the delta script; run every
  gate (`pytest -q`, `ctest --preset linux-gcc`, the
  `windows-llvm-mingw-cross` build, `pytest serve`); update PROVENANCE baseline
  and touch table; re-check the SP5 pass-through table (below) against the new
  engine's flags and protocol.
- **Ledger:** PROVENANCE is the only list of touches; the script reads it.
- **Known exception:** `tools/strata_tokenizer.py` (35 lines, `7de35b0c`).
  Recommended: move the GLM aliases into our own module (for example
  `secure_frontend/tools/` or `tools/secure/`) that imports upstream's
  tokenizer, and restore upstream's file; offer the aliases upstream as a pull
  request in parallel. Until then the delta script lists it as a recorded
  exception.
- **GLM engine:** `engines/glm/`, imported as a subtree (or a pinned
  submodule — SP6 picks) from `glm-upstream`/`glm53f`, never merged into
  `src/`. Its own PROVENANCE section records commit and local changes.
- **SP5 parity:** a typed pass-through table maps each supported engine flag to
  a bounded typed value; unknown flags are rejected; the engine is unmodified.

## 10. Security invariants this spec keeps

- Token on every listener incl. loopback; CSPRNG; constant-time; never logged;
  no dev mode added — TM "Network and authentication"; AUTH-01..04, CFG-05.
- Loopback default; remote and wildcard need separate opt-ins — NET-01..03.
- Schema-validated config, unknown keys errors, metadata-only secrets, canonical
  roots — CFG-01..04, CFG-06.
- Network only in foreground `pull`/`update`, never from the API or at start —
  DL-01, PRIV-01, PRIV-03 (TM amendment, §7).
- Verified, staged, atomic artifacts; SBOM and pinned provenance — DL-02..05.
- No telemetry, update check or crash upload — PRIV-01, PRIV-04.
- No MCP/tools/plugins; `mcp_servers`/`before_load` are config errors — EXT-01/02.
- No elevation by installer, service or child — PROC-06. Hermetic tests only.

## 11. Sub-project mapping

| SP | Delivers from this spec |
|---|---|
| SP1 | `strata` binary skeleton with `serve`, `version`, `key show/rotate`, `list` (scan); config file (§4) with validation and precedence; per-OS paths (§3); CSPRNG token + protected store; first-run steps 1–3 and the banner; NET-03 fix; base `doctor` checks |
| SP2 | Background start for `run`, `server.json`/lock, eager READY, `ps`/`status`, `stop` (authenticated shutdown), restart/backoff, typed child env |
| SP3 | Streaming and cancellation needed by interactive `run`; concurrency limits; NET/WEB/DOS/EXT tests |
| SP4 | Manifest format and index; `import` first, then networked `pull`; `rm`, `show`; manifest-composed engine launch (§5); `update` download/verify/swap mechanism; TLS decision; DL-* tests; TM amendment |
| SP5 | Typed pass-through table, profiles and parity behind the same CLI (NVIDIA host) |
| SP6 | `engines/glm/` tracking `glm53f`; GLM manifest entries |
| SP7 | Linux tarball/.deb and Windows per-user installer; `service enable/disable/status`; `uninstall`; full `doctor`; release manifest + signing; SBOM; offline GPU validation kit |
| Tooling (any SP, recommend SP1) | Done by the upstream-submodule layout: no delta script needed; tokenizer aliases moved to `tools/secure/secure_tokenizer.py` |

## 12. Open questions for the owner

1. **Config format: JSON or TOML?** Recommend JSON (reuses the bounded parser,
   matches upstream's run config, no new dependency); TOML would be friendlier
   to hand-edit but needs a new parser.
2. **Does `run` start a background server, or run the engine in-process?**
   Recommend background `strata serve` (one model load shared by `run` and apps,
   matches the Ollama feel, keeps one listener with one token check).
3. **`/health` `starting`: 200 or 503 once SP2 has eager READY?** Recommend 503
   with `engine=starting` (as AGENT_HANDOFF already plans), and `run` waits on it.
4. **Models under XDG data / `%LOCALAPPDATA%`, or next to config?** Recommend XDG
   data / `%LOCALAPPDATA%` (large, non-roaming, separately configurable).
5. **DPAPI on Windows from day one?** Recommend yes, if SP1's Windows runtime
   test (MSYS2 job) can cover it; otherwise ACL-only file first and DPAPI before
   SP7 release.
6. **Keep `strata-secure-server` as a name?** Recommend `strata serve` as the
   only user-facing entry; keep `strata-secure-server` as a developer/test
   executable until SP2, then remove it.
