# DuckLake — OpenLearnia fork

Fork of [duckdb/ducklake](https://github.com/duckdb/ducklake), the SQL lakehouse
catalog extension, paired with our [DuckDB fork](https://github.com/openlearnia/duckdb)
and shipped as the **Grain bundle**: signed, version-locked Linux builds
(amd64 + arm64) of `ducklake`, `postgres_scanner`, `httpfs`, and `quack`.

- **Branch:** `ducklake-2-preview-port`
- **Upstream base:** ducklake `main` @ `620150b1`, DuckDB `v2.0-cyanoptera`
- **Runtime version:** `v2.0.0-alpha42376` (paired core: `sync/v2-cyanoptera-alpha42376`)
- **Latest release:** [releases](https://github.com/openlearnia/ducklake/releases)

## What's in this fork

### Catalog replication

Replicate a live catalog — native DuckDB or DuckLake — into a DuckLake
destination, manually or under a background supervisor.

```sql
ATTACH 'ducklake:postgres:host=... dbname=meta' AS replica (DATA_PATH 's3://...');

-- create a job, then sync on demand...
SELECT replication_id FROM ducklake_replicate_create('replica', 'prod');
SELECT * FROM ducklake_replicate_catchup('replica', 1);

-- ...or run continuously
SELECT * FROM ducklake_replicate_create('replica', 'prod', interval_ms := 200);
SELECT * FROM ducklake_replicate_start('replica');
```

- **Two sync modes.** `copy` duplicates data files into destination storage;
  `data_sync_mode := 'share'` registers the *same physical files* in the
  destination catalog — zero bytes copied — while reconciling additions,
  removals, delete files, compaction output, and source table drops.
- **Four incremental strategies**, chosen per table: `snapshot` (DuckLake
  snapshot deltas), `merge` (primary-key diff), `watermark` (append-only
  cursor column), `full` (fallback). Filters:
  `include := 'app.*', exclude := 'app.staging_*'`.
- **Supervisor lifecycle.** One background supervisor per database + catalog:
  interval scheduling with exponential backoff, pause/resume/stop, error
  isolation per table, auto-recovery when a source reattaches, and clean
  teardown — supervisors are interrupted and joined before the owning
  `DuckDB` instance is destroyed, so embedded deployments exit cleanly.
- **Observable.** `ducklake_replicate_status` reports per-job state, lag
  (`snapshots_behind`, `lag_ms`), and `last_error`;
  `ducklake_replicate_tables` reports per-table strategy, cursors, and row
  counts.

API: `ducklake_replicate_create`, `ducklake_replicate_catchup`,
`ducklake_replicate_start`, `ducklake_replicate_stop`,
`ducklake_replicate_pause`, `ducklake_replicate_resume`,
`ducklake_replicate_resume_all`, `ducklake_replicate_status`,
`ducklake_replicate_tables`, `ducklake_replicate_drop`.

### Materialized views

Native `CREATE MATERIALIZED VIEW` / `REFRESH MATERIALIZED VIEW` /
`DROP MATERIALIZED VIEW` on DuckLake catalogs, with incremental maintenance:

- **Refresh modes:** `skip`, `delta` (signed SUM/COUNT CDC, derived AVG,
  conditional MIN/MAX with extremum-invalidation), `incremental`
  (changed-key recompute), `join_incremental` (fact-side INNER equijoins;
  dimension changes fall back to full).
- **Staleness control:** `REFRESH MATERIALIZED VIEW IF STALE`,
  `is_stale` listing, and `SET ducklake_mv_stale_read = allow|warn|error`
  governing reads of stale views.
- **Introspection:** `ducklake_materialized_views()` and
  `ducklake_materialized_view_refresh_history()` expose definitions,
  dependencies, and per-refresh metrics (rows scanned/written, duration).

### JavaScript stored procedures

Persisted procedures executed on a quickjs-ng runtime embedded in the core,
with full SQL access through a `duckdb` global:

```sql
CREATE PROCEDURE append_and_count(value INTEGER)
RETURNS INTEGER
LANGUAGE JAVASCRIPT
AS $$
  await duckdb.execute('INSERT INTO t VALUES ($1)', [value]);
  const rows = await duckdb.query('SELECT count(*) AS c FROM t');
  return rows[0].c;
$$;

CALL append_and_count(7);
```

Async (`await`, `Promise.all`), parameterized statements, and transactional
semantics are supported; procedure bodies persist in catalog metadata.

### Vortex data files (preview)

[Vortex](https://github.com/vortex-data/vortex) as a managed-table data-file
format alongside Parquet:

- `data_file_format := 'vortex'` on catalog/table options
  (`ducklake_default_data_file_format` for catalog-wide default).
- Managed-table parity: insert, scan, positional deletes, compaction,
  encryption, snapshot isolation, time travel — through a MultiFileReader
  adapter that keeps DuckLake's delete/snapshot filters working.
- External `.vortex` files register via `ducklake_add_data_files` using
  footer-only `vortex_full_metadata`, with the same Hive-partition and
  type-check coverage as Parquet.

## Install

Release assets are signed for the paired runtime (`v2.0.0-alpha42376`):

```bash
gh release download <tag> -R openlearnia/ducklake \
  -p 'duckdb-*-linux_arm64.tar.gz' -p 'ducklake-*-linux_arm64.duckdb_extension'
```

Or install straight from the hosted extension repository:

```sql
SET custom_extension_repository =
    'https://openlearnia-duckdb-extension-repository.defaultcms.workers.dev';
INSTALL ducklake;
LOAD ducklake;
```

The paired `duckdb` runtime also statically links `ducklake`, `httpfs`, and
`quack` — `LOAD` works with nothing installed.

## Linked repositories

| Repo | Role |
|---|---|
| [openlearnia/ducklake](https://github.com/openlearnia/ducklake) | This repo — the extension + replication/MV/procedure surface |
| [openlearnia/duckdb](https://github.com/openlearnia/duckdb) | Paired core (native MV engine, JS procedure runtime, teardown hooks) |
| [openlearnia/vortex](https://github.com/openlearnia/vortex) | Vortex columnar format, ingest-tuned for DuckLake |

## Status

Everything above is tested: the replication suite covers copy/share,
incremental strategies, lifecycle, isolation, and error paths (384
assertions); materialized views pass 542 assertions under both Parquet and
Vortex formats. PostgreSQL-backed metadata is exercised against two live
instances with concurrent supervisors, writer churn, and a Go/`duckdb-go`
HTTP embedding.

Fork CI debt unrelated to these features: the upstream backwards-compat job
(vcpkg/`libpq` toolchain) and parts of the core `Main` workflow
(tidy/relassert deprecation warnings) are red on all commits.
