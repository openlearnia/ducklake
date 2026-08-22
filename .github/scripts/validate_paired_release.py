#!/usr/bin/env python3

from pathlib import Path
import re
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]
CALLER = ROOT / ".github/workflows/GrainExtensionPublish.yml"
REUSABLE = ROOT / ".github/workflows/_grain_extension_distribution.yml"
REF_FILE = ROOT / ".github/duckdb-ref"


def fail(message: str) -> None:
    print(f"paired release validation failed: {message}", file=sys.stderr)
    raise SystemExit(1)


if not REF_FILE.is_file():
    fail(".github/duckdb-ref is missing")

duckdb_ref = REF_FILE.read_text().strip()
if not re.fullmatch(r"[0-9a-f]{40}", duckdb_ref):
    fail(".github/duckdb-ref must contain one full lowercase commit SHA")

submodule_ref = subprocess.check_output(
    ["git", "-C", str(ROOT / "duckdb"), "rev-parse", "HEAD"], text=True
).strip()
if duckdb_ref != submodule_ref:
    fail(f"DuckDB ref {duckdb_ref} does not match submodule HEAD {submodule_ref}")

caller = CALLER.read_text()
reusable = REUSABLE.read_text()
required_caller_fragments = (
    "override_duckdb_repository: https://github.com/openlearnia/duckdb.git",
    "duckdb_version: ${{ needs.get-duckdb-version.outputs.duckdb_ref }}",
    "upload_duckdb_binaries: true",
    "chmod +x duckdb-runtime/bin/duckdb",
)
for fragment in required_caller_fragments:
    if fragment not in caller:
        fail(f"caller workflow is missing: {fragment}")

if "upload_duckdb_binaries:" not in reusable:
    fail("reusable workflow has no paired DuckDB binary output contract")
if "duckdb-${{ inputs.duckdb_version }}-runtime-${{matrix.duckdb_arch}}" not in reusable:
    fail("reusable workflow does not upload the pinned DuckDB runtime artifact")

print(f"paired release contract valid for DuckDB {duckdb_ref}")
