#!/usr/bin/env bash
#
# Build the PSXTerm runtime bundle for PS5 out of pinned PacBrew recipes.
#
# STATUS: IN PROGRESS - the recipe extraction, pinning, download and
# verification are in place, and the cross toolchain itself is fine (a probe
# that includes <errno.h> compiles from a plain shell with prospero.sh
# sourced). The zlib stage still fails: zlib's configure mis-detects the cross
# environment (it ends up with -DNO_STRERROR -DNO_vsnprintf and a build that
# cannot find errno), so the detection has to be seeded the way PacBrew's
# makepkg environment does it. Do not report this bundle as working until
# `curl --version` runs on hardware.
#
# Roadmap rules this script follows:
#   - PacBrew is a BUILD-TIME source of PS5 ports, pinned to one revision;
#   - only the packages PSXTerm needs are built (no ci-libs.sh, no games, no
#     SDL/Mesa/FFmpeg);
#   - the result is a relocatable bundle under one prefix, with the CA bundle
#     inside it, so it can be installed with the existing file transfer.
#
# usage:
#   tools/ps5-runtime-build.sh [--bundle DIR] [--work DIR] [--jobs N]
#
# environment:
#   PS5_PAYLOAD_SDK   path to the payload SDK (default: the one used by the
#                     cross builds)
#   PACBREW_REPO      path to a pacbrew-repo checkout (cloned if missing)
#
set -uo pipefail

PACBREW_PIN="dbb6998c90910f58d4f781fc2cab0ecb5ac202b4" # pinned revision
PACBREW_URL="https://github.com/ps5-payload-dev/pacbrew-repo.git"

PACBREW_REPO="${PACBREW_REPO:-$HOME/psxterm-tools/pacbrew-repo}"
PS5_PAYLOAD_SDK="${PS5_PAYLOAD_SDK:-$HOME/psxterm-tools/sdk-ps5/ps5-payload-sdk}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

BUNDLE="$REPO_ROOT/dist/ps5-runtime"
WORK="$HOME/psxterm-build/ps5-runtime"
JOBS="$(nproc 2>/dev/null || echo 4)"
PKGS=(zlib openssl libpsl curl)

while [ $# -gt 0 ]; do
    case "$1" in
    --bundle) BUNDLE="$2"; shift 2 ;;
    --work) WORK="$2"; shift 2 ;;
    --jobs) JOBS="$2"; shift 2 ;;
    -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

log() { echo "==> $*"; }
die() { echo "ERROR: $*" >&2; exit 1; }

[ -d "$PS5_PAYLOAD_SDK" ] || die "payload SDK not found at $PS5_PAYLOAD_SDK"
[ -f "$PS5_PAYLOAD_SDK/toolchain/prospero.sh" ] ||
    die "no prospero.sh in $PS5_PAYLOAD_SDK/toolchain"

# ---------------------------------------------------------------- pacbrew ---

if [ ! -d "$PACBREW_REPO/.git" ]; then
    log "cloning pacbrew-repo"
    git clone "$PACBREW_URL" "$PACBREW_REPO" >/dev/null 2>&1 ||
        die "cannot clone pacbrew-repo"
fi

(
    cd "$PACBREW_REPO" || die "cannot enter $PACBREW_REPO"
    have="$(git rev-parse HEAD 2>/dev/null || echo none)"
    if [ "$have" != "$PACBREW_PIN" ]; then
        log "pinning pacbrew-repo to ${PACBREW_PIN:0:12}"
        git fetch --depth 1 origin "$PACBREW_PIN" >/dev/null 2>&1 ||
            die "cannot fetch the pinned revision"
        git checkout --detach "$PACBREW_PIN" >/dev/null 2>&1 ||
            die "cannot check out the pinned revision"
    fi
)

# Read one field from a pinned recipe, so the script follows the recipes
# instead of duplicating them.
recipe_field() {
    local pkg="$1" field="$2"

    sed -n "s/^${field}=//p" "$PACBREW_REPO/$pkg/PKGBUILD" | head -1 |
        tr -d "'\""
}

# ------------------------------------------------------------------ build ---

mkdir -p "$WORK" "$BUNDLE"
mkdir -p "$BUNDLE/bin" "$BUNDLE/lib" "$BUNDLE/etc" "$BUNDLE/include"
mkdir -p "$BUNDLE/share"

fetch() {
    local url="$1" want="$2" out="$3"

    if [ -f "$out" ]; then
        echo "$want  $out" | sha256sum -c - >/dev/null 2>&1 && return 0
    fi

    log "fetching $(basename "$out")"
    curl -fsSL -o "$out" "$url" || return 1
    echo "$want  $out" | sha256sum -c - >/dev/null 2>&1
}

build_zlib() {
    local ver url sum tarball
    ver="$(recipe_field zlib pkgver)"
    url="https://github.com/madler/zlib/releases/download/v$ver/zlib-$ver.tar.gz"
    sum="$(recipe_field zlib sha256sums)"
    tarball="$WORK/zlib-$ver.tar.gz"

    fetch "$url" "${sum//[()]/}" "$tarball" || die "zlib download failed"
    rm -rf "$WORK/zlib-$ver"
    tar -xf "$tarball" -C "$WORK" || die "zlib unpack failed"

    # zlib's own configure mis-detects a cross environment (it ends up
    # without vsnprintf and errno), so its CMake build is used instead: the
    # SDK ships a CMake toolchain file that our own cross builds already use
    # successfully.
    (
        cd "$WORK/zlib-$ver" || die "zlib dir missing"
        cmake -S . -B build \
            -DCMAKE_TOOLCHAIN_FILE="$PS5_PAYLOAD_SDK/toolchain/prospero.cmake" \
            -DCMAKE_INSTALL_PREFIX="$BUNDLE" \
            -DBUILD_TESTING=OFF \
            -DZLIB_BUILD_TESTING=OFF \
            -DZLIB_BUILD_EXAMPLES=OFF >/dev/null || exit 1
        cmake --build build -j"$JOBS" >/dev/null || exit 1
        cmake --install build >/dev/null || exit 1
    ) || die "zlib build failed"

    # Only the static library may stay in the bundle: a console payload has no
    # dynamic linker, and our loader does not resolve shared libraries, so a
    # binary that links libz.so.1 starts and then dies on its own PLT (that is
    # exactly how curl behaved before this line existed).
    rm -f "$BUNDLE"/lib/libz.so "$BUNDLE"/lib/libz.so.*
}

build_openssl() {
    local ver url sum tarball
    ver="$(recipe_field openssl pkgver)"
    url="https://github.com/openssl/openssl/releases/download/openssl-$ver/openssl-$ver.tar.gz"
    sum="$(recipe_field openssl sha256sums)"
    tarball="$WORK/openssl-$ver.tar.gz"

    fetch "$url" "${sum//[()]/}" "$tarball" || die "openssl download failed"
    rm -rf "$WORK/openssl-$ver"
    tar -xf "$tarball" -C "$WORK" || die "openssl unpack failed"

    (
        cd "$WORK/openssl-$ver" || die "openssl dir missing"
        # shellcheck disable=SC1090
        source "$PS5_PAYLOAD_SDK/toolchain/prospero.sh"

        # The SDK environment stages installs under its own sysroot; the
        # bundle must receive everything instead.
        export DESTDIR=

        ./Configure BSD-x86_64 no-tests no-apps no-shared \
            --prefix="$BUNDLE" >/dev/null || exit 1
        make -j"$JOBS" build_sw >/dev/null || exit 1
        make install_sw >/dev/null || exit 1
    ) || die "openssl build failed"
}

build_libpsl() {
    local ver url sum tarball
    ver="$(recipe_field libpsl pkgver)"
    url="https://github.com/rockdaboot/libpsl/releases/download/$ver/libpsl-$ver.tar.gz"
    sum="$(recipe_field libpsl sha256sums)"
    tarball="$WORK/libpsl-$ver.tar.gz"

    fetch "$url" "${sum//[()]/}" "$tarball" || die "libpsl download failed"
    rm -rf "$WORK/libpsl-$ver"
    tar -xf "$tarball" -C "$WORK" || die "libpsl unpack failed"

    (
        cd "$WORK/libpsl-$ver" || die "libpsl dir missing"
        # shellcheck disable=SC1090
        source "$PS5_PAYLOAD_SDK/toolchain/prospero.sh"

        # See the note in build_openssl: install into the bundle, not into the
        # SDK sysroot the environment stages towards.
        export DESTDIR=

        ./configure --prefix="$BUNDLE" --host=x86_64-pc-freebsd \
            --enable-static --disable-shared --disable-nls \
            --disable-gtk-doc-html >/dev/null || exit 1
        make -j"$JOBS" >/dev/null || exit 1
        make install >/dev/null || exit 1
    ) || die "libpsl build failed"
}

build_curl() {
    local ver url sum tarball
    ver="$(recipe_field curl pkgver)"
    url="https://curl.haxx.se/download/curl-$ver.tar.xz"
    sum="$(recipe_field curl sha256sums)"
    tarball="$WORK/curl-$ver.tar.xz"

    fetch "$url" "${sum//[()]/}" "$tarball" || die "curl download failed"
    rm -rf "$WORK/curl-$ver"
    tar -xf "$tarball" -C "$WORK" || die "curl unpack failed"

    (
        cd "$WORK/curl-$ver" || die "curl dir missing"

        # Same source preparation the pinned recipe performs.
        sed -i 's|define USE_XATTR| |g' src/tool_xattr.h
        sed -i 's|hg.mozilla.org|hg-edge.mozilla.org|g' scripts/mk-ca-bundle.pl
        autoreconf -fi >/dev/null 2>&1 || exit 1

        # shellcheck disable=SC1090
        source "$PS5_PAYLOAD_SDK/toolchain/prospero.sh"

        # See the note in build_openssl: the bundle is the install target.
        export DESTDIR=

        # Fetch CA bundle
        ./scripts/mk-ca-bundle.pl >/dev/null 2>&1 ||
            echo "warning: CA bundle generation failed, using the fallback"

        # Existing prefix contents are picked up through CPPFLAGS/LDFLAGS.
        #
        # --without-libpsl is deliberate: measured on hardware, a payload that
        # links libpsl hangs before its own main (its static constructor never
        # returns there), and curl was the only thing in this bundle linking
        # it. The public suffix list only affects cookie handling, which a
        # download does not need.
        #
        # The CA bundle path is the one the runtime layout installs, not the
        # build directory: a baked-in host path made every request fail with
        # "Problem with the SSL CA cert" on the console.
        PKG_CONFIG_PATH="$BUNDLE/lib/pkgconfig" \
        CPPFLAGS="-I$BUNDLE/include" \
        LDFLAGS="-L$BUNDLE/lib" \
        ./configure --prefix="$BUNDLE" --host=x86_64-pc-freebsd \
            --enable-static --disable-shared \
            --with-openssl \
            --without-libpsl \
            --with-ca-bundle=/data/psxterm/runtime/etc/ca-bundle.crt \
            --disable-docs >/dev/null || exit 1

        make -j"$JOBS" >/dev/null || exit 1
        make install >/dev/null || exit 1

        [ -f ca-bundle.crt ] && cp ca-bundle.crt "$BUNDLE/etc/ca-bundle.crt"
    ) || die "curl build failed"
}

for pkg in "${PKGS[@]}"; do
    log "building $pkg ($(recipe_field "$pkg" pkgver))"
    "build_$pkg"
done

# ------------------------------------------------------------- packaging ---

CA="$BUNDLE/etc/ca-bundle.crt"
if [ ! -s "$CA" ]; then
    log "no CA bundle from the build, fetching the Mozilla bundle"
    curl -fsSL -o "$CA" https://curl.se/ca/cacert.pem ||
        echo "warning: no CA bundle; HTTPS certificate validation will fail"
fi

[ -s "$CA" ] || echo "warning: $CA is empty"

cat >"$BUNDLE/runtime.json" <<EOF
{
  "version": 1,
  "platform": "ps5",
  "arch": "x86_64",
  "pacbrew": "$PACBREW_PIN",
  "packages": {
    "zlib": "$(recipe_field zlib pkgver)",
    "openssl": "$(recipe_field openssl pkgver)",
    "libpsl": "$(recipe_field libpsl pkgver)",
    "curl": "$(recipe_field curl pkgver)"
  }
}
EOF

log "bundle ready: $BUNDLE"
echo
echo "curl binary:  $(ls -l "$BUNDLE/bin/curl" 2>/dev/null | awk '{print $5" bytes"}')"
echo "CA bundle:    $(wc -c <"$CA" 2>/dev/null) bytes"
echo "manifest:     $BUNDLE/runtime.json"
echo
echo "install with:"
echo "  psxterm push <host> $BUNDLE/bin/curl /data/psxterm/runtime/bin/curl"
echo "  psxterm push <host> $CA /data/psxterm/runtime/etc/ca-bundle.crt"
