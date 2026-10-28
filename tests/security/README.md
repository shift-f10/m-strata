# Security acceptance specification

These are implementation-independent acceptance tests for the portable
frontend. Automate them in the Windows and Linux CI matrix. Tests marked MUST
block release; SHOULD tests block a security-related change unless a documented
exception is approved.

Test fixtures must use disposable roots and a fake engine that records argv,
environment, inherited handles/descriptors, bind address, and received input.
Never place real tokens in fixtures or CI output.

## Network and token tests (MUST)

| ID | Given / action | Expected result |
|---|---|---|
| NET-01 | Start with no bind setting. | Only loopback is listening; no wildcard or LAN listener exists. |
| NET-02 | Supply `0.0.0.0`, `::`, LAN address, or public address without explicit remote opt-in. | Startup fails with a clear non-secret error. |
| NET-03 | Supply `0.0.0.0` or `::` with regular remote opt-in but no dedicated wildcard opt-in. | Startup fails; wildcard exposure cannot reuse the ordinary remote-listening switch. |
| AUTH-01 | Request every public route with no token, malformed authorization, or wrong token. | `401`/`403`; no route-specific data or model metadata is returned. |
| AUTH-02 | Request with a valid token. | Authorized request succeeds; token is absent from response and logs. |
| AUTH-03 | Request using token strings with different prefix/suffix lengths repeatedly. | Authentication behavior does not expose a length/prefix oracle; implementation uses constant-time comparison. |
| AUTH-04 | Enable unauthenticated development mode with a non-loopback bind. | Startup fails. |
| DOS-01 | Send oversize headers/body, deeply nested JSON, invalid chunking, and slow/incomplete body. | Connection/request is bounded and rejected; service remains healthy. |
| DOS-02 | Exceed configured concurrent/streaming request limits, then cancel a stream. | Excess is rejected predictably; resources are released and service remains responsive. |

## Host, Origin, and CORS tests (MUST)

| ID | Given / action | Expected result |
|---|---|---|
| WEB-01 | Send valid-token request with unknown, duplicate, malformed, IP-literal, or control-byte `Host`. | Request is rejected; no permissive fallback occurs. |
| WEB-02 | Send same-origin allowed `Host` and token. | Normal API operation succeeds. |
| WEB-03 | Send browser request with untrusted `Origin`, including `null`, lookalike subdomain, mixed case, default-port variant, and duplicate header. | Request is rejected; no CORS allow header is emitted. |
| WEB-04 | Use default configuration with a permitted API request. | No `Access-Control-Allow-Origin` header exists. |
| WEB-05 | Configure one allowed origin and send it, then send a different origin. | Only exact configured origin is allowed; arbitrary origin is never reflected and credentialed wildcard is never emitted. |
| WEB-06 | Send cross-origin preflight/state-changing requests. | Only explicitly allowed origin/method/header combinations succeed; unauthorized cross-origin state change fails. |

## Configuration and path tests (MUST)

| ID | Given / action | Expected result |
|---|---|---|
| CFG-01 | Use unknown, duplicate, type-confused, missing-required, or out-of-range configuration values. | Validation fails before listener/child starts. |
| CFG-02 | Use model/log/cache paths with `..`, absolute escape, NUL/control bytes, or a sibling-prefix trick. | Validation rejects them and writes nothing outside configured roots. |
| CFG-03 | Point a permitted-looking path through a symlink (Linux) or junction/reparse point (Windows) outside root. | Validation rejects it; no external file is opened/written. |
| CFG-04 | On Windows use drive-relative, UNC, `\\?\\`, reserved device, and alternate-data-stream forms. | Unsafe forms are rejected. |
| CFG-05 | Configure a token then inspect process list, child argv/environment, logs, errors, and metrics. | Token is absent from all observable locations except approved protected storage. |
| CFG-06 | Inspect the long-lived parsed configuration after startup. | It contains secret-source/validation metadata only, not a plaintext token copy. |

## Download and supply-chain tests (MUST)

| ID | Given / action | Expected result |
|---|---|---|
| DL-01 | Start service and serve an inference request without invoking acquire. | No network connection/download is attempted. |
| DL-02 | Acquire a manifest with a correct digest. | HTTPS artifact is staged, byte-limited, SHA-256 verified, then atomically published under model root. |
| DL-03 | Acquire wrong digest, truncated bytes, oversize bytes, HTTP URL, disallowed redirect, or invalid manifest. | Command fails; no accepted artifact exists and prior valid artifact remains intact. |
| DL-04 | Interrupt acquire during download or verification. | Temporary data is cleaned or safely quarantined; it is never selected for loading. |
| DL-05 | Inspect build/release metadata. | Pinned dependency/provenance data and SBOM are present and internally consistent. |

## Privacy and offline-operation tests (MUST)

| ID | Given / action | Expected result |
|---|---|---|
| PRIV-01 | Start, use, stop, and crash-restart the service while outbound networking is monitored. | No outbound connection, DNS lookup, telemetry, update check, crash upload, or remote configuration request occurs. |
| PRIV-02 | Submit prompts, completions, image inputs, and credentials using default logging. | Values do not appear in logs, metrics, crash material, or network traffic. |
| PRIV-03 | Invoke inference and API endpoints with values resembling acquire/update commands. | No download, update, or external connection is initiated. |
| PRIV-04 | Inspect release manifest and linked dependencies. | No analytics, telemetry, advertising, crash-upload, or automatic-update component is enabled. |

## Process-boundary tests (MUST)

| ID | Given / action | Expected result |
|---|---|---|
| PROC-01 | Supply model name/config values containing shell metacharacters, quotes, newline, and Unicode. | Fake engine receives them as literal typed argv values; no shell process or injected command runs. |
| PROC-02 | Have fake engine report inherited environment/handles. | Only allowlisted values and required IPC handles exist; no API token or unrelated inherited handle/descriptor appears. |
| PROC-03 | Attempt direct connection to engine endpoint from another process/user context where testable. | Connection is impossible or rejected; only frontend can use engine IPC. |
| PROC-04 | Crash/hang/exit fake engine repeatedly. | Frontend reports safe generic status, enforces bounded restart/backoff, and does not leak buffers/children. |
| PROC-05 | Stop frontend with running child/grandchild. | Windows Job Object and Linux process-group behavior terminate all descendants; no orphan listener remains. |
| PROC-06 | Inspect child permissions and startup behavior. | No elevation is requested; child/service use configured low privilege and restricted writable roots. |

## Extension exclusion tests (MUST)

| ID | Given / action | Expected result |
|---|---|---|
| EXT-01 | Inspect default binary/config and request API/schema fields for MCP, plugins, tools, or command execution. | Feature is absent or rejects with a fixed disabled response; no process/network action occurs. |
| EXT-02 | Attempt to enable extension through model manifest, environment, unknown config, or HTTP payload. | Validation rejects it before any extension loads. |

## Platform and regression requirements

- Execute all MUST tests on a supported Windows runner and Linux runner.
- Run each test with IPv4 loopback; run NET-01/NET-02 and host validation with
  IPv6 enabled where the runner supports it.
- Add a regression test for every discovered security defect before its fix is
  accepted.
- Security tests must be hermetic: use local test servers, fake artifacts, and
  fake engine processes; no public network or proprietary weights.
- A release record must state runner OS/version, frontend commit, engine commit,
  test results, dependency scan result, and unresolved accepted risks.
