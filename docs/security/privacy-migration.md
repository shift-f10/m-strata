# Privacy migration inventory

## Purpose

This is a deliberately narrow inventory of known network-capable paths in the
Strata `v0.2.2` baseline. It prevents the native frontend migration from
accidentally retaining an implicit outbound behavior. It is not an assertion
that these paths are malicious: several are normal, user-visible installer or
optional image/MCP features. They are incompatible with the secure release
profile until replaced by a reviewed explicit action.

## Secure-release rule

The default build and normal service lifecycle must not make an outbound
connection or DNS lookup. A release may include no analytics, telemetry upload,
crash upload, update check, remote configuration, advertising, or automatic
download client.

The only allowed future network operation is the user-invoked `acquire`
command specified in [`threat-model.md`](threat-model.md). It must be separate
from the server and its HTTP API.

## Baseline paths to quarantine or replace

| Baseline area | Current capability | Secure migration action |
| --- | --- | --- |
| `upstream/START-HERE.bat` | May download Python from `python.org`. | Do not use in secure packaging. Ship a native executable or make dependency acquisition a separate explicit action. |
| `upstream/setup.py` | Downloads models, engine releases, llama.cpp, Python packages, and GPU support assets; performs network probes. Since `v0.2.2` it also re-reads a model file that fails its SHA-256 from the same host in HTTP `Range` pieces and rewrites the blocks that differ (`repair_download`), and, when a release publishes no flat engine zip, runs `strata.py update --no-control` to fetch the engine package from the signed update feed (`engine_from_feed`). | Replace with offline local-artifact inventory plus the explicit digest-verified `acquire` command. Do not reuse its startup flow. |
| `upstream/update.sh`, `upstream/UPDATE.bat` | Updates from GitHub/git or a ZIP. Since `v0.2.2` the `setup.py --update` step they run can also fetch the engine from the signed update feed. | Exclude from secure distribution until a separately designed, signed, user-invoked updater exists. No automatic update check. |
| `upstream/serve/server.py` image handling | Can fetch HTTP(S) image URLs supplied in requests. | Native API accepts local/base64 image bytes only in the first secure release. Reject remote image URLs. |
| `upstream/serve/mcp.py` | Can contact configured MCP HTTP endpoints and launch tool servers. | Exclude from default native build. MCP, tools, and plugins remain disabled. |
| `upstream/serve/telemetry.py`, `upstream/serve/prometheus.py` | Local hardware/request monitoring and metrics surfaces. | These are not evidence of outbound analytics, but the native release must make local metrics opt-in, token-protected, and documented. Done 2026-10-22 (owner request, session log step 16) as `listener.monitor`: off by default, `GET /metrics` behind the key, documented in the README. In memory only: the last 100 generations' token counts and the engine's timings (never their text), CPU/RAM from the OS, the GPU from the system's `nvidia-smi` as a separate child (no NVML in the server, unlike telemetry.py). No Prometheus text format. |
| `upstream/serve/web/` | Browser UI includes static external links. | Not shipped as is. Reintroduced 2026-10-21/22 (owner decisions) as `listener.web_ui` (off by default): our page with upstream's look. Its UI kit (MIT) and Outfit font (OFL) are copied from the pin and the rest is written for this server (provenance: docs/PROVENANCE.md). Everything is bundled from `secure_frontend/web/` and embedded at build time. No third-party or remote resource: upstream's GitHub link, Images, MCP and settings views were dropped; its Monitor came back with `listener.monitor` (session log step 16, built from DOM nodes like the rest). Tests check that no other host is named and there is no inline code. Self-only CSP. |
| `upstream/setup.sh` (macOS branch, since `v0.1.44`) | Downloads a standalone Python from `github.com/astral-sh/python-build-standalone` (SHA-256 pinned). | Same as `START-HERE.bat`: not used in secure packaging. |
| `upstream/metal/setup_mac.py` (new in `v0.1.44`, run by `setup.py` on macOS) | Downloads Apple's metal-cpp zip, model shards and the vision mmproj; builds the engine. | Same as `setup.py`. macOS is not a target of this project. |
| Image generation (`upstream/tools/image/`, `upstream/serve/images.py`, `setup.py --images`, new in `v0.1.44`) | Setup downloads stable-diffusion.cpp/ggml zips (GitHub), the LunarG Vulkan SDK and image models (Hugging Face); the server adds `POST /v1/images/generations` and `GET /v1/images/files/<id>` and runs a local `sd-cli` subprocess when the config has an `images` section. | Exclude from the native build; no image generation in the first secure release. |
| `upstream/strata_cli/update_cmd.py`, `upstream/strata_control/updater.py` (with `transfer.py`, `feed.py`; present at `v0.2.1.3`) | `strata update` downloads the Ed25519-signed `strata-feed.json` from GitHub Releases and the engine and app packages it lists. Since `v0.2.2` `setup.py` runs it. | Same as `update.sh`: exclude from secure distribution; not used by the native build. |
| `upstream/apps/host` (desktop host app; `apps/desktop` is its Tauri shell; both present at `v0.2.1.3`) | Cloud model APIs, web search, Hugging Face and GitHub downloads (image models, llama.cpp, `cloudflared`), a GitHub release check, a LAN share listener on `0.0.0.0`, and a Cloudflare tunnel (`cloudflared`, quick `trycloudflare.com` or named). Since `v0.2.2` the LAN share and the tunnel start without an API key (a warning only), and the host probes a free share port on `0.0.0.0` at start. | Exclude from secure packaging (SP7 stages neither tree); not used by the native build. |

## Verification work required before release

1. Add hermetic tests for `PRIV-01` through `PRIV-04` in
   [`../../tests/security/README.md`](../../tests/security/README.md).
2. Monitor socket creation and DNS resolution around startup, inference,
   failure/restart, and shutdown on Windows and Linux.
3. Produce a release manifest that lists every linked component with potential
   network capability and states whether it is compiled in, reachable, and
   user-triggered.
4. Review the final executable's imports/dependencies and configuration surface
   for telemetry, update, crash-reporting, and remote-configuration clients.
5. Keep legacy Python installer/service files out of the secure package until
   their replacement has passed these checks.

## Evidence baseline

The current inventory was derived from direct source review on 2026-10-11
Pacific time, re-reviewed against `v0.1.44.1` on 2026-10-14 (three rows added),
and re-reviewed against `v0.2.1.3` on 2026-10-20 (no row changed: every named
reference still exists; upstream added `apps/desktop` and `apps/host`, which
are not part of any package we stage — SP7 packaging must exclude them).
It was re-reviewed against `v0.2.2` on 2026-10-21 (every named reference
still exists and no new host appears; the `setup.py` row grew by a ranged
model-file repair and a signed-feed engine fetch, which `update.sh`/`UPDATE.bat`
now reach; two rows added: the `strata update` feed client that fetch runs,
and `apps/host`, whose LAN share and Cloudflare tunnel no longer require an
API key; the new `strata_control` modules `models_move.py`, `gpu_roles.py`
and `vram_fit.py` and `tools/idle_gpu_ab.py` are local only).
Representative network-capable references include
`upstream/START-HERE.bat`, `upstream/setup.py`, `upstream/UPDATE.bat`, `upstream/update.sh`, `upstream/serve/server.py`, and
`upstream/serve/mcp.py` (the baseline is the `upstream/` submodule). Future agents must update this inventory when adding or
removing a network-capable code path.
