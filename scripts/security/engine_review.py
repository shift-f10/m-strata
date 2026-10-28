#!/usr/bin/env python3
"""Engine supply-chain review gate (docs/security/engine-supply-chain.md).

Two checks, each a hard gate of the contract:

  diff    Scan every line added between two pinned refs of an engine repository
          (an upstream/ or engines/ submodule, or a fetched dependency) for the
          patterns the contract names: network use, process launch, runtime code
          loading, persistence, build-time fetches, embedded URLs, obfuscated
          blobs, and added or changed binary files. Each finding has a stable id;
          the gate passes only when every finding carries a recorded ruling in the
          rulings file and none is ruled "refused".

  binary  Check a built Windows engine binary (PE): its imported and delay-loaded
          DLLs, the DLL names it embeds (what it can LoadLibrary at run time) and
          the process/network functions it imports or names, against an allowlist.

Stdlib only; reads git through subprocess with fixed argument lists. Nothing is
executed from the repository under review.

  engine_review.py diff --repo upstream --old v0.2.1.3 --new v0.2.2 \
      --rulings docs/security/engine-reviews/strata-v0.2.1.3-v0.2.2.json --report out/review.md
  engine_review.py diff ... --write-template        # writes a rulings skeleton (ruling "")
  engine_review.py binary --exe out/engine-cuda-v0.2.2/strata.exe \
      --allowlist docs/security/engine-binary-allowlist.json
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

# --- diff -------------------------------------------------------------------------------

COMPILED = (".c", ".cc", ".cpp", ".cxx", ".cu", ".cuh", ".h", ".hh", ".hpp", ".hxx", ".inl", ".inc", ".metal", ".m", ".mm")
SCRIPTS = (".sh", ".bash", ".bat", ".cmd", ".ps1", ".psm1")
JAVASCRIPT = (".js", ".mjs", ".cjs", ".jsx", ".ts", ".tsx", ".mts", ".cts")


def surface_of(path: str) -> str:
    """Which part of the engine a changed path belongs to; the rules are chosen per surface."""
    lower = path.lower()
    name = lower.rsplit("/", 1)[-1]
    if name == "cmakelists.txt" or lower.endswith(".cmake") or lower.startswith("cmake/"):
        return "build"
    if lower.endswith(COMPILED):
        return "compiled"
    if lower.endswith(".py"):
        return "python"
    if lower.endswith(JAVASCRIPT):
        return "javascript"
    if lower.endswith(SCRIPTS) or name in ("dockerfile", "makefile"):
        return "script"
    if lower.endswith((".sha256", ".sha512", ".sha256sum", ".sha256sums")) or name in ("sha256sums", "checksums.txt"):
        return "digest"
    if (name.startswith("requirements") and name.endswith(".txt")) or name in (
            "package.json", "package-lock.json", "npm-shrinkwrap.json", "pnpm-lock.yaml", "yarn.lock", "pyproject.toml",
            "setup.cfg", "pipfile", "pipfile.lock", "poetry.lock", "uv.lock", "cargo.toml", "cargo.lock", "go.mod", "go.sum",
            "flake.lock", "flake.nix", "vcpkg.json", "conanfile.txt", "conanfile.py") or lower.endswith(".lock"):
        return "deps"
    if lower.startswith(".github/") or lower.endswith((".yml", ".yaml")):
        return "ci"
    return "other"


@dataclass(frozen=True)
class Rule:
    rule: str
    surfaces: tuple[str, ...]
    pattern: re.Pattern[str]
    meaning: str


def _r(text: str) -> re.Pattern[str]:
    return re.compile(text)


RULES = (
    Rule("net", ("compiled",), _r(r"\b(WSAStartup|getaddrinfo|gethostbyname|inet_pton|inet_addr|InternetOpen\w*|InternetConnect\w*|"
                                  r"HttpOpenRequest\w*|WinHttp\w+|URLDownloadTo\w+|curl_easy_\w+|socket\s*\(|connect\s*\(|"
                                  r"recvfrom\s*\(|sendto\s*\()"), "network use"),
    Rule("exec", ("compiled",), _r(r"\b(system|popen|_popen|_wsystem|_wpopen|WinExec|ShellExecute\w*|CreateProcess\w*|"
                                   r"execv\w*|execl\w*|posix_spawn\w*|fork)\s*\("), "process launch"),
    Rule("load", ("compiled",), _r(r"\b(LoadLibrary\w*|dlopen|GetProcAddress|dlsym)\s*\("), "runtime code loading"),
    Rule("persist", ("compiled", "python", "script"), _r(r"\b(RegSetValue\w*|RegCreateKey\w*|CreateService\w*|schtasks|crontab|"
                                                          r"SetWindowsHookEx\w*|CreateRemoteThread|WriteProcessMemory|"
                                                          r"VirtualAllocEx|HKEY_LOCAL_MACHINE|CurrentVersion\\\\Run)"),
         "persistence or cross-process access"),
    Rule("fetch", ("build",), _r(r"\b(FetchContent_Declare|FetchContent_MakeAvailable|FetchContent_Populate|ExternalProject_Add|"
                                 r"GIT_REPOSITORY|URL_HASH|execute_process|add_custom_command|add_custom_target)\b|"
                                 r"file\s*\(\s*DOWNLOAD|\bURL\s+\S"), "build-time fetch or command"),
    Rule("url", ("compiled", "build", "script"), _r(r"\b(https?|ftp)://"), "embedded URL"),
    Rule("py-net", ("python",), _r(r"\b(urllib\b|requests\.|http\.client|socket\.|urlopen|ftplib|smtplib|paramiko|websocket)"),
         "network use"),
    Rule("py-exec", ("python",), _r(r"\b(subprocess\.|os\.system|os\.popen|os\.exec\w*|os\.spawn\w*|pty\.spawn)|"
                                    r"(?<![\w.])(eval|exec)\s*\(|__import__\s*\("), "process launch or dynamic code"),
    Rule("py-load", ("python",), _r(r"\b(ctypes|cdll|windll|importlib\.import_module|marshal\.loads|pickle\.loads)\b"),
         "native or serialized code loading"),
    Rule("py-obf", ("python",), _r(r"\b(b64decode|base64\.|zlib\.decompress|codecs\.decode|bytes\.fromhex)"),
         "decoding of embedded data"),
    Rule("sh-fetch", ("script", "ci"), _r(r"(?i)\b(curl|wget|Invoke-WebRequest|iwr|Start-BitsTransfer|bitsadmin|certutil)\b|"
                                          r"-enc(odedcommand)?\s"), "network fetch or encoded command"),
    Rule("js-net", ("javascript",), _r(r"\bfetch\s*\(|XMLHttpRequest|new\s+WebSocket|\bhttps?\.(request|get)\s*\(|"
                                       r"\bnet\.(connect|createConnection)\s*\(|\bdgram\b|\btls\.connect\s*\("), "network use"),
    Rule("js-exec", ("javascript",), _r(r"\bchild_process\b|\b(spawn|spawnSync|execFile|execFileSync|execSync)\s*\(|"
                                        r"(?<![\w.])(eval|exec)\s*\(|new\s+Function\s*\(|\bvm\.(runIn\w+|Script)\b"),
         "process launch or dynamic code"),
    Rule("blob", ("compiled", "python", "javascript", "build", "script", "ci", "other"),
         _r(r"[A-Za-z0-9+/]{160,}={0,2}|(\\x[0-9a-fA-F]{2}){32,}"), "long encoded blob"),
)


@dataclass(frozen=True)
class Finding:
    id: str
    rule: str
    meaning: str
    path: str
    line: int  # 0 for whole-file findings (binary files)
    text: str


def finding_id(rule: str, path: str, text: str) -> str:
    """Stable across line shifts: the rule, the file and the trimmed line text."""
    return hashlib.sha256(f"{rule}\0{path}\0{' '.join(text.split())}".encode("utf-8", "surrogateescape")).hexdigest()[:16]


def git(repo: Path, *args: str) -> str:
    result = subprocess.run(["git", "-C", str(repo), *args], check=True, capture_output=True)
    return result.stdout.decode("utf-8", "surrogateescape")


def scan_diff(repo: Path, old: str, new: str) -> list[Finding]:
    findings: list[Finding] = []
    for row in git(repo, "diff", "--no-renames", "--numstat", old, new).splitlines():
        added, removed, path = row.split("\t", 2)
        if added == "-" and removed == "-":  # git's mark for a binary file
            findings.append(Finding(finding_id("binary", path, ""), "binary", "added or changed binary file", path, 0, ""))
    current = ""
    line_no = 0
    for raw in git(repo, "diff", "--no-renames", "--no-color", "--no-ext-diff", "-U0", old, new).splitlines():
        if raw.startswith("+++ "):
            current = raw[6:] if raw.startswith("+++ b/") else ""
            continue
        if raw.startswith("@@"):
            match = re.search(r"\+(\d+)", raw)
            line_no = int(match.group(1)) if match else 0
            continue
        if not current or not raw.startswith("+"):
            continue
        text = raw[1:]
        surface = surface_of(current)
        if surface == "digest":  # one decision per checksum file, not per line
            findings.append(Finding(finding_id("digest", current, ""), "digest", "changed pinned checksum file", current, 0, ""))
            continue
        if surface == "deps":  # one decision per dependency manifest: what was added or re-pinned
            findings.append(Finding(finding_id("deps", current, ""), "deps", "changed dependency manifest", current, 0, ""))
            continue
        for rule in RULES:
            if surface in rule.surfaces and rule.pattern.search(text):
                findings.append(Finding(finding_id(rule.rule, current, text), rule.rule, rule.meaning, current, line_no,
                                        text.strip()[:240]))
        line_no += 1
    unique: dict[str, Finding] = {}
    for finding in findings:
        unique.setdefault(finding.id, finding)  # one ruling covers identical lines in one file
    return sorted(unique.values(), key=lambda f: (f.path, f.line, f.rule))


RULINGS = ("benign", "accepted", "refused")


def load_rulings(path: Path) -> dict[str, dict[str, str]]:
    if not path.exists():
        return {}
    data = json.loads(path.read_text(encoding="utf-8"))
    return data.get("rulings", {})


def diff_main(args: argparse.Namespace) -> int:
    repo = Path(args.repo)
    findings = scan_diff(repo, args.old, args.new)
    old_commit = git(repo, "rev-parse", f"{args.old}^{{commit}}").strip()
    new_commit = git(repo, "rev-parse", f"{args.new}^{{commit}}").strip()
    rulings_path = Path(args.rulings)
    if args.write_template:
        existing = load_rulings(rulings_path)
        skeleton = {
            "engine": args.engine, "old": args.old, "old_commit": old_commit, "new": args.new, "new_commit": new_commit,
            "rulings": {f.id: existing.get(f.id, {"ruling": "", "why": "", "where": f"{f.path}:{f.line}", "rule": f.rule,
                                                   "text": f.text}) for f in findings},
        }
        rulings_path.parent.mkdir(parents=True, exist_ok=True)
        # LF on every platform: the file's SHA-256 is recorded (contract C9).
        with rulings_path.open("w", encoding="utf-8", newline="\n") as handle:
            handle.write(json.dumps(skeleton, indent=1, ensure_ascii=False) + "\n")
        print(f"wrote {len(findings)} findings to {rulings_path}")
        return 0
    rulings = load_rulings(rulings_path)
    unruled = [f for f in findings if rulings.get(f.id, {}).get("ruling") not in RULINGS]
    refused = [f for f in findings if rulings.get(f.id, {}).get("ruling") == "refused"]
    by_rule: dict[str, int] = {}
    for finding in findings:
        by_rule[finding.rule] = by_rule.get(finding.rule, 0) + 1
    lines = [f"# Engine diff review: {args.engine} {args.old} ({old_commit[:8]}) -> {args.new} ({new_commit[:8]})", "",
             f"Findings: {len(findings)} ({', '.join(f'{k} {v}' for k, v in sorted(by_rule.items())) or 'none'}); "
             f"unruled: {len(unruled)}; refused: {len(refused)}.", "",
             "| id | rule | where | ruling | text |", "| --- | --- | --- | --- | --- |"]
    for finding in findings:
        ruling = rulings.get(finding.id, {})
        verdict = ruling.get("ruling") or "**UNRULED**"
        text = finding.text.replace("|", "\\|")
        lines.append(f"| `{finding.id}` | {finding.rule} | `{finding.path}:{finding.line}` | {verdict} | `{text[:120]}` |")
    report = "\n".join(lines) + "\n"
    if args.report:
        with Path(args.report).open("w", encoding="utf-8", newline="\n") as handle:
            handle.write(report)
    print(lines[0])
    print(lines[2])
    if refused:
        print("refused findings block the bump", file=sys.stderr)
        return 1
    if unruled:
        print(f"{len(unruled)} findings have no ruling in {rulings_path}", file=sys.stderr)
        return 1
    return 0


# --- binary -----------------------------------------------------------------------------

NETWORK_DLLS = ("ws2_32.dll", "wsock32.dll", "winhttp.dll", "wininet.dll", "urlmon.dll", "dnsapi.dll", "iphlpapi.dll",
                "mswsock.dll", "httpapi.dll", "webio.dll")
FORBIDDEN_NAMES = ("CreateProcessA", "CreateProcessW", "CreateProcessAsUserA", "CreateProcessAsUserW", "WinExec",
                   "ShellExecuteA", "ShellExecuteW", "ShellExecuteExA", "ShellExecuteExW", "CreateRemoteThread",
                   "WriteProcessMemory", "VirtualAllocEx", "SetWindowsHookExA", "SetWindowsHookExW", "WSAStartup",
                   "InternetOpenA", "InternetOpenW", "URLDownloadToFileA", "URLDownloadToFileW", "WinHttpOpen",
                   "getaddrinfo", "RegSetValueExA", "RegSetValueExW", "CreateServiceA", "CreateServiceW")


def _rva_to_offset(sections: list[tuple[int, int, int, int]], rva: int) -> int | None:
    for virtual_address, virtual_size, raw_offset, raw_size in sections:
        if virtual_address <= rva < virtual_address + max(virtual_size, raw_size):
            return raw_offset + (rva - virtual_address)
    return None


def _cstring(data: bytes, offset: int) -> str:
    end = data.index(b"\0", offset)
    return data[offset:end].decode("ascii", "replace")


def pe_imports(data: bytes) -> dict[str, list[str]]:
    """{dll (lower case): [imported function names]} from the import and delay-import directories.

    An import by ordinal carries no name, so it is listed as "#<ordinal>": the gate refuses it
    unless the allowlist rules it (allowed_ordinals), since no name rule can see what it binds.
    """
    if data[:2] != b"MZ":
        raise ValueError("not a PE file (no MZ header)")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("not a PE file (no PE signature)")
    sections_count, optional_size = struct.unpack_from("<H", data, pe + 6)[0], struct.unpack_from("<H", data, pe + 20)[0]
    optional = pe + 24
    magic = struct.unpack_from("<H", data, optional)[0]
    pe32_plus = magic == 0x20B
    directories = optional + (112 if pe32_plus else 96)
    sections = []
    table = optional + optional_size
    for index in range(sections_count):
        base = table + 40 * index
        virtual_size, virtual_address, raw_size, raw_offset = struct.unpack_from("<IIII", data, base + 8)
        sections.append((virtual_address, virtual_size, raw_offset, raw_size))
    thunk_size, ordinal_flag = (8, 1 << 63) if pe32_plus else (4, 1 << 31)
    result: dict[str, list[str]] = {}

    def names_at(thunk_rva: int) -> list[str]:
        names: list[str] = []
        offset = _rva_to_offset(sections, thunk_rva)
        while offset is not None:
            value = struct.unpack_from("<Q" if pe32_plus else "<I", data, offset)[0]
            if value == 0:
                break
            if value & ordinal_flag:
                names.append(f"#{value & 0xFFFF}")
            else:
                name_offset = _rva_to_offset(sections, value & 0x7FFFFFFF)
                if name_offset is not None:
                    names.append(_cstring(data, name_offset + 2))
            offset += thunk_size
        return names

    import_rva = struct.unpack_from("<I", data, directories + 8)[0]  # directory 1: imports
    offset = _rva_to_offset(sections, import_rva) if import_rva else None
    while offset is not None:
        original_first_thunk, _, _, name_rva, first_thunk = struct.unpack_from("<IIIII", data, offset)
        if name_rva == 0:
            break
        name_offset = _rva_to_offset(sections, name_rva)
        dll = _cstring(data, name_offset).lower() if name_offset is not None else "?"
        result.setdefault(dll, []).extend(names_at(original_first_thunk or first_thunk))
        offset += 20
    delay_rva = struct.unpack_from("<I", data, directories + 13 * 8)[0]  # directory 13: delay imports
    offset = _rva_to_offset(sections, delay_rva) if delay_rva else None
    while offset is not None:
        _, name_rva, _, _, name_table_rva = struct.unpack_from("<IIIII", data, offset)
        if name_rva == 0:
            break
        name_offset = _rva_to_offset(sections, name_rva)
        dll = _cstring(data, name_offset).lower() if name_offset is not None else "?"
        result.setdefault(dll, []).extend(names_at(name_table_rva))
        offset += 32
    return result


def embedded_dll_names(data: bytes) -> set[str]:
    """DLL names the binary carries as text (ASCII or UTF-16): what it can load by name at run time."""
    found = {m.group(0).decode("ascii").lower() for m in re.finditer(rb"[A-Za-z0-9_\-.]{2,64}\.dll", data, re.IGNORECASE)}
    wide = re.finditer(rb"(?:[A-Za-z0-9_\-.]\x00){2,64}\.\x00[dD]\x00[lL]\x00[lL]\x00", data)
    found |= {m.group(0).decode("utf-16-le").lower() for m in wide}
    return found


def _allowed(name: str, patterns: list[str]) -> bool:
    return any(re.fullmatch(pattern.replace(".", r"\.").replace("*", ".*"), name) for pattern in patterns)


def binary_main(args: argparse.Namespace) -> int:
    allow = json.loads(Path(args.allowlist).read_text(encoding="utf-8"))
    problems: list[str] = []
    for exe in args.exe:
        data = Path(exe).read_bytes()
        imports = pe_imports(data)
        embedded = embedded_dll_names(data)
        print(f"{exe}: {len(imports)} imported DLLs, {len(embedded)} embedded DLL names")
        for dll, functions in sorted(imports.items()):
            if dll in NETWORK_DLLS:
                problems.append(f"{exe}: imports network library {dll}")
            elif not _allowed(dll, allow["imports"]):
                problems.append(f"{exe}: imports {dll}, not in the allowlist")
            for function in functions:
                if function.startswith("#"):
                    if f"{dll}{function}" not in allow.get("allowed_ordinals", {}):
                        problems.append(f"{exe}: imports {dll} ordinal {function[1:]} by number (no name for the "
                                        "forbidden-function rule to check); rule it in allowed_ordinals or refuse")
                elif function in FORBIDDEN_NAMES and function not in allow.get("allowed_functions", []):
                    problems.append(f"{exe}: imports {function} from {dll}")
        for dll in sorted(embedded):
            if dll in NETWORK_DLLS:
                problems.append(f"{exe}: names network library {dll} (loadable at run time)")
            elif not _allowed(dll, allow["imports"] + allow["loadable"]):
                problems.append(f"{exe}: names {dll} (loadable at run time), not in the allowlist")
        text = data.decode("latin-1")
        for function in FORBIDDEN_NAMES:
            if function in allow.get("allowed_functions", []):
                continue
            if re.search(rf"(?<![A-Za-z0-9_]){function}(?![A-Za-z0-9_])", text) and not any(
                    function in functions for functions in imports.values()):
                problems.append(f"{exe}: names {function} (resolvable with GetProcAddress)")
    for problem in problems:
        print(problem, file=sys.stderr)
    print("binary gate: " + ("FAIL" if problems else "PASS"))
    return 1 if problems else 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    sub = parser.add_subparsers(dest="command", required=True)
    diff = sub.add_parser("diff", help="scan old..new of an engine repository")
    diff.add_argument("--repo", required=True)
    diff.add_argument("--old", required=True)
    diff.add_argument("--new", required=True)
    diff.add_argument("--engine", default="")
    diff.add_argument("--rulings", required=True, help="JSON file of recorded rulings")
    diff.add_argument("--report", help="write the markdown report here")
    diff.add_argument("--write-template", action="store_true", help="write/refresh the rulings skeleton and exit 0")
    binary = sub.add_parser("binary", help="check built Windows engine binaries")
    binary.add_argument("--exe", required=True, nargs="+")
    binary.add_argument("--allowlist", required=True)
    args = parser.parse_args(argv)
    return diff_main(args) if args.command == "diff" else binary_main(args)


if __name__ == "__main__":
    raise SystemExit(main())
