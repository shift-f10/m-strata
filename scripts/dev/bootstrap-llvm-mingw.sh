#!/usr/bin/env bash
# Fetches ONE pinned llvm-mingw release (open-source clang + mingw-w64 runtime)
# into .toolchains/ for cross-compiling the secure frontend to Windows from
# Linux. The digest below was recorded on first download (2026-10-13); the
# script refuses any file that does not match it.
set -euo pipefail
repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
version="20261006"
name="llvm-mingw-${version}-ucrt-ubuntu-22.04-x86_64"
archive="${name}.tar.xz"
sha256="5f9c6ed95b2d4bdb2869a488c5fd5857fbdabcf288a0aa3eb1da43f6a08d8ab4"
url="https://github.com/mstorsjo/llvm-mingw/releases/download/${version}/${archive}"
dest="$repo/.toolchains"
mkdir -p "$dest/downloads"

if [[ -x "$dest/$name/bin/x86_64-w64-mingw32-clang++" ]]; then
  echo "bootstrap-llvm-mingw: $name already present"
else
  if [[ ! -f "$dest/downloads/$archive" ]]; then
    if [[ -n "${STRATA_TOOLCHAIN_DIR:-}" && -f "$STRATA_TOOLCHAIN_DIR/$archive" ]]; then
      cp "$STRATA_TOOLCHAIN_DIR/$archive" "$dest/downloads/$archive"
    else
      curl -sSL --fail -o "$dest/downloads/$archive" "$url"
    fi
  fi
  if ! echo "$sha256  $dest/downloads/$archive" | sha256sum -c -; then
    rm -f -- "$dest/downloads/$archive"
    echo "bootstrap-llvm-mingw: digest mismatch; the downloaded archive was deleted, re-run to fetch it again" >&2
    exit 1
  fi
  tar -xJf "$dest/downloads/$archive" -C "$dest"
fi
ln -sfn "$name" "$dest/llvm-mingw"
"$dest/llvm-mingw/bin/x86_64-w64-mingw32-clang++" --version | head -1
