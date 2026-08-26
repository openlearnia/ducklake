# Plan: Definition Versioning for Materialized Views & Stored Procedures

**Date:** 2026-08-26
**Branch:** `ducklake-2-preview-port`
**Status:** Proposal (planning only — nothing implemented)
**Scope:** DuckLake metadata catalog: `ducklake_materialized_view*` and `ducklake_procedure*` tables

---

## 1. Problem

MVs and stored procedures store their definitions in the DuckLake metadata catalog
(`sql`/`body`, parameters, dependencies, refresh state). As this fork evolves those
definitions (new columns, changed SQL conventions, parameter-model changes, dialect
handling), there is currently **no way to know which "shape"/semantics a stored
definition was written with**, and therefore no way to either:

- migrate stored MVs/SPs to a new definition format, or
- keep reading old-format definitions compatibly.

Today the only version signal is the whole-catalog `ducklake_metadata.version`
(currently `'1.2'`), which gates attach but says nothing about individual objects.

## 2. Current state (verified)

| Area | Where | Notes |
| --- | --- | --- |
| Catalog version tag | `ducklake_metadata(key,value,...)` `'version'` | Fresh lakes initialize at `'1.2'` (`InitializeDuckLake`, `ducklake_metadata_manager.cpp:246`) |
| Attach-time gate + migration chain | `LoadExistingDuckLake`, `ducklake_initializer.cpp:155-209` | Hard-coded `"1.2"` check; refuses unless `AUTOMATIC_MIGRATION TRUE`; walks `MigrateV01..MigrateV06` |
| Migration helpers | `ducklake_metadata_manager.cpp` | `ExecuteMigration()` supports `{IF_NOT_EXISTS}`/`{IF_EXISTS}`/`{WHERE_EMPTY}` placeholders for tolerant dev-version migrations |
| MV tables | `ducklake_materialized_view` (10 cols), `_dependency`, `_refresh_history` (13 cols) | Created at init; `MigrateV05` (1.0→1.1) creates MV+dependency for old lakes |
| Refresh-history table | `EnsureMaterializedViewRefreshHistoryTable()` called at attach (`ducklake_initializer.cpp:208`) | **Out-of-band** — not part of the versioned migration chain; skipped for read-only attaches |
| SP tables | `ducklake_procedure`, `ducklake_procedure_parameters` | Created at init; `MigrateV06` (1.1→1.2) for old lakes |
| MV writer | `WriteNewMaterializedViews` (`:3060+`) | Uses **column-less** `INSERT INTO ... VALUES (...)` |
| SP writer | `WriteNewProcedures` (`:2976+`) | Also **column-less** INSERT |
| Readers | `LoadMaterializedViews` (`:3147`), procedure loader (`:962`) | Explicit column-list SELECTs (safe against appended columns) |
| Info structs | `DuckLakeMaterializedViewInfo` (`ducklake_metadata_info.hpp:357`), `DuckLakeProcedureInfo` (`:137`) | **No version field** |
| Rows are snapshot-versioned | `begin_snapshot`/`end_snapshot` on all rows | Historical rows must stay readable at old snapshots |

Hard-coded `'1.2'` appears in three places (init INSERT, initializer check, mismatch
error message) — a drift hazard this work should fix with constants.

## 3. Design

### 3.1 Per-kind definition versions (recommended)

Add one nullable `BIGINT` column per object table:

```sql
ALTER TABLE {METADATA_CATALOG}.ducklake_materialized_view ADD COLUMN IF NOT EXISTS definition_version BIGINT;
ALTER TABLE {METADATA_CATALOG}.ducklake_procedure       ADD COLUMN IF NOT EXISTS definition_version BIGINT;
-- backfill legacy rows
UPDATE {METADATA_CATALOG}.ducklake_materialized_view SET definition_version = 1 WHERE definition_version IS NULL;
UPDATE {METADATA_CATALOG}.ducklake_procedure       SET definition_version = 1 WHERE definition_version IS NULL;
```

Semantics: `definition_version = N` means "this row's `sql`/`body`/parameters/deps
follow definition-contract vN". **v1 = the shape that ships today.** Readers treat
`NULL` as `1` (defensive, pre-migration safety).

Why this over alternatives:

- **vs one shared counter for MV+SP:** they evolve independently; an SP parameter-model
  change shouldn't force MV rewrites. Two independent counters avoid lockstep bumps.
- **vs a scoped tag in `ducklake_metadata`:** the existing scope machinery ('schema'/'table')
  is global-per-scope; it cannot say *"this object is v2 but that one is still v1"*,
  and it doesn't time-travel with rows. A per-row column does both.
- Naming avoids `schema_version`, which in DuckLake already means table-schema versions
  (`ducklake_schema_versions`) — a collision we don't want.

Parameter rows (`ducklake_procedure_parameters`, `ducklake_materialized_view_dependency`)
inherit the parent row's version — no separate column needed.

### 3.2 Catalog version bump 1.2 → 1.3

Follow the established pattern exactly:

- `MigrateV07()`: the two ALTER/UPDATE pairs above, plus fold
  `EnsureMaterializedViewRefreshHistoryTable()` into the chain so refresh history
  becomes formally versioned instead of out-of-band; ends with
  `UPDATE ducklake_metadata SET value='1.3' WHERE key='version'`.
- `InitializeDuckLake()`: new lakes create the columns inline and insert `'version','1.3'`.
- Initializer chain gains `if (version == "1.2") { MigrateV07(); version="1.3"; }` and the
  final guard becomes `!= "1.3"`.
- Introduce constants (e.g. `kCurrentCatalogVersion = "1.3"`,
  `kMinSupportedCatalogVersion`, `kDefaultDefinitionVersion = 1`) and replace the three
  hard-coded `'1.2'` strings.

Read-only attach keeps its current contract: it can't migrate, so it skips table-creating
steps (same rule as today's refresh-history skip at `ducklake_initializer.cpp:205-209`).

### 3.3 C++ plumbing

- `DuckLakeMaterializedViewInfo` / `DuckLakeProcedureInfo` gain a
  `uint64_t definition_version` field (default 1).
- Writers stamp the current constant; **both column-less INSERTs switch to explicit
  column lists** (required once columns exist).
- Loaders `COALESCE(definition_version, 1)`; on load, if `version > kSupported`,
  throw a clear error naming the object and the required extension version
  ("materialized view 'x' uses definition version 2; this build supports up to 1 —
  upgrade the ducklake extension").

### 3.4 Compatibility matrix (the "migrate or keep compatible" answer)

| Scenario | Behavior |
| --- | --- |
| Old catalog (≤1.2) attached to new build | Unchanged upstream contract: refuse unless `AUTOMATIC_MIGRATION TRUE`, then `MigrateV07` upgrades; all objects become v1 |
| New catalog (1.3) attached to old build | Refuses at the existing `"1.2"` gate (forward-compat story: don't downgrade) |
| Object with unsupported future definition version | Hard error on load (strict). Optional later: ATTACH/SET `DEFINITION_VERSION_POLICY={'strict'\|'compatible'}` where `compatible` best-effort loads with the oldest reader — deferred until a real v2 exists so "best effort" is definable |
| Time travel to pre-migration snapshots | Safe: the new column is additive; historical rows read as v1 |
| Re-attach after migration | Idempotent: version already 1.3, chain no-ops |

### 3.5 Object-level migration framework (phase 3)

For future *breaking* definition changes (not needed until a real v2 lands):

- A registry of steps per kind: `{kind, from_version, to_version, rewrite_fn}` where
  `rewrite_fn` transforms sql/body/parameters/deps.
- **Eager** execution inside the catalog migration for pure-metadata changes.
- **Lazy** execution for SQL-text changes, at natural rewrite points so history stays
  immutable: MVs upgrade during `REFRESH`/`ducklake_refresh_materialized_view` (definition
  re-stamped atomically with the refresh commit); SPs upgrade on `CREATE OR REPLACE`.
- Optional convenience: `CALL ducklake_migrate_definitions('ducklake')` to force-upgrade
  all current rows by opening new snapshot rows (never mutating `end_snapshot`-closed history).

## 4. Multi-backend notes

Migrations are plain SQL executed through each metadata manager (DuckDB/SQLite/Postgres/
MySQL/Quack). `ADD COLUMN IF NOT EXISTS` support varies (Postgres ≥9.6 yes; SQLite/MySQL
no). Reuse the existing `ExecuteMigration` `{IF_NOT_EXISTS}` placeholder machinery rather
than raw `IF NOT EXISTS` in `MigrateV07`, and test the migration on every backend CI covers.

## 5. Test plan

- Fixtures: generate `data/old_ducklake/v12.db.gz`-style catalogs containing MVs (with
  dependencies + refresh-history rows) and procedures (with parameters) at version 1.2.
- Extend `test/sql/migration/`: default-refuse error; `AUTOMATIC_MIGRATION TRUE` success;
  MV/SP readable and refreshable/callable post-migrate; `definition_version` stamped;
  detach/re-attach idempotency; time-travel query at a pre-migration snapshot; read-only
  attach leaves catalog unmigrated.
- Hand-crafted row with `definition_version = 99` → clean error message.
- Per-backend migration coverage where CI runs backend-specific suites.

## 6. Rollout phases

1. **Phase 0 — constants refactor:** single source of truth for current catalog version;
   replace the three `'1.2'` literals. Low-risk prep.
2. **Phase 1 — plumbing:** info-struct fields, explicit-column INSERTs, `COALESCE` reads,
   unsupported-version errors. Behavior-preserving on 1.2 catalogs (readers fall back to 1).
3. **Phase 2 — version bump:** `MigrateV07`, init DDL `'1.3'`, initializer chain, fold in
   refresh-history ensure, fixtures + tests + doc updates.
4. **Phase 3 — (deferred) object-migration framework + policy knob** when the first real v2
   definition change arrives.

## 7. Open questions (need a decision before implementation)

1. ~~Column name~~ **Decided:** `definition_version`.
2. Separate MV and SP counters (recommended) vs one shared counter?
3. Fold `EnsureMaterializedViewRefreshHistoryTable` into `MigrateV07` (recommended) or leave
   out-of-band?
4. Ship the strict-only policy now and defer `DEFINITION_VERSION_POLICY='compatible'` until a
   real v2 exists (recommended)?
5. While in the area: the schema-level orphan cleanup list includes `ducklake_view`/`macro`/
   `procedure` but not the MV tables — confirm MV cleanup is fully covered elsewhere
   (`FlushDrop` on commit) or add them.

## 8. Implementation review (2026-08-26)

### Materialized views

All statement handling lives in `src/functions/ducklake_materialized_view.cpp` (≈2,020 lines)
plus a parser extension (`ducklake_materialized_view_parser.cpp`) that intercepts
catalog-qualified `CREATE/REFRESH/DROP MATERIALIZED VIEW` and rewrites them into calls to the
`ducklake_create/refresh/drop_materialized_view` table functions (unqualified names fall
through to native DuckDB MVs; `IF NOT EXISTS`/`IF EXISTS` are explicitly rejected).

- **CREATE**: parses a single SELECT, runs an eligibility analysis (`AnalyzeMaterializedView`)
  restricting incremental support to one base table or one INNER equijoin, mandatory GROUP BY,
  and SUM/COUNT/MIN/MAX/AVG aggregates without DISTINCT/FILTER/ORDER-BY-in-aggregate/windows/
  subqueries. Base refs are qualified into the lake and dependency table ids recorded. The
  stored `sql` replaces the lake name with a `{DUCKLAKE_CATALOG}.` placeholder. A backing table
  is created under the *logical MV name* (`catalog_materialized_view` flag) with a private
  UUID-derived path; initial files flow through copy-to-file → the `DuckLakeMVRefresh` physical
  operator, which retires prior backing files and appends new ones atomically in the commit.
- **REFRESH** selects a mode per run against snapshot CDC (`ducklake_table_insertions/
  deletions` over `[last_refreshed+1, current]`): `skipped`, `delta` (algebraic SUM/COUNT,
  AVG derived from matching SUM/COUNT state, COUNT(\*) guards group liveness),
  conditional-delta for MIN/MAX (per-key rebuild when an extremum row is deleted),
  `incremental` (recompute changed keys), `join_incremental` (fact-only CDC; any dim change
  forces full), or `full`. A logical row-diff query feeds rows_added/removed/changed metrics;
  history rows are written in the same commit as the refresh.
- **Catalog load** (`ducklake_catalog.cpp:551+`): backing tables are projected under the MV's
  logical name; older catalogs that persisted UUID-derived names are corrected at load time
  without a metadata rewrite.
- **Listing**: `ducklake_materialized_views` computes `is_stale` per view via the dependency
  change feed; `ducklake_materialized_view_refresh_history` tries the extended 14-column
  contract and falls back to the original 6-column contract when the metric columns are absent
  (read-only older attach).

Findings:

1. **Column-dedupe edge case** (`CreateMaterializedViewBind`, ≈line 1024): after the first
   collision the retry loop rebuilds candidates from the raw `name`, so for auto-named
   `"unnamed"` columns a deeper collision yields `_3` instead of `unnamed_3`. Cosmetic;
   needs ≥3 unnamed outputs plus a literal conflicting name.
2. **`if_stale` is inert**: parsed and threaded through, but both plain REFRESH and
   `REFRESH ... IF STALE` take the same skip-when-clean path (`(void)if_stale;`). Fine if
   intentional; document it.
3. The refresh-history reader's try-extended/fallback-to-short pattern is exactly the ad-hoc
   compatibility mechanism `definition_version` will formalize — good precedent for reader
   behavior.
4. Minor perf: `is_stale` in the listing runs a change-feed query per view (N queries for N
   MVs); batching can come later.
5. Coverage is broad: 24 test files under `test/sql/materialized_view/` including delta
   safety/nulls, join-incremental, parser edge cases, stale reads, transactions, name
   conflicts.

### Stored procedures

Execution is delegated entirely to the DuckDB 2.0-preview core: native
`CREATE PROCEDURE ... RETURNS ... LANGUAGE JAVASCRIPT AS $$...$$` and `CALL` dispatch
(the fork's test asserts `CALL` resolves like a table function). This fork owns persistence
and transactional DDL only:

- **Create** via `DuckLakeSchemaEntry::CreateFunction` handling `CatalogType::PROCEDURE_ENTRY`
  → `DuckLakeProcedureEntry` (`ducklake_schema_entry.cpp:138`).
- **Commit** (`ducklake_transaction_state.cpp:520` `GetNewProcedureInfo`): assigns
  `ProcedureIndex` from `next_catalog_id`, serializes language/body/return_type/parameters;
  `WriteNewProcedures`/`DropProcedures` emit the INSERT/flush; the `snapshot_changes` feed
  records `created_procedure`/`dropped_procedure`; drop/create races have conflict checks.
- **Load**: procedures read with a correlated parameter subquery
  (`ducklake_metadata_manager.cpp:962`) and rebuilt into `CreateProcedureInfo`
  (`ducklake_catalog.cpp:672`). Orphaned parameters are GC'd when parent rows expire;
  schema-level cleanup covers `ducklake_procedure`.
- Persistence, reattach visibility, and rollback are covered by
  `test/sql/procedures/test_simple_procedure.test`.

Gaps relevant to versioning: no version marker anywhere; `return_type`/parameter types
round-trip through `DuckLakeTypes::ToString/FromString`, so a core type-system change would
silently reinterpret stored rows — precisely the drift `definition_version` guards against.
Execution semantics are owned by the core engine build, which is likewise unrecorded.

### Consequences for the definition_version design

- Confirmed Phase 1 touchpoints: stamp in `CreateMaterializedViewBind` (where `mv_info` is
  built) and in `GetNewProcedureInfo`; validate on load in `LoadMaterializedViews`, the
  procedure-load query, and the catalog rebuild loops; add the field to
  `DuckLakeMaterializedViewInfo` / `DuckLakeProcedureInfo`.
- The **v1 contract** should be documented as including: the `{DUCKLAKE_CATALOG}.` placeholder
  convention, single-SELECT eligibility restrictions, snapshot-CDC-based refresh modes, and
  verbatim SP language/body storage.
- Eager backfill during `MigrateV07` (add column, default 1) is sufficient — no definition
  text rewriting is needed today; lazy rewrite hooks belong to phase 3.
