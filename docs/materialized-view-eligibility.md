# Materialized views: creation, refresh modes, and eligibility

A DuckLake materialized view is a table whose contents DuckLake can maintain for you. On
`REFRESH` it can either recompute the view from scratch or update it incrementally, applying
only what changed in the base table.

This page documents how to create and refresh a view, the four refresh modes DuckLake can choose
between, and which query shapes qualify for the cheaper ones.

The behaviour below was verified against a release build; the strategy selection lives in the
analyzer in
[`src/functions/ducklake_materialized_view.cpp`](../src/functions/ducklake_materialized_view.cpp)
(`MaterializedViewAnalysis`, from line 167) and the mode choice in `BuildRefreshPlan`
(from line 1765).

## Creating and refreshing

Views are created with a table function, **not** `CREATE MATERIALIZED VIEW` — the latter creates
an ordinary table that DuckLake will not maintain:

```sql
ATTACH 'ducklake:lake.db' AS lake (DATA_PATH 'lake_files/');
USE lake;

CREATE TABLE events AS SELECT i % 3 AS key, i % 5 AS amount FROM range(10) t(i);

SELECT * FROM ducklake_create_materialized_view(
    'lake',
    view_name := 'totals',
    query := 'SELECT key, SUM(amount) AS total, COUNT(amount) AS n, COUNT(*) AS rows
              FROM events GROUP BY key');

SELECT * FROM ducklake_refresh_materialized_view('lake', view_name := 'totals');
```

`ducklake_refresh_materialized_view` also accepts `schema_name :=` and `if_stale :=`. Its result
includes `refresh_mode`, which is the mode it used, and that is what this page is mostly about.

## The four refresh modes

| Mode | Selected when | What it does |
| --- | --- | --- |
| `full` | definition is not incrementally maintainable, or this is the first refresh | Recomputes the whole view |
| `delta` | the aggregate state is delta-maintainable | Applies only the changed rows to the stored aggregate state |
| `incremental` | maintainable, but the aggregate state is not delta-maintainable | Recomputes and diffs against the stored result |
| `join_incremental` | maintainable join over two base tables | Applies changes to the joined result |

All three maintained modes produce results identical to recomputing the query. They differ only in
how much work they do: `delta` does the least, `incremental` does more, and both are cheaper than
`full` on a large base table.

**The mode is chosen per refresh, not stored on the view.** The first refresh of any view is
always `full`, because there is no previous state to update. A refresh that finds no change to the
base table also stays on `full`. Only once the base table has actually changed does a
maintainable definition report `delta` or `incremental`, so do not read an early `full` as a
rejection.

Observed sequence for a `delta`-capable definition over a small table:

| Step | Action | Mode |
| --- | --- | --- |
| 1 | `ducklake_create_materialized_view` | `full` |
| 2 | `ducklake_refresh_materialized_view` (no change) | `full` |
| 3 | insert into the base table, then refresh | `delta` |

`REFRESH ... IF STALE` short-circuits and reports `skipped` when the sources have not changed.

## Every shape works; only the cost differs

A view definition does **not** have to be incrementally maintainable to be usable. Shapes that
cannot be maintained incrementally are created normally and refreshed correctly — they simply
always run `full`. So there is no "unsupported definition" error to hit here.

The following all create successfully and refresh with `full`:

| Shape | Example |
| --- | --- |
| `DISTINCT` | `SELECT DISTINCT key FROM events` |
| `LIMIT` / `FETCH` | `... GROUP BY key LIMIT 5` |
| `HAVING` | `... GROUP BY key HAVING COUNT(*) > 0` |
| Common table expressions | `WITH x AS (...) SELECT ...` |
| Set operations | `... UNION ...` |
| No aggregate at all | `SELECT key FROM events` |

## Qualifying for `delta`

`delta` is the cheapest mode: it updates the stored aggregate state in place rather than
recomputing. Two conditions must hold.

**`COUNT(*)` must be present.** It tracks which groups are still live. Without it, a group whose
rows have all been deleted could never be removed from the view.

**Every `SUM` needs a matching `COUNT` of the same expression.** SQL returns `NULL` for `SUM`
over an all-NULL group but `0` over an empty set, and the paired count is what lets DuckLake tell a
real zero from that ambiguity. `AVG` is derived from a `SUM`/`COUNT` pair and needs the same.

```sql
-- reaches delta
SELECT key, SUM(amount) AS total, COUNT(amount) AS n, COUNT(*) AS rows
FROM events GROUP BY key;
```

`MIN` and `MAX` do not block `delta`; when present, DuckLake uses a conditional variant that
rebuilds their per-group invalidation.

Worked example, observed on a refresh following a change to the base table:

| Definition | Mode |
| --- | --- |
| `SUM(amount), COUNT(amount), COUNT(*)` | `delta` |
| `MIN(amount), MAX(amount), COUNT(*)` | `delta` (conditional variant) |
| `SUM(amount)` on its own | `incremental` |

The last row is the common trap. A `SUM` with no paired `COUNT` does **not** break correctness —
it gives up the cheap `delta` path and falls back to `incremental`, which recomputes and diffs.
The result is still exactly right, including removing groups whose rows were all deleted.

## Qualifying for `join_incremental`

A view over a join is maintained incrementally when the definition is:

- a single `INNER` equi-join between exactly two base tables, in the same catalog, and
- a join condition that is a single equality linking the two relations, and
- aggregates over **fact-side** columns only — an aggregate over a dimension column cannot be
  updated from fact changes alone, and
- every `GROUP BY` key present in the select list, since DuckLake derives the change set from the
  projected columns.

Anything else — a compound join condition, a subquery or table function in the `FROM`, three or
more relations, a cross join — falls back to `full`.

## Constraint types on the backing table

`CHECK` and `FOREIGN KEY` are rejected on DuckLake tables. `PRIMARY KEY` and `UNIQUE` are accepted
but **never enforced** — DuckLake builds no index for them, so a row that violates a declared key
is accepted. A `PRIMARY KEY` column is still `NOT NULL`, and the declared keys are visible to
`duckdb_constraints()`, `information_schema.table_constraints`, and `duckdb_tables()`. Note that
`ALTER TABLE ... ADD PRIMARY KEY` records the key but does *not* add `NOT NULL`, unlike
`CREATE TABLE ... PRIMARY KEY`.

## Diagnosing a view that always refreshes fully

Because an ineligible definition is never rejected, the only way to find out why a view is stuck
on `full` is to compare its definition against the tables above. The analyzer does compute a
human-readable reason for each case, but that reason is internal and is not currently exposed
through SQL — surfacing it (in the refresh result, or as a diagnostic view) would be the most
useful improvement to this feature.