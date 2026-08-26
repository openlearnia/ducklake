# DuckLake's browser build is intentionally self-contained. The demo uses
# inlined rows, so native-only network extensions and external catalogs are
# not part of this artifact.
if(NOT DEFINED ENV{DUCKLAKE_SOURCE_DIR})
  message(FATAL_ERROR "DUCKLAKE_SOURCE_DIR must point at the DuckLake checkout")
endif()

duckdb_extension_load(ducklake
  SOURCE_DIR "$ENV{DUCKLAKE_SOURCE_DIR}"
)
