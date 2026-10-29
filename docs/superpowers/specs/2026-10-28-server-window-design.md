# The server as a window, with a Configuration tab (2026-10-28)

The owner (2026-10-28): "instead of a console application, we should make it into a window application ... minimized
into the tray that has the start and stop models menu ... show the monitor statistics ... right click it to select exit
or select a model to start and stop"; "also add a configuration tab that we can configure and manage it's settings like
api key, etc"; "we should also add the download models section in the windows app so that they don't have to open the
web ui to download or point to a model". His choices, asked: **a native shell with the page inside** (WebView2 on
Windows, WebKitGTK on Linux; closing the window keeps it in the tray), and a Configuration tab for **all of**: the API
key, the listener and LAN access, limits and timeouts, and the folders.

## Two parts

1. **A Settings tab in the page** (every way of opening the page gets it, the window included). It gathers what Models >
   Settings holds today (model folders, email alerts, web search) with four new sections. It answers loopback clients
   and the hosts `models.control_remote_networks` lists, like the settings it joins.
2. **`strata-window`**, a small native program beside the server: it runs `strata-unified-server` as its child with no
   console, shows the page in an embedded web view, and lives in the tray.

Downloads and Add a model are already in the page (Models > Download models), so the window has them with no new code.

## Part 1: the Settings tab

A fifth top-level tab, **Settings** (on a phone the five tabs share the row; checked at 320 px). Models > Settings moves
there. New sections, each saved to `data/config.json` and applied when the server restarts (as the settings today):

- **API key.** The page shows the key it signed in with (hidden until "Show", with "Copy") - it already holds it, so no
  route hands a key out. **Rotate** (`POST /v1/strata/key/rotate`, loopback only, confirmed by typing ROTATE) makes a
  new key in the token store, as `strata key rotate` does, and shows it once; the running server keeps the old key
  until it restarts. A key from `--token-env` (development) cannot be rotated here, and the page says so.
- **Listener and LAN.** `listener.host`, `listener.port`, `listener.allow_remote`, `listener.allow_wildcard_bind`,
  `listener.host_aliases`, `models.control_remote`, `models.control_remote_networks`.
- **Limits and timeouts.** `limits.engine.generation_timeout_s`, `limits.engine.ready_timeout_s`,
  `limits.chat.max_prompt_tokens`, `limits.chat.max_completion_tokens`, `pull.connections`.
- **Folders.** `models.downloads_root`, `models.default_root` (with the folder picker the page already has), and the
  model folders switch that is already there.

Routes (`ConfigRoutes`, the `SearchRoutes`/`AlertRoutes` pattern): `POST /v1/strata/config` (the file's values and
whether they can be written), `POST /v1/strata/config/settings` (the new values: each key checked by the typed config's
own rules - the whole file is parsed and validated before it is written, so a value the server would refuse at start is
refused here, by name), `POST /v1/strata/key/rotate`. Nothing about a key is ever logged.

## Part 2: `strata-window`

- **Start.** It starts `strata-unified-server` beside it with the same arguments it was given (so `--data-dir`, `--port`
  pass through), with no console window, its output written to `data/logs/server.log` (bounded, rotated). It reads the
  server's "Sign in: open http://127.0.0.1:PORT/#login=..." line and opens the page through it (the one-time link,
  this PC only), on the Monitor tab: the page takes `#login=<code>&view=monitor` (or `#view=monitor`); a restart's link
  keeps the view the page shows.
- **As built (2026-10-28).**
  - Every start of the server gets the environment the window started with: reading a `--token-env` key scrubs the
    variable from the reader, so a restart would otherwise lose it.
  - A restarted server's link reaches the page as a fragment change, so the page now claims a link on `hashchange`
    as well as at load. It shows the key once only when the key is new to the tab.
  - On Linux the tray menu is refreshed every 3 s (AppIndicator exports a fixed menu), and a middle click opens the
    window.
- **Window.** The page in WebView2 (Windows) / WebKitGTK (Linux). Navigation stays on the server's own origin; a link
  elsewhere opens in the default browser. The web view's data lives in `data/state/window` (owner-only). Closing the
  window hides it to the tray.
- **Tray.** A tray icon; left click opens the window; right click shows: Open M-Strata, Server log, one entry per model
  card (Start, or Stop for the running one), Restart the server, Exit. The model entries use the server's own routes with
  the API key read from the data folder's token store, as `strata key show` reads it (never logged, never shown).
- **Log (2026-10-29; the owner: "in the window app version we need to add a log capability, the console have it but
  this one do not so we can troubleshoot").** Server log (the tray, Ctrl+Shift+L, and on Windows the title bar's menu)
  opens a window with the server's console lines, newest last. The sign-in link is never among them. It also shows the
  window's own notes when the server starts, restarts or ends. It keeps 2000 lines or 1 MiB in memory (`LogLines`) and
  follows new lines unless scrolled up. Copy all, Save as... and Open the logs folder are its buttons. Every line is also
  in `data/logs/server.log`.
- **Exit.** Stops the server (its Ctrl-Break / SIGTERM path: the running model stops cleanly), then quits. Restart does
  the same and starts it again (the way settings apply).
- **Console mode stays.** `strata-unified-server` is unchanged: development, tests and headless Linux run it as before.

### Build

- Windows: WebView2 through Microsoft's WebView2 SDK (its headers and static loader; BSD-style license), kept in
  `.toolchains/webview2`; the runtime is part of Windows 10/11 (Evergreen). Linux: GTK 3, WebKitGTK 4.1 and
  libayatana-appindicator3 when pkg-config finds them.
- `make.py --with window` builds it (off by default, like email and search); the out folder then holds
  `strata-window(.exe)`, and README.txt says to start that one.

## Security

The window is a local program of the same user as the server: it reads the key the way `strata key show` does, holds it
in memory only, and passes it only to the server on 127.0.0.1. The web view loads only the server's page (other origins
are refused or handed to the default browser), runs no other content, and keeps its profile private. The new routes
answer the clients the existing settings answer; key rotation answers loopback clients only.
