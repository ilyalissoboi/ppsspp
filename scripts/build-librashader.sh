#!/usr/bin/env bash
# Builds librashader's C API for the host desktop platform (macOS or Linux) under the bare name
# PPSSPP's loader looks for, so it can be shipped next to the executable. Windows has its own
# script: scripts/build-librashader.ps1. Android has android/build-librashader.sh (keep the pinned
# tag below in sync with that one).
# Usage: scripts/build-librashader.sh [OUTDIR]   (default: <repo>/build-librashader)
# Env:   LIBRASHADER_TAG (default librashader-v0.12.0), LIBRASHADER_SRC
#
# Pass the result to CMake to have it copied next to the binary automatically:
#   ./b.sh --release -DLIBRASHADER_PREBUILT=$PWD/build-librashader/librashader.so   # via $CMAKE_ARGS
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
TAG=${LIBRASHADER_TAG:-librashader-v0.12.0}
SRC=${LIBRASHADER_SRC:-$REPO/build/librashader-src}
OUT=${1:-$REPO/build-librashader}
FEATURES=runtime-vulkan,runtime-opengl   # the backends PPSSPP can drive on macOS/Linux

need() { command -v "$1" >/dev/null 2>&1 || { echo "error: '$1' not found ($2)" >&2; exit 1; }; }
need git "install git"
need cargo "install Rust stable >= 1.88 (rustup, or your distro's rust package)"

case "$(uname -s)" in
	Darwin) LIBNAME=librashader.dylib; ARTIFACT=liblibrashader_capi.dylib;;
	Linux)  LIBNAME=librashader.so;    ARTIFACT=liblibrashader_capi.so;;
	*) echo "error: unsupported host '$(uname -s)' (Windows: scripts/build-librashader.ps1)" >&2; exit 1;;
esac

if [ ! -d "$SRC/.git" ]; then
	git clone --depth 1 --branch "$TAG" https://github.com/SnowflakePowered/librashader.git "$SRC"
else
	(cd "$SRC" && git fetch --depth 1 origin "refs/tags/$TAG:refs/tags/$TAG" && git checkout -q "$TAG")
fi

echo "== $LIBNAME ($TAG, features: $FEATURES)"
# The soname/install name must be the bare name: PPSSPP preloads the library by full path, then
# librashader_ld.h does its own bare-name dlopen, which only resolves to the already-loaded image
# if that is what the library calls itself (Common/GPU/Librashader/LibrashaderLoader.cpp).
if [ "$LIBNAME" = librashader.so ]; then
	( cd "$SRC" && RUSTFLAGS="-C link-arg=-Wl,-soname,librashader.so" \
	  cargo build -p librashader-capi --release --no-default-features --features "$FEATURES" )
else
	( cd "$SRC" && cargo build -p librashader-capi --release --no-default-features --features "$FEATURES" )
fi

mkdir -p "$OUT"
cp "$SRC/target/release/$ARTIFACT" "$OUT/$LIBNAME"

if [ "$LIBNAME" = librashader.dylib ]; then
	# cargo writes an absolute install name into target/release/deps, and install_name_tool
	# invalidates the linker's ad-hoc signature, so re-sign afterwards or macOS refuses to load it.
	install_name_tool -id librashader.dylib "$OUT/$LIBNAME"
	codesign -f -s - "$OUT/$LIBNAME"
	otool -D "$OUT/$LIBNAME" | tail -1 | grep -qx 'librashader.dylib' \
		|| { echo "error: install name is not librashader.dylib" >&2; exit 1; }
	SYMS=$(nm -gU "$OUT/$LIBNAME")
else
	if command -v readelf >/dev/null 2>&1; then
		readelf -d "$OUT/$LIBNAME" | grep -q 'SONAME.*\[librashader\.so\]' \
			|| { echo "error: soname is not librashader.so" >&2; exit 1; }
	else
		echo "warning: readelf not found, skipping soname verification" >&2
	fi
	SYMS=$(nm -gD --defined-only "$OUT/$LIBNAME")
fi

# A library missing a runtime still loads and reports success - every chain creation on that backend
# then fails instead - so check the entry points we actually call.
for sym in libra_vk_filter_chain_create libra_gl_filter_chain_create; do
	grep -q "$sym" <<< "$SYMS" || { echo "error: $LIBNAME is missing $sym" >&2; exit 1; }
done
n=$(grep -c 'libra_' <<< "$SYMS" || true)
[ "$n" -ge 40 ] || { echo "error: only $n libra_* exports in $LIBNAME" >&2; exit 1; }

echo "   ok: $OUT/$LIBNAME ($n exports, $(du -h "$OUT/$LIBNAME" | cut -f1))"
