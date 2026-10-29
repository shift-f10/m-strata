#!/bin/sh
# install.sh - from a fresh Ubuntu 24.04 (or newer) / Debian PC to a built M-Strata in one step (install.bat's twin).
#
#   ./install.sh                                    build into ../M-Strata-app: email alerts, web search, the window and the
#                                                   TUI (without a display - DISPLAY, WAYLAND_DISPLAY - the TUI only)
#   ./install.sh --out /opt/m-strata --with email,search,tui
#   ./install.sh --plan                             only say what it would install, fetch and run
#   ./install.sh --yes                              do not ask before installing
#   ./install.sh --debugging                        a server that logs verbosely, to find out why a model does not
#                                                   start or calibrate (the plan says what to take care of)
#
# What it does, each step only when needed:
#   1. finds what the build needs: git, Python 3.10+ with venv, GCC 13+ (build-essential), pkg-config, the CUDA
#      Toolkit 13.0, OpenSSL's headers (--with email) and GTK 3 / WebKitGTK 4.1 / Ayatana AppIndicator (--with window);
#   2. lists what is missing and asks, then installs it with apt (sudo) - the CUDA Toolkit from NVIDIA's own apt
#      repository (its cuda-keyring package). The NVIDIA driver is not installed here (your distribution's way);
#   3. when this script is not inside the repository, clones it (with its submodules) beside the script;
#   4. puts CMake and Ninja in the repository's own Python environment (.venv-dev, which make.sh uses);
#   5. fetches the pinned llama.cpp commit the engine builds ggml from (.toolchains/llama.cpp; an existing ../llama.cpp at
#      that commit is used as it is);
#   6. runs make.sh into a new folder and says how to start it.
# Downloads: apt packages (your distribution's and NVIDIA's signed repositories), Python packages from PyPI, the
# repositories from GitHub. Nothing else is downloaded, and nothing is sent anywhere.
set -eu

# what the build pins (keep in step with engine/CMakeLists.txt, README.md and install.ps1: tools/secure/test_make.py)
LLAMA_PIN=3cf03257f219afbe7334045ff7c6a06ac68c627d
LLAMA_URL=https://github.com/ggml-org/llama.cpp.git
REPO_URL=https://github.com/shift-f10/m-strata.git
CUDA_PACKAGE=cuda-toolkit-13-0
CUDA_ROOT=/usr/local/cuda-13.0

out=""
with=""
ggml=""
plan=0
debugging=0
yes=0
while [ $# -gt 0 ]; do
  case "$1" in
    --out) out=$2; shift 2 ;;
    --with) with=$2; shift 2 ;;
    --ggml-dir) ggml=$2; shift 2 ;;
    --plan) plan=1; shift ;;
    --debugging) debugging=1; shift ;;
    --yes) yes=1; shift ;;
    -h|--help) sed -n '2,23p' "$0"; exit 0 ;;
    *) echo "install: unknown option $1 (see ./install.sh --help)" >&2; exit 2 ;;
  esac
done

say() { printf '%s\n' "$*"; }
step() { printf '\n== %s\n' "$*"; }
fail() { printf '\ninstall: %s\n' "$*" >&2; exit 1; }
has_feature() { case ",$with," in *",$1,"*) return 0 ;; esac; return 1; }
installed() { dpkg-query -W -f='${Status}' "$1" 2>/dev/null | grep -q 'install ok installed'; }

# --- 1. what is there ------------------------------------------------------------------------------------------------
# both front ends by default, the user picks one to run; a machine with no display (a server over SSH) gets the TUI only
if [ -z "$with" ]; then
  if [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]; then with="email,search,window,tui"
  else with="email,search,tui"; no_display=1; fi
fi

step 'Looking at what this PC has'
[ "$(uname -s)" = Linux ] || fail 'install.sh is for Linux; on Windows run install.bat'
[ "$(uname -m)" = x86_64 ] || fail 'M-Strata builds on x86_64 only'
[ -r /etc/os-release ] || fail 'no /etc/os-release: install the packages by hand (README.md, Install)'
. /etc/os-release
case "${ID:-}" in
  ubuntu) repo_os="ubuntu$(printf '%s' "$VERSION_ID" | tr -d .)" ;;
  debian) repo_os="debian$VERSION_ID" ;;
  *) fail "this script knows Ubuntu and Debian (apt); on ${ID:-this system} install the packages by hand (README.md, Install)" ;;
esac
command -v apt-get >/dev/null 2>&1 || fail 'apt-get was not found'

packages="git python3 python3-venv build-essential pkg-config wget ca-certificates"
if has_feature email; then packages="$packages libssl-dev"; fi
if has_feature window; then packages="$packages libgtk-3-dev libwebkit2gtk-4.1-dev libayatana-appindicator3-dev"; fi
missing=""
for package in $packages; do
  if installed "$package"; then say "  $package: installed"; else say "  $package: missing - will install"; missing="$missing $package"; fi
done
cuda=""
if [ -x "$CUDA_ROOT/bin/nvcc" ]; then cuda=$CUDA_ROOT; say "  CUDA Toolkit 13.0: $CUDA_ROOT"
else say "  CUDA Toolkit 13.0: missing - will install $CUDA_PACKAGE from NVIDIA's $repo_os repository"; fi
if command -v nvidia-smi >/dev/null 2>&1; then driver=yes; say "  NVIDIA driver: $(command -v nvidia-smi)"
else driver=""; say '  NVIDIA driver (to run, not to build): not found - install it your distribution'"'"'s way'; fi
if has_feature window; then say '  (the window on Linux is written but has not yet been compiled by its authors)'; fi

# --- where the repository is -----------------------------------------------------------------------------------------
here=$(cd "$(dirname "$0")" && pwd)
if [ -f "$here/make.py" ]; then in_repo=1; repo=$here; else in_repo=0; repo="$here/m-strata"; fi
parent=$(dirname "$repo")
if [ -z "$out" ]; then
  out="$parent/M-Strata-app"
  n=2
  # a new folder each time (make never deletes)
  while [ -d "$out" ] && [ -n "$(ls -A "$out" 2>/dev/null)" ]; do out="$parent/M-Strata-app-$n"; n=$((n + 1)); done
fi
ggml_given=1
if [ -z "$ggml" ]; then
  ggml_given=0
  ggml="$repo/.toolchains/llama.cpp"
  if [ -d "$parent/llama.cpp/.git" ] && command -v git >/dev/null 2>&1 &&
     [ "$(git -C "$parent/llama.cpp" rev-parse HEAD 2>/dev/null || true)" = "$LLAMA_PIN" ]; then
    ggml="$parent/llama.cpp"
  fi
fi

step 'The plan'
if [ -n "$missing" ]; then say "  sudo apt-get install$missing"; fi
if [ -z "$cuda" ]; then say "  NVIDIA's cuda-keyring for $repo_os, then sudo apt-get install $CUDA_PACKAGE"; fi
if [ "$in_repo" = 0 ]; then say "  clone $REPO_URL (with its submodules) into $repo"; fi
say "  CMake and Ninja into $repo/.venv-dev (pip, from PyPI)"
if [ "$ggml_given" = 1 ]; then say "  llama.cpp: $ggml, as it is"; else say "  llama.cpp at $LLAMA_PIN in $ggml"; fi
make_flags=""
[ "$debugging" = 1 ] && make_flags="--debugging"
say "  make.sh --out $out --with $with${make_flags:+ $make_flags}"
if [ "$debugging" = 1 ]; then
  say ''
  say "  --debugging: the server will log verbosely, to find out why a model does not start or calibrate. It logs"
  say "  one line per request (never its text) and the engine's own output, which can hold what the engine prints of"
  say "  your prompts and the paths of your model files. Run it on your own PC, keep the server's log private,"
  say "  look through a log before you share it, and build again without the debugging option for everyday use."
  say "  The engine's output can be turned off without a new build: Settings > Troubleshooting."
fi
if [ "${no_display:-0}" = 1 ]; then say "  (no display here: the TUI only; --with email,search,window,tui adds the window)"; fi
if [ "$plan" = 1 ]; then say ''; say 'install: --plan given - nothing was installed or changed.'; exit 0; fi
if [ "$yes" = 0 ]; then
  printf 'Go ahead? [y/N] '
  read -r answer </dev/tty || answer=""
  case "$answer" in y|Y|yes|YES) ;; *) say 'install: stopped; nothing was changed.'; exit 1 ;; esac
fi

# --- 2. install what is missing --------------------------------------------------------------------------------------
if [ -n "$missing" ]; then
  step 'Installing packages'
  sudo apt-get update
  # shellcheck disable=SC2086 # the list splits into packages
  sudo apt-get install -y $missing
fi
if [ -z "$cuda" ]; then
  step 'Installing the CUDA Toolkit 13.0'
  keyring=$(mktemp -d)
  wget -q -O "$keyring/cuda-keyring.deb" "https://developer.download.nvidia.com/compute/cuda/repos/$repo_os/x86_64/cuda-keyring_1.1-1_all.deb" ||
    fail "NVIDIA has no CUDA repository for $repo_os: install the CUDA Toolkit 13.0 by hand and run again"
  sudo dpkg -i "$keyring/cuda-keyring.deb"
  rm -rf -- "$keyring"
  sudo apt-get update
  sudo apt-get install -y "$CUDA_PACKAGE"
  [ -x "$CUDA_ROOT/bin/nvcc" ] || fail "the CUDA Toolkit is not at $CUDA_ROOT after its install"
  cuda=$CUDA_ROOT
fi
python3 -c 'import sys; sys.exit(0 if sys.version_info >= (3, 10) else 1)' || fail 'Python 3.10 or newer is needed'
gcc_major=$(gcc -dumpversion | cut -d. -f1)
[ "$gcc_major" -ge 13 ] || fail "GCC 13 or newer is needed (this system has GCC $gcc_major): Ubuntu 24.04 or newer has it"

# --- 3. the repository -------------------------------------------------------------------------------------------------
if [ "$in_repo" = 0 ]; then
  step 'Cloning the repository'
  git clone --recurse-submodules "$REPO_URL" "$repo"
elif [ -d "$repo/.git" ]; then
  step "The repository's submodules"
  git -C "$repo" submodule update --init --recursive
fi

# --- 4. CMake and Ninja in the repository's Python environment (make.sh runs make.py with it) --------------------------
step 'CMake and Ninja'
if [ ! -x "$repo/.venv-dev/bin/python" ]; then python3 -m venv "$repo/.venv-dev"; fi
"$repo/.venv-dev/bin/python" -m pip install --disable-pip-version-check cmake ninja

# --- 5. llama.cpp at the pin -------------------------------------------------------------------------------------------
step 'llama.cpp'
if [ "$ggml_given" = 1 ]; then
  # a folder you named is used as it is: never initialised, fetched or checked out here
  [ -f "$ggml/ggml/CMakeLists.txt" ] || fail "--ggml-dir $ggml has no ggml/CMakeLists.txt: name a llama.cpp checkout"
  if [ -d "$ggml/.git" ]; then
    head=$(git -C "$ggml" rev-parse HEAD 2>/dev/null || true)
    if [ "$head" != "$LLAMA_PIN" ]; then say "  note: $ggml is at $head, not the tested $LLAMA_PIN"; fi
  fi
else
  if [ ! -d "$ggml/.git" ]; then
    # only the pinned commit's files (a full clone is about 450 MB)
    mkdir -p "$ggml"
    git -C "$ggml" init -q
    git -C "$ggml" remote add origin "$LLAMA_URL"
  fi
  if [ "$(git -C "$ggml" rev-parse HEAD 2>/dev/null || true)" != "$LLAMA_PIN" ]; then
    git -C "$ggml" fetch --depth 1 origin "$LLAMA_PIN"
    git -C "$ggml" -c advice.detachedHead=false checkout -q FETCH_HEAD
  fi
fi
say "  $ggml"

# --- 6. the build ----------------------------------------------------------------------------------------------------
step "Building into $out (the first build compiles the CUDA engine: allow an hour or more)"
"$repo/make.sh" --out "$out" --ggml-dir "$ggml" --cuda-root "$cuda" --with "$with" $make_flags || fail 'make stopped; its message is above'

step 'Done'
say "  M-Strata is in $out. Start it with:"
if has_feature window; then say "    $out/strata-window          (a window with the page, and a tray icon)"; fi
if has_feature tui; then say "    $out/strata-tui             (the same server in this terminal, with a dashboard)"; fi
say "    $out/strata-unified-server  (prints a one-time login link: open it in a browser on this PC)"
if [ -z "$driver" ]; then say '  No NVIDIA driver was found: install it before starting a model.'; fi
