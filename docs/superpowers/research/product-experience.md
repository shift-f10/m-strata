# Product-experience research (2026-10-13) — Ollama bar vs upstream Strata

Source: read-only research subagent; Ollama facts from public docs (URLs inline); Strata facts from setup.py, START-HERE.bat, setup.sh, docs/AI_SETUP.md, docs/INSTALL.md, serve/runconfig.py, README.md, serve/server.py (grep for --api-key / urlopen).

## 1. Ollama CLI surface

| Verb | Semantics |
|---|---|
| `run <model>` | Interactive chat or one-off prompt; pulls the model if missing (third-party confirmations only). |
| `serve` | Starts the server; `serve --help` lists env vars; only documented manual start. |
| `pull` / `rm` / `ls` (`list`) | Download, delete, list local models. |
| `ps` / `stop <model>` | List loaded models / unload one; models stay loaded 5 min (`OLLAMA_KEEP_ALIVE`). |
| `create -f Modelfile` | Derived model (base, system prompt, parameters). |
| `cp`, `show`, `push` | Exist; `push` needs sign-in. |
| `signin`/`signout`, `launch <app>` | Cloud account; `launch` configures Claude Code/Codex/OpenCode etc. against Ollama. |
| bare `ollama` | Prompts to run a model or connect an app. |

First run: `run` auto-starting the server is NOT documented; in practice the tray app (Windows), app (macOS) or systemd unit (Linux) already runs it. Models: macOS `~/.ollama/models`; Linux service `/usr/share/ollama/.ollama/models`; Windows `%USERPROFILE%\.ollama\models`; `OLLAMA_MODELS` overrides. Config is env vars only (`OLLAMA_HOST`, `OLLAMA_MODELS`, `OLLAMA_ORIGINS`, `OLLAMA_KEEP_ALIVE`, `OLLAMA_NO_CLOUD`; `~/.ollama/server.json`).

Install: Linux `curl -fsSL https://ollama.com/install.sh | sh` (root/sudo; `/usr/local/bin`; system user `ollama`; `/etc/systemd/system/ollama.service` with `Restart=always`; NO checksum/signature; installs NVIDIA `cuda-drivers` + DKMS if `nvidia-smi` fails). Windows: `OllamaSetup.exe` (Inno Setup, `PrivilegesRequired=lowest`, `%LOCALAPPDATA%\Programs\Ollama`, user PATH, tray app, Startup-folder .lnk). macOS: `Ollama.dmg`. API default `127.0.0.1:11434`; OpenAI-compatible `/v1/` (api_key "required but ignored"); Anthropic `/v1/messages` (no key validation); CORS via `OLLAMA_ORIGINS`. Updates: macOS/Windows auto-download then "Restart to update"; Linux re-run the script.

Conflicts with our contract: no auth on any listener (~12k exposed instances reported, blog.leakix.net/2026/02/ollama-exposed/); auto-updates; `curl|sh` without integrity check + driver installs + system unit; cloud features on by default, "limited usage metadata"; pulls from a mutable registry by tag.

## 2. Upstream Strata onboarding today

Steps: download/clone → `START-HERE.bat` / `./setup.sh` (or AI/MCP-driven) → launcher finds/installs Python 3.10+ (winget/python.org or sudo apt/dnf/pacman) → `.venv` → `setup.py` asks family/size by RAM (32 GB→Coder, 48/64→IQ2_XS, 96+→IQ3_S), context by VRAM, images, projection (`--yes` defaults) → pip installs requirements + NVIDIA CUDA wheels → engine: prebuilt zip from GitHub Releases (own tag, then `releases/latest` fallback) or build (VS Build Tools/CUDA Toolkit, llama.cpp zip at pinned commit; AMD: ROCm nightlies ~10 GB) → model 70–120 GB from Hugging Face at pinned revisions (`HF_REVISIONS` setup.py:63-69), resumable, + MTP draft; mirrors `HF_ENDPOINT` / `--source modelscope` (self-attested hashes) → writes `strata-<model>.json`, `run-<model>.bat/.sh`, starts server foreground, opens `http://127.0.0.1:8080`. Update: `UPDATE.bat`/`update.sh` = `git pull` + refresh engine/pip/configs.

On disk: program = Strata folder (`.venv/`, `engine/`, `third_party/`); run config `strata-<model>.json` (+`.bak`), `run-<model>.*`, `strata-<model>.log`; models `Strata-data/` (`models/`, `packs/`, `mtp/`) next to the folder or `--data-dir`; pointer `%APPDATA%\Strata\settings.json` / `~/.config/strata/settings.json`; chats and typed API key in browser localStorage.

Server config: run-config JSON (`exe`, `args`, `port`, `gpu`, `host`, `api_key`, `vision`, `sampling`, `mcp_servers`, `cors_origins`, ...); `serve/runconfig.py` exposes 15 "safe" keys via `/config` (excludes host, api_key, CORS, MCP). Flags `--host` (default 127.0.0.1), `--api-key` / `$STRATA_API_KEY`, checked on `/v1/*` as Bearer or `x-api-key` (server.py:2811, 6161). **README: "Any API key and any model name work"** — no key on loopback by default; key stored in PLAINTEXT in `strata-<model>.json`; Docker defaults `0.0.0.0:8080` with no key.

Network-touching steps: Python install (winget/python.org/distro); pip (PyPI, version-pinned not hash-pinned); ROCm nightlies; prebuilt engine (GitHub Releases, `latest` fallback; SHA-256 from releases API but **installs anyway if no digest**, setup.py:2766-2820); build from source (llama.cpp zip pinned commit; NVIDIA CUDA repo); model + MTP (HF pinned revisions, SHA-256; ModelScope self-attested); update (`git pull`); `--inspect <url>`; MCP servers; browser auto-open (local). No telemetry or update-check-at-start found.

## 3. Gap table

| Expectation | Ollama | Upstream Strata | Secure frontend today | Spec must add | Constraint |
|---|---|---|---|---|---|
| One binary | `ollama` | bat/sh + venv + engine exe | `strata-secure-server` + 5 required flags | single `strata` CLI with built-in defaults for models-root, tokenizer pack, engine, token | hermetic; nothing fetched at start |
| One install per OS | pkg/installer | zip/clone + setup.py | none | Linux .deb/.rpm or tarball + user unit; Windows per-user signed installer (no admin); both ship engine + CUDA runtime | digest-pinned; no `curl|sh`; no driver install (detect and tell); no pip at runtime |
| Zero-config first run | `run x` | questions + foreground window | every path named | `strata run <model>` starts loopback server if none, generates token, prints connect info | token on loopback always; bind 127.0.0.1 |
| Model mgmt | pull/ls/rm/show/cp/create | `--setup`, `--gguf-dir` | none | `pull` (manifest-pinned, resumable, SHA-256), `list`, `rm`, `show`; defer `create`/`cp` | explicit network only in `pull`/`update`; refuse unverified |
| Service/auto-start | system unit; app at login | none | none | opt-in `strata service install/enable` | no elevation; user; off by default |
| Status | `ps`, `stop` | `/health`, `/v1/status`, Monitor | `/health` | `strata ps`/`status`/`stop` with token | token on loopback |
| Key | none | optional plaintext JSON | `--token-env`/`--token-file` | `strata key show` / `key rotate` | real secret, protected storage |
| Uninstall | OS/manual | delete folders | none | `strata uninstall` / OS uninstaller, "keep models?" | remove units + tokens |
| Update | auto (Win/mac), script (Linux) | `git pull` + `latest` | none | `strata update`: signed manifest → diff → confirm → verify → atomic swap → keep previous | never automatic; no `latest`; fail closed without digest |
| Telemetry/cloud | cloud on by default | none seen | none | state "no network except pull/update"; default-deny egress test | no phone-home |

## 4. Token UX options

| Option | How | Pros | Cons |
|---|---|---|---|
| A. per-user protected file + CLI reads it | 256-bit random → `$XDG_CONFIG_HOME/strata/token` (0600, dir 0700) / `%APPDATA%\Strata\token` (ACL user+SYSTEM); server and CLI read it; `run`/`ps`/`stop` attach it | cross-platform, zero prompts, works with same-user service, matches `--token-file` | at-rest plaintext under OS perms (like `~/.ssh`) |
| B. `strata key show` | prints token, `--env` prints `export OPENAI_API_KEY=…` | explicit, scriptable | screen/shell history |
| C. first-run "connect your app" banner | base URL + token once + hint | Ollama-like immediacy | scrollback; never to log file |
| D. OS keychain (DPAPI / libsecret) | `CryptProtectData`; Secret Service | encrypted at rest; DPAPI free on Windows | libsecret absent headless / needs unlocked session; apps still need plaintext |
| E. per-app tokens | `key add <name>` / `revoke` | least privilege, revocable | more UX; defer |

Recommendation: A as source of truth + B + C on top; optionally DPAPI-wrap on Windows; `key rotate`; file format holds several named tokens for E later; never write token to logs/config JSON/`/config`/URLs; web UI gets it via same-origin session bootstrap (one-time code), not `?token=`.

## 5. Service / auto-start options

| OS | Option | Runs as | Start | Notes |
|---|---|---|---|---|
| Linux | systemd **user** unit `~/.config/systemd/user/strata.service` | user | login (boot with `loginctl enable-linger`) | no root; hardening `NoNewPrivileges`, `ProtectSystem=strict`, `ReadWritePaths=` data dir; `LimitMEMLOCK` may bind |
| Linux | system unit + `strata` user | service user | boot | root; token shared via group — weaker boundary; admin path only |
| Windows | Task Scheduler "At log on" for current user, not highest privileges | user | logon | no admin; upstream INSTALL.md:46-47 warns two task settings or 24x slower start |
| Windows | HKCU Run / Startup shortcut | user | logon | simplest; implies visible/tray process |
| Windows | Windows Service | LocalSystem | boot | admin; different identity breaks per-user DPAPI/ACLs; avoid as default |

Recommendation: default NO auto-start (`run`/`serve` on demand); opt-in `strata service enable` → user unit / per-user logon task; `service disable`; never elevate; writable roots = data dir + logs; system-wide mode documented admin path; uninstall removes unit/task + token.
