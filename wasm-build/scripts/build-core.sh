#!/usr/bin/env bash

set -euo pipefail

variant=${1:?usage: build-core.sh <mvp|eh> <duckdb-wasm-dir> <ducklake-dir> <output-dir>}
duckdb_wasm_dir=${2:?missing duckdb-wasm checkout}
ducklake_dir=${3:?missing ducklake checkout}
output_dir=${4:?missing output directory}

case "$variant" in
  mvp|eh) ;;
  *) echo "Unsupported WASM variant: $variant" >&2; exit 2 ;;
esac

for required in emcmake emmake patch; do
  command -v "$required" >/dev/null || { echo "Missing required command: $required" >&2; exit 1; }
done

test -f "$duckdb_wasm_dir/scripts/wasm_build_lib.sh"
test -f "$duckdb_wasm_dir/lib/cmake/duckdb.cmake"
test -d "$ducklake_dir/duckdb"
mkdir -p "$output_dir"

apply_patch_once() {
  local patch_file=$1
  if git -C "$duckdb_wasm_dir" apply --check "$patch_file"; then
    git -C "$duckdb_wasm_dir" apply "$patch_file"
  elif git -C "$duckdb_wasm_dir" apply --reverse --check "$patch_file"; then
    echo "Patch already applied: $patch_file"
  else
    echo "Patch does not apply cleanly: $patch_file" >&2
    exit 1
  fi
}

apply_patch_once "$ducklake_dir/wasm-build/duckdb-wasm-custom.patch"

export DUCKLAKE_SOURCE_DIR="$ducklake_dir"
export DUCKDB_WASM_LOADABLE_EXTENSIONS=1
export DUCKDB_EXTENSION_CONFIGS="$ducklake_dir/wasm-build/extension_config_wasm.cmake"
export DUCKDB_WASM_VERSION="openlearnia-ducklake-playground"

bash "$duckdb_wasm_dir/scripts/wasm_build_lib.sh" relperf "$variant" "$ducklake_dir/duckdb"

generated_dir="$duckdb_wasm_dir/packages/duckdb-wasm/src/bindings"
cp "$generated_dir/duckdb-${variant}.wasm" "$output_dir/duckdb-${variant}.wasm"
cp "$generated_dir/duckdb-${variant}.js" "$output_dir/duckdb-${variant}.js"

if [[ -f "$generated_dir/duckdb-${variant}.pthread.js" ]]; then
  cp "$generated_dir/duckdb-${variant}.pthread.js" "$output_dir/duckdb-${variant}.pthread.js"
fi

test -s "$output_dir/duckdb-${variant}.wasm"
echo "Built custom DuckLake $variant runtime: $output_dir/duckdb-${variant}.wasm"
