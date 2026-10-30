#!/bin/sh
# install.sh - from a fresh Ubuntu 22.04 (or newer) / Debian 13 PC to a built M-Strata in one step (install.bat's twin).
#
#   ./install.sh                                    ask what to build (Enter takes each default: everything, the
#                                                   Debugging plugin off, and without a display - DISPLAY,
#                                                   WAYLAND_DISPLAY - no window), then build into ../M-Strata-app
#   ./install.sh --out /opt/m-strata --with email,search,tui
#   ./install.sh --without maya,raon1,vision        leave the GLM/DeepSeek/MiniMax engines, raon1 or pictures out
#   ./install.sh --plan                             only say what it would install, fetch and run
#   ./install.sh --yes                              ask nothing: build everything (no debugging; no window without a
#                                                   display), install without asking
#   ./install.sh --debugging                        the Debugging plugin on: a server that logs verbosely, to find
#                                                   out why a model does not start or calibrate (the plan says what
#                                                   to take care of)
#   ./install.sh --signing-passphrase               seal the plugin signing key the build makes with a passphrase
#                                                   (make asks for it; asked when this PC has no key yet)
#   ./install.sh --searxng                          a local SearXNG for web search, in Docker (installed when missing):
#                                                   127.0.0.1:8888, this PC only; the Web search plugin uses it from
#                                                   the first start (asked when web search is built)
#   ./install.sh --searxng-url URL                  a SearXNG you already run, for web search (asked before the local
#                                                   one); the Web search plugin uses it from the first start
#
# What it does, each step only when needed:
#   1. finds what the build needs: git, Python 3.10+ with venv, GCC 13+ (build-essential; on Ubuntu 22.04 gcc-13 from
#      Ubuntu's toolchain PPA, which the build then uses while the system's gcc stays), pkg-config, the CUDA Toolkit
#      13.0, OpenSSL's headers (--with email) and GTK 3 / WebKitGTK 4.1 / Ayatana AppIndicator (--with window) - and
#      stops before installing anything when the system cannot have them;
#   2. lists what is missing and asks, then installs it with apt (sudo) - the CUDA Toolkit from NVIDIA's own apt
#      repository (its cuda-keyring package). The NVIDIA driver is not installed here (your distribution's way);
#   3. when this script is not inside the repository, clones it (with its submodules) beside the script;
#   4. puts CMake and Ninja in the repository's own Python environment (.venv-dev, which make.sh uses);
#   5. fetches the pinned llama.cpp commit the engine builds ggml from (.toolchains/llama.cpp; an existing ../llama.cpp at
#      that commit is used as it is);
#   6. with --searxng (or its question's yes), sets up SearXNG in Docker beside the repository (docker/searxng);
#   7. runs make.sh into a new folder and says how to start it.
# Downloads: apt packages (your distribution's and NVIDIA's signed repositories, and on Ubuntu 22.04 Ubuntu's
# toolchain PPA for GCC 13), Python packages from PyPI, the repositories from GitHub, and with --searxng SearXNG's
# image from Docker Hub. Nothing else is downloaded, and nothing is sent anywhere.
set -eu

# what the build pins (keep in step with engine/CMakeLists.txt, README.md and install.ps1: tools/secure/test_make.py)
LLAMA_PIN=3cf03257f219afbe7334045ff7c6a06ac68c627d
LLAMA_URL=https://github.com/ggml-org/llama.cpp.git
REPO_URL=https://github.com/shift-f10/m-strata.git
CUDA_PACKAGE=cuda-toolkit-13-0
CUDA_ROOT=/usr/local/cuda-13.0
# the system's description (tests point it at a stand-in's)
os_release=${M_STRATA_OS_RELEASE:-/etc/os-release}

out=""
with=""
with_given=0
without=""
without_given=0
ggml=""
plan=0
debugging=0
debugging_given=0
signing_passphrase=0
signing_passphrase_given=0
searxng=0
searxng_given=0
searxng_url=""
yes=0
while [ $# -gt 0 ]; do
  case "$1" in
    --out) out=$2; shift 2 ;;
    --with) with=$2; with_given=1; shift 2 ;;
    --without) without=$2; without_given=1; shift 2 ;;
    --ggml-dir) ggml=$2; shift 2 ;;
    --plan) plan=1; shift ;;
    --debugging) debugging=1; debugging_given=1; shift ;;
    --signing-passphrase) signing_passphrase=1; signing_passphrase_given=1; shift ;;
    --searxng) searxng=1; searxng_given=1; shift ;;
    --searxng-url) searxng_url=$2; shift 2 ;;
    --yes) yes=1; shift ;;
    -h|--help) sed -n '2,38p' "$0"; exit 0 ;;
    *) echo "install: unknown option $1 (see ./install.sh --help)" >&2; exit 2 ;;
  esac
done

say() { printf '%s\n' "$*"; }
step() { printf '\n== %s\n' "$*"; }
fail() { printf '\ninstall: %s\n' "$*" >&2; exit 1; }
has_feature() { case ",$with," in *",$1,"*) return 0 ;; esac; return 1; }
installed() { dpkg-query -W -f='${Status}' "$1" 2>/dev/null | grep -q 'install ok installed'; }
# ask_text QUESTION: a line from the terminal in $reply (Enter, or no terminal: empty)
ask_text() {
  printf '  %s ' "$1"
  reply=""
  read -r reply </dev/tty || { printf '\n'; reply=""; }
}
# a SearXNG address as make and the server take one (make.py's valid_searxng_url; make checks the port's range)
valid_searxng_url() {
  [ "${#1}" -le 200 ] &&
    printf '%s' "$1" | grep -Eq '^[Hh][Tt][Tt][Pp][Ss]?://(\[[0-9A-Fa-f:.]{2,}\]|[A-Za-z0-9][A-Za-z0-9.-]{0,252})(:[0-9]{1,5})?(/[A-Za-z0-9._~/-]*)?$' &&
    ! printf '%s' "${1#*://}" | grep -q '//'
}
# ask QUESTION y|n: yes or no from the terminal; Enter (or no terminal) takes the default
ask() {
  if [ "$2" = y ]; then hint='[Y/n]'; else hint='[y/N]'; fi
  while :; do
    printf '  %s %s ' "$1" "$hint"
    reply=""
    read -r reply </dev/tty || { printf '\n'; reply=""; }
    case "$reply" in
      '') [ "$2" = y ]; return ;;
      y|Y|yes|Yes|YES) return 0 ;;
      n|N|no|No|NO) return 1 ;;
      *) say '  y or n, please (Enter takes the capital one)' ;;
    esac
  done
}

# --- what to build (owner 2026-10-29: "we should ask these questions, if they want email alerts, search, etc so that
# they can build exactly what they want"): a question for each of make's choices that --with, --without or --debugging
# did not answer on the command line; --yes asks nothing and takes the defaults. make's lists (make.py's
# OPTIONAL_FEATURES and LEAVE_OUT: tools/secure/test_install.py)
known_features="email search window tui"
known_leave_out="maya raon1 vision"
with=$(printf '%s' "$with" | tr -d ' ')
without=$(printf '%s' "$without" | tr -d ' ')
for name in $(printf '%s' "$with" | tr ',' ' '); do
  case " $known_features " in *" $name "*) ;; *) fail "--with takes only email, search, window, tui, not $name" ;; esac
done
for name in $(printf '%s' "$without" | tr ',' ' '); do
  case " $known_leave_out " in *" $name "*) ;; *) fail "--without takes only maya, raon1, vision, not $name" ;; esac
done
# both front ends by default, the user picks one to run; a machine with no display (a server over SSH) gets the TUI only
display=0
if [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]; then display=1; fi
if [ "$with_given" = 0 ]; then
  if [ "$display" = 1 ]; then with="email,search,window,tui"
  else with="email,search,tui"; no_display=1; fi
fi
# this PC's plugin signing key, where make keeps it (scripts/plugin_signing.py's default_key_path): the build makes one
# when there is none - sealed with a passphrase if asked - and one already sealed needs its passphrase (make asks)
signing_key_file="${XDG_CONFIG_HOME:-$HOME/.config}/m-strata/plugin-signing-key.json"
key_exists=0
key_sealed=0
if [ -e "$signing_key_file" ]; then
  key_exists=1
  if grep -Eq '"passphrase"[[:space:]]*:[[:space:]]*true' "$signing_key_file" 2>/dev/null; then key_sealed=1; fi
fi
ask_passphrase=0
if [ "$yes" = 0 ] && [ "$signing_passphrase_given" = 0 ] && [ "$key_exists" = 0 ]; then ask_passphrase=1; fi
# a local SearXNG for web search (owner 2026-10-30: "ask if the user wants it locally which we will install if yes"):
# asked whenever web search is built; a yes sets it up in Docker, installing Docker when it is missing. Its default is
# yes where Docker is already here, no where it would be installed.
docker_ready=0
if command -v docker >/dev/null 2>&1 && docker compose version >/dev/null 2>&1; then docker_ready=1; fi
if [ "$docker_ready" = 1 ]; then searxng_default=y; else searxng_default=n; fi
# (or a SearXNG you already run: its address, asked first - owner 2026-10-30: "if so they can just provide details")
if [ -n "$searxng_url" ]; then
  valid_searxng_url "$searxng_url" || fail "--searxng-url: not a SearXNG address (http(s)://host[:port][/path], with no user, query or fragment): $searxng_url"
  [ "$searxng" = 0 ] || fail "--searxng sets up a local SearXNG and --searxng-url names one you run: one or the other"
fi
ask_searxng=0
if [ "$yes" = 0 ] && [ "$searxng_given" = 0 ] && [ -z "$searxng_url" ] && { [ "$with_given" = 0 ] || has_feature search; }; then ask_searxng=1; fi
if [ "$yes" = 0 ] && { [ "$with_given" = 0 ] || [ "$without_given" = 0 ] || [ "$debugging_given" = 0 ] || [ "$ask_passphrase" = 1 ] || [ "$ask_searxng" = 1 ]; }; then
  step 'What to build'
  say '  Answer y or n; Enter takes the capital one. Qwen, Gemma 4 and flint models are always built.'
  if [ "$with_given" = 0 ]; then
    with=""
    no_display=0
    if [ "$display" = 1 ]; then window_default=y; window_note=""; else window_default=n; window_note=" (no display here)"; fi
    if ask 'Email alerts: the server e-mails you when a download or a calibration ends, an engine stops, or it starts?' y; then with="$with,email"; fi
    if ask 'Web search: the models search the web through a SearXNG you run?' y; then with="$with,search"; fi
    if ask "The window app (strata-window): the page in a window of its own, with a tray icon$window_note?" "$window_default"; then with="$with,window"; fi
    if ask 'The terminal app (strata-tui): the server in a terminal, with a dashboard?' y; then with="$with,tui"; fi
    with=${with#,}
  fi
  if [ "$without_given" = 0 ]; then
    without=""
    if ! ask "GLM-5.3, DeepSeek-V4.1 and MiniMax-M3.1 models (Project Maya's engines, the largest part of the build)?" y; then without="$without,maya"; fi
    if ! ask 'Raon-1-mini models (the raon1 engine)?' y; then without="$without,raon1"; fi
    if ! ask 'Pictures in Qwen chats (the picture encoder, strata-vision)?' y; then without="$without,vision"; fi
    without=${without#,}
  fi
  if [ "$debugging_given" = 0 ]; then
    if ask 'The Debugging plugin turned on: verbose logs, to find out why a model does not start or calibrate (the plan says what it logs)?' n; then debugging=1; fi
  fi
  if [ "$ask_passphrase" = 1 ]; then
    if ask 'Seal the plugin signing key this build makes with a passphrase (make asks for it now, and at every later build or plugin you sign)?' n; then signing_passphrase=1; fi
  fi
  if [ "$ask_searxng" = 1 ] && has_feature search; then
    while :; do
      ask_text 'A SearXNG you already run for web search? Its address (http://host:port), or Enter for none:'
      [ -z "$reply" ] && break
      if valid_searxng_url "$reply"; then searxng_url=$reply; break; fi
      say "  not a SearXNG address: http(s)://host[:port][/path], with no user, query or fragment"
    done
    if [ -z "$searxng_url" ]; then
      if [ "$docker_ready" = 1 ]; then docker_note=""; else docker_note=", installing Docker"; fi
      if ask "A local SearXNG for web search (in Docker$docker_note; 127.0.0.1:8888, this PC only)?" "$searxng_default"; then searxng=1; fi
    fi
  fi
fi
if [ "$yes" = 1 ] && [ "$searxng_given" = 0 ] && [ -z "$searxng_url" ] && has_feature search && [ "$docker_ready" = 1 ]; then searxng=1; fi
if [ "$searxng" = 1 ] && ! has_feature search; then
  fail "--searxng sets up the Web search plugin's SearXNG: build web search too (--with ...,search)"
fi
if [ -n "$searxng_url" ] && ! has_feature search; then
  fail "--searxng-url is the Web search plugin's SearXNG: build web search too (--with ...,search)"
fi

# --- 1. what is there ------------------------------------------------------------------------------------------------
step 'Looking at what this PC has'
[ "$(uname -s)" = Linux ] || fail 'install.sh is for Linux; on Windows run install.bat'
[ "$(uname -m)" = x86_64 ] || fail 'M-Strata builds on x86_64 only'
[ -r "$os_release" ] || fail "no $os_release: install the packages by hand (README.md, Install)"
. "$os_release"
case "${ID:-}" in
  ubuntu) repo_os="ubuntu$(printf '%s' "$VERSION_ID" | tr -d .)" ;;
  debian) repo_os="debian$VERSION_ID" ;;
  *) fail "this script knows Ubuntu and Debian (apt); on ${ID:-this system} install the packages by hand (README.md, Install)" ;;
esac
command -v apt-get >/dev/null 2>&1 || fail 'apt-get was not found'

# GCC 13 or newer, the build's C++20 (the owner's Azure VM, 2026-10-30: Ubuntu 22.04 has GCC 11, and this used to be found
# only after the CUDA Toolkit was installed). Ubuntu 24.04+ and Debian 13+ have it as gcc; gcc-13/g++-13 beside an older
# gcc are what the build uses (CC, CXX, CUDAHOSTCXX: the system's gcc stays as it is); Ubuntu 22.04 gets them from
# Ubuntu's toolchain PPA; anything else stops here, before anything is installed.
gcc_version() { "$1" -dumpversion 2>/dev/null | cut -d. -f1; }
gcc_major=$(gcc_version gcc || true)
gcc_major=${gcc_major:-0}
release_major=${VERSION_ID%%.*}
compiler=""
gcc_ppa=0
if [ "$gcc_major" -ge 13 ]; then
  say "  GCC: $gcc_major"
elif command -v gcc-13 >/dev/null 2>&1 && command -v g++-13 >/dev/null 2>&1; then
  compiler=13
  say "  GCC 13: gcc-13 and g++-13 (the build uses them; the system's gcc is $gcc_major)"
elif [ "$ID" = ubuntu ] && [ "$release_major" = 22 ]; then
  compiler=13
  gcc_ppa=1
  say "  GCC 13: missing - will install gcc-13 and g++-13 from Ubuntu's toolchain PPA (the system's gcc stays as it is)"
elif [ "$gcc_major" = 0 ] && { { [ "$ID" = ubuntu ] && [ "$release_major" -ge 24 ]; } || { [ "$ID" = debian ] && [ "$release_major" -ge 13 ]; }; }; then
  say "  GCC: comes with build-essential (13 or newer on $PRETTY_NAME)"
else
  fail "GCC 13 or newer is needed (this system has GCC $gcc_major): Ubuntu 24.04 or newer, or Debian 13, has it; on Ubuntu 22.04 this script installs it"
fi

packages="git python3 python3-venv build-essential pkg-config wget ca-certificates"
if [ "$gcc_ppa" = 1 ]; then packages="$packages software-properties-common"; fi  # (add-apt-repository)
if has_feature email; then packages="$packages libssl-dev"; fi
if has_feature window; then packages="$packages libgtk-3-dev libwebkit2gtk-4.1-dev libayatana-appindicator3-dev"; fi
if [ "$searxng" = 1 ] && [ "$docker_ready" = 0 ]; then
  # Docker and its compose plugin, as the distribution packages them
  if [ "$ID" = ubuntu ]; then packages="$packages docker.io docker-compose-v2"; else packages="$packages docker.io docker-compose"; fi
fi
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
if [ "$gcc_ppa" = 1 ]; then
  say "  sudo add-apt-repository -y ppa:ubuntu-toolchain-r/test, then sudo apt-get install -y gcc-13 g++-13 (Ubuntu's toolchain PPA)"
fi
if [ -z "$cuda" ]; then say "  NVIDIA's cuda-keyring for $repo_os, then sudo apt-get install $CUDA_PACKAGE"; fi
if [ "$in_repo" = 0 ]; then say "  clone $REPO_URL (with its submodules) into $repo"; fi
say "  CMake and Ninja into $repo/.venv-dev (pip, from PyPI)"
if [ "$ggml_given" = 1 ]; then say "  llama.cpp: $ggml, as it is"; else say "  llama.cpp at $LLAMA_PIN in $ggml"; fi
make_flags=""
[ "$debugging" = 1 ] && make_flags="--debugging"
# a sealed key goes to make as sealed; one there unsealed is used as it is (make seals only a key it makes)
if [ "$key_sealed" = 1 ] || { [ "$signing_passphrase" = 1 ] && [ "$key_exists" = 0 ]; }; then
  make_flags="${make_flags:+$make_flags }--signing-passphrase"
fi
if [ -n "$without" ]; then make_flags="--without $without${make_flags:+ $make_flags}"; fi
searxng_dir="$parent/docker/searxng"
if [ "$searxng" = 1 ]; then
  # (8888 already taken by something else stops it here; a SearXNG this script set up before is started again)
  if command -v ss >/dev/null 2>&1 && ss -ltn 2>/dev/null | grep -q ':8888 ' && [ ! -f "$searxng_dir/compose.yaml" ]; then
    fail "port 8888 on this PC is taken: stop what uses it, or leave the local SearXNG out"
  fi
  say "  SearXNG for web search: $searxng_dir (Docker; 127.0.0.1:8888, this PC only) - the Web search plugin's address from the first start"
  make_flags="${make_flags:+$make_flags }--searxng-url http://127.0.0.1:8888"
elif [ -n "$searxng_url" ]; then
  say "  SearXNG for web search: $searxng_url (yours) - the Web search plugin's address from the first start"
  make_flags="${make_flags:+$make_flags }--searxng-url $searxng_url"
fi
if [ "$key_exists" = 0 ]; then
  if [ "$signing_passphrase" = 1 ]; then say "  plugin signing key: $signing_key_file - made by this build, sealed with a passphrase: make asks for it, twice"
  else say "  plugin signing key: $signing_key_file - made by this build (a file only you can read, no passphrase)"; fi
elif [ "$key_sealed" = 1 ]; then
  say "  plugin signing key: $signing_key_file, as it is - sealed with a passphrase: make asks for it"
else
  say "  plugin signing key: $signing_key_file, as it is (no passphrase)"
  if [ "$signing_passphrase" = 1 ]; then say '  (--signing-passphrase seals only a key the build makes: this one is used as it is)'; fi
fi
if [ "$compiler" = 13 ]; then make_env="CC=gcc-13 CXX=g++-13 CUDAHOSTCXX=g++-13 "; else make_env=""; fi
say "  ${make_env}make.sh --out $out --with $with${make_flags:+ $make_flags}"
if [ "$debugging" = 1 ]; then
  say ''
  say "  --debugging: the Debugging plugin ships on, so the server logs verbosely, to find out why a model does not"
  say "  start or calibrate. It logs one line per request (never its text) and the engine's own output, which can"
  say "  hold what the engine prints of your prompts and the paths of your model files. Run it on your own PC,"
  say "  keep the server's log private, look through a log before you share it, and turn the plugin off in the"
  say "  Plugins tab for everyday use. The engine's output alone can be turned off in Settings > Troubleshooting."
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
if [ "$gcc_ppa" = 1 ]; then
  # before the CUDA Toolkit's download: the compiler is the smaller step, and the one this system may refuse
  step "GCC 13 from Ubuntu's toolchain PPA"
  sudo add-apt-repository -y ppa:ubuntu-toolchain-r/test
  sudo apt-get update
  sudo apt-get install -y gcc-13 g++-13
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
if [ "$compiler" = 13 ]; then
  [ "$(gcc_version gcc-13 || echo 0)" -ge 13 ] || fail 'gcc-13 is not usable after its install'
  export CC=gcc-13 CXX=g++-13 CUDAHOSTCXX=g++-13  # make's CMake runs take them; the system's gcc stays
else
  gcc_major=$(gcc_version gcc || true)
  [ "${gcc_major:-0}" -ge 13 ] || fail "GCC 13 or newer is needed (this system has GCC ${gcc_major:-none}): Ubuntu 24.04 or newer has it"
fi

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

# --- 6. a local SearXNG for web search (the README's "Web search through SearXNG", owner 2026-10-30) -----------------
# Loopback only (127.0.0.1:8888: nothing outside this PC reaches it); its server bound to IPv4 inside the container (the
# image binds IPv6 by default, which fails where Docker has no IPv6); JSON on; the README's extra engines; a secret made
# here, kept when it is set up again.
if [ "$searxng" = 1 ]; then
  step "SearXNG for web search, in $searxng_dir"
  docker_cmd=docker
  docker info >/dev/null 2>&1 || docker_cmd="sudo docker"  # (the docker group takes a new login to apply)
  mkdir -p "$searxng_dir/config" "$searxng_dir/cache"
  (
    cd "$searxng_dir"
    cat > compose.yaml <<'EOF'
services:
  searxng:
    image: docker.io/searxng/searxng:latest
    container_name: searxng
    restart: unless-stopped
    ports: ["127.0.0.1:8888:8080"]   # this PC only: M-Strata's server reaches it, nothing outside does
    environment: ["GRANIAN_HOST=0.0.0.0"]   # IPv4 inside the container
    volumes: ["./config:/etc/searxng", "./cache:/var/cache/searxng"]
EOF
    if [ ! -f config/settings.yml ]; then
      secret=$(python3 -c 'import secrets; print(secrets.token_hex(32))')
      cat > config/settings.yml <<EOF
use_default_settings: true
server:
  secret_key: "$secret"
  limiter: false
  base_url: "http://127.0.0.1:8888/"
search:
  formats: [html, json]   # M-Strata's Web search plugin reads the JSON
engines:
  - name: bing
    disabled: false
  - name: qwant
    disabled: false
  - name: yahoo
    disabled: false
  - name: mojeek
    disabled: false
    inactive: false
EOF
    fi
    $docker_cmd compose up -d
  ) || fail 'SearXNG did not start: its message is above'
  answered=0
  for _ in 1 2 3 4 5 6 7 8 9 10 11 12; do
    if wget -qO- "http://127.0.0.1:8888/search?q=m-strata&format=json" 2>/dev/null | grep -q '"results"'; then answered=1; break; fi
    sleep 5
  done
  if [ "$answered" = 1 ]; then say '  SearXNG answers on http://127.0.0.1:8888'
  else say '  SearXNG has not answered yet (its first start can take a minute); the Web search plugin uses it once it does'; fi
fi

# --- 7. the build ----------------------------------------------------------------------------------------------------
step "Building into $out (the first build compiles the CUDA engine: allow an hour or more)"
"$repo/make.sh" --out "$out" --ggml-dir "$ggml" --cuda-root "$cuda" --with "$with" $make_flags || fail 'make stopped; its message is above'

step 'Done'
say "  M-Strata is in $out. Start it with:"
if has_feature window; then say "    $out/strata-window          (a window with the page, and a tray icon)"; fi
if has_feature tui; then say "    $out/strata-tui             (the same server in this terminal, with a dashboard)"; fi
say "    $out/strata-unified-server  (prints a one-time login link: open it in a browser on this PC)"
if [ "$searxng" = 1 ]; then say "  Web search: SearXNG on http://127.0.0.1:8888 ($searxng_dir: docker compose there stops and starts it)"
elif [ -n "$searxng_url" ]; then say "  Web search: your SearXNG at $searxng_url"; fi
if [ -z "$driver" ]; then say '  No NVIDIA driver was found: install it before starting a model.'; fi
