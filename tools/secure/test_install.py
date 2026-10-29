"""install.bat / install.ps1 / install.sh (owner 2026-10-28: "make an easy install bat that does pretty much everything,
download dependencies, install them then run the build"): what they pin is what the build pins, and they run make."""
from __future__ import annotations

import re
import shutil
import subprocess
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
    """The owner (2026-10-29): --debugging from the installer too, telling the user what it means before the build."""
    ps, sh = text("install.ps1"), text("install.sh")
    assert "[switch]$Debugging" in ps and "if ($Debugging) { $makeArgs += '--debugging' }" in ps
    assert "--debugging) debugging=1; shift ;;" in sh and '[ "$debugging" = 1 ] && make_flags="--debugging"' in sh
    assert '--with "$with" $make_flags' in sh
    for script in (ps, sh):
        for words in ("your prompts", "keep the server's log private", "before you share it", "without the debugging option"):
            assert words in script, words
