# postgres_scanner needs DONT_LINK because it depends on libpq/OpenSSL
if (NOT MINGW AND NOT ${WASM_ENABLED})
    duckdb_extension_load(postgres_scanner
            DONT_LINK
            GIT_URL https://github.com/duckdb/duckdb-postgres
            GIT_TAG c91ea5779322c97dfff1940f67b3a4d5b6a1e07e
            SUBMODULES "database-connector"
            APPLY_PATCHES
            )
endif()
