#!/usr/bin/env bash

set -euo pipefail

package_dir=${1:?usage: prepare-workers.sh <wasm-playground-dir>}
runtime_dir="$package_dir/public/runtime"
node_modules_dir="$package_dir/node_modules/@duckdb/duckdb-wasm/dist"

mkdir -p "$runtime_dir"
if [[ -f "$node_modules_dir/duckdb-browser-mvp.worker.js" ]]; then
  cp "$node_modules_dir/duckdb-browser-mvp.worker.js" "$runtime_dir/duckdb-browser-mvp.worker.js"
  cp "$node_modules_dir/duckdb-browser-eh.worker.js" "$runtime_dir/duckdb-browser-eh.worker.js"
else
  cp "$node_modules_dir/duckdb-browser.worker.js" "$runtime_dir/duckdb-browser-mvp.worker.js"
  cp "$node_modules_dir/duckdb-browser-next.worker.js" "$runtime_dir/duckdb-browser-eh.worker.js"
fi

test -s "$runtime_dir/duckdb-browser-mvp.worker.js"
test -s "$runtime_dir/duckdb-browser-eh.worker.js"
