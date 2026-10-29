# GUI-first install: one out folder, setup on the page, a folder per model, calibrate - design (2026-10-27)

## 0. What the owner asked (2026-10-27, verbatim where quoted)

1. "ensure the start up is as easy as running the binary" - "configuration and start can be done under the models
   tab", "but still leave room for a json configuration".
2. "We should be able to specify folder location per model when downloading or selecting an image, just in case we
   move the models elsewhere. It should update the cards automatically if we configure it."
3. "a make script that will build all the binaries and engines and output them to a desired folder and from there we
   can just launch the secure-strata-server".
4. "an out folder for what we only truly need to run our frontend and engines once the make is completely done, that
   way the users can just copy those files out and they have a clean only needed files".
5. "in the models page, we need to add a calibrate feature per model and a recalibrate option when settings changes.
   Refer to the upstream strata and project maya on what calibrate does."
6. "our target are for both windows and linux ... write most of the code for linux as well".

Decisions asked and answered (2026-10-27):
- **D1 first login: a one-time link.** The console prints a one-time login link (loopback only, 10 minutes, single
  use); opening it shows the key on the page once and logs the browser in. The key is never printed or logged.
- **D2 data folder: `data/` beside the binaries.** Portable: moving the out folder moves the install. `--data-dir` or
  `--config` point elsewhere.
- **D3 cuBLAS is bundled** in `out/engine` (NVIDIA lists cuBLAS among the CUDA toolkit's redistributable files), so
  the folder runs on any PC with an NVIDIA driver.
- **D4 (the owner's request is the ruling): the page may name a model folder.** This reverses the model launcher
  spec's D1 ("cards stay operator files") and narrows the threat model's "the page never supplies a path": a model
  folder (data, never an executable, an environment variable or an argument) may come from the page, only on the
  loopback-only control routes, and only through the same root checks every configured folder passes.

## 1. Where things stand (read 2026-10-27; file:line in the research notes of this session)

- The server needs a config: `roots.models`, `engine.executable`, `engine.model_id`, `roots.secret`, an existing
  owner-only `roots.state`, a token made by `init`; `web_ui`, `monitor` and `models.control` default off. The launcher
  refuses to boot with no card. `init` writes only single-model configs. The key is never shown by the server.
- Cards are operator files (`models.cards`) plus server-written download cards in `<state>/cards`; the launcher can
  add a card at runtime but not replace or remove one; the page never sees a path.
- Downloads land in one `downloads_root` store (`<root>/<id>/<tag>/<v-hex>/`); only prepared Qwen downloads get a
  card. The config writer cannot render decimals.
- There are no CMake `install()` rules; the windows preset is a test-hooks build ("never ship this binary"); the
  release scripts know neither `multiengine` nor its family libraries; the engine builds only from `.bat` files.
- Upstream Strata's calibrate (`upstream/tools/calibrate.py`, wrapped by `setup.py --calibrate`,
  `strata engine calibrate`, `strata_control/calibration*.py`) is a **decode-speed sweep** of machine-dependent flags:
  `--pcie-frac`, `--spec-min-p`, `--pool-workers`, `--adapt-every/-swaps/-decay` (and `--prefill-pipe` with a layer
  split): three fixed prompts, 128 new tokens, median decode tok/s; a candidate is kept only when more than 3% faster,
  confirmed in interleaved rounds; results go into the run config's flags and a store keyed by GPU names + VRAM, CPU,
  RAM, model and context. Its newer tuner (`strata tune`, presets.py) marks a preset hard-stale (GPU/CPU/RAM/model/
  backend changed: not applied, re-tune) or soft-stale (driver/engine epoch/age: applied, recheck offered).
- Project Maya's calibrate (`maya.py --calibrate`, `tools/calibrate_glm.py`, `calibrate_ds4_tp.py`,
  `calibrate_draft.py`) is the same idea for its engines: GLM sweeps the PCIe share, CPU-lane threads and disk read
  chunks per request; DeepSeek sweeps `--tp` and `--cpu-threads` with one start per candidate; draft calibration
  compares drafting on and off. Keep a setting only when 3-5% faster; key = GPU names, VRAM, CPU, RAM, quant,
  context; the engines also keep their own timing caches beside the pack.

## 2. The out folder and the make script

`make.py` at the repository root (Python 3.10+, standard library only), with `make.bat` and `make.sh` that call it:

```
make --out <dir> [--ggml-dir <llama.cpp>] [--cuda-root <toolkit>] [--cuda-arch native|"86;89"]
     [--without maya,raon1] [--with email] [--no-cuda-runtime] [--build-dir <dir>] [--engine-build-dir <dir>]
     [--jobs N] [--cmake <exe>] [--plan]
```

(As built: strata, gemma4 and flint are in every engine; `--without` leaves out the optional families; `--with
email` builds the email alerts in (5a); `--plan` prints the commands only; `--out` must not exist or be empty, and
make never deletes.)

It configures and builds the engine (Release, CUDA, the chosen families, test programs off) and the frontend (a new
release preset per platform with test hooks off), installs both with CMake `install()` rules of a `runtime`
component into a staging folder, checks it (`scripts/secure_distribution_check.py`: no `.py`, no symlink, no hooks
build, no test program), copies the bundled cuBLAS (D3), writes `README.txt` and `config.example.json`, makes the
folder owner-only (Windows: a protected owner-only DACL; POSIX: `go-w` on the tree), and moves it to `--out`:

```
<out>/
  strata-secure-server(.exe)     the server and its page
  strata(.exe)                   the CLI (downloads child, key show/rotate)
  engine/multiengine(.exe)       the launcher
  engine/multiengine_<family>    one library per family (.dll / .so), strata_kq
  engine/cublas*, cublasLt*      bundled cuBLAS (D3; --no-cuda-runtime leaves them out)
  README.txt  config.example.json
  data/                          created at the first start (D2), never by make
```

Nothing else: no sources, tests, packs or build trees. The tokenizer packs are no longer needed for GGUF models
(section 4.2). The Qwen prepare step (Python) stays an operator tool outside the folder.

## 3. Start-up: run the binary

`strata-secure-server` with no arguments uses `<exe dir>/data` (D2) and, the first time, creates it:

- `data/` owner-only; `data/secret/` with a new key (DPAPI-wrapped on Windows, 0600 on POSIX), `data/state/` (with
  `cards/`, the cards the page and downloads write, and `calibration/`: both under `roots.state`, so a server started
  with `--config` keeps them too), `data/downloads/`, `data/models/`, `data/logs/`, `data/cache/`;
- `data/config.json`, an ordinary config file (the same schema, editable by hand: "room for a json configuration"):
  launcher mode, `listener` 127.0.0.1:8080 with `web_ui` and `monitor` on, `models.control` and `models.downloads` on,
  `downloads_root` `data/downloads`, the default models folder `data/models`, and the engine
  `<exe dir>/engine/multiengine(.exe)` with the bundled cuBLAS folder as its library folder.
- If the install folder fails the owner-only check (a copy keeps the destination's inherited permissions), the first
  start makes it owner-only (Windows: protected DACL for the user, SYSTEM and Administrators; POSIX: `go-w`), says so,
  and refuses only if that fails.

Later starts read `data/config.json`. `--config FILE` (today's path) and `--data-dir DIR` still work. The launcher
boots with no card: the Models tab then offers only "Add a model" and "Download".

**One-time login link (D1).** At every start the console prints
`Open http://127.0.0.1:<port>/#login=<code>` - a fresh 32-byte code, valid 10 minutes or until used, accepted only
from a loopback client. The page posts it to `POST /v1/strata/login-link` (no key, loopback only, constant-time
compare, one use, rate limited), receives the key once, keeps it like a typed key, and shows it once with a copy
button. The code and the key never reach a log file. `listener.login_link: false` turns the link off.

**Settings on the page.** A "Settings" sub-tab of Models shows the listener (address, port, remote access), the
default models folder and the downloads folder; a change is validated, written atomically to `data/config.json` and
takes effect at the next start (the validated config is immutable while serving; the page says so). *As of
2026-10-27 the Settings sub-tab holds only the email alerts (5a), in a build with them; the listener and folder
settings are not built yet (edit `data/config.json`).*

## 4. A folder per model, and cards that follow it

### 4.1 Cards the server writes

Cards added from the page live in `<roots.state>/cards/<name>.json` (the same card format; a download's card is
`<catalog id>.json` there; operator cards named by `models.cards` stay read-only). They load at boot beside the listed
ones whenever `models.control` or `models.downloads` is on. A card the server wrote records its `source`: `"folder"`
or `"download:<catalog id>"`. The launcher gains replace and remove for these cards (refused while a start or stop
runs; remove also while the card runs). A card is written owner-only, with decimals rendered.

**Built (2026-10-27):** `card_builder` (`card_for_gguf`, `card_with_folder`, `DeleteCodes`, `delete_gguf_model_files`),
`gguf_metadata` (a bounded header reader), `load_tokenizer_from_gguf`, `local_folders` (drives, a folder's listing,
`make_model_folder_private`, `make_private_folder`), `card_routes` (the routes below) and `ModelLauncher::replace_card`
/ `remove_card` / `card`. The page's routes, all POST, keyed, loopback clients only (also with
`models.control_remote`): `/v1/strata/folders` (`{}`: the drives; `{"path"}`: one folder), `/folders/private`,
`/folders/new`, `/cards/add`, `/cards/folder`, `/cards/delete/begin`, `/cards/delete`. New error rows:
`folder_refused` (422) and `card_refused` (422) carry the module's fixed reason as their message; `folder_not_private`
(409), `card_exists` (409), `card_read_only` (403), `card_in_use` (409), `delete_refused` (403), and
`login_link_refused` (403) for a spent or wrong sign-in link.

### 4.2 Add a model (a folder, or a GGUF in it)

"Add a model" takes a folder (an absolute local path typed or pasted, or picked from a server-side folder browser
that lists sub-folders of the user's drives over the loopback control route) and lists the GGUF files in it. For the
chosen file the server reads the GGUF header (a bounded metadata reader: no tensor data) and:

- picks the profile from `general.architecture`: `gemma4` -> gemma4, `flint` -> flint (both run as `--native`); others
  are listed as not yet addable from the page (Qwen needs its prepared pack, GLM/raon1 their packs);
- takes the served name from the file name, the sampling defaults from `general.sampling.*`, and the context bound
  from the GGUF;
- reads the tokenizer **from the GGUF itself** at every start (vocabulary, merges, token types, by the profile's
  tokenizer mode - the same loader checks as a pack), so a GGUF card needs no tokenizer pack (a card with no
  `roots.tokenizer_pack` and a `flint` or `gemma4` profile: `ComposedLaunch::tokenizer_gguf`, the model's own file held
  open beneath `roots.models`); checked token-exact on the owner's 42 GB flint GGUF against llama.cpp's ids;
- writes the card: `roots.models` = that folder, `engine.model_id` = the file's stem, the engine from the config.

**Folders as they are (owner, 2026-10-27: "an option to not change the permissions when browsing and using existing
models in the local system ... ignore that other users can access the files locally").** One owner switch, the existing
`roots.allow_untrusted_media` (until then the exFAT/FAT opt-in for the operator config's model roots), now also reaches
the page: with it on, Add a model, a card's new folder and the Danger zone open the folder the page names with
`RootPolicy::external` (local, canonical, held open, no link below it; no owner or shared-write check), a pack
strata-prepare builds beside such files is not made owner-only, and the page never asks to make a folder private. It is
set in Models > Settings > Model folders (`POST /v1/strata/folders/sharing`, for the clients the file routes serve: this
PC and, since 2026-10-28, the listed hosts and subnets, as the email and search settings), saved to the config file, and
applies at the next start. Unchanged: downloads (still into a private folder), the server's own folders, the config
file. Threat model: "Model folders as they are", and its residual.

**Make private with a model's pack (2026-10-28).** The page's "Make it private" takes a folder of model files that also
holds the folders a model's tools put beside them - `pack/` and `pack-<name>/` (Maya's setup, the server's GLM and
raon1 packs), `maya-ds4/` and `maya-ds4-<name>/`, Hugging Face's `.cache/` - each walked without following links; any
other sub-folder still refuses it.

### 4.3 Download into a chosen drive or folder

Owner, 2026-10-27: "I want to be able to select what drive or folder the images/models can be downloaded into and must
check there is enough space available before letting it download. It should be a seamless process and running it for
the first time after downloaded must run the required calibration."

- The download form gains a destination: a drive (the page lists the local drives with their free space) or a folder
  on one (default: `models.downloads_root`). The chosen folder becomes that download's **model store** (the same
  layout as `downloads_root`: `<folder>/<id>/download/<version>/`, an `index.json`, a `.staging` folder): `strata
  --config <file> --store <folder> pull ...` (the new `--store` option, models.cards configs only, the folder checked
  as the downloads root is). The folder must be owner-only; on a drive whose root others may write (a typical second
  drive), the page's "New private folder" makes one owner-only from its creation (`/folders/new`; Windows: a protected
  DACL, so others can neither rename nor delete it; POSIX: 0700, refused inside a world-writable folder without the
  sticky bit). Each download's folder is kept in `<state>/downloads/destinations.json` for a resume after a restart;
  a download moved to another folder starts over there.
- **Space first.** Before anything is fetched the server totals the model's files (the catalog's sizes, or the
  source's listing for a pasted link) and compares them with the destination volume's free space less the downloads'
  margin (2 GiB, as before); short of it, the download is refused (507) and nothing starts. A download is checked
  again before it resumes, and Maya's sizes once they are resolved.
- **One volume.** The transfer stages in the store's own `.staging` folder on the destination volume and is verified
  there, so finishing is a rename (no second copy, no space needed elsewhere).
- The card is written as in 4.2 the moment the files are verified (a Gemma 4 or flint GGUF, from the catalog or a
  paste matched to that family, when `models.default_engine` is set: its folder the installed version, its source
  `download:<id>`), and the model is marked "not calibrated": its first start runs the calibration first (5.2), then
  serves. Seamless: download, then Start.

### 4.3a Model files already on this PC (owner, 2026-10-27)

"If there are no cards built yet at start, a user can go to the download section and instead of downloading a model,
they can also point them to a folder containing the model files and it can build the card. In addition, once the model
is added to the card it disappears in the download section."

- The Download section has two ways in: download a model, or **use model files already on this PC** - a folder (4.2's
  folder browser), whose GGUF files are listed; choosing one builds its card exactly as 4.2 (no copy, no move: the card
  points at that folder). It works with no card yet (the launcher boots card-less, §3).
- A model that has a card is no longer offered in the Download section: the page hides a download whose id a card's
  `source` names (`download:<id>`), or whose model file (`"file"`: the first shard's stem) is a card's model.

### 4.4 Moving a model

A card's "Folder" field shows its model folder. Changing it (after moving the files) checks the folder holds the
card's model file, rewrites the card and replaces it in the launcher; a running model keeps running and the new
folder applies at its next start. Removing a card never deletes model files.

### 4.5 Delete a model (danger zone; owner, 2026-10-27)

"In the card section, a dangerous section option would allow the user to delete the image but will require a random
code verification, then prompt for the api key again then a final verification message that deletion is permanent and
not reversible and then delete it once verified."

Each card the page made has a collapsed **Danger zone** with "Delete this model". The three checks are the server's,
not only the page's (loopback control routes; the model must not be running; a server without an API key
(`listener.loopback_without_key`) refuses deletes):

1. `POST /v1/strata/cards/delete/begin {"name"}` returns a fresh random code (XXXX-XXXX from an unambiguous
   alphabet, valid 5 minutes, one use) that the page shows; the user types it back.
2. The page then asks for the API key again (the key in the tab is not reused).
3. A final dialog states that the deletion is permanent and cannot be undone; confirming sends
   `POST /v1/strata/cards/delete {"name", "code", "key", "permanent": true}`.

The server checks the code (spent by any try, right or wrong), the key (the server's own verifier) and the flag,
then deletes **only that model's files** - the GGUF file(s) the card names (all shards, each checked to be a regular
file before the first goes) - then the card, its remembered options and its calibration (and its "recalibrate"
switch); the folder itself is removed only when it is left empty, and nothing outside the card's model folder is
touched. For a downloaded model (source `download:<id>`) the download store then lets it go too: `strata rm
<id>:download` in that download's store (the index row and the rest of the version folder), so the Download list
offers it again; the answer's `download_removed` says whether that worked (the page warns when not). A card named by
`models.cards` (an operator file) can't be deleted from the page.

## 5. Calibrate and recalibrate

### 5.1 What it does (upstream's and Maya's method, on our engines)

A calibration is a decode-speed sweep of the engine settings that depend on the machine, for one card at its current
options, with the GPU to itself (the running model is stopped first, as upstream refuses to calibrate under a running
server). The workload is upstream's: a warm-up, then three fixed prompts rendered with the card's own template,
thinking off, greedy, 128 new tokens each; the score is the median decode tok/s. A candidate setting is kept only when
more than 3% faster than the current one, and the winner is confirmed against the defaults in interleaved rounds. A
candidate whose engine fails to start is dropped.

Settings swept per engine (each start reads its flags once, so one start per candidate, as Maya's DeepSeek path):

| profile | swept | as upstream / Maya |
|---|---|---|
| flint (ours) | `--cpu-lane` {off, on}, `--pcie-frac` {0, .25, .5, .75}, `--cpu-threads` {N, 2N/3, N/2} (N = the machine's threads) | pcie share, CPU threads |
| gemma4 (ours) | `--cpu-threads` {N, 2N/3, N/2} | CPU threads |
| qwen (upstream Strata) | `--pcie-frac` {0, .2, .35, .55, .75, .9, 1}, `--spec-min-p` {.3, .5, .7}, `--pool-workers` {2/3, 1/2, 1/4 of the engine's own: the physical cores less the host's}, then `--adapt-every 1 --adapt-swaps {80, 160} --adapt-decay .97` on 512-token answers | `tools/calibrate.py` steps 1, 2, 4, 5 |
| raon1, glm53 (Maya) | `glm.pcie_share` {0, .1, .25, .5, .75}, then `glm.cpu_lane` {3/4, 2/3, 1/2, 1/4 of the engine's own}, each step ending below 70% of its best | `calibrate_glm.py` steps 1-2 |
| deepseek, minimax (Maya) | `--cpu-threads` {3/4, 1/2 of the physical cores; 2 and 4 fewer first on a CPU without SMT}, the step ending at the first that gains nothing; a 5% keep margin | `calibrate_ds4_tp.py`, `tools/mm3/calibrate.py` |

Built (phase 4, 2026-10-27): the flint and gemma4 sweeps above (`src/calibration.cpp`, `sweep_plan` and `run_sweep`):
the baseline first, then coordinate descent over the flags in the table's order with the 3% keep margin, failed
candidates dropped, then two interleaved confirmation rounds (defaults, winner, defaults, winner); the winner keeps its
place only when it repeats. Cancel is checked before and after each trial.

Built (2026-10-28; owner: "make sure every model we have have the tuning and calibration option as well"): the qwen,
glm53, raon1, deepseek and minimax sweeps above.
- A step may set several flags at once (the expert tier's three), may measure on longer answers (the best so far is
  measured again on them first, and the confirmation uses them), and may end early (Maya's `STOP_BELOW`).
- The GLM settings are two typed rows, `glm.pcie_share` and `glm.cpu_lane`, which the launch renders as the engine's
  `STRATA_GLM_PCIE_SHARE` / `STRATA_GLM_CPU_LANE`, as Maya's calibration writes them into a config's environment.
- MiniMax also gets Maya's prefill calibration (`calibrate_mm3_prefill.py`, the engine's own `--calibrate-prefill`):
  - it runs before a MiniMax card's first start on this machine and with each Calibrate;
  - it is recorded as `<card>.prefill.json` and shown on the card;
  - one that did not finish is not retried by itself.
- Not built:
  - DeepSeek's `--tp` on several GPUs (this PC has one; the launch gives `--tp` by the GPU count);
  - the DSpark drafter's speed calibration (the owner's packs carry no drafter);
  - upstream's `--prefill-pipe` (a layer split) and its AMD stream switch;
  - Maya's GLM `READ_CHUNKS` (only when part of the model is read from the SSD);
  - the hybrid-CPU extra counts. **Not built yet:** the expert usage profile (the workload run once with
`--expert-profile-save`, used at start with `--expert-profile`, the Strata paper's VRAM cache filled from use); the
upstream and Maya sweeps.

### 5.2 The result, and when it applies

`<roots.state>/calibration/<card>.json`: the settings kept, the scores, the date, and the **key** they were measured under:
GPU names and VRAM, CPU name, RAM, the engine files' size and modification time, the model file's size and
modification time, and the card's options that change the measurement (context, chunk, expert VRAM, KV). At each start
the launcher composes the kept settings after the card's own arguments (frontend-owned values, each still checked
against the profile's allowlist; the usage-profile path is frontend-owned and lives in `<roots.state>/calibration`).

- **Never calibrated** (a new card, from a download or from "Add a model"): a flint or Gemma 4 card's first start
  calibrates first, then serves with the result (owner, 2026-10-27: "running it for the first time after downloaded
  must run the required calibration"); the page shows the calibration's progress in the card while it runs. The other
  profiles' sweeps are many starts of a large model (an hour or more), so they run when asked (Calibrate), as
  upstream's and Maya's do; MiniMax's first start runs Maya's prefill calibration (a few minutes), which Maya also runs
  unasked.
- **Hard stale** (GPU, CPU, RAM, engine or model file changed): not applied; the card shows "Calibrate again".
- **Settings changed** (an option in the key differs from the start's options): not applied; the card shows
  "Settings changed - recalibrate". The card's "Recalibrate when settings change" switch (saved with the card's
  options) runs the calibration before such a start instead of asking.

### 5.3 On the page

Each card gets "Calibrate" (or "Recalibrate"), a progress line (stage and candidate, from the job), "Cancel" (the
engine is stopped, nothing is saved), the last result (tok/s before and after, date) and the switch above. Routes:
`POST /v1/strata/models/calibrate {"name", "options"?, "then_start"?, "auto_recalibrate"?}`,
`POST .../calibrate/cancel {"name"}`, `POST .../calibrate/auto {"name", "on"}`, and the state in each card's
`calibration` member of `GET /v1/strata/models` (`state`: missing, fresh, stale or settings_changed; `auto`; the
progress while one runs; the last result or error), all on the control routes and served only while their handlers
are set. The switch is kept in the calibration file (in memory until a first result exists).

## 5a. Email alerts (optional; owner, 2026-10-27)

"Make sure the smtp is compatible with ionos smtp settings" - asked: alerts by email; "Built into the server but make
it an optional setting, alerts are optional and configurable, make it to where we can build it with or without".

- **Build option** `SECURE_FRONTEND_EMAIL_ALERTS`, **off by default** (owner: "We should be able to build the server
  with or without the email sending capability for users who opt not to have it"; then "Make the build option
  disabled by default but for ours we have it enabled"): a plain build has no mail code and no TLS library at all, its
  `alerts.*` keys are refused by name ("this build has no email alerts") and the page shows no email settings.
  `make --with email` (or `-DSECURE_FRONTEND_EMAIL_ALERTS=ON`) builds it in; our own builds do (the repository's dev
  presets set it, and our make command passes `--with email`).
- **Sending** is the server's own minimal SMTP client over TLS (Windows: SChannel through SSPI; Linux: OpenSSL), with a
  certificate check against the system's trust store and the host name; never plain text. Two modes: `ssl` (implicit TLS,
  port 465) and `starttls` (port 587, STARTTLS required before AUTH); AUTH PLAIN, else LOGIN. One short plain-text
  message per alert, sent from a background queue (a slow or failing server never delays a request), at most one per
  minute per kind, no retry storms.
- **The server's no-egress check** (`egress_server_imports_no_resolver_or_http_client`): the owner chose this over a
  curl child knowing it "brings network and TLS code into the server process". A build without email keeps the check
  as it was (no resolver, no HTTP client); a build with email may import `getaddrinfo` (the SMTP host lookup) and
  nothing else on the list. The runtime checks (no outbound socket during a server's lifecycle) are unchanged: nothing
  connects until alerts are enabled in config.
- **IONOS** (its help pages, read 2026-10-27): host `smtp.ionos.com` (or the account's regional
  host, e.g. `smtp.ionos.de`, `smtp.ionos.co.uk`), port 465 `ssl` (IONOS's primary) or 587 `starttls` (its alternative
  where 465 is blocked), user name the full mailbox address with that mailbox's password, the From address that mailbox,
  and TLS 1.2 or later (IONOS refuses older): the client never offers less than TLS 1.2. The page's preset fills these.
- **Settings** (`alerts.email.*` in config.json, the Settings sub-tab): enabled, host, port, security, user name, From,
  To (1-5 addresses), and which alerts: download finished, download failed, calibration finished or failed, the engine
  stopped unexpectedly, the server started. The password is set on the page and kept in the secret folder like the API
  key (DPAPI on Windows, 0600 on POSIX); it is never shown or logged again. "Send a test email" reports the server's
  answer (its code and text, no secret).

## 6. Security (threat model additions)

- New unauthenticated route: only `POST /v1/strata/login-link`, loopback only, single-use code, constant time,
  rate limited; it returns the key once. Everything else stays keyed; the control routes stay loopback-only unless
  `models.control_remote`.
- Paths from the page (D4): only model folders, only on control routes, each through `open_root` (local, not a
  network or redirected folder, owner-trusted unless `roots.allow_untrusted_media`), never echoed in an error. From this
  PC, and (owner, 2026-10-27, "Allow all from subnets") from a host or subnet `models.control_remote_networks` lists.
- The first start's permission fix touches only the install folder it runs from and only when the user owns it.
- Calibration composes only allowlisted flags and frontend-owned paths; it never takes an argument from the page.
- Email alerts (5a) are the server's only outbound connection, built only on request and off until configured; TLS is
  mandatory with the certificate and host name checked; the SMTP password is a secret like the API key; an alert's
  text names the event and the card, never a path, a prompt or a key.

## 7. Phases (TDD; RED recorded for each)

1. **Out folder**: `install()` rules (engine + frontend, `runtime` component), release presets, `make.py` +
   wrappers, the distribution check updated for `multiengine`, cuBLAS and the layout; checked by building into a
   folder and serving flint from it.
2. **Run the binary**: the data folder, the first-start bootstrap and permission fix, launcher with no card, the
   one-time login link (server route + page), Settings sub-tab.
3. **Cards from the page**: the GGUF header reader, the tokenizer from the GGUF, card writer (+ decimals), launcher
   replace/remove, "use files on this PC" and folder routes + page, downloads into a chosen drive or folder with the
   space check, carded models hidden from the Download list, the danger-zone delete.
4. **Calibrate**: the job runner on the supervisor, the per-profile sweeps, the result store and staleness, composing
   at start, the routes + page. *Built 2026-10-27, except the expert usage profile and the upstream/Maya sweeps;
   checked with the fake engine (`--decode-ms`, `--faster-with`), not yet on the GPU.*
5. **Email alerts** (optional build): the SMTP client over SChannel/OpenSSL, the settings and secret, the alert queue,
   the Settings sub-tab with the IONOS preset and the test email. *Built 2026-10-27; the SMTP dialogue is tested
   against a scripted transport, the SChannel path compiles and links here, the OpenSSL path is not compiled yet (no
   Linux build here); no live send to IONOS has been made.*
6. **Docs**: README (install, first start), threat model, handoff, provenance.
