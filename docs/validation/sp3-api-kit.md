# SP3 API validation kit — offline GPU proof (spec §9.5)

This procedure runs **on the owner's GPU box** against a real model. GitHub Actions are disabled
(owner ruling 2026-10-20) and the CI cross-build is compile-only, so four facts can never be proven
by CI and belong in the **release record**, not in any test suite:

1. **Real tokenizer splits of multi-byte UTF-8** — real engines cut tokens mid-character; the stream
   must still be valid UTF-8 and equal to the non-stream text at a fixed seed.
2. **`STOP` latency during a long prefill (risk R4)** — the engine must honour `STOP` while it is
   still prefilling, or the generation slot stays pinned until the drain timeout.
3. **Whether the engine applies `GEN` `temperature`/`top_p`/`seed`** — the frontend forwards them
   (decision B6); only a real sampler can show the forwarding is honoured.
4. **Real clients against a real model** — the `openai` Python SDK and `curl -N` must consume the
   stream the way SDKs do, and must **raise** (not silently accept) a truncated stream.

Every command below is offline: the server listens on loopback and the probe talks only to that
loopback address (the client library refuses any other address by construction).

## 1. Setup

- Build the probe **from the release commit** (the same commit that produced the binaries under test):
  - Linux: `cmake --build --preset linux-gcc --target strata-api-probe`
  - Windows (MSYS2): `cmake --build --preset windows-msys2-clang --target strata-api-probe`
  The probe is test-only: it is never installed and never linked into `strata`.
- Start the server with the real model: `strata serve` (note the listening port `P` —
  the run's `server.json`/startup log prints it) and make sure the token file `T` (owner-only
  permissions, `0600`) holds the API token.
- All probe commands take: `--port P --token-file T --model M [--host 127.0.0.1|::1]` and print one
  line: `PASS <mode> <facts>` (exit 0), `FAIL <mode> <reason>` (exit 1) or a usage line (exit 2).

## 2. Egress snapshot — before

Capture the server's sockets before anything runs; save the output into the record.

- Linux: `ss -tunap | grep strata`
- Windows: `netstat -abno` (find the `strata.exe` PID lines)

## 3. Probe runs

Run each command once; every line must print `PASS`. Record the numbers verbatim.

| Command | Pass criterion | Record |
| --- | --- | --- |
| `strata-api-probe --port P --token-file T --model M stream` | `PASS`; `[DONE]` and the terminal chunk seen | events, bytes, total_ms |
| `… nonstream` | `PASS` | bytes |
| `… cancel-after=5` | `PASS` | next_request_ms vs `limits.engine.stop_drain_timeout_s` |
| `… cancel-during-prefill` | `PASS` | next_request_ms (risk R4); a value near the drain timeout means raise `stop_drain_timeout_s` |
| `… concurrent=3` | `PASS` (FIFO) | — |
| `… utf8` | `PASS`: stream text equals non-stream text at seed 7, temperature 0, valid UTF-8 | bytes |
| `… seed` | `PASS`: same text twice at seed 42 | — |

## 4. Real clients

Python (`pip install openai`, no network needed — the base URL is loopback):

```python
from openai import OpenAI
client = OpenAI(base_url="http://127.0.0.1:P/v1/", api_key=open("T").read().strip())
stream = client.chat.completions.create(model="M", messages=[{"role": "user", "content": "hi"}], stream=True)
text = "".join(part.delta.content or "" for part in stream)   # must run to the end without raising
print(len(text))
```

Then **kill the server mid-stream**: start the same call and `strata stop` (or `Ctrl-C` the server)
before it finishes — the SDK must **raise** (an incomplete-chunk / connection error), not return a
truncated answer. A cut stream carries no terminal chunk and no `[DONE]` by design (spec §2.6), so
accepting it silently would be a data-integrity bug.

curl (events must print as they arrive, and the output must end with `data: [DONE]`):

```bash
curl -N -H "Authorization: Bearer $(cat T)" -H 'Content-Type: application/json' \
  -d '{"model":"M","messages":[{"role":"user","content":"hi"}],"stream":true}' \
  http://127.0.0.1:P/v1/chat/completions
```

## 5. Egress snapshot — during and after

Run the same snapshot commands of §2 **while a stream is running** (start a long `stream` probe in
one window, snapshot in another) and again **after `strata stop`**. Expected: the server's only TCP
sockets are its listener and its accepted loopback connections; **no UDP**, and no connection to any
address outside loopback. After `strata stop` the server owns no sockets at all.

## 6. Record template

Copy into the release record, filling every field:

```text
date:                    UTC
commit (git rev-parse):
box / OS / GPU:
model + tokenizer:
strata version:

probe lines (verbatim, one per mode):
  PASS stream  ...
  PASS nonstream ...
  PASS cancel-after=5 ...
  PASS cancel-during-prefill ...
  PASS concurrent=3 ...
  PASS utf8 ...
  PASS seed ...

egress snapshots: before / during / after   (paste outputs)

openai-python: complete-run = ... ; mid-stream-kill raised = <exception type>
curl -N: last line = ...

notes / follow-ups:
```
