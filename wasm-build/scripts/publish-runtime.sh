#!/usr/bin/env bash
# Uploads the compiled DuckDB-WASM cores to the R2 bucket that backs the
# playground's /runtime/ route. The cores exceed the 25 MiB Workers static
# asset cap, so they must live in R2 instead of the deployed asset bundle.
#
# usage: publish-runtime.sh <runtime-dir> [bucket]
set -euo pipefail

runtime_dir=${1:?usage: publish-runtime.sh <runtime-dir> [bucket]}
bucket=${2:-openlearnia-ducklake-wasm-runtime}

shopt -s nullglob
wasm_files=("$runtime_dir"/*.wasm)
if [[ ${#wasm_files[@]} -eq 0 ]]; then
  echo "No .wasm files found in $runtime_dir" >&2
  exit 1
fi

for wasm_file in "${wasm_files[@]}"; do
  name=$(basename "$wasm_file")
  echo "Uploading $name to r2:$bucket/$name"
  npx --yes wrangler@4.76.0 r2 object put "$bucket/$name" \
    --file "$wasm_file" \
    --content-type application/wasm \
    --remote
done
echo "Runtime cores published to r2:$bucket"
