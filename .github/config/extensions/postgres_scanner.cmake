# postgres_scanner needs DONT_LINK because it depends on libpq/OpenSSL
if (NOT MINGW AND NOT ${WASM_ENABLED})
    duckdb_extension_load(postgres_scanner
            DONT_LINK
            GIT_URL https://github.com/duckdb/duckdb-postgres
            GIT_TAG 8422ea363de8f599bc6e9ce3685051c3713a7c01
            SUBMODULES "database-connector"
            APPLY_PATCHES
            )
endif()
