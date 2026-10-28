"""make.py (the repository root's build-everything script): the commands it runs and the out folder it accepts.

Only pure parts are tested here (argument lists, the layout check, the example config, the permission commands); a
real build is checked by running make.py itself (docs/superpowers/specs/2026-10-27-gui-first-install-design.md §2).
"""
from __future__ import annotations

import importlib.util
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
_spec = importlib.util.spec_from_file_location("strata_make", ROOT / "make.py")
make = importlib.util.module_from_spec(_spec)
sys.modules["strata_make"] = make
_spec.loader.exec_module(make)

HOOK_MARKER = b"SECURE_FRONTEND_TEST_" + b"HOOKS_COMPILED_IN"


def options(*extra: str, platform: str = "windows"):
    return make.parse_args(["--out", str(ROOT / "out" / "dist-test"), "--ggml-dir", "G:/llama.cpp",
                            "--cuda-root", "C:/CUDA/v13.0", *extra], platform=platform)


def test_engine_configure_is_the_tested_box_build():
    args = make.engine_configure_args(options(), "windows")
    for expected in ("-DCMAKE_BUILD_TYPE=Release", "-DSTRATA_ENABLE_CUDA=ON", "-DSTRATA_BUILD_TESTS=OFF",
                     "-DSTRATA_NATIVE_EXPERTS=ON", "-DSTRATA_Q6K_EXPERTS=ON", "-DSTRATA_MMQ_KQUANTS=ON",
                     "-DSTRATA_BUILD_MAYA=ON", "-DSTRATA_BUILD_RAON1=ON", "-DSTRATA_GLM_WERROR=OFF",
                     "-DMULTIENGINE_BUNDLE_CUBLAS=ON", "-DSTRATA_GGML_DIR=G:/llama.cpp",
                     "-DCUDAToolkit_ROOT=C:/CUDA/v13.0", "-DCMAKE_CUDA_COMPILER=C:/CUDA/v13.0/bin/nvcc.exe",
                     "-DCMAKE_CUDA_ARCHITECTURES=native"):
        assert expected in args, expected
    assert args[args.index("-G") + 1] == "Ninja"


def test_an_existing_build_never_gets_its_compiler_respelled(tmp_path):
    """CMake deletes a build's cache when CMAKE_CUDA_COMPILER differs from the cached spelling (even C:\\ vs C:/) and
    reconfigures without the other options; a reused build folder keeps its own compiler."""
    reused = make.parse_args(["--out", str(tmp_path / "out"), "--ggml-dir", "G:/llama.cpp", "--cuda-root", "C:/CUDA/v13.0",
                              "--engine-build-dir", str(tmp_path)], platform="windows")
    (tmp_path / "CMakeCache.txt").write_text("CMAKE_CUDA_COMPILER:FILEPATH=C:\\CUDA\\v13.0\\bin\\nvcc.exe\n")
    args = make.engine_configure_args(reused, "windows")
    assert not any(a.startswith("-DCMAKE_CUDA_COMPILER=") for a in args)
    assert "-DSTRATA_ENABLE_CUDA=ON" in args and "-DSTRATA_BUILD_MAYA=ON" in args


def test_an_existing_build_keeps_its_make_program(tmp_path):
    """A different ninja reads another version's dependency log as stale and rebuilds everything: a reused build folder
    keeps the make program in its cache; a new one gets the one found."""
    assert make.make_program_args(tmp_path, "C:/ninja.exe") == []  # no ninja file: nothing to pass
    (tmp_path / "ninja.exe").write_text("")
    assert make.make_program_args(tmp_path, str(tmp_path / "ninja.exe")) == ["-DCMAKE_MAKE_PROGRAM=" + str(tmp_path / "ninja.exe")]
    (tmp_path / "CMakeCache.txt").write_text("CMAKE_MAKE_PROGRAM:FILEPATH=G:/venv/ninja.exe\n")
    assert make.make_program_args(tmp_path, str(tmp_path / "ninja.exe")) == []


def test_families_and_cuda_options_follow_the_command_line():
    args = make.engine_configure_args(options("--without", "maya,raon1", "--cuda-arch", "86;89",
                                              "--no-cuda-runtime"), "linux")
    assert "-DSTRATA_BUILD_MAYA=OFF" in args and "-DSTRATA_BUILD_RAON1=OFF" in args
    assert "-DCMAKE_CUDA_ARCHITECTURES=86;89" in args and "-DMULTIENGINE_BUNDLE_CUBLAS=OFF" in args
    assert "-DCMAKE_CUDA_COMPILER=C:/CUDA/v13.0/bin/nvcc" in args


def test_unknown_family_is_refused():
    try:
        options("--without", "flint")
    except SystemExit as exc:
        assert "maya" in str(exc) and "raon1" in str(exc)
    else:
        raise AssertionError("flint is part of every engine build; only maya and raon1 can be left out")


def test_frontend_configure_is_a_release_build_without_test_hooks():
    args = make.frontend_configure_args(options(), "windows")
    for expected in ("-DCMAKE_BUILD_TYPE=Release", "-DBUILD_TESTING=OFF", "-DSTRATA_BUILD_SECURE_FRONTEND_TESTS=OFF",
                     "-DSECURE_FRONTEND_TEST_HOOKS=OFF", "-DSECURE_FRONTEND_WERROR=ON"):
        assert expected in args, expected


def test_email_alerts_are_built_only_when_asked():
    """Owner, 2026-10-27: the email capability is a build option, off by default; our builds pass --with email."""
    assert "-DSECURE_FRONTEND_EMAIL_ALERTS=OFF" in make.frontend_configure_args(options(), "windows")
    assert "-DSECURE_FRONTEND_EMAIL_ALERTS=ON" in make.frontend_configure_args(options("--with", "email"), "linux")
    try:
        options("--with", "telemetry")
    except SystemExit as exc:
        assert "email" in str(exc)
    else:
        raise AssertionError("--with takes only the optional features it knows")


def test_web_search_is_built_only_when_asked(tmp_path):
    """Owner, 2026-10-27: web search is a build option, off by default, on in our builds (--with search); with it,
    strata-search - the MCP server the server runs for it, the install's only search connection - sits beside the
    server, and an out folder without it is incomplete."""
    assert "-DSECURE_FRONTEND_WEB_SEARCH=OFF" in make.frontend_configure_args(options(), "windows")
    assert "-DSECURE_FRONTEND_WEB_SEARCH=ON" in make.frontend_configure_args(options("--with", "search"), "linux")
    both = make.frontend_configure_args(options("--with", "email,search"), "windows")
    assert "-DSECURE_FRONTEND_EMAIL_ALERTS=ON" in both and "-DSECURE_FRONTEND_WEB_SEARCH=ON" in both
    assert "strata-search.exe" in make.required_files(options("--with", "search"), "windows")
    assert "strata-search" in make.required_files(options("--with", "search", platform="linux"), "linux")
    assert not [f for f in make.required_files(options(), "windows") if "strata-search" in f]
    root = stage(tmp_path, "windows")
    problems = "\n".join(make.layout_problems(root, options("--with", "search"), "windows"))
    assert "strata-search.exe: missing" in problems, problems
    assert "strata-search" not in "\n".join(make.layout_problems(root, options(), "windows"))


MAYA_PROFILES = ("expert-profile-glm.bin", "expert-profile-ds4.bin", "expert-profile-mm3.bin")
# MiniMax-M3.1-Flash (owner 2026-10-28): Maya's chat template every MiniMax pack carries, with its licence beside it
MINIMAX_FILES = ("engine/templates/minimax-m3.1/chat_template.jinja", "engine/templates/minimax-m3.1/LICENSE.minimax-m31")


def stage(tmp_path: Path, platform: str, extra: tuple[str, ...] = (), skip: tuple[str, ...] = ()) -> Path:
    exe = ".exe" if platform == "windows" else ""
    lib = (lambda n: n + ".dll") if platform == "windows" else (lambda n: "lib" + n + ".so")
    files = [f"strata-secure-server{exe}", f"strata{exe}", f"engine/multiengine{exe}", f"engine/strata-prepare{exe}",
             "engine/profiles/expert-profile.bin", "engine/profiles/expert-profile-coder.bin", "engine/build.json",
             "README.txt", "config.example.json", "LICENSE", "NOTICE.md"] + ["engine/profiles/" + p for p in MAYA_PROFILES] + \
        list(MINIMAX_FILES)
    files += ["engine/" + lib(n) for n in ("multiengine_strata", "multiengine_gemma4", "multiengine_flint",
                                           "multiengine_raon1", "multiengine_maya", "multiengine_maya_ds4",
                                           "multiengine_maya_mm3", "strata_kq")]
    files += (["engine/cublas64_13.dll", "engine/cublasLt64_13.dll", "msvcp140.dll", "engine/vcruntime140.dll"]
              if platform == "windows" else ["engine/libcublas.so.13", "engine/libcublasLt.so.13"])
    for name in list(files) + list(extra):
        if name in skip:
            continue
        path = tmp_path / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b"binary")
    return tmp_path


def test_a_runtime_folder_passes(tmp_path):
    for platform in ("windows", "linux"):
        root = stage(tmp_path / platform, platform)
        assert make.layout_problems(root, options(platform=platform), platform) == []


def test_build_leftovers_and_sources_are_refused(tmp_path):
    root = stage(tmp_path, "windows", extra=("engine/multiengine_strata.lib", "engine/multiengine.pdb",
                                             "engine/f_host_tests.exe", "engine/verify_parity.exe",
                                             "engine/multiengine_glm.dll", "tools/make.py", "src/launcher.cpp",
                                             "CMakeCache.txt"))
    problems = "\n".join(make.layout_problems(root, options(), "windows"))
    for name in ("multiengine_strata.lib", "multiengine.pdb", "f_host_tests.exe", "verify_parity.exe",
                 "multiengine_glm.dll", "make.py", "launcher.cpp", "cmakecache.txt"):
        assert name in problems.lower(), name


def test_missing_runtime_files_are_reported(tmp_path):
    root = stage(tmp_path, "windows", skip=("strata.exe", "engine/multiengine_flint.dll", "engine/cublasLt64_13.dll"))
    problems = "\n".join(make.layout_problems(root, options(), "windows"))
    assert "strata.exe" in problems and "multiengine_flint.dll" in problems and "cublasLt" in problems
    # without the bundled runtime, cuBLAS is neither required nor allowed
    lean = stage(tmp_path / "lean", "windows", skip=("engine/cublas64_13.dll", "engine/cublasLt64_13.dll"))
    assert make.layout_problems(lean, options("--no-cuda-runtime"), "windows") == []
    # a family left out is not required
    no_maya = stage(tmp_path / "nomaya", "windows", skip=("engine/multiengine_maya.dll", "engine/multiengine_maya_ds4.dll",
                                                          "engine/multiengine_maya_mm3.dll") +
                    tuple("engine/profiles/" + p for p in MAYA_PROFILES) + MINIMAX_FILES)
    assert make.layout_problems(no_maya, options("--without", "maya"), "windows") == []


def test_strata_prepare_and_the_expert_profiles_are_runtime_files(tmp_path):
    """Owner, 2026-10-27 ("executables to handle what needs to be done so we don't have any other dependencies"): the
    native pack tool and the expert profiles a Qwen3.8 card names are part of every out folder."""
    root = stage(tmp_path, "windows", skip=("engine/strata-prepare.exe", "engine/profiles/expert-profile.bin"))
    problems = "\n".join(make.layout_problems(root, options(), "windows"))
    assert "engine/strata-prepare.exe: missing" in problems and "engine/profiles/expert-profile.bin: missing" in problems, problems


def test_maya_expert_profiles_come_with_the_maya_families(tmp_path):
    """The expert profiles Maya's setup starts GLM-5.3, DeepSeek and MiniMax from: in every out folder with the Maya
    families (the server passes expert-profile-glm.bin to each GLM launch, as Maya's setup does), not without them."""
    root = stage(tmp_path, "linux", skip=("engine/profiles/expert-profile-glm.bin",))
    problems = "\n".join(make.layout_problems(root, options(platform="linux"), "linux"))
    assert "engine/profiles/expert-profile-glm.bin: missing" in problems, problems


def test_minimax_template_and_licence_come_with_the_maya_families(tmp_path):
    """MiniMax-M3.1-Flash (owner 2026-10-28, under the MiniMax Community License): Maya's chat template, which the server
    hands strata-prepare for every MiniMax pack, and the licence that covers it (its notice goes with every copy), in
    every out folder with the Maya families."""
    for missing in MINIMAX_FILES:
        root = stage(tmp_path / missing.rsplit("/", 1)[-1], "windows", skip=(missing,))
        problems = "\n".join(make.layout_problems(root, options(), "windows"))
        assert f"{missing}: missing" in problems, problems


def test_the_license_and_notices_come_with_every_out_folder(tmp_path):
    """Owner 2026-10-28 (the AGPL-3.0 licence): the out folder is a copy of the program, so it carries the
    AGPL's text and NOTICE.md (the copyright, the MIT notice of Strata, the MiniMax permission), as the root holds them."""
    for name in ("LICENSE", "NOTICE.md"):
        root = stage(tmp_path / name, "windows", skip=(name,))
        problems = "\n".join(make.layout_problems(root, options(), "windows"))
        assert f"{name}: missing" in problems, problems


def test_the_folder_readme_names_its_license_and_how_to_start_it():
    """Owner 2026-10-28 ("make sure the readme is up to date and ensure the syntax for configuring and starting it are
    correct"): README.txt says the folder's licence and files, starts the server as each shell types it, gives options
    with --data-dir (alone they do not read data/), and names the generation time limit."""
    windows = make.readme_text(options("--with", "email,search"), "windows")
    linux = make.readme_text(options(platform="linux"), "linux")
    for text in (windows, linux):
        assert "AGPL-3.0" in text and "LICENSE" in text and "NOTICE.md" in text
        assert "--data-dir data --port" in text and "limits.engine.generation_timeout_s" in text
        assert "strata-secure-server --help" in text or "strata-secure-server.exe --help" in text
    assert ".\\strata-secure-server.exe" in windows and "./strata-secure-server" in linux
    assert "engine/templates/" in windows and "strata-search" in windows and "strata-search" not in linux


def test_a_test_hooks_build_is_refused(tmp_path):
    root = stage(tmp_path, "windows")
    (root / "strata-secure-server.exe").write_bytes(b"MZ..." + HOOK_MARKER + b"...")
    problems = "\n".join(make.layout_problems(root, options(), "windows"))
    assert "test-hooks" in problems or "TEST_HOOKS" in problems


def test_the_build_record_names_each_engine_and_its_source(tmp_path):
    """Owner, 2026-10-27 ("in the about, it should list the build of our engines"): engine/build.json, which the server's
    About shows - when and from which commit the folder was built, the pinned ggml, and each family library with the
    models it runs and the source it is built from (read from the tree at build time)."""
    root = stage(tmp_path, "windows")
    record = make.build_record(options(), "windows", root, commit="0123456789ab", built="2026-10-27T23:00:00Z")
    assert record["schema_version"] == 1 and record["commit"] == "0123456789ab" and record["built"] == "2026-10-27T23:00:00Z"
    ggml = (ROOT / "engine" / "third_party" / "ggml" / "VERSION.txt").read_text(encoding="utf-8").split()[0][:12]
    maya = (ROOT / "engine" / "third_party" / "maya" / "VERSION").read_text(encoding="utf-8").strip()
    assert record["ggml"] == ggml and record["cuda_arch"] == "native" and record["features"] == []
    engines = {e["library"]: e for e in record["engines"]}
    assert list(engines) == ["multiengine_strata.dll", "multiengine_gemma4.dll", "multiengine_flint.dll", "multiengine_raon1.dll",
                             "multiengine_maya.dll", "multiengine_maya_ds4.dll", "multiengine_maya_mm3.dll"]
    assert engines["multiengine_maya.dll"]["source"] == "Project Maya " + maya and "GLM-5.3" in engines["multiengine_maya.dll"]["models"]
    assert engines["multiengine_strata.dll"]["source"].startswith("upstream Strata, engine 0.")
    assert "Raon-1-mini" in engines["multiengine_raon1.dll"]["models"] and "Project Maya" in engines["multiengine_raon1.dll"]["source"]
    assert all(e["bytes"] == len(b"binary") for e in record["engines"])
    lean = make.build_record(options("--without", "maya,raon1", "--with", "search", platform="linux"), "linux",
                             stage(tmp_path / "linux", "linux"), commit="x", built="y")
    assert [e["library"] for e in lean["engines"]] == ["libmultiengine_strata.so", "libmultiengine_gemma4.so", "libmultiengine_flint.so"]
    assert lean["features"] == ["search"]
    assert "engine/build.json" in make.required_files(options(), "windows")
    json.dumps(record)   # it is written as JSON


def test_the_example_config_is_a_launcher_config_for_the_folder():
    config = json.loads(make.example_config_text("windows"))
    assert config["schema_version"] == 1
    assert config["listener"]["host"] == "127.0.0.1" and config["listener"]["web_ui"] is True
    assert config["models"]["control"] is True and config["models"]["downloads"] is True
    assert "token" in config and "roots" in config
    assert "cards" not in config["models"] or config["models"]["cards"] == []


def test_owner_only_commands():
    windows = make.owner_only_commands(Path("D:/SecureStrata"), "windows", user="me")
    assert windows[0][:2] == ["icacls", str(Path("D:/SecureStrata"))] and "/inheritance:r" in windows[0]
    assert any("me:(OI)(CI)F" in arg for arg in windows[0])
    assert any(cmd[-1] == "/T" and "/reset" in cmd for cmd in windows[1:])
    posix = make.owner_only_commands(Path("/opt/secure-strata"), "linux", user="me")
    assert posix == [["chmod", "-R", "go-w", "/opt/secure-strata"]]
