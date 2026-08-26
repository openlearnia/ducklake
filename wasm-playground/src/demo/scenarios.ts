export type ScenarioId =
  | 'materialized-view'
  | 'refresh-history'
  | 'snapshots'
  | 'javascript-procedure';

export interface Scenario {
  id: ScenarioId;
  title: string;
  summary: string;
  focus: string;
  sql: string;
}

export const DEMO_SEED_SQL = `
ATTACH 'ducklake:/openlearnia/feature-playground.ducklake' AS demo (
  DATA_PATH '/openlearnia/feature-playground-files',
  DATA_INLINING_ROW_LIMIT 100
);

CREATE TABLE demo.orders (
  order_id INTEGER,
  customer VARCHAR,
  region VARCHAR,
  amount INTEGER
);

INSERT INTO demo.orders VALUES
  (1001, 'Aster', 'North', 120),
  (1002, 'Beryl', 'South', 80),
  (1003, 'Cedar', 'North', 150),
  (1004, 'Dahlia', 'West', 210);
`;

export const scenarios: readonly Scenario[] = [
  {
    id: 'materialized-view',
    title: 'Materialized views',
    summary: 'Create a managed aggregate and query its stored result.',
    focus: 'DuckLake-managed materialized view',
    sql: `CREATE MATERIALIZED VIEW demo.orders_by_region AS
SELECT region, count(*) AS orders, sum(amount) AS revenue
FROM demo.orders
GROUP BY region
ORDER BY region;

SELECT * FROM demo.orders_by_region;`,
  },
  {
    id: 'refresh-history',
    title: 'Refresh history',
    summary: 'Change the source, refresh incrementally, and inspect the audit row.',
    focus: 'Incremental refresh and logical row diff',
    sql: `CREATE MATERIALIZED VIEW demo.orders_history_mv AS
SELECT region, count(*) AS orders, sum(amount) AS revenue
FROM demo.orders
GROUP BY region;

INSERT INTO demo.orders VALUES (1005, 'Elm', 'North', 95);
REFRESH MATERIALIZED VIEW demo.orders_history_mv;

SELECT view_name, refresh_mode, rows_refreshed, rows_added, rows_removed,
       rows_changed, refresh_snapshot
FROM ducklake_materialized_view_refresh_history('demo')
WHERE view_name = 'orders_history_mv'
ORDER BY refresh_snapshot DESC
LIMIT 1;`,
  },
  {
    id: 'snapshots',
    title: 'Snapshots',
    summary: 'Inspect the append-only DuckLake snapshot timeline.',
    focus: 'Snapshot metadata and time-travel foundation',
    sql: `SELECT snapshot_id, schema_version, changes
FROM ducklake_snapshots('demo')
ORDER BY snapshot_id;`,
  },
  {
    id: 'javascript-procedure',
    title: 'JavaScript procedures',
    summary: 'Create a persisted typed procedure and call it from SQL.',
    focus: 'First-class JavaScript procedure runtime',
    sql: `USE demo;

CREATE PROCEDURE add_tax(value INTEGER, rate DOUBLE)
RETURNS DOUBLE
LANGUAGE JAVASCRIPT
AS $$
return value * (1 + rate);
$$;

SELECT add_tax(100, 0.2) AS total_with_tax;
USE memory;`,
  },
];
