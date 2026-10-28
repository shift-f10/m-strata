# Secure-native deployment and package readiness

## Status

This is the deployment contract for the eventual secure-native distribution.
The native `strata-secure-server` now provides a runnable, serial local chat
path for its deliberately constrained Qwen/text tokenizer profile. It is not
yet a full compatibility or release claim for every upstream model/template,
vision, or backend profile. Do not package the upstream Python workflow as a
"secure" release.

The source checkout intentionally retains upstream installer, update, Python
service, MCP, telemetry, and web files for provenance and compatibility work.
They are **not** selected for a secure-native distribution.

## Package gate

Stage a package in a new directory rather than archiving this repository, then
run the gate against that exact staging directory:

```sh
python3 scripts/secure_distribution_check.py --package-dir /absolute/path/to/stage
python3 scripts/secure_distribution_check.py --package-dir /absolute/path/to/stage --release
```

The normal mode rejects legacy service and installer/update artifacts at any
depth of the staging tree: a `serve` or `sycl/serve` directory, `setup.py`,
`START-HERE.bat`, update scripts, `requirements.txt`, MCP artifacts, Python
source, and anything under an `upstream` directory. It also rejects
symlinks in a staging tree so a reviewer cannot be shown files from outside the
candidate package.

`--release` additionally requires these files at the package root:

- `secure-release-manifest.json`
- `sbom.spdx.json`

The gate checks the following required manifest fields. It is deliberately a
small structural check; the release reviewer must still validate SBOM contents,
signatures, provenance, and test evidence.

```json
{
  "profile": "secure-native",
  "network_default": "offline",
  "legacy_components": [],
  "privacy": {
    "analytics": false,
    "telemetry": false,
    "crash_upload": false,
    "update_check": false,
    "remote_configuration": false,
    "advertising": false,
    "automatic_downloads": false,
    "mcp": false,
    "plugins": false
  },
  "components": [
    {"name": "strata-secure-frontend", "version": "RELEASE_VERSION", "source": "BUILD_PROVENANCE"}
  ]
}
```

This gate is intentionally fail-closed for Python. If a future native release
has a reviewed reason to ship Python, add a narrow allowlist, a threat-model
update, and a regression test before relaxing it. Never bypass the check by
renaming a quarantined component.

## Common deployment requirements

- Build from a pinned source revision using the checked-in CMake presets; do
  not invoke `setup.py`, `START-HERE.bat`, `update.sh`, or `UPDATE.bat`.
- Use a C++20 compiler and CMake. The frontend itself is dependency-free; GPU
  backend requirements belong to the selected Strata engine backend.
- Obtain the executable, engine, and GGUF files before deployment through an
  operator-controlled, checksum-verified channel. The server must not fetch
  them on startup or request handling.
- Create a dedicated model directory owned by the service user. Do not make it
  world-writable and do not allow symlink/junction escapes from it.
- Generate the token with `strata-secure-server init` (CSPRNG, 43 characters,
  never printed). On Linux it is an owner-only `0600` file in a `0700`
  directory. On Windows it is a DPAPI-wrapped file (`STRATAK1` + blob) with a
  protected DACL for you and SYSTEM; the reader also accepts an operator-made
  plaintext file under the same ACL rules. Until the `windows-msys2-clang` CI
  job has passed the `windows-runtime` token tests, treat the Windows file path
  as unverified and prefer `--token-env` there.
- An operator-made token must be at least 32 bytes (at most 256) and may end
  in one LF or CRLF. On Windows a plaintext token file must not start with the
  bytes `STRATAK1`: such a file is treated as DPAPI-wrapped and rejected as
  undecryptable.
- Bind only to `127.0.0.1` initially. Remote and wildcard binding require the
  explicit runtime opt-ins defined in the threat model and their separate
  security review.
- Do not expose the engine child endpoint, model directory, or token file
  directly to clients. The frontend is the only public listener.
- Start `strata-secure-server` with an already-local tokenizer pack, selected
  `.gguf` model, engine executable, and each engine option supplied as a
  distinct `--engine-arg` (or `engine.args` element in the `--config` file;
  run `--check` first: it validates every key without side effects and lists
  every problem by key path). It appends `--serve` itself, runs exactly one child
  with a fixed frontend-rendered environment (nothing inherited from the
  server's), and exposes only authenticated
  `GET /health`, `GET /v1/models`, and non-streaming text-only
  `POST /v1/chat/completions`. The request `model` must equal the startup
  `--model-id`; streaming, tools, images, URLs, MCP, downloads, and remote
  model selection fail closed.
- Keep request logging disabled by default. If diagnostics are enabled, use
  local bounded retention and verify prompt/token redaction.

## Linux prerequisites and operational checks

- A supported x86_64 Linux distribution, a current C++20 compiler (GCC or
  Clang), CMake, and the vendor runtime/toolkit matching the selected CUDA,
  HIP, or SYCL engine build.
- A non-root service account with ownership of the model and secret roots.
  Use `umask 077` when creating token files. The Linux protected-file token
  reader requires a regular file owned by the current user with no group/world
  permissions and no symlink components.
- Permit local TCP loopback. No outbound firewall exception is required for
  normal operation. A deployment with outbound DNS or internet access is not
  evidence that the service needs it.
- Before release, run the CMake/CTest suite plus all Linux MUST cases from
  [`../../tests/security/README.md`](../../tests/security/README.md), including
  monitored startup/inference/shutdown/restart proving no outbound connection
  or DNS lookup.
- The engine runs with `PR_SET_NO_NEW_PRIVS` (default). On hosts where the
  NVIDIA driver creates `/dev/nvidia-uvm*` on first CUDA use by executing the
  setuid `nvidia-modprobe`, NNP blocks that: create the nodes at boot instead
  (as root: `nvidia-modprobe -u -c=0`, or enable `nvidia-persistenced`). The
  frontend logs a hint naming this fix when an engine dies before READY under
  NNP. `--allow-new-privileges` turns NNP off; it exists for the offline GPU
  validation kit and such hosts, and is not the default.
- `strata-secure-server` refuses to start as root (Windows: with an elevated
  token). Containers must run it as an unprivileged user.
- `--state-dir` must be an existing directory owned by the service user with
  no group/other permissions; the engine's working directory, temp and CUDA
  cache live below it.

## Windows prerequisites and operational checks

- Windows 10 or 11 x64, a supported MSVC/Visual Studio C++20 toolchain, CMake,
  and the vendor driver/runtime matching the selected CUDA, HIP, or SYCL engine
  build. Match the backend build to the installed driver; do not use the legacy
  Python bootstrap to obtain it.
- Run under a standard user account. Do not request elevation. Store models in
  a user-owned directory outside broadly writable locations such as a shared
  temp directory.
- Windows token files require a current-user-owned protected DACL on the secret
  directory and the file; the reader opens every component with `READ_CONTROL`
  and rejects reparse points (junctions, symlinks), path escapes, extra hard
  links, inherited or broad ACLs and non-regular files. Roots under OneDrive
  Known Folder Move or a roaming profile resolve to a network path and are
  rejected; keep models, secrets and config on a local disk.
- Permit loopback through the local firewall if policy requires it. Do not add
  public inbound rules for the initial deployment. Remote/WAN operation needs
  an explicit firewall rule, explicit runtime opt-ins, and the remote-operation
  acceptance gate in the threat model.
- Before release, execute CMake/CTest and every Windows MUST security case,
  including Job Object descendant cleanup, restricted handle inheritance, and
  monitored no-outbound-network lifecycle tests.

## Release evidence checklist

The release owner records the following alongside a staged package:

1. Exact frontend and engine commits, compiler/toolchain, backend, and target
   OS version.
2. `--release` package-gate output, release manifest, and SBOM.
3. Passing Windows and Linux security matrices and dependency scan results.
4. A recorded outbound-network/DNS observation for start, API use, inference,
   child restart, and shutdown.
5. A human review of the package file list confirming no legacy Python server,
   MCP/tool/plugin system, telemetry, updater, or analytics component is
   selected.
6. Unresolved accepted risks to state: PROC-03 same-uid reopen of engine stdin
   on Linux (SP2 spec §3.13); PROC-05 frontend SIGKILL leaves engine
   descendants, without a delegated cgroup and, with one, until the operator's
   scope is killed (SP2 spec §3.7); the SP2 environment allowlist and the
   `PR_SET_NO_NEW_PRIVS` default not yet verified on a GPU.

See [`threat-model.md`](threat-model.md) for the normative security contract
and [`privacy-migration.md`](privacy-migration.md) for the upstream artifacts
that remain quarantined.
