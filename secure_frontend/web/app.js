// Strata's built-in chat page (listener.web_ui), modelled on upstream Strata's serve/web UI (pin
// v0.2.2, MIT License, Copyright (c) 2026 Niko1221 and the Strata contributors; see
// LICENSE.strata-ui.txt) and written for this server's routes: /health, /v1/models,
// /v1/chat/completions, (listener.monitor) /metrics, (models.control) /v1/strata/models with
// its start and stop routes and (models.downloads) /v1/strata/downloads with its start, cancel
// and link routes, nothing else.
//
// The API key is the login: kept in sessionStorage (this tab only) and sent only as
// "Authorization: Bearer" to this origin, with cookies off. On a keyless loopback server
// (listener.loopback_without_key) the model list answers without a key and the login is skipped.
// Model output is never parsed as HTML: the Markdown renderer below builds DOM nodes and sets
// text with textContent.
(function () {
  "use strict";

  var SLOT_KEY = "strata.apiKey";
  var SLOT_CHAT = "strata.chat";
  var SLOT_SETTINGS = "strata.settings";
  // Upstream's four thinking levels (session log step 16): none closes the think block; low,
  // medium and high (the template's default) write its effort sentence.
  var DEFAULTS = { thinking: "high", temperature: 1.0, top_p: 0.95, max: "", seed: "", show: true, tuned: false, search: true };
  var LEVELS = { none: "answers right away", low: "short", medium: "medium", high: "thorough (default)", auto: "the model decides" };
  var SPRITE = "/ui/sprite.svg#";
  var DEG = String.fromCharCode(176);
  var DOT = " " + String.fromCharCode(183) + " ";

  var state = {
    key: null, keyless: false, models: [], model: "", history: [], attachments: [],
    controller: null, settings: null, health: null, pollTimer: null
  };

  function $(id) { return document.getElementById(id); }

  // ---------------------------------------------------------------- storage (this tab only)
  function load(slot, fallback) {
    try {
      var text = window.sessionStorage.getItem(slot);
      return text === null ? fallback : JSON.parse(text);
    } catch (e) { return fallback; }
  }
  function save(slot, value) {
    try {
      if (value === null) { window.sessionStorage.removeItem(slot); } else { window.sessionStorage.setItem(slot, JSON.stringify(value)); }
    } catch (e) { /* storage blocked: kept in memory for this page only */ }
  }

  // ---------------------------------------------------------------- small DOM helpers
  function el(tag, className, text) {
    var node = document.createElement(tag);
    if (className) { node.className = className; }
    if (text !== undefined && text !== null) { node.textContent = text; }
    return node;
  }
  function icon(name, small) {
    var svg = document.createElementNS("http://www.w3.org/2000/svg", "svg");
    svg.setAttribute("class", small ? "st-icon st-icon--sm" : "st-icon");
    var use = document.createElementNS("http://www.w3.org/2000/svg", "use");
    use.setAttribute("href", SPRITE + name);
    svg.appendChild(use);
    return svg;
  }
  function iconButton(name, label, onClick) {
    var button = el("button", "st-btn st-btn--icon");
    button.type = "button";
    button.title = label;
    button.setAttribute("aria-label", label);
    button.appendChild(icon(name));
    button.addEventListener("click", onClick);
    return button;
  }
  function clear(node) { while (node.firstChild) { node.removeChild(node.firstChild); } }

  function toast(kind, title, text) {
    var box = el("div", "st-toast st-toast--" + kind);
    box.appendChild(icon(kind === "error" ? "i-error" : kind === "success" ? "i-check" : "i-info"));
    var body = el("div");
    body.appendChild(el("div", "st-toast__title", title));
    if (text) { body.appendChild(el("div", null, text)); }
    box.appendChild(body);
    $("toasts").appendChild(box);
    window.setTimeout(function () { if (box.parentNode) { box.parentNode.removeChild(box); } }, 4000);
  }

  // Clipboard: the async API needs a secure context, which a page served over plain HTTP on the
  // LAN is not; the fallback copies from a hidden text area.
  function copyText(text) {
    if (navigator.clipboard && window.isSecureContext) {
      navigator.clipboard.writeText(text).then(function () { toast("success", "Copied"); },
        function () { toast("error", "Could not copy"); });
      return;
    }
    var area = el("textarea");
    area.value = text;
    area.setAttribute("readonly", "");
    area.className = "copy-buffer";
    document.body.appendChild(area);
    area.select();
    var ok = false;
    try { ok = document.execCommand("copy"); } catch (e) { ok = false; }
    document.body.removeChild(area);
    toast(ok ? "success" : "error", ok ? "Copied" : "Could not copy");
  }

  // ---------------------------------------------------------------- API
  function api(path, options, withoutKey) {
    var init = options || {};
    var headers = init.headers || {};
    if (state.key && !withoutKey) { headers.Authorization = "Bearer " + state.key; }
    init.headers = headers;
    init.cache = "no-store";
    init.credentials = "omit";
    init.referrerPolicy = "no-referrer";
    return window.fetch(path, init);
  }
  function errorText(status, body) {
    try {
      var parsed = JSON.parse(body);
      // A refused link also carries the server's fixed reason (which Hugging Face or .gguf rule).
      if (parsed && parsed.error && parsed.error.message) {
        return parsed.error.message + (parsed.reason ? ": " + parsed.reason : "") + " (" + (parsed.error.code || status) + ")";
      }
    } catch (e) { /* not JSON */ }
    return "HTTP " + status;
  }
  // The error body's code ("folder_not_private", ...), or "".
  function errorCode(body) {
    try {
      var parsed = JSON.parse(body);
      return parsed && parsed.error && typeof parsed.error.code === "string" ? parsed.error.code : "";
    } catch (e) { return ""; }
  }
  // The model list with the current credentials: the list, or null on 401.
  async function listModels() {
    var response = await api("/v1/models");
    var body = await response.text();
    if (response.status === 401) { return null; }
    if (!response.ok) { throw new Error(errorText(response.status, body)); }
    var parsed = JSON.parse(body);
    return parsed && Array.isArray(parsed.data) ? parsed.data.map(function (m) { return m.id; }) : [];
  }

  // ---------------------------------------------------------------- views
  // The Models tab has two sub-tabs (owner: "tab out the pages ... so it's not too busy"): the
  // models you can start, and the models you can download. Default: start.
  var modelsSubtab = "start";
  function showModelsSubtab(name) {
    modelsSubtab = name;
    $("models-pane-start").hidden = name !== "start";
    $("models-pane-download").hidden = name !== "download";
    $("models-subtab-start").setAttribute("aria-selected", name === "start" ? "true" : "false");
    $("models-subtab-download").setAttribute("aria-selected", name === "download" ? "true" : "false");
  }

  function showView(name) {
    state.view = name;
    ["login", "chat", "models", "monitor", "settings", "about"].forEach(function (view) { $("view-" + view).hidden = view !== name; });
    var signedIn = name !== "login";
    $("tabs").hidden = !signedIn;
    $("new-btn").hidden = name !== "chat";
    $("header-model").hidden = !signedIn || !launcher.enabled;
    ["chat", "models", "monitor", "settings", "about"].forEach(function (tab) { $("tab-btn-" + tab).setAttribute("aria-selected", tab === name ? "true" : "false"); });
    if (name === "monitor") { startMonitor(); } else { stopMonitor(); }
    if (name === "models") { renderModels(false); refreshLauncher(); refreshDownloads(); showModelsSubtab(modelsSubtab); }
    if (name === "settings") { refreshFolders(); refreshAlerts(); refreshSearch(); refreshServerConfig(); renderKey(); }
    if (name === "about") { renderAbout(); }
    if (name === "chat") { $("input").focus(); }
  }
  function showLogin(message) {
    stopPolling();
    stopMonitor();
    stopLauncherPoll();
    stopDownloadsPoll();
    setPill("idle", "Signed out");
    $("login-error").textContent = message || "";
    $("login-error").hidden = !message;
    $("key").value = "";
    keyShown = false;  // a new key shown once is not left on the page for the next sign-in
    $("key-new").hidden = true;
    $("key-new-value").textContent = "";
    showView("login");
    $("key").focus();
  }
  function enterApp(models, view) {
    state.models = models;
    state.model = models.length ? models[0] : "";
    $("sign-out-row").hidden = state.keyless;
    renderChat();
    showView(view || "chat");
    startPolling();
    refreshLauncher();  // a server with model control off answers 404 and the page stays as it was
    refreshDownloads();  // likewise with downloads off; a transfer already running is followed
    refreshFolders(); refreshAlerts(); refreshSearch(); refreshServerConfig();  // the Settings tab shows with any of them
    loadHostGpus();      // the cards' GPUs choice lists this PC's GPUs
  }

  async function signIn(key, view) {
    state.key = key;
    $("sign-in").disabled = true;
    var models;
    try { models = await listModels(); } catch (e) { $("sign-in").disabled = false; showLogin("Cannot reach the server: " + e.message); return; }
    $("sign-in").disabled = false;
    if (models === null) { state.key = null; save(SLOT_KEY, null); showLogin("That API key was not accepted."); return; }
    save(SLOT_KEY, key);
    state.keyless = false;
    enterApp(models, view);
  }
  // GUI-first start-up: the server prints a one-time login link (<this page>/#login=<code>) at every start; the code
  // is exchanged once, from this PC, for the API key (POST /v1/strata/login-link, no key needed). null: refused.
  async function claimLoginLink(code) {
    try {
      // through the one API helper, never with a key (the route takes none; after a rotation the old one is dead)
      var response = await api("/v1/strata/login-link", {
        method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ code: code })
      }, true);
      if (!response.ok) { return null; }
      var body = await response.json();
      return body && typeof body.key === "string" && body.key ? body.key : null;
    } catch (e) {
      return null;
    }
  }
  // The key, shown once (the page keeps it only in this tab).
  function showKeyOnce(key) {
    var dialog = $("key-once");
    $("key-once-value").textContent = key;
    $("key-once-copy").onclick = function () {
      if (navigator.clipboard) { navigator.clipboard.writeText(key).then(function () { $("key-once-copy").textContent = "Copied"; }); }
    };
    $("key-once-close").onclick = function () { $("key-once-value").textContent = ""; dialog.close(); };
    if (typeof dialog.showModal === "function") { dialog.showModal(); }
  }
  function signOut(message) {
    if (state.controller) { state.controller.abort(); }
    state.key = null;
    save(SLOT_KEY, null);
    newChat();
    showLogin(message || "");
  }

  // ---------------------------------------------------------------- status pill (/health)
  function setPill(stateName, text) {
    $("pill").setAttribute("data-state", stateName);
    $("pill-text").textContent = text;
  }
  async function pollHealth() {
    if (state.controller) { return; }  // the chat itself drives the pill while it runs
    try {
      var response = await api("/health");
      var parsed = JSON.parse(await response.text());
      state.health = parsed;
      var engine = parsed && parsed.engine;
      // The engine changed under the page (a start or stop from elsewhere): the Models tab and
      // the chat's model follow it. The 2 s poll covers a start this page made.
      if (launcher.enabled && !launcher.timer && engine !== launcher.engine) { refreshLauncher(); }
      launcher.engine = engine;
      if (response.status === 401) { setPill("idle", "Key needed"); }
      else if (engine === "ready") { setPill("idle", "Ready"); }
      else if (engine === "generating") { setPill("generating", "Busy"); }
      else if (engine === "starting") { setPill("queued", "Starting"); }
      else if (engine === "none") { setPill("idle", "No model"); }  // launcher mode, nothing running
      else { setPill("queued", engine === "failed" ? "Engine error" : "Unavailable"); }
    } catch (e) { setPill("idle", "Offline"); }
  }
  function startPolling() { stopPolling(); pollHealth(); state.pollTimer = window.setInterval(pollHealth, 5000); }
  function stopPolling() { if (state.pollTimer) { window.clearInterval(state.pollTimer); state.pollTimer = null; } }

  // ---------------------------------------------------------------- Markdown (DOM only)
  function inline(parent, text) {
    // `code`, **bold**, *italic*/_italic_, [label](http(s)://...) - everything else is text.
    var pattern = /(`[^`\n]+`)|(\*\*[^*\n]+\*\*|__[^_\n]+__)|(\*[^*\n]+\*|_[^_\n]+_)|(\[[^\]\n]+\]\(https?:\/\/[^)\s]+\))/g;
    var last = 0;
    var match;
    while ((match = pattern.exec(text)) !== null) {
      if (match.index > last) { parent.appendChild(document.createTextNode(text.slice(last, match.index))); }
      var token = match[0];
      if (match[1]) { parent.appendChild(el("code", "inline", token.slice(1, -1))); }
      else if (match[2]) { var strong = el("strong"); inline(strong, token.slice(2, -2)); parent.appendChild(strong); }
      else if (match[3]) { var em = el("em"); inline(em, token.slice(1, -1)); parent.appendChild(em); }
      else {
        var close = token.indexOf("](");
        var link = el("a", null, token.slice(1, close));
        link.href = token.slice(close + 2, -1);
        link.target = "_blank";
        link.rel = "noopener noreferrer";
        parent.appendChild(link);
      }
      last = match.index + token.length;
    }
    if (last < text.length) { parent.appendChild(document.createTextNode(text.slice(last))); }
  }
  function codeBlock(language, code) {
    var box = el("div", "st-code");
    var head = el("div", "st-code__head");
    head.appendChild(el("span", null, language || "code"));
    head.appendChild(iconButton("i-copy", "Copy code", function () { copyText(code); }));
    box.appendChild(head);
    var pre = el("pre");
    pre.appendChild(el("code", null, code));
    box.appendChild(pre);
    return box;
  }
  function tableRow(line) {
    var cells = line.trim().replace(/^\|/, "").replace(/\|$/, "").split("|");
    return cells.map(function (cell) { return cell.trim(); });
  }
  function renderMarkdown(target, text) {
    clear(target);
    var lines = text.replace(/\r\n/g, "\n").split("\n");
    var i = 0;
    var paragraph = [];
    function flush() {
      if (!paragraph.length) { return; }
      var p = el("p");
      paragraph.forEach(function (line, index) { if (index) { p.appendChild(el("br")); } inline(p, line); });
      target.appendChild(p);
      paragraph = [];
    }
    while (i < lines.length) {
      var line = lines[i];
      var fence = /^\s*(```|~~~)\s*([\w+#.-]*)\s*$/.exec(line);
      if (fence) {
        flush();
        var body = [];
        i += 1;
        while (i < lines.length && lines[i].trim() !== fence[1]) { body.push(lines[i]); i += 1; }
        target.appendChild(codeBlock(fence[2], body.join("\n")));
        i += 1;
        continue;
      }
      var heading = /^(#{1,6})\s+(.*)$/.exec(line);
      if (heading) { flush(); var h = el(heading[1].length <= 2 ? "h3" : "h4"); inline(h, heading[2]); target.appendChild(h); i += 1; continue; }
      if (/^\s*([-*_])(\s*\1){2,}\s*$/.test(line)) { flush(); target.appendChild(el("hr")); i += 1; continue; }
      if (/^\s*>/.test(line)) {
        flush();
        var quote = [];
        while (i < lines.length && /^\s*>/.test(lines[i])) { quote.push(lines[i].replace(/^\s*>\s?/, "")); i += 1; }
        var blockquote = el("blockquote");
        renderMarkdown(blockquote, quote.join("\n"));
        target.appendChild(blockquote);
        continue;
      }
      if (/^\s*\|.*\|\s*$/.test(line) && i + 1 < lines.length && /^\s*\|?\s*:?-{2,}/.test(lines[i + 1])) {
        flush();
        var wrap = el("div", "table-wrap");
        var table = el("table");
        var headRow = el("tr");
        tableRow(line).forEach(function (cell) { var th = el("th"); inline(th, cell); headRow.appendChild(th); });
        var thead = el("thead"); thead.appendChild(headRow); table.appendChild(thead);
        var tbody = el("tbody");
        i += 2;
        while (i < lines.length && /^\s*\|.*\|\s*$/.test(lines[i])) {
          var row = el("tr");
          tableRow(lines[i]).forEach(function (cell) { var td = el("td"); inline(td, cell); row.appendChild(td); });
          tbody.appendChild(row);
          i += 1;
        }
        table.appendChild(tbody);
        wrap.appendChild(table);
        target.appendChild(wrap);
        continue;
      }
      var bullet = /^\s*([-*+]|\d+[.)])\s+(.*)$/.exec(line);
      if (bullet) {
        flush();
        var ordered = /\d/.test(bullet[1]);
        var list = el(ordered ? "ol" : "ul");
        while (i < lines.length) {
          var item = /^\s*([-*+]|\d+[.)])\s+(.*)$/.exec(lines[i]);
          if (!item || /\d/.test(item[1]) !== ordered) { break; }
          var li = el("li"); inline(li, item[2]); list.appendChild(li);
          i += 1;
        }
        target.appendChild(list);
        continue;
      }
      if (line.trim() === "") { flush(); i += 1; continue; }
      paragraph.push(line);
      i += 1;
    }
    flush();
  }

  // ---------------------------------------------------------------- chat rendering
  function messageNode(message) {
    var node = el("div", "st-msg st-msg--" + message.role);
    var parts = { node: node, thinking: null, thinkingBody: null, searches: null, bubble: null, meta: null };
    if (message.role === "assistant") {
      var details = el("details", "st-collapse");
      var summary = el("summary");
      summary.appendChild(icon("i-thinking", true));
      summary.appendChild(el("span", null, "Thinking"));
      var chevron = icon("i-chevron", true);
      chevron.setAttribute("class", "st-icon st-icon--sm st-chev");
      summary.appendChild(chevron);
      details.appendChild(summary);
      parts.thinkingBody = el("div", "st-collapse__body thinking", message.reasoning || "");
      details.appendChild(parts.thinkingBody);
      details.hidden = !message.reasoning;
      parts.thinking = details;
      node.appendChild(details);
      // Web search: what the model searched for while it answered (the server's strata_search events), in a box as the
      // thinking is (owner 2026-10-28), with the round limit when the answer reached it
      var box = el("details", "st-collapse search-box");
      var head = el("summary");
      head.appendChild(icon("i-tool", true));
      var title = el("span", null, "");
      head.appendChild(title);
      var fold = icon("i-chevron", true);
      fold.setAttribute("class", "st-icon st-icon--sm st-chev");
      head.appendChild(fold);
      box.appendChild(head);
      var body = el("div", "st-collapse__body");
      var list = el("ul", "search-list");
      var note = el("p", "search-limit");
      body.appendChild(list);
      body.appendChild(note);
      box.appendChild(body);
      parts.searches = { box: box, title: title, list: list, note: note };
      fillSearches(parts, message.searches || [], message.searchLimit || 0);
      node.appendChild(box);
    }
    if (message.role === "user" && Array.isArray(message.images) && message.images.length) {
      var shelf = el("div", "msg-pictures");
      message.images.forEach(function (p) {
        var img = el("img");
        img.src = p.url;
        img.alt = p.name;
        img.title = p.name;
        shelf.appendChild(img);
      });
      node.appendChild(shelf);
    }
    parts.bubble = el("div", "st-bubble");
    if (message.role === "assistant") { renderMarkdown(parts.bubble, message.content || ""); } else { parts.bubble.textContent = message.content; }
    parts.bubble.hidden = message.role === "user" && !message.content;
    node.appendChild(parts.bubble);
    parts.meta = el("div", "st-msg__meta");
    fillMeta(parts, message);
    node.appendChild(parts.meta);
    return parts;
  }
  function fillSearches(parts, searches, limit) {
    var s = parts.searches;
    clear(s.list);
    searches.forEach(function (q) { s.list.appendChild(el("li", null, q || "(an empty search)")); });
    s.title.textContent = "Searched the web" + (searches.length ? DOT + searches.length + (searches.length === 1 ? " search" : " searches") : "");
    s.note.textContent = limit ? "Reached the limit of " + limit + " search rounds; the answer uses what was found." : "";
    s.note.hidden = !limit;
    s.box.hidden = !searches.length;
  }
  function fillMeta(parts, message) {
    clear(parts.meta);
    if (message.role === "assistant" && message.stats) { parts.meta.appendChild(el("span", null, message.stats)); }
    if (message.error) { parts.meta.appendChild(el("p", "msg-error", message.error)); }
    if (message.content) {
      parts.meta.appendChild(iconButton("i-copy", "Copy", function () { copyText(message.content); }));
    }
  }
  function renderChat() {
    var chat = $("chat");
    Array.prototype.slice.call(chat.querySelectorAll(".st-msg")).forEach(function (node) { chat.removeChild(node); });
    $("chat-empty").hidden = state.history.length > 0;
    state.history.forEach(function (message) { chat.appendChild(messageNode(message).node); });
    scrollToEnd();
  }
  function scrollToEnd() { var scroll = $("chat-scroll"); scroll.scrollTop = scroll.scrollHeight; }
  function newChat() {
    if (state.controller) { return; }
    state.history = [];
    state.attachments = [];
    save(SLOT_CHAT, null);
    renderAttachments();
    renderChat();
    $("composer-hint").textContent = "";
    $("input").focus();
  }

  // ---------------------------------------------------------------- attachments (text files, pictures)
  // Vision for cards (spec 2026-10-28): a picture goes to the model as an OpenAI image_url part (a data: URL) when the
  // running card reads pictures (its Pictures option is on the GPU or the CPU). The server holds it to vision.max_n and
  // vision.max_side; this page sends at most PICTURES_MAX per question and none over PICTURE_MAX_BYTES.
  var PICTURE_TYPES = ["image/png", "image/jpeg", "image/webp"];
  var PICTURES_MAX = 4;
  var PICTURE_MAX_BYTES = 20 * 1024 * 1024;
  function picturesOffered() {
    if (!launcher.enabled || !launcher.data || !launcher.data.running) { return false; }
    var card = findCard(launcher.data.running);
    var options = card && Array.isArray(card.options) ? card.options : [];
    for (var i = 0; i < options.length; i += 1) {
      if (options[i] && options[i].name === "vision") { return options[i].value === "gpu" || options[i].value === "cpu"; }
    }
    return false;
  }
  function renderAttachments() {
    var box = $("attachments");
    clear(box);
    box.hidden = state.attachments.length === 0;
    state.attachments.forEach(function (file, index) {
      var chip = el("span", "chip", file.name);
      chip.appendChild(iconButton("i-trash", "Remove " + file.name, function () { state.attachments.splice(index, 1); renderAttachments(); }));
      box.appendChild(chip);
    });
  }
  function addFiles(files) {
    Array.prototype.forEach.call(files, function (file) {
      var reader = new FileReader();
      if (oneOf(PICTURE_TYPES, file.type)) {
        if (!picturesOffered()) { toast("error", "This model reads no pictures", file.name + ": start a model with its Pictures option on"); return; }
        var pictures = state.attachments.filter(function (a) { return a.image; }).length;
        if (pictures >= PICTURES_MAX) { toast("error", "At most " + PICTURES_MAX + " pictures per question", file.name); return; }
        if (file.size > PICTURE_MAX_BYTES) { toast("error", "Picture too large (20 MB at most)", file.name); return; }
        reader.onload = function () {
          state.attachments.push({ name: file.name, image: String(reader.result || "") });
          renderAttachments();
        };
        reader.onerror = function () { toast("error", "Could not read", file.name); };
        reader.readAsDataURL(file);
        return;
      }
      reader.onload = function () {
        var text = String(reader.result || "");
        if (text.indexOf("\u0000") >= 0) { toast("error", "Not a text file", file.name); return; }
        state.attachments.push({ name: file.name, text: text });
        renderAttachments();
      };
      reader.onerror = function () { toast("error", "Could not read", file.name); };
      reader.readAsText(file);
    });
  }

  // ---------------------------------------------------------------- sending
  function busy(on) {
    $("send-btn").hidden = on;
    $("stop-btn").hidden = !on;
    $("new-btn").disabled = on;
  }
  // What the server takes: user and assistant turns in strict alternation (this page sends no
  // system turn). A question that never got an answer, as in a chat saved by an older page,
  // would make every later request a 400 (session log issue 40), so the newer question takes
  // its place.
  function conversation(history) {
    var turns = [];
    history.forEach(function (m) {
      if (!m || typeof m.content !== "string" || (!m.content && !(Array.isArray(m.images) && m.images.length))) { return; }
      var last = turns.length ? turns[turns.length - 1].role : "";
      if (m.role === "user" && last === "user") {
        turns[turns.length - 1] = m;
      } else if (m.role === "user" || (m.role === "assistant" && last === "user")) {
        turns.push(m);
      }
    });
    return turns;
  }
  // Owner 2026-10-27 (item 46): the running card says whether its profile takes reasoning_effort (a GLM card's refuses
  // it); without model control the page cannot tell and sends it.
  function sendsReasoning() {
    if (!launcher.enabled || !launcher.data || !launcher.data.running) { return true; }
    var card = findCard(launcher.data.running);
    return !card || card.reasoning_effort !== false;
  }
  // Owner 2026-10-29 ("make sure the card matches the chat settings when the engine is started as default so each model
  // reset it to the recommended"): the running card's thinking level, temperature and top-p - the values it started
  // with, its model's recommended unless changed on the card - are the chat's settings until they are changed here, and
  // each start of a model sets them back to its card's.
  function runningCard() {
    return launcher.enabled && launcher.data && launcher.data.running ? findCard(launcher.data.running) : null;
  }
  function cardOption(card, name) {
    var options = card && Array.isArray(card.options) ? card.options : [];
    for (var i = 0; i < options.length; i += 1) {
      if (options[i] && options[i].name === name) {
        return { value: options[i].value !== undefined && options[i].value !== null ? options[i].value : options[i].default,
                 choices: Array.isArray(options[i].choices) ? options[i].choices : [] };
      }
    }
    return null;
  }
  // The card's settings in the page's terms: Gemma's and flint's thinking on / off is High / Off; a card with no such
  // option keeps the page's own.
  function cardSettings(card) {
    var thinking = cardOption(card, "thinking");
    var level = thinking && thinking.value === "on" ? "high" : thinking && thinking.value === "off" ? "none"
      : thinking && LEVELS[thinking.value] ? thinking.value : null;
    var temperature = cardOption(card, "temperature");
    var topP = cardOption(card, "top_p");
    return { thinking: level, temperature: temperature && typeof temperature.value === "number" ? temperature.value : null,
             top_p: topP && typeof topP.value === "number" ? topP.value : null };
  }
  // MiniMax's adaptive thinking (its card's "auto"): the drawer offers Auto, which names no level.
  function autoOffered() {
    var thinking = cardOption(runningCard(), "thinking");
    return !!thinking && thinking.choices.indexOf("auto") >= 0;
  }
  function settingsFrom(card, s) {
    var from = cardSettings(card);
    var out = Object.assign({}, s);
    out.thinking = from.thinking || DEFAULTS.thinking;
    out.temperature = from.temperature !== null ? from.temperature : DEFAULTS.temperature;
    out.top_p = from.top_p !== null ? from.top_p : DEFAULTS.top_p;
    out.tuned = false;
    out.card = card ? card.name + "|" + JSON.stringify(from) : "";
    return out;
  }
  // Called on every look at the launcher: a running card the settings were not set from (another model, or the same one
  // started with other options) resets them; `started` (this page saw it start) resets them for a restart too.
  function followCard(started) {
    var card = runningCard();
    if (!card || !modelReady()) { return; }
    var next = settingsFrom(card, state.settings);
    if (!started && next.card === state.settings.card) { return; }
    state.settings = next;
    save(SLOT_SETTINGS, next);
    if ($("drawer").getAttribute("data-open") === "true") { showSettings(); }
  }
  // Owner 2026-10-28: the running card's Web search checkbox (listed while the server's web search is on) says whether
  // its model may search; the chat's own switch then turns it off for this browser's chats.
  function searchOffered() {
    if (!launcher.enabled || !launcher.data || !launcher.data.running) { return false; }
    var card = findCard(launcher.data.running);
    var options = card && Array.isArray(card.options) ? card.options : [];
    for (var i = 0; i < options.length; i += 1) {
      if (options[i] && options[i].name === "web_search") { return options[i].value === true; }
    }
    return false;
  }
  // The latest question's pictures go as image_url parts; an earlier question's are named in its words (the request stays
  // within the server's picture limit, and the page does not keep pictures past a reload).
  function pictureNote(m) {
    return (m.images || []).map(function (p) { return "[picture: " + p.name + "]"; }).join(" ");
  }
  // Owner 2026-10-29 ("keep earlier searches in the chat history as upstream's page does"): an answer that searched
  // goes back with its rounds - per round the text before its calls, the calls and the results the model read - and its
  // own text after the last round (upstream's assistantMessages), so a later question builds on what it found. The
  // server takes them as strata_rounds (it takes no tool messages from clients).
  function requestMessages(turns) {
    var last = -1;
    turns.forEach(function (m, i) { if (m.role === "user") { last = i; } });
    return turns.map(function (m, i) {
      if (m.role === "assistant" && Array.isArray(m.rounds) && m.rounds.length) {
        return { role: m.role, content: m.content.slice(m.roundsEnd || 0).trim(), strata_rounds: m.rounds };
      }
      var pictures = Array.isArray(m.images) ? m.images : [];
      if (!pictures.length) { return { role: m.role, content: m.content }; }
      if (i !== last) { return { role: m.role, content: (pictureNote(m) + (m.content ? "\n\n" + m.content : "")) }; }
      var parts = [];
      if (m.content) { parts.push({ type: "text", text: m.content }); }
      pictures.forEach(function (p) { parts.push({ type: "image_url", image_url: { url: p.url } }); });
      return { role: m.role, content: parts };
    });
  }
  // The chat as this browser keeps it: pictures as their names (a data URL is too large to keep).
  function savedChat() {
    return conversation(state.history).map(function (m) {
      if (!Array.isArray(m.images) || !m.images.length) { return m; }
      var kept = {};
      Object.keys(m).forEach(function (k) { if (k !== "images") { kept[k] = m[k]; } });
      kept.content = pictureNote(m) + (m.content ? "\n\n" + m.content : "");
      return kept;
    });
  }
  // A call's arguments as the server sent them (an object), else none.
  function callArguments(search) {
    return search.arguments && typeof search.arguments === "object" && !Array.isArray(search.arguments) ? search.arguments : {};
  }
  // The calls that ran, as rounds in order: each the answer's text since the round before (trimmed), its calls and their
  // results; `end` is where the answer's own text after the last round starts. Null when none ran.
  function roundsOf(calls, text) {
    var ran = calls.filter(function (c) { return c.result !== null && !c.skipped; });
    if (!ran.length) { return null; }
    var rounds = [];
    var pos = 0;
    var numbers = [];
    ran.forEach(function (c) { if (numbers.indexOf(c.round) < 0) { numbers.push(c.round); } });
    numbers.forEach(function (n) {
      var group = ran.filter(function (c) { return c.round === n; });
      var at = Math.min(Math.max(pos, group[0].at || 0), text.length);
      rounds.push({ content: text.slice(pos, at).trim(), calls: group.map(function (c) { return { name: c.name, arguments: c.arguments }; }),
                    results: group.map(function (c) { return c.result; }) });
      pos = at;
    });
    return { rounds: rounds, end: pos };
  }
  function requestBody() {
    var s = state.settings;
    var body = {
      model: state.model,
      messages: requestMessages(conversation(state.history)),
      stream: true,
      stream_options: { include_usage: true },
      reasoning_effort: LEVELS[s.thinking] ? s.thinking : "high"
    };
    if (!sendsReasoning() || s.thinking === "auto") { delete body.reasoning_effort; }
    if (s.search === false && searchOffered()) { body.strata_search = false; }
    if (s.tuned) { body.temperature = Number(s.temperature); body.top_p = Number(s.top_p); }
    if (s.max) { body.max_tokens = Number(s.max); }
    if (s.seed) { body.seed = Number(s.seed); }
    return body;
  }
  function handleEvent(text, run, parts) {
    var lines = text.split("\n");
    for (var i = 0; i < lines.length; i += 1) {
      if (lines[i].indexOf("data: ") !== 0) { continue; }
      var data = lines[i].slice(6);
      if (data === "[DONE]") { return true; }
      var event;
      try { event = JSON.parse(data); } catch (e) { continue; }
      if (event.error) {
        run.error = CHAT_ERRORS[event.error.code] || (event.error.message || "error") + " (" + (event.error.code || "") + ")";
        continue;
      }
      if (event.usage) { run.usage = event.usage; }
      // Web search: the server's search loop says what runs (a call, its result, the round limit)
      var search = event.strata_search;
      if (search && search.event === "limit") { run.limit = search.max_rounds; }
      if (search && (search.event === "call" || search.event === "limit")) {
        if (search.event === "call") {
          setPill("generating", "Searching");
          run.searches.push(search.arguments && typeof search.arguments.query === "string" ? search.arguments.query : "");
          // kept for the history: the call, and where the answer's text was when it came
          run.calls.push({ round: search.round, name: String(search.name || ""), arguments: callArguments(search), at: run.answer.length,
                           result: null, skipped: false });
        }
        fillSearches(parts, run.searches, run.limit);
      } else if (search && search.event === "result") {
        if (search.ok === false && search.skipped !== true) { run.searchFailed = String(search.text || "the search failed"); }
        // the result the model read, on the round's first call still waiting for one (a skipped call never ran)
        var pending = null;
        for (var c = 0; c < run.calls.length && !pending; c += 1) {
          if (run.calls[c].round === search.round && run.calls[c].result === null && !run.calls[c].skipped) { pending = run.calls[c]; }
        }
        if (pending) {
          if (search.skipped === true) { pending.skipped = true; } else { pending.result = String(search.text || ""); }
        }
      }
      var choice = event.choices && event.choices[0];
      if (!choice) { continue; }
      var delta = choice.delta || {};
      if (delta.reasoning_content) {
        if (!run.firstToken) { run.firstToken = performance.now(); setPill("generating", "Thinking"); }
        run.reasoning += delta.reasoning_content;
        parts.thinking.hidden = false;
        if (state.settings.show && !run.answer) { parts.thinking.open = true; }
        parts.thinkingBody.textContent = run.reasoning;
      }
      if (delta.content) {
        if (!run.firstToken) { run.firstToken = performance.now(); }
        if (!run.answer && parts.thinking.open && run.reasoning) { parts.thinking.open = false; }
        setPill("generating", "Writing");
        run.answer += delta.content;
        run.dirty = true;
      }
      if (choice.finish_reason) { run.finish = choice.finish_reason; }
    }
    return false;
  }
  async function send(text) {
    var content = text;
    var images = state.attachments.filter(function (file) { return file.image; }).map(function (file) { return { name: file.name, url: file.image }; });
    var texts = state.attachments.filter(function (file) { return !file.image; });
    if (texts.length) {
      var blocks = texts.map(function (file) { return "File " + file.name + ":\n```\n" + file.text + "\n```"; });
      content = blocks.join("\n\n") + (text ? "\n\n" + text : "");
    }
    if (state.attachments.length) {
      state.attachments = [];
      renderAttachments();
    }
    var user = { role: "user", content: content };
    if (images.length) { user.images = images; }
    state.history.push(user);
    $("chat-empty").hidden = true;
    $("chat").appendChild(messageNode(user).node);
    var reply = { role: "assistant", content: "", reasoning: "", stats: "", error: "" };
    var parts = messageNode(reply);
    parts.bubble.classList.add("cursor");
    $("chat").appendChild(parts.node);
    scrollToEnd();
    var run = { answer: "", reasoning: "", finish: "", usage: null, error: "", firstToken: 0, dirty: false, searches: [], searchFailed: "",
                limit: 0, calls: [] };
    state.controller = new AbortController();
    var started = performance.now();
    busy(true);
    setPill("reading", "Reading");
    $("composer-hint").textContent = "";
    var painter = window.setInterval(function () {
      if (run.dirty) { run.dirty = false; renderMarkdown(parts.bubble, run.answer); scrollToEnd(); }
    }, 80);
    try {
      var response = await api("/v1/chat/completions", {
        method: "POST", headers: { "Content-Type": "application/json" },
        body: JSON.stringify(requestBody()), signal: state.controller.signal
      });
      if (response.status === 401 && !state.keyless) {
        window.clearInterval(painter);
        state.controller = null;
        busy(false);
        state.history.pop();
        signOut("The server no longer accepts this API key.");
        return;
      }
      if (!response.ok) {
        run.error = chatError(response.status, await response.text());
      } else {
        var reader = response.body.getReader();
        var decoder = new TextDecoder();
        var buffer = "";
        var done = false;
        while (!done) {
          var chunk = await reader.read();
          if (chunk.done) { break; }
          buffer += decoder.decode(chunk.value, { stream: true });
          var boundary = buffer.indexOf("\n\n");
          while (boundary >= 0 && !done) {
            done = handleEvent(buffer.slice(0, boundary), run, parts);
            buffer = buffer.slice(boundary + 2);
            boundary = buffer.indexOf("\n\n");
          }
        }
      }
    } catch (e) {
      run.error = e && e.name === "AbortError" ? "Stopped" : "Request failed: " + (e && e.message ? e.message : e);
    }
    window.clearInterval(painter);
    state.controller = null;
    busy(false);
    parts.bubble.classList.remove("cursor");
    renderMarkdown(parts.bubble, run.answer);
    var seconds = (performance.now() - started) / 1000;
    var stats = [];
    if (state.model) { stats.push(state.model); }
    if (run.usage && run.usage.completion_tokens) {
      // The writing speed, from the first token on; a reply of a few tokens can arrive in one
      // chunk, so a rate is shown only when there is enough of it to mean something.
      var tokens = run.usage.completion_tokens;
      var genSeconds = run.firstToken ? (performance.now() - run.firstToken) / 1000 : seconds;
      stats.push(tokens + " tokens");
      if (tokens >= 16 && genSeconds >= 0.25) { stats.push((tokens / genSeconds).toFixed(1) + " tok/s"); }
    }
    stats.push(seconds.toFixed(1) + " s");
    if (run.finish && run.finish !== "stop") { stats.push(run.finish); }
    if (run.searchFailed) { stats.push("a search failed: " + run.searchFailed.slice(0, 160)); }
    reply.content = run.answer;
    reply.reasoning = run.reasoning;
    if (run.searches.length) { reply.searches = run.searches; }
    var kept = roundsOf(run.calls, run.answer);
    if (kept) { reply.rounds = kept.rounds; reply.roundsEnd = kept.end; }
    if (run.limit) { reply.searchLimit = run.limit; }
    reply.stats = stats.join(" - ");
    reply.error = run.error;
    // A turn with an answer stands (a stopped one keeps its text). Without one, the question
    // leaves the conversation (it stays on screen with the error): the server takes
    // alternating user/assistant turns, so two questions in a row would be refused.
    if (run.answer) { state.history.push(reply); } else { state.history.pop(); }
    parts.bubble.hidden = !run.answer;
    fillMeta(parts, reply);
    save(SLOT_CHAT, savedChat());
    pollHealth();
    scrollToEnd();
    $("input").focus();
  }
  function submit() {
    var input = $("input");
    var text = input.value.trim();
    if ((!text && !state.attachments.length) || state.controller || !modelReady()) { return; }
    input.value = "";
    autosize();
    send(text);
  }
  function autosize() {
    var input = $("input");
    input.style.height = "auto";
    input.style.height = Math.min(input.scrollHeight, window.innerHeight * 0.4) + "px";
  }

  // ---------------------------------------------------------------- export
  function exportChat() {
    if (!state.history.length) { toast("info", "Nothing to save yet"); return; }
    var text = "# Strata chat\n\n" + state.history.map(function (m) {
      return (m.role === "user" ? "## You\n\n" : "## " + (state.model || "Assistant") + "\n\n") + m.content;
    }).join("\n\n") + "\n";
    var link = el("a");
    link.href = URL.createObjectURL(new Blob([text], { type: "text/markdown" }));
    link.download = "strata-chat.md";
    document.body.appendChild(link);
    link.click();
    document.body.removeChild(link);
    window.setTimeout(function () { URL.revokeObjectURL(link.href); }, 1000);
  }

  // ---------------------------------------------------------------- sampling drawer
  function showSettings() {
    var s = state.settings;
    var auto = autoOffered();
    if (s.thinking === "auto" && !auto) { s.thinking = DEFAULTS.thinking; }
    $("s-thinking-auto").hidden = !auto;
    Array.prototype.forEach.call($("s-thinking").querySelectorAll("button"), function (b) {
      b.setAttribute("aria-checked", b.getAttribute("data-v") === s.thinking ? "true" : "false");
    });
    $("o-thinking").textContent = LEVELS[s.thinking] || "";
    // Untuned, the page sends neither and the running card's values apply (the sliders stand at them); without model
    // control the page cannot read them, so the starting position is not what runs.
    var card = runningCard();
    var from = cardSettings(card);
    var shown = function (value, cards) {
      return s.tuned ? Number(value).toFixed(2) : cards !== null ? Number(cards).toFixed(2) + " (the card's)" : "model default";
    };
    $("s-temp").value = s.temperature; $("o-temp").textContent = shown(s.temperature, from.temperature);
    $("s-topp").value = s.top_p; $("o-topp").textContent = shown(s.top_p, from.top_p);
    // what the sliders read back (a value past a slider's range stands at its end): moving one is what tunes
    state.shown = { temperature: Number($("s-temp").value), top_p: Number($("s-topp").value) };
    var rest = [];
    var topK = cardOption(card, "top_k");
    var minP = cardOption(card, "min_p");
    if (topK && typeof topK.value === "number") { rest.push("top-k " + topK.value); }
    if (minP && typeof minP.value === "number" && minP.value > 0) { rest.push("min-p " + minP.value); }
    $("s-card-note").textContent = rest.length ? "Also from the card: " + rest.join(", ") + " (set them in the Models tab)" : "";
    $("s-card-note").hidden = !rest.length;
    $("s-max").value = s.max; $("s-seed").value = s.seed;
    $("s-show").setAttribute("aria-checked", s.show ? "true" : "false");
    $("s-search").setAttribute("aria-checked", s.search !== false ? "true" : "false");
    $("s-search-row").hidden = !searchOffered();
  }
  function openDrawer(open) {
    $("drawer").setAttribute("data-open", open ? "true" : "false");
    $("drawer").setAttribute("aria-hidden", open ? "false" : "true");
    $("scrim").hidden = !open;
    if (open) { showSettings(); }
  }
  function applySettings() {
    var s = state.settings;
    var thinking = $("s-thinking").querySelector("[aria-checked=true]");
    s.thinking = thinking ? thinking.getAttribute("data-v") : "high";
    var temperature = Number($("s-temp").value);
    var topP = Number($("s-topp").value);
    var was = state.shown || { temperature: s.temperature, top_p: s.top_p };
    s.tuned = s.tuned || temperature !== was.temperature || topP !== was.top_p;
    s.temperature = temperature;
    s.top_p = topP;
    s.max = $("s-max").value.trim();
    s.seed = $("s-seed").value.trim();
    s.show = $("s-show").getAttribute("aria-checked") === "true";
    s.search = $("s-search").getAttribute("aria-checked") === "true";
    save(SLOT_SETTINGS, s);
    openDrawer(false);
    toast("success", "Settings applied");
  }

  // ---------------------------------------------------------------- about
  function fact(list, label, value, copyable) {
    list.appendChild(el("dt", null, label));
    var dd = el("dd");
    var code = el("code", null, value);
    dd.appendChild(code);
    if (copyable) { dd.appendChild(iconButton("i-copy", "Copy " + label, function () { copyText(value); })); }
    list.appendChild(dd);
  }
  function renderAbout() {
    var model = $("facts-model");
    clear(model);
    fact(model, "Model", state.models.length ? state.models.join(", ") : "-", false);
    fact(model, "Engine", state.health && state.health.engine ? state.health.engine : "-", false);
    fact(model, "Server", window.location.origin, false);
    var apiFacts = $("facts-api");
    clear(apiFacts);
    fact(apiFacts, "Base URL", window.location.origin + "/v1", true);
    fact(apiFacts, "Model", state.model || "-", true);
    fact(apiFacts, "API key", state.keyless ? "not needed (loopback server)" : "the key you signed in with", false);
    $("dark-toggle").setAttribute("aria-checked", window.StrataTheme && window.StrataTheme.get() === "dark" ? "true" : "false");
    refreshAbout();
  }
  // Multi-GPU (owner 2026-10-28): this PC's GPUs as nvidia-smi numbers them (the About's hardware.gpus), for the cards'
  // GPUs choice; null until read. Read once after sign-in; an open card form is redrawn with them.
  var hostGpus = null;
  var hostGpusLoading = false;
  function setHostGpus(list) {
    var had = Array.isArray(hostGpus) ? hostGpus.length : -1;
    hostGpus = Array.isArray(list) ? list.filter(function (g) { return g && typeof g.index === "number" && typeof g.name === "string"; }) : [];
    if (hostGpus.length !== had && launcher.formFor && launcher.data) {
      var card = findCard(launcher.formFor);
      if (card && Array.isArray(card.options) && card.options.some(function (o) { return o && o.kind === "devices"; })) { renderDetail(card, true); }
    }
  }
  async function loadHostGpus() {
    if (hostGpus !== null || hostGpusLoading) { return; }
    hostGpusLoading = true;
    var d = null;
    try {
      var response = await api("/v1/strata/about");
      if (response.ok) { d = JSON.parse(await response.text()); }
    } catch (e) { d = null; }
    hostGpusLoading = false;
    setHostGpus(d && d.hardware && Array.isArray(d.hardware.gpus) ? d.hardware.gpus : []);
  }
  // Owner 2026-10-27: the build of our engines (engine/build.json, which make.py writes) and this PC's hardware, from
  // GET /v1/strata/about.
  async function refreshAbout() {
    var d = null;
    try {
      var response = await api("/v1/strata/about");
      if (response.ok) { d = JSON.parse(await response.text()); }
    } catch (e) { d = null; }
    var build = $("facts-build"), hardware = $("facts-hardware"), engines = $("about-engines");
    clear(build);
    clear(hardware);
    clear(engines);
    var b = d && d.build, h = (d && d.hardware) || {};
    if (!b) {
      $("about-build-note").textContent = d ? "This engine folder has no build record (make.py writes engine/build.json)."
                                            : "The server did not answer.";
    } else {
      $("about-build-note").textContent = "The engine folder's build record, as make.py wrote it.";
      fact(build, "Built", (b.built || "-") + (b.commit ? " from commit " + b.commit : ""), false);
      fact(build, "ggml", b.ggml || "-", false);
      fact(build, "GPU targets", b.cuda_arch || "-", false);
      if (b.features && b.features.length) { fact(build, "Features", b.features.join(", "), false); }
      // one row per library (owner 2026-10-28: the engines read badly as wrapped lines)
      var table = el("table", "st-table about-engines");
      var headRow = el("tr");
      [["Models", ""], ["Library", ""], ["Built from", ""], ["Size", "num"]].forEach(function (h) { headRow.appendChild(el("th", h[1] || null, h[0])); });
      var thead = el("thead");
      thead.appendChild(headRow);
      table.appendChild(thead);
      var tbody = el("tbody");
      // each cell names its column: a phone shows the engines as labelled blocks
      var labelled = function (row, label, cell) { cell.setAttribute("data-label", label); row.appendChild(cell); };
      (b.engines || []).forEach(function (e) {
        var row = el("tr");
        labelled(row, "Models", el("td", null, e.models || "-"));
        var lib = el("td");
        lib.appendChild(el("code", null, e.library));
        labelled(row, "Library", lib);
        labelled(row, "Built from", el("td", null, e.source || "-"));
        labelled(row, "Size", el("td", "num", e.bytes ? fmt(e.bytes / 1048576) + " MB" : "-"));
        tbody.appendChild(row);
      });
      table.appendChild(tbody);
      engines.appendChild(table);
    }
    fact(hardware, "CPU", h.cpu || "-", false);
    fact(hardware, "Memory", h.ram_bytes ? gb(h.ram_bytes, 0) + " GB" : "-", false);
    var gpus = Array.isArray(h.gpus) ? h.gpus : [];
    if (gpus.length > 1) {   // every GPU, numbered as the cards' GPUs choice numbers them
      gpus.forEach(function (g) { fact(hardware, "GPU " + g.index, g.name + (g.mem_bytes ? ", " + gb(g.mem_bytes, 0) + " GB" : ""), false); });
    } else {
      fact(hardware, "GPU", h.gpu || (gpus.length ? gpus[0].name : "-"), false);
    }
    fact(hardware, "System", h.os || "-", false);
    if (d && d.hardware) { setHostGpus(gpus); }
  }

  // ---------------------------------------------------------------- Models (the model launcher)
  // The server's model cards (GET /v1/strata/models, models.control): one row per card, the
  // running one first; the selected card's options are rendered from the server's description
  // (a new option is a server change only); Start and Stop post to the control routes. A server
  // with model control off answers 404: the tab stays hidden and the chat is exactly as before.
  var PROFILES = { qwen: "Qwen engine (upstream Strata)", gemma4: "Gemma 4 engine (strata-gemma4-engine)",
                   raon1: "Raon-1 engine (multiengine_raon1)", flint: "flint engine (multiengine_flint)" };
  var CARD_STATES = { stopped: ["", "Stopped"], starting: ["st-badge--queued", "Starting"],
                      running: ["st-badge--generating", "Running"], failed: ["st-badge--error", "Failed"],
                      calibrating: ["st-badge--reading", "Calibrating"] };
  var NO_MODEL = "No model is running \u2014 open the Models tab";
  // The launcher's chat refusals (spec section 5.4), as sentences; the server's own text otherwise.
  var CHAT_ERRORS = {
    model_switching: "The server is switching models. Send again in a few seconds.",
    no_model_running: "No model is running. Start one in the Models tab.",
    model_not_running: "That model is not running any more. The chat now uses the running model: send again."
  };
  var launcher = { enabled: false, data: null, selected: "", formFor: "", controls: [], limits: [], timer: null,
                   startedAt: 0, acting: false, vram: "", metricsOff: false, engine: undefined, chatFor: undefined,
                   calibrateThenStart: "" };

  function launcherStarting(d) {
    if (!d) { return false; }
    if (d.state === "starting") { return true; }
    // a calibration (spec section 5) is polled and shown like a start: the GPU is the calibration's
    return (Array.isArray(d.data) ? d.data : []).some(function (card) { return card && (card.state === "starting" || card.state === "calibrating"); });
  }
  // Chat is possible: always with model control off (today's page); else only with a card running.
  function modelReady() {
    return !launcher.enabled || !!(launcher.data && launcher.data.state === "running" && launcher.data.running);
  }
  function cardsOf(d) { return d && Array.isArray(d.data) ? d.data.filter(function (card) { return card && typeof card.name === "string"; }) : []; }
  function findCard(name) {
    var cards = cardsOf(launcher.data);
    for (var i = 0; i < cards.length; i += 1) { if (cards[i].name === name) { return cards[i]; } }
    return null;
  }
  function startingName(d) {
    var cards = cardsOf(d).filter(function (card) { return card.state === "starting" || card.state === "calibrating"; });
    return cards.length ? cards[0].name : d && d.running ? d.running : "";
  }
  function profileText(profile) { return PROFILES[profile] || (profile ? "Engine profile " + profile : ""); }
  function stateBadge(name) {
    var badge = CARD_STATES[name] || ["", name || "-"];
    return el("span", "st-badge " + badge[0], badge[1]);
  }
  // A bound just above zero (top_p's open lower bound is the smallest double) reads "above 0".
  function num(n) { if (n > 0 && n < 1e-6) { return "above 0"; } return Math.floor(n) === n ? fmt(n) : String(n); }
  function valueText(v) { return typeof v === "boolean" ? (v ? "on" : "off") : typeof v === "number" ? num(v) : String(v); }
  // A listed value's label from the server ("128K", "1M", "Off"); otherwise the value itself.
  function labelOf(option, v) {
    if (Array.isArray(option.choices) && Array.isArray(option.choice_labels)) {
      var at = option.choices.indexOf(v);
      if (at >= 0 && at < option.choice_labels.length) { return String(option.choice_labels[at]); }
    }
    return valueText(v);
  }
  function boundsText(option) {
    var parts = [];
    var listed = option.kind !== "choice" && Array.isArray(option.choices);  // a dropdown shows its own range
    if (!listed && typeof option.min === "number" && typeof option.max === "number") { parts.push(num(option.min) + " to " + num(option.max)); }
    if (option.default !== undefined && option.default !== null) { parts.push("default " + labelOf(option, option.default)); }
    return parts.join(DOT);
  }

  // One control per option kind; each returns { node, read, set }: read() gives { value } or
  // { error } (a sentence naming the option) or { skip } (sent by nobody: the server keeps it).
  function switchControl(option, value) {
    var toggle = el("button", "st-toggle");
    toggle.type = "button";
    toggle.id = "opt-" + option.name;
    toggle.setAttribute("role", "switch");
    toggle.setAttribute("aria-label", option.label || option.name);
    var set = function (v) { toggle.setAttribute("aria-checked", v === true ? "true" : "false"); };
    toggle.addEventListener("click", function () { set(toggle.getAttribute("aria-checked") !== "true"); });
    set(value);
    return { node: toggle, read: function () { return { value: toggle.getAttribute("aria-checked") === "true" }; }, set: set };
  }
  function numberControl(option, value, integral, withAuto) {
    var name = option.label || option.name;
    var hasMin = typeof option.min === "number";
    var hasMax = typeof option.max === "number";
    var wrap = el("div", "option-number");
    var input = el("input", "st-input");
    input.type = "number";
    input.id = "opt-" + option.name;
    input.step = integral ? "1" : "any";
    if (hasMin) { input.min = String(option.min); }
    if (hasMax) { input.max = String(option.max); }
    input.placeholder = hasMin && hasMax ? num(option.min) + " to " + num(option.max) : "a number";
    var auto = null;
    if (withAuto) {
      var autoLabel = el("label", "option-auto");
      auto = el("input");
      auto.type = "checkbox";
      auto.setAttribute("aria-label", name + ": auto");
      auto.addEventListener("change", function () { input.disabled = auto.checked; if (!auto.checked) { input.focus(); } });
      autoLabel.appendChild(auto);
      autoLabel.appendChild(document.createTextNode("auto"));
      wrap.appendChild(autoLabel);
    }
    wrap.appendChild(input);
    var set = function (v) {
      var isAuto = withAuto && v === "auto";
      if (auto) { auto.checked = isAuto; }
      input.disabled = isAuto;
      input.value = typeof v === "number" && isFinite(v) ? String(v) : "";
    };
    var read = function () {
      if (auto && auto.checked) { return { value: "auto" }; }
      var text = input.value.trim();
      var n = Number(text);
      if (!text || !isFinite(n)) { return { error: name + ": enter a number" + (withAuto ? " or tick auto" : "") }; }
      if (integral && Math.floor(n) !== n) { return { error: name + ": enter a whole number" }; }
      if ((hasMin && n < option.min) || (hasMax && n > option.max)) {
        return { error: name + ": " + (hasMin && hasMax ? "from " + num(option.min) + " to " + num(option.max) : hasMin ? "at least " + num(option.min) : "at most " + num(option.max)) };
      }
      return { value: n };
    };
    set(value);
    return { node: wrap, read: read, set: set };
  }
  function choiceControl(option, value) {
    var choices = Array.isArray(option.choices) ? option.choices : [];
    var labels = Array.isArray(option.choice_labels) && option.choice_labels.length === choices.length ? option.choice_labels : null;
    var select = el("select", "st-input");
    select.id = "opt-" + option.name;
    choices.forEach(function (choice, i) {
      var item = el("option", null, labels ? String(labels[i]) : valueText(choice));
      item.value = String(choice);
      if (labels && typeof choice === "number" && String(labels[i]) !== String(choice)) { item.title = fmt(choice) + " tokens"; }
      select.appendChild(item);
    });
    var set = function (v) { select.value = v === undefined || v === null ? "" : String(v); };
    var read = function () {
      var picked = choices.filter(function (choice) { return String(choice) === select.value; });
      return picked.length ? { value: picked[0] } : { error: (option.label || option.name) + ": pick one of the choices" };
    };
    // at_most (KV streaming under the context): values above the ceiling are not offered; a choice
    // above it moves to the largest one left.
    var limit = function (ceiling) {
      var best = null;
      choices.forEach(function (choice, i) {
        var over = typeof choice === "number" && typeof ceiling === "number" && choice > ceiling;
        select.options[i].disabled = over;
        select.options[i].hidden = over;
        if (!over) { best = choice; }
      });
      var current = choices.filter(function (choice) { return String(choice) === select.value; });
      if (current.length && typeof ceiling === "number" && typeof current[0] === "number" && current[0] > ceiling && best !== null) {
        select.value = String(best);
      }
    };
    set(value);
    return { node: select, read: read, set: set, limit: limit, element: select };
  }
  // Multi-GPU (owner 2026-10-28): the GPUs a model runs on, one box per GPU of this PC (as nvidia-smi numbers them);
  // none ticked is auto. Two or more split the model across them; an engine that runs on one (max 1) takes one. Where
  // this PC has one GPU the choice is not shown and nothing is sent (the card keeps its own).
  function devicesControl(option, value) {
    var gpus = Array.isArray(hostGpus) ? hostGpus : [];
    var most = typeof option.max === "number" ? option.max : 16;
    var wrap = el("div", "option-devices");
    wrap.id = "opt-" + option.name;
    var boxes = [];
    gpus.forEach(function (g) {
      var label = el("label", "option-auto");
      var box = el("input");
      box.type = "checkbox";
      box.value = String(g.index);
      box.addEventListener("change", function () {   // one GPU at most: ticking one clears the others
        if (most === 1 && box.checked) { boxes.forEach(function (b) { if (b !== box) { b.checked = false; } }); }
      });
      label.appendChild(box);
      label.appendChild(document.createTextNode("GPU " + g.index + ": " + g.name + (g.mem_bytes ? " (" + gb(g.mem_bytes, 0) + " GB)" : "")));
      boxes.push(box);
      wrap.appendChild(label);
    });
    var set = function (v) {
      var picked = typeof v === "string" && v !== "auto" ? v.split(",") : [];
      boxes.forEach(function (b) { b.checked = picked.indexOf(b.value) >= 0; });
    };
    var read = function () {
      if (gpus.length < 2) { return { skip: true }; }
      var picked = boxes.filter(function (b) { return b.checked; }).map(function (b) { return b.value; });
      if (picked.length > most) { return { error: (option.label || option.name) + ": at most " + most + (most === 1 ? " GPU" : " GPUs") }; }
      return { value: picked.length ? picked.join(",") : "auto" };
    };
    set(value);
    return { node: wrap, read: read, set: set, hidden: gpus.length < 2 };
  }
  function optionControl(option, value) {
    switch (option.kind) {
      case "devices": return devicesControl(option, value);
      case "boolean": return switchControl(option, value);
      case "integer": return Array.isArray(option.choices) ? choiceControl(option, value) : numberControl(option, value, true, false);
      case "decimal": return numberControl(option, value, false, false);
      case "choice": return choiceControl(option, value);
      case "integer_or_auto": return numberControl(option, value, true, true);
      case "decimal_or_auto": return numberControl(option, value, false, true);
      default:  // a kind this page does not know: shown, never sent (the server keeps its value)
        return { node: el("code", "option-fixed", JSON.stringify(value)), read: function () { return { skip: true }; }, set: function () {} };
    }
  }
  function buildOptions(card) {
    var box = $("model-options");
    clear(box);
    launcher.formFor = card.name;
    launcher.controls = [];
    launcher.limits = [];
    var options = Array.isArray(card.options) ? card.options.filter(function (o) {
      // a Qwen3.6 file without its own draft layer: its MTP switch would switch nothing
      return o && typeof o.name === "string" &&
             !(o.name === "mtp" && card.draft && card.draft.kind === "inside" && card.draft.in_file === false);
    }) : [];
    if (!options.length) { box.appendChild(el("p", "muted small", "This card has no options to set.")); }
    options.forEach(function (option) {
      var control = optionControl(option, option.value === undefined || option.value === null ? option.default : option.value);
      var field = el("div", "st-field model-option");
      var label = el("label", "st-label");
      label.htmlFor = "opt-" + option.name;
      label.appendChild(document.createTextNode(option.label || option.name));
      var bounds = boundsText(option);
      if (bounds) { label.appendChild(el("output", null, bounds)); }
      field.appendChild(label);
      field.appendChild(control.node);
      if (option.help) { field.appendChild(el("span", "muted small", option.help)); }
      field.hidden = control.hidden === true;
      box.appendChild(field);
      launcher.controls.push({ option: option, read: control.read, set: control.set, field: field, limit: control.limit,
                               element: control.element });
    });
    // An option with at_most follows the other option's dropdown (KV streaming under the context).
    launcher.controls.forEach(function (c) {
      if (!c.option.at_most || !c.limit) { return; }
      var ceiling = findControl(c.option.at_most);
      if (!ceiling || !ceiling.element) { return; }
      var apply = function () { var r = ceiling.read(); if (r.value !== undefined) { c.limit(r.value); } };
      ceiling.element.addEventListener("change", apply);
      launcher.limits.push(apply);
      apply();
    });
  }
  function findControl(name) {
    for (var i = 0; i < launcher.controls.length; i += 1) { if (launcher.controls[i].option.name === name) { return launcher.controls[i]; } }
    return null;
  }
  function collectOptions() {
    var values = {};
    var errors = [];
    launcher.controls.forEach(function (c) {
      c.field.classList.remove("invalid");
      var result = c.read();
      if (result.error) { errors.push(result.error); c.field.classList.add("invalid"); return; }
      if (!result.skip) { values[c.option.name] = result.value; }
    });
    return { values: values, error: errors.join("; ") };
  }
  function resetOptions() {
    launcher.controls.forEach(function (c) { c.field.classList.remove("invalid"); c.set(c.option.default); });
    launcher.limits.forEach(function (apply) { apply(); });
    showStartError("");
  }

  function progressText(d) {
    var calibrating = cardsOf(d).filter(function (card) { return card.state === "calibrating"; });
    if (calibrating.length) {
      var run = calibrating[0].calibration && calibrating[0].calibration.running;
      return "Calibrating " + calibrating[0].name + (run && run.trials ? DOT + "trial " + run.trial + " of " + run.trials : "");
    }
    if (launcherStarting(d)) {
      var seconds = launcher.startedAt ? Math.max(0, Math.round((Date.now() - launcher.startedAt) / 1000)) : 0;
      return "Loading " + (startingName(d) || "the model") + DOT + seconds + " s" + (launcher.vram ? DOT + launcher.vram : "");
    }
    if (d.state === "running" && d.running) { return d.running + " is running"; }
    if (d.state === "failed") { return "The last start failed"; }
    return "No model is running";
  }
  function renderDetail(card, rebuild) {
    var d = launcher.data;
    $("model-detail").hidden = !card || !d;
    if (!card || !d) { return; }
    $("model-detail-name").textContent = card.name;
    var badgeBox = $("model-detail-state");
    clear(badgeBox);
    badgeBox.appendChild(stateBadge(card.state));
    $("model-detail-profile").textContent = profileText(card.profile);
    var error = card.state === "failed" ? card.error || d.error || "The engine stopped before it was ready." : "";
    $("model-detail-error").textContent = error ? "Did not start: " + error : "";
    $("model-detail-error").hidden = !error;
    if (rebuild) { buildOptions(card); }
    var starting = launcherStarting(d);
    var thisRuns = d.state === "running" && d.running === card.name;
    $("model-start").textContent = thisRuns ? "Restart" : "Start";
    $("model-start").disabled = starting || launcher.acting;
    $("model-stop").disabled = !d.running || launcher.acting;
    $("model-defaults").disabled = !launcher.controls.length;
    // GUI-first start-up: a card the page made shows its folder (Change... follows moved files) and its Danger zone.
    var pageCard = card.page_card === true;
    $("model-folder").hidden = !pageCard || typeof card.folder !== "string";
    $("model-folder-path").textContent = pageCard && typeof card.folder === "string" ? card.folder : "";
    $("model-folder-change").disabled = launcher.acting || card.state === "starting";
    var draft = pageCard && card.draft && typeof card.draft === "object" ? card.draft : null;
    // owner 2026-10-29: a Qwen3.6 card's draft layer is in its model file (kind "inside"): said, never chosen
    var inside = draft !== null && draft.kind === "inside";
    $("model-draft").hidden = !draft;
    $("model-draft-path").textContent = !draft ? "" : inside
      ? (draft.in_file === true ? "In the model file: the card drafts with it (the MTP switch below turns it off)"
                                : "This model file has no MTP draft layer, so the card runs without one")
      : (typeof draft.path === "string" ? draft.path : "None found: the model runs without one");
    $("model-draft-change").hidden = inside;
    $("model-draft-change").disabled = launcher.acting || card.state === "starting";
    $("model-draft-help").textContent = inside
      ? "This model's draft layer is part of its model file (a Qwen3.6 GGUF from Unsloth carries one): there is no folder to point at."
      : "The model's draft layer guesses tokens ahead, so answers come faster. Kept somewhere else? Point the card at it.";
    $("danger-zone").hidden = !pageCard;
    // Owner 2026-10-28: the card can be taken off the list, its files kept - the running one too (stopped first)
    $("model-remove-box").hidden = !pageCard;
    $("model-remove").disabled = launcher.acting || card.state === "starting" || card.state === "calibrating";
    var inUse = card.state === "running" || card.state === "starting" || d.running === card.name;
    $("model-delete").disabled = inUse || launcher.acting;
    $("model-delete").title = inUse ? "Stop the model first" : "";
    $("model-loading").hidden = !starting;
    $("model-progress").textContent = progressText(d);
    renderCalibration(card, starting);
  }
  // GUI-first start-up (spec section 5.3): the card's calibration - what it is, the speed it found, its progress while
  // it runs - with Calibrate / Recalibrate, Cancel and the "Recalibrate when settings change" switch.
  var CAL_STATES = { missing: ["", "Not calibrated"], fresh: ["st-badge--generating", "Calibrated"],
                     stale: ["st-badge--error", "Out of date"], settings_changed: ["st-badge--queued", "Settings changed"] };
  function calibrationText(card, cal) {
    if (cal.running && cal.running.stage === "prompt reading") {
      return "Measuring how this PC reads prompts (one start of the engine, a few minutes); the chat is unavailable meanwhile.";
    }
    if (cal.running) {
      var run = cal.running;
      return "Measuring the engine's settings: trial " + run.trial + " of " + run.trials + (run.stage ? " (" + run.stage + ")" : "") +
             (typeof run.best_tok_s === "number" ? DOT + "best so far " + fmt(run.best_tok_s, 1) + " tok/s" : "") +
             ". Each trial is one start of the engine; the chat is unavailable meanwhile.";
    }
    if (cal.error) { return "The last calibration did not finish: " + cal.error; }
    var found = typeof cal.before === "number" && typeof cal.after === "number"
      ? fmt(cal.before, 1) + " tok/s with the engine's defaults, " + fmt(cal.after, 1) + " tok/s " +
        (Array.isArray(cal.args) && cal.args.length ? "with " + cal.args.join(" ") : "(the defaults were fastest)") : "";
    if (cal.state === "fresh") { return "Calibrated" + (cal.date ? " " + String(cal.date).slice(0, 10) : "") + ": " + found + "."; }
    if (cal.state === "stale") { return "This PC, the engine or the model changed since the calibration, so it is not used: calibrate again."; }
    if (cal.state === "settings_changed") {
      return "These options differ from the ones it was calibrated at, so it is not used at them: recalibrate" +
             (found ? " (it found " + found + ")" : "") + ".";
    }
    // (the engines whose sweep is a few short starts calibrate before a page card's first start; a large model's when asked)
    if (card.page_card === true && cal.first_start === true) {
      return "Not calibrated yet: the first start measures this PC's fastest engine settings first (a few minutes), then serves.";
    }
    return "Not calibrated: Calibrate measures this PC's fastest engine settings for this model, each candidate one start of " +
           "the engine (" + (cal.first_start === true ? "a few minutes" : "for a large model an hour or more") + ").";
  }
  // MiniMax: the prefill calibration (how this PC reads prompts), which the engine keeps in the model's pack
  function prefillText(card, p) {
    if (!p) {
      return card.page_card === true
        ? "Prompt reading: the first start measures how this PC reads prompts first (a few minutes)."
        : "Prompt reading: not measured yet; Calibrate measures it.";
    }
    if (p.state === "failed") {
      return "Prompt reading: the last measurement did not finish, so the engine reads prompts with its defaults; Calibrate measures again.";
    }
    if (p.state === "stale") {
      return "Prompt reading: this PC, the engine or the model changed since it was measured; the next start measures it again.";
    }
    return "Prompt reading" + (p.date ? " (" + String(p.date).slice(0, 10) + ")" : "") + ": chunks of " + p.chunk + " tokens, " +
           (p.gemm === "hipblaslt" ? "hipBLASLt" : "BLAS") + " matrix products, " + fmt(p.tok_s, 0) + " tok/s (the engine's defaults " +
           fmt(p.default_tok_s, 0) + " tok/s).";
  }
  function renderCalibration(card, starting) {
    var cal = card.calibration && typeof card.calibration === "object" ? card.calibration : null;
    $("model-calibration").hidden = !cal;
    if (!cal) { return; }
    var badge = CAL_STATES[cal.running ? "" : cal.state] || ["st-badge--reading", "Calibrating"];
    var box = $("model-calibration-state");
    clear(box);
    box.appendChild(el("span", "st-badge " + badge[0], cal.running ? "Calibrating" : badge[1]));
    $("model-calibration-text").textContent = calibrationText(card, cal);
    var prefill = $("model-calibration-prefill");
    prefill.hidden = !("prefill" in cal);
    prefill.textContent = "prefill" in cal ? prefillText(card, cal.prefill) : "";
    var run = cal.running;
    $("model-calibration-progress").hidden = !run;
    $("model-calibration-bar").style.width = run && run.trials ? Math.min(100, (100 * run.trial) / run.trials) + "%" : "0%";
    $("model-calibrate").textContent = cal.state === "missing" ? "Calibrate" : "Recalibrate";
    $("model-calibrate").disabled = !!run || starting || launcher.acting;
    $("model-calibrate-cancel").hidden = !run;
    $("model-calibration-auto").checked = cal.auto === true;
    $("model-calibration-auto").disabled = launcher.acting;
  }
  // `thenStart` true: the calibrate-first question's "Calibrate, then start" (it said a running model stops); a click on
  // Calibrate passes its event.
  async function calibrateModel(thenStart) {
    var start = thenStart === true;
    var d = launcher.data;
    var card = findCard(launcher.selected);
    if (!d || !card || launcher.acting || launcherStarting(d)) { return; }
    showStartError("");
    var collected = collectOptions();
    if (collected.error) { showStartError(collected.error); return; }
    var running = d.running && d.state === "running" ? d.running : "";
    if (!start && running && !window.confirm("Calibrating stops " + running + " for a few minutes (each trial is one start of the engine). Go on?")) { return; }
    launcher.acting = true;
    renderDetail(card, false);
    var answer = await downloadsAnswer(start ? api("/v1/strata/models/calibrate", jsonPost({ name: card.name, options: collected.values, then_start: true }))
                                             : api("/v1/strata/models/calibrate", jsonPost({ name: card.name, options: collected.values })));
    launcher.acting = false;
    if (answer.signedOut) { return; }
    if (answer.error) { showStartError(answer.error); await refreshLauncher(); return; }
    launcher.calibrateThenStart = start ? card.name : "";
    launcher.startedAt = Date.now();
    stopLauncherPoll();
    await pollLauncher();
  }
  // Owner 2026-10-29 ("during calibration, allow users to cancel calibration if they choose to"): Cancel stops it and
  // keeps nothing; when it was the page's own "calibrate, then start", the start it was for is offered without it once the
  // calibration has ended (still on that card's page).
  async function cancelCalibration() {
    var thenStart = launcher.calibrateThenStart;
    var answer = await downloadsAnswer(api("/v1/strata/models/calibrate/cancel", jsonPost({})));
    if (answer.signedOut) { return; }
    if (answer.error) { showStartError(answer.error); await refreshLauncher(); return; }
    launcher.calibrateThenStart = "";
    toast("info", "Calibration cancelled", "Nothing was kept.");
    await refreshLauncher();
    if (!thenStart) { return; }
    for (var i = 0; i < 120 && launcherStarting(launcher.data); i += 1) {
      await new Promise(function (resolve) { setTimeout(resolve, 500); });
      await refreshLauncher();
    }
    if (launcher.selected !== thenStart || launcherStarting(launcher.data)) { return; }
    if (window.confirm(thenStart + ": calibration cancelled, nothing was kept. Start it now without calibrating?")) {
      await startModel(true);
    }
  }
  // Owner 2026-10-29 ("starting an uncalibrated server recommend calibrating first and when answered yes, it will
  // calibrate"): Start on a card never calibrated on this PC asks first. Resolves "calibrate", "start" or "" (cancelled,
  // Esc too).
  function askCalibrateFirst(card, running) {
    return new Promise(function (resolve) {
      var dialog = $("calibrate-first");
      $("calibrate-first-title").textContent = "Calibrate " + card.name + " first?";
      $("calibrate-first-text").textContent = card.name + " has not been calibrated on this PC. Calibrating first is " +
        "recommended: it tries the engine's settings (each trial is one start of the model, a few minutes in all), then " +
        "starts the model with the fastest. You can cancel it while it runs." +
        (running ? " " + running + " is stopped first." : "");
      var answered = false;
      var done = function (answer) {
        if (answered) { return; }
        answered = true;
        if (dialog.open) { dialog.close(); }
        resolve(answer);
      };
      $("calibrate-first-yes").onclick = function () { done("calibrate"); };
      $("calibrate-first-no").onclick = function () { done("start"); };
      $("calibrate-first-cancel").onclick = function () { done(""); };
      dialog.onclose = function () { done(""); };
      if (typeof dialog.showModal === "function") { dialog.showModal(); } else { done(""); }
    });
  }
  async function setAutoRecalibrate() {
    var card = findCard(launcher.selected);
    if (!card) { return; }
    var on = $("model-calibration-auto").checked;
    var answer = await downloadsAnswer(api("/v1/strata/models/calibrate/auto", jsonPost({ name: card.name, on: on })));
    if (answer.signedOut) { return; }
    if (answer.error) { showStartError(answer.error); }
    await refreshLauncher();
  }
  function renderModels(rebuild) {
    var d = launcher.data;
    if (!d) { return; }
    var cards = cardsOf(d);
    var running = cards.filter(function (card) { return card.name === d.running; });
    var rest = cards.filter(function (card) { return card.name !== d.running; });
    var list = $("model-list");
    clear(list);
    $("models-empty").hidden = cards.length > 0;
    running.concat(rest).forEach(function (card) {
      var row = el("button", "model-row");
      row.type = "button";
      row.setAttribute("aria-pressed", card.name === launcher.selected ? "true" : "false");
      var text = el("span", "model-row__text");
      text.appendChild(el("span", "model-row__name", card.name));
      text.appendChild(el("span", "muted small", profileText(card.profile)));
      row.appendChild(text);
      row.appendChild(stateBadge(card.state));
      row.addEventListener("click", function () {
        var changed = launcher.selected !== card.name;
        if (changed) { showStartError(""); }
        launcher.selected = card.name;
        renderModels(changed);
      });
      list.appendChild(row);
    });
    renderDetail(findCard(launcher.selected), rebuild || launcher.formFor !== launcher.selected);
  }

  function setComposerAvailable(on, text) {
    $("input").disabled = !on;
    $("send-btn").disabled = !on;
    $("attach-btn").disabled = !on;
    $("no-model").hidden = on;
    if (!on) { $("no-model-text").textContent = text || NO_MODEL; }
  }
  function setLauncherEnabled(on) {
    launcher.enabled = on;
    $("models-denied").hidden = true;
    $("tab-btn-models").hidden = !on;
    $("header-model").hidden = !on || !$("view-login").hidden;
    $("local-models").hidden = !on;
    $("models-subtab-download").hidden = !on && !downloads.enabled;
    if (on) { return; }
    launcher.data = null;
    stopLauncherPoll();
    setComposerAvailable(true);
    if (!$("view-models").hidden) { showView("chat"); }
  }
  // A client the server does not let control models (a remote one without models.control_remote):
  // the tab stays, showing the reason and the switch instead of the cards; chat is unchanged.
  function setLauncherDenied() {
    launcher.enabled = false;
    launcher.data = null;
    stopLauncherPoll();
    setComposerAvailable(true);
    $("tab-btn-models").hidden = false;
    $("header-model").hidden = true;
    clear($("model-list"));
    $("models-empty").hidden = true;
    $("model-detail").hidden = true;
    $("models-denied").hidden = false;
    $("local-models").hidden = true;
  }
  // The chat follows the launcher: the header names the running model, the composer is off
  // with a pointer to the Models tab while none runs, and /v1/models is asked again when the
  // running card changes, so state.model is the running card's name.
  function syncChat() {
    var d = launcher.data;
    if (!launcher.enabled || !d) { return; }
    var ready = modelReady();
    var starting = launcherStarting(d);
    $("header-model").textContent = ready ? d.running : starting ? "Starting " + (startingName(d) || "a model") : "No model running";
    $("header-model").setAttribute("data-state", ready ? "running" : starting ? "starting" : "none");
    setComposerAvailable(ready, starting ? "Starting " + (startingName(d) || "a model") + DOT + "the chat opens when it is ready"
                                : d.state === "failed" ? "The model did not start \u2014 open the Models tab" : NO_MODEL);
    // Asked once per change of the running card (not on every 2 s poll while one loads).
    var want = ready ? d.running : "";
    if (want !== launcher.chatFor) {
      launcher.chatFor = want;
      if (want !== state.model || (want && state.models.indexOf(want) < 0)) { refreshChatModel(want); }
    }
    followCard(false);
  }
  async function refreshChatModel(want) {
    var models;
    try { models = await listModels(); } catch (e) { return; }
    if (models === null) { return; }
    state.models = models;
    state.model = want && models.indexOf(want) >= 0 ? want : models.length ? models[0] : "";
    if (!$("view-about").hidden) { renderAbout(); }
  }
  function chatError(status, body) {
    var code = "";
    try {
      var parsed = JSON.parse(body);
      code = parsed && parsed.error && typeof parsed.error.code === "string" ? parsed.error.code : "";
    } catch (e) { code = ""; }
    if (!CHAT_ERRORS[code]) { return errorText(status, body); }
    // The page's picture of the running model was stale: read it again (and /v1/models with it).
    launcher.chatFor = undefined;
    if (launcher.enabled) { refreshLauncher(); } else { refreshChatModel(""); }
    return CHAT_ERRORS[code];
  }

  function applyLauncher(parsed) {
    var wasStarting = launcherStarting(launcher.data) || launcher.startedAt > 0;
    launcher.data = parsed;
    setLauncherEnabled(true);
    var starting = launcherStarting(parsed);
    if (starting && !launcher.startedAt) { launcher.startedAt = Date.now(); }
    if (!findCard(launcher.selected)) {
      var cards = cardsOf(parsed);
      launcher.selected = parsed.running && findCard(parsed.running) ? parsed.running : cards.length ? cards[0].name : "";
    }
    renderModels(false);
    syncChat();
    if (starting) {
      if (!launcher.timer) { launcher.timer = window.setTimeout(pollLauncher, 2000); }
      return;
    }
    if (!wasStarting) { return; }
    launcher.startedAt = 0;
    launcher.vram = "";
    var failed = cardsOf(parsed).filter(function (card) { return card.state === "failed"; });
    if (parsed.state === "running" && parsed.running) {
      followCard(true);   // a start this page saw: the chat's settings are the card's again
      toast("success", parsed.running + " is running");
    }
    else if (parsed.state === "failed" || failed.length) {
      toast("error", "The model did not start", (failed.length && failed[0].error) || parsed.error || "");
    }
  }
  async function refreshLauncher() {
    var response, text;
    try { response = await api("/v1/strata/models"); text = await response.text(); } catch (e) { return; }
    if (response.status === 404) { setLauncherEnabled(false); return; }
    // 403: this client may not control models (models.control_remote off): the tab says why and
    // how to allow it; chat stays as it is.
    if (response.status === 403) { setLauncherDenied(); return; }
    if (!response.ok) { return; }
    var parsed;
    try { parsed = JSON.parse(text); } catch (e) { return; }
    if (!parsed || typeof parsed !== "object" || !Array.isArray(parsed.data)) { return; }
    applyLauncher(parsed);
  }
  // While a card loads: the launcher's state every 2 s, and the Monitor's VRAM figure (when
  // the Monitor is on) for the progress line.
  async function sampleVram() {
    if (launcher.metricsOff) { return; }
    try {
      var response = await api("/metrics");
      var text = await response.text();
      if (response.status === 404) { launcher.metricsOff = true; return; }
      if (!response.ok) { return; }
      var hw = (JSON.parse(text) || {}).hardware || {};
      launcher.vram = hw.gpu_mem_total ? "GPU memory " + gb(hw.gpu_mem_used) + " / " + gb(hw.gpu_mem_total, 0) + " GB" : "";
    } catch (e) { /* the progress line goes without the figure */ }
  }
  async function pollLauncher() {
    launcher.timer = null;
    if (launcherStarting(launcher.data)) { await sampleVram(); }
    await refreshLauncher();
    // A poll that failed (the server busy or briefly away) asks again while the card loads.
    if (launcher.enabled && launcherStarting(launcher.data) && !launcher.timer) { launcher.timer = window.setTimeout(pollLauncher, 2000); }
  }
  function stopLauncherPoll() { if (launcher.timer) { window.clearTimeout(launcher.timer); launcher.timer = null; } }

  function showStartError(text) {
    $("model-start-error").textContent = text;
    $("model-start-error").hidden = !text;
  }
  // The server's refusal (its usual error body): the fixed message, the option it names (by
  // its label) and the code.
  function refusal(status, body) {
    var parsed = null;
    try { parsed = JSON.parse(body); } catch (e) { parsed = null; }
    if (!parsed || !parsed.error) { return { text: "HTTP " + status, param: "" }; }
    var param = typeof parsed.error.param === "string" ? parsed.error.param : "";
    var control = param ? findControl(param) : null;
    var text = parsed.error.message || "HTTP " + status;
    if (param) { text += " (option: " + (control ? control.option.label || param : param) + ")"; }
    if (parsed.error.code) { text += " [" + parsed.error.code + "]"; }
    return { text: text, param: param };
  }
  // `without` true: start without calibrating, asked already (a cancelled calibrate-then-start); a click on Start passes
  // its event.
  async function startModel(without) {
    var d = launcher.data;
    var card = findCard(launcher.selected);
    if (!d || !card || launcher.acting || launcherStarting(d)) { return; }
    showStartError("");
    var collected = collectOptions();
    if (collected.error) { showStartError(collected.error); return; }
    var running = d.running && d.state === "running" ? d.running : "";
    var withoutCalibrating = without === true;
    var cal = card.calibration && typeof card.calibration === "object" ? card.calibration : null;
    if (!withoutCalibrating && cal && !cal.running && cal.state === "missing") {
      var choice = await askCalibrateFirst(card, running);
      if (!choice) { return; }
      if (choice === "calibrate") { await calibrateModel(true); return; }
      withoutCalibrating = true;
    } else if (!withoutCalibrating) {
      var question = running === card.name
        ? "Restart " + card.name + " with these options? The chat is unavailable while it loads."
        : "Stop " + running + " and start " + card.name + "? The chat is unavailable while it loads.";
      if (running && !window.confirm(question)) { return; }
    }
    launcher.calibrateThenStart = "";
    launcher.acting = true;
    renderDetail(card, false);
    var response, text;
    try {
      response = await api("/v1/strata/models/start", {
        method: "POST", headers: { "Content-Type": "application/json" },
        body: withoutCalibrating ? JSON.stringify({ name: card.name, options: collected.values, calibrate: false })
                                 : JSON.stringify({ name: card.name, options: collected.values })
      });
      text = await response.text();
    } catch (e) {
      launcher.acting = false;
      renderDetail(card, false);
      showStartError("Cannot reach the server: " + (e && e.message ? e.message : e));
      return;
    }
    launcher.acting = false;
    if (response.status === 401 && !state.keyless) { signOut("The server no longer accepts this API key."); return; }
    if (!response.ok) {
      var refused = refusal(response.status, text);
      var bad = refused.param ? findControl(refused.param) : null;
      if (bad) { bad.field.classList.add("invalid"); }
      showStartError(refused.text);
      await refreshLauncher();
      return;
    }
    launcher.startedAt = Date.now();
    launcher.vram = "";
    stopLauncherPoll();
    await pollLauncher();
  }
  async function stopModel() {
    var d = launcher.data;
    if (!d || !d.running || launcher.acting) { return; }
    showStartError("");
    launcher.acting = true;
    renderModels(false);
    var response, text;
    try {
      response = await api("/v1/strata/models/stop", { method: "POST" });
      text = await response.text();
    } catch (e) {
      launcher.acting = false;
      renderModels(false);
      showStartError("Cannot reach the server: " + (e && e.message ? e.message : e));
      return;
    }
    launcher.acting = false;
    if (response.status === 401 && !state.keyless) { signOut("The server no longer accepts this API key."); return; }
    if (!response.ok) { showStartError(refusal(response.status, text).text); } else { toast("info", "Model stopped"); }
    await refreshLauncher();
  }

  // ---------------------------------------------------------------- Downloads (models.downloads)
  // The Models tab's Downloads section (GET /v1/strata/downloads): the free space of the downloads
  // drive, one card per catalog GGUF (Strata's, Project Maya's) and per pasted link, Download,
  // Pause, Resume and Cancel, and a progress line for the running transfer. A paused download keeps
  // its partial files on the server; Resume is a start, which continues from them. The server decides everything (the
  // Hugging Face and .gguf rules, the sizes and hashes, the space check); the page pre-checks a
  // pasted link only so that an obvious mistake is named without a round trip. A server with
  // downloads off answers 404 and the section stays hidden.
  // Download cards are grouped into family sections (owner: "organize them into sections ... based
  // on similarities"); the key is each card's `family` field, mapped to a friendly heading.
  var DL_FAMILIES = [["qwen", "Qwen3.8-Flash-Next"], ["unsloth", "Qwen3.8-Flash-Next (Unsloth)"],
                     ["gyro", "Qwen3.8-Flash-Next (Gyro)"], ["coder", "Qwen3.8-Flash-Next Coder"],
                     ["qwen36", "Qwen3.6-35B-A3B"], ["swift", "Swift 1.5"],
                     ["glm53", "GLM-5.3-Flash (Project Maya)"], ["deepseek", "DeepSeek-V4.1-Flash (Project Maya)"],
                     ["minimax", "MiniMax-M3.1-Flash (Project Maya; MiniMax Community License)"],
                     // owner 2026-10-28: our own pins (catalog source m-strata)
                     ["gemma4", "Gemma 4 (Google)"], ["flint", "flint (RecursiveSI)"], ["raon1", "Raon-1-mini (KRAFTON)"],
                     ["custom", "Your downloads"]];
  var DL_STATES = {
    available: ["", "Available"], queued: ["st-badge--queued", "Queued"], downloading: ["st-badge--reading", "Downloading"],
    verifying: ["st-badge--reading", "Verifying"], preparing: ["st-badge--reading", "Preparing"],
    ready: ["st-badge--generating", "Ready"], installed: ["st-badge--generating", "Downloaded"],
    failed: ["st-badge--error", "Failed"], cancelled: ["st-badge--queued", "Cancelled"],
    paused: ["st-badge--queued", "Paused"]
  };
  var DL_ACTIVE = ["queued", "downloading", "verifying", "preparing"];
  var DL_STARTABLE = ["available", "failed", "cancelled"];
  var DL_CANCELLABLE = ["queued", "downloading", "paused"];
  // Owner 2026-10-28 ("change the banner from available to Locally Provided vs Downloaded"): what this PC already has of an
  // entry, as the server's list says it ("have", the one rule the page and strata-tui share).
  var DL_HAVE = { downloaded: ["st-badge--generating", "Downloaded"], local: ["st-badge--reading", "Locally provided"],
                  other: ["st-badge--queued", "Other version on this PC"] };
  // Owner 2026-10-28 ("put accordion control so that user can expand per related section"): the family groups the viewer
  // opened, remembered for this tab (sessionStorage; the page works without it).
  var DL_OPEN_SLOT = "strata.downloads.open";
  // A pasted link: an https address on huggingface.co whose path (before any query or fragment)
  // names a .gguf file. The server checks the link again, and the downloader every redirect.
  var HF_LINK = /^https:\/\/huggingface\.co\//;
  var GGUF_FILE = /\.gguf$/i;
  var downloads = { enabled: false, data: null, timer: null, acting: false, errors: Object.create(null),
                    states: Object.create(null), confirm: null, folder: "", folderFree: null,
                    folderTotal: null, folderVolume: "",     // the picked folder's drive: its size and name (the folders route)
                    vision: Object.create(null),   // the "Vision files" boxes ticked here (id -> bool)
                    open: readOpenGroups(),        // groups the viewer opened (remembered)
                    closedHere: Object.create(null), openHere: Object.create(null), wanted: Object.create(null),
                    justAdded: "" };               // the id of a link just started (its group opens)

  function oneOf(list, value) { return list.indexOf(value) >= 0; }
  function dlCards(d) {
    return d && Array.isArray(d.data) ? d.data.filter(function (card) { return card && typeof card.id === "string" && typeof card.name === "string"; }) : [];
  }
  function downloadsActive(d) { return dlCards(d).some(function (card) { return oneOf(DL_ACTIVE, card.state); }); }
  // Sizes in decimal GB with one decimal, as the server's refusal text writes them.
  function dgb(bytes) { return fmt((Number(bytes) || 0) / 1e9, 1); }
  function clock(seconds) {
    var s = Math.max(0, Math.round(seconds));
    var rest = s % 60;
    return Math.floor(s / 60) + ":" + (rest < 10 ? "0" : "") + rest;
  }
  function freeBytes() {
    if (downloads.folder) { return typeof downloads.folderFree === "number" ? downloads.folderFree : null; }
    var disk = downloads.data && downloads.data.disk;
    // the server writes 0 of 0 bytes for a drive it could not read: not known
    return disk && typeof disk.free_bytes === "number" && Number(disk.total_bytes) > 0 ? disk.free_bytes : null;
  }
  // GUI-first start-up (spec section 4.3): where downloads go - the server's downloads folder, or one picked here, sent
  // with every start and link check (the server checks the folder and its free space again).
  function withFolder(payload) {
    if (downloads.folder) { payload.folder = downloads.folder; }
    return payload;
  }
  // A card that cannot start for lack of room: the chosen folder's free space, else the server's own check. An installed
  // one needs none (its Set up downloads nothing).
  function lacksRoom(card) {
    if (card.state === "installed") { return false; }
    if (!downloads.folder) { return card.fits === false; }
    var free = freeBytes();
    return free !== null && typeof card.needed_bytes === "number" && card.needed_bytes > free;
  }
  // Section 4.3a: a model that already has a card - from this download, or the same model added from a folder on this PC -
  // is not offered for download; since 2026-10-28 it is shown as what this PC has ("downloaded", "local" or null). A
  // server without "have" still names a download that is set up or installed.
  function haveOf(card) {
    if (card.have === "downloaded" || card.have === "local") { return card.have; }
    if (card.have !== undefined) { return null; }
    return card.state === "ready" || card.state === "installed" ? "downloaded" : null;
  }
  // What an entry shows as on this PC: its have, unless a download of it is under way or paused (its state says how that
  // goes); null otherwise.
  function onThisPc(card) {
    var have = haveOf(card);
    return have !== null && !oneOf(DL_ACTIVE, card.state) && card.state !== "paused" ? have : null;
  }
  // A card has a file of that name and another size (another revision): said, with that card, while the download - the
  // catalog's version - stays offered.
  function otherVersion(card) {
    return card.have === "other" && !oneOf(DL_ACTIVE, card.state) && card.state !== "paused";
  }
  function haveBadge(have) {
    var badge = DL_HAVE[have];
    return el("span", "st-badge " + badge[0], badge[1]);
  }
  async function chooseDestination() {
    var picked = await pickFolder("Where downloads go", "Pick a drive and a folder for the model files, or make a new private " +
                                  "folder there. Its free space is checked before anything downloads.", "folder", downloads.folder);
    if (!picked || !picked.folder) { return; }
    downloads.folder = picked.folder;
    downloads.folderFree = typeof picked.free === "number" ? picked.free : null;
    downloads.folderTotal = null;
    downloads.folderVolume = "";
    renderDownloads();
    lookupVolume(picked.folder);
  }
  // Back to the server's own downloads folder (nothing is sent with the requests then).
  function useDefaultDestination() {
    downloads.folder = "";
    downloads.folderFree = null;
    downloads.folderTotal = null;
    downloads.folderVolume = "";
    renderDownloads();
    $("downloads-dest-change").focus();   // the button pressed is hidden now
  }
  // A path as compared here: a Windows path (a drive letter) in one case with one separator, no trailing separator but a
  // drive's or the root's own.
  function folderKey(path) {
    var text = String(path || "");
    if (/^[A-Za-z]:/.test(text)) { text = text.replace(/\//g, "\\").toLowerCase(); }
    while (text.length > 1 && /[\\/]$/.test(text) && !/^[a-z]:\\$/.test(text)) { text = text.slice(0, -1); }
    return text;
  }
  function sameFolder(a, b) { return folderKey(a) === folderKey(b); }
  // Whether `path` is on the drive or mount `root`.
  function under(path, root) {
    var p = folderKey(path);
    var r = folderKey(root);
    if (p.indexOf(r) !== 0) { return false; }
    var rest = p.slice(r.length);
    return rest === "" || /[\\/]$/.test(r) || rest.charAt(0) === "/" || rest.charAt(0) === "\\";
  }
  // The picked folder's drive, its name and size, from this PC's drives (the folders route; the longest that holds it).
  async function lookupVolume(folder) {
    var answer = await downloadsAnswer(api("/v1/strata/folders", jsonPost({})));
    if (answer.signedOut || answer.error || downloads.folder !== folder) { return; }
    var volumes = answer.body && Array.isArray(answer.body.volumes) ? answer.body.volumes : [];
    var best = null;
    volumes.forEach(function (v) {
      if (v && typeof v.path === "string" && v.path && under(folder, v.path) && (!best || v.path.length > best.path.length)) { best = v; }
    });
    if (!best) { return; }
    downloads.folderVolume = best.path + (best.label ? " (" + String(best.label) + ")" : "");
    downloads.folderTotal = typeof best.total_bytes === "number" ? best.total_bytes : null;
    renderDownloads();
  }
  // Owner 2026-10-28 ("make prominent the device and download location selection so that it does not look just a bunch of
  // text"): the Download to panel. A path in its chip shortens in the middle: the start gives way, the last folder stays.
  function middleChip(node, path) {
    clear(node);
    var end = path.length;
    while (end > 1 && /[\\/]/.test(path.charAt(end - 1))) { end -= 1; }   // a trailing separator is not the last folder
    var cut = Math.max(path.lastIndexOf("\\", end - 1), path.lastIndexOf("/", end - 1));
    var tail = cut > 0 && cut < end - 1 ? path.slice(cut) : "";
    node.appendChild(el("span", "dl-dest__path-head", tail ? path.slice(0, cut) : path));
    if (tail) { node.appendChild(el("span", "dl-dest__path-tail", tail)); }
    node.title = path;
  }
  function destinationDrive() {
    var disk = downloads.data && downloads.data.disk;
    if (!downloads.folder) { return disk && disk.root ? String(disk.root) : "-"; }
    if (downloads.folderVolume) { return downloads.folderVolume; }
    var letter = /^([A-Za-z]:)/.exec(downloads.folder);
    return letter ? letter[1].toUpperCase() : "This PC";
  }
  // The largest download waiting to go where downloads now go - queued, under way or paused there, or the link being
  // confirmed - when the free space cannot hold it; null when everything fits or the free space is not known.
  function destinationShort() {
    var free = freeBytes();
    if (free === null) { return null; }
    var disk = downloads.data && downloads.data.disk;
    var here = downloads.folder || (disk && typeof disk.folder === "string" ? disk.folder : "");
    var pending = dlCards(downloads.data).filter(function (card) {
      return oneOf(DL_CANCELLABLE, card.state) && sameFolder(card.folder, here);
    }).map(function (card) { return { name: card.name, bytes: Number(card.needed_bytes) || 0 }; });
    var offer = downloads.confirm && downloads.confirm.data;
    if (offer) { pending.push({ name: offer.name ? String(offer.name) : "the pasted link", bytes: Number(offer.needed_bytes) || 0 }); }
    var largest = null;
    pending.forEach(function (item) { if (!largest || item.bytes > largest.bytes) { largest = item; } });
    return largest && largest.bytes > free ? largest : null;
  }
  function renderDestination() {
    var d = downloads.data;
    if (!d) { return; }
    var disk = d.disk || null;
    var box = $("downloads-dest");
    var picked = downloads.folder !== "";
    var path = picked ? downloads.folder : (disk && typeof disk.folder === "string" ? disk.folder : "");
    $("downloads-dest-drive").textContent = destinationDrive();
    middleChip($("downloads-dest-path"), path || "the server's downloads folder");
    $("downloads-dest-default").textContent = picked ? "Picked on this page (the server's own downloads folder is the default)"
                                                     : "The server's downloads folder (default)";
    $("downloads-dest-reset").hidden = !picked;
    var line = $("downloads-disk");
    var free = freeBytes();
    var total = picked ? downloads.folderTotal : (disk && typeof disk.total_bytes === "number" ? disk.total_bytes : null);
    if (picked) {
      line.textContent = "Free space there: " + (typeof downloads.folderFree === "number" ? dgb(downloads.folderFree) + " GB" : "-") +
                         (typeof total === "number" && total > 0 ? " of " + dgb(total) + " GB" : "");
      line.hidden = false;
    } else if (disk && typeof disk.free_bytes === "number" && typeof disk.total_bytes === "number") {
      var root = String(disk.root || "the downloads drive");
      line.textContent = "Free space on " + root + (/:$/.test(root) ? " " : ": ") + dgb(disk.free_bytes) + " GB of " + dgb(disk.total_bytes) + " GB";
      line.hidden = false;
    } else {
      line.hidden = true;
    }
    var short = destinationShort();
    var gauge = $("downloads-dest-gauge");
    var known = free !== null && typeof total === "number" && total > 0;
    gauge.hidden = !known;
    if (known) {
      var used = Math.max(0, Math.min(100, Math.round((100 * (total - free)) / total)));
      $("downloads-dest-gauge-bar").style.width = used + "%";
      gauge.setAttribute("aria-valuenow", String(used));
      gauge.setAttribute("aria-valuetext", dgb(total - free) + " GB used, " + dgb(free) + " GB free of " + dgb(total) + " GB");
      var tone = short ? "danger" : used >= 90 ? "warn" : "";
      if (tone) { gauge.setAttribute("data-tone", tone); } else { gauge.removeAttribute("data-tone"); }
    }
    var warn = $("downloads-dest-warn");
    warn.textContent = short ? "Not enough room for " + short.name + ": " + dgb(short.bytes) + " GB needed, " + dgb(free) + " GB free" : "";
    warn.hidden = !short;
    box.setAttribute("data-short", short ? "true" : "false");
  }
  // The accordion (owner 2026-10-28): the groups the viewer opened, kept for this tab as the page keeps everything (load
  // and save: sessionStorage, every access guarded; the threat model keeps this script off lasting storage). Without
  // storage every group starts as its contents say.
  function readOpenGroups() {
    var open = Object.create(null);
    var saved = load(DL_OPEN_SLOT, null);
    if (saved && typeof saved === "object" && !Array.isArray(saved)) {
      Object.keys(saved).forEach(function (key) { if (saved[key] === true) { open[key] = true; } });
    }
    return open;
  }
  function saveOpenGroups() { save(DL_OPEN_SLOT, downloads.open); }
  // A group with a download under way, a paused one, a failed one (of a model not on this PC), or a link just added opens
  // itself.
  function groupWantsOpen(members) {
    return members.some(function (card) {
      return oneOf(DL_ACTIVE, card.state) || card.state === "paused" || (card.state === "failed" && onThisPc(card) === null) ||
             card.id === downloads.justAdded;
    });
  }
  // Open: the viewer opened it (remembered), or it opened itself on this page and the viewer has not closed it since; it
  // opens itself again when something new is under way in it, or a link was just added to it.
  function groupOpen(key, members) {
    var wants = groupWantsOpen(members);
    var added = members.some(function (card) { return card.id === downloads.justAdded; });
    if (wants && (!downloads.wanted[key] || added)) { delete downloads.closedHere[key]; downloads.openHere[key] = true; }
    if (added) { downloads.justAdded = ""; }
    downloads.wanted[key] = wants;
    if (downloads.closedHere[key]) { return false; }
    return downloads.open[key] === true || downloads.openHere[key] === true;
  }
  function setGroupOpen(key, open) {
    if (open) {
      downloads.open[key] = true;
      delete downloads.closedHere[key];
    } else {
      delete downloads.open[key];
      delete downloads.openHere[key];
      downloads.closedHere[key] = true;
    }
    saveOpenGroups();
  }
  // "6 models, 1 downloading, 2 on this PC" (DOT between them): what the group holds, for its header.
  function groupSummary(members) {
    var count = function (test) { return members.filter(test).length; };
    var parts = [fmt(members.length) + (members.length === 1 ? " model" : " models")];
    var running = count(function (card) { return card.state === "downloading" || card.state === "verifying" || card.state === "preparing"; });
    var queued = count(function (card) { return card.state === "queued"; });
    var paused = count(function (card) { return card.state === "paused"; });
    var failed = count(function (card) { return card.state === "failed"; });
    var here = count(function (card) { return onThisPc(card) !== null; });
    if (running) { parts.push(fmt(running) + " downloading"); }
    if (queued) { parts.push(fmt(queued) + " queued"); }
    if (paused) { parts.push(fmt(paused) + " paused"); }
    if (failed) { parts.push(fmt(failed) + " failed"); }
    if (here) { parts.push(fmt(here) + " on this PC"); }
    return parts.join(DOT);
  }
  function noRoomText(needed) {
    var free = freeBytes();
    return "Insufficient disk space: " + dgb(needed) + " GB needed, " + (free === null ? "-" : dgb(free)) + " GB free";
  }
  function dlBadge(name) {
    var badge = Object.prototype.hasOwnProperty.call(DL_STATES, name) ? DL_STATES[name] : ["", String(name || "-")];
    return el("span", "st-badge " + badge[0], badge[1]);
  }
  // The running transfer's figures: the card's (all its files), else the transfer's own.
  function transferFigures(card, active) {
    if (typeof card.total === "number" && card.total > 0) { return { done: Number(card.bytes) || 0, total: card.total }; }
    if (active && typeof active.total === "number" && active.total > 0) { return { done: Number(active.bytes) || 0, total: active.total }; }
    return { done: 0, total: Number(card.size_bytes) || 0 };
  }
  function transferText(figures, active) {
    var pct = figures.total > 0 ? Math.min(100, Math.floor((100 * figures.done) / figures.total)) : 0;
    var parts = [dgb(figures.done) + " of " + dgb(figures.total) + " GB (" + pct + "%)"];
    if (active && typeof active.rate_bps === "number" && active.rate_bps > 0) { parts.push(fmt(active.rate_bps / 1e6, 1) + " MB/s"); }
    if (active && typeof active.eta_s === "number" && isFinite(active.eta_s) && active.eta_s >= 0) { parts.push("ETA " + clock(active.eta_s)); }
    return parts.join(DOT);
  }

  function downloadCard(card, d) {
    var active = d.active && d.active.id === card.id ? d.active : null;
    // Owner 2026-10-28: a model already on this PC (from this download, or the same model added from a folder) wears what
    // it is - Downloaded or Locally provided - and names its card, with no Download; one under way or paused keeps its
    // state and its controls. Another version of its file wears that, and keeps its Download.
    var have = onThisPc(card);
    var onPc = have !== null;
    var other = !onPc && otherVersion(card);
    // installed, but the server does not count it as on this PC: its card step never ran (owner 2026-10-28); Set up runs it
    var setUp = !onPc && card.state === "installed";
    var box = el("div", "dl-card");
    box.setAttribute("data-state", String(card.state));
    if (onPc) { box.setAttribute("data-have", have); }
    if (other) { box.setAttribute("data-have", "other"); }
    var head = el("div", "dl-card__head");
    head.appendChild(el("span", "dl-card__name", card.name));
    head.appendChild(onPc ? haveBadge(have) : other ? haveBadge("other") : dlBadge(card.state));
    box.appendChild(head);
    if (card.about) { box.appendChild(el("p", "muted small", String(card.about))); }
    // Maya's catalog pins no sizes: the server reads them from Hugging Face when the download starts
    var facts = [card.size_known === false ? "size read from Hugging Face at the start" : dgb(card.size_bytes) + " GB",
                 fmt(card.files) + (card.files === 1 ? " file" : " files")];
    if (card.repo) { facts.push(String(card.repo)); }
    box.appendChild(el("p", "dl-card__facts", facts.join(DOT)));
    // Vision for cards (spec 2026-10-28): the model's picture encoder, a second file, comes too when ticked; its card then
    // reads pictures. Offered before the download starts (and on a resume); afterwards the card says it came.
    var visionBytes = Number(card.vision_bytes) || 0;
    var offerVision = !onPc && visionBytes > 0 && (oneOf(DL_STARTABLE, card.state) || card.state === "paused");
    if (offerVision) {
      var visionLabel = el("label", "option-auto dl-card__vision");
      var visionBox = el("input");
      visionBox.type = "checkbox";
      visionBox.checked = wantsVision(card);
      visionBox.disabled = downloads.acting;
      visionBox.addEventListener("change", function () { downloads.vision[card.id] = visionBox.checked; });
      visionLabel.appendChild(visionBox);
      visionLabel.appendChild(document.createTextNode("Vision files (+" + dgb(visionBytes) + " GB): read pictures with this model"));
      box.appendChild(visionLabel);
    } else if (card.vision === true) {
      box.appendChild(el("p", "muted small", "With its vision files: the model reads pictures"));
    }
    if (setUp) {
      box.appendChild(el("p", "dl-card__here", "Its files are on this PC, but its card was not made: Set up makes it (nothing is downloaded again)"));
    }
    if (other) {
      box.appendChild(el("p", "dl-card__here", "Another version of this file is on this PC" +
                         (card.have_card ? " (its card: " + String(card.have_card) + ")" : "") + "; a download brings the catalog's"));
    }
    if (onPc) {
      box.appendChild(el("p", "dl-card__here", "Already on this PC" + (card.have_card ? "; its card: " + String(card.have_card) : "")));
      // downloaded without a card (download only): the note says why
      if (!card.have_card && card.note) { box.appendChild(el("p", "muted small", String(card.note))); }
    } else if (card.runnable === false && card.note) {
      box.appendChild(el("p", "dl-card__note", String(card.note)));
    } else if (card.note) {
      box.appendChild(el("p", "muted small", String(card.note)));
    }
    var startable = !onPc && (oneOf(DL_STARTABLE, card.state) || setUp);
    var noRoom = startable && lacksRoom(card);
    if (noRoom) { box.appendChild(el("p", "msg-warn", noRoomText(card.needed_bytes))); }
    if (card.state === "paused" && lacksRoom(card)) { box.appendChild(el("p", "msg-warn", noRoomText(card.needed_bytes))); }
    if (card.state === "downloading" || card.state === "paused") {
      var paused = card.state === "paused";
      var figures = transferFigures(card, paused ? null : active);
      var bar = el("div", "st-progress");
      var fill = el("div", "st-progress__bar");
      fill.style.width = (figures.total > 0 ? Math.min(100, (100 * figures.done) / figures.total) : 0) + "%";
      bar.appendChild(fill);
      box.appendChild(bar);
      box.appendChild(el("p", "dl-card__progress", paused ? "Paused at " + transferText(figures, null) + "; Resume continues from there"
                                                          : transferText(figures, active)));
      if (!paused && active && typeof active.file === "string" && active.file && card.files > 1) { box.appendChild(el("p", "muted small", "Now: " + active.file)); }
    } else if (card.state === "verifying" || card.state === "preparing") {
      var wait = el("div", "st-progress model-loading");
      wait.appendChild(el("div", "st-progress__bar"));
      box.appendChild(wait);
      box.appendChild(el("p", "dl-card__progress", card.state === "verifying" ? "Verifying: checking each file's size and SHA-256"
                                                                              : "Preparing: building the model's pack and its card"));
    } else if (card.state === "queued") {
      box.appendChild(el("p", "dl-card__progress", "Queued: starts when the download before it ends"));
    }
    // an earlier attempt that failed does not matter for a model already here
    if (card.state === "failed" && !onPc) {
      box.appendChild(el("p", "msg-error", "Download failed: " + (card.error ? String(card.error) : "no reason given")));
    }
    var refused = downloads.errors[card.id];
    if (refused) { box.appendChild(el("p", "msg-error", refused)); }
    var actions = el("div", "dl-card__actions");
    if (startable) {
      var download = el("button", "st-btn st-btn--primary st-btn--sm", setUp ? "Set up" : "Download");
      download.type = "button";
      download.disabled = noRoom || downloads.acting;
      if (noRoom) { download.title = "Not enough free space on the downloads drive"; }
      download.addEventListener("click", function () { startDownload(card); });
      actions.appendChild(download);
    }
    if (card.state === "paused") {
      var resume = el("button", "st-btn st-btn--primary st-btn--sm", "Resume");
      resume.type = "button";
      resume.disabled = lacksRoom(card) || downloads.acting;
      if (lacksRoom(card)) { resume.title = "Not enough free space on the downloads drive"; }
      resume.addEventListener("click", function () { startDownload(card); });
      actions.appendChild(resume);
    }
    if (card.state === "queued" || card.state === "downloading") {
      var pause = el("button", "st-btn st-btn--secondary st-btn--sm", "Pause");
      pause.type = "button";
      pause.disabled = downloads.acting;
      pause.title = "Stop for now and keep what is downloaded; Resume continues from there";
      pause.addEventListener("click", function () { pauseDownload(card); });
      actions.appendChild(pause);
    }
    if (oneOf(DL_CANCELLABLE, card.state)) {
      var cancel = el("button", "st-btn st-btn--secondary st-btn--sm", "Cancel");
      cancel.type = "button";
      cancel.disabled = downloads.acting;
      cancel.addEventListener("click", function () { cancelDownload(card); });
      actions.appendChild(cancel);
    }
    if (actions.firstChild) { box.appendChild(actions); }
    return box;
  }
  function renderDownloads() {
    var d = downloads.data;
    if (!d) { return; }
    renderDestination();
    var list = $("downloads-list");
    // a group header in focus keeps it across the redraw (the list is redrawn on every poll)
    var focused = document.activeElement && list.contains(document.activeElement) ? document.activeElement.id : "";
    clear(list);
    var cards = dlCards(d);
    var known = DL_FAMILIES.map(function (group) { return group[0]; });
    // Owner 2026-10-28: each family group an accordion section - its header a button naming the family and what it holds,
    // its cards shown when it is open
    DL_FAMILIES.concat([["", "Other models"]]).forEach(function (group) {
      var members = cards.filter(function (card) { return group[0] ? card.family === group[0] : !oneOf(known, card.family); });
      if (!members.length) { return; }
      var key = group[0] || "other";
      var open = groupOpen(key, members);
      var section = el("div", "dl-group");
      section.setAttribute("data-family", key);
      var panel = el("div", "dl-cards");
      panel.id = "dl-panel-" + key;
      var toggle = el("button", "dl-group__head");
      toggle.type = "button";
      toggle.id = "dl-head-" + key;
      toggle.setAttribute("aria-expanded", open ? "true" : "false");
      toggle.setAttribute("aria-controls", panel.id);
      var chevron = icon("i-chevron", true);
      chevron.setAttribute("class", "st-icon st-icon--sm dl-group__chevron");
      chevron.setAttribute("aria-hidden", "true");
      toggle.appendChild(chevron);
      var words = el("span", "dl-group__words");
      words.appendChild(el("span", "dl-group__title", group[1]));
      words.appendChild(el("span", "dl-group__summary", groupSummary(members)));
      toggle.appendChild(words);
      toggle.addEventListener("click", function () {
        var now = toggle.getAttribute("aria-expanded") !== "true";
        setGroupOpen(key, now);
        toggle.setAttribute("aria-expanded", now ? "true" : "false");
        panel.hidden = !now;
      });
      section.appendChild(toggle);
      panel.setAttribute("role", "region");
      panel.setAttribute("aria-labelledby", toggle.id);
      panel.hidden = !open;
      members.forEach(function (card) { panel.appendChild(downloadCard(card, d)); });
      section.appendChild(panel);
      list.appendChild(section);
    });
    if (focused && $(focused)) { $(focused).focus(); }
  }

  function setDownloadsShown(on) {
    downloads.enabled = on;
    $("downloads").hidden = !on;
    $("downloads-body").hidden = !on;
    $("downloads-denied").hidden = true;
    // the Download sub-tab shows with downloads, and with model control (model files already on this PC)
    $("models-subtab-download").hidden = !on && !launcher.enabled;
    if (on) { return; }
    if (modelsSubtab === "download" && !launcher.enabled) { showModelsSubtab("start"); }  // nothing left there: fall back
    downloads.data = null;
    stopDownloadsPoll();
  }
  // A client the server does not let download (a remote one without models.control_remote): the
  // section shows the reason and the switch instead of the cards, like the Models tab's note.
  function setDownloadsDenied() {
    downloads.enabled = false;
    downloads.data = null;
    stopDownloadsPoll();
    $("downloads").hidden = false;
    $("downloads-body").hidden = true;
    $("downloads-denied").hidden = false;
    $("models-subtab-download").hidden = false;
  }
  function applyDownloads(parsed) {
    var before = downloads.states;
    var after = Object.create(null);
    var readyNow = [];
    dlCards(parsed).forEach(function (card) {
      var was = before[card.id];
      after[card.id] = card.state;
      if (!was || was === card.state) { return; }
      if (card.state === "ready") { readyNow.push(card); }
      else if (card.state === "installed") { toast("success", card.name + " is downloaded", card.note ? String(card.note) : ""); }
      else if (card.state === "failed") { toast("error", card.name + " did not download", card.error ? String(card.error) : ""); }
    });
    downloads.states = after;
    downloads.data = parsed;
    setDownloadsShown(true);
    populateProfiles();
    renderDownloads();
    // A card turned ready: the launcher has its new model card, so the list above is read again.
    if (readyNow.length) { refreshLauncher(); toast("success", readyNow[0].name + " is ready", "Its card is in the model list."); }
    if (!downloadsActive(parsed)) { stopDownloadsPoll(); return; }
    if (!downloads.timer) { downloads.timer = window.setTimeout(pollDownloads, 2000); }
  }
  async function refreshDownloads() {
    var response, text;
    try { response = await api("/v1/strata/downloads"); text = await response.text(); } catch (e) { return; }
    if (response.status === 404) { setDownloadsShown(false); return; }
    // 403: this client may not download (models.control_remote off): the section says why.
    if (response.status === 403) { setDownloadsDenied(); return; }
    if (!response.ok) { return; }
    var parsed;
    try { parsed = JSON.parse(text); } catch (e) { return; }
    if (!parsed || typeof parsed !== "object" || !Array.isArray(parsed.data)) { return; }
    applyDownloads(parsed);
  }
  // While a card is queued, downloading, verifying or preparing: the list every 2 s.
  async function pollDownloads() {
    downloads.timer = null;
    await refreshDownloads();
    // A poll that failed (the server busy or briefly away) asks again while a transfer runs.
    if (downloads.enabled && downloadsActive(downloads.data) && !downloads.timer) { downloads.timer = window.setTimeout(pollDownloads, 2000); }
  }
  function stopDownloadsPoll() { if (downloads.timer) { window.clearTimeout(downloads.timer); downloads.timer = null; } }

  function jsonPost(payload) { return { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(payload) }; }
  // A download route's answer: { body } on success, { error } with the server's refusal as text
  // (its message and code: 409 already queued, 507 insufficient_storage with both figures, ...),
  // or { signedOut } when the key is no longer accepted.
  async function downloadsAnswer(request) {
    var response, text;
    try { response = await request; text = await response.text(); } catch (e) {
      return { error: "Cannot reach the server: " + (e && e.message ? e.message : e) };
    }
    if (response.status === 401 && !state.keyless) { signOut("The server no longer accepts this API key."); return { signedOut: true }; }
    if (!response.ok) { return { error: errorText(response.status, text), code: errorCode(text) }; }
    var parsed = null;
    try { parsed = text ? JSON.parse(text) : null; } catch (e) { parsed = null; }
    return { body: parsed };
  }
  // The box as ticked here, else as the server kept it (a resume after a restart).
  function wantsVision(card) {
    if (Object.prototype.hasOwnProperty.call(downloads.vision, card.id)) { return downloads.vision[card.id] === true; }
    return card.vision === true;
  }
  async function startDownload(card) {
    if (downloads.acting) { return; }
    downloads.errors[card.id] = "";
    downloads.acting = true;
    renderDownloads();
    var request = { id: card.id };
    if ((Number(card.vision_bytes) || 0) > 0) { request.vision = wantsVision(card); }
    var answer = await downloadsAnswer(api("/v1/strata/downloads/start", jsonPost(withFolder(request))));
    downloads.acting = false;
    if (answer.signedOut) { return; }
    if (answer.error) { downloads.errors[card.id] = answer.error; }
    renderDownloads();
    await refreshDownloads();
  }
  // Pause: the server stops the transfer and keeps the partial files.
  async function pauseDownload(card) {
    if (downloads.acting) { return; }
    downloads.errors[card.id] = "";
    downloads.acting = true;
    renderDownloads();
    var answer = await downloadsAnswer(api("/v1/strata/downloads/pause", jsonPost({ id: card.id })));
    downloads.acting = false;
    if (answer.signedOut) { return; }
    if (answer.error) { downloads.errors[card.id] = answer.error; } else { toast("info", "Download paused", card.name + ": Resume continues from where it stopped"); }
    renderDownloads();
    await refreshDownloads();
  }
  async function cancelDownload(card) {
    if (downloads.acting || !window.confirm("Cancel the download of " + card.name + "? The part downloaded so far is deleted.")) { return; }
    downloads.errors[card.id] = "";
    downloads.acting = true;
    renderDownloads();
    var answer = await downloadsAnswer(api("/v1/strata/downloads/cancel", jsonPost({ id: card.id })));
    downloads.acting = false;
    if (answer.signedOut) { return; }
    if (answer.error) { downloads.errors[card.id] = answer.error; } else { toast("info", "Download cancelled", card.name); }
    renderDownloads();
    await refreshDownloads();
  }

  // The pasted link (O3), pre-checked here (O4 is enforced by the server and the downloader).
  function linkProblem(url) {
    if (!url) { return "Paste the address of a .gguf file on Hugging Face."; }
    if (!HF_LINK.test(url)) { return "Only Hugging Face links can be downloaded: the address must be an https link on huggingface.co."; }
    if (!GGUF_FILE.test(url.split(/[?#]/)[0])) { return "Only .gguf files can be downloaded: the address must name a file ending in .gguf."; }
    return "";
  }
  function showLinkError(text) {
    $("downloads-link-error").textContent = text;
    $("downloads-link-error").hidden = !text;
  }
  function hideConfirm() {
    downloads.confirm = null;
    clear($("downloads-confirm"));
    $("downloads-confirm").hidden = true;
    renderDestination();   // the panel's warning counts a link being confirmed
  }
  // The families a pasted GGUF can be matched to (from the list's "profiles"): picking one has the
  // server write a runnable card after the download. The empty choice downloads without a card.
  function populateProfiles() {
    var sel = $("downloads-profile");
    if (!sel) { return; }
    var list = downloads.data && Array.isArray(downloads.data.profiles) ? downloads.data.profiles : [];
    var current = sel.value;
    clear(sel);
    var none = el("option", null, "Download only (no card made)");
    none.value = "";
    sel.appendChild(none);
    list.forEach(function (p) {
      if (!p || typeof p.key !== "string") { return; }
      var opt = el("option", null, String(p.display || p.key) + (p.runnable === false ? " (download only)" : ""));
      opt.value = p.key;
      sel.appendChild(opt);
    });
    sel.value = current;
    if (sel.selectedIndex < 0) { sel.selectedIndex = 0; }
  }
  // The links typed into the multipart part inputs (the shards still to paste), trimmed, non-empty.
  function gatherParts() {
    var box = $("downloads-parts");
    if (!box || box.hidden) { return []; }
    var inputs = box.querySelectorAll("input");
    var out = [];
    for (var i = 0; i < inputs.length; i++) { var v = inputs[i].value.trim(); if (v) { out.push(v); } }
    return out;
  }
  async function doCheck(extra) {
    var url = $("downloads-url").value.trim();
    var problem = linkProblem(url);
    if (problem) { hideConfirm(); showLinkError(problem); return; }
    showLinkError("");
    var profile = $("downloads-profile") ? $("downloads-profile").value : "";
    $("downloads-check").disabled = true;
    var body = { url: url };
    if (profile) { body.profile = profile; }
    if (extra && extra.length) { body.extra_urls = extra; }
    var answer = await downloadsAnswer(api("/v1/strata/downloads/link", jsonPost(withFolder(body))));
    $("downloads-check").disabled = false;
    if (answer.signedOut) { return; }
    if (answer.error) { showLinkError(answer.error); return; }
    var offer = answer.body && answer.body.confirm;
    if (!offer || typeof offer !== "object") { showLinkError("The server's answer did not describe the file."); return; }
    downloads.confirm = { url: url, profile: profile, data: offer };
    renderConfirm();
  }
  // A fresh Check clears any part inputs; "re-check" keeps and submits what has been pasted.
  function checkLink() { clear($("downloads-parts")); $("downloads-parts").hidden = true; hideConfirm(); doCheck([]); }
  function recheckLink() { var extra = gatherParts(); hideConfirm(); doCheck(extra); }
  // A split (multipart) GGUF: one input per shard still to paste (at most five more parts), then a
  // re-check that sends them all. The server pulls every shard to the one location.
  function renderParts(mp) {
    var box = $("downloads-parts");
    clear(box);
    var missing = Array.isArray(mp.missing) ? mp.missing.slice(0, 5) : [];
    box.appendChild(el("p", "st-label", "Paste the link to each remaining part (same model, same revision):"));
    missing.forEach(function (name) {
      var row = el("div", "downloads-link__row");
      var inp = el("input", "st-input");
      inp.type = "text";
      inp.spellcheck = false;
      inp.placeholder = String(name);
      inp.setAttribute("aria-label", "Link for " + String(name));
      row.appendChild(inp);
      box.appendChild(row);
    });
    if (Array.isArray(mp.missing) && mp.missing.length > 5) {
      box.appendChild(el("p", "msg-warn", "This model has more than six parts, which is more than can be added here."));
    }
    var recheck = el("button", "st-btn st-btn--secondary st-btn--sm", "Add parts and re-check");
    recheck.type = "button";
    recheck.addEventListener("click", recheckLink);
    box.appendChild(recheck);
    box.hidden = false;
  }
  // What the link brings, before anything is fetched: the repository, the commit, each file with
  // its size, the total and whether it fits; Download starts it.
  function renderConfirm() {
    var box = $("downloads-confirm");
    clear(box);
    var offer = downloads.confirm && downloads.confirm.data;
    if (!offer) { box.hidden = true; return; }
    box.appendChild(el("span", "dl-card__name", offer.name ? String(offer.name) : "This file"));
    var rows = el("dl", "facts");
    var row = function (label, value, title) {
      rows.appendChild(el("dt", null, label));
      var dd = el("dd");
      var code = el("code", null, value);
      if (title) { code.title = title; }
      dd.appendChild(code);
      rows.appendChild(dd);
    };
    row("Repository", offer.repo ? String(offer.repo) : "-");
    var commit = typeof offer.commit === "string" ? offer.commit : "";
    row("Commit", commit ? commit.slice(0, 7) : "-", commit);
    box.appendChild(rows);
    var files = Array.isArray(offer.files) ? offer.files.filter(function (file) { return file && typeof file.path === "string"; }) : [];
    var list = el("ul", "dl-confirm__files");
    files.forEach(function (file) { list.appendChild(el("li", null, file.path + DOT + dgb(file.size) + " GB")); });
    box.appendChild(list);
    box.appendChild(el("p", "dl-card__facts", "Total " + dgb(offer.size_bytes) + " GB" + DOT + files.length + (files.length === 1 ? " file" : " files")));
    var fits = offer.fits !== false;
    var free = freeBytes();
    box.appendChild(fits ? el("p", "muted small", "Fits: " + dgb(offer.needed_bytes) + " GB needed" + (free === null ? "" : ", " + dgb(free) + " GB free"))
                         : el("p", "msg-warn", noRoomText(offer.needed_bytes)));
    // A split (multipart) GGUF: say how many parts are pasted, and gather the rest when incomplete.
    var mp = offer.multipart && typeof offer.multipart === "object" ? offer.multipart : null;
    var complete = !mp || mp.complete !== false;
    if (mp && Number(mp.parts) > 1) {
      box.appendChild(el("p", "muted small", "Split model: " + (Number(mp.have) || 0) + " of " + Number(mp.parts) + " parts pasted."));
    }
    if (mp && Number(mp.parts) > 1 && !complete) { renderParts(mp); } else { $("downloads-parts").hidden = true; clear($("downloads-parts")); }
    // The matched family (its card is made after the download), or a plain download when none chosen.
    if (offer.family && offer.family !== "custom") {
      box.appendChild(el("p", "muted small", offer.runnable === false
        ? "Runs as " + String(offer.family) + " - download only for now (this server names no engine for it)."
        : "Runs as " + String(offer.family) + " - a card is made in Models after the download."));
    } else {
      box.appendChild(el("p", "muted small", "No family chosen: the files download but no card is made. Pick one above to make it runnable."));
    }
    var actions = el("div", "dl-card__actions");
    var go = el("button", "st-btn st-btn--primary st-btn--sm", "Download");
    go.type = "button";
    go.disabled = !fits || !complete || downloads.acting;
    go.addEventListener("click", startLink);
    actions.appendChild(go);
    var later = el("button", "st-btn st-btn--secondary st-btn--sm", "Not now");
    later.type = "button";
    later.addEventListener("click", hideConfirm);
    actions.appendChild(later);
    box.appendChild(actions);
    box.hidden = false;
    renderDestination();
  }
  async function startLink() {
    var offer = downloads.confirm;
    if (!offer || downloads.acting) { return; }
    showLinkError("");
    downloads.acting = true;
    renderConfirm();
    var answer = await downloadsAnswer(api("/v1/strata/downloads/start", jsonPost(withFolder({ url: offer.url, confirm: true }))));
    downloads.acting = false;
    if (answer.signedOut) { return; }
    if (answer.error) { renderConfirm(); showLinkError(answer.error); return; }
    hideConfirm();
    $("downloads-url").value = "";
    // its group opens, wherever it is listed
    downloads.justAdded = answer.body && typeof answer.body.id === "string" ? answer.body.id : "";
    toast("success", "Download queued", offer.data.name ? String(offer.data.name) : "");
    await refreshDownloads();
  }

  // ---------------------------------------------------------------- Folders and cards on this PC (models.control)
  // GUI-first start-up (spec 2026-10-27 section 4): the server's folder browser (POST /v1/strata/folders: this PC's drives,
  // one folder's sub-folders and .gguf files), a card made from a model file already on this PC (cards/add), a card's
  // folder changed after its files moved (cards/folder), a model folder made private, a new private folder for
  // downloads, and the Danger zone's delete. The server answers these for its own machine only and checks every path
  // and every step itself; the page shows paths as text.
  var picker = { resolve: null, mode: "folder", listing: null, volumes: null };

  function showPickerError(text) { $("folder-picker-error").textContent = text; $("folder-picker-error").hidden = !text; }
  async function pickerOpen(path) {
    showPickerError("");
    var answer = await downloadsAnswer(api("/v1/strata/folders", jsonPost(path ? { path: path } : {})));
    if (answer.signedOut) { closePicker(null); return; }
    if (answer.error) { showPickerError(answer.error); return; }
    var body = answer.body || {};
    picker.volumes = Array.isArray(body.volumes) ? body.volumes : null;
    picker.listing = picker.volumes ? null : body;
    renderPicker();
  }
  function pickerRow(label, detail, onClick) {
    var row = el("button", "folder-row");
    row.type = "button";
    row.appendChild(el("span", "folder-row__name", label));
    if (detail) { row.appendChild(el("span", "muted small", detail)); }
    if (onClick) { row.addEventListener("click", onClick); } else { row.disabled = true; }
    return row;
  }
  function pathJoin(base, name) {
    var sep = /^[A-Za-z]:/.test(base) || base.indexOf("\\") >= 0 ? "\\" : "/";
    return base.charAt(base.length - 1) === sep ? base + name : base + sep + name;
  }
  function renderPicker() {
    var list = $("folder-picker-list");
    clear(list);
    var listing = picker.listing;
    var folderMode = picker.mode === "folder";
    $("folder-picker-path").textContent = listing ? String(listing.path) : "This PC";
    $("folder-picker-up").disabled = !listing;
    $("folder-picker-free").textContent = listing && typeof listing.free_bytes === "number" ? "Free space here: " + dgb(listing.free_bytes) + " GB" : "";
    $("folder-picker-use").hidden = !folderMode;
    $("folder-picker-use").disabled = !listing;
    $("folder-picker-new").hidden = !folderMode || !listing;
    if (!listing) {
      (picker.volumes || []).forEach(function (v) {
        if (!v || typeof v.path !== "string") { return; }
        list.appendChild(pickerRow(v.path + (v.label ? " (" + v.label + ")" : ""), dgb(v.free_bytes) + " GB free of " + dgb(v.total_bytes) + " GB",
                                   function () { pickerOpen(v.path); }));
      });
      if (!list.firstChild) { list.appendChild(el("p", "muted small", "No local drive was found.")); }
      return;
    }
    var entries = Array.isArray(listing.entries) ? listing.entries.filter(function (e) { return e && typeof e.name === "string"; }) : [];
    entries.forEach(function (entry) {
      if (entry.folder) {
        list.appendChild(pickerRow(entry.name, "folder", function () { pickerOpen(pathJoin(String(listing.path), entry.name)); }));
      } else {
        // A model file: picked in file mode; shown (not picked) when a folder is wanted.
        list.appendChild(pickerRow(entry.name, dgb(entry.bytes) + " GB",
                                   folderMode ? null : function () { closePicker({ folder: String(listing.path), file: entry.name }); }));
      }
    });
    if (listing.truncated) { list.appendChild(el("p", "muted small", "Only the first 1,000 entries are shown.")); }
    if (!entries.length) { list.appendChild(el("p", "muted small", folderMode ? "No folders here." : "No folders or .gguf files here.")); }
  }
  function pickerUp() {
    if (picker.listing) { pickerOpen(picker.listing.parent ? String(picker.listing.parent) : ""); }
  }
  function closePicker(result) {
    if ($("folder-picker").open) { $("folder-picker").close(); }
    var resolve = picker.resolve;
    picker.resolve = null;
    if (resolve) { resolve(result); }
  }
  // Resolves { folder, free } (folder mode: "Use this folder"), { folder, file } (file mode: a .gguf picked) or null.
  function pickFolder(title, help, mode, start) {
    if (picker.resolve) { closePicker(null); }
    picker.mode = mode;
    picker.listing = null;
    picker.volumes = null;
    $("folder-picker-title").textContent = title;
    $("folder-picker-help").textContent = help;
    $("folder-picker-typed").value = "";
    $("folder-picker-new-name").value = "";
    clear($("folder-picker-list"));
    showPickerError("");
    var promise = new Promise(function (resolve) { picker.resolve = resolve; });
    $("folder-picker").showModal();
    pickerOpen(start || "");
    return promise;
  }
  async function pickerNewFolder() {
    var listing = picker.listing;
    var name = $("folder-picker-new-name").value.trim();
    if (!listing || !name) { showPickerError("Type a name for the new folder."); return; }
    var answer = await downloadsAnswer(api("/v1/strata/folders/new", jsonPost({ parent: String(listing.path), name: name })));
    if (answer.signedOut) { closePicker(null); return; }
    if (answer.error) { showPickerError(answer.error); return; }
    $("folder-picker-new-name").value = "";
    if (answer.body && typeof answer.body.path === "string") { pickerOpen(answer.body.path); }
  }

  // A folder other users of this PC may change (409 folder_not_private): offered to be made private, then tried again.
  async function offerPrivate(folder) {
    var question = "Other users of this PC can change files in this folder:\n" + folder + "\n\nMake it private, so only you, " +
                   "the system and administrators can change it? Only a folder that holds just model files can be made private." +
                   "\n\n(To use folders like this one as they are, turn on \"Use model folders as they are\" in Settings.)";
    if (!window.confirm(question)) { return false; }
    var made = await downloadsAnswer(api("/v1/strata/folders/private", jsonPost({ path: folder })));
    if (made.error) { toast("error", "The folder was not made private", made.error); return false; }
    return !made.signedOut;
  }
  function showLocalError(text) { $("local-models-error").textContent = text; $("local-models-error").hidden = !text; }
  async function addCard(folder, file) {
    // owner 2026-10-27: a Qwen3.8, GLM-5.3, DeepSeek, MiniMax or raon1 model has its pack built first (the answer comes when
    // it is done)
    toast("info", "Adding " + file, "A Qwen3.8, GLM-5.3, DeepSeek, MiniMax or raon1 model has its pack built first: up to a few minutes.");
    var answer = await downloadsAnswer(api("/v1/strata/cards/add", jsonPost({ folder: folder, file: file })));
    if (answer.signedOut) { return; }
    if (answer.code === "folder_not_private") {
      if (await offerPrivate(folder)) { await addCard(folder, file); }
      return;
    }
    if (answer.error) { showLocalError(answer.error); return; }
    var name = answer.body && typeof answer.body.name === "string" ? answer.body.name : file;
    toast("success", name + " was added", "Its card is in Start a model.");
    launcher.selected = name;
    await refreshLauncher();
    if (downloads.enabled) { await refreshDownloads(); }
    showModelsSubtab("start");
    // a model that drafts with a draft layer of its own, none found beside it: point to it now
    var draft = answer.body && answer.body.draft;
    if (draft && draft.kind === "inside") {
      toast("info", name + " added", draft.in_file === true ? "MTP: its draft layer is in the model file, and the card drafts with it."
                                                       : "MTP: this model file has no draft layer, so the card runs without one.");
    }
    if (draft && draft.kind !== "inside" && draft.path === null && window.confirm(name + " can guess tokens ahead with an MTP draft layer, which makes its " +
                                                       "answers faster, but none was found beside it.\n\nPoint to its draft layer now?")) {
      await pointDraft(findCard(name));
    }
  }
  async function addLocalModel() {
    showLocalError("");
    var picked = await pickFolder("Choose a model file", "Open the folder that holds the model and pick its .gguf file (a split " +
                                  "model: its first part). The files stay where they are.", "file");
    if (picked && picked.file) { await addCard(picked.folder, picked.file); }
  }
  async function repointCard(card, folder) {
    var answer = await downloadsAnswer(api("/v1/strata/cards/folder", jsonPost({ name: card.name, folder: folder })));
    if (answer.signedOut) { return; }
    if (answer.code === "folder_not_private") {
      if (await offerPrivate(folder)) { await repointCard(card, folder); }
      return;
    }
    if (answer.error) { showStartError(answer.error); return; }
    toast("success", card.name + " reads its files from the new folder",
          card.state === "running" ? "The running model keeps going; the new folder applies at its next start." : "");
    await refreshLauncher();
    if (downloads.enabled) { await refreshDownloads(); }   // the Download list says again what this PC has
  }
  async function changeCardFolder() {
    var card = findCard(launcher.selected);
    if (!card || card.page_card !== true) { return; }
    showStartError("");
    var picked = await pickFolder("Move " + card.name, "Pick the folder that holds " + (card.file ? card.file + ".gguf" : "the model's file") +
                                  " now.", "folder");
    if (picked && picked.folder) { await repointCard(card, picked.folder); }
  }

  // Owner 2026-10-29 ("when browsing for models, if it supports mtp we should be able to point where the mtp is at too"):
  // a card whose model drafts with a draft layer of its own (card.draft: a Qwen3.8 pack's upstream mtp\rt folder, a
  // Gemma 4 model's mtp-*.gguf) shows where it is and is pointed at one the user picks; a model added with none found
  // offers that at once.
  async function pointDraft(card) {
    if (!card || !card.draft) { return; }
    showStartError("");
    var folder = card.draft.kind === "folder";
    var picked = await pickFolder("MTP draft layer for " + card.name,
                                  folder ? "Open the draft layer's folder (upstream's mtp\\rt, which holds dense.bin, experts.bin and " +
                                           "draft_vocab.bin) or a folder holding it, then use it."
                                         : "Open the folder that holds the model's MTP draft file and pick it (its name starts with mtp-).",
                                  folder ? "folder" : "file");
    if (picked && picked.folder) { await sendDraft(card, picked.folder, folder ? null : picked.file); }
  }
  async function sendDraft(card, folder, file) {
    var body = { name: card.name, folder: folder };
    if (file) { body.file = file; }
    var answer = await downloadsAnswer(api("/v1/strata/cards/mtp", jsonPost(body)));
    if (answer.signedOut) { return; }
    if (answer.code === "folder_not_private") {
      if (await offerPrivate(folder)) { await sendDraft(card, folder, file); }
      return;
    }
    if (answer.error) { showStartError(answer.error); return; }
    toast("success", card.name + " drafts with its MTP layer", card.state === "running" ? "It is used from the model's next start." : "");
    await refreshLauncher();
  }

  // Owner 2026-10-28 ("an ability to remove an active model card but not delete files"): the card goes off the list, its
  // model files stay; a running model is stopped first (the server removes the card once it has stopped).
  async function removeCard() {
    var card = findCard(launcher.selected);
    if (!card || card.page_card !== true) { return; }
    showStartError("");
    var runs = launcher.data && launcher.data.running === card.name;
    var question = "Remove the card " + card.name + "? Its model files stay on this PC; add it again from its folder." +
      (runs ? " The running model is stopped first." : "");
    if (!window.confirm(question)) { return; }
    launcher.acting = true;
    renderDetail(card, false);
    var answer = await downloadsAnswer(api("/v1/strata/cards/remove", jsonPost({ name: card.name })));
    launcher.acting = false;
    if (answer.signedOut) { return; }
    if (answer.error) { showStartError(answer.error); await refreshLauncher(); return; }
    var stopping = answer.body && answer.body.stopping === true;
    toast("success", stopping ? card.name + " is stopping; its card is removed when it has stopped" : card.name + "'s card was removed",
          "Its model files stay on this PC.");
    launcher.selected = "";
    if (stopping) {
      // the server stops it, then removes the card: follow it as a stop
      stopLauncherPoll();
      await pollLauncher();
    } else {
      await refreshLauncher();
    }
    if (downloads.enabled) { await refreshDownloads(); }   // the Download list says again what this PC has
  }

  // The Danger zone (spec section 4.5): a fresh code from the server, typed back; the API key typed again (the key this tab
  // holds is not reused); a last warning that it is permanent. The server checks all three, then deletes only the
  // model's files (and its card).
  var removal = { name: "", code: "", key: "", step: "", folder: "" };
  function deleteStep(step) {
    removal.step = step;
    $("delete-step-code").hidden = step !== "code";
    $("delete-step-key").hidden = step !== "key";
    $("delete-step-final").hidden = step !== "final";
    $("delete-next").textContent = step === "final" ? "Delete permanently" : step === "done" ? "Close" : "Next";
    $("delete-next").className = "st-btn " + (step === "final" ? "st-btn--danger" : "st-btn--primary");
  }
  function showDeleteError(text) { $("delete-error").textContent = text; $("delete-error").hidden = !text; }
  function closeDelete() {
    removal = { name: "", code: "", key: "", step: "", folder: "" };
    $("delete-code").textContent = "";
    $("delete-code-input").value = "";
    $("delete-key-input").value = "";
    if ($("delete-dialog").open) { $("delete-dialog").close(); }
  }
  async function beginDelete() {
    var card = findCard(launcher.selected);
    if (!card || card.page_card !== true) { return; }
    showStartError("");
    var answer = await downloadsAnswer(api("/v1/strata/cards/delete/begin", jsonPost({ name: card.name })));
    if (answer.signedOut) { return; }
    if (answer.error) { showStartError(answer.error); return; }
    var code = answer.body && typeof answer.body.code === "string" ? answer.body.code : "";
    if (!code) { showStartError("The server gave no code."); return; }
    removal = { name: card.name, code: code, key: "", step: "code", folder: typeof card.folder === "string" ? card.folder : "" };
    $("delete-title").textContent = "Delete " + card.name;
    $("delete-code").textContent = code;
    $("delete-code-input").value = "";
    $("delete-key-input").value = "";
    showDeleteError("");
    deleteStep("code");
    $("delete-dialog").showModal();
    $("delete-code-input").focus();
  }
  async function deleteNext() {
    showDeleteError("");
    if (removal.step === "done") { closeDelete(); return; }
    if (removal.step === "code") {
      if ($("delete-code-input").value.trim().toUpperCase() !== removal.code) { showDeleteError("That is not the code shown above."); return; }
      deleteStep("key");
      $("delete-key-input").focus();
      return;
    }
    if (removal.step === "key") {
      removal.key = $("delete-key-input").value;
      $("delete-key-input").value = "";
      if (!removal.key) { showDeleteError("Enter the API key."); return; }
      $("delete-final-text").textContent = "Deleting " + removal.name + " removes its model files" +
        (removal.folder ? " from " + removal.folder : "") + " and its card. This is permanent and cannot be undone.";
      deleteStep("final");
      return;
    }
    if (removal.step !== "final") { return; }
    $("delete-next").disabled = true;
    var answer = await downloadsAnswer(api("/v1/strata/cards/delete", jsonPost({ name: removal.name, code: removal.code, key: removal.key, permanent: true })));
    $("delete-next").disabled = false;
    removal.key = "";
    if (answer.signedOut) { closeDelete(); return; }
    if (answer.error) {
      // the code is spent by any try; delete_refused already says to ask for a new one
      showDeleteError(answer.code === "delete_refused" ? answer.error : answer.error + " Start again for a new code.");
      deleteStep("done");
      return;
    }
    var done = answer.body || {};
    toast("success", removal.name + " was deleted", fmt(Number(done.files) || 0) + (done.files === 1 ? " file" : " files") + DOT +
          dgb(done.bytes) + " GB freed");
    if (done.download_removed === false) {
      // a downloaded model: its files are gone, but its download store still lists it (the server log says why)
      toast("error", "Its download record stayed", "The Download list may still show it as downloaded until the store is cleaned.");
    }
    closeDelete();
    launcher.selected = "";
    await refreshLauncher();
    if (downloads.enabled) { await refreshDownloads(); }
  }

  // ---------------------------------------------------------------- Settings: email alerts
  // GUI-first start-up (spec section 5a): the server's email alerts, in a build that has them (its alerts route answers;
  // a build without them answers 404 and the Settings sub-tab stays hidden). The settings are read from and saved to
  // the server's config file and apply when it restarts; the password is saved on its own and never shown again.
  var alerts = { built: false, data: null };
  function showAlertsError(text) { $("alerts-error").textContent = text; $("alerts-error").hidden = !text; }
  function alertEventBoxes() { return Array.prototype.slice.call($("alerts-events").querySelectorAll("input[data-event]")); }
  function fillAlerts(d) {
    $("alerts-enabled").checked = d.enabled === true;
    $("alerts-host").value = typeof d.host === "string" ? d.host : "";
    $("alerts-port").value = typeof d.port === "number" ? String(d.port) : "465";
    $("alerts-security").value = d.security === "starttls" ? "starttls" : "ssl";
    $("alerts-username").value = typeof d.username === "string" ? d.username : "";
    $("alerts-from").value = typeof d.from === "string" ? d.from : "";
    $("alerts-to").value = Array.isArray(d.to) ? d.to.join(", ") : "";
    var events = Array.isArray(d.events) ? d.events : [];
    alertEventBoxes().forEach(function (box) { box.checked = events.indexOf(box.getAttribute("data-event")) >= 0; });
    $("alerts-password-state").textContent = d.password_set === true ? "A password is saved (it is never shown)." : "No password is saved yet.";
    var editable = d.editable !== false;
    ["alerts-save", "alerts-enabled"].forEach(function (id) { $(id).disabled = !editable; });
    if (!editable) { $("alerts-result").textContent = "This server was started without a config file it can write; edit the file by hand."; }
  }
  async function refreshAlerts() {
    var response, text;
    try { response = await api("/v1/strata/alerts", jsonPost({})); text = await response.text(); } catch (e) { return; }
    alerts.built = response.ok;
    $("alerts-settings").hidden = !alerts.built;
    showSettingsSubtab();
    if (!alerts.built) { return; }
    try { alerts.data = JSON.parse(text); } catch (e) { alerts.data = null; }
    if (alerts.data) { fillAlerts(alerts.data); }
  }
  function alertsForm() {
    var to = $("alerts-to").value.split(",").map(function (t) { return t.trim(); }).filter(function (t) { return t; });
    return {
      enabled: $("alerts-enabled").checked, host: $("alerts-host").value.trim(), port: Number($("alerts-port").value) || 465,
      security: $("alerts-security").value, username: $("alerts-username").value.trim(), from: $("alerts-from").value.trim(), to: to,
      events: alertEventBoxes().filter(function (box) { return box.checked; }).map(function (box) { return box.getAttribute("data-event"); })
    };
  }
  // IONOS: smtp.ionos.com, SSL/TLS on 465, the full mailbox address as the user name and From.
  function fillIonos() {
    $("alerts-host").value = "smtp.ionos.com";
    $("alerts-port").value = "465";
    $("alerts-security").value = "ssl";
    var mailbox = $("alerts-username").value.trim() || $("alerts-from").value.trim();
    if (mailbox) { $("alerts-username").value = mailbox; $("alerts-from").value = mailbox; }
    $("alerts-result").textContent = "IONOS filled in: use the mailbox's full address as the user name and its password." +
      " Where port 465 is blocked, pick STARTTLS (587).";
  }
  async function saveAlerts() {
    showAlertsError("");
    var answer = await downloadsAnswer(api("/v1/strata/alerts/settings", jsonPost(alertsForm())));
    if (answer.signedOut) { return; }
    if (answer.error) { showAlertsError(answer.error); return; }
    $("alerts-result").textContent = "Saved. They apply when the server restarts.";
    await refreshAlerts();
  }
  async function saveAlertsPassword() {
    showAlertsError("");
    var password = $("alerts-password").value;
    $("alerts-password").value = "";
    if (!password) { showAlertsError("Type the mailbox's password first."); return; }
    var answer = await downloadsAnswer(api("/v1/strata/alerts/password", jsonPost({ password: password })));
    password = "";
    if (answer.signedOut) { return; }
    if (answer.error) { showAlertsError(answer.error); return; }
    $("alerts-result").textContent = "Password saved.";
    await refreshAlerts();
  }
  async function testAlerts() {
    showAlertsError("");
    $("alerts-result").textContent = "Sending a test email through the saved settings...";
    $("alerts-test").disabled = true;
    var answer = await downloadsAnswer(api("/v1/strata/alerts/test", jsonPost({})));
    $("alerts-test").disabled = false;
    if (answer.signedOut) { return; }
    if (answer.error) { $("alerts-result").textContent = ""; showAlertsError(answer.error); return; }
    var r = answer.body || {};
    $("alerts-result").textContent = r.sent === true ? "Sent: check the inbox." : "";
    if (r.sent !== true) {
      showAlertsError("Not sent (" + String(r.stage || "-") + (r.code ? ", the server said " + r.code : "") + "): " + String(r.reply || ""));
    }
  }

  // The Settings tab holds the API key, the server's own settings, the model folders, the email alerts and the web search:
  // shown when the server has any of them (spec 2026-10-28-server-window-design, Part 1).
  function showSettingsSubtab() {
    var any = alerts.built || search.built || folders.built || serverConfig.built;
    $("tab-btn-settings").hidden = !any;
  }

  // ---------------------------------------------------------------- Settings: the API key
  // The key this page signed in with, shown on request and copied (no route hands a key out); rotation makes a new one in
  // the server's key store (typed ROTATE first), shown once, working after the server restarts.
  var keyShown = false;
  function renderKey() {
    var shown = keyShown && !state.keyless && !!state.key;
    $("key-value").textContent = shown ? state.key : (state.keyless ? "not needed (a loopback server without a key)" : "\u2022".repeat(16));
    $("key-show").textContent = keyShown ? "Hide" : "Show";
    $("key-show").disabled = state.keyless || !state.key;
    $("key-copy").disabled = state.keyless || !state.key;
    var rotatable = !!(serverConfig.data && serverConfig.data.key && serverConfig.data.key.rotatable === true);
    $("key-rotate").disabled = !rotatable;
    $("key-rotate-confirm").disabled = !rotatable;
    $("key-state").textContent = rotatable ? "" : "This server's key comes from an environment variable (a development start): it is not rotated here.";
  }
  function showKeyError(text) { $("key-error").textContent = text || ""; $("key-error").hidden = !text; }
  async function rotateKey() {
    showKeyError("");
    if ($("key-rotate-confirm").value.trim() !== "ROTATE") { showKeyError("Type ROTATE to confirm."); return; }
    $("key-rotate").disabled = true;
    var answer = await downloadsAnswer(api("/v1/strata/key/rotate", jsonPost({ confirm: "ROTATE" })));
    $("key-rotate").disabled = false;
    if (answer.signedOut) { return; }
    if (answer.error) { showKeyError(answer.error); return; }
    var r = answer.body || {};
    $("key-rotate-confirm").value = "";
    $("key-new-value").textContent = String(r.key || "");
    $("key-new").hidden = !r.key;
    $("settings-restart-note").hidden = false;
  }

  // ---------------------------------------------------------------- Settings: the server's own settings
  // POST /v1/strata/config lists each setting with its type, the config file's value (null: its default), the default, the
  // bounds and the schema's own words; each group's Save sends its values (null: back to the default). The server checks
  // them as it checks its config at start, and they apply when it restarts.
  var serverConfig = { built: false, data: null };
  var CONFIG_GROUPS = {
    server: ["listener.host", "listener.port", "listener.allow_remote", "listener.allow_wildcard_bind", "listener.host_aliases",
             "models.control_remote", "models.control_remote_networks"],
    limits: ["limits.engine.generation_timeout_s", "limits.engine.ready_timeout_s", "limits.chat.max_prompt_tokens",
             "limits.chat.max_completion_tokens", "pull.connections"],
    paths: ["models.downloads_root", "models.default_root"],
    logs: ["logging.engine_output"]
  };
  var CONFIG_FOLDERS = ["models.downloads_root", "models.default_root"];
  var CONFIG_LABELS = {
    "listener.host": "Listen on (address)",
    "listener.port": "Port",
    "listener.allow_remote": "Serve other PCs on the network",
    "listener.allow_wildcard_bind": "Allow listening on every address (0.0.0.0)",
    "listener.host_aliases": "Names this server answers to (one per line)",
    "models.control_remote": "Let the PCs listed below manage models",
    "models.control_remote_networks": "PCs or subnets that may manage models (one per line)",
    "limits.engine.generation_timeout_s": "Longest answer (seconds)",
    "limits.engine.ready_timeout_s": "Longest model start (seconds)",
    "limits.chat.max_prompt_tokens": "Largest prompt (tokens)",
    "limits.chat.max_completion_tokens": "Largest answer (tokens)",
    "pull.connections": "Connections per download",
    "models.downloads_root": "Downloads folder",
    "models.default_root": "Folder Add a model opens",
    "logging.engine_output": "Write the engine's own output to the server log"
  };
  function configSetting(key) {
    var list = serverConfig.data && Array.isArray(serverConfig.data.settings) ? serverConfig.data.settings : [];
    for (var i = 0; i < list.length; i += 1) { if (list[i] && list[i].key === key) { return list[i]; } }
    return null;
  }
  function configField(setting) {
    var field = el("div", "config-field");
    var id = "cfg-" + setting.key.replace(/\./g, "-");
    var label = CONFIG_LABELS[setting.key] || setting.key;
    var input;
    if (setting.type === "boolean") {
      var check = el("label", "option-auto");
      input = el("input");
      input.type = "checkbox";
      input.id = id;
      input.checked = setting.value === null ? setting["default"] === true : setting.value === true;
      check.appendChild(input);
      check.appendChild(document.createTextNode(label));
      field.appendChild(check);
    } else {
      field.appendChild(el("label", "st-label", label)).htmlFor = id;
      if (setting.type === "string array") {
        input = el("textarea", "st-input config-list");
        input.rows = 3;
        input.value = Array.isArray(setting.value) ? setting.value.join("\n") : "";
        input.placeholder = "One per line";
      } else {
        input = el("input", "st-input");
        input.type = setting.type === "integer" ? "number" : "text";
        if (setting.type === "integer") {
          input.step = "1";
          if (typeof setting.min === "number") { input.min = String(setting.min); }
          if (typeof setting.max === "number") { input.max = String(setting.max); }
        }
        input.value = setting.value === null || setting.value === undefined ? "" : String(setting.value);
        input.placeholder = setting["default"] === null || setting["default"] === undefined ? "Not set" : "Default: " + String(setting["default"]);
        input.spellcheck = false;
        input.autocapitalize = "off";
      }
      input.id = id;
      if (CONFIG_FOLDERS.indexOf(setting.key) >= 0) {
        var row = el("div", "config-folder");
        row.appendChild(input);
        var choose = el("button", "st-btn st-btn--secondary st-btn--sm", "Choose");
        choose.type = "button";
        choose.addEventListener("click", async function () {
          var picked = await pickFolder(label, "Pick a drive and a folder, or make a new private folder there. Then Save; the server " +
                                        "uses it after it restarts.", "folder", input.value.trim());
          if (picked && picked.folder) { input.value = String(picked.folder); }
        });
        row.appendChild(choose);
        field.appendChild(row);
      } else {
        field.appendChild(input);
      }
    }
    if (setting.help) { field.appendChild(el("span", "muted small", String(setting.help))); }
    return { node: field, input: input, setting: setting };
  }
  var configFields = Object.create(null);
  function renderServerConfig() {
    Object.keys(CONFIG_GROUPS).forEach(function (group) {
      var grid = document.querySelector('.config-grid[data-group="' + group + '"]');
      if (!grid) { return; }
      clear(grid);
      configFields[group] = [];
      CONFIG_GROUPS[group].forEach(function (key) {
        var setting = configSetting(key);
        if (!setting) { return; }
        var made = configField(setting);
        grid.appendChild(made.node);
        configFields[group].push(made);
      });
    });
    var editable = !!(serverConfig.data && serverConfig.data.editable);
    document.querySelectorAll("[data-save]").forEach(function (button) { button.disabled = !editable; });
  }
  async function refreshServerConfig() {
    var response, text;
    try { response = await api("/v1/strata/config", jsonPost({})); text = await response.text(); } catch (e) { return; }
    if (!response.ok) {
      serverConfig.built = false;
      ["key-settings", "server-settings", "limits-settings", "paths-settings", "logs-settings"].forEach(function (id) { $(id).hidden = true; });
      showSettingsSubtab();
      return;
    }
    try { serverConfig.data = JSON.parse(text); } catch (e) { return; }
    serverConfig.built = true;
    ["key-settings", "server-settings", "limits-settings", "paths-settings"].forEach(function (id) { $(id).hidden = false; });
    // Troubleshooting only in a make --debugging build (owner 2026-10-29), as the server says
    $("logs-settings").hidden = serverConfig.data.debugging !== true;
    renderServerConfig();
    renderKey();
    showSettingsSubtab();
  }
  function configErrorBox(group) { return document.querySelector('[data-error="' + group + '"]'); }
  async function saveServerConfig(group) {
    var box = configErrorBox(group);
    box.hidden = true;
    var values = {};
    var problem = "";
    (configFields[group] || []).forEach(function (f) {
      var key = f.setting.key;
      if (f.setting.type === "boolean") {
        // a box left at its default stays out of the file
        values[key] = f.setting.value === null && f.input.checked === (f.setting["default"] === true) ? null : f.input.checked;
        return;
      }
      if (f.setting.type === "string array") {
        var lines = f.input.value.split("\n").map(function (line) { return line.trim(); }).filter(function (line) { return line; });
        values[key] = lines.length ? lines : null;
        return;
      }
      var raw = f.input.value.trim();
      if (!raw) { values[key] = null; return; }
      if (f.setting.type === "integer") {
        var n = Number(raw);
        if (!isFinite(n) || Math.floor(n) !== n) { problem = problem || (CONFIG_LABELS[key] || key) + ": enter a whole number"; return; }
        values[key] = n;
        return;
      }
      values[key] = raw;
    });
    if (problem) { box.textContent = problem; box.hidden = false; return; }
    var button = document.querySelector('[data-save="' + group + '"]');
    button.disabled = true;
    var answer = await downloadsAnswer(api("/v1/strata/config/settings", jsonPost({ values: values })));
    button.disabled = false;
    if (answer.signedOut) { return; }
    if (answer.error) { box.textContent = answer.error; box.hidden = false; return; }
    $("settings-restart-note").hidden = false;
    await refreshServerConfig();
  }

  // ---------------------------------------------------------------- Settings: model folders
  // Owner 2026-10-27: the server may use model folders as they are, other users' access included, and never change their
  // permissions (roots.allow_untrusted_media). Read from and saved to the config file; it applies when the server
  // restarts. Its route answers the clients the model-file routes serve (this PC, and the listed hosts and subnets), so
  // the section shows wherever the "make it private" dialog can name it; elsewhere it stays hidden.
  var folders = { built: false, data: null };
  function showFoldersError(text) { $("folders-error").textContent = text; $("folders-error").hidden = !text; }
  function fillFolders(d) {
    $("folders-shared").checked = d.shared === true;
    // owner 2026-10-29 (experimental): model folders on network shares
    $("folders-network").checked = d.network === true;
    $("folders-network-state").textContent = d.network_running === true ? "This server takes model folders on network shares." :
      "This server takes model folders on this PC's own drives only.";
    $("folders-state").textContent = d.running === true ? "This server uses model folders as they are." :
      "This server uses only model folders other users of this PC cannot change.";
    var editable = d.editable !== false;
    ["folders-save", "folders-shared", "folders-network"].forEach(function (id) { $(id).disabled = !editable; });
    if (!editable) { $("folders-result").textContent = "This server was started without a config file it can write; edit the file by hand."; }
  }
  async function refreshFolders() {
    var response, text;
    try { response = await api("/v1/strata/folders/sharing", jsonPost({})); text = await response.text(); } catch (e) { return; }
    folders.built = response.ok;
    $("folders-settings").hidden = !folders.built;
    showSettingsSubtab();
    if (!folders.built) { return; }
    try { folders.data = JSON.parse(text); } catch (e) { folders.data = null; }
    if (folders.data) { fillFolders(folders.data); }
  }
  async function saveFolders() {
    showFoldersError("");
    var answer = await downloadsAnswer(api("/v1/strata/folders/sharing", jsonPost({ shared: $("folders-shared").checked, network: $("folders-network").checked })));
    if (answer.signedOut) { return; }
    if (answer.error) { showFoldersError(answer.error); return; }
    $("folders-result").textContent = "Saved. It applies when the server restarts.";
    await refreshFolders();
  }

  // ---------------------------------------------------------------- Settings: web search
  // Web search (spec 2026-10-27-web-search-design section 2): the server's web search through a SearXNG instance, in a
  // build that has it (its search route answers; a build without it answers 404 and the section stays hidden). The
  // settings are read from and saved to the server's config file and apply when it restarts; a test search runs one
  // search through the saved settings and shows what a model would read.
  var search = { built: false, data: null };
  function showSearchError(text) { $("search-error").textContent = text; $("search-error").hidden = !text; }
  function fillSearch(d) {
    $("search-enabled").checked = d.enabled === true;
    $("search-url").value = typeof d.url === "string" ? d.url : "";
    $("search-results").value = typeof d.results === "number" ? String(d.results) : "5";
    $("search-rounds").value = typeof d.max_rounds === "number" ? String(d.max_rounds) : "8";
    $("search-timeout").value = typeof d.timeout_s === "number" ? String(d.timeout_s) : "30";
    $("search-state").textContent = d.running === true ? "Web search is on for this server's chats." :
      "Web search is off for this server's chats.";
    var editable = d.editable !== false;
    ["search-save", "search-enabled"].forEach(function (id) { $(id).disabled = !editable; });
    if (!editable) { $("search-result").textContent = "This server was started without a config file it can write; edit the file by hand."; }
  }
  async function refreshSearch() {
    var response, text;
    try { response = await api("/v1/strata/search", jsonPost({})); text = await response.text(); } catch (e) { return; }
    search.built = response.ok;
    $("search-settings").hidden = !search.built;
    showSettingsSubtab();
    if (!search.built) { return; }
    try { search.data = JSON.parse(text); } catch (e) { search.data = null; }
    if (search.data) { fillSearch(search.data); }
  }
  function searchForm() {
    return {
      enabled: $("search-enabled").checked, url: $("search-url").value.trim(), results: Number($("search-results").value) || 5,
      max_rounds: Number($("search-rounds").value) || 8, timeout_s: Number($("search-timeout").value) || 30
    };
  }
  async function saveSearch() {
    showSearchError("");
    var answer = await downloadsAnswer(api("/v1/strata/search/settings", jsonPost(searchForm())));
    if (answer.signedOut) { return; }
    if (answer.error) { showSearchError(answer.error); return; }
    $("search-result").textContent = "Saved. They apply when the server restarts.";
    await refreshSearch();
  }
  async function testSearch() {
    showSearchError("");
    $("search-result").textContent = "Searching through the saved settings...";
    $("search-output").hidden = true;
    $("search-test").disabled = true;
    var answer = await downloadsAnswer(api("/v1/strata/search/test", jsonPost({ query: $("search-query").value.trim() })));
    $("search-test").disabled = false;
    if (answer.signedOut) { return; }
    if (answer.error) { $("search-result").textContent = ""; showSearchError(answer.error); return; }
    var r = answer.body || {};
    $("search-result").textContent = r.ok === true ? "It works. This is what a model reads (" + String(r.ms || 0) + " ms):" : "";
    $("search-output").textContent = String(r.text || "");
    $("search-output").hidden = !r.text || r.ok !== true;
    if (r.ok !== true) { showSearchError("The search failed: " + String(r.text || "")); }
  }

  // ---------------------------------------------------------------- Monitor (GET /metrics)
  // Upstream's Monitor tab on this server's /metrics (listener.monitor): asked once a second
  // while the tab is open. A server with the Monitor off answers 404 and the tab says so.
  var SVG_NS = "http://www.w3.org/2000/svg";
  var METRICS = [
    { key: "speed", label: "Speed", icon: "i-gauge", series: "tok_s" },
    { key: "gpu", label: "GPU load", icon: "i-gpu", series: "gpu_util", max: 100 },
    { key: "vram", label: "VRAM", icon: "i-layers", series: "gpu_mem_used" },
    { key: "temp", label: "GPU temp", icon: "i-thermometer", series: "gpu_temp", tone: "warn", max: 90 },
    { key: "power", label: "Power", icon: "i-bolt", series: "gpu_power" },
    { key: "pcie", label: "PCIe", icon: "i-link", tone: "info" },
    { key: "cpu", label: "CPU", icon: "i-cpu", series: "cpu", max: 100 },
    // upstream's eighth tile (serve/web/app.js:132): this PC's disks' reads, MB/s (GB/s past 1000); eight tiles fill
    // the grid's rows evenly at four, two and one across
    { key: "disk", label: "Disk read", icon: "i-disk", series: "disk_read_mb", tone: "info" }
  ];
  var FINISH = { stop: ["", "Done"], length: ["", "Max tokens"], cancel: ["st-badge--queued", "Stopped"],
                 disconnect: ["st-badge--queued", "Closed"], error: ["st-badge--error", "Error"] };
  var monitor = { timer: null, showAll: false, last: null, polling: false };

  function fmt(value, digits) {
    if (value === null || value === undefined || !isFinite(value)) { return "-"; }
    var d = digits || 0;
    return Number(value).toLocaleString(undefined, { minimumFractionDigits: d, maximumFractionDigits: d });
  }
  function gb(bytes, digits) { return fmt(bytes / 1073741824, digits === undefined ? 1 : digits); }
  // A size with its unit: GB from 1 GiB up, MB below (an engine's own memory, a parked conversation).
  function size(bytes) { return bytes >= 1073741824 ? gb(bytes) + " GB" : fmt(bytes / 1048576) + " MB"; }
  function kfmt(n) { return n >= 1000 ? fmt(n / 1000, n >= 10000 ? 0 : 1) + "K" : fmt(n); }
  function ctxfmt(n) { return n >= 1048576 ? fmt(n / 1048576, n % 1048576 ? 1 : 0) + "M" : n >= 1024 ? fmt(n / 1024) + "K" : fmt(n); }
  function svgNode(tag, attributes) {
    var node = document.createElementNS(SVG_NS, tag);
    Object.keys(attributes).forEach(function (name) { node.setAttribute(name, attributes[name]); });
    return node;
  }

  function buildMetricCards() {
    var grid = $("metrics");
    clear(grid);
    METRICS.forEach(function (m) {
      var card = el("div", "st-card metric-card");
      var body = el("div", "st-metric");
      var label = el("span", "st-metric__label");
      label.appendChild(icon(m.icon, true));
      label.appendChild(document.createTextNode(m.label));
      body.appendChild(label);
      if (m.key === "speed") {
        var values = el("div", "speed-values");
        [["speed", "Decode"], ["prefill", "Prefill"]].forEach(function (pair) {
          var column = el("div", pair[0] === "prefill" ? "speed-prefill" : null);
          var value = el("span", "st-metric__value", "-");
          value.id = "mv-" + pair[0];
          var sub = el("span", "st-metric__sub", pair[1]);
          sub.id = "ms-" + pair[0];
          column.appendChild(value);
          column.appendChild(sub);
          values.appendChild(column);
        });
        body.appendChild(values);
      } else {
        var value = el("span", "st-metric__value", "-");
        value.id = "mv-" + m.key;
        var sub = el("span", "st-metric__sub");
        sub.id = "ms-" + m.key;
        body.appendChild(value);
        body.appendChild(sub);
      }
      var spark = svgNode("svg", { "class": "st-metric__spark", id: "sp-" + m.key, viewBox: "0 0 100 32", preserveAspectRatio: "none" });
      if (m.tone) { spark.setAttribute("data-tone", m.tone); }
      spark.appendChild(svgNode("path", { "class": "area", fill: "currentColor", opacity: ".12" }));
      spark.appendChild(svgNode("path", { "class": "line", fill: "none", stroke: "currentColor", "stroke-width": "1.6",
                                          "stroke-linejoin": "round", "stroke-linecap": "round", "vector-effect": "non-scaling-stroke" }));
      body.appendChild(spark);
      card.appendChild(body);
      grid.appendChild(card);
    });
  }
  function setMetric(key, value, unit, sub) {
    var node = $("mv-" + key);
    clear(node);
    node.appendChild(document.createTextNode(value === null || value === undefined ? "-" : value));
    if (unit && value !== null && value !== undefined) { node.appendChild(el("small", null, unit)); }
    $("ms-" + key).textContent = sub || "";
  }
  function spark(key, values, max) {
    var svg = $("sp-" + key);
    if (!svg) { return; }
    var line = svg.querySelector(".line");
    var area = svg.querySelector(".area");
    var points = (values || []).map(function (x) { return x === null || x === undefined ? 0 : x; });
    if (points.length < 2) { line.setAttribute("d", ""); area.setAttribute("d", ""); return; }
    var top = Math.max.apply(null, [max || 0, 1].concat(points));
    var path = points.map(function (x, i) {
      return (i ? "L" : "M") + ((i / (points.length - 1)) * 100).toFixed(2) + "," + (30 - (x / top) * 26).toFixed(2);
    }).join("");
    line.setAttribute("d", path);
    area.setAttribute("d", path + "L100,32L0,32Z");
  }
  function setBar(id, percent) { $(id).style.width = Math.max(0, Math.min(100, percent || 0)) + "%"; }

  function totalsText(t) {
    if (!t || !t.requests) { return ""; }
    var since = new Date(t.since * 1000).toLocaleString([], { weekday: "short", hour: "2-digit", minute: "2-digit" });
    var read = Math.max(0, t.prompt_tokens - t.reused);
    var prefill = t.prompt_ms > 0 && read > 0 ? " at " + fmt(read / (t.prompt_ms / 1000)) + " tok/s" : "";
    var decode = t.decode_ms > 0 && t.output_tokens > 0 ? " at " + fmt(t.output_tokens / (t.decode_ms / 1000), 1) + " tok/s" : "";
    return "Since " + since + ": " + fmt(t.requests) + " requests" + DOT + fmt(read) + " prompt tokens read" + prefill +
           " (" + fmt(t.reused) + " reused)" + DOT + fmt(t.output_tokens) + " written" + decode;
  }
  function requestRow(r) {
    var row = el("tr");
    var cell = function (text, numeric) { var td = el("td", numeric ? "num" : null, text); row.appendChild(td); return td; };
    cell(new Date(r.time * 1000).toLocaleTimeString([], { hour: "2-digit", minute: "2-digit", second: "2-digit" }));
    cell(r.model || "-");  // the model it ran on (owner 2026-10-27)
    var badge = FINISH[r.finish] || ["", r.finish || "-"];
    var status = cell("");
    status.appendChild(el("span", "st-badge " + badge[0], badge[1]));
    cell(fmt(r.prompt_tokens), true);
    cell(fmt(r.reused), true);
    cell(fmt(r.output_tokens), true);
    var fresh = r.prompt_tokens === null || r.prompt_tokens === undefined ? null : Math.max(0, r.prompt_tokens - (r.reused || 0));
    cell(fresh > 0 && r.prompt_ms > 0 ? fmt(fresh / (r.prompt_ms / 1000)) : "-", true);
    cell(fmt(r.decode_tok_s, 1), true);
    cell(fmt(r.duration_s, 1) + " s", true);
    return row;
  }

  function renderMonitor(m) {
    monitor.last = m;
    var live = m.live || {}, hw = m.hardware || {}, st = m.hardware_static || {}, eng = m.engine || {}, h = m.history || {};
    var requests = m.requests || [];
    // stopped (owner 2026-10-27): no engine runs, so no live figures of the one that ran; the history stays below
    var stopped = live.state === "stopped";
    // Owner 2026-10-28 ("when the models are stop and nothing is loaded, the stats seem to stay"): the engine's own
    // figures (the last request's speeds, its context, its experts and memory) only while an engine is up - not while
    // one starts, has failed or was unloaded after idling, nor while the server does not answer (offline). A server
    // without engine.loaded: up unless stopped.
    var offline = live.state === "offline";
    var loaded = eng.loaded === undefined ? !stopped && !offline : eng.loaded === true;
    var last = loaded ? requests[0] : null;
    var busy = live.state === "reading" || live.state === "generating";
    // The model state.
    var on = live.queued > 0 ? "queued" : live.state;
    Array.prototype.forEach.call($("state-badges").querySelectorAll(".st-badge"), function (b) {
      var s = b.getAttribute("data-s");
      b.classList.toggle("on", s === on || s === live.state);
    });
    var label = "Waiting for a request", detail = "", pct = 0;
    var progress = $("state-progress");
    progress.removeAttribute("data-tone");
    if (offline) {
      label = "The server is not answering";
      detail = "its figures come back when it does";
    } else if (stopped) {
      label = "No model is running";
      detail = "stopped";
    } else if (live.state === "unloaded") {
      // unloaded after idling, or a lazy start that has not loaded yet: either way not in memory now
      label = "The model is not loaded";
      detail = "the next request loads it";
    } else if (live.state === "starting") {
      label = "The engine is starting";
    } else if (live.state === "calibrating") {
      // owner 2026-10-29: the model that ran was stopped first; each trial starts the model being calibrated
      label = "Calibrating " + (eng.model || "a model");
      detail = "each trial starts the model, so the GPU memory in use is the calibration's";
    } else if (live.state === "error") {
      label = "The engine is not available";
    } else if (live.state === "reading") {
      label = "Reading prompt";
      progress.setAttribute("data-tone", "info");
      detail = fmt(live.prompt_tokens) + " tokens" + DOT + fmt(live.elapsed_s, 1) + " s";
    } else if (live.state === "generating") {
      label = "Generating";
      pct = live.max_tokens ? (100 * live.generated) / live.max_tokens : 0;
      detail = fmt(live.generated) + " tokens" + (live.tok_s ? DOT + fmt(live.tok_s, 1) + " tok/s" : "");
    } else if (last) {
      detail = "last: " + fmt(last.output_tokens) + " tokens" + (last.decode_tok_s ? " at " + fmt(last.decode_tok_s, 1) + " tok/s" : "");
    }
    if (live.queued > 0) { detail += (detail ? DOT : "") + fmt(live.queued) + " queued"; }
    $("state-label").textContent = label;
    $("state-detail").textContent = detail;
    setBar("state-bar", pct);
    // The cards.
    var speed = live.state === "generating" ? live.tok_s : last ? last.decode_tok_s : null;
    setMetric("speed", speed === null || speed === undefined ? null : fmt(speed, 1), "t/s",
              live.state === "generating" ? "Decode now" : last ? "Decode last request" : "Decode");
    var prefill = busy && live.prefill_tok_s ? live.prefill_tok_s
                : last && last.prompt_ms > 0 ? Math.max(0, last.prompt_tokens - (last.reused || 0)) / (last.prompt_ms / 1000) : null;
    setMetric("prefill", prefill === null ? null : fmt(prefill), "t/s",
              live.state === "reading" ? "Prefill now" : busy ? "Prefill this request" : last ? "Prefill last request" : "Prefill");
    spark("speed", h.tok_s);
    var has = function (v) { return v !== null && v !== undefined; };
    // A PC without a GPU reading keeps the GPU tiles in place, each saying so (GPU load says why: the note).
    var gpuNone = !offline && !has(hw.gpu_util) && !has(hw.gpu_mem_used) && !has(hw.gpu_temp) ? "no GPU reading" : "";
    var gpuSub = st.gpu_name || st.gpu_note || "";
    setMetric("gpu", hw.gpu_util === null || hw.gpu_util === undefined ? null : fmt(hw.gpu_util), "%", gpuSub);
    spark("gpu", h.gpu_util, 100);
    setMetric("vram", hw.gpu_mem_used === null || hw.gpu_mem_used === undefined ? null : gb(hw.gpu_mem_used),
              hw.gpu_mem_total ? "/ " + gb(hw.gpu_mem_total, 0) + " GB" : "GB", gpuNone);
    spark("vram", h.gpu_mem_used, hw.gpu_mem_total);
    setMetric("temp", hw.gpu_temp === null || hw.gpu_temp === undefined ? null : fmt(hw.gpu_temp), DEG + "C", gpuNone);
    spark("temp", h.gpu_temp, 90);
    setMetric("power", hw.gpu_power === null || hw.gpu_power === undefined ? null : fmt(hw.gpu_power), "W",
              hw.gpu_power_limit ? "of " + fmt(hw.gpu_power_limit) + " W limit" : gpuNone);
    spark("power", h.gpu_power, hw.gpu_power_limit);
    var gen = hw.gpu_pcie_gen_max || hw.gpu_pcie_gen;
    setMetric("pcie", gen ? "Gen" + gen : null, hw.gpu_pcie_width ? "x" + hw.gpu_pcie_width : "",
              hw.gpu_pcie_gen && gen && hw.gpu_pcie_gen < gen ? "idle Gen" + hw.gpu_pcie_gen : gpuNone);
    setMetric("cpu", hw.cpu === null || hw.cpu === undefined ? null : fmt(hw.cpu), "%", st.threads ? st.threads + " threads" : "");
    spark("cpu", h.cpu, 100);
    // Disk read (owner 2026-10-28, upstream's tile: serve/web/app.js:312-319): every disk of this PC, MB/s, GB/s past
    // 1000, with the write rate and the minute's peak; a disk-bound model reads its experts here.
    var diskHistory = h.disk_read_mb || [];
    var diskPeak = Math.max.apply(null, [0].concat(diskHistory.filter(has)));
    var rate = function (mb) { return mb >= 1000 ? fmt(mb / 1024, 2) + " GB/s" : fmt(mb, mb < 10 ? 1 : 0) + " MB/s"; };
    if (!has(hw.disk_read_mb)) {
      setMetric("disk", null, "", !offline && diskHistory.length > 2 && !diskHistory.some(has) ? "not readable on this PC" : "");
    } else {
      var bigDisk = hw.disk_read_mb >= 1000;
      var diskSub = [];
      if (has(hw.disk_write_mb)) diskSub.push("write " + rate(hw.disk_write_mb));
      if (diskPeak > 0) diskSub.push("peak " + rate(diskPeak));
      setMetric("disk", bigDisk ? fmt(hw.disk_read_mb / 1024, 2) : fmt(hw.disk_read_mb, hw.disk_read_mb < 10 ? 1 : 0),
                bigDisk ? "GB/s" : "MB/s", diskSub.join(DOT));
    }
    spark("disk", diskHistory);
    // Context fill: the running request, else the last one.
    var ctx = eng.max_context || 0;
    var used = busy ? (live.prompt_tokens || 0) + (live.generated || 0) : last ? (last.prompt_tokens || 0) + (last.output_tokens || 0) : 0;
    var frac = ctx ? Math.min(1, used / ctx) : 0;
    $("ctx-fill").setAttribute("stroke-dasharray", (235.6 * frac).toFixed(1) + " 314.2");
    $("ctx-fill").style.opacity = 235.6 * frac >= 3 ? "1" : "0";  // a near-zero arc would draw just its round cap
    $("ctx-pct").textContent = Math.round(frac * 100) + "%";
    $("ctx-sub").textContent = ctx ? kfmt(used) + " / " + ctxfmt(ctx) : "-";
    // Owner ruling: the VRAM and RAM cards show EXPERTS LOADED beside the GB. Owner 2026-10-28 ("the System Ram reports
    // MB usage only and not experts + mb usage"): /metrics engine.memory carries every family's own figures in one
    // shape (the experts in VRAM and in RAM, their RAM bytes) and the engine process's own memory - from its start,
    // not only once it decodes; none of it while no engine is up.
    var mem = loaded && eng.memory ? eng.memory : {};
    var vramParts = [];
    if (has(mem.experts_vram)) vramParts.push(fmt(mem.experts_vram) + " experts loaded");
    if (hw.gpu_mem_total) vramParts.push(gb(hw.gpu_mem_used) + " / " + gb(hw.gpu_mem_total, 0) + " GB");
    $("vram-text").textContent = vramParts.length ? vramParts.join(" \u00b7 ") : "-";
    setBar("vram-bar", hw.gpu_mem_total ? (100 * hw.gpu_mem_used) / hw.gpu_mem_total : 0);
    var ramParts = [];
    if (has(mem.experts_ram)) {
      ramParts.push(fmt(mem.experts_ram) + " experts loaded" + (has(mem.experts_ram_bytes) ? " (" + size(mem.experts_ram_bytes) + ")" : ""));
    } else if (has(mem.experts_ram_bytes)) {
      ramParts.push("experts " + size(mem.experts_ram_bytes));
    }
    if (has(mem.process_bytes)) ramParts.push("engine " + size(mem.process_bytes));
    if (hw.ram_total) ramParts.push(gb(hw.ram_used) + " / " + gb(hw.ram_total, 0) + " GB");
    $("ram-text").textContent = ramParts.length ? ramParts.join(" \u00b7 ") : "-";
    var ramPct = hw.ram_total ? (100 * hw.ram_used) / hw.ram_total : 0;
    setBar("ram-bar", ramPct);
    if (ramPct > 92) { $("ram-progress").setAttribute("data-tone", "danger"); } else { $("ram-progress").removeAttribute("data-tone"); }
    $("temp-text").textContent = hw.gpu_temp === null || hw.gpu_temp === undefined ? "-" : fmt(hw.gpu_temp) + " " + DEG + "C";
    setBar("temp-bar", hw.gpu_temp || 0);
    // The recent requests.
    var body = $("req-body");
    clear(body);
    if (!requests.length) {
      var empty = el("tr");
      var td = el("td", "muted", "No requests yet");
      td.colSpan = 9;
      empty.appendChild(td);
      body.appendChild(empty);
    } else {
      requests.slice(0, monitor.showAll ? requests.length : 12).forEach(function (r) { body.appendChild(requestRow(r)); });
    }
    var kept = m.requests_kept === undefined ? requests.length : m.requests_kept;
    $("req-all").hidden = kept <= 12;
    $("req-all").textContent = monitor.showAll ? "Show fewer" : "Show all (" + kept + ")";
    $("req-wrap").classList.toggle("all", monitor.showAll);
    $("req-totals").textContent = totalsText(m.totals);
    renderConvCache(m.conversation_cache, live.state);
  }

  // The conversation cache (owner 2026-10-28: "upstream strata has disk read and conversation cache as well"; upstream's
  // card, serve/web/app.js:210-241, on /metrics conversation_cache): what the engine keeps of earlier conversations -
  // the state of the last one always, whole conversations parked in RAM with --conversation-cache-mib - and how much of
  // the prompts the requests got back from it.
  function sinceText(t) {
    if (!t) { return ""; }
    var s = Math.max(0, Date.now() / 1000 - t);
    return s < 60 ? "just now" : s < 3600 ? fmt(s / 60) + " min ago" : fmt(s / 3600, 1) + " h ago";
  }
  function ccFact(list, label, value) {
    list.appendChild(el("dt", null, label));
    list.appendChild(el("dd", null, value === null || value === undefined ? "-" : value));
  }
  function renderConvCache(c, liveState) {
    var list = $("cc-facts");
    clear(list);
    var cache = c || {};
    var enabled = cache.enabled === true;
    $("cc-bars").hidden = !enabled;
    var pct = function (a, b) { return b ? Math.min(100, (100 * a) / b) : 0; };
    if (enabled) {
      $("cc-slots-text").textContent = fmt(cache.parked) + " / " + fmt(cache.slots);
      setBar("cc-slots-bar", pct(cache.parked, cache.slots));
      var budget = cache.budget_mib * 1048576;
      $("cc-mem-text").textContent = size(cache.bytes) + " / " + size(budget);
      setBar("cc-mem-bar", pct(cache.bytes, budget));
    }
    $("cc-sum").textContent = cache.requests ? fmt(cache.requests_reused) + " of " + fmt(cache.requests) + " requests reused part of their prompt" : "";
    var share = cache.prompt_tokens ? " (" + fmt((100 * cache.reused_tokens) / cache.prompt_tokens) + "% of all prompt tokens)" : "";
    ccFact(list, "Last request", cache.last_prompt !== null && cache.last_prompt !== undefined
      ? fmt(cache.last_reused || 0) + " of " + fmt(cache.last_prompt) + " prompt tokens reused" : null);
    ccFact(list, "Reused since start", cache.requests ? fmt(cache.reused_tokens) + " tokens" + share : null);
    if (enabled) {
      ccFact(list, "Parked / restored", fmt(cache.parks) + " / " + fmt(cache.restores) + (cache.evictions ? DOT + fmt(cache.evictions) + " evicted" : ""));
      ccFact(list, "Last switch", cache.last_event
        ? (cache.last_event === "parked" ? "Parked " : "Restored ") + fmt(cache.last_tokens) + " tokens, " + sinceText(cache.last_at) : null);
    }
    var note;
    if (liveState === "offline") {
      note = "The server is not answering.";
    } else if (!c) {
      note = "This server does not report a conversation cache.";
    } else if (cache.supported !== true && cache.supported !== false) {
      note = "No engine is up: nothing is kept of earlier conversations now.";
    } else if (cache.supported === false) {
      note = "Not used by this engine: it keeps no conversation cache, so this card only counts the reuse its requests report.";
    } else if (enabled) {
      note = "A request that continues a parked conversation gets its state back instead of reading it again; the oldest goes when the slots or the memory are full.";
    } else {
      note = "The engine keeps the last conversation's state, so a follow-up reads only what is new. To keep several conversations (agents taking turns), start it with --conversation-cache-mib 8192.";
    }
    $("cc-note").textContent = note;
  }

  // The server does not answer the Monitor (it stopped or restarts, the network is down, the key changed): no live
  // figure stays on the page - the engine's or this PC's - while the history (the recent requests, the totals, the
  // charts) keeps what happened.
  function offlineSnapshot(previous) {
    var p = previous || {};
    return { engine: { loaded: false }, live: { state: "offline", queued: 0 }, hardware: {}, hardware_static: {},
             history: p.history || {}, requests: p.requests || [], requests_kept: p.requests_kept, totals: p.totals,
             conversation_cache: null };
  }
  function showMonitorOff(off) {
    $("monitor-off").hidden = !off;
    ["state-card", "metrics", "monitor-row", "req-card"].forEach(function (id) { $(id).hidden = off; });
  }
  async function pollMonitor() {
    if (monitor.polling) { return; }  // one poll at a time: a slow one never lands after a newer one
    monitor.polling = true;
    var response, text;
    try {
      response = await api("/metrics");
      text = await response.text();
    } catch (e) {
      // the next poll tries again; meanwhile nothing the server said last stays on the page as if it were live
      monitor.polling = false;
      renderMonitor(offlineSnapshot(monitor.last));
      return;
    }
    monitor.polling = false;
    if (response.status === 404) { showMonitorOff(true); stopMonitor(); return; }
    if (response.status === 401 && !state.keyless) { signOut("The server no longer accepts this API key."); return; }
    // a busy server (this address's connection cap while the page polls several things) is up: keep the figures
    if (response.status === 429 || (response.status === 503 && errorCode(text) === "server_busy")) { return; }
    var parsed = null;
    try { parsed = response.ok ? JSON.parse(text) : null; } catch (e) { parsed = null; }
    if (!parsed) { renderMonitor(offlineSnapshot(monitor.last)); return; }
    showMonitorOff(false);
    renderMonitor(parsed);
  }
  function startMonitor() {
    stopMonitor();
    pollMonitor();
    monitor.timer = window.setInterval(pollMonitor, 1000);
  }
  function stopMonitor() { if (monitor.timer) { window.clearInterval(monitor.timer); monitor.timer = null; } }

  // ---------------------------------------------------------------- theme
  function syncThemeIcon() {
    var dark = window.StrataTheme && window.StrataTheme.get() === "dark";
    $("theme-icon").setAttribute("href", SPRITE + (dark ? "i-sun" : "i-moon"));
    $("dark-toggle").setAttribute("aria-checked", dark ? "true" : "false");
  }
  function toggleTheme() {
    if (!window.StrataTheme) { return; }
    window.StrataTheme.set(window.StrataTheme.get() === "dark" ? "light" : "dark");
    syncThemeIcon();
  }

  // ---------------------------------------------------------------- wiring
  function wire() {
    $("login-form").addEventListener("submit", function (event) {
      event.preventDefault();
      var key = $("key").value.trim();
      if (!key) { $("login-error").textContent = "Enter the API key."; $("login-error").hidden = false; return; }
      signIn(key);
    });
    $("composer").addEventListener("submit", function (event) { event.preventDefault(); submit(); });
    $("input").addEventListener("keydown", function (event) {
      if (event.key === "Enter" && !event.shiftKey && !event.isComposing) { event.preventDefault(); submit(); }
    });
    $("input").addEventListener("input", autosize);
    $("stop-btn").addEventListener("click", function () { if (state.controller) { state.controller.abort(); } });
    $("new-btn").addEventListener("click", newChat);
    $("export-btn").addEventListener("click", exportChat);
    $("attach-btn").addEventListener("click", function () { $("file").click(); });
    $("file").addEventListener("change", function () { addFiles($("file").files); $("file").value = ""; });
    var composer = $("composer");
    composer.addEventListener("dragover", function (event) { event.preventDefault(); composer.classList.add("dragging"); });
    composer.addEventListener("dragleave", function () { composer.classList.remove("dragging"); });
    composer.addEventListener("drop", function (event) {
      event.preventDefault();
      composer.classList.remove("dragging");
      if (event.dataTransfer && event.dataTransfer.files) { addFiles(event.dataTransfer.files); }
    });
    $("sampling-btn").addEventListener("click", function () { openDrawer(true); });
    $("drawer-close").addEventListener("click", function () { openDrawer(false); });
    $("scrim").addEventListener("click", function () { openDrawer(false); });
    Array.prototype.forEach.call($("s-thinking").querySelectorAll("button"), function (button) {
      button.addEventListener("click", function () {
        Array.prototype.forEach.call($("s-thinking").querySelectorAll("button"), function (b) {
          b.setAttribute("aria-checked", b === button ? "true" : "false");
        });
        $("o-thinking").textContent = LEVELS[button.getAttribute("data-v")] || "";
      });
    });
    $("s-temp").addEventListener("input", function () { $("o-temp").textContent = Number($("s-temp").value).toFixed(2); });
    $("s-topp").addEventListener("input", function () { $("o-topp").textContent = Number($("s-topp").value).toFixed(2); });
    $("s-show").addEventListener("click", function () {
      $("s-show").setAttribute("aria-checked", $("s-show").getAttribute("aria-checked") === "true" ? "false" : "true");
    });
    $("s-search").addEventListener("click", function () {
      $("s-search").setAttribute("aria-checked", $("s-search").getAttribute("aria-checked") === "true" ? "false" : "true");
    });
    // Reset: the running card's settings (its model's recommended), else the page's own
    $("s-reset").addEventListener("click", function () {
      var card = runningCard();
      state.settings = card ? settingsFrom(card, Object.assign({}, DEFAULTS)) : Object.assign({}, DEFAULTS);
      showSettings();
    });
    $("s-apply").addEventListener("click", applySettings);
    $("tab-btn-chat").addEventListener("click", function () { showView("chat"); });
    $("tab-btn-models").addEventListener("click", function () { showView("models"); });
    $("models-subtab-start").addEventListener("click", function () { showModelsSubtab("start"); });
    $("models-subtab-download").addEventListener("click", function () { showModelsSubtab("download"); });
    $("tab-btn-settings").addEventListener("click", function () { showView("settings"); });
    $("key-show").addEventListener("click", function () { keyShown = !keyShown; renderKey(); });
    $("key-copy").addEventListener("click", function () { if (state.key) { copyText(state.key); } });
    $("key-rotate").addEventListener("click", rotateKey);
    $("key-new-copy").addEventListener("click", function () { copyText($("key-new-value").textContent); });
    document.querySelectorAll("[data-save]").forEach(function (button) {
      button.addEventListener("click", function () { saveServerConfig(button.getAttribute("data-save")); });
    });
    $("alerts-ionos").addEventListener("click", fillIonos);
    $("alerts-save").addEventListener("click", saveAlerts);
    $("alerts-password-save").addEventListener("click", saveAlertsPassword);
    $("alerts-test").addEventListener("click", testAlerts);
    $("search-save").addEventListener("click", saveSearch);
    $("folders-save").addEventListener("click", saveFolders);
    $("search-test").addEventListener("click", testSearch);
    $("no-model-open").addEventListener("click", function () { showView("models"); });
    $("models-refresh").addEventListener("click", function () { refreshLauncher(); });
    $("model-start").addEventListener("click", startModel);
    $("model-stop").addEventListener("click", stopModel);
    $("model-defaults").addEventListener("click", resetOptions);
    $("downloads-refresh").addEventListener("click", function () { refreshDownloads(); });
    $("downloads-dest-change").addEventListener("click", chooseDestination);
    $("downloads-dest-reset").addEventListener("click", useDefaultDestination);
    $("models-empty-add").addEventListener("click", function () { showModelsSubtab("download"); });
    $("local-models-pick").addEventListener("click", addLocalModel);
    $("model-folder-change").addEventListener("click", changeCardFolder);
    $("model-draft-change").addEventListener("click", function () { pointDraft(findCard(launcher.selected)); });
    $("model-delete").addEventListener("click", beginDelete);
    $("model-remove").addEventListener("click", removeCard);
    $("model-calibrate").addEventListener("click", calibrateModel);
    $("model-calibrate-cancel").addEventListener("click", cancelCalibration);
    $("model-calibration-auto").addEventListener("change", setAutoRecalibrate);
    $("delete-next").addEventListener("click", deleteNext);
    $("delete-cancel").addEventListener("click", closeDelete);
    $("delete-dialog").addEventListener("cancel", function () { closeDelete(); });
    ["delete-code-input", "delete-key-input"].forEach(function (id) {
      $(id).addEventListener("keydown", function (event) { if (event.key === "Enter") { event.preventDefault(); deleteNext(); } });
    });
    $("folder-picker-up").addEventListener("click", pickerUp);
    $("folder-picker-cancel").addEventListener("click", function () { closePicker(null); });
    $("folder-picker").addEventListener("cancel", function (event) { event.preventDefault(); closePicker(null); });
    $("folder-picker-use").addEventListener("click", function () {
      if (picker.listing) { closePicker({ folder: String(picker.listing.path), free: picker.listing.free_bytes }); }
    });
    $("folder-picker-typed-form").addEventListener("submit", function (event) {
      event.preventDefault();
      var typed = $("folder-picker-typed").value.trim();
      if (typed) { pickerOpen(typed); }
    });
    $("folder-picker-new").addEventListener("submit", function (event) { event.preventDefault(); pickerNewFolder(); });
    $("downloads-link-form").addEventListener("submit", function (event) { event.preventDefault(); checkLink(); });
    $("downloads-url").addEventListener("input", function () {
      showLinkError("");
      if (downloads.confirm && downloads.confirm.url !== $("downloads-url").value.trim()) { hideConfirm(); }
    });
    $("tab-btn-monitor").addEventListener("click", function () { showView("monitor"); });
    $("tab-btn-about").addEventListener("click", function () { showView("about"); });
    $("req-all").addEventListener("click", function () {
      monitor.showAll = !monitor.showAll;
      if (monitor.last) { renderMonitor(monitor.last); }
    });
    buildMetricCards();
    $("theme-btn").addEventListener("click", toggleTheme);
    $("dark-toggle").addEventListener("click", toggleTheme);
    $("sign-out").addEventListener("click", function () { signOut(""); });
  }

  // The view an address asks for - #view=<name>, or #login=<code>&view=<name> - among the views every server has
  // (strata-window opens the page on the Monitor, as the owner asked); null: none.
  function addressView(hash) {
    var asked = /(?:^#|&)view=(chat|monitor|about)$/.exec(hash || "");
    return asked ? asked[1] : null;
  }

  // A one-time login link in the address - at load, or opened later in this tab (strata-window does so after a server
  // restart: a fragment change, no reload) - the code leaves the address bar at once, then buys this tab the key. true
  // when there was one. A failed link while signed in keeps the session (a notice only).
  async function claimFromAddress(signedIn) {
    var link = /^#login=([A-Za-z0-9_-]{16,128})(?:&view=[a-z]+)?$/.exec(window.location.hash || "");
    if (!link) { return false; }
    var view = addressView(window.location.hash) || (signedIn ? state.view : null);
    try { window.history.replaceState(null, "", window.location.pathname + window.location.search); } catch (e) { /* kept */ }
    var before = state.key;
    var claimed = await claimLoginLink(link[1]);
    if (claimed) {
      await signIn(claimed, view);
      // shown once when new to this tab (a first sign-in, a rotation's new key), not for a restart's same key
      if (state.key === claimed && claimed !== before) { showKeyOnce(claimed); }
      return true;
    }
    var expired = "That login link has expired or was already used. Restart the server for a new one, or sign in with your key.";
    if (signedIn) { toast("error", expired); return true; }
    showLogin(expired);
    return true;
  }

  async function start() {
    state.settings = Object.assign({}, DEFAULTS, load(SLOT_SETTINGS, {}));
    // Settings saved by the page's earlier Off / On control: off is none, on was high.
    if (state.settings.thinking === "off") { state.settings.thinking = "none"; }
    if (!LEVELS[state.settings.thinking]) { state.settings.thinking = "high"; }
    // A saved chat is repaired on the way in: it never ends on an unanswered question.
    var saved = load(SLOT_CHAT, []);
    state.history = Array.isArray(saved) ? conversation(saved) : [];
    if (state.history.length && state.history[state.history.length - 1].role === "user") { state.history.pop(); }
    wire();
    syncThemeIcon();
    window.addEventListener("hashchange", function () { claimFromAddress(!!state.key || state.keyless); });
    if (await claimFromAddress(false)) { return; }
    var key = load(SLOT_KEY, null);
    if (typeof key === "string" && key) { await signIn(key, addressView(window.location.hash)); return; }
    try {
      var models = await listModels();  // keyless loopback server: the list answers without a key
      if (models !== null) { state.keyless = true; enterApp(models, addressView(window.location.hash)); return; }
    } catch (e) {
      showLogin("Cannot reach the server: " + e.message);
      return;
    }
    showLogin("");
  }

  start();
}());
