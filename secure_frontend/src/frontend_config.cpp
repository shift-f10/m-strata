#include "secure_frontend/frontend_config.hpp"

#include "secure_frontend/config_defaults.hpp"
#include "secure_frontend/engine_arguments.hpp"
#include "secure_frontend/http_policy.hpp"
#include "secure_frontend/json.hpp"
#include "secure_frontend/model_inventory.hpp"
#include "secure_frontend/search_url.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <charconv>
#include <limits>
#include <set>
#include <string>
#include <utility>

#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"
#endif

namespace secure_frontend {
namespace {
namespace d = config_defaults;
using T = ConfigValueType;

// SP5 M1 Task 4: a decimal row with this default has no default — an absent
// entry stays unset (never sent to the engine).
constexpr double kUnsetDecimal = std::numeric_limits<double>::quiet_NaN();

// Shortest round-trip decimal text (shared by issue messages and --check).
std::string decimal_text(const double value) {
  char buffer[32];
  const auto [end, ec] = std::to_chars(buffer, buffer + sizeof(buffer), value);
  return ec == std::errc{} ? std::string{buffer, end} : std::string{"?"};
}

constexpr std::array<ConfigKey, 43 + 7 + 3 + 9 + 7 + 3 + 13 + 6 + 8 + 1 + 9 + 9 + 1 + 2 + 1 + 1 + 2 + 4 + 5 + 1 + 1 /* roots.allow_untrusted_media */ + 1 /* glm.kv */ + 3 /* listener.login_link, models.default_engine, models.default_root */ + 8 /* alerts.email.* */ + 1 /* models.control_remote_networks */ + 5 /* search.* */ + 2 /* vision.mmproj, vision.gpu */ + 2 /* glm.pcie_share, glm.cpu_lane */> kSchema{{
    {"schema_version", T::integer, false, 1, 1, d::kSchemaVersion, false, "", "",
     "Config format version; must be 1. Required in a config file."},
    {"listener.host", T::string, false, 0, 0, 0, false, "127.0.0.1", "--host",
     "Bind address: an IPv4/IPv6 literal or localhost. Other hostnames are rejected (never resolved)."},
    {"listener.port", T::integer, false, 1, 65535, d::kListenerPort, false, "", "--port", "TCP port."},
    {"listener.allow_remote", T::boolean, false, 0, 0, 0, false, "", "--allow-remote",
     "Required for every non-loopback address."},
    {"listener.allow_wildcard_bind", T::boolean, false, 0, 0, 0, false, "", "--allow-wildcard-bind",
     "Also required for 0.0.0.0, :: and every other spelling of the unspecified address."},
    {"listener.backlog", T::integer, false, 1, 4096, d::kListenerBacklog, false, "", "", "listen() backlog."},
    {"listener.cors_origins", T::string_array, false, 0, 0, 0, false, "", "",
     "Exact browser origins allowed CORS: http:// or https://, a lower-case host, an optional port, no path; never * or null; at most 16."},
    {"listener.host_aliases", T::string_array, false, 0, 0, 0, false, "", "",
     "Extra exact Host values (host or host:port) clients use; required when the bind is a wildcard; at most 16."},
    {"listener.web_ui", T::boolean, false, 0, 0, 0, false, "", "",
     "Serve the built-in chat page at / (bundled files only). The page asks for the API key and keeps it in that tab; every API route still needs the key."},
    {"listener.loopback_without_key", T::boolean, false, 0, 0, 0, false, "", "",
     "Answer the API and the page without the API key: only with a loopback listener.host, and with no token.source; Host and Origin checks still apply."},
    {"listener.login_link", T::boolean, false, 0, 0, 0, false, "", "",
     "Print a one-time login link at every start (GUI-first start-up): opened from this PC (loopback only, within 10 minutes, once), it hands the page the API key. Needs listener.web_ui and a token.source."},
    {"listener.monitor", T::boolean, false, 0, 0, 0, false, "", "",
     "Serve GET /metrics (with the key) for the page's Monitor tab: the engine's state, the last 100 requests' token counts and timings (never their text), and this machine's CPU, RAM and GPU readings (the GPU through the system's nvidia-smi, a separate process)."},
    {"limits.http.request_deadline_ms", T::integer, false, 1, 3'600'000, d::kRequestDeadlineMs, false, "", "",
     "Total time to read one request, header and body."},
    {"limits.http.max_request_bytes", T::integer, false, 4096, 268'435'456, d::kMaxRequestBytes, false, "", "",
     "Largest request (header and body) the listener and parser accept."},
    {"limits.http.max_header_bytes", T::integer, false, 1024, 65'536, d::kMaxHeaderBytes, false, "", "",
     "Largest header block; at most limits.http.max_request_bytes."},
    {"limits.http.max_headers", T::integer, false, 8, 256, d::kMaxHeaders, false, "", "", "Most header fields."},
    {"limits.http.max_request_line_bytes", T::integer, false, 256, 16'384, d::kMaxRequestLineBytes, false, "", "",
     "Longest request line; at most limits.http.max_header_bytes."},
    {"limits.http.header_deadline_ms", T::integer, false, 1, 3'600'000, d::kHeaderDeadlineMs, false, "", "",
     "Time from accept to the end of the request header; at most limits.http.request_deadline_ms."},
    {"limits.http.write_stall_ms", T::integer, false, 100, 300'000, d::kWriteStallMs, false, "", "",
     "Longest send without progress before the client is treated as gone."},
    {"limits.http.response_write_deadline_ms", T::integer, false, 1'000, 3'600'000, d::kResponseWriteDeadlineMs, false, "", "",
     "Time to send a whole complete response, or a stream's tail after DONE; at least limits.http.write_stall_ms."},
    {"limits.http.stream_keepalive_ms", T::integer, false, 1'000, 300'000, d::kStreamKeepaliveMs, false, "", "",
     "SSE comment interval while no content flows (streaming)."},
    {"limits.chat.max_body_bytes", T::integer, false, 1, 268'435'456, d::kChatMaxBodyBytes, false, "", "",
     "Largest chat request body; at most max_request_bytes minus max_header_bytes."},
    {"limits.chat.max_messages", T::integer, false, 1, 65'536, d::kChatMaxMessages, false, "", "",
     "Most messages in one chat request."},
    {"limits.chat.max_message_bytes", T::integer, false, 1, 268'435'456, d::kChatMaxMessageBytes, false, "", "",
     "Largest single message content."},
    {"limits.chat.max_prompt_tokens", T::integer, false, 1, 1'048'576, d::kChatMaxPromptTokens, false, "", "",
     "Largest tokenized prompt sent to the engine."},
    {"limits.chat.max_completion_tokens", T::integer, false, 1, 1'048'576, d::kChatMaxCompletionTokens, false, "", "",
     "Largest max_tokens a request may ask for."},
    {"limits.chat.default_max_tokens", T::integer, false, 0, 1'048'576, d::kChatDefaultMaxTokens, false, "", "",
     "max_tokens when a request omits it; 0 = auto: min(max_completion_tokens, context - prompt - 8); at most limits.chat.max_completion_tokens."},
    {"limits.chat.max_generated_bytes", T::integer, false, 1, 16'777'216, d::kChatMaxGeneratedBytes, false, "", "",
     "Largest generated text returned."},
    {"limits.engine.ready_timeout_s", T::integer, false, 1, 86'400, d::kEngineReadyTimeoutS, false, "", "",
     "How long the supervisor waits for the engine's READY after each start; a miss kills and restarts it (upstream STRATA_ENGINE_READY_S)."},
    {"limits.engine.generation_timeout_s", T::integer, false, 1, 86'400, d::kEngineGenerationTimeoutS, false, "", "",
     "Total time for one generation before STOP."},
    {"limits.engine.stop_drain_timeout_s", T::integer, false, 1, 600, d::kEngineStopDrainTimeoutS, false, "", "",
     "Time to drain to DONE after STOP before the engine is marked failed."},
    {"limits.concurrency.max_concurrent_requests", T::integer, false, 1, 256, d::kMaxConcurrentRequests, false, "", "",
     "Worker threads: connections read or answered at once; 1 = serial."},
    {"limits.concurrency.max_queued_generations", T::integer, false, 0, 254, 0, false, "", "",
     "Chats waiting behind the running generation; 0 when max_concurrent_requests is 1, otherwise at most max_concurrent_requests - 2."},
    {"limits.concurrency.queue_timeout_ms", T::integer, false, 1'000, 3'600'000, d::kQueueTimeoutMs, false, "", "",
     "Longest wait for the engine before 503 server_busy."},
    {"limits.concurrency.max_connections_per_remote_address", T::integer, false, 1, 256, 0, false, "", "",
     "Workers one non-loopback address may hold; at most max_concurrent_requests. Loopback is exempt."},
    {"roots.models", T::string, true, 0, 0, 0, false, "", "--models-root",
     "Absolute directory of .gguf files. The root itself may be a symlink; nothing below it may be."},
    {"roots.tokenizer_pack", T::string, false, 0, 0, 0, false, "", "--tokenizer-pack",
     "Absolute tokenizer directory; only with strata-unified-server --expert (a managed model brings its own)."},
    {"roots.secret", T::string, false, 0, 0, 0, false, "", "--secret-root",
     "Absolute directory holding the token file. Default: the config file's directory."},
    {"roots.logs", T::string, false, 0, 0, 0, false, "", "", "Absolute log directory (created 0700; unused in SP1)."},
    {"roots.cache", T::string, false, 0, 0, 0, false, "", "",
     "Absolute cache directory (created 0700); when set, it holds the engine's CUDA cache in its cuda subdirectory."},
    {"roots.allow_untrusted_media", T::boolean, false, 0, 0, 0, false, "", "--allow-untrusted-media",
     "Use model folders as they are, even where other users of this PC can read or change them (shared folders, exFAT/FAT media whose permissions the OS ignores): roots.models and roots.tokenizer_pack open with RootPolicy::external (no owner-only / not-shared-writable check), and the page adds, moves and packs models in such folders without changing their permissions (the Settings tab's Model folders switch). Owner opt-in; links below the root are still refused; the server's own folders keep their checks."},
    {"roots.state", T::string, false, 0, 0, 0, false, "", "--state-dir",
     "Absolute, existing owner-only directory for engine state: the engine's cwd, per-generation temp and (without roots.cache) the CUDA cache. Required to serve."},
    {"token.source", T::string, false, 0, 0, 0, false, "file:token", "--token-file, --token-env",
     "file:<relative path below roots.secret> or env:<NAME>. Metadata only; the token is never in the config."},
    {"engine.profile", T::string, false, 0, 0, 0, false, "", "--profile",
     "qwen, glm53 or gemma4; only with --expert (a managed model's manifest sets it)."},
    {"engine.executable", T::string, true, 0, 0, 0, false, "", "--engine",
     "Absolute path of the engine; a regular file not writable by group or others."},
    {"engine.model_id", T::string, true, 0, 0, 0, false, "", "--model-id",
     "Default model reference name[:tag] from the model index (or, with --expert, an inventory ID)."},
    {"engine.args", T::string_array, false, 0, 0, 0, false, "", "--engine-arg (repeatable)",
     "Extra engine arguments; frontend-owned and unsupported flags are rejected."},
    {"engine.lib_dirs", T::string_array, false, 0, 0, 0, false, "", "--engine-lib-dir (repeatable)",
     "Absolute library directories for the engine (default: the engine executable's directory)."},
    {"engine.gpu", T::string, false, 0, 0, 0, false, "", "--gpu",
     "GPU device indices for the engine, e.g. 0 or 0,1 (distinct, 0-255, at most 16); rendered as the engine's --gpu argument (strata family: no GPU variable). Not supported with engine.profile glm53."},
    {"engine.allow_new_privileges", T::boolean, false, 0, 0, 0, false, "", "--allow-new-privileges",
     "Linux: do not set PR_SET_NO_NEW_PRIVS on the engine (default: set). Only for hosts whose NVIDIA driver must run setuid nvidia-modprobe to create /dev/nvidia-uvm on first use."},
    {"pull.downloader", T::string, false, 0, 0, 0, false, "", "",
     "Absolute path of curl for pull/update. Default /usr/bin/curl (Windows: System32\\curl.exe). Validated when a "
     "transfer or doctor --downloader runs, never at serve start."},
    {"pull.proxy", T::string, false, 0, 0, 0, false, "", "",
     "http://host:port or https://host:port; absent = --noproxy *. No credentials. Environment proxies are never read."},
    {"pull.allow_private_addresses", T::boolean, false, 0, 0, 0, false, "", "",
     "Allow sources that resolve to private or special-use addresses."},
    {"pull.max_total_bytes", T::integer, false, 1048576, 17592186044416, 1099511627776, false, "", "",
     "Largest manifest total a pull accepts (bytes)."},
    {"pull.max_duration_s", T::integer, false, 60, 604800, 172800, false, "", "", "Time budget for a whole pull run."},
    {"pull.stall_timeout_s", T::integer, false, 10, 3600, 120, false, "", "", "Abort a transfer slower than 1024 B/s for this long."},
    {"pull.connect_timeout_s", T::integer, false, 1, 300, 30, false, "", "", "TCP/TLS connect timeout per request."},
    // upstream's downloader's default (upstream/setup.py DL_CONNECTIONS 16; owner 2026-10-27: downloads were one stream)
    {"pull.connections", T::integer, false, 1, 32, 16, false, "", "",
     "Connections for a large download (parallel ranged, 32 MiB pieces; 1 = one stream). Hugging Face's CDN limits each connection, so several are much faster."},
    // SP7 M2 Task 3 (spec D-10): the release mirrors. Untrusted list; trust
    // comes from the pinned release key (D-2), never from these URLs.
    {"update.source", T::string_array, false, 0, 0, 0, false, "", "",
     "HTTPS release-mirror URLs that 'strata update' fetches the signed release envelope from."},
    {"logging.requests", T::boolean, false, 0, 0, 0, false, "", "",
     "One line per request to stderr: route, status, code, time, bytes, loopback or remote; never paths, headers, tokens or text."},
    // SP5 M1 Task 4 (parity row `sampling`): bounds copied from the pin —
    // clean_shared_defaults (serve/server.py:7331-7367) and the wire conditions
    // (serve/server.py:1360-1394). Unset rows are never sent. The 2.0 caps,
    // the 8192 window cap and the 32-bit seed cap are added security bounds (C-1).
    {"defaults.sampling.temperature", T::decimal, false, 0, 0, 0, false, "", "",
     "Request default temperature (pin: 0..2); unset keeps the engine default.", 0.0, 2.0, kUnsetDecimal, false},
    {"defaults.sampling.top_p", T::decimal, false, 0, 0, 0, false, "", "",
     "Request default top_p (pin: 0 < top_p <= 1); unset keeps the engine default.", 0.0, 1.0, kUnsetDecimal, true},
    {"defaults.sampling.top_k", T::integer, false, 1, 64, -1, false, "", "",
     "Request default top_k candidates (pin: an integer 1..64); unset keeps the engine default.", 0.0, 0.0, kUnsetDecimal, false},
    {"defaults.sampling.min_p", T::decimal, false, 0, 0, 0, false, "", "",
     "Request default min_p (pin wire: only 0 < min_p <= 1 is sent; 0 stays off).", 0.0, 1.0, kUnsetDecimal, false},
    {"defaults.sampling.presence_penalty", T::decimal, false, 0, 0, 0, false, "", "",
     "Request default presence penalty (0 off; cap 2 added, C-1).", 0.0, 2.0, kUnsetDecimal, false},
    {"defaults.sampling.repetition_penalty", T::decimal, false, 0, 0, 0, false, "", "",
     "Request default repetition penalty (pin wire: only != 1.0 is sent; cap 2 added, C-1).", 0.0, 2.0, kUnsetDecimal, true},
    {"defaults.sampling.frequency_penalty", T::decimal, false, 0, 0, 0, false, "", "",
     "Request default frequency penalty (0 off; cap 2 added, C-1).", 0.0, 2.0, kUnsetDecimal, false},
    {"defaults.sampling.penalty_last_n", T::integer, false, 0, 8192, -1, false, "", "",
     "Penalty window; unset means the pin default of 64 when any penalty is on (cap added, C-1).", 0.0, 0.0, kUnsetDecimal, false},
    {"defaults.sampling.seed", T::integer, false, 1, 4294967295, -1, false, "", "",
     "Request default sampling seed (pin: a positive integer; 32-bit cap added, C-1).", 0.0, 0.0, kUnsetDecimal, false},
    // SP5 M1 Task 5 (parity rows reasoning_* / fit_max_tokens): kinds and defaults copied from the
    // pin — effort clean_shared_defaults serve/server.py:7341-7343; budget_tokens :8120-8127 and
    // reasoning_budget :3794-3807 (a whole number, 0 = no budget; the 0..4294967295 bounds are ADDED
    // security bounds, C-1); close_retry :8015; loop_recovery :8129-8136 (the pin also takes bools,
    // the typed row names the three words); effort_position :2841-2843; anthropic_thinking
    // :8102-8104; fit_max_tokens :3485-3488 read at :7996 (false answers 400 over the room, :4536).
    {"reasoning.effort", T::string, false, 0, 0, 0, false, "", "",
     "Thinking level for requests that name none: none, low, medium or high (pin levels); unset is the template's default, high. Qwen packs with <think>: none closes the think block; low and high write the template's effort sentence, medium none."},
    {"reasoning.budget_tokens", T::integer, false, 0, 4294967295, 0, false, "", "",
     "Default thinking budget in tokens (0 = none, the pin default); bounds added, C-1."},
    {"reasoning.close_retry", T::boolean, false, 0, 0, 0, false, "", "",
     "Close an unfinished thinking block once when the reply ends inside it (pin opt-in, off)."},
    {"reasoning.loop_recovery", T::string, false, 0, 0, 0, false, "off", "",
     "A reasoning loop ends the reply (off, the pin default), stops (stop) or continues once (recover)."},
    {"reasoning.effort_position", T::string, false, 0, 0, 0, false, "start", "",
     "Where a non-default effort sits in the prompt: start (the pin default) or end; end adds the engine's --tail-role-token."},
    {"reasoning.anthropic_thinking", T::string, false, 0, 0, 0, false, "model", "",
     "Thinking default for /v1/messages-shaped clients: model (the pin default) or on_request."},
    {"engine.fit_max_tokens", T::boolean, false, 0, 0, 0, true, "", "",
     "Fit an over-room max_tokens to the context room (true, the pin default); false answers the §7.2 400 instead."},
    // SP5 M1 Task 6 (parity rows lazy_load / idle_unload_s / mtp): runconfig.py:38 "Start
    // without loading the model; the first request loads it (text only)"; :35 "0 or empty:
    // never" (whole seconds; the 86400 cap is ADDED, C-1); :36 + strata_control/
    // draft_layer.py is_off: the draft layer is on unless an explicit false.
    {"engine.lazy_load", T::boolean, false, 0, 0, 0, false, "", "",
     "Start with the engine unloaded; the first request loads it (the pin's lazy_load, text only)."},
    {"engine.idle_unload_s", T::integer, false, 0, 86400, 0, false, "", "",
     "Seconds without a request before the engine is unloaded (0: never, the pin default); cap added, C-1."},
    {"engine.mtp", T::boolean, false, 0, 0, 0, true, "", "",
     "Draft layer (MTP): on unless explicitly false; false drops --mtp/--spec-min-p/--batch-mtp at every start."},
    // SP5 M1 Task 7 (the VRAM / GPU / elastic family). Kinds and defaults verbatim from the
    // pin; caps and closed vocabularies added (C-1). vram_reserve_mib: runconfig.py:42-43
    // renders --vram-reserve-mib (engine default 700, unset composes nothing). min_free_vram_mib:
    // int-or 0 svc-side gate (serve/server.py:8083-8084). vram_elastic/vram_segment_mib:
    // #533 (:2943-2948, engine segment default 512). elastic/elastic_mib/elastic_idle_s:
    // the vision turn (server.py:2548-2552, defaults 2048 MiB / 120 s; Task 11 consumes them).
    // gpu_order: only "as_given" is read (:3105-3112). gpu_policy: images.py POLICIES
    // (:45, :173-175). layer_split: "auto" or rising first-layer numbers (:2870-2894).
    // split_skip_if_fits: :2940-2942. hip_ordinal: :3016-3030. uma: images.py:23/:334.
    {"engine.vram_reserve_mib", T::integer, false, 0, 1048576, -1, false, "", "",
     "VRAM MiB the engine leaves free (renders --vram-reserve-mib; unset: the engine's own 700)."},
    {"engine.min_free_vram_mib", T::integer, false, 0, 1048576, 0, false, "", "",
     "Refuse to load when free VRAM is below this (0: never; the driver check runs on the owner's GPU box)."},
    {"engine.vram_elastic", T::boolean, false, 0, 0, 0, false, "", "",
     "Segment the expert cache so VRAM can be given back and taken back (renders --vram-elastic, #533)."},
    {"engine.vram_segment_mib", T::integer, false, 1, 1048576, -1, false, "", "",
     "Segment size for vram_elastic (renders --vram-segment-mib; unset: the engine's own 512)."},
    {"engine.elastic", T::boolean, false, 0, 0, 0, false, "", "",
     "The image encoder starts with the first picture and stops when idle (needs vram_elastic; the vision turn)."},
    {"engine.elastic_idle_s", T::integer, false, 1, 86400, 120, false, "", "",
     "Seconds without a picture before the elastic encoder stops (pin default 120); whole seconds, cap added."},
    {"engine.elastic_mib", T::integer, false, 1, 1048576, 2048, false, "", "",
     "VRAM MiB the expert cache keeps free while the encoder runs (pin default 2048)."},
    {"engine.gpu_order", T::string, false, 0, 0, 0, false, "auto", "",
     "GPU order: auto (reorder by card speed, where the driver can tell) or as_given (keep the configured order)."},
    {"engine.gpu_policy", T::string, false, 0, 0, 0, false, "auto", "",
     "GPU turn for a picture: auto, keep, shrink or unload the engine's VRAM first (the POLICIES set)."},
    {"engine.layer_split", T::string, false, 0, 0, 0, false, "auto", "",
     "First layer of each later GPU (rising numbers from 2), or auto; renders --layer-split with several GPUs."},
    {"engine.split_skip_if_fits", T::boolean, false, 0, 0, 0, false, "", "",
     "Let an auto split run on the first card alone when it holds every expert and the KV (renders --split-skip-if-fits)."},
    {"engine.hip_ordinal", T::integer, false, 0, 127, -1, false, "", "",
     "The HIP number of the discrete card (setup-recorded); wins over one (or zero) configured GPUs."},
    {"engine.uma", T::boolean, false, 0, 0, 0, false, "", "",
     "An APU: engine and image turn share the RAM either way, so a picture never needs the engine moved."},
    // SP5 M1 Task 8 (the timeout / queue / request-shape family). engine_silence_s:
    // runconfig.py:39 "num>=0 ... default 300 s, 0 = wait" (cap added, C-1); the watchdog
    // is serve/server.py:274-276/:2861-2868. answer_room: :3490/:8014 `is not False`
    // (default true). repeat_stop_tokens: :8115-8118 whole number >= 0, default
    // REPEAT_STOP_TOKENS=256 (:197; 0 = off; cap added, C-1). max/min tokens: the pin's
    // keys bound the vision encoder (:2564-2567); the typed rows re-scope to request
    // output bounds (added, C-1; Ruling T8-e). model.aliases: runconfig.py:34 +
    // set_aliases :3764-3777.
    {"engine.silence_s", T::integer, false, 0, 86400, 300, false, "", "",
     "End a request when the engine says nothing valid for this long (pin default 300 s; 0: wait, still bounded by the total generation timeout)."},
    {"engine.answer_room", T::boolean, false, 0, 0, 0, true, "", "",
     "Close a long un-budgeted thinking reply early enough to leave the answer room (true, the pin default)."},
    {"engine.repeat_stop_tokens", T::integer, false, 0, 65536, 256, false, "", "",
     "One token this many times in a row ends a reply as length (pin default 256; 0: off). Cap added, C-1."},
    {"engine.max_tokens", T::integer, false, 1, 1048576, -1, false, "", "",
     "Ceiling on a request's output budget (unset: the completion cap; an added frontend bound, C-1)."},
    {"engine.min_tokens", T::integer, false, 1, 1048576, -1, false, "", "",
     "Floor on a request's output budget (unset: none; an added frontend bound, C-1)."},
    {"model.aliases", T::string_array, false, 0, 0, 0, false, "", "",
     "Other model names the server lists and answers to (exact match only, like the pin's set_aliases)."},
    {"model.name", T::string, false, 0, 0, 0, false, "", "",
     "The name the server serves the model as: /v1/models lists it, the chat page shows it, requests name it (like the pin's model_name). Unset: the model's own id (its inventory file name, or name:tag)."},
    // SP5 M1 Task 9 (the session family). slot_save_path: the pin's /slots file directory
    // (:8092-8100 + slot_save_dir :3469-3481) — typed with our absolute rule; the route
    // itself is not in our spec (Rulings T9-a/b). expert_profile_save / -every: the pin
    // appends both engine flags when the path is set (:3000-3005; docstring default 10
    // minutes, 0 = at QUIT only, :2996; added cap C-1). The four booleans are the pin's
    // opt-in falses (:8043-:8044 `is True`, :8106/:8110 strict bool) whose pin-side
    // consumers (OutputParser recovery, frontend._env_to_user, the Codex routes) we do not
    // serve (config_only). keep_files: the pin's ImageService scratch retention
    // (images.py:188 `or 64`); typed with added bounds, D-9 keeps image generation out.
    {"session.slot_save_path", T::string, false, 0, 0, 0, false, "", "",
     "Directory for engine slot session save/restore files (absolute; the /slots route is not served today)."},
    {"session.expert_profile_save", T::string, false, 0, 0, 0, false, "", "",
     "Where the engine re-saves its learned expert profile (absolute path; --expert-profile-save)."},
    {"session.expert_profile_save_every", T::integer, false, 0, 1440, 10, false, "", "",
     "Re-save the learned expert profile every N minutes (pin default 10; 0: at engine exit only). Cap added, C-1."},
    {"session.tool_call_recovery", T::boolean, false, 0, 0, 0, false, "", "",
     "Attempt to recover malformed tool-call output (the pin's opt-in; stored for parity today)."},
    {"session.env_to_user", T::boolean, false, 0, 0, 0, false, "", "",
     "Pass configured environment hints to the user prompt path (the pin's opt-in; stored for parity today)."},
    {"session.codex_thread_titles", T::boolean, false, 0, 0, 0, false, "", "",
     "Codex thread titles (#923; the pin's opt-in; the route is not served here)."},
    {"session.codex_compaction_cache", T::boolean, false, 0, 0, 0, false, "", "",
     "Codex compaction cache (#924; the pin's opt-in; the route is not served here)."},
    {"session.keep_files", T::integer, false, 1, 1024, 64, false, "", "",
     "How many image-scratch files to retain (pin default 64; bounds added, C-1; the consumer is out per D-9)."},
    // SP5 M1 Task 10 (the C-4 narrowing): engine-internal parallelism only — the pin renders
    // this as --batch N (serve/server.py:2975); 2..8 is the engine's batch window
    // (PARALLEL_MAX :2954) and larger values ride on for the engine to adjust (:2969-2974).
    // The cap 1024 is added (C-1), taken from the pin's own --parallel bound (:7845).
    {"replicas.parallel", T::integer, false, 1, 1024, 1, false, "", "",
     "Decode up to N requests together inside the one engine (1: one at a time; the N-child scheduler is not implemented)."},
    // SP5 M1 Task 11 (D-9 vision split): image INPUT is typed and plumbed, generation stays
    // out. Defaults at the pin — max_side/max_n carry the images-block values (images.py:102-
    // 103 `or 2048`/`or 4`; caps added, C-1); need_mib overrides the pin's VRAM arithmetic
    // (images.py:156; the driver gate itself is owner-box, cf. T7-c); no_flash_attn rides the
    // spec §5.1 family (not read by this pin — the kit proves the child flag); resident is
    // the pin's eager encoder (server.py:2511/:2552; false: the first image wakes it);
    // threads 0 = the pin's unset (truthy -> --threads, server.py:2562).
    {"vision.enabled", T::boolean, false, 0, 0, 0, false, "", "",
     "Read images: a supervised maya-vision child encodes image_url parts for the engine."},
    {"vision.max_side", T::integer, false, 1, 16384, 2048, false, "", "",
     "Largest image side accepted before decoding (pin default 2048; cap added, C-1)."},
    {"vision.max_n", T::integer, false, 1, 64, 4, false, "", "",
     "Most images one request may carry (pin default 4; cap added, C-1)."},
    {"vision.need_mib", T::integer, false, 1, 1048576, -1, false, "", "",
     "Override the free-VRAM figure the encoder is checked against (unset: computed; bounds added, C-1)."},
    {"vision.no_flash_attn", T::boolean, false, 0, 0, 0, false, "", "",
     "Run the encoder without flash attention (added, spec §5.1; the encoder's --no-flash-attn)."},
    {"vision.resident", T::boolean, false, 0, 0, 0, true, "", "",
     "Keep the encoder running from boot (the pin's eager resident encoder; false: the first image starts it)."},
    {"vision.threads", T::integer, false, 0, 256, 0, false, "", "",
     "Encoder CPU threads (0: the tool's own default; cap added, C-1)."},
    // The STRATA_VISION_READY_S counterpart the Task-2 parity row promises (its gate
    // default 300 s, server.py:327; 0 = wait for ever, :291 — the shape engine.silence_s
    // took in Task 8; bound added, C-1).
    {"vision.ready_timeout_s", T::integer, false, 0, 86400, 300, false, "", "",
     "Seconds the encoder gets to print READY (0: wait for ever, as at the pin)."},
    // The STRATA_VISION_ENCODE_S counterpart promised by the Task-2 parity row
    // (gate default 300 s, server.py:328; 0 = wait for ever, :291; bound added, C-1).
    {"vision.encode_timeout_s", T::integer, false, 0, 86400, 300, false, "", "",
     "Seconds one image encoding may take (0: wait for ever, as at the pin)."},
    // Vision for model cards (spec 2026-10-28-vision-for-cards-design.md): the card's own encoder file and where it runs.
    {"vision.mmproj", T::string, false, 0, 0, 0, false, "", "",
     "Absolute path of the picture encoder (an mmproj GGUF) a model card's engine reads images with; a card row."},
    {"vision.gpu", T::boolean, false, 0, 0, 0, false, "", "",
     "Encode pictures on the GPU (the encoder's --gpu; off: the CPU); a card row."},
    // SP5 M2 Task 3 (spec §D-2; C-3 keeps Maya's names): the engine-side GLM
    // knobs. Kinds and defaults are Maya v1.1.21's verbatim (parity table rows
    // cite the vendored pin); the caps and closed sets are added (C-1). The
    // rows are accepted while engine.profile is absent (a managed model's
    // SP4 manifest decides the profile; the launch, Task 7, refuses them for
    // a qwen manifest) and refused by name when engine.profile is "qwen".
    {"glm.thinking_budget", T::integer, false, 0, 1000000000, 32768, false, "", "",
     "Thinking-token budget when a request names none (Maya's default 32768; 0 disables the budget)."},
    {"glm.reasoning_effort", T::string, false, 0, 0, 0, false, "", "",
     "Thinking level when a request names none: none, low, high or max (Maya's canonical levels; Maya's aliases such as \"medium\" are not accepted here)."},
    {"glm.fit_max_tokens", T::boolean, false, 0, 0, 0, false, "", "",
     "Fit a request's output budget to the context room on the GLM side (Maya's default: false)."},
    {"glm.experimental_speed_projection", T::boolean, false, 0, 0, 0, false, "", "",
     "GLM speed projection (Maya's default: false)."},
    {"glm.gpu", T::string, false, 0, 0, 0, false, "", "",
     "GPU device indices for the GLM engine, e.g. 0 or 0,1 (distinct, 0-255, at most 16)."},
    {"glm.layer_split", T::string, false, 0, 0, 0, false, "auto", "",
     "First layer of each later GPU's share (rising numbers, e.g. 18 or 16,32), or auto; the GLM engine's --layer-split."},
    {"glm.tokenizer", T::string, false, 0, 0, 0, false, "", "",
     "Absolute tokenizer-pack path for the GLM engine (default: the pack named by the model)."},
    {"glm.model_name", T::string, false, 0, 0, 0, false, "", "",
     "Served model id for the GLM engine (1-256 characters; a managed model takes its identity from the manifest)."},
    // SP5 M2 Task 7 (D-8): the launcher-managed --max-context becomes typed. The
    // default is Maya's DEFAULT_CONTEXT (maya.py:3148); the 1-1048576 bound is
    // added (C-1). Composition lands in engine_arguments (the Task-2 mapped-away
    // --max-context row's destination).
    {"glm.max_context", T::integer, false, 1, 1048576, 32768, false, "", "",
     "GLM context length in tokens the engine is started with (Maya's default 32768)."},
    // the composed GLM launch's --kv, the GLM engine's own closed vocabulary (glm_driver.cpp): fp16 its default
    // (rendered as nothing), int8 glm5-next's INT8 latent, q8_0 / q4_0 raon1's quantized latent cache
    {"glm.kv", T::string, false, 0, 0, 0, false, "fp16", "",
     "KV cache precision the GLM engine starts with: fp16 (default; raon1 keeps its exact F32 cache), int8, or q8_0 / "
     "q4_0 (raon1's quantized cache)."},
    // SP5-P2 Task 4 (D-7; SP6 §4.9 sheet M2): MAYA's RAM headroom gains a typed
    // value source. Integer 6-1024, default 16: MAYA's own default is 6
    // (glm_fast_path.cu:1110/:1510) while SETTINGS.md:28 records 16 as the value a
    // desktop stayed responsive at; the row projects at its default onto the
    // closed child-env allowlist (glm_environment_knobs).
    {"glm.ram_headroom_gb", T::integer, false, 6, 1024, 16, false, "", "",
     "Gigabytes of RAM the GLM engine leaves free when sizing its pinned tier (6-1024; default 16)."},
    // Calibration (owner 2026-10-28, Maya's tools/calibrate_glm.py): the GLM engine's CPU lane, which it reads from its
    // environment (glm_fast_path.cu) - the share of a route's RAM-tier experts copied over PCIe, and the lane's
    // threads (the engine caps them at 64). Unset: the engine's own choice. A card's calibration sets them at its start.
    {"glm.pcie_share", T::decimal, false, 0, 0, 0, false, "", "",
     "Share (0-1) of a route's RAM-tier experts the GLM engine copies over PCIe instead of computing them on its CPU lane "
     "(STRATA_GLM_PCIE_SHARE); unset: the engine times one expert each way and splits by that.", 0.0, 1.0, kUnsetDecimal, false},
    {"glm.cpu_lane", T::integer, false, 1, 64, -1, false, "", "",
     "CPU threads of the GLM engine's CPU lane (1-64, STRATA_GLM_CPU_LANE); unset: the physical cores."},
    // SP5-P2 Task 4 (D-7): the Maya v1.1.21 server gained the idle-unload timer —
    // the config's "idle_unload_s" (serve/server.py:8298; default 0 = never,
    // :3753). It wins over engine.idle_unload_s for a glm53 profile; the 86400
    // cap is added, same as the M1 key (C-1).
    {"glm.idle_unload_s", T::integer, false, 0, 86400, 0, false, "", "",
     "Seconds a GLM engine idles unloaded before it gives back the memory (0 = never; under glm53 this wins over engine.idle_unload_s)."},
    // Model launcher (plan 2026-10-23 Task 1; spec §3.1, D1/D2/D5): the model cards the page may
    // start. With models.cards set, each card file holds its model's engine, weights, tokenizer
    // and page options, and the main config holds the server.
    {"models.cards", T::string_array, false, 0, 0, 0, false, "", "",
     "Absolute paths of model card files (at most 16): the page's Models tab starts and stops them. Each card holds its model's engine.*, model.aliases, roots.models, roots.tokenizer_pack, defaults.sampling.*, reasoning.* and limits.chat token rows, which the main config then may not set."},
    {"models.start", T::string, false, 0, 0, 0, false, "", "",
     "The card (by name) started at boot; unset: the server boots with no engine and waits for the page. Needs models.cards."},
    {"models.control", T::boolean, false, 0, 0, 0, false, "", "",
     "The page and API may start and stop model cards (keyed; loopback clients only unless models.control_remote). Needs models.cards."},
    {"models.control_remote", T::boolean, false, 0, 0, 0, false, "", "",
     "Model control also from non-loopback clients. Needs models.control."},
    {"models.control_remote_networks", T::string_array, false, 0, 0, 0, false, "", "",
     "With models.control_remote: the hosts (an IP address) and subnets (CIDR, e.g. 172.16.1.0/24) other machines may control models from; empty: every client with the key. Listed clients may also use the page's model-file actions (this PC's folders, add a model, a card's folder, delete), which otherwise stay this-PC-only. IPv4 or IPv6, never a name; at most 16."},
    // Model downloads (plan 2026-10-23-model-downloads; owner rule O4: Hugging Face and .gguf only).
    {"models.downloads", T::boolean, false, 0, 0, 0, false, "", "",
     "The Models tab's downloads: Hugging Face .gguf files only, fetched by strata pull as a separate confined process (the server itself has no network code). Keyed; loopback clients only unless models.control_remote. Needs models.cards and models.downloads_root."},
    {"models.downloads_root", T::string, false, 0, 0, 0, false, "", "",
     "Absolute, existing owner-only directory the downloads are installed in (strata's model store layout); strata's own verbs use it as roots.models with a models.cards config."},
    {"models.prepare_python", T::string, false, 0, 0, 0, false, "", "",
     "Absolute path of the Python that prepares a downloaded Qwen model (upstream's pack tools; they need numpy and regex). Unset: Qwen downloads stay files only. Needs models.prepare_tools."},
    {"models.prepare_tools", T::string, false, 0, 0, 0, false, "", "",
     "Absolute directory of upstream Strata's tools (iq_pack.py, strata_tokenizer.py, gguf_reader.py), read only. Needs models.prepare_python."},
    {"models.download_engine_card", T::string, false, 0, 0, 0, false, "", "",
     "The qwen card whose engine (executable, library folders, GPU, options) a prepared download's card uses."},
    // GUI-first start-up (docs/superpowers/specs/2026-10-27-gui-first-install-design.md §3-§4).
    {"models.default_engine", T::string, false, 0, 0, 0, false, "", "",
     "Absolute path of the engine (multiengine) the cards the page writes use; with it the server also serves with no card yet. The first start sets it to the engine beside the server."},
    {"models.default_root", T::string, false, 0, 0, 0, false, "", "",
     "Absolute folder the page offers first for a download or a model's files (the first start's data/models)."},
    // GUI-first start-up (spec 2026-10-27 §5a): email alerts, read only in a build with SECURE_FRONTEND_EMAIL_ALERTS.
    {"alerts.email.enabled", T::boolean, false, 0, 0, 0, false, "", "",
     "Send email alerts (a build with SECURE_FRONTEND_EMAIL_ALERTS only; any other build refuses alerts.*). The password is kept in roots.secret (smtp-password), never in this file."},
    {"alerts.email.host", T::string, false, 0, 0, 0, false, "", "",
     "The SMTP server, a DNS name (IONOS: smtp.ionos.com, or the account's regional host). Needed with alerts.email.enabled."},
    {"alerts.email.port", T::integer, false, 1, 65535, 465, false, "", "",
     "The SMTP port: 465 with ssl (IONOS's primary), 587 with starttls."},
    {"alerts.email.security", T::string, false, 0, 0, 0, false, "ssl", "",
     "ssl (TLS from the first byte) or starttls (STARTTLS before AUTH); never plain text; TLS 1.2 or later."},
    {"alerts.email.username", T::string, false, 0, 0, 0, false, "", "",
     "The SMTP user name (IONOS: the full mailbox address). Needed with alerts.email.enabled."},
    {"alerts.email.from", T::string, false, 0, 0, 0, false, "", "",
     "The From address (IONOS: the mailbox the user name logs in to). Needed with alerts.email.enabled."},
    {"alerts.email.to", T::string_array, false, 0, 0, 0, false, "", "",
     "1-5 addresses the alerts go to. Needed with alerts.email.enabled."},
    {"alerts.email.events", T::string_array, false, 0, 0, 0, false, "", "",
     "Which alerts are sent: download_finished, download_failed, calibration_finished, calibration_failed, engine_stopped, server_started (absent: all)."},
    // Web search (docs/superpowers/specs/2026-10-27-web-search-design.md §2), read only in a build with SECURE_FRONTEND_WEB_SEARCH.
    {"search.enabled", T::boolean, false, 0, 0, 0, false, "", "",
     "Let the model search the web through a SearXNG instance when it is unsure or the answer needs recent information (a build with SECURE_FRONTEND_WEB_SEARCH only; any other build refuses search.*). The searches leave this PC: what the model searches for comes from the chat."},
    {"search.url", T::string, false, 0, 0, 0, false, "", "",
     "The SearXNG instance, http(s)://host[:port][/path], with the JSON format on (its settings.yml: search.formats lists json). Needed with search.enabled."},
    {"search.results", T::integer, false, 1, 10, 5, false, "", "", "Results the model gets per search (1-10)."},
    {"search.max_rounds", T::integer, false, 1, 8, 8, false, "", "",
     "Searches one answer may make (upstream's mcp.max_rounds, 1-8)."},
    {"search.timeout_s", T::integer, false, 5, 120, 30, false, "", "",
     "Seconds each step of a search may take: the connection, the request, each read of the reply (5-120)."},
}};

void add(ConfigErrors& errors, std::string key, const ConfigIssueCode code, std::string reason) {
  errors.push_back(ConfigIssue{std::move(key), code, std::move(reason)});
}

// Reads one key with its schema type, range and default; records issues.
class DraftReader final {
 public:
  DraftReader(const FrontendConfigDraft& draft, ConfigErrors& errors) : draft_(draft), errors_(errors) {}

  std::int64_t integer(const std::string_view path) {
    const ConfigKey& key = *find_config_key(path);
    const auto* entry = lookup(key);
    if (entry == nullptr) return key.default_integer;
    const auto* value = std::get_if<std::int64_t>(&entry->value);
    if (value == nullptr) return wrong_type(key), key.default_integer;
    if (*value < key.minimum || *value > key.maximum) {
      add(errors_, std::string{path}, ConfigIssueCode::out_of_range,
          "must be between " + std::to_string(key.minimum) + " and " + std::to_string(key.maximum));
      return key.default_integer;
    }
    return *value;
  }
  bool boolean(const std::string_view path) {
    const ConfigKey& key = *find_config_key(path);
    const auto* entry = lookup(key);
    if (entry == nullptr) return key.default_boolean;
    const auto* value = std::get_if<bool>(&entry->value);
    if (value == nullptr) return wrong_type(key), key.default_boolean;
    return *value;
  }
  std::optional<std::string> string(const std::string_view path) {
    const ConfigKey& key = *find_config_key(path);
    const auto* entry = lookup(key);
    if (entry == nullptr) {
      if (key.default_string.empty()) return std::nullopt;
      return std::string{key.default_string};
    }
    const auto* value = std::get_if<std::string>(&entry->value);
    if (value == nullptr) return wrong_type(key), std::nullopt;
    return *value;
  }
  std::vector<std::string> strings(const std::string_view path) {
    const ConfigKey& key = *find_config_key(path);
    const auto* entry = lookup(key);
    if (entry == nullptr) return {};
    const auto* value = std::get_if<std::vector<std::string>>(&entry->value);
    if (value == nullptr) return wrong_type(key), std::vector<std::string>{};
    return *value;
  }
  // SP5 M1 Task 4: an optional integer row (default_integer outside the range
  // = unset); range-checked when present.
  std::optional<std::int64_t> optional_integer(const std::string_view path) {
    const ConfigKey& key = *find_config_key(path);
    const auto* entry = lookup(key);
    if (entry == nullptr) return std::nullopt;
    const auto* value = std::get_if<std::int64_t>(&entry->value);
    if (value == nullptr) return wrong_type(key), std::nullopt;
    if (*value < key.minimum || *value > key.maximum) {
      add(errors_, std::string{path}, ConfigIssueCode::out_of_range,
          "must be between " + std::to_string(key.minimum) + " and " + std::to_string(key.maximum));
      return std::nullopt;
    }
    return *value;
  }
  // SP5 M1 Task 4: a decimal row; integers are accepted (JSON "1" is a valid
  // number); the NaN default means "unset".
  std::optional<double> decimal(const std::string_view path) {
    const ConfigKey& key = *find_config_key(path);
    const auto* entry = lookup(key);
    if (entry == nullptr) {
      if (std::isnan(key.default_decimal)) return std::nullopt;
      return key.default_decimal;
    }
    double value = 0.0;
    if (const auto* i = std::get_if<std::int64_t>(&entry->value)) value = static_cast<double>(*i);
    else if (const auto* dp = std::get_if<double>(&entry->value)) value = *dp;
    else return wrong_type(key), std::nullopt;
    if (value < key.minimum_decimal || value > key.maximum_decimal ||
        (key.minimum_exclusive && value == key.minimum_decimal)) {
      add(errors_, std::string{path}, ConfigIssueCode::out_of_range,
          (key.minimum_exclusive ? "must be greater than " : "must be at least ") + decimal_text(key.minimum_decimal) +
          (key.minimum_exclusive ? " and at most " : " and at most ") + decimal_text(key.maximum_decimal));
      return std::nullopt;
    }
    return value;
  }
  bool present(const std::string_view path) const { return draft_.entries.find(path) != draft_.entries.end(); }

 private:
  const ConfigDraftEntry* lookup(const ConfigKey& key) {
    const auto it = draft_.entries.find(key.path);
    if (it != draft_.entries.end()) return &it->second;
    if (key.required) add(errors_, std::string{key.path}, ConfigIssueCode::missing_required, "is required");
    return nullptr;
  }
  void wrong_type(const ConfigKey& key) {
    add(errors_, std::string{key.path}, ConfigIssueCode::wrong_type, std::string{type_reason(key.type)});
  }
  const FrontendConfigDraft& draft_;
  ConfigErrors& errors_;
};

std::optional<CanonicalRoot> canonical_root(DraftReader& read, ConfigErrors& errors, const std::string_view path,
                                            RootPolicy policy = RootPolicy::existing) {
  const auto text = read.string(path);
  if (!text.has_value()) return std::nullopt;
  auto opened = open_root(path_from_utf8(*text), policy);
  if (auto* error = std::get_if<PathError>(&opened)) {
    add(errors, std::string{path}, ConfigIssueCode::invalid_value, error->message);
    return std::nullopt;
  }
  return std::get<CanonicalRoot>(std::move(opened));
}

// roots.logs / roots.cache: checked, never created, during validation.
std::optional<std::filesystem::path> creatable_root(DraftReader& read, ConfigErrors& errors, const std::string_view path) {
  const auto text = read.string(path);
  if (!text.has_value()) return std::nullopt;
  auto checked = check_creatable_root(path_from_utf8(*text));
  if (auto* error = std::get_if<PathError>(&checked)) {
    add(errors, std::string{path}, ConfigIssueCode::invalid_value, error->message);
    return std::nullopt;
  }
  return std::get<std::filesystem::path>(std::move(checked));
}

// roots.state: absolute, non-empty, no NUL. No filesystem access here, so
// --check stays side-effect free; serve checks it is an existing owner-only
// directory before anything is spawned.
std::optional<std::filesystem::path> absolute_path(DraftReader& read, ConfigErrors& errors, const std::string_view path) {
  const auto text = read.string(path);
  if (!text.has_value()) return std::nullopt;
  if (text->empty() || text->find('\0') != std::string::npos || !path_from_utf8(*text).is_absolute()) {
    add(errors, std::string{path}, ConfigIssueCode::invalid_value, "must be an absolute directory");
    return std::nullopt;
  }
  return path_from_utf8(*text);
}

std::optional<SecretSource> token_source(const std::string& text) {
  if (text.rfind("env:", 0) == 0) {
    auto result = make_environment_secret_source(std::string_view{text}.substr(4));
    if (auto* source = std::get_if<SecretSource>(&result)) return std::move(*source);
    return std::nullopt;
  }
  if (text.rfind("file:", 0) == 0) {
    auto result = make_protected_file_secret_source(std::string_view{text}.substr(5));
    if (auto* source = std::get_if<SecretSource>(&result)) return std::move(*source);
  }
  return std::nullopt;
}

std::string json_quoted(const std::string_view text) {
  std::string out;
  append_json_string(&out, text);
  return out;
}

}  // namespace

std::filesystem::path default_downloader_path() {
#if defined(_WIN32)
  // Pre-flight F-32: a failure or a directory longer than the buffer never
  // yields the relative "curl.exe"; the buffer grows to the reported size once.
  std::wstring system(MAX_PATH, L'\0');
  for (int attempt = 0; attempt < 2; ++attempt) {
    const UINT length = GetSystemDirectoryW(system.data(), static_cast<UINT>(system.size()));
    if (length == 0) return {};
    if (length < system.size()) {
      system.resize(length);
      const std::filesystem::path directory(system);
      if (!directory.is_absolute()) return {};
      return directory / L"curl.exe";
    }
    system.assign(length, L'\0');  // length includes the terminator when the buffer was too small
  }
  return {};
#else
  return std::filesystem::path{"/usr/bin/curl"};
#endif
}

std::span<const ConfigKey> config_schema() noexcept { return kSchema; }

bool is_valid_email_address(const std::string_view text) noexcept {
  constexpr std::string_view kSpecials = "<>()[],;:\\\"";
  if (text.empty() || text.size() > 254U) return false;
  if (!std::all_of(text.begin(), text.end(), [&](const char c) { return c > 0x20 && c < 0x7f && kSpecials.find(c) == std::string_view::npos; })) {
    return false;
  }
  const auto at = text.find('@');
  if (at == std::string_view::npos || at == 0U || at > 64U || text.find('@', at + 1U) != std::string_view::npos) return false;
  const auto domain = text.substr(at + 1U);
  return !domain.empty() && domain.find('.') != std::string_view::npos && domain.front() != '.' && domain.back() != '.' &&
         domain.find("..") == std::string_view::npos &&
         std::all_of(domain.begin(), domain.end(), [](const char c) {
           return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-';
         });
}

const ConfigKey* find_config_key(const std::string_view path) noexcept {
  for (const auto& key : kSchema) if (key.path == path) return &key;
  return nullptr;
}

namespace {

// SP5 M1 Task 8 (parity row aliases): the entry grammar is SP1's one model-ID grammar
// (validate_model_id) with an added 64-character cap (C-1); aliases join /v1/models and
// must stay inside the JSON model-field bounds. set_aliases (:3764-3777) drops empties
// and duplicates at use; the typed row rejects an empty entry outright.
bool is_valid_model_alias(const std::string_view value) noexcept {
  return !value.empty() && value.size() <= 64U && std::holds_alternative<std::string>(validate_model_id(value));
}

// SP5 M1 Task 9 (Ruling T9-b): the pin's slot_save_dir shape — non-empty, no control
// characters (serve/server.py:3472-3475) — plus our added absolute rule (C-1, like
// engine.lib_dirs): there is no config-cwd to resolve a relative path against, and
// validation never touches the filesystem.
bool is_absolute_store_path(const std::string& text) noexcept {
  return !text.empty() && text.find('\0') == std::string::npos &&
         std::none_of(text.begin(), text.end(), [](const char c) {
           return static_cast<unsigned char>(c) < 0x20U || c == 0x7f;
         }) &&
         path_from_utf8(text).is_absolute();
}

// SP7 M2 Task 3 (D-10): the config grammar for update.source entries. Mirrors
// parse_source_url's constraints (HTTPS, host + path, no port, no userinfo,
// no query/fragment, printable and unambiguous); the verb re-parses with
// parse_source_url as the authority at use time.
bool is_valid_release_source_url(std::string_view text) noexcept {
  if (!text.starts_with("https://") || text.size() > 512) return false;
  const std::string_view rest = text.substr(8);
  if (rest.empty()) return false;
  for (const char c : rest) {
    const auto u = static_cast<unsigned char>(c);
    if (u < 0x21U || u > 0x7EU) return false;
    if (c == '@' || c == '?' || c == '#' || c == '%' || c == '[' || c == ']') return false;
  }
  const std::size_t slash = rest.find('/');
  if (slash == std::string_view::npos || slash == 0) return false;
  const std::string_view host = rest.substr(0, slash);
  for (const char c : host) {
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-')) return false;
  }
  return !host.empty() && host.front() != '-' && host.back() != '-' && host.find("..") == std::string_view::npos;
}

// Model launcher: a card file is named by an absolute path (no config-cwd to resolve against;
// load never touches the filesystem, the launcher reads the cards at boot).
bool is_absolute_card_file(const std::string_view text) noexcept {
  return !text.empty() && std::none_of(text.begin(), text.end(), [](const char c) {
           return static_cast<unsigned char>(c) < 0x20U || c == 0x7f;
         }) &&
         path_from_utf8(std::string{text}).is_absolute();
}

bool is_alert_event(const std::string_view text) noexcept {
  return std::find(kAlertEvents.begin(), kAlertEvents.end(), text) != kAlertEvents.end();
}

bool is_ip_network(const std::string_view text) noexcept { return parse_ip_network(text).has_value(); }

constexpr std::array<ConfigKeyExtension, 10> kSchemaExtensions{{
    {"models.cards", 16U, &is_absolute_card_file, "each entry must be an absolute path of a card file", ""},
    {"models.control_remote_networks", 16U, &is_ip_network,
     "each entry must be an IP address or a subnet such as 172.16.1.0/24 (no host bits set); names are never resolved", ""},
    {"alerts.email.to", 5U, &is_valid_email_address, "each entry must be an email address (at most 5)", ""},
    {"alerts.email.events", kAlertEvents.size(), &is_alert_event,
     "each entry must be one of download_finished, download_failed, calibration_finished, calibration_failed, engine_stopped, server_started", ""},
    {"listener.cors_origins", static_cast<std::size_t>(config_defaults::kMaxListEntries), &is_valid_cors_origin,
     "each entry must be http:// or https://, a lower-case host, an optional port and no path; * and null are not allowed", ""},
    {"update.source", static_cast<std::size_t>(config_defaults::kMaxListEntries), &is_valid_release_source_url,
     "each entry must be an HTTPS release URL: https://host/path, no port, userinfo, query or fragment", ""},
    {"listener.host_aliases", static_cast<std::size_t>(config_defaults::kMaxListEntries), &is_valid_host_alias,
     "each entry must be a host or host:port", ""},
    // SP5 M1 Task 8 (parity row aliases): the entry grammar is SP1's one model-ID grammar
    // (validate_model_id) with an added 64-character cap (C-1). set_aliases (:3764-3777)
    // strips and drops empties and duplicates at use; the typed row rejects outright.
    {"model.aliases", static_cast<std::size_t>(config_defaults::kMaxListEntries), &is_valid_model_alias,
     "each entry must be a model name: ASCII letters, digits, dot, underscore or hyphen, at most 64 characters", ""},
    {"limits.concurrency.max_queued_generations", 0U, nullptr, "", "derived: min(4, max(0, workers-2))"},
    {"limits.concurrency.max_connections_per_remote_address", 0U, nullptr, "", "derived: min(4, workers); min(6, workers) with listener.web_ui"},
}};

// Reads a string-array row and applies its SP3 bounds; any failure records one
// fixed issue naming the key and yields an empty list.
std::vector<std::string> read_bounded_strings(DraftReader& read, ConfigErrors& errors, const std::string_view path) {
  std::vector<std::string> values = read.strings(path);
  const ConfigKeyExtension* extension = find_config_key_extension(path);
  if (extension == nullptr) return values;
  if (extension->max_entries != 0U && values.size() > extension->max_entries) {
    add(errors, std::string{path}, ConfigIssueCode::out_of_range,
        "must have at most " + std::to_string(extension->max_entries) + " entries");
    return {};
  }
  std::set<std::string> seen;
  for (const auto& value : values) {
    if (extension->entry_valid != nullptr && !extension->entry_valid(value)) {
      add(errors, std::string{path}, ConfigIssueCode::invalid_value, std::string{extension->entry_reason});
      return {};
    }
    if (!seen.insert(value).second) {
      add(errors, std::string{path}, ConfigIssueCode::invalid_value, "must not repeat an entry");
      return {};
    }
  }
  return values;
}

// GUI-first start-up (spec 2026-10-27 §5a): alerts.email.*, only in a build that has them (any alerts.* row is refused
// by name otherwise). Each row's value is checked in every build, so a mistyped row says what is wrong with it there too.
AlertsConfig read_alerts(DraftReader& read, const FrontendConfigDraft& draft, ConfigErrors& errors) {
  AlertsConfig alerts_config;
  auto& email = alerts_config.email;
  email.enabled = read.boolean("alerts.email.enabled");
  const auto printable = [](const std::string_view text) {
    return !text.empty() && text.size() <= 254U &&
           std::all_of(text.begin(), text.end(), [](const char c) { return c > 0x20 && c < 0x7f; });
  };
  if (const auto smtp_host = read.string("alerts.email.host")) {
    const bool dns = printable(*smtp_host) && smtp_host->size() <= 253U && smtp_host->front() != '-' && smtp_host->front() != '.' && smtp_host->back() != '.' &&
                     smtp_host->find("..") == std::string::npos &&
                     std::all_of(smtp_host->begin(), smtp_host->end(), [](const char c) {
                       return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-';
                     });
    if (dns) email.host = *smtp_host;
    else add(errors, "alerts.email.host", ConfigIssueCode::invalid_value, "must be a DNS name (letters, digits, dots and dashes)");
  }
  email.port = static_cast<std::uint16_t>(read.integer("alerts.email.port"));
  if (const auto security = read.string("alerts.email.security")) {
    if (*security == "ssl") email.security = SmtpSecurity::ssl;
    else if (*security == "starttls") email.security = SmtpSecurity::starttls;
    else add(errors, "alerts.email.security", ConfigIssueCode::invalid_value, "must be ssl or starttls");
  }
  if (const auto username = read.string("alerts.email.username")) {
    if (printable(*username)) email.username = *username;
    else add(errors, "alerts.email.username", ConfigIssueCode::invalid_value, "must be 1-254 printable ASCII characters without spaces");
  }
  if (const auto from = read.string("alerts.email.from")) {
    if (is_valid_email_address(*from)) email.from = *from;
    else add(errors, "alerts.email.from", ConfigIssueCode::invalid_value, "must be an email address");
  }
  email.to = read_bounded_strings(read, errors, "alerts.email.to");
  if (draft.entries.contains("alerts.email.events")) {
    email.events = read_bounded_strings(read, errors, "alerts.email.events");
  } else {
    email.events.assign(kAlertEvents.begin(), kAlertEvents.end());
  }
  if (draft.entries.contains("alerts.email.to") && email.to.empty()) {
    add(errors, "alerts.email.to", ConfigIssueCode::invalid_value, "needs 1-5 addresses");
  }
  if (!email_alerts_built()) {
    for (const auto& [path, entry] : draft.entries) {
      if (path.rfind("alerts.", 0) == 0U) {
        add(errors, path, ConfigIssueCode::invalid_value, "this build has no email alerts (built without SECURE_FRONTEND_EMAIL_ALERTS)");
      }
    }
    return {};
  }
  if (email.enabled) {
    for (const std::string_view row : {"alerts.email.host", "alerts.email.username", "alerts.email.from", "alerts.email.to"}) {
      if (!draft.entries.contains(row)) add(errors, std::string{row}, ConfigIssueCode::missing_required, "is needed with alerts.email.enabled");
    }
  }
  return alerts_config;
}

// Web search (spec 2026-10-27-web-search-design §2): search.*, only in a build that has it (any search.* row is refused by
// name otherwise). Each row's value is checked in every build, as alerts.* are.
SearchConfig read_search(DraftReader& read, const FrontendConfigDraft& draft, ConfigErrors& errors) {
  SearchConfig search;
  search.enabled = read.boolean("search.enabled");
  if (const auto url = read.string("search.url")) {
    if (url->size() <= 2048U && parse_search_url(*url).has_value()) search.url = *url;
    else add(errors, "search.url", ConfigIssueCode::invalid_value, "must be http(s)://host[:port][/path], with no user, query or fragment");
  }
  search.results = static_cast<std::size_t>(read.integer("search.results"));
  search.max_rounds = static_cast<std::size_t>(read.integer("search.max_rounds"));
  search.timeout = std::chrono::seconds(read.integer("search.timeout_s"));
  if (!web_search_built()) {
    for (const auto& [path, entry] : draft.entries) {
      if (path.rfind("search.", 0) == 0U) {
        add(errors, path, ConfigIssueCode::invalid_value, "this build has no web search (built without SECURE_FRONTEND_WEB_SEARCH)");
      }
    }
    return {};
  }
  if (search.enabled && !draft.entries.contains("search.url")) {
    add(errors, "search.url", ConfigIssueCode::missing_required, "is needed with search.enabled");
  }
  return search;
}

bool has_issue_for(const ConfigErrors& errors, const std::string_view path) {
  return std::any_of(errors.begin(), errors.end(), [path](const ConfigIssue& issue) { return issue.key_path == path; });
}
}  // namespace

std::variant<SearchConfig, ConfigErrors> search_from_draft(const FrontendConfigDraft& draft) {
  ConfigErrors errors;
  DraftReader read(draft, errors);
  auto search = read_search(read, draft, errors);
  if (!errors.empty()) return errors;
  return search;
}

std::variant<AlertsConfig, ConfigErrors> alerts_from_draft(const FrontendConfigDraft& draft) {
  ConfigErrors errors;
  DraftReader read(draft, errors);
  auto alerts = read_alerts(read, draft, errors);
  if (!errors.empty()) return errors;
  return alerts;
}

const ConfigKeyExtension* find_config_key_extension(const std::string_view path) noexcept {
  for (const auto& extension : kSchemaExtensions) if (extension.path == path) return &extension;
  return nullptr;
}

std::uint32_t derived_max_queued_generations(const std::uint32_t workers) noexcept {
  const std::uint32_t spare = workers > 2U ? workers - 2U : 0U;
  return std::min<std::uint32_t>(static_cast<std::uint32_t>(config_defaults::kDerivedQueueCap), spare);
}

std::uint32_t derived_max_connections_per_remote_address(const std::uint32_t workers, const bool web_ui) noexcept {
  const auto cap = web_ui ? config_defaults::kDerivedRemoteCapWebUi : config_defaults::kDerivedRemoteCap;
  return std::min<std::uint32_t>(static_cast<std::uint32_t>(cap), workers);
}

bool is_config_section(const std::string_view path) noexcept {
  return std::any_of(kSchema.begin(), kSchema.end(), [path](const ConfigKey& key) {
    return key.path.size() > path.size() && key.path.substr(0, path.size()) == path && key.path[path.size()] == '.';
  });
}

std::string_view type_reason(const ConfigValueType type) noexcept {
  switch (type) {
    case ConfigValueType::boolean: return "must be true or false";
    case ConfigValueType::integer: return "must be an integer";
    case ConfigValueType::string: return "must be a string";
    case ConfigValueType::string_array: return "must be an array of strings";
    case ConfigValueType::decimal: return "must be a number";
  }
  return "has the wrong type";
}

std::string render_config_schema_markdown() {
  std::string out = "| Key | Type | Default | Range | Option | Meaning |\n| --- | --- | --- | --- | --- | --- |\n";
  for (const auto& key : kSchema) {
    std::string type;
    std::string fallback;
    std::string range = "-";
    switch (key.type) {
      case ConfigValueType::boolean:
        type = "boolean";
        fallback = key.default_boolean ? "`true`" : "`false`";
        break;
      case ConfigValueType::integer:
        type = "integer";
        fallback = (key.default_integer < key.minimum || key.default_integer > key.maximum)
                       ? "unset"
                       : "`" + std::to_string(key.default_integer) + "`";
        if (const auto* ext = find_config_key_extension(key.path); ext != nullptr && !ext->derived_default.empty()) {
          fallback = "`" + std::string{ext->derived_default} + "`";
        }
        range = std::to_string(key.minimum) + "-" + std::to_string(key.maximum);
        break;
      case ConfigValueType::string:
        type = "string";
        fallback = !key.default_string.empty() ? "`" + std::string{key.default_string} + "`"
                   : key.required             ? "required"
                                              : "unset";
        break;
      case ConfigValueType::string_array:
        type = "string array";
        fallback = "`[]`";
        break;
      case ConfigValueType::decimal:
        type = "number";
        fallback = std::isnan(key.default_decimal) ? "unset" : "`" + decimal_text(key.default_decimal) + "`";
        range = (key.minimum_exclusive ? ">" : "") + decimal_text(key.minimum_decimal) + "-" +
                decimal_text(key.maximum_decimal);
        break;
    }
    const std::string option = key.flag.empty() ? "-" : "`" + std::string{key.flag} + "`";
    out += "| `" + std::string{key.path} + "` | " + type + " | " + fallback + " | " + range + " | " + option + " | " +
           std::string{key.description} + " |\n";
  }
  return out;
}

namespace {
std::string_view profile_text(const EngineProfile profile) {
  switch (profile) {
    case EngineProfile::qwen: return "qwen";
    case EngineProfile::glm53: return "glm53";
    case EngineProfile::gemma4: return "gemma4";
    case EngineProfile::deepseek: return "deepseek";
    case EngineProfile::raon1: return "raon1";
    case EngineProfile::flint: return "flint";
    case EngineProfile::minimax: return "minimax";
  }
  return "qwen";  // unreachable: every value is a case above
}
}  // namespace

EngineArgumentsProfile arguments_profile_for(const EngineProfile profile) {
  switch (profile) {
    case EngineProfile::qwen: return EngineArgumentsProfile::strata_0143;
    case EngineProfile::glm53: return EngineArgumentsProfile::lighttransport_glm53;
    case EngineProfile::gemma4: return EngineArgumentsProfile::strata_gemma4;
    case EngineProfile::deepseek: return EngineArgumentsProfile::maya_ds4;  // Maya's maya-ds4
    case EngineProfile::raon1: return EngineArgumentsProfile::lighttransport_glm53;  // rides the glm_pack engine
    case EngineProfile::flint: return EngineArgumentsProfile::strata_flint;
    case EngineProfile::minimax: return EngineArgumentsProfile::maya_mm3;  // Maya's maya-mm3
  }
  return EngineArgumentsProfile::strata_0143;  // unreachable: every value is a case above
}

std::string format_issue(const ConfigIssue& issue) { return issue.key_path + ": " + issue.reason; }

std::optional<ModelId> make_model_id(const std::string_view text) {
  const auto colon = text.find(':');
  const auto name = text.substr(0, colon);
  const bool ok = std::holds_alternative<std::string>(validate_model_id(name)) &&
                  (colon == std::string_view::npos ||
                   std::holds_alternative<std::string>(validate_model_id(text.substr(colon + 1))));
  if (!ok) return std::nullopt;
  return ModelId{std::string{text}};
}

ConfigValidationResult validate(const FrontendConfigDraft& draft) {
  ConfigErrors errors;
  for (const auto& [path, entry] : draft.entries) {
    if (find_config_key(path) == nullptr) add(errors, path, ConfigIssueCode::unknown_key, "unknown key");
  }
  DraftReader read(draft, errors);
  if (draft.config_directory.has_value() && !read.present("schema_version")) {
    add(errors, "schema_version", ConfigIssueCode::missing_required, "is required in a config file");
  }
  (void)read.integer("schema_version");

  // listener
  const std::string host_text = read.string("listener.host").value_or("127.0.0.1");
  const auto port = read.integer("listener.port");
  const bool allow_remote = read.boolean("listener.allow_remote");
  const bool allow_wildcard = read.boolean("listener.allow_wildcard_bind");
  const bool web_ui = read.boolean("listener.web_ui");
  const bool loopback_without_key = read.boolean("listener.loopback_without_key");
  const bool monitor = read.boolean("listener.monitor");
  const bool login_link = read.boolean("listener.login_link");
  const auto backlog = read.integer("listener.backlog");
  const auto host = parse_listener_address(host_text);
  if (!host.has_value()) {
    add(errors, "listener.host", ConfigIssueCode::invalid_value,
        "must be an IP literal or localhost; hostnames are never resolved");
  } else if (host->scope() != AddressScope::loopback && !allow_remote) {
    add(errors, "listener.host", ConfigIssueCode::remote_not_enabled,
        "a non-loopback address needs listener.allow_remote (--allow-remote)");
  } else if (host->scope() == AddressScope::wildcard && !allow_wildcard) {
    add(errors, "listener.host", ConfigIssueCode::wildcard_bind_not_enabled,
        "a wildcard address also needs listener.allow_wildcard_bind (--allow-wildcard-bind)");
  }
  // listener.loopback_without_key (owner decision 2026-10-21, session log step 12): the one
  // keyless listener — a loopback host only, and no key source that would go unread.
  if (loopback_without_key) {
    if (host.has_value() && host->scope() != AddressScope::loopback) {
      add(errors, "listener.loopback_without_key", ConfigIssueCode::conflicting_values,
          "needs a loopback listener.host (127.0.0.1, ::1 or localhost); a remote or wildcard bind always needs the key");
    }
    if (read.present("token.source")) {  // named in the file or by --token-file/--token-env
      add(errors, "token.source", ConfigIssueCode::conflicting_values,
          "no key is read when listener.loopback_without_key is true; remove one of the two");
    }
  }
  // GUI-first start-up (spec 2026-10-27 §3, D1): the one-time login link hands the page the key, so it needs a key
  // and the page.
  if (login_link && (loopback_without_key || !web_ui)) {
    add(errors, "listener.login_link", ConfigIssueCode::conflicting_values,
        loopback_without_key ? "hands out the API key, so it cannot go with listener.loopback_without_key"
                             : "needs listener.web_ui (the page opens the link)");
  }
  // SP3 §4.3/§4.4: a wildcard bind trusts only the configured Host aliases.
  std::vector<std::string> cors_origins = read_bounded_strings(read, errors, "listener.cors_origins");
  std::vector<std::string> host_aliases = read_bounded_strings(read, errors, "listener.host_aliases");
  if (host.has_value() && host->scope() == AddressScope::wildcard && host_aliases.empty() &&
      !has_issue_for(errors, "listener.host_aliases")) {
    add(errors, "listener.host_aliases", ConfigIssueCode::invalid_value,
        "must name at least one Host value when the bind is a wildcard");
  }

  // limits
  const auto deadline_ms = read.integer("limits.http.request_deadline_ms");
  const auto max_request = read.integer("limits.http.max_request_bytes");
  const auto max_header = read.integer("limits.http.max_header_bytes");
  const auto max_headers = read.integer("limits.http.max_headers");
  const auto max_line = read.integer("limits.http.max_request_line_bytes");
  const auto header_deadline_ms = read.integer("limits.http.header_deadline_ms");
  const auto write_stall_ms = read.integer("limits.http.write_stall_ms");
  const auto response_deadline_ms = read.integer("limits.http.response_write_deadline_ms");
  const auto keepalive_ms = read.integer("limits.http.stream_keepalive_ms");
  const bool log_requests = read.boolean("logging.requests");
  // SP5 M1 Task 4: defaults.sampling.* (unset rows stay unset).
  SamplingDefaults sampling_defaults{
      read.decimal("defaults.sampling.temperature"), read.decimal("defaults.sampling.top_p"),
      read.decimal("defaults.sampling.min_p"), read.decimal("defaults.sampling.presence_penalty"),
      read.decimal("defaults.sampling.repetition_penalty"), read.decimal("defaults.sampling.frequency_penalty"),
      read.optional_integer("defaults.sampling.top_k"), read.optional_integer("defaults.sampling.penalty_last_n"),
      read.optional_integer("defaults.sampling.seed")};
  // SP5 M1 Task 5: reasoning.* (pin vocabularies; the string rows carry the pin's defaults).
  ReasoningConfig reasoning;
  if (const auto text = read.string("reasoning.effort")) {
    if (*text == "none") reasoning.effort = ReasoningEffort::none;
    else if (*text == "low") reasoning.effort = ReasoningEffort::low;
    else if (*text == "medium") reasoning.effort = ReasoningEffort::medium;
    else if (*text == "high") reasoning.effort = ReasoningEffort::high;
    else add(errors, "reasoning.effort", ConfigIssueCode::invalid_value, "must be none, low, medium or high");
  }
  reasoning.budget_tokens = read.integer("reasoning.budget_tokens");
  reasoning.close_retry = read.boolean("reasoning.close_retry");
  if (const auto text = read.string("reasoning.loop_recovery")) {
    if (*text == "off") reasoning.loop_recovery = LoopRecovery::off;
    else if (*text == "stop") reasoning.loop_recovery = LoopRecovery::stop;
    else if (*text == "recover") reasoning.loop_recovery = LoopRecovery::recover;
    else add(errors, "reasoning.loop_recovery", ConfigIssueCode::invalid_value, "must be off, stop or recover");
  }
  if (const auto text = read.string("reasoning.effort_position")) {
    if (*text == "start") reasoning.effort_position = EffortPosition::start;
    else if (*text == "end") reasoning.effort_position = EffortPosition::end;
    else add(errors, "reasoning.effort_position", ConfigIssueCode::invalid_value, "must be start or end");
  }
  if (const auto text = read.string("reasoning.anthropic_thinking")) {
    if (*text == "model") reasoning.anthropic_thinking = AnthropicThinking::model;
    else if (*text == "on_request") reasoning.anthropic_thinking = AnthropicThinking::on_request;
    else add(errors, "reasoning.anthropic_thinking", ConfigIssueCode::invalid_value, "must be model or on_request");
  }
  const bool fit_max_tokens = read.boolean("engine.fit_max_tokens");
  // SP5 M1 Task 6 (parity rows lazy_load / idle_unload_s / mtp, runconfig.py:35-38):
  // lifecycle family reads; mtp is on unless an explicit false (draft_layer.py is_off).
  const bool lazy_load = read.boolean("engine.lazy_load");
  const std::int64_t idle_unload_s = read.integer("engine.idle_unload_s");
  const bool mtp = read.boolean("engine.mtp");
  const auto max_body = read.integer("limits.chat.max_body_bytes");
  const auto max_messages = read.integer("limits.chat.max_messages");
  const auto max_message = read.integer("limits.chat.max_message_bytes");
  const auto max_prompt = read.integer("limits.chat.max_prompt_tokens");
  const auto max_completion = read.integer("limits.chat.max_completion_tokens");
  const auto default_max = read.integer("limits.chat.default_max_tokens");
  const auto max_generated = read.integer("limits.chat.max_generated_bytes");
  const auto ready_s = read.integer("limits.engine.ready_timeout_s");
  const auto generation_s = read.integer("limits.engine.generation_timeout_s");
  const auto drain_s = read.integer("limits.engine.stop_drain_timeout_s");
  const auto concurrent = read.integer("limits.concurrency.max_concurrent_requests");
  // SP3 §5.1: a derived value is used only when the key is absent; an explicit
  // value is validated as written and never adjusted.
  const auto workers = static_cast<std::uint32_t>(concurrent);
  const bool queue_set = read.present("limits.concurrency.max_queued_generations");
  const auto queued = queue_set ? read.integer("limits.concurrency.max_queued_generations")
                                : static_cast<std::int64_t>(derived_max_queued_generations(workers));
  const auto queue_timeout_ms = read.integer("limits.concurrency.queue_timeout_ms");
  const bool cap_set = read.present("limits.concurrency.max_connections_per_remote_address");
  const auto remote_cap = cap_set ? read.integer("limits.concurrency.max_connections_per_remote_address")
                                  : static_cast<std::int64_t>(derived_max_connections_per_remote_address(workers, web_ui));
  if (max_header > max_request) {
    add(errors, "limits.http.max_header_bytes", ConfigIssueCode::conflicting_values,
        "must not exceed limits.http.max_request_bytes");
  }
  if (max_line > max_header) {
    add(errors, "limits.http.max_request_line_bytes", ConfigIssueCode::conflicting_values,
        "must not exceed limits.http.max_header_bytes");
  }
  if (max_body > max_request - max_header) {
    add(errors, "limits.chat.max_body_bytes", ConfigIssueCode::conflicting_values,
        "must not exceed limits.http.max_request_bytes minus limits.http.max_header_bytes");
  }
  if (default_max > max_completion) {
    add(errors, "limits.chat.default_max_tokens", ConfigIssueCode::conflicting_values,
        "must not exceed limits.chat.max_completion_tokens");
  }
  if (header_deadline_ms > deadline_ms) {
    add(errors, "limits.http.header_deadline_ms", ConfigIssueCode::conflicting_values,
        "must not exceed limits.http.request_deadline_ms");
  }
  if (response_deadline_ms < write_stall_ms) {
    add(errors, "limits.http.response_write_deadline_ms", ConfigIssueCode::conflicting_values,
        "must be at least limits.http.write_stall_ms");
  }
  if (concurrent == 1 && queued != 0) {
    add(errors, "limits.concurrency.max_queued_generations", ConfigIssueCode::conflicting_values,
        "must be 0 when limits.concurrency.max_concurrent_requests is 1");
  } else if (concurrent >= 2 && queued + 2 > concurrent) {
    add(errors, "limits.concurrency.max_queued_generations", ConfigIssueCode::conflicting_values,
        "plus 2 must not exceed limits.concurrency.max_concurrent_requests");
  }
  if (remote_cap > concurrent) {
    add(errors, "limits.concurrency.max_connections_per_remote_address", ConfigIssueCode::conflicting_values,
        "must not exceed limits.concurrency.max_concurrent_requests");
  }

  // roots: each is opened once, here, and held open by the FrontendConfig.
  // roots.allow_untrusted_media (owner opt-in): model data on exFAT/FAT media whose permissions the OS ignores
  // opens with RootPolicy::external (owner/shared-writable check skipped; links below are still refused). The
  // secret root keeps the strict check (it holds the API token).
  const bool untrusted_media = read.boolean("roots.allow_untrusted_media");
  secure_frontend::set_relaxed_paths(untrusted_media);
  const RootPolicy models_policy = untrusted_media ? RootPolicy::external : RootPolicy::existing;
  const auto models = canonical_root(read, errors, "roots.models", models_policy);
  const auto tokenizer = canonical_root(read, errors, "roots.tokenizer_pack", models_policy);
  auto secret = canonical_root(read, errors, "roots.secret");
  const auto logs = creatable_root(read, errors, "roots.logs");
  const auto cache = creatable_root(read, errors, "roots.cache");
  const auto state = absolute_path(read, errors, "roots.state");

  // token. An unset roots.secret defaults to the config file's folder, and only
  // for a file: token: an env: token reads nothing below a secret root (spec
  // §3.2, §4), so that folder is not opened or checked for it.
  const std::string source_text = read.string("token.source").value_or("file:token");
  auto token = token_source(source_text);
  if (!token.has_value()) {
    add(errors, "token.source", ConfigIssueCode::invalid_value,
        "must be env:NAME ([A-Za-z_][A-Za-z0-9_]{0,127}) or file:RELATIVE-PATH below roots.secret");
  } else if (token->kind() == SecretSourceKind::protected_file && !read.present("roots.secret")) {
    if (!draft.config_directory.has_value()) {
      add(errors, "roots.secret", ConfigIssueCode::missing_required, "is required when token.source is file:…");
    } else {
      auto opened = open_root(*draft.config_directory);
      if (auto* root = std::get_if<CanonicalRoot>(&opened)) {
        secret = std::move(*root);
      } else {
        add(errors, "roots.secret", ConfigIssueCode::invalid_value,
            "is unset, so it defaults to the config file's folder, which is not usable: " +
                std::get<PathError>(opened).message);
      }
    }
  }

  // engine
  std::optional<EngineProfile> profile;
  if (const auto text = read.string("engine.profile")) {
    if (*text == "qwen") profile = EngineProfile::qwen;
    else if (*text == "glm53") profile = EngineProfile::glm53;
    else if (*text == "gemma4") profile = EngineProfile::gemma4;
    else if (*text == "deepseek") profile = EngineProfile::deepseek;
    else if (*text == "raon1") profile = EngineProfile::raon1;
    else if (*text == "flint") profile = EngineProfile::flint;
    else if (*text == "minimax") profile = EngineProfile::minimax;
    else add(errors, "engine.profile", ConfigIssueCode::invalid_value, "must be qwen, glm53, gemma4, deepseek, raon1, flint or minimax");
  }
  std::optional<CanonicalFile> executable;
  if (const auto text = read.string("engine.executable")) {
    auto opened = open_executable(path_from_utf8(*text));
    if (auto* error = std::get_if<PathError>(&opened)) {
      add(errors, "engine.executable", ConfigIssueCode::invalid_value, error->message);
    } else {
      executable = std::get<CanonicalFile>(std::move(opened));
    }
  }
  const auto model_text = read.string("engine.model_id");
  std::optional<ModelId> model_id = model_text ? make_model_id(*model_text) : std::nullopt;
  if (model_text.has_value() && !model_id.has_value()) {
    add(errors, "engine.model_id", ConfigIssueCode::invalid_value,
        "must be name or name:tag, each 1-128 characters of [A-Za-z0-9._-], starting with a letter or digit");
  }
  const auto args = read.strings("engine.args");
  const auto arguments_profile = arguments_profile_for(profile.value_or(EngineProfile::qwen));
  if (const auto rejected = validate_engine_user_arguments(arguments_profile, args)) {
    add(errors, "engine.args", ConfigIssueCode::invalid_value, rejected->message);
  }
  // SP5 M2 Task 7 (D-8, C-5): the SP1 rule "glm53 requires at least one --engine-arg" is
  // retired with the pass-through — the GLM launch is composed, and validate_engine_user_arguments
  // above refuses ANY engine.args row under the glm53 profile.
  // Each entry is one directory: the platform's list separator would split
  // LD_LIBRARY_PATH (':') or PATH (';') into extra, unvalidated entries.
#if defined(_WIN32)
  constexpr char kListSeparator = ';';
#else
  constexpr char kListSeparator = ':';
#endif
  std::vector<std::filesystem::path> lib_dirs;
  for (const auto& text : read.strings("engine.lib_dirs")) {
    if (text.empty() || text.find('\0') != std::string::npos || text.find(kListSeparator) != std::string::npos ||
        !path_from_utf8(text).is_absolute()) {
      add(errors, "engine.lib_dirs", ConfigIssueCode::invalid_value, "every entry must be an absolute directory");
      break;
    }
    lib_dirs.push_back(path_from_utf8(text));
  }
  std::vector<unsigned> gpus;
  if (const auto gpu_text = read.string("engine.gpu")) {
    if (const auto parsed = parse_gpu_list(*gpu_text)) {
      if (profile == EngineProfile::glm53 || profile == EngineProfile::raon1) {
        // The glm_pack engine (glm53, raon1) takes its GPUs from the glm env mechanism, not --gpu.
        add(errors, "engine.gpu", ConfigIssueCode::conflicting_values,
            "is not supported with the glm_pack engine (engine.profile glm53 or raon1)");
      } else {
        gpus = *parsed;
      }
    } else {
      add(errors, "engine.gpu", ConfigIssueCode::invalid_value,
          "must be a comma-separated list of distinct device indices 0-255, at most 16");
    }
  }
  const bool allow_new_privileges = read.boolean("engine.allow_new_privileges");
  // SP5 M1 Task 7: the VRAM / GPU / elastic family. Kinds/defaults verbatim from the pin
  // (runconfig.py:42-43; serve/server.py:2548-2552, 2870-2894, 2947, 8083; images.py:45);
  // the layer_split grammar is the pin's own (layer_split_value, :2875-2892: blank or
  // "auto" is the default; otherwise rising integers from >=2, one per GPU after the
  // first — the count rule applies only with several GPUs, as at :2888).
  const auto vram_reserve_mib = read.optional_integer("engine.vram_reserve_mib");
  const std::int64_t min_free_vram = read.integer("engine.min_free_vram_mib");
  const bool vram_elastic = read.boolean("engine.vram_elastic");
  const auto vram_segment_mib = read.optional_integer("engine.vram_segment_mib");
  const bool elastic = read.boolean("engine.elastic");
  const std::int64_t elastic_idle_s = read.integer("engine.elastic_idle_s");
  const std::int64_t elastic_mib = read.integer("engine.elastic_mib");
  GpuOrder gpu_order{GpuOrder::auto_order};
  if (const auto text = read.string("engine.gpu_order")) {
    if (*text == "auto") gpu_order = GpuOrder::auto_order;
    else if (*text == "as_given") gpu_order = GpuOrder::as_given;
    else add(errors, "engine.gpu_order", ConfigIssueCode::invalid_value, "must be auto or as_given");
  }
  GpuTurnPolicy gpu_policy{GpuTurnPolicy::auto_policy};
  if (const auto text = read.string("engine.gpu_policy")) {
    if (*text == "auto") gpu_policy = GpuTurnPolicy::auto_policy;
    else if (*text == "keep") gpu_policy = GpuTurnPolicy::keep;
    else if (*text == "shrink") gpu_policy = GpuTurnPolicy::shrink;
    else if (*text == "unload") gpu_policy = GpuTurnPolicy::unload;
    else add(errors, "engine.gpu_policy", ConfigIssueCode::invalid_value, "must be auto, keep, shrink or unload");
  }
  const std::string layer_split = read.string("engine.layer_split").value_or("");
  if (auto trimmed = layer_split; !(trimmed.empty() || trimmed == "auto")) {
    std::vector<std::int64_t> layers;
    bool grammar = true;
    std::size_t start = 0;
    while (start <= trimmed.size()) {
      const auto comma = trimmed.find(',', start);
      const auto piece = trimmed.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
      std::size_t begin = piece.find_first_not_of(" \t");
      std::size_t end = piece.find_last_not_of(" \t");
      if (begin == std::string::npos) {
        grammar = false;
        break;
      }
      std::int64_t value = 0;
      const auto [ptr, ec] = std::from_chars(piece.data() + begin, piece.data() + end + 1, value);
      if (ec != std::errc{} || ptr != piece.data() + end + 1) {
        grammar = false;
        break;
      }
      layers.push_back(value);
      if (comma == std::string::npos) break;
      start = comma + 1;
    }
    bool rising = grammar && !layers.empty() && layers.front() >= 2;
    for (std::size_t i = 1; i < layers.size(); ++i) {
      if (layers[i] <= layers[i - 1]) rising = false;
    }
    const bool count_ok = gpus.size() <= 1 || layers.size() == gpus.size() - 1;
    if (!grammar || !rising || !count_ok) {
      add(errors, "engine.layer_split", ConfigIssueCode::invalid_value,
          "must be auto, or the first layer of each later GPU: rising numbers from 2, one per GPU after the first (for example \"24,36,42\" for 4 GPUs), not a count per card");
    }
  }
  const bool split_skip_if_fits = read.boolean("engine.split_skip_if_fits");
  const auto hip_ordinal = read.optional_integer("engine.hip_ordinal");
  const bool uma = read.boolean("engine.uma");
  // SP5 M1 Task 8: the timeout / request-shape family. silence 0 = wait (the pin's),
  // repeat 0 = off (:8115); the token bounds stay unset until configured; aliases keep
  // set_aliases' grammar (dedup and dropping the model's own name happen at wiring).
  const std::int64_t silence_s = read.integer("engine.silence_s");
  const bool answer_room = read.boolean("engine.answer_room");
  const std::int64_t repeat_stop_tokens = read.integer("engine.repeat_stop_tokens");
  const auto max_output = read.optional_integer("engine.max_tokens");
  const auto min_output = read.optional_integer("engine.min_tokens");
  const std::vector<std::string> aliases = read_bounded_strings(read, errors, "model.aliases");
  // model.name (session log step 13): the served name, in the alias grammar.
  std::optional<std::string> served_name = read.string("model.name");
  if (served_name.has_value() && !is_valid_model_alias(*served_name)) {
    add(errors, "model.name", ConfigIssueCode::invalid_value,
        "must be 1-64 characters of [A-Za-z0-9._-] (name or name:tag), starting with a letter or digit");
    served_name.reset();
  }
  // SP5 M1 Task 9 (the session family; serve/server.py:2999-3005, :8043-8110, :8092-8100 +
  // slot_save_dir :3469-3481, images.py:188). Path rows keep the pin's shape (non-empty, no
  // control characters) and ride our added absolute rule (T9-b, engine.lib_dirs precedent):
  // there is no config-cwd to resolve a relative path against, and load never touches the
  // filesystem (the pin's makedirs 0700 is a runtime act).
  SessionPolicy session;
  const auto slot_text = read.string("session.slot_save_path");
  if (slot_text.has_value()) {
    if (is_absolute_store_path(*slot_text)) session.slot_save_path = *slot_text;
    else add(errors, "session.slot_save_path", ConfigIssueCode::invalid_value, "must be an absolute path without control characters");
  }
  const auto profile_text = read.string("session.expert_profile_save");
  if (profile_text.has_value()) {
    if (is_absolute_store_path(*profile_text)) session.expert_profile_save = *profile_text;
    else add(errors, "session.expert_profile_save", ConfigIssueCode::invalid_value, "must be an absolute path without control characters");
  }
  session.expert_profile_save_every = read.integer("session.expert_profile_save_every");
  session.tool_call_recovery = read.boolean("session.tool_call_recovery");
  session.env_to_user = read.boolean("session.env_to_user");
  session.codex_thread_titles = read.boolean("session.codex_thread_titles");
  session.codex_compaction_cache = read.boolean("session.codex_compaction_cache");
  session.keep_files = read.integer("session.keep_files");
  // SP5 M1 Task 10 (the C-4 narrowing): engine-internal parallelism only.
  const ReplicasConfig replicas{read.integer("replicas.parallel")};
  // SP5 M1 Task 11 (D-9 vision split): image input plumbing; generation stays out.
  VisionConfig vision;
  vision.enabled = read.boolean("vision.enabled");
  vision.max_side = read.integer("vision.max_side");
  vision.max_n = read.integer("vision.max_n");
  vision.need_mib = read.optional_integer("vision.need_mib");
  vision.no_flash_attn = read.boolean("vision.no_flash_attn");
  vision.resident = read.boolean("vision.resident");
  vision.threads = read.integer("vision.threads");
  vision.ready_timeout_s = read.integer("vision.ready_timeout_s");
  vision.encode_timeout_s = read.integer("vision.encode_timeout_s");
  vision.gpu = read.boolean("vision.gpu");
  if (const auto mmproj = read.string("vision.mmproj")) {
    if (is_absolute_store_path(*mmproj)) vision.mmproj = path_from_utf8(*mmproj);
    else add(errors, "vision.mmproj", ConfigIssueCode::invalid_value, "must be an absolute path");
  }

  // SP5 M2 Task 3 (spec §D-2): the glm.* family. Kinds and defaults are the
  // vendored pin's (serve/server.py:4090/:4077/:3819/:1274-1277, validators
  // :3798); the caps, the closed effort vocabulary and the layer_split
  // grammar are added (C-1). The qwen-profile gate mirrors the existing
  // engine.gpu rule the other way: an explicit engine.profile "qwen" refuses
  // every present glm.* row by name (conflicting_values); with no explicit
  // profile a managed model's SP4 manifest decides at launch, so the rows
  // validate here and Task 7 refuses them for a qwen manifest (R-T3-a).
  GlmSettings glm;
  bool any_glm_row = false;
  for (const auto& [path, entry] : draft.entries) {
    static_cast<void>(entry);
    if (path.rfind("glm.", 0) == 0) any_glm_row = true;
  }
  glm.thinking_budget = read.integer("glm.thinking_budget");
  glm.max_context = read.integer("glm.max_context");
  glm.kv = read.string("glm.kv").value_or("fp16");
  if (glm.kv != "fp16" && glm.kv != "int8" && glm.kv != "q8_0" && glm.kv != "q4_0") {
    add(errors, "glm.kv", ConfigIssueCode::invalid_value, "must be fp16, int8, q8_0 or q4_0");
  }
  glm.ram_headroom_gb = read.integer("glm.ram_headroom_gb");
  glm.pcie_share = read.decimal("glm.pcie_share");
  glm.cpu_lane = read.optional_integer("glm.cpu_lane");
  glm.idle_unload_s = read.integer("glm.idle_unload_s");
  glm.rows_present = any_glm_row;  // R-T3-a: refused at launch when the manifest names the qwen profile
  if (const auto text = read.string("glm.reasoning_effort")) {
    if (*text == "none") glm.reasoning_effort = GlmReasoningEffort::none;
    else if (*text == "low") glm.reasoning_effort = GlmReasoningEffort::low;
    else if (*text == "high") glm.reasoning_effort = GlmReasoningEffort::high;
    else if (*text == "max") glm.reasoning_effort = GlmReasoningEffort::max;
    else add(errors, "glm.reasoning_effort", ConfigIssueCode::invalid_value, "must be none, low, high or max");
  }
  glm.fit_max_tokens = read.boolean("glm.fit_max_tokens");
  glm.experimental_speed_projection = read.boolean("glm.experimental_speed_projection");
  if (const auto gpu_text = read.string("glm.gpu")) {
    if (const auto parsed = parse_gpu_list(*gpu_text)) glm.gpus = *parsed;
    else add(errors, "glm.gpu", ConfigIssueCode::invalid_value,
             "must be a comma-separated list of distinct device indices 0-255, at most 16");
  }
  glm.layer_split = read.string("glm.layer_split").value_or("");
  if (glm.layer_split.empty()) glm.layer_split = "auto";
  if (glm.layer_split != "auto") {
    // The vendored grammar (serve/server.py:1274-1277): the first layer of
    // each later GPU's share, e.g. "18" or "16,32". The rising-numbers check
    // is added (C-1); the count-against-gpus check belongs to the launch
    // (Task 7), because a manifest may name the cards the config does not.
    std::vector<std::int64_t> layers;
    bool grammar = true;
    std::size_t start = 0;
    while (start <= glm.layer_split.size()) {
      const auto comma = glm.layer_split.find(',', start);
      const auto piece = glm.layer_split.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
      const std::size_t begin = piece.find_first_not_of(" \t");
      const std::size_t end = piece.find_last_not_of(" \t");
      if (begin == std::string::npos) {
        grammar = false;
        break;
      }
      std::int64_t value = 0;
      const auto [ptr, ec] = std::from_chars(piece.data() + begin, piece.data() + end + 1, value);
      if (ec != std::errc{} || ptr != piece.data() + end + 1 || value < 1) {
        grammar = false;
        break;
      }
      layers.push_back(value);
      if (comma == std::string::npos) break;
      start = comma + 1;
    }
    bool rising = grammar && !layers.empty();
    for (std::size_t i = 1; i < layers.size(); ++i) {
      if (layers[i] <= layers[i - 1]) rising = false;
    }
    if (!rising) {
      add(errors, "glm.layer_split", ConfigIssueCode::invalid_value,
          "must be auto, or the first layer of each later GPU's share: rising numbers from 1 (for example \"18\" or \"16,32\")");
    }
  }
  if (const auto text = read.string("glm.tokenizer")) {
    if (is_absolute_store_path(*text)) glm.tokenizer = *text;
    else add(errors, "glm.tokenizer", ConfigIssueCode::invalid_value, "must be an absolute path");
  }
  if (const auto text = read.string("glm.model_name")) {
    if (text->empty() || text->size() > 256 || text->find('\0') != std::string::npos) {
      add(errors, "glm.model_name", ConfigIssueCode::invalid_value, "must be 1-256 characters");
    } else {
      glm.model_name = *text;
    }
  }
  if (any_glm_row && profile.has_value() && *profile != EngineProfile::glm53 && *profile != EngineProfile::raon1) {
    // The glm.* rows configure the glm_pack engine, which serves both glm53 and raon1.
    for (const auto& [path, entry] : draft.entries) {
      static_cast<void>(entry);
      if (path.rfind("glm.", 0) == 0) {
        add(errors, path, ConfigIssueCode::conflicting_values, "is only supported with the glm_pack engine (engine.profile glm53 or raon1)");
      }
    }
  }
  // Gemma 4 plan 1 Task 3 (ruling): the rows the qwen launch turns into engine flags (VRAM and
  // split, the learned-profile save, --batch, the MTP strip, the effort-end tail) have no Gemma
  // engine counterpart, so on an explicit gemma4 profile each is refused by name rather than
  // silently ignored. A managed model's manifest decides its profile at launch (and the engine
  // gate refuses gemma4 manifests until plan 4).
  // flint's engine is built like Gemma's (multiengine_flint), so the same rows are refused on it by name.
  if (profile == EngineProfile::gemma4 || profile == EngineProfile::flint) {
    const bool gemma = profile == EngineProfile::gemma4;
    static constexpr std::array<std::string_view, 9> kQwenLaunchRows{
        "engine.vram_reserve_mib",   "engine.vram_elastic",         "engine.vram_segment_mib",
        "engine.layer_split",        "engine.split_skip_if_fits",   "session.expert_profile_save",
        "session.expert_profile_save_every", "replicas.parallel",   "engine.mtp"};
    for (const auto row : kQwenLaunchRows) {
      if (draft.entries.contains(row)) {
        add(errors, std::string{row}, ConfigIssueCode::conflicting_values,
            gemma ? "is not supported with engine.profile gemma4 (the Gemma engine has no such option)"
                  : "is not supported with engine.profile flint (the flint engine has no such option)");
      }
    }
    if (draft.entries.contains("reasoning.effort_position")) {
      add(errors, "reasoning.effort_position", ConfigIssueCode::conflicting_values,
          gemma ? "is not supported with engine.profile gemma4 (Gemma's template has no effort sentence)"
                : "is not supported with engine.profile flint (flint's template has no effort sentence)");
    }
  }

  // pull (SP4 §3.9)
  std::filesystem::path downloader = default_downloader_path();
  if (const auto text = read.string("pull.downloader")) {
    const auto candidate = path_from_utf8(*text);
    if (!candidate.is_absolute()) add(errors, "pull.downloader", ConfigIssueCode::invalid_value, "must be an absolute path");
    else downloader = candidate;
  }
  std::optional<ProxySetting> proxy;
  if (const auto text = read.string("pull.proxy")) {
    auto parsed = parse_proxy(*text);
    if (std::holds_alternative<UrlProblem>(parsed)) {
      add(errors, "pull.proxy", ConfigIssueCode::invalid_value,
          "must be http://host:port or https://host:port without credentials, path or query");
    } else {
      proxy = std::get<ProxySetting>(std::move(parsed));
    }
  }
  const bool allow_private = read.boolean("pull.allow_private_addresses");
  // SP7 M2 Task 3 (D-10): the release mirrors, schema-bounded like the arrays above.
  std::vector<std::string> update_source = read_bounded_strings(read, errors, "update.source");
  const auto max_total = read.integer("pull.max_total_bytes");
  const auto max_duration = read.integer("pull.max_duration_s");
  const auto stall = read.integer("pull.stall_timeout_s");
  const auto connect = read.integer("pull.connect_timeout_s");
  const auto connections = read.integer("pull.connections");

  // Model launcher (plan 2026-10-23 Task 1): the card files and the control switches. The
  // launcher starts and stops engines itself, so the lifecycle rows and the vision family are
  // refused beside it by name; models.start names a card in the served-name grammar (the
  // launcher checks it against the cards it loaded).
  ModelsConfig models_config;
  for (const auto& text : read_bounded_strings(read, errors, "models.cards")) models_config.cards.push_back(path_from_utf8(text));
  models_config.start = read.string("models.start");
  models_config.control = read.boolean("models.control");
  models_config.control_remote = read.boolean("models.control_remote");
  models_config.from_card = draft.card;
  const bool launcher = draft.entries.contains("models.cards");
  if (models_config.start.has_value() && !is_valid_model_alias(*models_config.start)) {
    add(errors, "models.start", ConfigIssueCode::invalid_value,
        "must be a card name: 1-64 characters of [A-Za-z0-9._-] (name or name:tag), starting with a letter or digit");
  }
  if (!launcher) {
    for (const std::string_view row : {"models.start", "models.control"}) {
      if (draft.entries.contains(row)) add(errors, std::string{row}, ConfigIssueCode::conflicting_values, "needs models.cards");
    }
  }
  if (models_config.control_remote && !models_config.control) {
    add(errors, "models.control_remote", ConfigIssueCode::conflicting_values, "needs models.control");
  }
  // owner 2026-10-27: the hosts and subnets remote model control is limited to (each entry checked by its extension)
  for (const auto& text : read_bounded_strings(read, errors, "models.control_remote_networks")) {
    if (const auto network = parse_ip_network(text)) models_config.control_remote_networks.push_back(*network);
  }
  if (draft.entries.contains("models.control_remote_networks") && !models_config.control_remote) {
    add(errors, "models.control_remote_networks", ConfigIssueCode::conflicting_values, "needs models.control_remote");
  }
  // Model downloads: the switch, its store, and the prepare step (each path absolute; the files
  // are checked at boot, never here).
  models_config.downloads = read.boolean("models.downloads");
  const auto absolute_path = [&read, &errors](const std::string_view row) -> std::optional<std::filesystem::path> {
    const auto text = read.string(row);
    if (!text.has_value()) return std::nullopt;
    if (!is_absolute_store_path(*text)) {
      add(errors, std::string{row}, ConfigIssueCode::invalid_value, "must be an absolute path");
      return std::nullopt;
    }
    return path_from_utf8(*text);
  };
  models_config.downloads_root = absolute_path("models.downloads_root");
  models_config.prepare_python = absolute_path("models.prepare_python");
  models_config.prepare_tools = absolute_path("models.prepare_tools");
  models_config.default_engine = absolute_path("models.default_engine");
  models_config.default_root = absolute_path("models.default_root");
  if (!launcher) {
    for (const std::string_view row : {"models.default_engine", "models.default_root"}) {
      if (draft.entries.contains(row)) add(errors, std::string{row}, ConfigIssueCode::conflicting_values, "needs models.cards");
    }
  }
  models_config.download_engine_card = read.string("models.download_engine_card");
  if (models_config.download_engine_card.has_value() && !is_valid_model_alias(*models_config.download_engine_card)) {
    add(errors, "models.download_engine_card", ConfigIssueCode::invalid_value, "must be a card name");
    models_config.download_engine_card.reset();
  }
  if (models_config.downloads && !launcher) add(errors, "models.downloads", ConfigIssueCode::conflicting_values, "needs models.cards");
  if (models_config.downloads && launcher && !draft.entries.contains("models.downloads_root")) {
    add(errors, "models.downloads", ConfigIssueCode::conflicting_values, "needs models.downloads_root");
  }
  if (draft.entries.contains("models.prepare_python") != draft.entries.contains("models.prepare_tools")) {
    add(errors, draft.entries.contains("models.prepare_python") ? "models.prepare_python" : "models.prepare_tools",
        ConfigIssueCode::conflicting_values, "needs both models.prepare_python and models.prepare_tools");
  }
  if (launcher) {
    // vision.* rows are a card's (main_config_card_issues refuses them in the main config)
    for (const std::string_view row : {"engine.lazy_load", "engine.idle_unload_s"}) {
      if (draft.entries.contains(row)) {
        add(errors, std::string{row}, ConfigIssueCode::conflicting_values,
            "is not supported with models.cards (the launcher starts and stops each engine)");
      }
    }
  }

  if (!errors.empty() || !host || !models || !token || !executable || !model_id) {
    if (errors.empty()) add(errors, "config file", ConfigIssueCode::invalid_value, "is incomplete");
    return errors;
  }
  const auto u32 = [](const std::int64_t v) { return static_cast<std::uint32_t>(v); };
  ListenerConfig listener{*host, static_cast<std::uint16_t>(port), allow_remote, allow_wildcard, u32(backlog),
                          std::move(cors_origins), std::move(host_aliases), web_ui, loopback_without_key, monitor,
                          login_link};
  const Limits limits{
      HttpLimits{std::chrono::milliseconds(deadline_ms), u32(max_request), u32(max_header), u32(max_headers), u32(max_line),
                 std::chrono::milliseconds(header_deadline_ms), std::chrono::milliseconds(write_stall_ms),
                 std::chrono::milliseconds(response_deadline_ms), std::chrono::milliseconds(keepalive_ms)},
      ChatLimits{u32(max_body), u32(max_messages), u32(max_message), u32(max_prompt), u32(max_completion), u32(default_max),
                 u32(max_generated)},
      EngineLimits{std::chrono::seconds(ready_s), std::chrono::seconds(generation_s), std::chrono::seconds(drain_s)},
      ConcurrencyLimits{u32(concurrent), u32(queued), std::chrono::milliseconds(queue_timeout_ms), u32(remote_cap), !queue_set,
                        !cap_set}};
  Roots roots{*models, tokenizer, secret, logs, cache, state, untrusted_media};
  // SP5-P2 Task 4 (D-7, R-T4-b): under the glm53 profile the Maya-named key
  // (glm.idle_unload_s, serve/server.py:8298 at v1.1.21) drives the idle-unload
  // window; 0 means unset and the M1 engine.idle_unload_s keeps driving.
  const std::int64_t idle_window_s =
      (profile == EngineProfile::glm53 || profile == EngineProfile::raon1) && glm.idle_unload_s > 0 ? glm.idle_unload_s
                                                                                                    : idle_unload_s;
  EngineLaunch engine{profile,
                      *executable,
                      std::move(*model_id),
                      args,
                      std::move(lib_dirs),
                      std::move(gpus),
                      allow_new_privileges,
                      fit_max_tokens,
                      lazy_load,
                      std::chrono::seconds(idle_window_s),
                      mtp,
                      vram_reserve_mib,
                      min_free_vram,
                      vram_elastic,
                      vram_segment_mib,
                      elastic,
                      std::chrono::seconds(elastic_idle_s),
                      elastic_mib,
                      gpu_order,
                      gpu_policy,
                      layer_split.empty() ? std::string{"auto"} : layer_split,
                      split_skip_if_fits,
                      hip_ordinal,
                      uma,
                      std::chrono::seconds(silence_s),
                      answer_room,
                      static_cast<std::size_t>(repeat_stop_tokens),
                      max_output.has_value() ? std::optional<std::size_t>{static_cast<std::size_t>(*max_output)}
                                             : std::optional<std::size_t>{},
                      min_output.has_value() ? std::optional<std::size_t>{static_cast<std::size_t>(*min_output)}
                                             : std::optional<std::size_t>{},
                      std::move(aliases),
                      std::move(served_name)};
  AlertsConfig alerts_config = read_alerts(read, draft, errors);
  SearchConfig search_config = read_search(read, draft, errors);
  if (!errors.empty()) return errors;
  auto config_ptr = std::shared_ptr<const FrontendConfig>(
      new FrontendConfig(std::move(listener), limits, std::move(roots), std::move(*token), std::move(engine),
                         PullSettings{std::move(downloader), std::move(proxy), allow_private,
                                      static_cast<std::uint64_t>(max_total), std::chrono::seconds(max_duration),
                                      std::chrono::seconds(stall), std::chrono::seconds(connect), static_cast<std::uint32_t>(connections)},
                         LoggingConfig{log_requests}, std::move(sampling_defaults), reasoning, std::move(session),
                         replicas, vision, std::move(glm), UpdateSettings{std::move(update_source)},
                         std::move(models_config), std::move(alerts_config), std::move(search_config)));
  return ValidatedConfig{std::move(config_ptr)};
}

std::string describe(const FrontendConfig& c) {
  std::string out;
  const auto line = [&out](const std::string_view key, const std::string& value) {
    out.append(key).append(" = ").append(value).push_back('\n');
  };
  const auto root_text = [](const CanonicalRoot& root) { return json_quoted(path_to_utf8(root.path())); };
  const auto root_or_unset = [&root_text](const std::optional<CanonicalRoot>& root) {
    return root.has_value() ? root_text(*root) : std::string{"(unset)"};
  };
  const auto path_or_unset = [](const std::optional<std::filesystem::path>& p) {
    return p.has_value() ? json_quoted(path_to_utf8(*p)) : std::string{"(unset)"};
  };
  line("schema_version", "1");
  line("listener.host", json_quoted(c.listener.host.canonical_text()));
  line("listener.port", std::to_string(c.listener.port));
  line("listener.allow_remote", c.listener.allow_remote ? "true" : "false");
  line("listener.allow_wildcard_bind", c.listener.allow_wildcard_bind ? "true" : "false");
  line("listener.backlog", std::to_string(c.listener.backlog));
  const auto list_text = [](const std::vector<std::string>& values) {
    std::string text = "[";
    for (std::size_t i = 0; i < values.size(); ++i) text += (i ? "," : "") + json_quoted(values[i]);
    return text + "]";
  };
  line("listener.cors_origins", list_text(c.listener.cors_origins));
  line("listener.host_aliases", list_text(c.listener.host_aliases));
  line("listener.web_ui", c.listener.web_ui ? "true" : "false");
  line("listener.loopback_without_key", c.listener.loopback_without_key ? "true" : "false");
  line("listener.monitor", c.listener.monitor ? "true" : "false");
  line("limits.http.request_deadline_ms", std::to_string(c.limits.http.request_deadline.count()));
  line("limits.http.max_request_bytes", std::to_string(c.limits.http.max_request_bytes));
  line("limits.http.max_header_bytes", std::to_string(c.limits.http.max_header_bytes));
  line("limits.http.max_headers", std::to_string(c.limits.http.max_headers));
  line("limits.http.max_request_line_bytes", std::to_string(c.limits.http.max_request_line_bytes));
  line("limits.http.header_deadline_ms", std::to_string(c.limits.http.header_deadline.count()));
  line("limits.http.write_stall_ms", std::to_string(c.limits.http.write_stall.count()));
  line("limits.http.response_write_deadline_ms", std::to_string(c.limits.http.response_write_deadline.count()));
  line("limits.http.stream_keepalive_ms", std::to_string(c.limits.http.stream_keepalive.count()));
  line("limits.chat.max_body_bytes", std::to_string(c.limits.chat.max_body_bytes));
  line("limits.chat.max_messages", std::to_string(c.limits.chat.max_messages));
  line("limits.chat.max_message_bytes", std::to_string(c.limits.chat.max_message_bytes));
  line("limits.chat.max_prompt_tokens", std::to_string(c.limits.chat.max_prompt_tokens));
  line("limits.chat.max_completion_tokens", std::to_string(c.limits.chat.max_completion_tokens));
  line("limits.chat.default_max_tokens", std::to_string(c.limits.chat.default_max_tokens));
  line("limits.chat.max_generated_bytes", std::to_string(c.limits.chat.max_generated_bytes));
  line("limits.engine.ready_timeout_s", std::to_string(c.limits.engine.ready_timeout.count()));
  line("limits.engine.generation_timeout_s", std::to_string(c.limits.engine.generation_timeout.count()));
  line("limits.engine.stop_drain_timeout_s", std::to_string(c.limits.engine.stop_drain_timeout.count()));
  const auto& cc = c.limits.concurrency;
  line("limits.concurrency.max_concurrent_requests", std::to_string(cc.max_concurrent_requests));
  line("limits.concurrency.max_queued_generations",
       std::to_string(cc.max_queued_generations) + (cc.queue_derived ? " (derived)" : ""));
  line("limits.concurrency.queue_timeout_ms", std::to_string(cc.queue_timeout.count()));
  line("limits.concurrency.max_connections_per_remote_address",
       std::to_string(cc.max_connections_per_remote_address) + (cc.remote_cap_derived ? " (derived)" : ""));
  line("roots.models", root_text(c.roots.models));
  line("roots.tokenizer_pack", root_or_unset(c.roots.tokenizer_pack));
  line("roots.secret", root_or_unset(c.roots.secret));
  line("roots.logs", path_or_unset(c.roots.logs));
  line("roots.cache", path_or_unset(c.roots.cache));
  line("roots.state", path_or_unset(c.roots.state));
  line("token.source", json_quoted((c.token.kind() == SecretSourceKind::environment_variable ? "env:" : "file:") +
                              c.token.location()));
  line("engine.profile", c.engine.profile ? json_quoted(profile_text(*c.engine.profile)) : std::string{"(unset)"});
  line("engine.executable", json_quoted(path_to_utf8(c.engine.executable.path())));
  line("engine.model_id", json_quoted(c.engine.model_id.value()));
  std::string args = "[";
  for (const auto& arg : c.engine.args) args += (args.size() > 1U ? "," : "") + json_quoted(arg);
  line("engine.args", args + "]");
  std::string libs = "[";
  for (const auto& directory : c.engine.lib_dirs) libs += (libs.size() > 1U ? "," : "") + json_quoted(path_to_utf8(directory));
  line("engine.lib_dirs", libs + "]");
  std::string gpu_list;
  for (const unsigned device : c.engine.gpus) gpu_list += (gpu_list.empty() ? "" : ",") + std::to_string(device);
  line("engine.gpu", gpu_list.empty() ? std::string{"(unset)"} : json_quoted(gpu_list));
  line("engine.allow_new_privileges", c.engine.allow_new_privileges ? "true" : "false");
  line("pull.downloader", json_quoted(path_to_utf8(c.pull.downloader)));
  line("pull.proxy", c.pull.proxy ? json_quoted(render_proxy(*c.pull.proxy)) : std::string{"(unset)"});
  line("pull.allow_private_addresses", c.pull.allow_private_addresses ? "true" : "false");
  line("pull.max_total_bytes", std::to_string(c.pull.max_total_bytes));
  line("pull.max_duration_s", std::to_string(c.pull.max_duration.count()));
  line("pull.stall_timeout_s", std::to_string(c.pull.stall_timeout.count()));
  line("pull.connect_timeout_s", std::to_string(c.pull.connect_timeout.count()));
  line("pull.connections", std::to_string(c.pull.connections));
  line("logging.requests", c.logging.requests ? "true" : "false");
  // SP5 M1 Task 4: the defaults.sampling.* rows; an unset row stays unset and
  // is never sent (the engine keeps its own default).
  const auto decimal_or_unset = [](const std::optional<double>& value) {
    return value.has_value() ? decimal_text(*value) : std::string{"(unset)"};
  };
  const auto integer_or_unset = [](const std::optional<std::int64_t>& value) {
    return value.has_value() ? std::to_string(*value) : std::string{"(unset)"};
  };
  line("defaults.sampling.temperature", decimal_or_unset(c.sampling.temperature));
  line("defaults.sampling.top_p", decimal_or_unset(c.sampling.top_p));
  line("defaults.sampling.min_p", decimal_or_unset(c.sampling.min_p));
  line("defaults.sampling.presence_penalty", decimal_or_unset(c.sampling.presence_penalty));
  line("defaults.sampling.repetition_penalty", decimal_or_unset(c.sampling.repetition_penalty));
  line("defaults.sampling.frequency_penalty", decimal_or_unset(c.sampling.frequency_penalty));
  line("defaults.sampling.top_k", integer_or_unset(c.sampling.top_k));
  line("defaults.sampling.penalty_last_n", integer_or_unset(c.sampling.penalty_last_n));
  line("defaults.sampling.seed", integer_or_unset(c.sampling.seed));
  // SP5 M1 Task 5: the reasoning.* family and engine.fit_max_tokens.
  line("reasoning.effort",
       !c.reasoning.effort.has_value()                  ? std::string{"(unset)"}
       : *c.reasoning.effort == ReasoningEffort::none   ? "none"
       : *c.reasoning.effort == ReasoningEffort::low    ? "low"
       : *c.reasoning.effort == ReasoningEffort::medium ? "medium"
                                                        : "high");
  line("reasoning.budget_tokens", std::to_string(c.reasoning.budget_tokens));
  line("reasoning.close_retry", c.reasoning.close_retry ? "true" : "false");
  line("reasoning.loop_recovery",
       c.reasoning.loop_recovery == LoopRecovery::off      ? "off"
       : c.reasoning.loop_recovery == LoopRecovery::stop   ? "stop"
                                                           : "recover");
  line("reasoning.effort_position", c.reasoning.effort_position == EffortPosition::start ? "start" : "end");
  line("reasoning.anthropic_thinking",
       c.reasoning.anthropic_thinking == AnthropicThinking::model ? "model" : "on_request");
  // SP5 M1 Task 9: the session family.
  line("session.slot_save_path", c.session.slot_save_path.empty() ? std::string{"(unset)"} : json_quoted(c.session.slot_save_path));
  line("session.expert_profile_save", c.session.expert_profile_save.empty() ? std::string{"(unset)"} : json_quoted(c.session.expert_profile_save));
  line("session.expert_profile_save_every", std::to_string(c.session.expert_profile_save_every));
  line("session.tool_call_recovery", c.session.tool_call_recovery ? "true" : "false");
  line("session.env_to_user", c.session.env_to_user ? "true" : "false");
  line("session.codex_thread_titles", c.session.codex_thread_titles ? "true" : "false");
  line("session.codex_compaction_cache", c.session.codex_compaction_cache ? "true" : "false");
  line("session.keep_files", std::to_string(c.session.keep_files));
  // SP5 M1 Task 10: engine-internal parallelism (the C-4 narrowing's surviving half).
  line("replicas.parallel", std::to_string(c.replicas.parallel));
  // SP5 M1 Task 11: the vision family (D-9 split).
  line("vision.enabled", c.vision.enabled ? "true" : "false");
  line("vision.max_side", std::to_string(c.vision.max_side));
  line("vision.max_n", std::to_string(c.vision.max_n));
  line("vision.need_mib", c.vision.need_mib.has_value() ? std::to_string(*c.vision.need_mib) : std::string{"unset"});
  line("vision.no_flash_attn", c.vision.no_flash_attn ? "true" : "false");
  line("vision.resident", c.vision.resident ? "true" : "false");
  line("vision.threads", std::to_string(c.vision.threads));
  line("vision.ready_timeout_s", std::to_string(c.vision.ready_timeout_s));
  line("vision.mmproj", c.vision.mmproj.has_value() ? json_quoted(path_to_utf8(*c.vision.mmproj)) : std::string{"(unset)"});
  line("vision.gpu", c.vision.gpu ? "true" : "false");
  // SP5 M2 Task 3: the glm.* family (spec §D-2).
  line("glm.thinking_budget", std::to_string(c.glm.thinking_budget));
  line("glm.max_context", std::to_string(c.glm.max_context));
  line("glm.kv", c.glm.kv);
  line("glm.ram_headroom_gb", std::to_string(c.glm.ram_headroom_gb));
  line("glm.idle_unload_s", std::to_string(c.glm.idle_unload_s));
  line("glm.reasoning_effort",
       !c.glm.reasoning_effort.has_value()
           ? std::string{"(unset)"}
           : *c.glm.reasoning_effort == GlmReasoningEffort::none ? "none"
           : *c.glm.reasoning_effort == GlmReasoningEffort::low  ? "low"
           : *c.glm.reasoning_effort == GlmReasoningEffort::high ? "high"
                                                                  : "max");
  line("glm.fit_max_tokens", c.glm.fit_max_tokens ? "true" : "false");
  line("glm.experimental_speed_projection", c.glm.experimental_speed_projection ? "true" : "false");
  std::string glm_gpus;
  for (const unsigned device : c.glm.gpus) glm_gpus += (glm_gpus.empty() ? "" : ",") + std::to_string(device);
  line("glm.gpu", glm_gpus.empty() ? std::string{"(unset)"} : json_quoted(glm_gpus));
  line("glm.layer_split", json_quoted(c.glm.layer_split));
  // Model launcher (plan 2026-10-23 Task 1).
  std::string cards = "[";
  for (const auto& card : c.models.cards) cards += (cards.size() > 1U ? "," : "") + json_quoted(path_to_utf8(card));
  line("models.cards", cards + "]");
  line("models.start", c.models.start.has_value() ? json_quoted(*c.models.start) : std::string{"(unset)"});
  line("models.control", c.models.control ? "true" : "false");
  line("models.control_remote", c.models.control_remote ? "true" : "false");
  std::vector<std::string> networks;
  for (const auto& network : c.models.control_remote_networks) networks.push_back(ip_network_text(network));
  line("models.control_remote_networks", list_text(networks));
  const auto path_line = [](const std::optional<std::filesystem::path>& p) {
    return p.has_value() ? json_quoted(path_to_utf8(*p)) : std::string{"(unset)"};
  };
  line("models.downloads", c.models.downloads ? "true" : "false");
  line("models.downloads_root", path_line(c.models.downloads_root));
  line("models.prepare_python", path_line(c.models.prepare_python));
  line("models.prepare_tools", path_line(c.models.prepare_tools));
  line("models.download_engine_card",
       c.models.download_engine_card.has_value() ? json_quoted(*c.models.download_engine_card) : std::string{"(unset)"});
  line("models.default_engine", path_line(c.models.default_engine));
  line("models.default_root", path_line(c.models.default_root));
  if (email_alerts_built()) {
    const auto& email = c.alerts.email;
    const auto list = [](const std::vector<std::string>& items) {
      std::string text = "[";
      for (std::size_t i = 0; i < items.size(); ++i) text += (i == 0U ? "" : ",") + json_quoted(items[i]);
      return text + "]";
    };
    line("alerts.email.enabled", email.enabled ? "true" : "false");
    line("alerts.email.host", email.host.empty() ? std::string{"(unset)"} : json_quoted(email.host));
    line("alerts.email.port", std::to_string(email.port));
    line("alerts.email.security", email.security == SmtpSecurity::ssl ? "ssl" : "starttls");
    line("alerts.email.username", email.username.empty() ? std::string{"(unset)"} : json_quoted(email.username));
    line("alerts.email.from", email.from.empty() ? std::string{"(unset)"} : json_quoted(email.from));
    line("alerts.email.to", list(email.to));
    line("alerts.email.events", list(email.events));
  }
  line("glm.tokenizer", c.glm.tokenizer.has_value() ? json_quoted(*c.glm.tokenizer) : std::string{"(unset)"});
  line("glm.model_name", c.glm.model_name.has_value() ? json_quoted(*c.glm.model_name) : std::string{"(unset)"});
  line("engine.fit_max_tokens", c.engine.fit_max_tokens ? "true" : "false");
  // SP5 M1 Task 6: the lifecycle family (runconfig.py:35-38).
  line("engine.lazy_load", c.engine.lazy_load ? "true" : "false");
  line("engine.idle_unload_s", std::to_string(c.engine.idle_unload.count()));
  line("engine.mtp", c.engine.mtp ? "true" : "false");
  // SP5 M1 Task 7: the VRAM / GPU / elastic family.
  line("engine.vram_reserve_mib", integer_or_unset(c.engine.vram_reserve_mib));
  line("engine.min_free_vram_mib", std::to_string(c.engine.min_free_vram));
  line("engine.vram_elastic", c.engine.vram_elastic ? "true" : "false");
  line("engine.vram_segment_mib", integer_or_unset(c.engine.vram_segment_mib));
  line("engine.elastic", c.engine.elastic ? "true" : "false");
  line("engine.elastic_idle_s", std::to_string(c.engine.elastic_idle.count()));
  line("engine.elastic_mib", std::to_string(c.engine.elastic_mib));
  line("engine.gpu_order", c.engine.gpu_order == GpuOrder::as_given ? "as_given" : "auto");
  line("engine.gpu_policy",
       c.engine.gpu_policy == GpuTurnPolicy::keep    ? "keep"
       : c.engine.gpu_policy == GpuTurnPolicy::shrink ? "shrink"
       : c.engine.gpu_policy == GpuTurnPolicy::unload ? "unload"
                                                      : "auto");
  line("engine.layer_split", c.engine.layer_split);
  line("engine.split_skip_if_fits", c.engine.split_skip_if_fits ? "true" : "false");
  line("engine.hip_ordinal", integer_or_unset(c.engine.hip_ordinal));
  line("engine.uma", c.engine.uma ? "true" : "false");
  // SP5 M1 Task 8: the timeout / request-shape family.
  line("engine.silence_s", std::to_string(c.engine.silence.count()));
  line("engine.answer_room", c.engine.answer_room ? "true" : "false");
  line("engine.repeat_stop_tokens", std::to_string(c.engine.repeat_stop_tokens));
  line("engine.max_tokens", c.engine.max_tokens ? std::to_string(*c.engine.max_tokens) : std::string{"unset"});
  line("engine.min_tokens", c.engine.min_tokens ? std::to_string(*c.engine.min_tokens) : std::string{"unset"});
  line("model.aliases", list_text(c.engine.aliases));
  line("model.name", c.engine.served_name ? json_quoted(*c.engine.served_name) : std::string{"(unset)"});
  return out;
}

std::vector<ForcedEnvironmentValue> glm_environment_knobs(const FrontendConfig& config) {
  std::vector<ForcedEnvironmentValue> knobs;
  // maya-ref docs/SETTINGS.md:44: the config's "layer_split" sets STRATA_GLM_SPLIT
  // the same way; "auto" (Maya's own default, serve/server.py:1276) leaves the
  // §4.9 forced single-device default in place — Task 7 renders the split argv
  // and checks the count against the cards.
  if (config.glm.layer_split != "auto") knobs.push_back({std::string{"STRATA_GLM_SPLIT"}, config.glm.layer_split});
  // SP5-P2 Task 4 (D-7; SP6 §4.9 M2): the RAM headroom row always projects —
  // the typed config's default 16 IS the forced-safe value, and the operator
  // moves it (6..1024) through glm.ram_headroom_gb (SETTINGS.md:38).
  knobs.push_back({std::string{"STRATA_GLM_RAM_HEADROOM_GB"}, std::to_string(config.glm.ram_headroom_gb)});
  // the CPU lane as a calibration (or the operator) set it (maya-ref docs/SETTINGS.md: Maya's calibrate_glm.py writes
  // the same two into a config's env); unset rows leave the engine's own measurement in place
  if (config.glm.pcie_share) {
    char share[16];
    const auto [end, error] = std::to_chars(share, share + sizeof(share), *config.glm.pcie_share, std::chars_format::fixed, 3);
    if (error == std::errc{}) knobs.push_back({std::string{"STRATA_GLM_PCIE_SHARE"}, std::string{share, end}});
  }
  if (config.glm.cpu_lane) knobs.push_back({std::string{"STRATA_GLM_CPU_LANE"}, std::to_string(*config.glm.cpu_lane)});
  // SP5 M2 Task 8 (D-8): the vision turn — an on-demand encoder (vision.enabled,
  // not resident; maya-ref serve/server.py:4015) runs in memory the engine lends
  // from its expert pool, sized by STRATA_GLM_VISION_LEND_MB (server.py:4026).
  // The typed source is vision.need_mib (the encoder's VRAM need, images.py:156
  // pattern); unset means the owner-box measurement the kit records, so nothing
  // projects and the render gate (engine_environment) keeps the row out.
  if (config.vision.enabled && !config.vision.resident && config.vision.need_mib)
    knobs.push_back({std::string{"STRATA_GLM_VISION_LEND_MB"}, std::to_string(*config.vision.need_mib)});
  return knobs;
}

Config listener_config(const FrontendConfig& c) {
  // A keyless listener says so; HttpSocketServer::bind re-validates it (loopback only).
  return Config{c.listener.host.canonical_text(), c.listener.port, !c.listener.loopback_without_key,
                c.listener.host.scope() == AddressScope::loopback, c.listener.allow_remote,
                c.listener.allow_wildcard_bind, c.listener.loopback_without_key};
}

ConfigErrors create_runtime_roots(const FrontendConfig& c) {
  ConfigErrors errors;
  const std::pair<const char*, const std::optional<std::filesystem::path>*> roots[] = {
      {"roots.logs", &c.roots.logs}, {"roots.cache", &c.roots.cache}};
  for (const auto& [key, root] : roots) {
    if (!root->has_value()) continue;
    auto opened = open_root(**root, RootPolicy::create_private);
    if (const auto* failure = std::get_if<PathError>(&opened)) add(errors, key, ConfigIssueCode::invalid_value, failure->message);
  }
  return errors;
}

}  // namespace secure_frontend
