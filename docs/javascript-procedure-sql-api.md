# SQL access from JavaScript stored procedures

`CREATE PROCEDURE ... LANGUAGE JAVASCRIPT` bodies execute inside an embedded
[quickjs-ng](https://github.com/quickjs-ng/quickjs) sandbox. Historically that sandbox was
isolated: procedures could only compute over their scalar arguments and could never touch
the database. This document describes the SQL API injected into every JavaScript procedure.

## The `duckdb` global

Every procedure invocation receives a fresh `duckdb` object with two methods:

```sql
CREATE PROCEDURE add_stock(item VARCHAR, amount INTEGER)
RETURNS INTEGER
LANGUAGE JAVASCRIPT
AS $$
  // 1. run DDL/DML, or any statement; parameters bind positionally ($1, $2, ...)
  duckdb.execute('INSERT INTO inventory VALUES ($1, $2)', [item, amount]);

  // 2. run a read and get the rows back as an array of row objects
  const rows = duckdb.query('SELECT count(*) AS n FROM inventory');
  return Number(rows[0].n);
$$;

CALL add_stock('cherry', 3);   -- → 3
```

| Method | Accepts | Returns |
| --- | --- | --- |
| `duckdb.execute(sql[, params])` | one or more statements | `undefined`; throws on error |
| `duckdb.query(sql[, params])` | a statement producing rows | `Array<Object>` — one `{columnName: value, ...}` per row |

Both accept an optional `Array` of bind values (booleans, numbers, bigints, strings,
`null`). Values cast like ordinary prepared-statement parameters (e.g. the string
`'2026-01-15'` binds into a `DATE` parameter). Anything non-scalar passed as a parameter,
or anything other than `Array`/`undefined`/`null` as the second argument, raises a
`TypeError`. Both raise on failure with the original DuckDB error message, so SQL errors
are catchable in plain `try/catch`.

## Value conversion

* **Result columns → JavaScript:** `BOOLEAN` → boolean; integer families up to `BIGINT`
  and unsigned ints ≤ 32-bit → number; `FLOAT`, `DOUBLE`, `DECIMAL`, `HUGEINT`,
  `UBIGINT` → double (may lose precision); everything else (`VARCHAR`, `DATE`,
  `TIMESTAMP`, enums, blobs, lists, structs, …) → its string representation.
  Duplicate column names: the last column wins.
* **Parameters → SQL:** numbers that are exact integers within `int64` range bind as
  `BIGINT`, other numbers as `DOUBLE`, bigints as `UBIGINT`; casting to the statement's
  expected type happens server-side during binding.

## Execution semantics (read before relying on side effects)

1. **Dedicated connection.** Statements run on a private connection opened against the
   same database instance as the calling session — never on the caller's connection. A
   procedure can therefore always execute, even mid-query of the outer statement.
   Consequences:
   * Session-scoped state of the caller (`SET` options, open transactions) does **not**
     apply. Each helper call runs in its own auto-commit transaction.
   * Writes commit immediately: a later `duckdb.query` in the same body sees earlier
     writes; so does the caller after the procedure returns.
   * Uncommitted changes from the caller's transaction are **not** visible inside the
     procedure, and vice versa until they commit.
2. **Recursion limit.** Because procedures can run SQL (and SQL can `CALL` procedures),
   runaway recursion would exhaust the C++ stack. Nesting more than 16 procedure frames
   on one thread throws *"JavaScript procedure nesting limit exceeded"*. Raise it via
   `kMaxProcedureNesting` in `physical_call_procedure.cpp`.
3. **Resource limits per invocation.** Each `CALL` gets a fresh JS runtime with a 64 MiB
   heap cap and 8 MB stack cap (unchanged from the pre-SQL behavior). Query results are
   fully materialized C++-side before conversion, so result sets are bounded by database
   memory, not the JS heap cap.

## Implementation notes

Everything lives in
`src/execution/operator/helper/physical_call_procedure.cpp`:

* `RegisterSqlApi` installs the global before the body compiles; both functions share
  `JsCallSqlApi`, switching on the function's magic value (`0` = execute, `1` = query).
* The nested `Connection` for the invocation is held by `ProcedureSqlApi` and reached
  from callbacks through QuickJS' runtime opaque pointer.
* All C++ exceptions are converted to JavaScript errors at the callback boundary —
  no exception may unwind through quickjs's C frames.

Tests: `test/sql/catalog/procedure.test` (persistence/CALL semantics) and
`test/sql/catalog/procedure_sql_api.test` (SQL API).
