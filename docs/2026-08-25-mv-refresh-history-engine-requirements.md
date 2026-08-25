# Materialized View Refresh History — DuckDB + DuckLake Engine Requirements

**Date:** 2026-08-25
**Status:** Core storage + APIs **implemented**; refresh observability metrics are now being added and validated
**Audience:** DuckDB / DuckLake implementers (not QuackLab product UI)
**Repos:** `ducklake-2-preview-port`
**Out of scope here:** QuackLab Warehouse UI / ETL journal join — see Grain
`docs/superpowers/specs/2026-08-25-mv-refresh-history-requirements.md`

---

## 1. Problem

Operators and higher-level tools need a **durable, queryable history of materialized-view refreshes**.

Today (without this feature, or on builds that only stamp `last_refreshed_snapshot`):

| Question | Answer available? |
| --- | --- |
| When was this MV last refreshed? | Pointer only (`last_refreshed_snapshot` / in-memory last mode) |
| What refresh mode was used each time? | Lost after overwrite (DuckLake) or only latest (limited) |
| How many rows were written on each refresh? | Not retained as a history |
| Did a later `REFRESH` no-op (`skipped`)? | Not in a history table |
| Can I list all refreshes for one view? | No first-class API on older builds |

Inferring history from `snapshots().changes` is unreliable: refreshes often appear only as mutations on the **backing table id**, not as named MV events.

**Goal:** first-class refresh history in **both**:

1. **DuckDB** native materialized views (in-process catalog)
2. **DuckLake** materialized views (metadata catalog, durable across attach/reconnect)

---

## 2. Design principles

1. **History is append-only** for successful refreshes that commit.
2. **Current state** remains on the MV row (`last_refreshed_snapshot`, `is_stale`, etc.) — history does not replace it.
3. **Symmetric APIs:** DuckDB and DuckLake both expose a table function with the same *job* (list refresh events), even if column sets differ slightly where storage differs.
4. **Commit-scoped:** history rows are written in the same metadata transaction that commits the refresh (DuckLake) / same catalog mutation (DuckDB).
5. **No second refresh engine** — history records what `REFRESH` / `ducklake_refresh_materialized_view` already does.

---

## 3. DuckLake requirements

### 3.1 Metadata table (Must)

Catalog table (metadata database):

```sql
CREATE TABLE {METADATA_CATALOG}.ducklake_materialized_view_refresh_history (
  view_id           BIGINT,
  refresh_snapshot  BIGINT,
  refresh_time      TIMESTAMPTZ,
  refresh_mode      VARCHAR,
  rows_refreshed    BIGINT
);
```

| Column | Meaning |
| --- | --- |
| `view_id` | MV id (same id space as `ducklake_materialized_view.view_id`) |
| `refresh_snapshot` | Snapshot id at which the refresh committed |
| `refresh_time` | Wall-clock time recorded at commit (engine `NOW()` / equivalent) |
| `refresh_mode` | Mode string produced by the refresh planner (`full`, `delta`, `join_incremental`, `auto`, … — whatever the refresh path already returns) |
| `rows_refreshed` | Rows written into the backing table for this refresh (sum of flushed file row counts / equivalent) |

**Must:**

- **D-M1** Create table on new catalogs and via migrate/ensure on attach of older catalogs (`CREATE TABLE IF NOT EXISTS`).
- **D-M2** On every **successful** refresh that updates `last_refreshed_snapshot`, also **INSERT** one history row in the same commit batch.
- **D-M3** Initial materialization at `CREATE MATERIALIZED VIEW` / `ducklake_create_materialized_view` that populates the backing table **Must** insert a history row (mode typically `full`) with the create/refresh snapshot.
- **D-M4** **Drop policy (resolved):** history rows are **retained** in `ducklake_materialized_view_refresh_history` after `DROP MATERIALIZED VIEW` / `ducklake_drop_materialized_view` (audit). The default table function **hides** them by joining only **live** MVs (`end_snapshot IS NULL` / current snapshot membership). Dropped-view history remains readable only via direct metadata-catalog SQL. Optional later: `include_dropped` parameter on the TF.

### 3.2 Table function (Must)

```sql
SELECT *
FROM ducklake_materialized_view_refresh_history('catalog_alias');
```

**Return columns (Must):**

| Column | Type |
| --- | --- |
| `schema_name` | VARCHAR |
| `view_name` | VARCHAR |
| `refresh_snapshot` | BIGINT |
| `refresh_time` | TIMESTAMP / TIMESTAMPTZ |
| `refresh_mode` | VARCHAR |
| `rows_refreshed` | BIGINT |

**Must:**

- **D-M5** Join history → live MV → schema so callers get names, not only ids.
- **D-M6** Order stable: `refresh_snapshot ASC` (then `view_id`) unless documented otherwise.
- **D-M7** Visible to a separately attached reader after the writer commits (same durability rules as other metadata).

**Should:**

- **D-S1** Optional filters: `schema_name :=`, `view_name :=` named parameters (or WHERE on the function result is enough if bind is cheap).
- **D-S2** Emit a snapshot `changes` entry such as `materialized_views_refreshed=[view_id]` so `snapshots()` archaeology matches history (nice-to-have; history table remains authoritative).

### 3.3 Interaction with existing MV APIs (Must)

- **D-M8** `ducklake_materialized_views` continues to expose `last_refreshed_snapshot`, `is_stale`, `refresh_mode` (current). History is additive.
- **D-M9** `ducklake_refresh_materialized_view` return row still includes `refresh_mode` (and `rows_refreshed` if already part of the contract). History must match that committed outcome.
- **D-M10** `refresh_mode = 'skipped'` (no-op when not stale / nothing to apply) **Must not** insert a history row and **Must not** advance `last_refreshed_snapshot` (preserve current skip semantics unless explicitly changed and tested).

### 3.4 Failure / concurrency (Must / Won't)

- **D-M11** Aborted refresh transactions leave **no** history row and **no** `last_refreshed_snapshot` bump.
- **D-W1** Do **not** invent a separate “failed refresh” history table in v1 (failures are transaction errors).
- **D-M12** Concurrent refreshes of the same MV follow existing MV locking/serialization; history order matches commit order of snapshots.

### 3.5 Tests (Must)

SQL tests under `test/sql/materialized_view/ducklake_materialized_view_refresh_history.test`:

- **D-T1** Create MV → history row (`full` / populate).
- **D-T2** Mutate source → refresh → second history row with expected mode and `rows_refreshed`.
- **D-T3** Refresh when not stale → `skipped`; history row count **unchanged** (explicit).
- **D-T4** `DETACH` + `ATTACH` same catalog → history still queryable with same rows.
- **D-T5** After `DROP MATERIALIZED VIEW`, TF returns no rows for that name; raw metadata table still has ≥ prior row count.
- **D-T6** (Should) Postgres metadata catalog path if CI covers it.

---

## 4. DuckDB native MV requirements

Native DuckDB MVs are not lake-backed; history lives on the catalog entry (serialized with the database).

### 4.1 Storage (Must)

- **B-M1** Persist per-MV ordered lists: refresh timestamps + refresh modes (as already sketched via `materialized_view_refresh_times` / `materialized_view_refresh_modes` on create info / table entry).
- **B-M2** Append on successful `REFRESH MATERIALIZED VIEW` and on initial create populate.
- **B-M3** Survive `CHECKPOINT` / reopen of a persistent DuckDB file.

### 4.2 Table function (Must)

```sql
SELECT * FROM duckdb_materialized_view_refresh_history();
```

**Return columns (Must):**

| Column | Type |
| --- | --- |
| `database_name` | VARCHAR |
| `schema_name` | VARCHAR |
| `view_name` | VARCHAR |
| `refresh_ordinal` | BIGINT | 1-based index in history |
| `refresh_time` | TIMESTAMP |
| `refresh_mode` | VARCHAR |

**Must:**

- **B-M4** List all native MVs in the default database (or all attached DuckDB catalogs — document scope).
- **B-M5** Ordinal matches append order (create first, then each refresh).

**Should:**

- **B-S1** Optional `rows_refreshed` if cheap to compute for native refresh; else omit (DuckLake has files; native may not).

### 4.3 Tests (Must)

File: `test/sql/materialized_view/native_materialized_view_refresh_history.test`

- **B-T1** Create + refresh → two history rows with distinct ordinals/modes as applicable.
- **B-T2** `REFRESH … IF STALE` when fresh → no new history row (explicit skip).
- **B-T3** `CHECKPOINT` + detach/re-attach persistent `.db` → history preserved.

---

## 5. Cross-cutting consistency

| Concern | DuckDB | DuckLake |
| --- | --- | --- |
| Successful refresh recorded | Must | Must |
| Skipped / no-op recorded | Must **not** | Must **not** |
| Failed refresh recorded | Won't (v1) | Won't (v1) |
| Query API | `duckdb_materialized_view_refresh_history()` | `ducklake_materialized_view_refresh_history(catalog)` |
| Durability | DB file / catalog serialize | Metadata catalog table |
| Current pointer | last mode fields on entry | `last_refreshed_snapshot` + `is_stale` |

Mode vocabulary **Should** stay aligned where algorithms align (`full`, `delta`, …). Do not invent product-specific mode names in the engine.

---

## 6. Non-goals (engine)

| Non-goal | Why |
| --- | --- |
| ETL / job provenance (run id, actor) | Application layer |
| UI / REST | Application layer |
| Time-travel restore of an MV to an old refresh | Separate feature |
| Guaranteed retention policy / compaction of history | Later; v1 is append-only unbounded (document; optional prune API later) |
| Replacing `snapshots()` as the general change log | History is MV-specific |

## 7. Refresh observability extension

The initial history implementation records only the refresh snapshot, time, mode,
and rows written. The next engine increment should add operational metrics without
changing refresh behavior.

### 7.1 Required event fields

Append nullable columns to the DuckLake history table and append corresponding
fields to the native DuckDB history representation. Existing columns and their
order remain compatible; new fields are added at the end.

| Field | Type | Requirement and meaning |
| --- | --- | --- |
| `refresh_duration_ms` | BIGINT | Wall-clock elapsed time from refresh execution start through successful completion. Must be non-negative. |
| `rows_written` | BIGINT | Physical rows emitted by the refresh plan. This is the current `rows_refreshed` value and should remain as a compatibility alias. |
| `rows_added` | BIGINT | Rows newly present in the materialized result versus the immediately preceding committed result, when the engine can determine this exactly. |
| `rows_removed` | BIGINT | Rows no longer present versus the immediately preceding committed result, when determinable. |
| `rows_changed` | BIGINT | Existing logical rows whose values changed. Nullable when the MV has no stable row identity or an exact diff is not computed. |
| `source_snapshot` | BIGINT | Latest dependency snapshot included by the refresh, where DuckLake can determine it. |
| `source_snapshot_time` | TIMESTAMPTZ | Source commit/event time used for freshness calculations, where available. |
| `lag_ms` | BIGINT | `refresh_commit_time - source_snapshot_time`; NULL when source time is unavailable. |

`rows_written` is a physical write metric. It must not be presented as
`rows_added`: a full replacement can write N rows while the logical result has
zero additions, removals, or changes. `rows_added`, `rows_removed`, and
`rows_changed` are logical diff metrics and may be NULL rather than guessed.

### 7.2 Metric semantics by refresh mode

- `full`: the engine replaces the backing files. `rows_written` is exact. Logical
  add/remove/change counts require a stable key or an explicit result diff; do not
  infer them from files retired and written.
- `incremental`, `delta`, and `join_incremental`: physical inserted/deleted rows
  may be counted from the plan's write/delete operators. Logical changed rows are
  key-dependent and remain nullable unless the plan provides the required identity.
- `skipped`: return `refresh_mode = 'skipped'`, but do not append a history row.
- failed or rolled-back refresh: do not append a history row and do not update the
  current refresh pointer.

### 7.3 Timing and lag contract

Use a monotonic clock for elapsed duration and wall-clock timestamps only for
persisted event times. Record the event only after the refresh transaction has
successfully committed, so a failed transaction cannot leave a misleading
duration. If commit time is not available inside the execution operator, stage
the start timestamp and duration in transaction state and finalize the event at
commit.

Lag is source freshness lag, not time since the previous MV refresh. For DuckLake,
derive it from the newest dependency snapshot included in the refresh and its
metadata commit time. For native DuckDB, return NULL unless a dependency exposes
an equivalent source timestamp. Negative values caused by clock skew should be
stored as NULL (and optionally surfaced through diagnostics), not reported as
freshness.

### 7.4 Storage and API evolution

DuckLake should evolve the existing metadata table additively with
`ALTER TABLE ... ADD COLUMN IF NOT EXISTS` during writable initialization. Older
history rows remain valid with NULL extension fields. Read-only attaches must never
attempt this migration; the table function must continue to work against the
existing schema.

Native DuckDB should prefer a serialized list of refresh-event structs for new
metadata rather than adding an unbounded set of parallel arrays. For compatibility,
the current timestamp/mode arrays may remain readable while the struct form is
introduced. Checkpoint/reopen must preserve every field.

Both table functions should append the new columns after the existing contract:

```text
DuckLake: schema_name, view_name, refresh_snapshot, refresh_time, refresh_mode,
          rows_refreshed, refresh_duration_ms, rows_written, rows_added,
          rows_removed, rows_changed, source_snapshot, source_snapshot_time, lag_ms

DuckDB:  database_name, schema_name, view_name, refresh_ordinal, refresh_time,
          refresh_mode, refresh_duration_ms, rows_written, rows_added,
          rows_removed, rows_changed, source_snapshot_time, lag_ms
```

### 7.5 Retention, privacy, and current-state access

- History is append-only but unbounded in v1; document retention and leave pruning
  to a later explicit maintenance API.
- Do not store MV SQL text, row values, or source payloads in refresh history.
- Expose the latest successful event through the existing MV metadata function or
  a companion current-refresh function so dashboards do not scan all history.
- Preserve `rows_refreshed` for existing clients; document it as the legacy name
  for physical rows written.

### 7.6 Additional acceptance tests

- Duration is present and non-negative for create and successful refresh events.
- A full refresh does not claim logical add/remove/change counts from physical
  file replacement alone.
- Incremental refresh reports exact physical writes and deletes where available.
- Lag is NULL when no source timestamp exists and is never negative.
- A failed refresh, rollback, or skipped refresh adds no event.
- Existing catalogs migrate on writable attach, while read-only attaches perform no
  DDL and can still read legacy history rows.
- Native checkpoint/reopen preserves the extended event fields.

---

## 8. Acceptance criteria (engine)

| # | Criterion |
| --- | --- |
| A1 | After create + one successful refresh, DuckLake history function returns ≥2 rows for that view with increasing `refresh_snapshot`. |
| A2 | `last_refreshed_snapshot` equals the latest history `refresh_snapshot` for that view. |
| A3 | Skipped refresh does not add a history row. |
| A4 | Native DuckDB test `native_materialized_view_refresh_history.test` passes. |
| A5 | DuckLake test `ducklake_materialized_view_refresh_history.test` passes. |
| A6 | Older DuckLake catalogs without the table gain it on attach/initialize without manual DDL. |
| A7 | Extension registration exports `ducklake_materialized_view_refresh_history` (callable; not “Did you mean ducklake_materialized_views?”). |
| A8 | Successful create and refresh events expose non-negative duration and exact physical rows written. |
| A9 | Logical added/removed/changed counts are exact when key-aware diff data exists and NULL otherwise; they are never inferred from retired files. |
| A10 | Lag is computed from an available source timestamp, NULL when unavailable, and never negative. |
| A11 | Extended fields survive DuckLake reconnect and native DuckDB checkpoint/reopen. |

---

## 9. Implementation status (this fork)

Current branch status:

| Item | Status |
| --- | --- |
| Metadata table + ensure-on-init | Present; additive metric columns included |
| Write history on refresh commit | Present; duration, physical writes, source snapshot/time, and lag staged at refresh |
| `ducklake_materialized_view_refresh_history` TF | Present with appended metric columns |
| DuckDB `duckdb_materialized_view_refresh_history` | Present with appended duration/write/diff columns |
| SQL tests (DuckLake + native) | Present |
| Published Grain pin | **Does not include this yet** — live warehouses still lack the TF |
| Snapshot `changes` annotation for MV refresh | Not required / likely missing |
| Named filters on TF | Not required |
| Explicit drop/retain policy docs | Retain raw rows; table function lists live views |

### Current implementation gap for the observability extension

The current refresh operator already has the correct commit boundary: it counts
written file rows in `DuckLakeMVRefresh::Finalize`, stages the result in
transaction state, and writes the history row beside the MV snapshot update.
The next patch should extend that staged refresh record rather than calculate
metrics later from `snapshots()` or retired files.

The current operator does not yet capture elapsed time, source event time, or
logical row identity. It also retires all live backing files for a full refresh,
so retired-file rows are not a logical `rows_removed` value. Logical diff fields
must therefore remain nullable unless the refresh plan explicitly supplies a
key-aware diff.

**Implementer checklist to call this “done” for engine:**

1. Confirm create-path always writes history (not only later refreshes) — tests expect two `full` rows in DuckLake test after create+refresh.
2. Confirm skip path writes nothing.
3. Publish a preview build containing the implementation so ATTACH catalogs migrate and the table function resolves.
4. Add brief user-facing note under DuckLake MV docs: how to query history.
5. Use a key-aware logical-diff provider when the MV result has a stable identity. The current providers recognize `GROUP BY` output columns as the row key and compare the previous result with the candidate result using null-safe equality. Keyless or otherwise unsupported MVs continue to report NULL rather than inferring logical changes from file replacement.

---

## 10. Example usage (contract)

```sql
-- DuckLake
SELECT schema_name, view_name, refresh_snapshot, refresh_time, refresh_mode, rows_refreshed
FROM ducklake_materialized_view_refresh_history('warehouse')
WHERE schema_name = 'mv_lab' AND view_name = 'mv_agg'
ORDER BY refresh_snapshot;

-- DuckDB native
SELECT database_name, schema_name, view_name, refresh_ordinal, refresh_time, refresh_mode
FROM duckdb_materialized_view_refresh_history()
WHERE view_name = 'mv'
ORDER BY refresh_ordinal;
```

---

## 11. Open questions (engine)

1. **Logical identity:** should MV DDL accept an explicit key/identity declaration, or should exact logical diffs remain optional?
2. **Unbounded growth:** is a follow-up `ducklake_prune_materialized_view_refresh_history` in scope for v1.1?
3. **Source time:** which dependency timestamp is authoritative when dependencies have different commit times?
4. **Native source lag:** should native DuckDB expose NULL unless a source table provides an event-time contract?
5. **Upstream contribution:** land the storage/API extension in mainline DuckDB/DuckLake or keep it in the preview fork until MV support merges?

---

## 12. Success metric

A user (or QuackLab) can answer *“list every successful refresh of this MV with mode, time, snapshot, duration, physical writes, logical changes when available, and freshness lag”* using only the DuckLake/DuckDB table functions — no snapshot scraping and no application-side metrics table.
