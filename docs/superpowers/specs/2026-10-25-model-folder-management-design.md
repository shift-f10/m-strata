# Model-folder management — design (2026-10-25, owner request)

What the owner wants:
1. Downloaded models must NOT stay in the download folder — they move into a **models folder we choose**.
2. The WebUI must let us **select/point the engine at the folder** where the model files are, so if we move them we
   just re-point (no re-download).
3. **(Emphasized 2026-10-25) Moving a model must NEVER require rebuilding/recompiling the engine — it is a JSON
   config edit, full stop.**

## The no-recompile guarantee (requirement 3) — already true today, verified 2026-10-25
Model locations are **runtime config, never compile-time**:
- **Engine (`strata.exe`):** no model path is compiled in (`grep` of the engine + GLM-DLL CMakeLists for
  `configure_file`/`MODEL_*`/baked paths = empty). The model path arrives as a **runtime arg** (`--pack DIR`,
  `--native FILE`, `--glm-pack DIR`, tokenizer/expert-profile paths) supplied by a **JSON serve config** —
  `engine/our-glm.json`, `engine/our-qwen.json` (keys: `args[]` holding the path, `tokenizer`, `cwd`). Move a model
  → edit those JSON strings → re-run. No recompile. Every new family (raon1, flint) routes the same way
  (`generate.cpp` reads the family + path from the GGUF/args), so each is a JSON path edit too.
- **Frontend (`secure_frontend`):** `roots.models` + per-model cards are JSON/config rows
  (`frontend_config.cpp:104`, `:368`); re-point = edit the config (or the WebUI control in §B). No recompile.
- **The ONE compile-time path is unrelated to models:** `STRATA_GGML_DIR` in the build cache points at the ggml
  toolchain (now `G:\Strata\third_party\llama.cpp`). That is a **build-of-the-engine** input, touched only when the
  *toolchain* moves, never when a *model* moves. Keep these two concepts separate in any doc/UI: "models folder" =
  runtime JSON; "ggml dir" = build-time, dev-only.

Net: requirement 3 holds now. §A/§B below make the *models folder* itself a first-class, re-pointable JSON setting
(engine serve config + frontend `roots.models`), with the WebUI able to change it live.

## How it works today (grounded in secure_frontend)
- `pull` downloads into a **staging** area then **installs (moves) into `roots.models`** under a lock
  (`src/acquire_pull.cpp:344` "Destination: roots.models … under .lock"; `tidy_staging` cleans staging). So the
  move-after-download mechanism already exists.
- `models.downloads_root` is "the download store (strata's model-store layout); strata's verbs use it as
  roots.models". In the owner's current setup **`roots.models` IS the downloads folder**, so installs "stay in the
  download folder" — that's the root of complaint #1.
- `roots.models` is **config-file-only** today; the `web/` UI (`index.html`/`app.js`) is the chat page, with no
  settings/folder surface — that's complaint #2.
- Path policy: the models folder must be local (not a network share / OneDrive / redirected folder —
  `canonical_path_common.cpp:197`), owner-only unless `roots.allow_untrusted_media` is set (the exFAT opt-in).

## Design
### A. Separate "downloads/staging" from the chosen "models folder"
- Keep `models.downloads_root` as the **staging/download** area only.
- Make `roots.models` a **distinct, owner-chosen models folder**. `pull`/`import` download+verify into staging,
  then **move** the finished model into `roots.models` (the existing install step) and clean staging. Net: a
  downloaded model ends up in the models folder, never left in downloads.
- Default: if the two are unset, keep today's behavior; but the config (and the WebUI below) can point
  `roots.models` at any chosen local folder.

### B. WebUI "Models folder" control (point / re-point)
- Add a **settings panel** (or a field in a Models tab) to the chat page: an **absolute local path** text input for
  the models folder, a "Use this folder" button, and a "Re-scan" button. (A browser can't read an OS folder path
  from a native picker for security, so it's a typed/pasted absolute path — matching the frontend's path model. A
  server-side folder browser that lists subdirectories under a root is an optional later add.)
- New authenticated endpoints on `strata-unified-server`:
  - `GET /v1/models/root` → the current `roots.models` + the enumerated models there.
  - `POST /v1/models/root` `{ "path": "<absolute local dir>" }` → validate (same path policy: local, owner-only or
    relaxed), re-open the `CanonicalRoot`, **re-enumerate** (`enumerate_local_models`), remap the served model, and
    **persist** the new path to the config (atomic, owner-only, like `init`). Returns the new model list or a
    precise error (e.g. "not a directory", "network/redirected folder", "no models found").
- **Re-point after a manual move:** the owner moves the model files to a new folder, types the new path, hits "Use
  this folder" → the server re-points `roots.models` + re-enumerates. No re-download.
- Runtime apply: re-opening the root + re-enumerating is in-process (no engine reload needed to CHANGE the folder
  list); switching the *active* model still goes through the supervisor's launch. If a safe in-process swap of
  `roots.models` is awkward, fall back to a graceful listener reload with the updated config.

### C. Security / correctness
- The chosen folder runs the existing path policy; with `roots.allow_untrusted_media` it tolerates exFAT/FAT and
  junction-mounted media (the opt-in added 2026-10-25). Network/redirected folders stay refused.
- Persisting the config stays owner-only + atomic (reuse `init`'s writer). The token/secret roots are unaffected.

## Build order (once paths/box are back)
1. Config: ensure `roots.models` ≠ `models.downloads_root` by default; document both.
2. `strata-unified-server`: add `GET`/`POST /v1/models/root` (validate → re-open → re-enumerate → persist).
3. WebUI: a settings panel with the models-folder input + "Use this folder" + "Re-scan", wired to the endpoints.
4. Verify: download → lands in the models folder (not downloads); move files + re-point → models reappear, serve.
