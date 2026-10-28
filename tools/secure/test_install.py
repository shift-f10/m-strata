"""install.bat / install.ps1 / install.sh (owner 2026-10-28: "make an easy install bat that does pretty much everything,
download dependencies, install them then run the build"): what they pin is what the build pins, and they run make."""
from __future__ import annotations

import re
from pathlib import Path

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


def test_the_cpp_workload_is_what_make_asks_for():
    """The script installs, or adds to Build Tools, the workload make's own message names."""
    ps1 = text("install.ps1")
    assert ps1.count("Microsoft.VisualStudio.Workload.VCTools") == 2
    assert "Microsoft.VisualStudio.Workload.VCTools" in text("make.py")


def test_batch_and_powershell_line_endings():
    attributes = text(".gitattributes")
    assert "*.bat text eol=crlf" in attributes and "*.ps1 text eol=crlf" in attributes and "*.sh text eol=lf" in attributes
    assert (ROOT / "install.ps1").read_bytes().startswith(b"\xef\xbb\xbf"), "a BOM: Windows PowerShell 5.1 reads UTF-8 by it"
