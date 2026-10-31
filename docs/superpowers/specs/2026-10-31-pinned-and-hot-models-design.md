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

## 3. Hot: always ready

- **Warmed at its start:** once a hot card's engine is ready, the server runs one short generation through it (one token,
  thinking off where the family takes it; never a conversation, never logged as a request), so the first answer does not
  pay for cold pages and first-use setup.
- **Kept warm while idle:** while it runs and no request has reached it for ten minutes, the server warms it again, so the
  OS keeps its pages. A warm-up never waits for or delays a request: it runs only when the engine is idle, and a request
  arriving first takes the engine.
- What this does not do: lock an engine's memory from outside its process (no OS lets the server do that portably); the
  experts' RAM is already locked by the engines that pin it.

## 4. Hot: started again

- **An engine that dies** is restarted by its supervisor, as now. When its circuit opens, a hot card is started again
  by the server after a cool-down (one minute, doubling to fifteen), each attempt raising the engine-stopped alert, until
  it runs or the owner stops it or starts another card.
- **A server that restarts** starts the pinned card. With no pin, the hot card that was running when the server stopped
  starts again: the server keeps its name in `<roots.state>/hot.json` (owner-only) while it runs, and forgets it when the
  owner stops it or starts another card.

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

## Proof

Each part: RED then GREEN in the launcher's and routes' tests over the fake engine (a pin written and listed and started
at the next boot; a hot card's start with idle unload 0; a warm-up seen by the fake engine after READY and after an idle
spell, never during a request; a hot card restarted after its circuit opens, and after a server restart), the page in the
built-in browser against a scratch install, the TUI in a hidden console; Windows and Linux.
