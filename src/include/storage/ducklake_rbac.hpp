//===----------------------------------------------------------------------===//
// DuckLake
//
// ducklake_rbac.hpp
//
// Role-based access control for DuckLake catalogs.
//
// Roles and grants are persisted in the DuckLake metadata database
// (ducklake_role / ducklake_grant tables). The active role is supplied
// per session via the `ducklake_role` extension setting. Enforcement is
// enabled per catalog via the one-way `enable_rbac` attach option.
//
// This is defense-in-depth for embedding applications that perform their
// own authentication - it is not a security boundary against arbitrary
// untrusted SQL (see docs/rbac.md).
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/constants.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/vector.hpp"

namespace duckdb {

class ClientContext;
class DuckLakeCatalog;
class DuckLakeMetadataManager;
class DuckLakeTransaction;

//! Table-level privileges enforced by DuckLake RBAC
enum DuckLakePrivilege : uint64_t {
	DUCKLAKE_PRIVILEGE_NONE = 0,
	DUCKLAKE_PRIVILEGE_SELECT = 1ULL << 0,
	DUCKLAKE_PRIVILEGE_INSERT = 1ULL << 1,
	DUCKLAKE_PRIVILEGE_UPDATE = 1ULL << 2,
	DUCKLAKE_PRIVILEGE_DELETE = 1ULL << 3,
	DUCKLAKE_PRIVILEGE_CREATE = 1ULL << 4,
	DUCKLAKE_PRIVILEGE_DROP = 1ULL << 5,
	DUCKLAKE_PRIVILEGE_ALTER = 1ULL << 6,
	DUCKLAKE_PRIVILEGE_ADMIN = 1ULL << 7,
	DUCKLAKE_PRIVILEGE_ALL = (1ULL << 8) - 1,
};

//! Reserved grantee that applies to every session regardless of the
//! active `ducklake_role` setting
static constexpr const char *DUCKLAKE_PUBLIC_GRANTEE = "PUBLIC";

class DuckLakeRbac {
public:
	DuckLakeRbac(DuckLakeCatalog &catalog);

	//! Whether enforcement is enabled for this catalog
	bool IsEnabled() const {
		return enabled;
	}

	//! Enable/disable enforcement. One-way like DuckDB's security settings:
	//! enabling is allowed at any time, disabling once enabled throws.
	void SetEnabled(bool value);

	//! Parse a comma-separated privilege list ("SELECT,INSERT", "ALL")
	static uint64_t ParsePrivileges(const string &privileges);
	//! Serialize a privilege bitmask back to a comma-separated list
	static string PrivilegesToString(uint64_t privileges);

	//! Get the active role for the given context (ducklake_role setting);
	//! empty when unset - only PUBLIC grants apply
	static string GetActiveRole(ClientContext &context);
	//! Name of the role that holds ADMIN privileges (ducklake_admin_role
	//! setting, default "admin")
	static string GetAdminRole(ClientContext &context);

	//! True when the active role holds ADMIN (directly or via PUBLIC)
	bool HasAdmin(ClientContext &context);

	//! Verify the active role holds `privilege` on the given table. Throws
	//! a PermissionException otherwise. No-op when RBAC is disabled.
	void CheckTablePrivilege(ClientContext &context, DuckLakePrivilege privilege, class DuckLakeTableEntry &table);

	//! Verify the active role holds `privilege` on the given schema
	//! (CREATE/DROP of tables within it). Throws when denied. No-op when
	//! RBAC is disabled.
	void CheckSchemaPrivilege(ClientContext &context, DuckLakePrivilege privilege, class DuckLakeSchemaEntry &schema);

	//! Verify the active role holds `privilege` at catalog scope - used for
	//! CREATE SCHEMA, matched only against wildcard grants. Throws when
	//! denied. No-op when RBAC is disabled.
	void CheckCatalogPrivilege(ClientContext &context, DuckLakePrivilege privilege);

	//! Verify the active role holds ADMIN - required for grant/revoke and
	//! role management. Throws when denied. No-op when RBAC is disabled.
	void CheckAdmin(ClientContext &context);

	//! Drop the cached grants - called after any grant/revoke/role mutation
	void InvalidateCache();

private:
	struct GrantRow {
		string grantee;
		optional_idx schema_id;
		optional_idx table_id;
		uint64_t privileges;
	};

	DuckLakeCatalog &catalog;
	//! one-way: once true, SetEnabled(false) throws
	bool enabled = false;

	mutex cache_lock;
	bool cache_loaded = false;
	vector<GrantRow> grants;

	void LoadGrants(ClientContext &context);
	uint64_t GetPrivileges(ClientContext &context, const string &grantee, optional_idx schema_id,
	                       optional_idx table_id);
	bool HasAdminInternal(ClientContext &context);
};

} // namespace duckdb
