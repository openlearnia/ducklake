# DuckLake Role-Based Access Control (RBAC)

DuckLake RBAC adds table-level, role-based privileges to attached DuckLake
catalogs. It targets **embedding applications** (for example a Go HTTP query
server) that authenticate users themselves and then run queries on a shared
DuckLake catalog on the user's behalf.

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
