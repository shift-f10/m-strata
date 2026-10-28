# Our engine — the dispatcher

One stable API that runs several model families by spinning up the right **backend engine** on demand and
swapping when you change model family. Each backend is a **proven engine, unchanged** — qwen runs on the
**Strata** lineage (`C:\Strata`, `strata.exe`), GLM runs on the **Maya** lineage (`C:\project-maya`, `maya.exe`).
The dispatcher is the only new piece: it maps the request's `model` to a family, keeps that family's engine
resident, and on a family switch stops the current one (freeing the GPU) and starts the other.

This is the first testable cut: the dispatcher over the proven backends. Next phases (not here yet): the
security review of the adopted source, and consolidating the shared base into one lightweight core.

## Run

```
C:\multi-strata\dispatcher\run-dispatch.bat
```

- Front API on `http://localhost:8080`. Lazy: the first request for a family loads that backend (tens of GB,
  1-3 minutes cold). `run-dispatch.bat --prewarm qwen` loads qwen at start instead.
- Registry: `families.json` (which engine/config/venv each family uses, the aliases that select it, the
  internal backend port). Edit it to point at different configs or add families.

## Use

It speaks the same OpenAI/Anthropic API the backends serve; the `model` field picks the family.

```
# qwen (Strata)
curl -s http://localhost:8080/v1/chat/completions -H "Authorization: Bearer strata-secret-key" ^
  -H "Content-Type: application/json" ^
  -d "{\"model\":\"qwen\",\"messages\":[{\"role\":\"user\",\"content\":\"Hello\"}],\"max_tokens\":64}"

# switch to GLM (Maya) - the dispatcher stops qwen, frees the GPU, starts GLM
curl -s http://localhost:8080/v1/chat/completions -H "Authorization: Bearer strata-secret-key" ^
  -H "Content-Type: application/json" ^
  -d "{\"model\":\"glm\",\"messages\":[{\"role\":\"user\",\"content\":\"Hello\"}],\"max_tokens\":64}"
```

Control plane:
- `GET  /dispatch/status` — the active family, which families are known, backend health.
- `POST /dispatch/switch` with `{"family":"glm"}` — pre-warm/switch without a generation request.

Each backend's own log is in `dispatcher/logs/backend-<family>.log`.

## Security (reviewed 2026-10-25)

The two backend engines (`C:\Strata` and `C:\project-maya` serve layers + their launch configs) were
security-reviewed before adoption: **no malware, no telemetry/phone-home, no exfiltration, no request-driven
code execution, no backdoor auth.** Safe as local backends. (The only engine-side surface is a request-supplied
image-URL fetch — an SSRF/DoS vector — which stays harmless while the backends are loopback-only; run the qwen
backend with Strata's offline / `--no-remote-images` if you ever accept untrusted image URLs.)

Dispatcher hardening applied / notes:
- The front binds **127.0.0.1 (loopback) by default** — the dispatcher's `/dispatch` control plane is
  unauthenticated, so it must not be LAN-exposed. `--host 0.0.0.0` opts into LAN exposure and prints a warning;
  only do that behind a firewall (and ideally add auth).
- Backends are always pinned to loopback (`--host 127.0.0.1`), regardless of a config's `host`.
- Set a strong `api_key` in `strata-iq3_s.json` (the shipped `strata-secret-key` is a weak placeholder). The
  Maya/glm config has no `api_key` — fine only while loopback-bound.

## Families (today)

| family | model | lineage | engine | config |
|---|---|---|---|---|
| `qwen` | qwen3.8-flash-next-iq3_s | Strata | `C:\Strata\strata.exe` | `C:\Strata\strata-iq3_s.json` |
| `glm`  | glm-5.3-flash | Maya | `C:\project-maya\build\maya.exe` | `C:\project-maya\maya-maya-s-v2-iq2_xxs.json` |

deepseek will be added on the Maya lineage once its model/config is in place.
