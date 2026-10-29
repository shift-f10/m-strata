#!/usr/bin/env python3
"""make.py - build every binary and engine into one folder that holds only what runs them (Windows and Linux).

    python make.py --out DIR [--ggml-dir DIR] [--cuda-root DIR] [--cuda-arch auto|compatible|"86;89"]
                   [--without maya,raon1,vision] [--with email,search] [--no-cuda-runtime] [--build-dir DIR] [--engine-build-dir DIR]
                   [--jobs N] [--cmake EXE] [--plan]

make.bat (Windows) and make.sh (Linux) run it. It configures and builds the engine (engine/: the multiengine launcher
and one library per model family), upstream's picture encoder (upstream/tools/vision: engine/strata-vision, built out
of source) and the frontend (secure_frontend/: strata-unified-server and the strata CLI, a release build without test
hooks), installs only their `runtime` CMake components (and `cuda`, the bundled cuBLAS)
into a staging folder, checks that folder holds runtime files and nothing else, writes README.txt and
config.example.json, moves it to --out and makes it owner-only. The first start of strata-unified-server creates its
data/ folder there (docs/superpowers/specs/2026-10-27-gui-first-install-design.md).

Standard library only. It never deletes: --out must not exist yet (or be an empty folder).
"""
from __future__ import annotations

import argparse
import fnmatch
import importlib.util
import json
import os
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path, PureWindowsPath

ROOT = Path(__file__).resolve().parent
OPTIONAL_FAMILIES = ("maya", "raon1")   # the families a build may leave out; strata, gemma4 and flint are always built
# what --without takes: those families, and the picture encoder (strata-vision: a Qwen card reads pictures with it)
LEAVE_OUT = OPTIONAL_FAMILIES + ("vision",)
# built only when asked (off by default): email alerts (SECURE_FRONTEND_EMAIL_ALERTS), web search through a SearXNG
# instance (SECURE_FRONTEND_WEB_SEARCH: strata-search, an MCP server beside the server), the server in a window with a
# tray icon (SECURE_FRONTEND_WINDOW: strata-window; Windows needs the WebView2 SDK in .toolchains/webview2, Linux GTK 3,
# WebKitGTK 4.1 and Ayatana AppIndicator) and the server in a terminal (SECURE_FRONTEND_TUI: strata-tui, the headless
# twin of the window; nothing beyond the C++ standard library and the system's console)
OPTIONAL_FEATURES = ("email", "search", "window", "tui")


@dataclass
class Options:
    out: Path
    ggml_dir: str
    cuda_root: str
    cuda_arch: str
    without: tuple[str, ...]
    with_features: tuple[str, ...]
    cuda_runtime: bool
    build_dir: Path
    engine_build_dir: Path
    jobs: int
    cmake: str
    plan: bool
    debugging: bool = False


def host_platform() -> str:
    return "windows" if os.name == "nt" else "linux"


def _posix(text: str) -> str:
    return text.replace("\\", "/")


# The GPU targets when none are given (owner 2026-10-29: "it should detect what card I have", then "to make it
# lightweight ... detect the card and target that specifically, and if there is multiple cards, only those card types",
# and "if they have unsupported cards, then we compile to only supported ones and if none is present we say no supported
# card detected"): each supported card type nvidia-smi reports (compute capability 7.5 or newer, the engines' own rule),
# once, as a number with its machine code only - CMake's "native" had left the NVFP4 W4A4 unit at a plain sm_120 on an
# RTX 5090, which ptxas refuses. --cuda-arch compatible is every generation the engines build for - Turing, Ampere (A100,
# RTX 30), Ada, Hopper, Blackwell (RTX 50), all built here 2026-10-29 - with PTX for the newest, which later GPUs compile
# at load.
OLDEST_SUPPORTED_GPU = 75
COMPATIBLE_GPUS = (75, 80, 86, 89, 90, 120)
NO_SUPPORTED_GPU = ("make: no supported NVIDIA card detected (the engines need compute capability 7.5 or newer: the RTX 20 "
                    "series and later). To build anyway, name the targets: --cuda-arch compatible (every generation) or "
                    "a list such as --cuda-arch \"86;89\"")


def compute_capabilities_from(text: str) -> list[str]:
    """nvidia-smi --query-gpu=compute_cap's lines ("12.0", one per GPU); anything else (N/A, [Not Supported]) is skipped."""
    found = []
    for line in text.splitlines():
        major, dot, minor = line.strip().partition(".")
        if dot and major.isdigit() and minor.isdigit():
            found.append(line.strip())
    return found


def gpu_compute_capabilities() -> list[str]:
    smi = shutil.which("nvidia-smi")
    if not smi:
        return []
    try:
        done = subprocess.run([smi, "--query-gpu=compute_cap", "--format=csv,noheader"], capture_output=True, text=True,
                              timeout=60, check=False)
    except (OSError, subprocess.SubprocessError):
        return []
    return compute_capabilities_from(done.stdout) if done.returncode == 0 else []


def cuda_targets(given: str) -> str:
    """--cuda-arch: "auto" is this PC's supported card types (none: make stops), "compatible" every generation the
    engines build for; anything else is passed to CMake as it is."""
    if given == "compatible":
        return ";".join([f"{target}-real" for target in COMPATIBLE_GPUS[:-1]] + [str(COMPATIBLE_GPUS[-1])])
    if given != "auto":
        return given
    found = {int(major) * 10 + int(minor) for major, _, minor in (cap.partition(".") for cap in gpu_compute_capabilities())}
    supported = sorted(target for target in found if target >= OLDEST_SUPPORTED_GPU)
    for target in sorted(found - set(supported)):
        print(f"make: leaving out a card of compute capability {target // 10}.{target % 10} (older than the engines support)",
              flush=True)
    if not supported:
        raise SystemExit(NO_SUPPORTED_GPU)
    return ";".join(f"{target}-real" for target in supported)


def run_cuda_build(command: list[str], jobs: int, env: dict[str, str], plan: bool) -> None:
    """A CUDA build at `jobs`, and once more at a quarter of them when it stops: under many parallel jobs CUDA's compiler
    can die (here 2026-10-29: ptxas 0xC0000005 and cicc 0xC0000374 on two of llama.cpp's MMQ units, both built at 4)."""
    try:
        run([*command, "-j", str(jobs)], env, plan)
    except subprocess.CalledProcessError:
        if jobs <= 1:
            raise
        fewer = max(1, jobs // 4)
        print(f"make: the build stopped; trying once more with {fewer} jobs (what is built is kept)", flush=True)
        run([*command, "-j", str(fewer)], env, plan)


def parse_args(argv: list[str], platform: str | None = None) -> Options:
    platform = platform or host_platform()
    ap = argparse.ArgumentParser(prog="make", description="Build every binary and engine into one runtime-only folder.")
    ap.add_argument("--out", required=True, type=Path, help="the folder to create (must not exist, or be empty)")
    ap.add_argument("--ggml-dir", default=os.environ.get("STRATA_GGML_DIR", ""),
                    help="the pinned llama.cpp checkout the engine builds ggml from (env STRATA_GGML_DIR)")
    ap.add_argument("--cuda-root", default=os.environ.get("CUDA_PATH", os.environ.get("CUDA_HOME", "")),
                    help="the CUDA toolkit (env CUDA_PATH / CUDA_HOME)")
    ap.add_argument("--cuda-arch", default="auto",
                    help='GPU targets: "auto" (each supported card type this PC has), "compatible" (every generation the '
                         'engines build for: Turing to Blackwell), or a list such as "86;89;120"')
    ap.add_argument("--without", default="", help="families (or the picture encoder) to leave out: " + ", ".join(LEAVE_OUT))
    ap.add_argument("--with", dest="with_features", default="",
                    help="optional features to build in (off otherwise): " + ", ".join(OPTIONAL_FEATURES))
    ap.add_argument("--no-cuda-runtime", action="store_true", help="do not bundle cuBLAS (the PC needs a CUDA toolkit)")
    ap.add_argument("--build-dir", type=Path, default=ROOT / "out" / "make-build", help="where the builds are kept")
    ap.add_argument("--engine-build-dir", type=Path, default=None, help="an existing engine build to reuse")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--cmake", default=os.environ.get("CMAKE", ""), help="the cmake executable (default: on PATH)")
    ap.add_argument("--plan", action="store_true", help="print the commands and stop")
    ap.add_argument("--debugging", action="store_true",
                    help="a server that logs verbosely, to find out why a model does not start or calibrate "
                         "(it says what that logs and what to take care of)")
    args = ap.parse_args(argv)
    without = tuple(sorted({w.strip() for w in args.without.split(",") if w.strip()}))
    unknown = [w for w in without if w not in LEAVE_OUT]
    if unknown:
        raise SystemExit("make: --without takes only %s (strata, gemma4 and flint are part of every engine), not %s"
                         % (", ".join(LEAVE_OUT), ", ".join(unknown)))
    with_features = tuple(sorted({w.strip() for w in args.with_features.split(",") if w.strip()}))
    unknown = [w for w in with_features if w not in OPTIONAL_FEATURES]
    if unknown:
        raise SystemExit("make: --with takes only %s, not %s" % (", ".join(OPTIONAL_FEATURES), ", ".join(unknown)))
    if not args.ggml_dir:
        raise SystemExit("make: --ggml-dir (or STRATA_GGML_DIR) names the llama.cpp checkout the engine builds ggml from")
    if not args.cuda_root:
        raise SystemExit("make: --cuda-root (or CUDA_PATH) names the CUDA toolkit")
    build_dir = args.build_dir.absolute()
    return Options(out=args.out.absolute(), ggml_dir=_posix(args.ggml_dir), cuda_root=_posix(args.cuda_root),
                   cuda_arch=cuda_targets(args.cuda_arch), without=without, with_features=with_features,
                   cuda_runtime=not args.no_cuda_runtime,
                   build_dir=build_dir,
                   engine_build_dir=(args.engine_build_dir or build_dir / "engine").absolute(), jobs=max(1, args.jobs),
                   cmake=args.cmake, plan=args.plan, debugging=args.debugging)


def _on_off(flag: bool) -> str:
    return "ON" if flag else "OFF"


def engine_configure_args(opts: Options, platform: str) -> list[str]:
    """The engine's configure line: the configuration the box builds and tests (engine/build-g), Release, no tests.
    A folder that already holds a build keeps its compiler: CMake deletes the cache when CMAKE_CUDA_COMPILER is
    spelled differently from the cached value and reconfigures without the other options."""
    nvcc = opts.cuda_root + ("/bin/nvcc.exe" if platform == "windows" else "/bin/nvcc")
    compiler = [] if (opts.engine_build_dir / "CMakeCache.txt").is_file() else ["-DCMAKE_CUDA_COMPILER=" + nvcc]
    return ["-S", str(ROOT / "engine"), "-B", str(opts.engine_build_dir), "-G", "Ninja",
            "-DCMAKE_BUILD_TYPE=Release", "-DSTRATA_ENABLE_CUDA=ON", "-DSTRATA_BUILD_TESTS=OFF",
            "-DSTRATA_NATIVE_EXPERTS=ON", "-DSTRATA_Q6K_EXPERTS=ON", "-DSTRATA_MMQ_KQUANTS=ON",
            "-DSTRATA_BUILD_MAYA=" + _on_off("maya" not in opts.without),
            "-DSTRATA_BUILD_RAON1=" + _on_off("raon1" not in opts.without),
            "-DSTRATA_GLM_WERROR=OFF",   # the vendored Maya/raon1 trees are built as their authors build them
            "-DMULTIENGINE_BUNDLE_CUBLAS=" + _on_off(opts.cuda_runtime),
            "-DSTRATA_GGML_DIR=" + opts.ggml_dir, "-DCUDAToolkit_ROOT=" + opts.cuda_root,
            *compiler, "-DCMAKE_CUDA_ARCHITECTURES=" + opts.cuda_arch]


def vision_configure_args(opts: Options, platform: str) -> list[str]:
    """upstream's picture encoder (upstream/tools/vision, read in place and built out of source: upstream/ is never
    changed) over llama.cpp's mtmd from the engine's pinned checkout, with ggml-cuda for the engine's GPU targets;
    portable (AVX2, the static MSVC runtime) as upstream ships it. A reused folder keeps its compiler, as the engine's."""
    build = opts.build_dir / "vision"
    nvcc = opts.cuda_root + ("/bin/nvcc.exe" if platform == "windows" else "/bin/nvcc")
    compiler = [] if (build / "CMakeCache.txt").is_file() else ["-DCMAKE_CUDA_COMPILER=" + nvcc]
    return ["-S", str(ROOT / "upstream" / "tools" / "vision"), "-B", str(build), "-G", "Ninja",
            "-DCMAKE_BUILD_TYPE=Release", "-DLLAMA_DIR=" + opts.ggml_dir, "-DSTRATA_VISION_CUDA=ON", "-DSTRATA_PORTABLE=ON",
            "-DCUDAToolkit_ROOT=" + opts.cuda_root, *compiler, "-DCMAKE_CUDA_ARCHITECTURES=" + opts.cuda_arch]


def frontend_configure_args(opts: Options, platform: str) -> list[str]:
    """The frontend's configure line: a release build with no tests and no test hooks (the shipped configuration)."""
    del platform
    return ["-S", str(ROOT / "secure_frontend"), "-B", str(opts.build_dir / "frontend"), "-G", "Ninja",
            "-DCMAKE_BUILD_TYPE=Release", "-DBUILD_TESTING=OFF", "-DSTRATA_BUILD_SECURE_FRONTEND_TESTS=OFF",
            "-DSECURE_FRONTEND_TEST_HOOKS=OFF", "-DSECURE_FRONTEND_WERROR=ON",
            "-DSECURE_FRONTEND_EMAIL_ALERTS=" + _on_off("email" in opts.with_features),
            "-DSECURE_FRONTEND_WEB_SEARCH=" + _on_off("search" in opts.with_features),
            "-DSECURE_FRONTEND_WINDOW=" + _on_off("window" in opts.with_features),
            "-DSECURE_FRONTEND_TUI=" + _on_off("tui" in opts.with_features),
            "-DSECURE_FRONTEND_DEBUGGING=" + _on_off(opts.debugging)]


def frontend_build_targets(opts: Options) -> list[str]:
    """The frontend's programs to build: the server (strata-search comes with it) and strata, and strata-window and
    strata-tui, which need the server but are not needed by it."""
    return (["strata-unified-server", "strata"] + (["strata-window"] if "window" in opts.with_features else []) +
            (["strata-tui"] if "tui" in opts.with_features else []))


# ---- the out folder's layout ----------------------------------------------------------------------------------------

def _families(opts: Options) -> list[str]:
    names = ["multiengine_strata", "multiengine_gemma4", "multiengine_flint"]
    if "raon1" not in opts.without:
        names.append("multiengine_raon1")
    if "maya" not in opts.without:
        names += ["multiengine_maya", "multiengine_maya_ds4", "multiengine_maya_mm3"]
    return names + ["strata_kq"]


def required_files(opts: Options, platform: str) -> list[str]:
    exe = ".exe" if platform == "windows" else ""
    lib = (lambda n: n + ".dll") if platform == "windows" else (lambda n: "lib" + n + ".so")
    # strata-prepare (engine/src/prepare): a download's pack with no Python, and the expert profiles its cards name;
    # with the Maya families, the profiles Maya's setup starts them from (the server passes the GLM one to each launch),
    # and MiniMax-M3.1's chat template every MiniMax pack carries, with the MiniMax Community License that covers it
    maya = ([] if "maya" in opts.without else
            [f"engine/profiles/expert-profile-{f}.bin" for f in ("glm", "ds4", "mm3")] +
            ["engine/templates/minimax-m3.1/chat_template.jinja", "engine/templates/minimax-m3.1/LICENSE.minimax-m31"])
    # web search: strata-search, the MCP server the server runs beside itself for it
    search = [f"strata-search{exe}"] if "search" in opts.with_features else []
    # the window: strata-window, which runs the server beside it
    window = [f"strata-window{exe}"] if "window" in opts.with_features else []
    # the terminal: strata-tui, which runs the server beside it as the window does
    window += [f"strata-tui{exe}"] if "tui" in opts.with_features else []
    # the picture encoder beside the launcher, where the server looks for it (spec 2026-10-28)
    vision = [] if "vision" in opts.without else [f"engine/strata-vision{exe}"]
    # engine/build.json: the build record the server's About shows (build_record)
    return ([f"strata-unified-server{exe}", f"strata{exe}", f"engine/multiengine{exe}", f"engine/strata-prepare{exe}",
             "engine/profiles/expert-profile.bin", "engine/profiles/expert-profile-coder.bin", "engine/data/draft_vocab.bin",
             "engine/build.json",
             "README.txt", "config.example.json", "LICENSE", "NOTICE.md"] + search + window + vision + maya +
            ["engine/" + lib(n) for n in _families(opts)])


# ---- the build record (engine/build.json; owner 2026-10-27: the About lists the build of our engines) -------------

def _tree_text(relative: str) -> str:
    try:
        return (ROOT / relative).read_text(encoding="utf-8")
    except OSError:
        return ""


def _cmake_value(relative: str, pattern: str) -> str:
    import re
    found = re.search(pattern, _tree_text(relative))
    return found.group(1) if found else "?"


def build_record(opts: Options, platform: str, stage: Path, commit: str | None = None, built: str | None = None) -> dict:
    """What engine/build.json holds: when and from which commit this folder was built, the pinned ggml, the GPU targets
    and features, and each family library in it - the models it runs and the source it is built from, as the tree says
    at build time (engine/CMakeLists.txt's engine version, cmake/multiengine.cmake's family engine version,
    third_party/maya/VERSION)."""
    if commit is None:
        done = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "--short=12", "HEAD"], capture_output=True, text=True)
        commit = done.stdout.strip() if done.returncode == 0 else ""
    if built is None:
        built = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    engine = _cmake_value("engine/CMakeLists.txt", r"project\(strata VERSION ([0-9.]+)")
    family = _cmake_value("engine/cmake/multiengine.cmake", r'MULTIENGINE_FAMILY_ENGINE_VERSION "([0-9.]+)"')
    maya = _tree_text("engine/third_party/maya/VERSION").strip() or "?"
    sources = {
        "multiengine_strata": ("Qwen3.8-Flash family, Qwen3.6", f"upstream Strata, engine {engine}"),
        "multiengine_gemma4": ("Gemma 4", f"the Strata core, engine {family}"),
        "multiengine_flint": ("flint", f"the Strata core, engine {family}"),
        "multiengine_raon1": ("Raon-1-mini", f"our port of Project Maya v1.1.27's GLM engine, engine {family}"),
        "multiengine_maya": ("GLM-5.3-Flash", f"Project Maya {maya}"),
        "multiengine_maya_ds4": ("DeepSeek-V4.1-Flash", f"Project Maya {maya}"),
        "multiengine_maya_mm3": ("MiniMax-M3.1-Flash", f"Project Maya {maya}"),
    }
    lib = (lambda n: n + ".dll") if platform == "windows" else (lambda n: "lib" + n + ".so")
    engines = []
    for name in _families(opts):
        if name not in sources:
            continue   # strata_kq: a helper of the strata family, not an engine
        path = stage / "engine" / lib(name)
        models, source = sources[name]
        engines.append({"library": lib(name), "models": models, "source": source,
                        "bytes": path.stat().st_size if path.is_file() else 0})
    if "vision" not in opts.without:   # the picture encoder, a program beside them
        program = "strata-vision" + (".exe" if platform == "windows" else "")
        path = stage / "engine" / program
        engines.append({"library": program, "models": "pictures for the Qwen3.8-Flash family and Qwen3.6",
                        "source": "upstream Strata tools/vision, llama.cpp's mtmd at the pinned ggml",
                        "bytes": path.stat().st_size if path.is_file() else 0})
    return {"schema_version": 1, "built": built, "commit": commit,
            "ggml": (_tree_text("engine/third_party/ggml/VERSION.txt").split() or ["?"])[0][:12],
            "cuda_arch": opts.cuda_arch, "features": list(opts.with_features) + (["debugging"] if opts.debugging else []),
            "engines": engines}


def _cuda_runtime_patterns(platform: str) -> list[str]:
    return (["engine/cublas64_*.dll", "engine/cublasLt64_*.dll"] if platform == "windows"
            else ["engine/libcublas.so.*", "engine/libcublasLt.so.*"])


# The MSVC runtime CMake's InstallRequiredSystemLibraries puts beside each program (Microsoft's app-local deployment).
_MSVC_RUNTIME = ["msvcp140*.dll", "vcruntime140*.dll", "concrt140.dll", "vccorlib140.dll"]


def _distribution_check():
    spec = importlib.util.spec_from_file_location("secure_distribution_check",
                                                  ROOT / "scripts" / "secure_distribution_check.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def layout_problems(root: Path, opts: Options, platform: str) -> list[str]:
    """Every way `root` is not exactly a runtime folder: a missing file, a file that is not a runtime file (a build
    leftover, a source, a test program, a stale library), and the secure distribution check's own refusals."""
    problems: list[str] = []
    required = required_files(opts, platform)
    present = sorted(p.relative_to(root).as_posix() for p in root.rglob("*") if p.is_file())
    for name in required:
        if name not in present:
            problems.append(f"{name}: missing")
    cuda = _cuda_runtime_patterns(platform)
    if opts.cuda_runtime:
        for pattern in cuda:
            if not any(fnmatch.fnmatch(name, pattern) for name in present):
                problems.append(f"{pattern}: missing (the bundled cuBLAS; --no-cuda-runtime leaves it out)")
    allowed = set(required)
    for name in present:
        if name in allowed:
            continue
        if opts.cuda_runtime and any(fnmatch.fnmatch(name, pattern) for pattern in cuda):
            continue
        base = name.rsplit("/", 1)[-1].lower()
        if platform == "windows" and name.count("/") <= 1 and any(fnmatch.fnmatch(base, p) for p in _MSVC_RUNTIME):
            continue
        problems.append(f"{name}: not a runtime file")
    problems += _distribution_check().inspect(root, False)
    return problems


def example_config_text(platform: str) -> str:
    """config.example.json: the shape of data/config.json, which the first start writes (a hand-written config is
    still accepted with --config)."""
    base = "C:\\SecureStrata" if platform == "windows" else "/opt/secure-strata"
    sep = "\\" if platform == "windows" else "/"
    data = base + sep + "data"
    exe = ".exe" if platform == "windows" else ""
    config = {
        "schema_version": 1,
        "listener": {"host": "127.0.0.1", "port": 8080, "web_ui": True, "monitor": True},
        "token": {"source": "file:token"},
        "roots": {"secret": data + sep + "secret", "state": data + sep + "state", "logs": data + sep + "logs",
                  "cache": data + sep + "cache"},
        "engine": {"executable": base + sep + "engine" + sep + "multiengine" + exe},
        "models": {"control": True, "downloads": True, "downloads_root": data + sep + "downloads"},
    }
    return json.dumps(config, indent=2) + "\n"


README_TEXT = """M-Strata (Multiengine Strata) - runtime folder
==============================================

Start:      {server}{shell}
            The first start creates data/ here (the config, the API key, model cards and state) and prints a
            one-time login link (http://127.0.0.1:8080/#login=...). Open it in a browser on this PC: the page shows
            your API key once and signs you in. Every later start prints a fresh link.{window}
Options:    this folder's data/, on port 9090 for this start:
                {server} --port 9090
            a data folder elsewhere (made at its first start):
                {server} --data-dir DIR
            from a config file (config.example.json shows its shape):
                {server} --config data/config.json
            check data/config.json and print the settings in effect:
                {server} --check
            every option:
                {server} --help
            Options apply to that start on top of data/config.json; --data-dir and --config exclude each other.
Models:     the Models tab adds a model from a folder, downloads one into a folder you choose, starts, stops and
            calibrates it.
            The Settings tab holds {settings}.
Config:     data/config.json (to keep a port, set listener.port there). An answer cut after about two minutes:
            raise limits.engine.generation_timeout_s (120 s by default), then restart the server.
Key:        {strata} --config data/config.json key show   (or key rotate)

Contents:   strata-unified-server and strata, the frontend{search}{window_contents}
            engine/: multiengine (the launcher), one library per model family, strata-prepare{vision}, the expert
            profiles{templates}{cuda}
            LICENSE and NOTICE.md
            Nothing else is needed; move or copy the whole folder.

License:    AGPL-3.0-only (LICENSE). NOTICE.md holds the copyright, the MIT notice of the Strata code this builds on{minimax}.
{nvidia}"""

# make --debugging (owner 2026-10-29: "when set on, it will tell the user the implications of having this on and
# precautions to take"): printed before the build starts and after it, and kept in the folder's README.txt.
DEBUGGING_NOTICE = """Debugging:  this is a debugging build (make --debugging): the server logs verbosely, to find
            out why a model does not start or calibrate. It logs one line per request (route, status, time; never
            its text) and the engine's own output, which can hold what the engine prints of your prompts and
            the paths of your model files. Take care: run it on your own PC, keep the server's log private (its
            console, the window app's Server log, logs/), look through a log before you share it and take out
            prompts, paths and names, and build again without --debugging for everyday use. The engine's
            output can be turned off without a new build: Settings > Troubleshooting (logging.engine_output;
            logging.requests for the request lines)."""


def announce(opts: Options) -> None:
    """What make says before it builds: the GPU targets, and what a debugging build means."""
    print(f"make: GPU targets {opts.cuda_arch}", flush=True)
    if opts.debugging:
        print(DEBUGGING_NOTICE, flush=True)


NVIDIA_TEXT = """NVIDIA:     engine/{cublas}* are NVIDIA cuBLAS libraries, redistributed unmodified under the NVIDIA CUDA Toolkit
            End User License Agreement (Attachment A lists them as redistributable); they remain NVIDIA's.
"""


def readme_text(opts: Options, platform: str) -> str:
    windows = platform == "windows"
    server = ".\\strata-unified-server.exe" if windows else "./strata-unified-server"
    shell = "   (PowerShell; in cmd: strata-unified-server)" if windows else ""
    cuda = ",\n            NVIDIA's cuBLAS" if opts.cuda_runtime else ""
    search = ("\n            strata-search: web search through your SearXNG instance (the Settings tab)"
              if "search" in opts.with_features else "")
    held = ["the API key", "the server's settings", "the model folders"] + \
           (["email alerts"] if "email" in opts.with_features else []) + (["web search"] if "search" in opts.with_features else [])
    settings = ", ".join(held[:-1]) + " and " + held[-1]
    # strata-window: the same server in a window, with the page inside and an icon in the tray
    window_exe = ".\\strata-window.exe" if windows else "./strata-window"
    window = ("\nWindow:     " + window_exe + "   (takes the same options)\n"
              "            The same server in a window with the page inside, signed in for you, and an icon in the tray.\n"
              "            Closing the window keeps the server running; the tray icon's menu opens it, starts and stops\n"
              "            models, restarts the server and exits." if "window" in opts.with_features else "")
    tui_exe = ".\\strata-tui.exe" if windows else "./strata-tui"
    window += ("\nTerminal:   " + tui_exe + "   (takes the same options)\n"
               "            The same server in this terminal (over SSH too): a dashboard of its GPU, CPU, memory and speed,\n"
               "            the models (start, stop, options, calibrate), downloads, settings and the log; q quits and stops\n"
               "            the server." if "tui" in opts.with_features else "")
    window_contents = ("\n            strata-window: the server in a window with a tray icon" if "window" in opts.with_features else "")
    window_contents += ("\n            strata-tui: the server in a terminal, with a dashboard" if "tui" in opts.with_features else "")
    maya = "maya" not in opts.without
    templates = ", templates/minimax-m3.1/ (MiniMax's chat template and licence)" if maya else ""
    minimax = (",\n            and the permission to combine it with MiniMax-M3.1-Flash's template and model, which stay under"
               "\n            the MiniMax Community License (engine/templates/minimax-m3.1/)" if maya else "")
    nvidia = NVIDIA_TEXT.format(cublas="cublas" if windows else "libcublas") if opts.cuda_runtime else ""
    vision = "" if "vision" in opts.without else ",\n            strata-vision (a Qwen model's picture encoder)"
    return README_TEXT.format(server=server, shell=shell, strata=".\\strata.exe" if windows else "./strata", cuda=cuda,
                              search=search, settings=settings, templates=templates, minimax=minimax, nvidia=nvidia,
                              vision=vision, window=window, window_contents=window_contents) + \
           ("\n" + DEBUGGING_NOTICE + "\n" if opts.debugging else "")


def owner_only_commands(path: Path, platform: str, user: str) -> list[list[str]]:
    """Make `path` and everything below it owner-only (the server refuses an engine or a root other users may write):
    Windows, a protected DACL for the user that children inherit; POSIX, no group or other write."""
    if platform == "windows":
        return [["icacls", str(path), "/inheritance:r", "/grant:r", f"{user}:(OI)(CI)F"],
                ["icacls", str(path / "*"), "/reset", "/T"]]
    return [["chmod", "-R", "go-w", path.as_posix()]]


# ---- running ---------------------------------------------------------------------------------------------------------

def _find_vcvars() -> Path | None:
    roots = [os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"), os.environ.get("ProgramFiles", r"C:\Program Files")]
    vswhere = Path(roots[0]) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    if vswhere.is_file():
        found = subprocess.run([str(vswhere), "-latest", "-products", "*", "-requires",
                                "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"],
                               capture_output=True, text=True, check=False).stdout.strip().splitlines()
        for line in found:
            candidate = Path(line) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
            if candidate.is_file():
                return candidate
    for root in roots:
        for candidate in sorted(Path(root).glob("Microsoft Visual Studio/*/*/VC/Auxiliary/Build/vcvars64.bat")):
            return candidate
    return None


def missing_cpp_tools_message(vcvars: Path) -> str:
    """What to do when vcvars64 sets up no compiler: Visual Studio (or its Build Tools) is installed without its C++
    workload."""
    vcvars = PureWindowsPath(vcvars)  # a Windows install's path, whichever system formats the message (the tests: Linux too)
    install = vcvars.parents[3]  # <install>\VC\Auxiliary\Build\vcvars64.bat
    installer = (PureWindowsPath(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft Visual Studio" /
                 "Installer")
    return (f"make: {vcvars} did not set up the MSVC x64 environment (cl was not found after it).\n"
            f"      Visual Studio at {install} is installed without its C++ tools. Open Visual Studio Installer, choose\n"
            f"      Modify on it, tick \"Desktop development with C++\" (the MSVC v143 x64 compiler and a Windows SDK) and\n"
            f"      Modify; or, from an administrator prompt:\n"
            f"        \"{installer / 'setup.exe'}\" modify --installPath \"{install}\" "
            f"--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended --passive\n"
            f"      Then run make again (install the CUDA Toolkit after it, so it adds its Visual Studio integration).")


def merged_environment(env: dict[str, str], listed: str) -> dict[str, str]:
    """env with the variables cmd's `set` listed (vcvars64's environment). Windows names ignore case: cmd lists a name as
    its parent wrote it ("Path" in a PowerShell window) while os.environ upper-cases it, so every name is kept upper-case
    and a listed one replaces its own (owner 2026-10-29: vcvars64's Path sat beside the old PATH, and cl was looked for
    on the old one). cmd's hidden "=C:" drive entries have no name and are skipped."""
    merged = {key.upper(): value for key, value in env.items()}
    for line in listed.splitlines():
        key, eq, value = line.partition("=")
        if eq and key:
            merged[key.upper()] = value
    return merged


def build_environment(platform: str) -> dict[str, str]:
    """The environment the builds run in: on Windows the MSVC x64 developer environment (vcvars64), unless cl is on
    PATH already."""
    env = dict(os.environ)
    if platform != "windows" or shutil.which("cl", path=env.get("PATH")):
        return env
    vcvars = _find_vcvars()
    if vcvars is None:
        raise SystemExit("make: Visual Studio's C++ build tools (vcvars64.bat) were not found; install them or run from "
                         "an x64 Native Tools prompt")
    # One command line, passed as is (cmd does not read Python's \" quoting). vcvars64 can end with errorlevel 1 when it
    # does not find vswhere on PATH yet still sets everything up, so the result is judged by the environment it leaves.
    listed = subprocess.run(f'cmd /d /s /c "call "{vcvars}" >nul 2>nul & set"', capture_output=True, text=True,
                            check=False).stdout
    env = merged_environment(env, listed)
    if not shutil.which("cl", path=env.get("PATH")):
        raise SystemExit(missing_cpp_tools_message(vcvars))
    return env


def find_tool(name: str, given: str, env: dict[str, str]) -> str:
    if given:
        return given
    found = shutil.which(name, path=env.get("PATH"))
    if found:
        return found
    beside = Path(sys.executable).parent / (name + (".exe" if os.name == "nt" else ""))
    if beside.is_file():
        return str(beside)
    raise SystemExit(f"make: {name} was not found (put it on PATH" + (", or pass --cmake)" if name == "cmake" else ")"))


def make_program_args(build_dir: Path, ninja: str) -> list[str]:
    """CMAKE_MAKE_PROGRAM for a new build folder only: a reused one keeps the ninja in its cache (another ninja version
    reads the dependency log as stale and rebuilds everything)."""
    if (build_dir / "CMakeCache.txt").is_file() or not Path(ninja).is_file():
        return []
    return ["-DCMAKE_MAKE_PROGRAM=" + ninja]


def run(command: list[str], env: dict[str, str], plan: bool) -> None:
    print("+ " + " ".join(f'"{c}"' if " " in c else c for c in command), flush=True)
    if not plan:
        subprocess.run(command, env=env, check=True)


def main(argv: list[str] | None = None) -> int:
    platform = host_platform()
    opts = parse_args(sys.argv[1:] if argv is None else argv, platform)
    if opts.out.exists() and (not opts.out.is_dir() or any(opts.out.iterdir())):
        raise SystemExit(f"make: {opts.out} already exists and is not empty; choose a new folder (make never deletes)")
    env = build_environment(platform) if not opts.plan else dict(os.environ)
    cmake = find_tool("cmake", opts.cmake, env)
    ninja = shutil.which("ninja", path=env.get("PATH")) or str(Path(cmake).parent / ("ninja.exe" if os.name == "nt" else "ninja"))
    stage = opts.build_dir / ("stage-" + time.strftime("%Y%m%d-%H%M%S"))
    jobs = ["-j", str(opts.jobs)]
    announce(opts)

    run([cmake, *engine_configure_args(opts, platform), *make_program_args(opts.engine_build_dir, ninja)], env,
        opts.plan)
    run_cuda_build([cmake, "--build", str(opts.engine_build_dir), "--target", "multiengine", "strata_kq"], opts.jobs, env,
                   opts.plan)
    run([cmake, "--install", str(opts.engine_build_dir), "--prefix", str(stage), "--component", "runtime"], env, opts.plan)
    if opts.cuda_runtime:
        run([cmake, "--install", str(opts.engine_build_dir), "--prefix", str(stage), "--component", "cuda"], env,
            opts.plan)
    if "vision" not in opts.without:
        # upstream's picture encoder, into engine/ beside the launcher (it has no install rules: its one program is copied)
        run([cmake, *vision_configure_args(opts, platform), *make_program_args(opts.build_dir / "vision", ninja)], env,
            opts.plan)
        run_cuda_build([cmake, "--build", str(opts.build_dir / "vision"), "--target", "strata-vision"], opts.jobs, env,
                       opts.plan)
        program = "strata-vision" + (".exe" if platform == "windows" else "")
        print(f"+ copy {opts.build_dir / 'vision' / 'bin' / program} {stage / 'engine' / program}", flush=True)
        if not opts.plan:
            shutil.copy2(opts.build_dir / "vision" / "bin" / program, stage / "engine" / program)
    run([cmake, *frontend_configure_args(opts, platform), *make_program_args(opts.build_dir / "frontend", ninja)],
        env, opts.plan)
    run([cmake, "--build", str(opts.build_dir / "frontend"), "--target", *frontend_build_targets(opts), *jobs], env,
        opts.plan)
    run([cmake, "--install", str(opts.build_dir / "frontend"), "--prefix", str(stage), "--component", "runtime"], env,
        opts.plan)
    if opts.plan:
        return 0

    (stage / "README.txt").write_text(readme_text(opts, platform), encoding="utf-8", newline="\n")
    (stage / "config.example.json").write_text(example_config_text(platform), encoding="utf-8", newline="\n")
    # the program's license (AGPL-3.0, owner 2026-10-28) and its notices, as the repository's root holds them
    for name in ("LICENSE", "NOTICE.md"):
        shutil.copyfile(ROOT / name, stage / name)
    (stage / "engine" / "build.json").write_text(json.dumps(build_record(opts, platform, stage), indent=2) + "\n",
                                                 encoding="utf-8", newline="\n")
    problems = layout_problems(stage, opts, platform)
    if problems:
        print("make: the staged folder is not a clean runtime folder (" + str(stage) + "):", file=sys.stderr)
        for problem in problems:
            print("  - " + problem, file=sys.stderr)
        return 1
    opts.out.parent.mkdir(parents=True, exist_ok=True)
    if opts.out.exists():
        opts.out.rmdir()   # the empty folder the operator named (checked above)
    shutil.move(str(stage), str(opts.out))
    user = os.environ.get("USERNAME", "") if platform == "windows" else os.environ.get("USER", "")
    for command in owner_only_commands(opts.out, platform, user):
        run(command, env, False)
    total = sum(p.stat().st_size for p in opts.out.rglob("*") if p.is_file())
    print(f"make: {opts.out} ({total / 2**20:.0f} MiB): run strata-unified-server there")
    if opts.debugging:
        print(DEBUGGING_NOTICE)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
