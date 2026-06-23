#!/bin/bash
# rampart-iroh/docker/build-in-oven.sh <stage> -- runs INSIDE the iroh oven.
# Invoke via docker/build.sh, not directly.
#
# stage = build | install
#
# Builds rampart-iroh.so (Rust cdylib/staticlib + C shim) against the
# bind-mounted /usr/local/rampart-ml (rampart.h + the rampart binary used to
# resolve $RAMPART_INCLUDE).  Oven artifacts go under build/oven/ so the native
# target/ and the repo-root rampart-iroh.so are left untouched.
set -euo pipefail

STAGE="${1:-build}"
IROH=/iroh
BUILD=$IROH/build/oven
PREFIX="${RAMPART_PREFIX:-/usr/local/rampart-ml}"

# rampart on PATH so the Makefile's `rampart -c process.installPath` resolves the
# include dir; it's glibc 2.17, so it runs in this oven.
export PATH="$PREFIX/bin:$PATH"
# Keep the crate cache + build tree inside the mounted repo (gitignored build/):
# cache persists across runs, and we never touch the host's native target/.
export CARGO_HOME="$IROH/build/cargo-home"
export CARGO_TARGET_DIR="$BUILD/target"

enable_toolchain() {
    set +u
    # one of these globs won't match (only devtoolset OR gcc-toolset is present);
    # ls then exits non-zero -- tolerate it (|| true) so pipefail doesn't abort.
    sc=$(ls /opt/rh/gcc-toolset-*/enable /opt/rh/devtoolset-*/enable 2>/dev/null | sort -V | tail -1) || true
    [ -n "$sc" ] && source "$sc"
    set -u
}

case "$STAGE" in
  build)
    enable_toolchain
    echo "==> toolchain: $(gcc --version | head -1)"
    echo "==> rust: $(rustc --version)  cargo: $(cargo --version)"
    command -v rampart >/dev/null || { echo "rampart not on PATH (mount /usr/local/rampart-ml)" >&2; exit 1; }
    git config --global --add safe.directory '*' 2>/dev/null || true
    mkdir -p "$BUILD"
    # cargo respects CARGO_TARGET_DIR; the Makefile's TARGET_DIR/MODULE are
    # pointed into build/oven/ to match, so nothing lands in target/ or the repo
    # root.  --locked (in the Makefile's `lib` rule) pins Cargo.lock verbatim.
    make -C "$IROH" \
        TARGET_DIR=build/oven/target/release \
        MODULE=build/oven/rampart-iroh.so \
        lib module
    echo
    ls -l "$BUILD/rampart-iroh.so"
    echo "==> iroh build OK"
    ;;

  install)
    enable_toolchain   # for a matching `strip`
    [ -f "$BUILD/rampart-iroh.so" ] || {
        echo "no build at $BUILD/rampart-iroh.so -- run 'docker/build.sh build' first" >&2; exit 1; }
    install -d "$PREFIX/modules"
    install -m 755 "$BUILD/rampart-iroh.so" "$PREFIX/modules/"
    strip -S "$PREFIX/modules/rampart-iroh.so"
    if [ -d "$PREFIX/test" ]; then
        install -m 644 "$IROH/iroh-test.js" "$PREFIX/test/" && echo "installed iroh-test.js"
    fi
    if [ -d "$PREFIX/licenses" ]; then
        cp "$IROH"/licenses/* "$PREFIX/licenses/" 2>/dev/null && echo "installed license files" || true
    fi
    echo
    ls -l "$PREFIX/modules/rampart-iroh.so"
    echo "==> iroh install OK"
    ;;

  *)
    echo "unknown stage: $STAGE  (expected: build | install)" >&2
    exit 1
    ;;
esac
