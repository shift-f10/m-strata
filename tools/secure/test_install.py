"""install.bat / install.ps1 / install.sh (owner 2026-10-28: "make an easy install bat that does pretty much everything,
download dependencies, install them then run the build"): what they pin is what the build pins, and they run make."""
from __future__ import annotations

import importlib.util
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]


def text(name: str) -> str:
    return (ROOT / name).read_text(encoding="utf-8-sig")


def engine_llama_pin() -> str:
    found = re.search(r"GIT_TAG ([0-9a-f]{40})", text("engine/CMakeLists.txt"))
    assert found, "engine/CMakeLists.txt pins llama.cpp"
    return found.group(1)


def test_the_scripts_pin_the_engines_llama_cpp():
    pin = engine_llama_pin()
    assert f"$LlamaPin = '{pin}'" in text("install.ps1")
    assert f"LLAMA_PIN={pin}" in text("install.sh")
    assert f"git -C llama.cpp checkout {pin}" in text("README.md"), "the manual steps check out the same commit"


def test_the_window_sdk_is_the_one_provenance_records():
    provenance = text("docs/PROVENANCE.md")
    ps1 = text("install.ps1")
    version = re.search(r"\$WebView2Version = '([0-9.]+)'", ps1).group(1)
    sha = re.search(r"\$WebView2Sha256 = '([0-9a-f]{64})'", ps1).group(1)
    assert f"version `{version}`" in provenance and sha in provenance
    assert sha in text("README.md")
    # the build reads the SDK where the script unpacks it
    assert f"webview2/{version}" in text("secure_frontend/CMakeLists.txt")


def test_cuda_13_0_on_both_systems():
    assert "$CudaVersion = '13.0'" in text("install.ps1") and "'Nvidia.CUDA' @('--version', $CudaVersion)" in text("install.ps1")
    assert "CUDA_PACKAGE=cuda-toolkit-13-0" in text("install.sh") and "CUDA_ROOT=/usr/local/cuda-13.0" in text("install.sh")


def test_the_wrappers_run_the_scripts_and_the_scripts_run_make():
    bat = text("install.bat")
    assert 'powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" %*' in bat
    assert "& (Join-Path $repo 'make.bat') --out $Out --ggml-dir $ggml --cuda-root $cuda" in text("install.ps1")
    assert '"$repo/make.sh" --out "$out" --ggml-dir "$ggml" --cuda-root "$cuda" --with "$with"' in text("install.sh")
    # a plan run changes nothing: both stop right after the plan
    assert "if ($Plan) {" in text("install.ps1") and 'if [ "$plan" = 1 ]; then' in text("install.sh")


def test_a_named_llama_cpp_folder_is_used_as_it_is():
    """-GgmlDir / --ggml-dir: never git-initialised, fetched or checked out (it may be a vendored copy, not a clone)."""
    for name, named in (("install.ps1", "if ($GgmlDir) {\n  # a folder you named is used as it is"),
                        ("install.sh", 'if [ "$ggml_given" = 1 ]; then\n  # a folder you named is used as it is')):
        script = text(name)
        assert named in script, name
        branch = script.index(named)
        otherwise = script.index("} else {" if name == "install.ps1" else "\nelse\n", branch)
        # the only init, fetch and checkout are in the other branch, for the folder the script picked itself
        for command in ("init -q", "fetch --depth 1", "checkout -q FETCH_HEAD"):
            assert script.count(command) == 1 and script.index(command) > otherwise, (name, command)


def test_both_front_ends_by_default():
    """The owner (2026-10-28): "have the installer compile both as an option by default so the user can pick if they
    want the tui or window version or specify" - both unless -With / --with says otherwise; a Linux machine without a
    display (a server over SSH) gets the TUI only."""
    assert '[string]$With = "email,search,window,tui",' in text("install.ps1")
    sh = text("install.sh")
    assert 'with=""' in sh and 'then with="email,search,window,tui"' in sh and 'else with="email,search,tui"; no_display=1; fi' in sh
    assert '"${DISPLAY:-}${WAYLAND_DISPLAY:-}"' in sh
    # make itself takes the feature
    assert '"tui")' in text("make.py") and "strata-tui" in text("make.py")


def test_the_cpp_workload_is_what_make_asks_for():
    """The script installs, or adds to Build Tools, the workload make's own message names."""
    ps1 = text("install.ps1")
    assert ps1.count("Microsoft.VisualStudio.Workload.VCTools") == 2
    assert "Microsoft.VisualStudio.Workload.VCTools" in text("make.py")


def test_the_scripts_parse():
    """Owner 2026-10-29: install.bat stopped before doing anything ("$WebView2Version:" in a string is a drive-qualified
    variable to PowerShell). PowerShell's own parser reads install.ps1 - Windows PowerShell 5.1, which install.bat runs,
    else pwsh - and the shell reads install.sh with -n; skipped where neither shell is installed."""
    powershell = shutil.which("powershell") or shutil.which("pwsh")
    sh = shutil.which("sh")
    if not powershell and not sh:
        pytest.skip("neither PowerShell nor sh is on PATH")
    if powershell:
        path = str(ROOT / "install.ps1").replace("'", "''")
        parse = ("$errors = $null; [void][System.Management.Automation.Language.Parser]::ParseFile('" + path +
                 "', [ref]$null, [ref]$errors); foreach ($e in $errors) { '{0}: {1}' -f $e.Extent.StartLineNumber, $e.Message }")
        result = subprocess.run([powershell, "-NoProfile", "-NonInteractive", "-Command", parse],
                                capture_output=True, text=True, timeout=120, check=False)
        assert result.returncode == 0 and result.stdout.strip() == "", f"install.ps1 does not parse: {result.stdout}{result.stderr}"
    if sh:
        result = subprocess.run([sh, "-n", str(ROOT / "install.sh")], capture_output=True, text=True, timeout=60, check=False)
        assert result.returncode == 0, f"install.sh does not parse: {result.stderr}"


def test_batch_and_powershell_line_endings():
    attributes = text(".gitattributes")
    assert "*.bat text eol=crlf" in attributes and "*.ps1 text eol=crlf" in attributes and "*.sh text eol=lf" in attributes
    assert (ROOT / "install.ps1").read_bytes().startswith(b"\xef\xbb\xbf"), "a BOM: Windows PowerShell 5.1 reads UTF-8 by it"


def test_debugging_is_passed_to_make_and_explained_first():
    """The owner (2026-10-29): --debugging from the installer too, telling the user what it means before the build
    (2026-10-30: it ships the Debugging plugin on, which the Plugins tab turns off)."""
    ps, sh = text("install.ps1"), text("install.sh")
    assert "[switch]$Debugging" in ps and "if ($Debugging) { $makeArgs += '--debugging' }" in ps
    assert "--debugging) debugging=1; debugging_given=1; shift ;;" in sh and '[ "$debugging" = 1 ] && make_flags="--debugging"' in sh
    assert '--with "$with" $make_flags' in sh
    for script in (ps, sh):
        for words in ("your prompts", "keep the server's log private", "before you share it", "the Debugging plugin ships on",
                      "turn the plugin off in the"):
            assert words in script, words


def make_lists() -> tuple[tuple[str, ...], tuple[str, ...]]:
    spec = importlib.util.spec_from_file_location("strata_make_lists", ROOT / "make.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules["strata_make_lists"] = module  # its dataclass looks its module up there
    spec.loader.exec_module(module)
    return module.OPTIONAL_FEATURES, module.LEAVE_OUT


def test_the_installers_ask_what_to_build():
    """The owner (2026-10-29): "In the install section, we should ask these questions, if they want email alerts,
    search, etc so that they can build exactly what they want". One question for each of make's choices - every
    optional feature, every family it can leave out, the Debugging plugin on - asked before the PC is looked at (what it
    installs depends on them). An answer the command line gave (-With, -Without, -Debugging) is not asked again;
    -Yes / --yes asks nothing and takes every default: everything, no debugging."""
    features, leave_out = make_lists()
    ps, sh = text("install.ps1"), text("install.sh")
    # the installers know make's lists, and refuse anything else before they install anything
    assert "$KnownFeatures = @(" + ", ".join(f"'{f}'" for f in features) + ")" in ps
    assert "$KnownLeaveOut = @(" + ", ".join(f"'{f}'" for f in leave_out) + ")" in ps
    assert f'known_features="{" ".join(features)}"' in sh and f'known_leave_out="{" ".join(leave_out)}"' in sh
    # each defaults to yes (Linux's window: test_the_window_question_defaults_to_no_without_a_display) ...
    for feature in features:
        assert f"?' $true) {{ $features += '{feature}' }}" in ps, feature
        assert feature == "window" or f"?' y; then with=\"$with,{feature}\"; fi" in sh, feature
    for family in leave_out:
        assert f"?' $true)) {{ $leaveOut += '{family}' }}" in ps, family
        assert re.search(r"if ! ask ['\"][^\n]*\?['\"] y; then without=\"\$without," + family + r"\"; fi", sh), family
    # ... but the Debugging plugin (off unless asked for; owner 2026-10-30, a plugin)
    assert "$Debugging = [switch](Ask 'The Debugging plugin turned on" in ps and "?' $false)\n" in ps.replace("\r\n", "\n")
    assert "if ask 'The Debugging plugin turned on" in sh and "?' n; then debugging=1; fi" in sh
    # asked only when the command line did not say, and never with -Yes / --yes
    for option in ("With", "Without", "Debugging"):
        assert f"-not $Yes -and -not $PSBoundParameters.ContainsKey('{option}')" in ps, option
    for given in ("with_given", "without_given", "debugging_given"):
        assert f'[ "${given}" = 0 ]' in sh, given
    assert 'if [ "$yes" = 0 ] && {' in sh
    # before the PC is looked at, and what was chosen reaches make
    for script in (ps, sh):
        assert script.index("What to build") < script.index("Looking at what this PC has")
    assert "if ($leaveOut.Count -gt 0) { $makeArgs += @('--without', ($leaveOut -join ',')) }" in ps
    assert "& (Join-Path $repo 'make.bat') --out $Out --ggml-dir $ggml --cuda-root $cuda @makeArgs" in ps
    assert '--without) without=$2; without_given=1; shift 2 ;;' in sh and 'make_flags="--without $without' in sh


def test_the_window_question_defaults_to_no_without_a_display():
    """Linux: with no display (DISPLAY, WAYLAND_DISPLAY - a server over SSH) the window's question defaults to no, as
    the build without questions leaves it out (test_both_front_ends_by_default)."""
    sh = text("install.sh")
    assert 'if [ "$display" = 1 ]; then window_default=y; window_note=""; else window_default=n; window_note=" (no display here)"; fi' in sh
    assert '?" "$window_default"; then with="$with,window"; fi' in sh


def signing_module():
    spec = importlib.util.spec_from_file_location("strata_plugin_signing_for_install", ROOT / "scripts" / "plugin_signing.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module  # its dataclass looks its module up there
    spec.loader.exec_module(module)
    return module


def test_the_installers_look_for_the_signing_key_where_make_keeps_it():
    """This PC's plugin signing key is where make keeps it (scripts/plugin_signing.py's default_key_path): the user's
    APPDATA on Windows, XDG_CONFIG_HOME (else ~/.config) on Linux."""
    signing = signing_module()
    assert signing.default_key_path("windows", {"APPDATA": "C:\\A"}) == Path("C:\\A") / "M-Strata" / "plugin-signing-key.json"
    assert signing.default_key_path("linux", {"XDG_CONFIG_HOME": "/c"}) == Path("/c") / "m-strata" / "plugin-signing-key.json"
    ps = text("install.ps1")
    assert "$appData = if ($env:APPDATA) { $env:APPDATA } else { Join-Path $HOME 'AppData\\Roaming' }" in ps
    assert "$signingKey = Join-Path $appData 'M-Strata\\plugin-signing-key.json'" in ps
    assert 'signing_key_file="${XDG_CONFIG_HOME:-$HOME/.config}/m-strata/plugin-signing-key.json"' in text("install.sh")


# a key file as make writes it (scripts/plugin_signing.py's create_key; the seed is never read here)
SEALED_KEY = ('{\n  "schema": "m-strata.signing-key/1",\n  "public": "' + "a" * 64 + '",\n  "protection": "file",\n'
              '  "passphrase": true\n}\n')
UNSEALED_KEY = SEALED_KEY.replace('"passphrase": true', '"passphrase": false')
SEAL_QUESTION = "Seal the plugin signing key this build makes with a passphrase"


def make_line(output: str) -> str:
    lines = [line for line in output.splitlines() if line.strip().startswith(("make.bat --out", "make.sh --out"))]
    assert len(lines) == 1, output
    return lines[0]


def test_install_ps1_offers_the_signing_passphrase(tmp_path):
    """The owner (2026-10-30, "Fix the gaps"): make's --signing-passphrase from install.bat too. With no key on this PC
    yet the build makes one, and the installer asks (no by default) whether to seal it with a passphrase;
    -SigningPassphrase answers yes on the command line and -Yes takes the default. A key already sealed is passed on as
    sealed, unasked (make would stop without), so make asks for its passphrase; one that is not sealed is used as it is
    (the option does not seal a key that exists). The plan says which. Run as a plan, with APPDATA in a scratch folder
    and the answers piped in - which Read-Host then does not echo its question for, so each question is seen by what its
    answer does."""
    powershell = shutil.which("powershell") if sys.platform == "win32" else None
    if not powershell:
        pytest.skip("install.ps1 runs on Windows (Windows PowerShell)")
    key = tmp_path / "appdata" / "M-Strata" / "plugin-signing-key.json"

    def run(*options: str, answers: str = "") -> str:
        env = dict(os.environ, APPDATA=str(tmp_path / "appdata"))
        result = subprocess.run([powershell, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(ROOT / "install.ps1"),
                                 "-Plan", "-Out", str(tmp_path / "out"), *options],
                                input=answers, capture_output=True, text=True, timeout=180, env=env, check=False)
        assert result.returncode == 0, result.stdout + result.stderr
        return result.stdout

    assert re.search(r"\(Ask '" + SEAL_QUESTION + r"[^'\n]*\?' \$false\)", text("install.ps1")), "asked, no by default"
    # (the features and families given, so the questions left are the Debugging plugin's, then this one)
    asked = ("-With", "email,tui", "-Without", "maya")
    out = run(*asked, answers="\n\n")
    assert "--signing-passphrase" not in make_line(out) and str(key) + " - made by this build" in out
    out = run(*asked, answers="\ny\n")
    assert "--signing-passphrase" in make_line(out) and "made by this build, sealed with a passphrase: make asks for it" in out
    out = run("-Yes", answers="y\ny\n")
    assert "--signing-passphrase" not in make_line(out) and "--debugging" not in make_line(out)
    out = run("-Yes", "-SigningPassphrase")
    assert "--signing-passphrase" in make_line(out) and "made by this build, sealed with a passphrase" in out
    # a sealed key: not asked, passed on as sealed
    key.parent.mkdir(parents=True)
    key.write_text(SEALED_KEY, encoding="utf-8")
    out = run(*asked, answers="\n")
    assert "--signing-passphrase" in make_line(out)
    assert str(key) + ", as it is - sealed with a passphrase: make asks for it" in out
    # a key that is not sealed: as it is, not asked (a yes would have been said to be of no use), the option too
    key.write_text(UNSEALED_KEY, encoding="utf-8")
    out = run(*asked, answers="\ny\n")
    assert "--signing-passphrase" not in make_line(out) and str(key) + ", as it is" in out and "used as it is" not in out
    out = run("-Yes", "-SigningPassphrase")
    assert "--signing-passphrase" not in make_line(out) and str(key) + ", as it is" in out and "used as it is" in out


def test_install_sh_offers_the_signing_passphrase(tmp_path):
    """install.sh's twin of test_install_ps1_offers_the_signing_passphrase, with XDG_CONFIG_HOME in a scratch folder. It
    reads its answers from the terminal, and run here with none each question takes its default, so the yes is the
    option's. Run where install.sh runs: Ubuntu or Debian."""
    os_release = Path("/etc/os-release")
    known = sys.platform.startswith("linux") and os_release.is_file() and re.search(
        r"^ID=\"?(ubuntu|debian)\"?$", os_release.read_text(encoding="utf-8"), re.MULTILINE)
    if not known or not shutil.which("sh") or not shutil.which("dpkg-query"):
        pytest.skip("install.sh runs on Ubuntu or Debian")
    key = tmp_path / "config" / "m-strata" / "plugin-signing-key.json"

    def run(*options: str) -> str:
        env = dict(os.environ, XDG_CONFIG_HOME=str(tmp_path / "config"))
        env.pop("DISPLAY", None)
        env.pop("WAYLAND_DISPLAY", None)
        # (a session of its own: no terminal to read answers from)
        result = subprocess.run(["sh", str(ROOT / "install.sh"), "--plan", "--out", str(tmp_path / "out"), *options],
                                stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=180, env=env,
                                check=False, start_new_session=True)
        assert result.returncode == 0, result.stdout + result.stderr
        return result.stdout

    asked = ("--with", "email,tui", "--without", "maya")
    out = run(*asked)
    assert SEAL_QUESTION in out and "[y/N]" in out.split(SEAL_QUESTION)[1].splitlines()[0]
    assert "--signing-passphrase" not in make_line(out) and str(key) + " - made by this build" in out
    out = run("--yes")
    assert SEAL_QUESTION not in out and "--signing-passphrase" not in make_line(out)
    out = run("--yes", "--signing-passphrase")
    assert "--signing-passphrase" in make_line(out) and "made by this build, sealed with a passphrase" in out
    key.parent.mkdir(parents=True)
    key.write_text(SEALED_KEY, encoding="utf-8")
    out = run(*asked)
    assert SEAL_QUESTION not in out and "--signing-passphrase" in make_line(out)
    assert str(key) + ", as it is - sealed with a passphrase: make asks for it" in out
    key.write_text(UNSEALED_KEY, encoding="utf-8")
    out = run("--yes", "--signing-passphrase")
    assert "--signing-passphrase" not in make_line(out) and str(key) + ", as it is" in out and "used as it is" in out


def linux_installer_host() -> bool:
    os_release = Path("/etc/os-release")
    return bool(sys.platform.startswith("linux") and os_release.is_file() and shutil.which("sh") and shutil.which("dpkg-query")
                and re.search(r"^ID=\"?(ubuntu|debian)\"?$", os_release.read_text(encoding="utf-8"), re.MULTILINE))


def test_install_sh_finds_gcc_13_before_installing_anything(tmp_path):
    """The owner's Azure VM (2026-10-30, Ubuntu 22.04): install.sh installed the CUDA Toolkit (3.3 GB) and only then said
    GCC 13 was missing. The compiler is looked at with everything else, before anything is installed: GCC 13 or newer
    is used as it is; gcc-13/g++-13 beside an older gcc are what the build uses (CC, CXX, CUDAHOSTCXX - the system's gcc
    stays); on Ubuntu 22.04 the plan installs them from Ubuntu's toolchain PPA; anywhere else an older GCC stops the
    script at once. Run as a plan on a stand-in system: an os-release file (M_STRATA_OS_RELEASE) and a PATH whose gcc
    is the one given."""
    if not linux_installer_host():
        pytest.skip("install.sh runs on Ubuntu or Debian")
    real_bin = tmp_path / "real"
    real_bin.mkdir()
    # every command of this system but its compilers, which each case puts back as it needs
    for folder in ("/usr/local/bin", "/usr/bin", "/bin"):
        for entry in Path(folder).iterdir():
            name = entry.name
            if re.match(r"^(gcc|g\+\+|cc|c\+\+|x86_64-linux-gnu-(gcc|g\+\+))(-[0-9.]+)?$", name) or (real_bin / name).exists():
                continue
            (real_bin / name).symlink_to(entry)

    def system(name: str, os_id: str, version: str, compilers: dict[str, str]) -> dict[str, str]:
        root = tmp_path / name
        (root / "bin").mkdir(parents=True)
        for program, major in compilers.items():
            fake = root / "bin" / program
            fake.write_text(f"#!/bin/sh\n[ \"$1\" = -dumpversion ] && echo {major}.4.0\n", encoding="utf-8")
            fake.chmod(0o755)
        release = root / "os-release"
        release.write_text(f'ID={os_id}\nVERSION_ID="{version}"\nPRETTY_NAME="{os_id} {version}"\n', encoding="utf-8")
        return dict(os.environ, PATH=f"{root / 'bin'}:{real_bin}", M_STRATA_OS_RELEASE=str(release),
                    XDG_CONFIG_HOME=str(root / "config"))

    def run(env: dict[str, str]) -> subprocess.CompletedProcess:
        env.pop("DISPLAY", None)
        env.pop("WAYLAND_DISPLAY", None)
        return subprocess.run(["sh", str(ROOT / "install.sh"), "--plan", "--yes", "--out", str(tmp_path / "out")],
                              stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=180, env=env, check=False,
                              start_new_session=True)

    # Ubuntu 22.04, GCC 11: the PPA's gcc-13 in the plan, the system's gcc kept
    done = run(system("jammy", "ubuntu", "22.04", {"gcc": "11"}))
    assert done.returncode == 0, done.stdout + done.stderr
    assert "GCC 13: missing - will install gcc-13 and g++-13 from Ubuntu's toolchain PPA" in done.stdout
    assert "sudo add-apt-repository -y ppa:ubuntu-toolchain-r/test" in done.stdout and "sudo apt-get install -y gcc-13 g++-13" in done.stdout
    assert "CC=gcc-13 CXX=g++-13 CUDAHOSTCXX=g++-13" in done.stdout, "the build is pointed at them"
    # gcc-13 already beside GCC 11: used, nothing to install for it
    done = run(system("jammy13", "ubuntu", "22.04", {"gcc": "11", "gcc-13": "13", "g++-13": "13"}))
    assert done.returncode == 0 and "ppa:ubuntu-toolchain-r" not in done.stdout and "CC=gcc-13 CXX=g++-13" in done.stdout, done.stdout
    # GCC 13 as gcc: used as it is
    done = run(system("noble", "ubuntu", "24.04", {"gcc": "13"}))
    assert done.returncode == 0 and "GCC: 13" in done.stdout and "CC=gcc-13" not in done.stdout, done.stdout
    # no gcc yet on Ubuntu 24.04: build-essential brings 13
    done = run(system("noble-bare", "ubuntu", "24.04", {}))
    assert done.returncode == 0 and "comes with build-essential" in done.stdout, done.stdout
    # Debian 12 (GCC 12): stopped at once, before the plan and before anything is installed
    done = run(system("bookworm", "debian", "12", {"gcc": "12"}))
    assert done.returncode != 0 and "GCC 13 or newer is needed (this system has GCC 12)" in done.stderr + done.stdout
    assert "== The plan" not in done.stdout and "Installing" not in done.stdout


def test_the_gcc_check_comes_before_any_install():
    """In the script itself: the compiler is decided in step 1, and the toolchain PPA (when it is needed) is installed
    before the CUDA Toolkit's download."""
    sh = text("install.sh")
    assert sh.index("GCC 13 or newer is needed") < sh.index("step 'The plan'")
    assert sh.index("ppa:ubuntu-toolchain-r/test") < sh.index("step 'Installing the CUDA Toolkit 13.0'")
    assert 'os_release=${M_STRATA_OS_RELEASE:-/etc/os-release}' in sh
