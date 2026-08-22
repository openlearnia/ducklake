# DuckLake Role-Based Access Control (RBAC)

DuckLake RBAC adds table-level, role-based privileges that govern **both
DuckLake catalogs and DuckDB-native objects** (the `memory` database and
attached DuckDB files) from a single role/grant store. It targets
**embedding applications** (for example a Go HTTP query server) that
authenticate users themselves and then run queries on the user's behalf.

## How DuckDB-wide enforcement works

DuckDB's open-source core has no privilege system, so this feature ships a
small companion patch to DuckDB core (branch `rbac` of the
`openlearnia/duckdb` fork): a pluggable `AuthorizationProvider` interface on
`DBConfig` that is consulted from binder choke points (table/view reads,
INSERT/UPDATE/DELETE/MERGE, CREATE/DROP/ALTER, ATTACH/DETACH). With no
provider installed the hooks are no-ops - stock behavior.

The ducklake extension installs a provider backed by the role/grant store.
While at least one attached DuckLake catalog has RBAC enabled, the provider
enforces on DuckDB-native objects too, using **wildcard grants** (and the
admin bootstrap role):

- A wildcard grant (`SELECT`, `INSERT`, ... with no schema/table scope)
  applies to DuckDB-native objects as well as the lake.
- Schema- or table-scoped grants apply **only within the DuckLake catalog**
  - they do not name DuckDB-native objects.
- `ATTACH` / `DETACH` require admin while enforcement is active - this also
  closes the "detach and re-attach without RBAC" escape hatch. Detaching the
  last RBAC-enabled catalog disables DuckDB-wide enforcement again.
- Prepared-statement plans are checked at bind time; long-lived cached plans
  are not re-checked if the role setting changes mid-session - prefer one
  connection (and role) per user.
- `system`, `temp`, and hidden `__ducklake_metadata_*` catalogs are exempt,
  so internal machinery and DuckLake metadata queries never route through
  the provider.

## Hardening measures

While RBAC enforcement is active (an enabled lake attached):

- **File-reading table functions are admin-only**: `read_csv`,
  `read_parquet`, `read_json`, `read_text`, `glob` and their aliases
  require the admin role - otherwise a `SELECT` could bypass catalog
  privileges by reading the lake's raw data files directly.
- **`LOAD` / `INSTALL` are admin-only**: loading extension code is engine
  management.
- **`ATTACH` / `DETACH` are admin-only** (see above).
- **Prepared statements are re-bound before every execution**, so privilege
  checks re-run after grant revocations or role changes instead of reusing
  a cached plan. This applies only while enforcement is active; with RBAC
  off, plan caching behaves as usual.
- **Grants require an existing grantee**: `ducklake_grant` only accepts
  roles created via `ducklake_create_role` (or the reserved `PUBLIC`).
- **`CREATE VIEW` is gated** like other DDL (CREATE privilege on the
  schema), in both DuckLake and DuckDB-native catalogs.

Remaining known gaps (inherent to in-process enforcement): `COPY TO/FROM`
paths that do not bind table functions, `PRAGMA`s, and direct
metadata-database access by an admin-level session. The process boundary
remains the real security boundary - combine with
`enable_external_access=false`, `allowed_directories`, `lock_configuration`
and sandboxing per the DuckDB security documentation.

## Threat model

This feature is **defense-in-depth**, following the philosophy of DuckDB's
["Securing DuckDB"](https://duckdb.org/docs/lts/operations_manual/securing_duckdb/overview)
documentation. It is *not* a security boundary against arbitrary untrusted SQL:

- The active role is a session setting (`SET ducklake_role = ...`), supplied by
  the embedding application after its own authentication. A session that can
  run arbitrary SQL can simply set a different role.
- The grants live in the DuckLake metadata database. A process with file
  access can open the metadata database directly and bypass the extension.

For untrusted SQL input, combine RBAC with DuckDB's hardening settings
(`lock_configuration`, `enable_external_access = false`, `allowed_directories`,
sandboxing) - see the DuckDB security documentation. RBAC meaningfully limits
what an *authenticated application user* can reach through your query endpoint.

## Concepts

- **Roles** are named principals stored in the DuckLake metadata database
  (`ducklake_role` table). The reserved grantee `PUBLIC` matches every session.
- **Grants** (`ducklake_grant` table) map a grantee (role or `PUBLIC`) to a
  scope and a set of privileges:
  - *wildcard* (no schema/table): applies to the whole catalog
  - *schema-scoped*: applies to all tables in a schema
  - *table-scoped*: applies to a single table
- **Privileges**: `SELECT`, `INSERT`, `UPDATE`, `DELETE`, `CREATE`, `DROP`,
  `ALTER`, `ADMIN` (grants all others), and `ALL`.
- The **active role** comes from the session setting `ducklake_role`
  (empty = only `PUBLIC` grants apply).
- The **admin role** (setting `ducklake_admin_role`, default `admin`) always
  has all privileges; it is the bootstrap role for grant management.

## Enabling RBAC

RBAC is **off by default**; catalogs behave exactly as before. Enable it per
catalog:

```sql
ATTACH 'ducklake:lake.db' AS lake (DATA_PATH '...', ENABLE_RBAC true);
```

or at runtime for every attached DuckLake catalog:

```sql
SET ducklake_enable_rbac = true;
```

Enforcement is **one-way** (like DuckDB's own security settings): once enabled,
`SET ducklake_enable_rbac = false` throws. To disable, detach and re-attach
without the option.

## Managing roles and grants

```sql
SELECT ducklake_create_role('lake', 'reader');
SELECT ducklake_drop_role('lake', 'reader');   -- also removes its grants

-- wildcard grant: reader can read everything
SELECT ducklake_grant('lake', 'reader', 'SELECT');

-- schema-scoped grant
SELECT ducklake_grant('lake', 'reader', 'SELECT', schema := 'sales');

-- table-scoped grant
SELECT ducklake_grant('lake', 'writer', 'INSERT,UPDATE,DELETE', table_name := 'orders');

-- revoke
SELECT ducklake_revoke('lake', 'writer', 'DELETE', table_name := 'orders');

-- inspect
SELECT * FROM ducklake_roles('lake');
SELECT * FROM ducklake_grants('lake');
```

Grant management requires `ADMIN`: either the role named by
`ducklake_admin_role` (default `admin`) or a grantee holding the `ADMIN`
privilege. The recommended bootstrap is: create roles and grants *before*
enabling enforcement, or `SET ducklake_role = 'admin'`.

## Enforcement matrix

| Operation | Privilege checked | Hook |
|---|---|---|
| `SELECT` / scan | `SELECT` on table | `DuckLakeTableEntry::GetScanFunction` |
| `INSERT`, `COPY` into table | `INSERT` on table | `Catalog::PlanInsert` |
| `UPDATE` | `UPDATE` on table | `Catalog::PlanUpdate` |
| `DELETE` | `DELETE` on table | `Catalog::PlanDelete` |
| `MERGE INTO` | `INSERT` + `UPDATE` + `DELETE` | `Catalog::PlanMergeInto` |
| `CREATE TABLE` / `CREATE TABLE AS` | `CREATE` on schema | `CreateTableExtended`, `PlanCreateTableAs` |
| `CREATE SCHEMA` | `CREATE` (wildcard only) | `Catalog::CreateSchema` |
| `CREATE MACRO` / `VIEW` | `CREATE` on schema | `CreateFunction`, `CreateView` paths |
| `ALTER TABLE` | `ALTER` on table | `SchemaEntry::Alter` |
| `DROP TABLE` | `DROP` on table | `SchemaEntry::DropEntry` |
| `DROP VIEW` / `MACRO` | `DROP` on schema | `SchemaEntry::DropEntry` |
| `DROP SCHEMA` | `DROP` on schema | `Catalog::DropSchema` |
| grant/revoke/role management | `ADMIN` | management functions |

Denials raise `PermissionException`. Transaction-local tables (created in the
current uncommitted transaction) are exempt from per-table checks because
their creation was already gated by the `CREATE` check.

Roles and grants are **not snapshot-versioned**: they take effect immediately
and are shared by every session attached to the catalog. Grant changes are
visible to other sessions after their next grant-cache reload (cache is
invalidated on mutation and reloaded lazily).
