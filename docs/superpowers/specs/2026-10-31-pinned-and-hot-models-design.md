# Pinned and hot models (owner, 2026-10-31)

The owner: "one feature we need to have is to pin a model to start at startup in the settings and an option to make
loaded models hot so they do not unload and it is always ready. This is a prime feature we must have on both main and
branch." His answers to the two questions it raised:

- **What hot protects against:** idle unload, the model's memory being paged out, and crashes and restarts (not a second
  model kept loaded while another card runs: one card runs at a time, as now).
- **Where pinning lives:** a Start at startup choice in Settings and a pin on each card, in the page's Models tab and the
  TUI; one card pinned at a time.

Every part lands on `plugins` first and is cherry-picked onto `main` (both branches, the owner's words), each its own
commit, TDD, Windows and Linux.

## Today

- `models.start` (config row, launcher mode): the card started at boot, after the listener binds; unset, the server boots
  with no engine. Nothing on the page or in the TUI sets it.
- `engine.idle_unload_s` (main config row, 0 = never, the default; `glm.idle_unload_s` wins for the GLM engine's cards):
  the supervisor stops an engine idle that long, and the next request loads it again ("served from cold"). Nothing on the
  page sets it.
- A crashed engine is restarted by its supervisor with backoff; repeated failures open its circuit, and it stays down
  until the server restarts.
- The CUDA engines page-lock the experts' RAM themselves (upstream's `cudaHostRegister` arena), so that tier is never paged
  out; the rest of an engine's memory is the OS's to trim while it idles.

## 1. Pin a card to start at startup

- **The pin is `models.start`.** One card at most; the config file holds it, so it survives restarts and reinstalls of
  the program files.
- **`POST /v1/strata/models/pin`** `{"name": "<card>"}` or `{"name": null}` (model control's clients and key, as
  start and stop): writes `models.start` in the config file (the config writer, owner-only, as Settings saves), or
  removes it. Refused: a card the server does not hold, a server started without a config file it may write. It applies
  at the next start; nothing restarts.
- **The models list** says which card is pinned (`"pinned": true`), from the pin the server holds now (the route updates
  it), not the one it booted with.
- **Removing a pinned card unpins it**, so the next start does not name a missing card.
- **Settings:** a Start at startup choice (None, or each card by name), saved through the same route - so the two places
  never disagree, and it applies at once like the card's pin (not a managed row saved for the next start).
- **The Models tab:** each card has a pin; pinning one unpins the one before. **The TUI:** the Models view's `p` pins or
  unpins the card under the cursor, and the list marks the pinned one.

**As built (2026-10-31, part 1).** `POST /v1/strata/models/pin` (`kModelControlRoutes`, `RouteId::strata_pin`,
`ModelControl::pin`: served with model control, its clients and key, a small JSON body). `ModelLauncher::pin(name)` checks
the card (404 `model_not_found`), keeps the pin first (`LauncherSettings::save_pin`: the server's `update_config_file` of
the config it started with, `models.start` set or taken out; none, 422 `settings_refused` saying why), then holds it
(`settings.pinned`, the boot's `models.start` at first) - one pin at a time (`pin_mutex_`). The list gives each card
`"pinned"`, its last field. `forget_card` (every removal) unpins a removed pinned card and takes it out of the file. The
page: a card's Start at startup box (Pin / Unpin), the list's "starts with the server", Settings' Start at startup card
(drawn from the models list, saved through the route, hidden without model control). The TUI: `p` (its name, or null
when it is the pinned one), `*` in the list, a Startup row. A pinned card starts at boot as `models.start` always did.

## 2. Hot: kept loaded

- **Every card offers Keep loaded (`hot`)**, a server option like the answer's longest time: off by default; the card's
  file may list it on. Its value is the card's start's, remembered as every option is.
- **A hot card never idle-unloads**: its start writes `engine.idle_unload_s` 0 (and `glm.idle_unload_s` 0) whatever the
  main config says.
- **Settings:** Unload an idle model after (seconds, 0: never) - `engine.idle_unload_s`, a managed row - for the cards
  that are not hot.

**As built (2026-10-31, part 2).** Building it showed what idle unload is with model cards: the server refuses
`engine.idle_unload_s` and `engine.lazy_load` there ("the launcher starts and stops each engine"), so no card's engine
idle-unloads but the GLM engine's (GLM-5.3, raon1), through the main config's own `glm.idle_unload_s` - a main row, refused
on any other family. So:
- **Keep loaded (`hot`)** is in every family's option table after the longest answer, and a plugin family's server options
  (a manifest may not declare it); the launcher offers it off on every card (`offer_hot`). On, a GLM-engine card's start
  writes `glm.idle_unload_s` 0 (`write_option`); any other card has no idle unload to turn off, and the switch is the one
  parts 3 and 4 read. It never writes `engine.idle_unload_s` (that would refuse the start) or an engine flag.
- **No Settings row:** an idle-unload row could not apply with cards (the server refuses it), so Settings shows none.

## 3. Hot: always ready

- **Warmed at its start:** once a hot card's engine is ready, the server runs one short generation through it (one token,
  thinking off where the family takes it; never a conversation, never logged as a request), so the first answer does not
  pay for cold pages and first-use setup.
- **Kept warm while idle:** while it runs and no request has reached it for ten minutes, the server warms it again, so the
  OS keeps its pages. A warm-up never waits for or delays a request: it runs only when the engine is idle, and a request
  arriving first takes the engine.
- What this does not do: lock an engine's memory from outside its process (no OS lets the server do that portably); the
  experts' RAM is already locked by the engines that pin it.

**As built (2026-10-31, part 3).** A start whose values hold `hot` true marks the running card kept loaded and warms it as
soon as it runs (`ModelLauncher::warm`: the calibration's own request body - one token, `reasoning_effort` none where the
family takes it - through the model's chat route, on the launcher's worker). The worker's idle loop (every 5 s, or the
period when shorter) warms it again when it is ready and neither the last request (`EngineSupervisor::last_activity()`,
kept whatever the idle unload) nor the last warm-up is `warm_every` old (`LauncherSettings::warm_every`, 10 minutes). The
warm-up is one more request in the engine's one queue: a request that comes meanwhile waits for its one token at most.

## 4. Hot: started again

- **An engine that dies** is restarted by its supervisor, as now. When its circuit opens, a hot card is started again
  by the server after a cool-down (one minute, doubling to fifteen), each attempt raising the engine-stopped alert, until
  it runs or the owner stops it or starts another card.
- **A server that restarts** starts the pinned card. With no pin, the hot card that was running when the server stopped
  starts again: the server keeps its name in `<roots.state>/hot.json` (owner-only) while it runs, and forgets it when the
  owner stops it or starts another card.

**As built (2026-10-31, part 4).** The launcher holds the card kept loaded the owner last started (`kept_loaded_`): a start
whose values hold `hot` true keeps it, any other start forgets it, the owner's stop and the card's removal forget it, the
server's own stop keeps it; each change is written to `LauncherSettings::hot_file` (`<roots.state>/hot.json`,
`{"card": "<name>"}`, owner-only), which the next launcher reads back when it holds that card. The worker's idle loop sees
it down - not running, its start failed, or its engine's circuit open - and starts it again (`{"hot": true}` over its
remembered options, never calibrating) once `restart_after` has passed, each wait twice the last up to `restart_most` (1
and 15 minutes); a run starts the waits over. The engine's stop still raises its alert once. At the server's start, a pin
wins; with none, `start_kept_loaded()` starts the card kept loaded.

## 5. Linux: the server starts at boot (the owner, 2026-10-31)

The owner: "one additional settings for the linux, have a systemd option so we can have it autostart on boot, to which
may require sudo"; asked how, his answer: both ways, chosen at install.

- **Today:** `strata service enable` writes the hardened systemd *user* unit (NoNewPrivileges, ProtectSystem=strict,
  ProtectHome=read-only, the roots as ReadWritePaths, Restart=on-failure) and enables it: it starts when the owner logs in,
  not at boot. It never elevates.
- **As a user service at boot:** that unit, and `sudo loginctl enable-linger <owner>`, so the owner's systemd manager - and
  the unit - start at boot without a login. The only root change is the linger flag.
- **As a system service:** the same hardened unit with `User=<owner>`, installed as `/etc/systemd/system/m-strata.service`
  (root-owned, 0644) and enabled with sudo; it starts at boot, as the owner.
- **The installer asks** (install.sh, Linux): start at boot - as a user service, as a system service, or not (Enter: not;
  `--boot user|system|no` on the command line, `--yes` takes no). The steps that need root run through sudo there, in the
  owner's terminal; the server and the page never run sudo or systemctl.
- **`strata service`** gains `enable --system` (prints the system unit and the sudo commands that install it; the verb itself
  still never elevates) and `status` reports all three: the user unit, linger, the system unit.
- **Settings** (Linux): a Start at boot section shows which is on - read from the unit files and the linger flag - and the
  commands that turn it on or off; the server only reads them.
- A pinned card (part 1) is what such a boot then starts.
- **Found before building it (2026-10-31):** the Linux install runs `strata-unified-server` on its data folder
  (`<install>/data`: state, logs, cache, secret, downloads, models), while SP7's unit runs `strata --config ... serve` with
  only roots.state/logs/cache writable - under `ProtectHome=read-only` a download or a new model folder could not be
  written. And the first start prints its one-time login link on stdout, which under systemd is the journal - a log, where
  that link must never go. So the boot unit is the install's own: `ExecStart=<install>/strata-unified-server --data-dir
  <data>`, the whole data folder writable (the rest of home read-only, as before: a model folder elsewhere is read, never
  changed, so "make it private" there is done before), and the unit starts the server with `--no-login-link`, which
  prints none (sign in with the API key, `strata key show`, or through an app) - the server change this part adds. (A
  check for "stdout is not a terminal" was the first idea; strata-window reads its link from the server's piped output,
  so the flag is explicit instead.)

**As built (2026-10-31, part 5: the Linux installer).** `install.sh --boot user|system|no`, asked where systemd runs
(`/run/systemd/system`; Enter, no terminal and `--yes` take no) and refused elsewhere, with an `--out` folder holding a
space, or for another word. The plan names what it installs and prints the unit (`boot_unit`): `ExecStart=<out>/strata-
unified-server --data-dir <out>/data --no-login-link`, SP7's hardening (Restart=on-failure, RestartSec=5,
NoNewPrivileges, ProtectSystem=strict, ProtectHome=read-only, PrivateTmp, RestrictAddressFamilies AF_UNIX AF_INET
AF_INET6) with `ReadWritePaths=<out>/data`; the system unit adds `User=`/`Group=` (the owner) and network-online, and is
wanted by multi-user.target (the user unit by default.target). After the build (step 8) it makes `<out>/data` owner-only
(0700, so systemd can make it writable before the first start fills it), then either writes
`~/.config/systemd/user/m-strata.service` (0644), `systemctl --user daemon-reload` and `enable`, and `sudo loginctl
enable-linger <owner>`; or `sudo install -m 0644 -o root -g root` it as `/etc/systemd/system/m-strata.service`, `sudo
systemctl daemon-reload` and `enable`. It enables and does not start (a server already running from a shell would make a
started one retry its port); Done says how to start it now, read its journal, and stop it starting. The `strata service`
verb is unchanged (its SP7 unit runs `strata --config ... serve`).

## 6. One server, the apps manage it; Windows start too (the owner, 2026-10-31)

The owner: "we need the strata-unified-server binary as the main binary running the services, the window and tui should
be the ones we need to manage it so that we can launch it and either use the tui or window app and would still work
without erroring that the server is running already. This then would work seamlessly with the linux boot start and even
have a windows start option." His answers: Windows start both ways, chosen at install; an app that attached to a running
server keeps it running when it quits.

- **`strata-unified-server` runs the service**, whoever starts it: a boot service (part 5, and the Windows start below), a
  shell, or an app. strata-window and strata-tui manage it; neither is needed for it to run.
- **An app attaches to the server already running for its data folder** instead of failing on the port: before it starts
  one, it reads the data folder's config (the listener's host and port) and asks that address's `/health`; a server that
  answers with the data folder's own key (read from its secret folder, as the app reads it today) is used as it is - the
  page, the tray, the TUI's views - and the app says it attached. Only when none answers does it start the server itself,
  as today. A port held by something that is not this data folder's server is refused by name, as today.
- **Quitting**: an app that attached leaves the server running (it is not the app's); an app that started the server asks,
  as today, whether to stop it; a server a boot service runs is never stopped by an app's quit (the service would only
  start it again).
- **Windows start, chosen at install** (install.ps1, `-Boot logon|service|no`; `--yes` takes no):
  - **at log-on**: the Task Scheduler task `strata service enable` already makes (LeastPrivilege, the current user), its
    action the install's `strata-unified-server --data-dir <data>`;
  - **at boot**: a Windows service that runs `strata-unified-server --data-dir <data>` as the owner's account, created
    once at install with the owner's administrator approval (the installer's elevation; the server and the apps never
    elevate), set to start automatically and to restart on failure.
- **Settings** (Windows too): Start at boot shows which is on - the task, the service, or neither - and the commands that
  change it; the server only reads.
- **The login link**: a service's start (`--no-login-link`) prints no one-time login link (part 5), on Windows as on
  Linux; the apps sign in with the key from the data folder.

**Found before building the Windows start (2026-10-31; the owner: "Yes, use the task").** A Windows service logged on as an
account runs with that account's full token - no UAC split for a service logon - so on an administrator account it is
elevated, and the server refuses an elevated token (G4, privilege_check.cpp): "a Windows service as the owner" would not
start on an admin account. A Task Scheduler task with a start-up trigger, run whether or not the owner is logged on,
with the least privileges (RunLevel Limited: the filtered, non-elevated token) and the owner's password kept by Task
Scheduler (typed into Windows' own prompt at install), starts at boot as the owner and keeps the key file readable
(DPAPI needs the password; an S4U task, which stores none, could not read it). The owner chose that task in place of
the service. And the log-on task cannot simply run the server: started with the interactive token, a console program
opens a console window at every log-on - so it opens strata-window in the tray instead, which uses the server already
running or starts its own.

**As built (2026-10-31, part 6: the Windows start and Settings).**
- **install.ps1 -Boot logon|startup|no**, asked (Enter, piped answers' end and -Yes: no); refused for another word, and
  `logon` without the window. The plan prints the task (`Get-BootTask`); both have RunLevel LeastPrivilege, IgnoreNew, no
  time limit, run on batteries, Priority 6, the install folder as the working folder, the program path quoted. `logon`:
  a LogonTrigger for this user, the InteractiveToken, `strata-window.exe --tray`. `startup`: a BootTrigger, LogonType
  Password, `strata-unified-server.exe --data-dir "<out>\data" --no-login-link`, restarted after a failure (1 minute, 3
  times). Step 8, after the build, writes it as UTF-16 to `<out>\boot-task.xml` (kept: the record of what was made) and
  makes it: `logon` by schtasks as this user; `startup` by schtasks run with Windows' approval prompt (Start-Process -Verb
  RunAs), with `/RU <this user> /RP *` - schtasks asks for the password itself. A task not made is said, with the
  schtasks command that makes it by hand.
- **strata-window --tray** (window_shell `window_start`: only as the first argument; the rest go to the server): it
  opens hidden, in the tray (Win32 SW_HIDE; GTK shows the stack, not the window); the tray's Open shows it.
- **Settings' Start at boot** (boot_state.hpp, read only): Linux - the user unit and its default.target.wants link,
  `/var/lib/systemd/linger/<user>`, the system unit and its multi-user.target.wants link (starts: system, user, login,
  none - and installed but not enabled); Windows - `schtasks /Query /TN M-Strata /XML` through service_ops' run_argv
  (System32 only), a BootTrigger is startup, a LogonTrigger logon, else none. One sentence (`says`) and the command that
  changes it (`change`); POST /v1/strata/config answers it as `boot`, the page's Start at boot card and the TUI's Boot row
  show it. `strata service` is unchanged.

**As built (2026-10-31, part 6: the apps attach).**
- **The server**: `--no-login-link` sets `listener.login_link` false for that start. A keyed server with the page always
  holds its login links and serves the claim; `listener.login_link` only prints the start's link. `POST
  /v1/strata/login-link/new` (`kNewLoginLinkPath`, `RouteId::strata_login_link_new`, `ApiRouterConfig::new_login_link`):
  keyed, POST, a small JSON body, loopback only; it answers `{"code": ...}` (`no-store`), a fresh one-time code that
  replaces the one before it.
- **The shared check** (window_shell): `server_address(setup)` - the config's `listener.host`/`listener.port` (a wildcard
  bind as 127.0.0.1 or ::1) and the key (`server_key` is now its key half); `running_server(address, timeout)` - GET
  /health with the key: 200 or 503 with the health body (or the fail-closed 503) is `ours`; any other answer, or a port
  that holds the connection and never answers, is `other`; a refused connection is `none`; `fresh_sign_in(address)` - the
  new route's code as the link the server would print (`sign_in_url`'s rule), none for a keyless server.
- **strata-tui**: looks before the Debugging gate; attached, it skips the gate and the child, takes the address and key,
  says so in its Log, asks "Quit? The server keeps running." (`App::server_attached`) and prints that on the way out.
- **strata-window** (Windows and GTK): looks before the Debugging gate; attached, `attach_server()` opens the page with
  `fresh_sign_in`'s link (on a thread; none: the page's own sign-in), the tray lists the models from it, offers no
  Restart and **Exit (the server keeps running)**, which quits without a stop (a signal too, on Linux).
- A `--token-env` key is read once (the read scrubs the variable): the start's listening line falls back to the key the
  look read.

## Proof

Each part: RED then GREEN in the launcher's and routes' tests over the fake engine (a pin written and listed and started
at the next boot; a hot card's start with idle unload 0; a warm-up seen by the fake engine after READY and after an idle
spell, never during a request; a hot card restarted after its circuit opens, and after a server restart), the page in the
built-in browser against a scratch install, the TUI in a hidden console; Windows and Linux.
