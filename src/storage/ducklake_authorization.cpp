#include "storage/ducklake_rbac.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/authorization_provider.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "storage/ducklake_catalog.hpp"

namespace duckdb {

DuckLakeRbac *DuckLakeRbac::FindActiveRbac(ClientContext &context) {
	auto databases = context.db->GetDatabaseManager().GetDatabases(context);
	for (auto &attached : databases) {
		auto &catalog = attached->GetCatalog();
		if (catalog.GetCatalogType() == "ducklake" && catalog.Cast<DuckLakeCatalog>().Rbac().IsEnabled()) {
			return &catalog.Cast<DuckLakeCatalog>().Rbac();
		}
	}
	return nullptr;
}

bool DuckLakeRbac::CheckDuckDBObject(ClientContext &context, DuckLakePrivilege privilege, const string &) {
	if (!enabled || IsInternalConnection(context) || HasAdminInternal(context)) {
		return true;
	}
	auto role = GetActiveRole(context);
	uint64_t granted = GetPrivileges(context, DUCKLAKE_PUBLIC_GRANTEE, optional_idx(), optional_idx());
	if (!role.empty()) {
		granted |= GetPrivileges(context, role, optional_idx(), optional_idx());
	}
	const auto requested = static_cast<uint64_t>(privilege);
	return (granted & requested) == requested;
}

//! Bridge between the DuckDB core AuthorizationProvider and the DuckLake
//! role/grant store. Installed by the ducklake extension on load; active
//! only while at least one attached DuckLake catalog has RBAC enabled.
class DuckLakeAuthorizationProvider : public AuthorizationProvider {
public:
	static DuckLakePrivilege MapPrivileges(uint8_t privileges) {
		uint64_t result = DUCKLAKE_PRIVILEGE_NONE;
		if (privileges & AUTH_SELECT) {
			result |= DUCKLAKE_PRIVILEGE_SELECT;
		}
		if (privileges & AUTH_INSERT) {
			result |= DUCKLAKE_PRIVILEGE_INSERT;
		}
		if (privileges & AUTH_UPDATE) {
			result |= DUCKLAKE_PRIVILEGE_UPDATE;
		}
		if (privileges & AUTH_DELETE) {
			result |= DUCKLAKE_PRIVILEGE_DELETE;
		}
		if (privileges & AUTH_CREATE) {
			result |= DUCKLAKE_PRIVILEGE_CREATE;
		}
		if (privileges & AUTH_DROP) {
			result |= DUCKLAKE_PRIVILEGE_DROP;
		}
		if (privileges & AUTH_ALTER) {
			result |= DUCKLAKE_PRIVILEGE_ALTER;
		}
		if (privileges & AUTH_ADMIN) {
			result |= DUCKLAKE_PRIVILEGE_ADMIN;
		}
		return static_cast<DuckLakePrivilege>(result);
	}

	static bool Check(ClientContext &context, uint8_t privileges, const string &object_desc) {
		auto rbac = DuckLakeRbac::FindActiveRbac(context);
		if (!rbac) {
			// no attached catalog has RBAC enabled - allow everything
			return true;
		}
		return rbac->CheckDuckDBObject(context, MapPrivileges(privileges), object_desc);
	}

	static string DescribeObject(const string &schema_name, const string &object_name) {
		if (object_name.empty()) {
			return StringUtil::Format("schema \"%s\"", schema_name);
		}
		return StringUtil::Format("\"%s.%s\"", schema_name, object_name);
	}

	void CheckReadObject(ClientContext &context, Catalog &catalog, bool is_view, const string &schema_name,
	                     const string &object_name) override {
		if (catalog.GetCatalogType() == "ducklake") {
			// DuckLake catalogs enforce SELECT themselves (with finer
			// table-scope semantics) at scan-bind time
			return;
		}
		if (!Check(context, AUTH_SELECT, DescribeObject(schema_name, object_name))) {
			auto role = DuckLakeRbac::GetActiveRole(context);
			throw PermissionException(
			    "Role \"%s\" does not have SELECT privilege on %s (DuckDB catalog \"%s\")",
			    role.empty() ? DUCKLAKE_PUBLIC_GRANTEE : role, DescribeObject(schema_name, object_name),
			    catalog.GetName());
		}
	}

	void CheckModifyTable(ClientContext &context, Catalog &catalog, uint8_t privileges, const string &schema_name,
	                      const string &table_name) override {
		if (catalog.GetCatalogType() == "ducklake") {
			// enforced by DuckLake's own plan-time hooks
			return;
		}
		if (!Check(context, privileges, DescribeObject(schema_name, table_name))) {
			auto role = DuckLakeRbac::GetActiveRole(context);
			throw PermissionException("Role \"%s\" does not have %s privilege on %s (DuckDB catalog \"%s\")",
			                          role.empty() ? DUCKLAKE_PUBLIC_GRANTEE : role,
			                          DuckLakeRbac::PrivilegesToString(MapPrivileges(privileges)),
			                          DescribeObject(schema_name, table_name), catalog.GetName());
		}
	}

	void CheckModifySchema(ClientContext &context, Catalog &catalog, uint8_t privileges, const string &schema_name,
	                       const string &object_name) override {
		if (catalog.GetCatalogType() == "ducklake") {
			// enforced by DuckLake's own DDL hooks
			return;
		}
		if (!Check(context, privileges, DescribeObject(schema_name, object_name))) {
			auto role = DuckLakeRbac::GetActiveRole(context);
			throw PermissionException("Role \"%s\" does not have %s privilege on %s (DuckDB catalog \"%s\")",
			                          role.empty() ? DUCKLAKE_PUBLIC_GRANTEE : role,
			                          DuckLakeRbac::PrivilegesToString(MapPrivileges(privileges)),
			                          DescribeObject(schema_name, object_name), catalog.GetName());
		}
	}

	void CheckEngineManagement(ClientContext &context) override {
		auto rbac = DuckLakeRbac::FindActiveRbac(context);
		if (!rbac) {
			return;
		}
		// ATTACH/DETACH while RBAC is active requires admin - this also
		// prevents detaching an RBAC-enabled catalog to disable enforcement
		rbac->CheckAdmin(context);
	}

	bool RequireStatementRebind(ClientContext &context) override {
		return DuckLakeRbac::FindActiveRbac(context) != nullptr;
	}

	void CheckReadFile(ClientContext &context, const string &function_name) override {
		auto rbac = DuckLakeRbac::FindActiveRbac(context);
		if (rbac) {
			rbac->CheckAdmin(context);
		}
	}
};

void DuckLakeInstallAuthorizationProvider(DatabaseInstance &instance) {
	auto &config = DBConfig::GetConfig(instance);
	config.SetAuthorizationProvider(make_shared_ptr<DuckLakeAuthorizationProvider>());
}

} // namespace duckdb
