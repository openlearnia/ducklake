#!/usr/bin/env bash
# Build extensions and align the local extension repository to the current core.
#
# The extension repository layout is keyed by the duckdb source hash
# (build/release/repository/<hash>/<platform>/<name>.duckdb_extension). Every
# time the bundled duckdb submodule moves:
#   - previously built artifacts stop matching the new core's ABI gate, and
#   - external (FetchContent) extension builds do NOT notice the core change,
#     so a plain re-make copies stale binaries into the fresh hash directory.
# This script chains the standard build with the alignment steps so one command
# always yields a correct, current repository:
#
#   scripts/sync-extension-repo.sh [ENABLE_X=1 ...]   # build + prune stale
#   KEEP_OLD_HASHES=1 scripts/sync-extension-repo.sh  # build, keep stale dirs
#   WITH_SHELL=1 scripts/sync-extension-repo.sh       # also build matching shell
#
# Environment (GEN, ENABLE_*, ...) is passed through to `make release`.
set -euo pipefail
cd "$(dirname "$0")/.."

GEN="${GEN:-make}"
KEEP_OLD_HASHES="${KEEP_OLD_HASHES:-0}"
WITH_SHELL="${WITH_SHELL:-0}"

CURRENT_HASH="$(git -C duckdb rev-parse --short=10 HEAD)"
STAMP="build/release/repository/.built-for"
PREV_HASH="$(cat "$STAMP" 2>/dev/null || echo "")"

if [[ "$PREV_HASH" != "$CURRENT_HASH" ]]; then
    if [[ -n "$PREV_HASH" ]]; then
        echo "== core moved $PREV_HASH -> $CURRENT_HASH; forcing extension rebuilds =="
    fi
    rm -rf build/release/_deps/*_fc-*
fi

echo "== building extensions (GEN=$GEN) =="
GEN="$GEN" make release

echo "$CURRENT_HASH" > "$STAMP"

REPO_DIR="build/release/repository"
PLATFORM_DIR="$(ls "$REPO_DIR/$CURRENT_HASH" 2>/dev/null | head -1 || true)"
if [[ -z "$PLATFORM_DIR" ]]; then
    echo "error: no artifacts for current core $CURRENT_HASH in $REPO_DIR" >&2
    exit 1
fi

if [[ "$KEEP_OLD_HASHES" != "1" ]]; then
    for dir in "$REPO_DIR"/*/; do
        name="$(basename "$dir")"
        if [[ "$name" != "$CURRENT_HASH" ]]; then
            echo "== pruning stale $name =="
            rm -rf "$dir"
        fi
    done
fi

if [[ "$WITH_SHELL" == "1" ]]; then
    echo "== building matching shell =="
    cmake -S duckdb -B duckdb/build/shell-aligned -DCMAKE_BUILD_TYPE=Release > /dev/null
    cmake --build duckdb/build/shell-aligned --parallel "$(sysctl -n hw.ncpu)" --target shell > /dev/null
    SHELL_BIN="duckdb/build/shell-aligned/duckdb"
else
    SHELL_BIN="<a duckdb shell built from $CURRENT_HASH>"
fi

echo
echo "aligned: $REPO_DIR/$CURRENT_HASH/$PLATFORM_DIR"
echo "install with (from this directory):"
echo "  $SHELL_BIN -unsigned -c \"INSTALL <name> FROM local_build_release; LOAD <name>;\""
