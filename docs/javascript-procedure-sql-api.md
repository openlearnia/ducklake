# SQL access from JavaScript stored procedures

`CREATE PROCEDURE ... LANGUAGE JAVASCRIPT` bodies run inside an embedded
[quickjs-ng](https://github.com/quickjs-ng/quickjs) sandbox. Every invocation receives a
fresh `duckdb` object backed by a private DuckDB connection.

## Promise-based API

`execute`, `query`, and `transaction` always return Promises. Use `await` (or compose the
Promises with `Promise.all`) for every SQL operation:

```sql
CREATE PROCEDURE add_stock(item VARCHAR, amount INTEGER)
RETURNS INTEGER
LANGUAGE JAVASCRIPT
AS $$
  await duckdb.execute('INSERT INTO inventory VALUES ($1, $2)', [item, amount]);
  const rows = await duckdb.query('SELECT count(*) AS n FROM inventory');
  return Number(rows[0].n);
$$;

CALL add_stock('cherry', 3); -- -> 3
```

The procedure body itself is compiled as an `async` function, so ordinary JavaScript
microtasks and `await Promise.resolve(...)` work as expected. A procedure that only computes
and returns a scalar remains valid; its scalar is the fulfilled value of the implicit root
Promise.

| Method | Accepts | Fulfilled value |
| --- | --- | --- |
| `duckdb.execute(sql[, params])` | one or more statements | `undefined` |
| `duckdb.query(sql[, params])` | a statement producing rows | `Array<Object>` |
| `duckdb.transaction(callback)` | synchronous or async callback | callback's value, after commit |

Parameter values may be booleans, numbers, bigints, strings, or `null`. Values are bound like
ordinary prepared-statement parameters. Argument shape/conversion errors throw synchronously;
SQL and result-conversion failures reject the returned Promise with an `Error` whose message
retains DuckDB's error text.

This is a Promise-only surface. Existing procedures written for the old synchronous helpers
must add `await` before every `duckdb.execute`, `duckdb.query`, and `duckdb.transaction` call.

## Concurrency and operation lifetime

Each invocation owns one connection and a serialized SQL-operation queue. The queue executes at
most one statement at a time on that connection, while separate procedure invocations can run
concurrently. Therefore `Promise.all` is safe for batching, but statements submitted by one
procedure still settle in submission order:

```js
const [, , rows] = await Promise.all([
  duckdb.execute('INSERT INTO events VALUES (1)'),
  duckdb.execute('INSERT INTO events VALUES (2)'),
  duckdb.query('SELECT count(*) AS n FROM events')
]);
return rows[0].n; // 2
```

The host waits for the root procedure Promise and for every queued operation before returning
from `CALL`. An operation may therefore be intentionally un-awaited when only its side effect
matters, although awaiting it is recommended so errors can be handled explicitly. SQL errors
reject their own Promise; a rejected operation inside an explicit transaction also makes that
transaction rollback-only, even when JavaScript catches the rejection.

Statements run on the private connection, not the caller's connection. Caller-local settings
and uncommitted changes are not visible. Outside an explicit transaction, each operation is
independently auto-committed; later operations in the same procedure see earlier committed
writes.

## Transactions across `await`

`duckdb.transaction` owns the transaction boundary and keeps it open while the callback is
suspended at `await`:

```sql
CREATE PROCEDURE transfer(from_id INTEGER, to_id INTEGER, amount INTEGER)
RETURNS VARCHAR
LANGUAGE JAVASCRIPT
AS $$
  await duckdb.transaction(async () => {
    await duckdb.execute(
      'UPDATE accounts SET balance = balance - $1 WHERE id = $2', [amount, from_id]);
    await Promise.resolve();
    await duckdb.execute(
      'UPDATE accounts SET balance = balance + $1 WHERE id = $2', [amount, to_id]);
  });
  return 'transferred';
$$;
```

The callback may return normally or reject. All callback-submitted SQL is drained before the
transaction commits, including operations accidentally left un-awaited. The transaction rolls
back when the callback rejects, any SQL operation fails, result conversion fails, or commit
fails. Nested `duckdb.transaction` calls are rejected. Raw `BEGIN`, `START TRANSACTION`,
`COMMIT`, `ROLLBACK`, `ABORT`, and `END` statements (including later statements in a multi-
statement `execute`) are rejected because the callback owns transaction control.

`duckdb.transaction` is also rejected inside a `SECURITY DEFINER` procedure, matching
PostgreSQL — see below.

## Execution identity and `SECURITY`

Like PostgreSQL, a procedure declares who its body runs as. The clause goes between `LANGUAGE`
and `AS`, and defaults to `SECURITY INVOKER`:

```sql
CREATE PROCEDURE refresh_totals()
RETURNS INTEGER
LANGUAGE JAVASCRIPT
SECURITY DEFINER
AS $$
  await duckdb.execute('DELETE FROM totals');
  await duckdb.execute('INSERT INTO totals SELECT sum(balance) FROM accounts');
  return 1;
$$;
```

| Mode | Body runs as | `duckdb.transaction` |
| --- | --- | --- |
| `SECURITY INVOKER` (default) | the calling session's `ducklake_role` | allowed |
| `SECURITY DEFINER` | the creating session's `ducklake_role` | refused |

The clause is persisted in `ducklake_procedure.security_definer` and survives detach/reattach.
The execution identity is **not yet switched**: procedure SQL runs on a private connection that
inherits the calling session's `ducklake_role`, so a `SECURITY DEFINER` body currently executes
under the caller, exactly like an invoker body. What the mode does enforce today is the
transaction-control restriction below. Treat a definer procedure as reserved for its creator and
restrict `CALL` with `EXECUTE` grants accordingly, so the day identity switching lands the grant
model is already correct.

Because a definer body runs with privileges the caller may not hold, it may not open, commit, or
roll back a transaction — otherwise it could commit work the caller never observed. A definer
procedure that needs atomic work must instead have the caller wrap the `CALL`, or express the
work as a single statement that DuckLake commits atomically on its own.

### `EXECUTE` is required in both modes

Calling any procedure requires the `EXECUTE` privilege, whether or not it is `SECURITY
DEFINER` — the same rule PostgreSQL applies. `EXECUTE` is a distinct privilege from the table
privileges, so a role holding only `SELECT`/`INSERT`/`UPDATE`/`DELETE` cannot call a procedure:

```sql
SELECT * FROM ducklake_grant('lake', 'reporting', 'SELECT', table_name := NULL, "schema" := 'main');
-- reporting can read tables but not call routines

SELECT * FROM ducklake_grant('lake', 'reporting', 'EXECUTE', table_name := NULL, "schema" := 'main');
-- schema-scoped: applies to routines in main
```

Grants are scoped the same way as table grants. A schema-scoped `EXECUTE` grant covers routines
in that schema; a grant with no schema is catalog-wide and covers every schema. `EXECUTE` never
implies any table privilege, and no table privilege implies `EXECUTE`. Unlike PostgreSQL, a new
procedure is **not** automatically executable by `PUBLIC` — grants must be issued explicitly.

## Value conversion

* `BOOLEAN` becomes a JavaScript boolean.
* Integer types through `BIGINT` and unsigned integers through 32-bit become JavaScript
  numbers.
* `FLOAT`, `DOUBLE`, `DECIMAL`, `HUGEINT`, and `UBIGINT` become doubles and may lose precision.
* Other result types (`VARCHAR`, dates, timestamps, blobs, lists, structs, and so on) use their
  string representation.
* Query results are arrays of objects keyed by column name; duplicate names use the last value.
* `null` SQL cells become JavaScript `null`.

## Limits and implementation

Every `CALL` gets a fresh QuickJS runtime with a 64 MiB heap limit and an 8 MiB stack limit.
Query results are materialized before conversion. A nesting guard rejects more than 16 nested
JavaScript procedure calls, including calls made through SQL.

The implementation is in
`ducklake/duckdb/src/execution/operator/helper/physical_call_procedure.cpp`:

* `RegisterSqlApi` installs the Promise-based helpers.
* A per-invocation worker serializes DuckDB operations and publishes completion records.
* The QuickJS owner thread pumps Promise jobs, settles SQL Promises, and advances the
  transaction state machine.
* Runtime and connection cleanup waits for the worker and all completion records.

Coverage is provided by:

* `ducklake/duckdb/test/sql/catalog/procedure_sql_api.test` (native async, batching,
  transactions, rollback, validation, cleanup, and recursion cases).
* `ducklake/duckdb/test/sql/catalog/procedure.test` (baseline procedure behavior).
* `ducklake/test/sql/procedures/test_procedure_sql_api.test` (persisted DuckLake procedures).
