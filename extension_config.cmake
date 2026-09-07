# This file is included by DuckDB's build system. It specifies which extension to load

# Extension from this repo
duckdb_extension_load(ducklake
        SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
)

if(DEFINED ENV{DUCKLAKE_VORTEX_EXTENSION_DIR})
    duckdb_extension_load(vortex
            SOURCE_DIR $ENV{DUCKLAKE_VORTEX_EXTENSION_DIR}
            LOAD_TESTS
    )
endif()

if(NOT DEFINED ENV{DISABLE_EXTENSIONS_FOR_TEST})
    duckdb_extension_load(icu)
    duckdb_extension_load(json)
    duckdb_extension_load(tpch)
endif()

set(EXTENSION_CONFIG_BASE_DIR "${CMAKE_CURRENT_LIST_DIR}/.github/config/extensions/")
if($ENV{ENABLE_SQLITE_SCANNER})
    include("${EXTENSION_CONFIG_BASE_DIR}/sqlite_scanner.cmake")
endif()

if($ENV{ENABLE_POSTGRES_SCANNER})
    include("${EXTENSION_CONFIG_BASE_DIR}/postgres_scanner.cmake")
endif()

# Build network extensions against this exact DuckDB checkout. Never mix the
# preview fork with CDN binaries built for another ABI.
if($ENV{ENABLE_GRAIN_BUNDLE_EXTENSIONS})
    duckdb_extension_load(httpfs
            GIT_URL https://github.com/duckdb/duckdb-httpfs
            # Upstream pin (SigV4 moved into core); matches the duckdb
            # submodule's .github/config/extensions/httpfs.cmake.
            GIT_TAG fafb14f2c899ddfd1998f8adf2e07fbbfd28b3fd
            APPLY_PATCHES
    )
endif()

if($ENV{ENABLE_QUACK})
    include_directories(
            ${CMAKE_CURRENT_LIST_DIR}/duckdb/third_party/httplib
            ${CMAKE_CURRENT_LIST_DIR}/duckdb/extension/autocomplete/include
    )
    duckdb_extension_load(quack
            LOAD_TESTS
            # openlearnia fork: b2f2d10 ported to the cyanoptera v2.0.0-alpha
            # core APIs (Identifier/QualifiedName), RPC behavior unchanged.
            GIT_URL https://github.com/openlearnia/duckdb-quack.git
            GIT_TAG e51180739003868dad6c2d056dcaeeebd7f685bb
            SUBMODULES "extension-ci-tools"
            APPLY_PATCHES
    )
endif()
