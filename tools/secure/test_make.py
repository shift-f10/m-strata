"""make.py (the repository root's build-everything script): the commands it runs and the out folder it accepts.

Only pure parts are tested here (argument lists, the layout check, the example config, the permission commands); a
real build is checked by running make.py itself (docs/superpowers/specs/2026-10-27-gui-first-install-design.md §2).
"""
from __future__ import annotations

import hashlib
import importlib.util
import json
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
_spec = importlib.util.spec_from_file_location("strata_make", ROOT / "make.py")
make = importlib.util.module_from_spec(_spec)
sys.modules["strata_make"] = make
_spec.loader.exec_module(make)

HOOK_MARKER = b"SECURE_FRONTEND_TEST_" + b"HOOKS_COMPILED_IN"


@pytest.fixture(autouse=True)
def an_rtx_5090(monkeypatch):
    """The GPUs make finds (nvidia-smi's compute capabilities): one RTX 5090 unless a case says otherwise."""
    monkeypatch.setattr(make, "gpu_compute_capabilities", lambda: ["12.0"], raising=False)


def options(*extra: str, platform: str = "windows"):
    # A build folder that never exists (inside the --out that is never made): the arguments are a fresh build's
    # whatever this PC has built in out/make-build (2026-10-28: two cases failed once a real build sat there).
    return make.parse_args(["--out", str(ROOT / "out" / "dist-test"), "--build-dir", str(ROOT / "out" / "dist-test" / "make-build"),
                            "--ggml-dir", "G:/llama.cpp", "--cuda-root", "C:/CUDA/v13.0", *extra], platform=platform)


def test_engine_configure_is_the_tested_box_build():
    args = make.engine_configure_args(options(), "windows")
    for expected in ("-DCMAKE_BUILD_TYPE=Release", "-DSTRATA_ENABLE_CUDA=ON", "-DSTRATA_BUILD_TESTS=OFF",
                     "-DSTRATA_NATIVE_EXPERTS=ON", "-DSTRATA_Q6K_EXPERTS=ON", "-DSTRATA_MMQ_KQUANTS=ON",
                     "-DSTRATA_BUILD_MAYA=ON", "-DSTRATA_BUILD_RAON1=ON", "-DSTRATA_GLM_WERROR=OFF",
                     "-DMULTIENGINE_BUNDLE_CUBLAS=ON", "-DSTRATA_GGML_DIR=G:/llama.cpp",
                     "-DCUDAToolkit_ROOT=C:/CUDA/v13.0", "-DCMAKE_CUDA_COMPILER=C:/CUDA/v13.0/bin/nvcc.exe",
                     "-DCMAKE_CUDA_ARCHITECTURES=120-real"):
        assert expected in args, expected
    assert args[args.index("-G") + 1] == "Ninja"


EVERY_GENERATION = "75-real;80-real;86-real;89-real;90-real;120"


def test_the_gpu_targets_are_this_pcs_card_types(monkeypatch):
    """Owner 2026-10-29 ("I am building it for a 5090, it should detect what card I have"; a native build stopped in
    ptxas on it; then "to make it lightweight ... detect the card and target that specifically, and if there is multiple
    cards, only those card types"; "if they have unsupported cards, then we compile to only supported ones and if none is
    present we say no supported card detected"): by default each supported card type nvidia-smi reports (compute
    capability 7.5 or newer, the engines' own rule), once, as an explicit number with its machine code only; others are
    left out, and with none make stops and says so. --cuda-arch compatible is every generation the engines build for -
    Turing, Ampere, Ada, Hopper, Blackwell, all built here 2026-10-29 - with PTX for the newest, which later GPUs compile
    at load. A list given is kept."""
    for found, targets in ((["12.0"], "120-real"), (["8.9", "12.0", "12.0"], "89-real;120-real"), (["7.5"], "75-real"),
                           (["12.1", "8.6"], "86-real;121-real"), (["6.1", "12.0", "7.0"], "120-real")):
        monkeypatch.setattr(make, "gpu_compute_capabilities", lambda found=found: found)
        assert options().cuda_arch == targets, (found, options().cuda_arch)
    for found in ([], ["6.1"], ["6.1", "7.0"]):
        monkeypatch.setattr(make, "gpu_compute_capabilities", lambda found=found: found)
        with pytest.raises(SystemExit) as stopped:
            options()
        assert "no supported NVIDIA card detected" in str(stopped.value) and "--cuda-arch" in str(stopped.value), found

    def never():
        raise AssertionError("a list given is not looked up")
    monkeypatch.setattr(make, "gpu_compute_capabilities", never)
    assert options("--cuda-arch", "86;89").cuda_arch == "86;89" and options("--cuda-arch", "native").cuda_arch == "native"
    assert options("--cuda-arch", "compatible").cuda_arch == EVERY_GENERATION


def test_a_crashed_cuda_build_is_tried_once_more_with_fewer_jobs(monkeypatch):
    """The every-generation engine build here (2026-10-29, 20 jobs): CUDA's compiler died on two of llama.cpp's MMQ units
    (ptxas 0xC0000005, cicc 0xC0000374) and built them at 4 jobs. A failed CUDA build runs once more with a quarter of
    the jobs (what is built is kept); a second failure stops make, and one job has nothing fewer to try."""
    calls = []

    def flaky(command, env, plan):
        calls.append(list(command))
        if len(calls) == 1:
            raise subprocess.CalledProcessError(1, command)
    monkeypatch.setattr(make, "run", flaky)
    make.run_cuda_build(["cmake", "--build", "b", "--target", "multiengine"], 32, {}, False)
    assert [c[-2:] for c in calls] == [["-j", "32"], ["-j", "8"]] and calls[1][:5] == ["cmake", "--build", "b", "--target", "multiengine"]

    def broken(command, env, plan):
        calls.append(list(command))
        raise subprocess.CalledProcessError(1, command)
    monkeypatch.setattr(make, "run", broken)
    for jobs, tries in ((32, 2), (1, 1)):
        calls.clear()
        with pytest.raises(subprocess.CalledProcessError):
            make.run_cuda_build(["cmake", "--build", "b"], jobs, {}, False)
        assert len(calls) == tries, (jobs, calls)


def test_nvidia_smis_compute_capabilities_are_read():
    assert make.compute_capabilities_from("12.0\r\n8.9\n\n  7.5 \nN/A\n[Not Supported]\n") == ["12.0", "8.9", "7.5"]
    assert make.compute_capabilities_from("") == []


def test_the_fp4_units_get_the_arch_specific_targets(tmp_path):
    """RTX 50's FP4 MMA (mma with block scale) exists only in the arch-specific sm_12xa, and a plain sm_120 is refused by
    ptxas (the 5090's native build): engine/cmake/strata_fp4_archs.cmake gives the two NVFP4 W4A4 units the build's
    targets with "native" / "all" spelled out by CMake and every 12x made 12xa; older targets stay as they are."""
    cmake = shutil.which("cmake") or str(Path(sys.executable).parent / ("cmake.exe" if sys.platform == "win32" else "cmake"))
    if not Path(cmake).exists() and not shutil.which("cmake"):
        pytest.skip("cmake is not installed")
    module = (ROOT / "engine" / "cmake" / "strata_fp4_archs.cmake").as_posix()
    cases = (("75;120", "", "75;120a"), ("120-real;89-virtual", "", "120a-real;89-virtual"), ("native", "120-real", "120a-real"),
             ("native", "89-real", "89-real"), ("75;native", "120-real;86-real", "75;120a-real;86-real"), ("121", "", "121a"))
    for given, native, expected in cases:
        script = tmp_path / "probe.cmake"
        script.write_text(f'set(CMAKE_CUDA_ARCHITECTURES "{given}")\nset(CMAKE_CUDA_ARCHITECTURES_NATIVE "{native}")\n'
                          f'include("{module}")\nstrata_fp4_cuda_archs(result)\nmessage(STATUS "FP4=[${{result}}]")\n',
                          encoding="utf-8")
        done = subprocess.run([cmake, "-P", str(script)], capture_output=True, text=True, timeout=60, check=False)
        assert done.returncode == 0, done.stderr
        assert f"FP4=[{expected}]" in done.stdout + done.stderr, (given, native, done.stdout, done.stderr)
    for listing in ("engine/CMakeLists.txt", "engine/src/gemma4/CMakeLists.txt"):
        text = (ROOT / listing).read_text(encoding="utf-8")
        assert "strata_fp4_cuda_archs(" in text and 'REPLACE "^(12[0-9])$"' not in text, listing


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


def test_the_picture_encoder_is_upstreams_built_out_of_source(tmp_path):
    """Vision for cards (spec 2026-10-28 point 1): upstream's strata-vision (upstream/tools/vision, over llama.cpp's mtmd at
    the engine's pinned checkout) is built out of source with ggml-cuda for the engine's GPU targets, portable (AVX2, static
    runtime), into engine/ beside the launcher, where the server looks for it; --without vision leaves it out."""
    args = make.vision_configure_args(options("--cuda-arch", "86;89"), "windows")
    assert args[args.index("-S") + 1] == str(ROOT / "upstream" / "tools" / "vision")
    assert args[args.index("-B") + 1] == str(ROOT / "out" / "dist-test" / "make-build" / "vision")
    for expected in ("-DCMAKE_BUILD_TYPE=Release", "-DLLAMA_DIR=G:/llama.cpp", "-DSTRATA_VISION_CUDA=ON", "-DSTRATA_PORTABLE=ON",
                     "-DCUDAToolkit_ROOT=C:/CUDA/v13.0", "-DCMAKE_CUDA_COMPILER=C:/CUDA/v13.0/bin/nvcc.exe",
                     "-DCMAKE_CUDA_ARCHITECTURES=86;89"):
        assert expected in args, expected
    assert args[args.index("-G") + 1] == "Ninja"
    reused = make.parse_args(["--out", str(tmp_path / "out"), "--build-dir", str(tmp_path), "--ggml-dir", "G:/llama.cpp",
                              "--cuda-root", "C:/CUDA/v13.0"], platform="linux")
    (tmp_path / "vision").mkdir()
    (tmp_path / "vision" / "CMakeCache.txt").write_text("CMAKE_CUDA_COMPILER:FILEPATH=/usr/local/cuda/bin/nvcc\n")
    assert not any(a.startswith("-DCMAKE_CUDA_COMPILER=") for a in make.vision_configure_args(reused, "linux"))
    assert "engine/strata-vision.exe" in make.required_files(options(), "windows")
    assert "engine/strata-vision" in make.required_files(options(platform="linux"), "linux")
    assert not [f for f in make.required_files(options("--without", "vision"), "windows") if "strata-vision" in f]
    root = stage(tmp_path / "stage", "windows", skip=("engine/strata-vision.exe",))
    assert "engine/strata-vision.exe: missing" in "\n".join(make.layout_problems(root, options(), "windows"))
    assert make.layout_problems(root, options("--without", "vision"), "windows") == []
    record = make.build_record(options(), "windows", stage(tmp_path / "record", "windows"), commit="x", built="y")
    encoder = [e for e in record["engines"] if e["library"] == "strata-vision.exe"]
    assert encoder and "pictures" in encoder[0]["models"] and "upstream Strata" in encoder[0]["source"], record["engines"]
    lean = make.build_record(options("--without", "vision"), "windows", root, commit="x", built="y")
    assert not [e for e in lean["engines"] if e["library"].startswith("strata-vision")]


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
    strata-search - the MCP server the server runs for it, the install's only search connection - is in the Web search
    plugin's folder, plugins/search/ (owner 2026-10-30: search is a plugin), and an out folder without it is
    incomplete."""
    assert "-DSECURE_FRONTEND_WEB_SEARCH=OFF" in make.frontend_configure_args(options(), "windows")
    assert "-DSECURE_FRONTEND_WEB_SEARCH=ON" in make.frontend_configure_args(options("--with", "search"), "linux")
    both = make.frontend_configure_args(options("--with", "email,search"), "windows")
    assert "-DSECURE_FRONTEND_EMAIL_ALERTS=ON" in both and "-DSECURE_FRONTEND_WEB_SEARCH=ON" in both
    assert "plugins/search/strata-search.exe" in make.required_files(options("--with", "search"), "windows")
    assert "plugins/search/strata-search" in make.required_files(options("--with", "search", platform="linux"), "linux")
    assert not [f for f in make.required_files(options(), "windows") if "strata-search" in f]
    root = stage(tmp_path, "windows")
    problems = make.layout_problems(root, options("--with", "search"), "windows")
    assert "plugins/search/strata-search.exe: missing" in problems, problems
    assert "strata-search" not in "\n".join(make.layout_problems(root, options(), "windows"))


def test_the_window_is_built_only_when_asked(tmp_path):
    """Owner, 2026-10-28 ("instead of a console application, we should make it into a window application ... minimized
    into the tray"): strata-window is a build option, off by default (--with window); with it the program sits beside
    the server, README.txt says to start it, and an out folder without it is incomplete. The page's settings are in
    its Settings tab."""
    assert "-DSECURE_FRONTEND_WINDOW=OFF" in make.frontend_configure_args(options(), "windows")
    assert "-DSECURE_FRONTEND_WINDOW=ON" in make.frontend_configure_args(options("--with", "window"), "windows")
    assert "strata-window.exe" in make.required_files(options("--with", "window"), "windows")
    assert "strata-window" in make.required_files(options("--with", "window", platform="linux"), "linux")
    assert not [f for f in make.required_files(options(), "windows") if "strata-window" in f]
    root = stage(tmp_path, "windows")
    assert "strata-window.exe: missing" in "\n".join(make.layout_problems(root, options("--with", "window"), "windows"))
    windowed = make.readme_text(options("--with", "window"), "windows")
    plain = make.readme_text(options(), "windows")
    assert ".\\strata-window.exe" in windowed and "tray" in windowed and "strata-window" not in plain
    assert "./strata-window" in make.readme_text(options("--with", "window", platform="linux"), "linux")
    assert "Models > Settings" not in windowed and "Settings tab" in windowed
    # built, not only installed (2026-10-28: the first --with window build failed at install: nothing built the window,
    # which needs the server but is not needed by it, as strata-search is)
    assert make.frontend_build_targets(options("--with", "window")) == ["strata-unified-server", "strata", "strata-window"]
    assert make.frontend_build_targets(options()) == ["strata-unified-server", "strata"]


def test_missing_cpp_tools_say_how_to_add_them():
    """Owner 2026-10-28, a second PC: "vcvars64.bat did not set up the MSVC x64 environment (cl was not found after it)" -
    Build Tools 2022 without its C++ workload. The stop says which workload to add and how, by the installer or its
    command line."""
    from pathlib import Path as P
    vcvars = P(r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat")
    text = make.missing_cpp_tools_message(vcvars)
    assert str(vcvars) in text and "Desktop development with C++" in text and "Visual Studio Installer" in text
    assert ("setup.exe\" modify --installPath \"C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\" "
            "--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended") in text


def test_the_compiler_environment_replaces_the_path_whatever_its_case():
    """Owner 2026-10-29, install.bat on a PC with the C++ tools: "vcvars64.bat did not set up the MSVC x64 environment
    (cl was not found after it)". cmd lists the search path under the name its parent gave it - "Path" in a PowerShell
    window - while Python's copy of the environment calls it "PATH"; vcvars64's Path was added beside the old PATH and
    the check for cl read the old one. Each listed name replaces its own, whatever the case."""
    env = make.merged_environment({"PATH": r"C:\Windows", "TEMP": r"C:\Temp"},
                                  "Path=C:\\VS\\VC\\bin\\HostX64\\x64;C:\\Windows\nINCLUDE=C:\\VS\\VC\\include\n=C:=C:\\\nTemp=C:\\T2\n")
    assert [key for key in env if key.upper() == "PATH"] == ["PATH"] and env["PATH"] == r"C:\VS\VC\bin\HostX64\x64;C:\Windows"
    assert env["INCLUDE"] == r"C:\VS\VC\include" and [key for key in env if key.upper() == "TEMP"] == ["TEMP"] and env["TEMP"] == r"C:\T2"
    assert "" not in env, "cmd's hidden =C: drive entries are not variables"


MAYA_PROFILES = ("expert-profile-glm.bin", "expert-profile-ds4.bin", "expert-profile-mm3.bin")
# MiniMax-M3.1-Flash (owner 2026-10-28): Maya's chat template every MiniMax pack carries, with its licence beside it
MINIMAX_FILES = ("engine/templates/minimax-m3.1/chat_template.jinja", "engine/templates/minimax-m3.1/LICENSE.minimax-m31")


def stage(tmp_path: Path, platform: str, extra: tuple[str, ...] = (), skip: tuple[str, ...] = (),
          search: bool = False, email: bool = False) -> Path:
    exe = ".exe" if platform == "windows" else ""
    lib = (lambda n: n + ".dll") if platform == "windows" else (lambda n: "lib" + n + ".so")
    files = [f"strata-unified-server{exe}", f"strata{exe}", f"engine/multiengine{exe}", f"engine/strata-prepare{exe}",
             f"engine/strata-vision{exe}", "engine/profiles/expert-profile.bin", "engine/profiles/expert-profile-coder.bin",
             "engine/data/draft_vocab.bin", "engine/build.json", "README.txt", "config.example.json", "LICENSE", "NOTICE.md",
             "plugins/debugging/plugin.json", "plugins/debugging/plugin.sig"] + ["engine/profiles/" + p for p in MAYA_PROFILES] + \
        list(MINIMAX_FILES)
    if search:   # --with search: the Web search plugin, its manifest and strata-search, which the install puts there
        files += ["plugins/search/plugin.json", "plugins/search/plugin.sig", f"plugins/search/strata-search{exe}"]
    if email:   # --with email: the Email alerts plugin, its manifest and strata-email, which the install puts there
        files += ["plugins/email/plugin.json", "plugins/email/plugin.sig", f"plugins/email/strata-email{exe}"]
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


def test_a_plugins_program_has_the_msvc_runtime_beside_it(tmp_path):
    """strata-search in plugins/search/ loads the MSVC runtime from its own folder, as engine/'s programs do from
    engine/: make takes the runtime there, and nowhere deeper."""
    root = stage(tmp_path, "windows", search=True, extra=("plugins/search/vcruntime140.dll", "plugins/search/msvcp140.dll"))
    assert make.layout_problems(root, options("--with", "search"), "windows") == []
    deeper = stage(tmp_path / "deeper", "windows", search=True, extra=("plugins/search/bin/vcruntime140.dll",))
    assert "plugins/search/bin/vcruntime140.dll: not a runtime file" in make.layout_problems(deeper, options("--with", "search"), "windows")


def test_the_email_plugins_program_has_the_msvc_runtime_beside_it(tmp_path):
    """strata-email in plugins/email/ loads the MSVC runtime from its own folder too (the frontend's install puts it
    there on MSVC, as it does for plugins/search/): make takes the runtime there, and nowhere deeper."""
    runtime = ("plugins/email/vcruntime140.dll", "plugins/email/vcruntime140_1.dll", "plugins/email/msvcp140.dll")
    root = stage(tmp_path, "windows", email=True, extra=runtime)
    assert make.layout_problems(root, options("--with", "email"), "windows") == []
    both = stage(tmp_path / "both", "windows", search=True, email=True,
                 extra=runtime + ("plugins/search/vcruntime140.dll", "plugins/search/msvcp140.dll"))
    assert make.layout_problems(both, options("--with", "email,search"), "windows") == []
    deeper = stage(tmp_path / "deeper", "windows", email=True, extra=("plugins/email/bin/vcruntime140.dll",))
    assert "plugins/email/bin/vcruntime140.dll: not a runtime file" in make.layout_problems(deeper, options("--with", "email"), "windows")


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


def test_the_draft_vocabulary_is_a_runtime_file(tmp_path):
    """Owner, 2026-10-29 (Qwen3.8's MTP draft layer with no Python): the token subset strata-prepare mtp copies into a
    draft layer (upstream's data/draft_vocab.bin) is part of every out folder."""
    root = stage(tmp_path, "linux", skip=("engine/data/draft_vocab.bin",))
    problems = "\n".join(make.layout_problems(root, options(platform="linux"), "linux"))
    assert "engine/data/draft_vocab.bin: missing" in problems, problems


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
    correct"): README.txt says the folder's licence and files, starts the server as each shell types it, shows an
    option on top of data/ (--port alone, since uses_data_folder), and names the generation time limit."""
    windows = make.readme_text(options("--with", "email,search"), "windows")
    linux = make.readme_text(options(platform="linux"), "linux")
    for text in (windows, linux):
        assert "AGPL-3.0" in text and "LICENSE" in text and "NOTICE.md" in text
        assert "limits.engine.generation_timeout_s" in text and "--data-dir data" not in text
        assert "strata-unified-server --help" in text or "strata-unified-server.exe --help" in text
    assert ".\\strata-unified-server.exe --port 9090" in windows and "./strata-unified-server --port 9090" in linux
    assert ".\\strata-unified-server.exe" in windows and "./strata-unified-server" in linux
    assert "engine/templates/" in windows and "strata-search" in windows and "strata-search" not in linux


def test_a_test_hooks_build_is_refused(tmp_path):
    root = stage(tmp_path, "windows")
    (root / "strata-unified-server.exe").write_bytes(b"MZ..." + HOOK_MARKER + b"...")
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
    assert record["ggml"] == ggml and record["cuda_arch"] == "120-real" and record["features"] == []
    engines = {e["library"]: e for e in record["engines"]}
    assert list(engines) == ["multiengine_strata.dll", "multiengine_gemma4.dll", "multiengine_flint.dll", "multiengine_raon1.dll",
                             "multiengine_maya.dll", "multiengine_maya_ds4.dll", "multiengine_maya_mm3.dll", "strata-vision.exe"]
    assert engines["multiengine_maya.dll"]["source"] == "Project Maya " + maya and "GLM-5.3" in engines["multiengine_maya.dll"]["models"]
    assert engines["multiengine_strata.dll"]["source"].startswith("upstream Strata, engine 0.")
    assert "Raon-1-mini" in engines["multiengine_raon1.dll"]["models"] and "Project Maya" in engines["multiengine_raon1.dll"]["source"]
    assert all(e["bytes"] == len(b"binary") for e in record["engines"])
    lean = make.build_record(options("--without", "maya,raon1", "--with", "search", platform="linux"), "linux",
                             stage(tmp_path / "linux", "linux", search=True), commit="x", built="y")
    assert [e["library"] for e in lean["engines"]] == ["libmultiengine_strata.so", "libmultiengine_gemma4.so", "libmultiengine_flint.so",
                                                       "strata-vision"]
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


def test_debugging_is_a_plugin_off_unless_asked_for_and_says_what_it_means(tmp_path, capsys):
    """The owner (2026-10-29): "have an option in the installer/builder to have --debugging on so that it is verbose on
    the logs and can pinpoint issues like what's crashing the calibration ... when set on, it will tell the user the
    implications of having this on and precautions to take"; then (2026-10-30) "we should make the email, websearch,
    debugging all plugins as well". Every folder has the Debugging plugin (this repository's plugins/debugging), recorded
    in engine/build.json as a first-party plugin: --debugging ships it on, and the Plugins tab turns it on or off later,
    with no new build."""
    plain = options()
    asked = options("--debugging")
    assert asked.debugging and not plain.debugging
    shipped = (ROOT / "plugins" / "debugging" / "plugin.json").read_bytes()
    for opts in (plain, asked):
        # no compile-time switch: the server reads the plugin when it starts
        assert not any("SECURE_FRONTEND_DEBUGGING" in arg for arg in make.frontend_configure_args(opts, "windows"))
        assert "plugins/debugging/plugin.json" in make.required_files(opts, "linux")
    folder = tmp_path / "folder"
    make.stage_plugins(plain, folder)
    assert (folder / "plugins" / "debugging" / "plugin.json").read_bytes() == shipped, "the shipped manifest, as it is"
    for opts, on in ((plain, False), (asked, True)):
        record = make.build_record(opts, "windows", folder, commit="c", built="b")
        assert record["plugins"] == [{"id": "debugging", "manifest": hashlib.sha256(shipped).hexdigest(), "files": {}, "on": on}]
        assert "debugging" not in record["features"], "a plugin, not a feature of the build"
    # what it logs, what that can hold, and what to do about it: before the build starts and in the folder's README
    for words in ("one line per request", "the engine's own output", "your prompts", "the paths of your model files",
                  "keep the server's log private", "before you share it", "the Plugins tab", "Settings > Troubleshooting",
                  "only when you accept"):
        assert words in make.DEBUGGING_NOTICE, words
    make.announce(asked)
    assert make.DEBUGGING_NOTICE in capsys.readouterr().out
    make.announce(plain)
    assert "--debugging" not in capsys.readouterr().out
    assert make.DEBUGGING_NOTICE in make.readme_text(asked, "windows")
    assert "--debugging" not in make.readme_text(plain, "linux")
    assert "plugins/debugging: the Debugging plugin" in make.readme_text(plain, "linux")


def test_search_is_a_first_party_plugin_with_its_program_in_its_folder(tmp_path):
    """The owner (2026-10-30): "we should make the email, websearch, debugging all plugins as well" (spec 2026-10-30,
    section 10, "Search, made a tool plugin"). With --with search the folder holds plugins/search/: this repository's
    plugins/search/plugin.json, byte for byte, and strata-search(.exe), which the frontend's install puts there instead
    of beside the server. engine/build.json records it after the Debugging plugin, with the SHA-256 of its manifest and
    of its program as staged, and on: the server runs it only once its required url setting is filled in. README.txt
    names it, and its settings are in the Plugins tab, no longer in the Settings tab. Without --with search there is
    no search plugin."""
    shipped = (ROOT / "plugins" / "search" / "plugin.json").read_bytes()
    debugging = hashlib.sha256((ROOT / "plugins" / "debugging" / "plugin.json").read_bytes()).hexdigest()
    for platform, program in (("windows", "strata-search.exe"), ("linux", "strata-search")):
        searching = options("--with", "search", platform=platform)
        plain = options(platform=platform)
        required = make.required_files(searching, platform)
        assert "plugins/search/plugin.json" in required and f"plugins/search/{program}" in required, required
        assert program not in required, "no longer beside the server"
        assert not [f for f in make.required_files(plain, platform) if f.startswith("plugins/search")]

        folder = stage(tmp_path / platform, platform, search=True)
        (folder / "plugins" / "search" / program).write_bytes(b"the search program")
        make.stage_plugins(searching, folder)
        assert (folder / "plugins" / "search" / "plugin.json").read_bytes() == shipped, "the shipped manifest, as it is"
        assert make.layout_problems(folder, searching, platform) == []
        record = make.build_record(searching, platform, folder, commit="c", built="b")
        assert record["plugins"] == [
            {"id": "debugging", "manifest": debugging, "files": {}, "on": False},
            {"id": "search", "manifest": hashlib.sha256(shipped).hexdigest(),
             "files": {program: hashlib.sha256(b"the search program").hexdigest()}, "on": True}], record["plugins"]
        assert make.build_record(options("--with", "search", "--debugging", platform=platform), platform, folder,
                                 commit="c", built="b")["plugins"][0]["on"] is True

        # the old layout, strata-search beside the server: the plugin is incomplete and the root's copy is refused; the
        # build record is still written, so the layout check can say so
        old = stage(tmp_path / ("old-" + platform), platform, extra=(program,), skip=(f"plugins/search/{program}",),
                    search=True)
        make.stage_plugins(searching, old)
        problems = make.layout_problems(old, searching, platform)
        assert f"plugins/search/{program}: missing" in problems and f"{program}: not a runtime file" in problems, problems
        assert [p["files"] for p in make.build_record(searching, platform, old, commit="c", built="b")["plugins"]] == [{}, {}]

        # without --with search: nothing staged, and a search plugin in the folder is not a runtime file
        bare = stage(tmp_path / ("bare-" + platform), platform)
        make.stage_plugins(plain, bare)
        assert not (bare / "plugins" / "search").exists()
        assert [p["id"] for p in make.build_record(plain, platform, bare, commit="c", built="b")["plugins"]] == ["debugging"]
        stray = make.layout_problems(stage(tmp_path / ("stray-" + platform), platform, search=True), plain, platform)
        assert f"plugins/search/{program}: not a runtime file" in stray, stray

        # README.txt: the plugin among the contents, with its settings in the Plugins tab, not the Settings tab's
        text = make.readme_text(searching, platform)
        entry = text.split("plugins/search: ", 1)[1].split("\n            LICENSE", 1)[0]
        assert entry.startswith("the Web search plugin") and "SearXNG" in entry and "the Plugins tab" in entry, entry
        assert "strata-search: " not in text, "not a program beside the server"
        held = text.split("The Settings tab holds ", 1)[1].split(".", 1)[0]
        assert "web search" not in held and "the model folders" in held, held
        assert "plugins/search" not in make.readme_text(plain, platform)


def test_email_is_a_first_party_plugin_with_its_program_in_its_folder(tmp_path):
    """The owner (2026-10-30): "we should make the email, websearch, debugging all plugins as well" (spec 2026-10-30,
    section 10). With --with email the folder holds plugins/email/: this repository's plugins/email/plugin.json, byte for
    byte, and strata-email(.exe), the notifier the server runs, which the frontend's install puts there. engine/build.json
    records it after the Debugging plugin and the Web search plugin (debugging, search, email), with the SHA-256 of its
    manifest and of its program as staged, and on: the server runs it only once its required settings (the mail server,
    the login, the addresses, the password) are filled in. README.txt names it among the contents, after the search one,
    with its settings in the Plugins tab; the Settings tab no longer holds email alerts, in any build. Without --with
    email there is no email plugin."""
    shipped = (ROOT / "plugins" / "email" / "plugin.json").read_bytes()
    searching = hashlib.sha256((ROOT / "plugins" / "search" / "plugin.json").read_bytes()).hexdigest()
    debugging = hashlib.sha256((ROOT / "plugins" / "debugging" / "plugin.json").read_bytes()).hexdigest()
    for platform, program, finder in (("windows", "strata-email.exe", "strata-search.exe"),
                                      ("linux", "strata-email", "strata-search")):
        mailing = options("--with", "email", platform=platform)
        both = options("--with", "email,search", platform=platform)
        plain = options(platform=platform)
        assert make.first_party_plugins(both) == ["debugging", "search", "email"]
        assert make.first_party_plugins(mailing) == ["debugging", "email"]
        assert make.first_party_plugins(plain) == ["debugging"]
        assert make.plugin_files("email", platform) == [program]
        for opts in (mailing, both):
            required = make.required_files(opts, platform)
            assert "plugins/email/plugin.json" in required and f"plugins/email/{program}" in required, required
            assert program not in required, "not a program beside the server"
        assert not [f for f in make.required_files(plain, platform) if f.startswith("plugins/email")]

        folder = stage(tmp_path / platform, platform, search=True, email=True)
        (folder / "plugins" / "email" / program).write_bytes(b"the email program")
        (folder / "plugins" / "search" / finder).write_bytes(b"the search program")
        make.stage_plugins(both, folder)
        assert (folder / "plugins" / "email" / "plugin.json").read_bytes() == shipped, "the shipped manifest, as it is"
        assert make.layout_problems(folder, both, platform) == []
        record = make.build_record(both, platform, folder, commit="c", built="b")
        assert record["plugins"] == [
            {"id": "debugging", "manifest": debugging, "files": {}, "on": False},
            {"id": "search", "manifest": searching,
             "files": {finder: hashlib.sha256(b"the search program").hexdigest()}, "on": True},
            {"id": "email", "manifest": hashlib.sha256(shipped).hexdigest(),
             "files": {program: hashlib.sha256(b"the email program").hexdigest()}, "on": True}], record["plugins"]
        assert record["features"] == ["email", "search"]
        assert make.build_record(options("--with", "email,search", "--debugging", platform=platform), platform, folder,
                                 commit="c", built="b")["plugins"][0]["on"] is True

        # --with email alone: the email plugin follows the Debugging plugin
        alone = stage(tmp_path / ("alone-" + platform), platform, email=True)
        (alone / "plugins" / "email" / program).write_bytes(b"the email program")
        make.stage_plugins(mailing, alone)
        assert not (alone / "plugins" / "search").exists()
        assert make.layout_problems(alone, mailing, platform) == []
        assert [(p["id"], p["on"]) for p in make.build_record(mailing, platform, alone, commit="c", built="b")["plugins"]] == \
            [("debugging", False), ("email", True)]

        # strata-email beside the server: the plugin is incomplete and the root's copy is refused; the build record is
        # still written, so the layout check can say so
        old = stage(tmp_path / ("old-" + platform), platform, extra=(program,), skip=(f"plugins/email/{program}",),
                    email=True)
        make.stage_plugins(mailing, old)
        problems = make.layout_problems(old, mailing, platform)
        assert f"plugins/email/{program}: missing" in problems and f"{program}: not a runtime file" in problems, problems
        assert [p["files"] for p in make.build_record(mailing, platform, old, commit="c", built="b")["plugins"]] == [{}, {}]

        # without --with email: nothing staged, and an email plugin in the folder is not a runtime file
        bare = stage(tmp_path / ("bare-" + platform), platform)
        make.stage_plugins(plain, bare)
        assert not (bare / "plugins" / "email").exists()
        assert [p["id"] for p in make.build_record(plain, platform, bare, commit="c", built="b")["plugins"]] == ["debugging"]
        stray = make.layout_problems(stage(tmp_path / ("stray-" + platform), platform, email=True), plain, platform)
        assert f"plugins/email/{program}: not a runtime file" in stray, stray
        assert "plugins/email/plugin.json: not a runtime file" in stray, stray

        # README.txt: the plugin among the contents, after the search one, with its settings in the Plugins tab
        text = make.readme_text(both, platform)
        entry = text.split("plugins/email: ", 1)[1].split("\n            LICENSE", 1)[0]
        assert " ".join(entry.split()) == ("the Email alerts plugin, email alerts through your mail server (strata-email; "
                                           "its settings are in the Plugins tab)"), entry
        assert text.index("plugins/debugging: ") < text.index("plugins/search: ") < text.index("plugins/email: ")
        assert "strata-email: " not in text, "not a program beside the server"
        assert "plugins/email: the Email alerts plugin" in make.readme_text(mailing, platform)
        assert "plugins/email" not in make.readme_text(plain, platform)
        # the Settings tab no longer holds email alerts, whichever features the folder has
        for opts in (plain, mailing, both, options("--with", "search", platform=platform)):
            held = make.readme_text(opts, platform).split("The Settings tab holds ", 1)[1].split(".", 1)[0]
            assert held == "the API key, the server's settings and the model folders", held


# ---- trust by key (spec 2026-10-30 section 11; the owner's decision 8) ---------------------------------------------------

def _signing():
    import importlib.util
    spec = importlib.util.spec_from_file_location("plugin_signing", ROOT / "scripts" / "plugin_signing.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules["plugin_signing"] = module
    spec.loader.exec_module(module)
    return module


def test_make_signs_the_plugins_it_builds(tmp_path):
    """The owner (2026-10-30, decision 8): "when we run the install to build everything, it generates a private key"
    and make signs the plugins it builds. make takes this PC's signing key (--signing-key, else the user's own, outside
    every install and data folder), creates it when there is none, writes plugins/<id>/plugin.sig for each first-party
    plugin it stages - the statement of its manifest's and files' SHA-256s, signed - and the key's public half into
    engine/build.json (signing_key), which the server trusts. A folder without a plugin's signature is incomplete."""
    signing = _signing()
    key_path = tmp_path / "keys" / "plugin-signing-key.json"
    for platform, program in (("windows", "strata-search.exe"), ("linux", "strata-search")):
        opts = options("--with", "search,email", "--signing-key", str(key_path), platform=platform)
        assert opts.signing_key == key_path and opts.signing_passphrase is False
        folder = stage(tmp_path / platform, platform, search=True, email=True)
        make.stage_plugins(opts, folder)
        key, _ = make.signing_key(opts, platform)
        assert key_path.is_file() and key.public_hex == signing.load_key(key_path).public_hex
        make.sign_staged_plugins(opts, platform, folder, key)
        for plugin in ("debugging", "search", "email"):
            target = folder / "plugins" / plugin
            assert signing.verify_plugin(target, bytes.fromhex(key.public_hex)), plugin
            assert (target / "plugin.sig").read_bytes().partition(signing.FRAMING)[0] == \
                signing.folder_statement(target, platform, key.public_hex), f"{plugin}: the statement of its folder as staged"
        assert f"\nfile {program} " in (folder / "plugins" / "search" / "plugin.sig").read_text(encoding="utf-8"), "its program's line"
        record = make.build_record(opts, platform, folder, commit="c", built="b", signing_key=key.public_hex)
        assert record["signing_key"] == key.public_hex
        assert "signing_key" not in make.build_record(opts, platform, folder, commit="c", built="b")
        required = make.required_files(opts, platform)
        assert {"plugins/debugging/plugin.sig", "plugins/search/plugin.sig", "plugins/email/plugin.sig"} <= set(required)
        assert make.layout_problems(folder, opts, platform) == []
        (folder / "plugins" / "debugging" / "plugin.sig").unlink()
        assert "plugins/debugging/plugin.sig: missing" in make.layout_problems(folder, opts, platform)
    assert make.parse_args(["--out", "x", "--ggml-dir", "g", "--cuda-root", "c"]).signing_key is None, "default: the user's own"
    assert make.signing_key_path(options(platform="windows"), "windows") == signing.default_key_path("windows")
    assert make.parse_args(["--out", "x", "--ggml-dir", "g", "--cuda-root", "c", "--signing-passphrase"]).signing_passphrase is True


def test_make_py_signs_a_later_plugin_and_shows_the_key(tmp_path, capsys):
    """decision 8: "they can build new plugins later by specifying the private key". make.py --sign-plugin FOLDER signs a
    plugin folder made later with the same key (needing no build options), and --signing-key-show prints its public
    half, for another install's owner to add."""
    signing = _signing()
    key_path = tmp_path / "plugin-signing-key.json"
    platform = make.host_platform()
    folder = tmp_path / "lookup"
    folder.mkdir()
    program = "lookup.exe" if platform == "windows" else "lookup"
    (folder / "plugin.json").write_text(json.dumps({"schema": "m-strata.plugin/1", "id": "lookup", "kind": "tool",
                                                    "files": {"windows": ["lookup.exe"], "linux": ["lookup"]}}), encoding="utf-8")
    (folder / program).write_bytes(b"lookup")
    assert make.main(["--sign-plugin", str(folder), "--signing-key", str(key_path)]) == 0
    key = signing.load_key(key_path)
    assert signing.verify_plugin(folder, bytes.fromhex(key.public_hex))
    capsys.readouterr()
    assert make.main(["--signing-key-show", "--signing-key", str(key_path)]) == 0
    assert capsys.readouterr().out.strip() == key.public_hex
