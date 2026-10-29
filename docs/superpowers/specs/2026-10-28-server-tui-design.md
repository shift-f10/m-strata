# The server in a terminal: a headless build with a TUI (2026-10-28)

The owner (2026-10-28): "we should have an option to make a headless version of the frontend and instead of a window
app because it's headless, it will be a TUI with the same settings and options via the TUI. Make the TUI look
professional and make monitoring the default view in a nice graph dashboard TUI look".

## What it is

**`strata-tui`**, a terminal program beside the server: the headless twin of `strata-window`. It runs
`strata-unified-server` as its child (the window's `ServerSetup` and `ServerChild`, the same arguments passed through),
reads the key the server checks (`server_key`, held in memory only), and drives the server over loopback HTTP with that
key (`loopback_request`), the routes the page uses. The server's console lines go to the TUI's Log view and to
`<data>/logs/server.log` (the window's `RotatingLog`; the sign-in link is never logged or shown). Quitting stops the
server (an orderly stop, then a kill after the bound), as the window's Exit does.

It needs no desktop, no web view and no library beyond the C++ standard library and the operating system's console:
it runs over SSH on a Linux box as well as in Windows Terminal or a console window.

## The build

- CMake option `SECURE_FRONTEND_TUI` (off by default; the presets that turn email and search on turn it on too).
- `make --with tui` builds `strata-tui` into the out folder. A **headless** out folder is
  `--with email,search,tui` without `window`: no WebView2 or GTK is needed.
- The install scripts build **both front ends by default** (the owner, 2026-10-28: "have the installer compile both as
  an option by default so the user can pick if they want the tui or window version or specify"): `install.bat`'s
  `-With` defaults to `email,search,window,tui`. `install.sh` without `--with` builds both on a desktop, and only the
  TUI where there is no display (no `DISPLAY` or `WAYLAND_DISPLAY`, e.g. over SSH), so no GTK packages are installed
  there. Either takes `-With`/`--with` to choose, e.g. `email,search,tui`.

## The screen

A header bar (M-Strata, the server's state and address, the running model, the time), a tab row, the view, and a key
bar at the bottom with the current view's keys. Colours: 256-colour ANSI, with a plain fallback where the terminal says
it has none (`NO_COLOR`, `TERM=dumb`); box-drawing borders; the charts in Unicode braille (2x4 dots a cell), with a
block fallback.

Views (number keys 1-6, Tab / Shift-Tab, F1-F6; a view switch takes the focus off a card's options and reads the
settings again; the mouse wheel moves the selection in Models, Downloads and Settings and scrolls the Log):

1. **Monitor** (the default view), every second from `/metrics` and `/v1/strata/models`:
   - eight charts with their history, all the same width: decode tok/s (live), GPU use %, CPU %, RAM (against its
     total), GPU memory, GPU temperature, GPU power and Disk read (every disk of this PC, MB/s, GB/s past 1000 as the
     page and upstream's page say it, with its peak). Two rows of four from 120 columns, four rows of two from 80, else
     one a row. The server keeps a minute (one sample a second; RAM has no series there, so the TUI keeps its readings);
     the TUI keeps its own five minutes (300 samples), seeded from the server's minute and added to at each answer, so a
     wide terminal has a wide chart. Without a GPU, its four charts say "no GPU" instead of a scale over nothing;
   - three panels side by side, the same width and height (14 rows), from 120 columns (each a third: a 23-cell value
     column every line fits, counts in at most five cells: 1234, 12.3k, 1.2M):
     - Engine: the model; its status, said plainly when it is not loaded ("unloaded after idling", else the server's
       live state: stopped, starting, error) with none of a loaded engine's figures; its context; what it does now and
       the requests queued; the request under way (the answer's tokens of its most, the prompt's tokens and prefill
       speed, the time elapsed); its memory (experts in RAM: count and GB; experts in VRAM; the process's own); the
       requests served and the tokens out;
     - Machine: the GPU, gauges for GPU memory and RAM, the disks' read and write rates, the PCIe link, the CPU threads,
       the GPU note;
     - Conversation cache: on or off with its slots and budget, gauges for what is parked of the slots and what it holds
       of its budget, its parks, restores and evictions, its last event and how long ago, the prompt tokens it gave back
       and the requests it served. "Not used by this engine" for an engine without it, "no model running" with none
       loaded;
   - the last requests across the whole width, the newest first (time, model, prompt and reused tokens, output tokens,
     decode speed, the prefill speed over the tokens it read - those the cache gave back took no time, as the page
     counts it - and finish); on a narrow console the columns that do not fit are left out.
   A short console drops the last requests first (a 120 x 30 console shows the charts and the panels), then the panels
   (80 x 24: the charts only); below 120 columns there are no panels. A server that stopped shows none of its last
   readings as if live, the header's model neither. From an older server without these readings, each of their figures
   says "-" (never 0). The charts keep one time scale: after a long gap (polls failing for minutes) each series takes at
   most the server's minute, RAM's included.
2. **Models**: the cards (name, profile, state); the selected card's options (each in its kind: a switch, a number in
   its range, a choice) and its calibration (state, the speed before and after, the kept settings, MiniMax's prompt
   reading). Keys: Enter start (with the options shown; while a card runs it asks first, as the page does: "Restart X
   with these options?" or "Stop X and start Y?", y / n), o the options (Up/Down, Left/Right change, Esc back), s stop,
   c calibrate, x cancel the calibration, a recalibrate-when-settings-change on/off, r undo the options' changes. The
   options go with a start or a calibration as the page sends them: the kinds it knows (one it does not is shown, never
   changed or sent), each its value or else its default, the GPUs choice only once changed here. A number moves within
   the bounds it has (none: free); an "or auto" one goes below its minimum (0 without one) to auto, and back.
3. **Downloads**: the catalog and the downloads in progress (progress bars, speed); Enter or d start (or resume), p
   pause, x cancel.
4. **Settings**: every server setting the page's Settings tab has (`POST /v1/strata/config`, each with its type, its
   default and its words), edited in place and saved (`POST /v1/strata/config/settings`; applied at the server's next
   start, as on the page). Enter flips a switch or opens a field (a list by its items, "a, b"; a whole number within the
   setting's range, else the status line names the setting and its range); a change not yet saved is shown as saved
   values are, marked "* unsaved"; w saves, u undoes. The API key: hidden, shown by k; "none" for a keyless server on
   this PC, "could not be read" when the TUI could not read it; R rotates it after typing ROTATE.
5. **Log**: the server's console lines, scrollable (Up/Down, PageUp/PageDown, Home the oldest page, End the newest).
6. **About**: every reading of `/v1/strata/about` by its path (the versions, the engines, the GPUs), sizes in bytes as GB.

The status line: an action's answer (the server's own words when it refused), for ten seconds or until the view
changes; a poll's failure - the key refused (401), no answer, or a server listening on an address that is not this
PC's loopback (which the TUI, a loopback client, cannot reach) - until that poll answers again.

q (or Ctrl-C) quits after asking; the server stops with it.

## How it is built

- **The portable half** (`tui_*.cpp`, tested): a cell buffer with the drawing primitives (text, boxes, gauges, braille
  charts), the views drawn into it from the server's JSON, the key decoder (VT sequences to keys), and the diff from one
  frame to the next as VT output. A character takes the cells a terminal gives it: two for the East Asian wide ones
  and the emoji, none for a combining mark (drawn with the character before it; zero-width format characters are
  dropped), so a row stays aligned and the diff leaves no stale half. A control character from the server's answers
  (C0, DEL, C1) is drawn as a space: no text the server sends can carry a terminal sequence.
- **The terminal half** per system (thin): Windows - the console in VT mode (`ENABLE_VIRTUAL_TERMINAL_PROCESSING`,
  `ENABLE_VIRTUAL_TERMINAL_INPUT`), the alternate screen, UTF-8 output, the size from the screen buffer; Linux - termios
  raw mode and `TIOCGWINSZ`. The size is read again at every turn of the program's loop (every 50 ms), so a resize
  needs no SIGWINCH handler. A terminal that goes away (its input ends, or hangs up) makes the program quit.
- The requests run on a worker thread (a poll waits at most 5 s, a key's request 15 s; none is started once quitting
  began); the screen redraws on a change, a key or the one-second tick.
- **Every way out puts the terminal back first, then stops the server in order** (its model first): q, Ctrl-Break,
  SIGTERM, SIGHUP, SIGINT and SIGQUIT (Ctrl-C and Ctrl-\ are keys in raw mode), the terminal going away, and an
  exception (caught around the loop; the request worker is stopped and joined). After the terminal is back the TUI
  prints "strata-tui: stopping the server (its model first) ..." on the main screen, and the server's last lines still
  reach the log. The console window closing (or a logoff or shutdown) on Windows: the close handler holds the process
  until the server has stopped (Windows ends it about 5 s after the event, so the server gets 4 s); while the console
  closes, every console call of the TUI itself waits for that handler, so the Ctrl-Break comes from the TUI started
  again with no console (`strata-tui --ctrl-break <pid>`, attached to the server's console only).

## Security

The TUI is a client of the server on this PC: it has the same rights as the page opened from this PC (the loopback
rules), holds the key in memory only, and never prints or logs it but on an explicit Show. It takes no network input of
its own and listens on nothing.
