#!/usr/bin/env sh
# Build the rpcs3 core + the CellStation macOS bridge/harness natively for
# arm64. Mirrors ci/build-local.sh (Android) but targets the host Mac.
#
# Prereqs (Homebrew): cmake ninja ccache llvm qt ffmpeg  (see macos/README.md)
#   - Homebrew LLVM clang (>= 19) is required; Apple clang is too old for rpcs3.
#   - Qt6 is a configure-time-only dependency of rpcs3's monolithic CMake; we
#     build only rpcs3_emu + our bridge, which link no Qt.
#
# Overridable env:
#   BUILD_DIR   cmake build dir  (default: build-macos)
#   BUILD_TYPE  cmake build type (default: RelWithDebInfo)
#   LLVM_PREFIX Homebrew llvm     (default: $(brew --prefix llvm))
#   TARGET      build target      (default: cellstation_harness)
set -eu

cd "$(dirname "$0")/.."   # repo root (parent of macos/)

BUILD_DIR="${BUILD_DIR:-build-macos}"
BUILD_TYPE="${BUILD_TYPE:-RelWithDebInfo}"
LLVM_PREFIX="${LLVM_PREFIX:-$(brew --prefix llvm)}"
TARGET="${TARGET:-cellstation_harness}"

CC="$LLVM_PREFIX/bin/clang"
CXX="$LLVM_PREFIX/bin/clang++"
[ -x "$CC" ] || { echo "error: Homebrew clang not found at $CC (brew install llvm)" >&2; exit 1; }

# ---- reset + patch the submodule (apply-patches.sh is not idempotent) --------
git -C rpcs3 checkout -- . 2>/dev/null || true
git -C rpcs3 clean -fdq 2>/dev/null || true
sh ci/apply-patches.sh

# macOS-only patches applied ON TOP of the shared series (kept out of the shared
# patches/ so the Android build is untouched). Currently: gate pad_thread's Qt
# keyboard handler behind CELLSTATION_NO_QT_KEYBOARD (we build no Qt).
for p in macos/patches/*.patch; do
    [ -f "$p" ] || continue
    echo "Applying $p"
    git -C rpcs3 apply --verbose "../$p"
done

# ---- configure ---------------------------------------------------------------
echo "==> Configuring ($BUILD_DIR) with $($CXX --version | head -1)"
cmake -S macos -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_C_COMPILER="$CC" \
    -DCMAKE_CXX_COMPILER="$CXX" \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache \
    -DCMAKE_CXX_COMPILER_LAUNCHER=ccache

# ---- build -------------------------------------------------------------------
echo "==> Building $TARGET"
cmake --build "$BUILD_DIR" --target "$TARGET" -j"$(sysctl -n hw.ncpu)"
echo "==> Done: $BUILD_DIR/bridge/$TARGET"
