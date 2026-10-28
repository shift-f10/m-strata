#!/usr/bin/env bash
# SP7 M2 Task 6 (spec D-8/D-9): Linux release builder.
#
# Release-preset build -> bundle strata-<ver>-linux-x86_64/ carrying
# bin/strata, bin/strata-secure-server, engine/CUDA outputs when present,
# VERSION, the profile manifest, the SBOM, the D-3 update manifest (+ signature
# only when STRATA_SIGNING_KEY names a key file), SHA256SUMS -> tarball, and a
# .deb via dpkg-deb when present.  The distribution judge
# (secure_distribution_check.py --release, unchanged) gates the bundle, and an
# unsigned build is marked `unsigned: not releasable` (exact line).
# Runtime installs and GPU proofs are owner-box steps, never here.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PY="$REPO/.venv-dev/bin/python"
[ -x "$PY" ] || PY="$(command -v python3)"

VERSION=""
OUT=""
BUILD_DIR="$REPO/secure_frontend/out/build/linux-gcc-release"
SKIP_BUILD=0
NOTES_URL="https://secure-strata.invalid/release-notes"

while [ $# -gt 0 ]; do
    case "$1" in
        --version) VERSION="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        --notes-url) NOTES_URL="$2"; shift 2 ;;
        --skip-build) SKIP_BUILD=1; shift ;;
        *) echo "build_release: unknown argument: $1" >&2; exit 2 ;;
    esac
done

if [ -z "$VERSION" ] || [ -z "$OUT" ]; then
    echo "usage: build_release.sh --version V --out DIR [--build-dir DIR] [--skip-build] [--notes-url URL]" >&2
    exit 2
fi
if ! printf '%s' "$VERSION" | grep -Eq '^[A-Za-z0-9._-]{1,64}$'; then
    echo "build_release: version must be 1..64 of [A-Za-z0-9._-]: $VERSION" >&2
    exit 2
fi

if [ "$SKIP_BUILD" -eq 0 ]; then
    ( cd "$REPO/secure_frontend" \
        && cmake --preset linux-gcc-release \
        && cmake --build --preset linux-gcc-release )
fi

NAME="strata-$VERSION-linux-x86_64"
BUNDLE="$OUT/$NAME"
if [ -e "$BUNDLE" ]; then
    echo "build_release: refusing to reuse an existing bundle directory: $BUNDLE" >&2
    exit 1
fi
mkdir -p "$BUNDLE/bin"

for bin in strata strata-secure-server; do
    if [ ! -f "$BUILD_DIR/$bin" ]; then
        echo "build_release: required product binary missing: $BUILD_DIR/$bin (build first)" >&2
        exit 1
    fi
    cp "$BUILD_DIR/$bin" "$BUNDLE/bin/$bin"
    chmod 0755 "$BUNDLE/bin/$bin"
done

# Engine and CUDA outputs are copied only when this build tree actually has
# them (agent boxes lack the engine build and the GPU stack; the owner's box
# produces the full bundle — the offline kit proves that one).
for engine in strata-engine secure-strata-engine; do
    if [ -f "$BUILD_DIR/$engine" ]; then
        cp "$BUILD_DIR/$engine" "$BUNDLE/bin/$engine"
        chmod 0755 "$BUNDLE/bin/$engine"
        break
    fi
done
cuda="$(find "$BUILD_DIR" -maxdepth 2 -type f -name 'libcudart.so.*' 2>/dev/null | sort | head -1 || true)"
if [ -n "$cuda" ]; then
    mkdir -p "$BUNDLE/lib"
    cp "$cuda" "$BUNDLE/lib/$(basename "$cuda")"
fi

printf '%s\n' "$VERSION" > "$BUNDLE/VERSION"
"$PY" "$REPO/scripts/release/make_manifest.py" --version "$VERSION" \
    --out "$BUNDLE/secure-release-manifest.json"
"$PY" "$REPO/scripts/release/sbom.py" --version "$VERSION" \
    --out "$BUNDLE/sbom.spdx.json"
"$PY" "$REPO/scripts/release/make_update_manifest.py" --bundle "$BUNDLE" \
    --version "$VERSION" --notes-url "$NOTES_URL" --out "$BUNDLE/manifest.json"

if [ -n "${STRATA_SIGNING_KEY:-}" ]; then
    if [ ! -f "$STRATA_SIGNING_KEY" ]; then
        echo "build_release: STRATA_SIGNING_KEY does not name a file: $STRATA_SIGNING_KEY" >&2
        exit 1
    fi
    "$PY" "$REPO/scripts/release/sign.py" --key-file "$STRATA_SIGNING_KEY" \
        --manifest "$BUNDLE/manifest.json" --out "$BUNDLE/manifest.json.sig"
    seed="$(tr -d '[:space:]' < "$STRATA_SIGNING_KEY")"
    pin="$("$PY" "$REPO/scripts/release/keygen.py" --seed-hex "$seed" \
        | awk '/^pinned public key:/{print $4}')"
    "$PY" "$REPO/scripts/release/sign.py" --verify --key-hex "$pin" \
        --envelope "$BUNDLE/manifest.json.sig"
    echo "signed: manifest.json.sig verifies against the pinned key"
else
    echo "unsigned: not releasable"
fi

"$PY" "$REPO/scripts/secure_distribution_check.py" --package-dir "$BUNDLE" --release

( cd "$BUNDLE" && find . -type f ! -name SHA256SUMS -printf '%P\n' | sort | \
    xargs -d '\n' sha256sum > SHA256SUMS )
( cd "$BUNDLE" && sha256sum -c --quiet SHA256SUMS )

mkdir -p "$OUT"
tar -czf "$OUT/$NAME.tar.gz" -C "$OUT" "$NAME"

if command -v dpkg-deb >/dev/null 2>&1; then
    DEB_ROOT="$OUT/deb-root-$VERSION"
    if [ -e "$DEB_ROOT" ]; then
        echo "build_release: refusing to reuse an existing deb root: $DEB_ROOT" >&2
        exit 1
    fi
    mkdir -p "$DEB_ROOT/DEBIAN" "$DEB_ROOT/usr/bin" "$DEB_ROOT/usr/share/strata-secure"
    chmod 0755 "$DEB_ROOT/DEBIAN" "$DEB_ROOT/usr" "$DEB_ROOT/usr/bin" \
        "$DEB_ROOT/usr/share" "$DEB_ROOT/usr/share/strata-secure"
    {
        printf 'Package: strata-secure\n'
        printf 'Version: %s\n' "$VERSION"
        printf 'Architecture: amd64\n'
        printf 'Maintainer: Secure Strata <noreply@secure-strata.invalid>\n'
        printf 'Description: Secure Strata native offline frontend\n'
        printf ' Hardened C++ frontend for a locally supplied Strata engine;\n'
        printf ' offline by default; no analytics, telemetry, or downloads.\n'
    } > "$DEB_ROOT/DEBIAN/control"
    cp "$BUNDLE/bin/strata" "$BUNDLE/bin/strata-secure-server" "$DEB_ROOT/usr/bin/"
    cp "$BUNDLE/VERSION" "$BUNDLE/secure-release-manifest.json" "$BUNDLE/sbom.spdx.json" \
       "$BUNDLE/manifest.json" "$DEB_ROOT/usr/share/strata-secure/"
    [ -f "$BUNDLE/manifest.json.sig" ] && cp "$BUNDLE/manifest.json.sig" "$DEB_ROOT/usr/share/strata-secure/"
    dpkg-deb --build --root-owner-group "$DEB_ROOT" "$OUT/strata-secure_${VERSION}_amd64.deb"
    echo "deb: $OUT/strata-secure_${VERSION}_amd64.deb"
fi

echo "bundle: $BUNDLE"
echo "tarball: $OUT/$NAME.tar.gz"
