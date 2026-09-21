#!/usr/bin/env bash
# Build extensions and align the local extension repository to the current core.
#
# The extension repository layout is keyed by the duckdb source hash
# (build/release/repository/<hash>/<platform>/<name>.duckdb_extension). Every
# time the bundled duckdb submodule moves, previously built artifacts stop
# matching the new core's ABI gate. This script chains the standard build with
# the alignment steps so one command always yields a usable repository:
#
#   scripts/sync-extension-repo.sh [make args...]     # build + prune stale
#   KEEP_OLD_HASHES=1 scripts/sync-extension-repo.sh  # build, keep stale dirs
#
# Any environment (GEN, ENABLE_*) is passed through to `make release`.
set -euo pipefail
cd "$(dirname "$0")/.."

GEN="${GEN:-make}"
KEEP_OLD_HASHES="${KEEP_OLD_HASHES:-0}"

echo "== building extensions (GEN=$GEN) =="
GEN="$GEN" make release "$@"

REPO_DIR="build/release/repository"
CURRENT_HASH="$(git -C duckdb rev-parse --short=10 HEAD)"
PLATFORM_DIR="$(ls "$REPO_DIR/$CURRENT_HASH" 2>/dev/null | head -1 || true)"

if [[ -z "$PLATFORM_DIR" ]]; then
    echo "error: no artifacts for current core $CURRENT_HASH in $REPO_DIR" >&2
    echo "hint: the build may have produced a different version directory; check create_local_extension_repo output" >&2
    exit 1
fi

if [[ "$KEEP_OLD_HASHES" != "1" ]]; then
    for dir in "$REPO_DIR"/*/; do
        name="$(basename "$dir")"
        if [[ "$name" != "$CURRENT_HASH" ]]; then
            echo "== pruning stale $name (current core is $CURRENT_HASH) =="
            rm -rf "$dir"
        fi
    done
fi

echo
echo "aligned: $REPO_DIR/$CURRENT_HASH/$PLATFORM_DIR"
echo "install with:"
echo "  duckdb -unsigned -c \"INSTALL <name> FROM local_build_release; LOAD <name>;\"   # run from this directory"
echo "matching core version directory: $CURRENT_HASH"
