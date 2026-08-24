# OpenLearnia DuckDB extension repository

This Worker serves the R2 bucket using DuckDB's custom repository layout:

```text
<version>/<platform>/<extension>.duckdb_extension
```

DuckDB's installer probes the gzipped object first, so publish both forms for
each artifact:

```text
<extension>.duckdb_extension.gz
<extension>.duckdb_extension
```

For example:

```text
v2.0.0-alpha38615/linux_amd64/ducklake.duckdb_extension
v2.0.0-alpha38615/linux_amd64/httpfs.duckdb_extension
v2.0.0-alpha38615/linux_amd64/postgres_scanner.duckdb_extension
```

## Create and deploy

After authenticating Wrangler with the OpenLearnia Cloudflare account:

```sh
wrangler r2 bucket create openlearnia-duckdb-extensions
wrangler deploy
```

Upload release assets with the filenames shown above. Keep the exact DuckDB
version, platform, and fork commit together; extension binaries are ABI-bound.

The publish workflow also writes a root manifest at
`/artifacts.json`. It describes the latest ABI-locked bundle and its R2 object
paths, including SHA-256 values for the runtime and extensions. The manifest is
short-cacheable (60 seconds); versioned extension objects remain immutable.

```sh
curl https://extensions.openlearnia.com/artifacts.json
```

Clients install from the Worker URL:

```sql
-- HTTP works without bootstrapping httpfs in the DuckDB preview build.
INSTALL ducklake FROM 'http://extensions.openlearnia.com';
LOAD ducklake;
```

HTTPS and S3 repositories require an ABI-matched `httpfs` extension first. Do
not use the public CDN `httpfs` binary with the OpenLearnia fork.

If the custom binaries are unsigned, clients must explicitly enable unsigned
extensions. Signing the release artifacts is recommended before production use.
