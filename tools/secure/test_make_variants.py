"""Both compiler-math variants ship separately with verified build identities."""
import hashlib
import json
import os
import shutil
import subprocess
from dataclasses import replace

import pytest

from test_make import make, options, stage, an_rtx_5090


def test_dual_variant_configures_separate_trees_with_explicit_modes(tmp_path):
    opts = replace(options(), build_dir=tmp_path / "build", engine_build_dir=tmp_path / "accurate")
    accurate, fast = make.engine_variants(opts)
    assert accurate.engine_build_dir == opts.engine_build_dir
    assert fast.engine_build_dir == opts.build_dir / "engine-fast-math"
    assert accurate.engine_build_dir != fast.engine_build_dir
    for platform in ("windows", "linux"):
        assert "-DMULTIENGINE_FAST_MATH=OFF" in make.engine_configure_args(accurate, platform)
        assert "-DMULTIENGINE_FAST_MATH=ON" in make.engine_configure_args(fast, platform)
    with pytest.raises(SystemExit, match="both.*card"):
        options("--fast-math")


def test_both_variants_require_all_runtime_sidecars():
    for platform in ("windows", "linux"):
        files = make.required_files(options(platform=platform), platform)
        assert "engine/variants.json" in files
        for name in files:
            if name.startswith("engine/") and not name.startswith("engine/fast-math/") and name not in (
                    "engine/build.json", "engine/variants.json"):
                assert "engine/fast-math/" + name.removeprefix("engine/") in files


def cache(root, fast, maya=True):
    root.mkdir(parents=True, exist_ok=True)
    (root / "CMakeCache.txt").write_text("MULTIENGINE_FAST_MATH:BOOL=" + ("ON" if fast else "OFF") + "\n")
    if maya:
        cache(root / "maya", fast, False)


def test_variant_manifest_binds_self_contained_files_and_refuses_wrong_cache(tmp_path):
    opts = replace(options(), build_dir=tmp_path / "build", engine_build_dir=tmp_path / "accurate")
    accurate, fast = make.engine_variants(opts)
    cache(accurate.engine_build_dir, False)
    cache(fast.engine_build_dir, True)
    root = stage(tmp_path / "stage", "windows")
    make.write_variant_manifest(root, opts, "windows")
    document = json.loads((root / "engine/variants.json").read_text())
    assert document["schema_version"] == 1
    assert [(v["name"], v["directory"], v["fast_math"]) for v in document["variants"]] == [
        ("accurate", ".", False), ("fast-math", "fast-math", True)]
    for variant in document["variants"]:
        assert variant["executable"] == "multiengine.exe"
        relative_files = {row["path"] for row in variant["files"]}
        assert "multiengine.exe" in relative_files and "strata_kq.dll" in relative_files
        assert "cublas64_13.dll" in relative_files and "vcruntime140.dll" in relative_files
        assert not any(path.startswith("fast-math/") for path in relative_files)
        for row in variant["files"]:
            data = (root / "engine" / variant["directory"] / row["path"]).read_bytes()
            assert row["bytes"] == len(data) and row["sha256"] == hashlib.sha256(data).hexdigest()
    cache(fast.engine_build_dir / "maya", False, False)
    with pytest.raises(SystemExit, match="math.*cache|cache.*math"):
        make.write_variant_manifest(root, opts, "windows")


def test_fast_variant_missing_sidecar_and_unexpected_files_are_refused(tmp_path):
    root = stage(tmp_path, "windows", skip=("engine/fast-math/strata_kq.dll",),
                 extra=("engine/fast-math/source.cpp",))
    errors = "\n".join(make.layout_problems(root, options(), "windows"))
    assert "engine/fast-math/strata_kq.dll: missing" in errors
    assert "engine/fast-math/source.cpp: not a runtime file" in errors


def test_plan_builds_and_installs_both_variants_before_frontend(tmp_path, monkeypatch):
    commands = []
    monkeypatch.setattr(make, "find_tool", lambda *args: "cmake")
    monkeypatch.setattr(make, "run", lambda command, env, plan: commands.append(command))
    assert make.main(["--out", str(tmp_path / "dist"), "--build-dir", str(tmp_path / "build"),
                      "--ggml-dir", "fixture-ggml", "--cuda-root", "fixture-cuda", "--cuda-arch", "120-real",
                      "--without", "vision", "--plan"]) == 0
    configured = [c for c in commands if "-DMULTIENGINE_FAST_MATH=OFF" in c or "-DMULTIENGINE_FAST_MATH=ON" in c]
    assert len(configured) == 2
    assert "-DMULTIENGINE_FAST_MATH=OFF" in configured[0] and "-DMULTIENGINE_FAST_MATH=ON" in configured[1]
    builds = [c[c.index("--build") + 1] for c in commands if "--build" in c and "multiengine" in c]
    assert builds == [str(tmp_path / "build/engine"), str(tmp_path / "build/engine-fast-math")]
    installed = [c for c in commands if "--install" in c]
    assert len(installed) == 5  # two engines, each runtime+cuBLAS, then frontend
    assert installed[2][installed[2].index("--prefix") + 1].endswith("fast-variant-install")


def test_linux_cuda_install_materializes_soname_links(tmp_path):
    cmake = shutil.which("cmake")
    if os.name == "nt" or not cmake:
        pytest.skip("requires Linux CMake installation semantics")
    libraries = tmp_path / "cuda"
    libraries.mkdir()
    for name in ("libcublas", "libcublasLt"):
        (libraries / (name + ".so.13.2.1")).write_bytes(name.encode())
        (libraries / (name + ".so.13")).symlink_to(name + ".so.13.2.1")
    source = (make.ROOT / "engine/cmake/multiengine.cmake").read_text()
    install_rules = source.split("if(MULTIENGINE_BUNDLE_CUBLAS)", 1)[1].rsplit(
        'include("${CMAKE_CURRENT_LIST_DIR}/multiengine_accuracy.cmake")', 1)[0]
    (tmp_path / "CMakeLists.txt").write_text(
        'cmake_minimum_required(VERSION 3.20)\nproject(cuda_install_fixture NONE)\n'
        'function(find_package)\nendfunction()\nset(MULTIENGINE_BUNDLE_CUBLAS ON)\n'
        f'set(CUDAToolkit_LIBRARY_DIR "{libraries.as_posix()}")\n'
        'if(MULTIENGINE_BUNDLE_CUBLAS)' + install_rules)
    subprocess.run([cmake, "-S", str(tmp_path), "-B", str(tmp_path / "build")], check=True, capture_output=True)
    subprocess.run([cmake, "--install", str(tmp_path / "build"), "--prefix", str(tmp_path / "dist"),
                    "--component", "cuda"], check=True, capture_output=True)
    for name in ("libcublas", "libcublasLt"):
        installed = tmp_path / "dist/engine" / (name + ".so.13")
        assert installed.is_file() and not installed.is_symlink()
        assert installed.read_bytes() == name.encode()
