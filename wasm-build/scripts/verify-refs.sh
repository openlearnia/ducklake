#!/usr/bin/env bash

set -euo pipefail

ducklake_dir=${1:?usage: verify-refs.sh <ducklake-dir>}
refs_file="$ducklake_dir/wasm-build/refs.env"

test -f "$refs_file"

configured_wasm_ref=${DUCKDB_WASM_REF:-}
configured_emscripten_version=${EMSDK_VERSION:-}
configured_vcpkg_commit=${VCPKG_COMMIT:-}

# shellcheck disable=SC1090
source "$refs_file"

: "${DUCKDB_WASM_REF:?DUCKDB_WASM_REF is missing from refs.env}"
: "${DUCKDB_WASM_NPM_VERSION:?DUCKDB_WASM_NPM_VERSION is missing from refs.env}"
: "${EMSDK_VERSION:?EMSDK_VERSION is missing from refs.env}"
: "${VCPKG_COMMIT:?VCPKG_COMMIT is missing from refs.env}"
: "${VCPKG_TARGET_TRIPLET:?VCPKG_TARGET_TRIPLET is missing from refs.env}"
: "${DUCKDB_REF_FILE:?DUCKDB_REF_FILE is missing from refs.env}"

if [[ -n "$configured_wasm_ref" && "$configured_wasm_ref" != "$DUCKDB_WASM_REF" ]]; then
  echo "Workflow DUCKDB_WASM_REF does not match wasm-build/refs.env" >&2
  exit 1
fi
if [[ -n "$configured_emscripten_version" && "$configured_emscripten_version" != "$EMSDK_VERSION" ]]; then
  echo "Workflow EMSDK_VERSION does not match wasm-build/refs.env" >&2
  exit 1
fi
if [[ -n "$configured_vcpkg_commit" && "$configured_vcpkg_commit" != "$VCPKG_COMMIT" ]]; then
  echo "Workflow VCPKG_COMMIT does not match wasm-build/refs.env" >&2
  exit 1
fi

duckdb_ref_file="$ducklake_dir/$DUCKDB_REF_FILE"
test -f "$duckdb_ref_file"
expected_duckdb_ref=$(tr -d '[:space:]' < "$duckdb_ref_file")
if [[ ! "$expected_duckdb_ref" =~ ^[0-9a-f]{40}$ ]]; then
  echo "Invalid custom DuckDB ref in $DUCKDB_REF_FILE: $expected_duckdb_ref" >&2
  exit 1
fi

actual_duckdb_ref=$(git -C "$ducklake_dir/duckdb" rev-parse HEAD)
if [[ "$actual_duckdb_ref" != "$expected_duckdb_ref" ]]; then
  echo "Custom DuckDB checkout does not match $DUCKDB_REF_FILE" >&2
  echo "expected: $expected_duckdb_ref" >&2
  echo "actual:   $actual_duckdb_ref" >&2
  exit 1
fi

printf 'WASM refs verified: duckdb-wasm=%s emsdk=%s vcpkg=%s duckdb=%s\n' \
  "$DUCKDB_WASM_REF" "$EMSDK_VERSION" "$VCPKG_COMMIT" "$actual_duckdb_ref"
