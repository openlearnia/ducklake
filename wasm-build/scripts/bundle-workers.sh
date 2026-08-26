#!/usr/bin/env bash
# Builds the DuckDB-Wasm JS package (bindings + browser workers) from the same
# harness source that produced the custom cores, then installs every runtime
# file into the playground. Running the workers through this repo's own bundler
# is mandatory: published npm workers are paired with a different core ABI.
#
# The release bundle is not safe for this custom core: esbuild's minifier drops
# Emscripten's dynamically addressed dynCall_* declarations while leaving the
# invoke wrappers that call them. Debug mode keeps those declarations intact.
#
# usage: bundle-workers.sh <duckdb-wasm-dir> <playground-dir>
set -euo pipefail

duckdb_wasm_dir=${1:?usage: bundle-workers.sh <duckdb-wasm-dir> <playground-dir>}
playground_dir=${2:?missing playground dir}

cd "$duckdb_wasm_dir"

if [[ ! -d node_modules ]]; then
  echo "Installing harness dependencies..."
  if command -v yarn >/dev/null; then
    yarn install --frozen-lockfile
  else
    npx --yes yarn@1.22.22 install --frozen-lockfile
  fi
fi

pkg="$duckdb_wasm_dir/packages/duckdb-wasm"
# The playground never builds the cross-origin-isolated pthread variant, but
# bundle.mjs patches its glue unconditionally - stub it when absent so the
# mvp/eh outputs still build.
for f in duckdb-coi.js duckdb-coi.pthread.js; do
  [[ -f "$pkg/src/bindings/$f" ]] || cp "$pkg/src/bindings/duckdb-eh.js" "$pkg/src/bindings/$f"
done
[[ -f "$pkg/src/bindings/duckdb-coi.wasm" ]] || cp "$pkg/src/bindings/duckdb-eh.wasm" "$pkg/src/bindings/duckdb-coi.wasm"

# The generated glue does not reliably retain the indirect-call wrappers for
# custom DuckLake signatures. Patch the source glue before esbuild sees it so
# the wrappers stay at module scope in the worker bundle.
node "$playground_dir/../wasm-build/scripts/fix-dyncalls.mjs" \
  "$pkg/src/bindings/duckdb-mvp.js" \
  "$pkg/src/bindings/duckdb-eh.js"

cd "$pkg"
node bundle.mjs debug

runtime_dir="$playground_dir/public/runtime"
cp dist/duckdb-browser-mvp.worker.js dist/duckdb-browser-eh.worker.js "$runtime_dir/"
for variant in mvp eh; do
  cp "src/bindings/duckdb-${variant}.js" "$runtime_dir/"
done
node "$playground_dir/../wasm-build/scripts/verify-dyncall-bundle.mjs" \
  "$runtime_dir/duckdb-browser-mvp.worker.js" \
  "$runtime_dir/duckdb-browser-eh.worker.js"
echo "Workers and glue installed into $runtime_dir"
