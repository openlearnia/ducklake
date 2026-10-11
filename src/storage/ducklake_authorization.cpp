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
#include "storage/ducklake_schema_entry.hpp"

namespace duckdb {

vector<reference<DuckLakeRbac>> DuckLakeRbac::FindActiveRbacs(ClientContext &context) {
	vector<reference<DuckLakeRbac>> result;
	auto databases = context.db->GetDatabaseManager().GetDatabases(context);
	for (auto &attached : databases) {
		auto &catalog = attached->GetCatalog();
		if (catalog.GetCatalogType() == "ducklake" && catalog.Cast<DuckLakeCatalog>().Rbac().IsEnabled()) {
			result.push_back(catalog.Cast<DuckLakeCatalog>().Rbac());
		}
	}
	return result;
}

bool DuckLakeRbac::CheckDuckDBObject(ClientContext &context, DuckLakePrivilege privilege, const string &,
                                     optional_idx schema_id) {
	if (!enabled || IsInternalConnection(context) || HasAdminInternal(context)) {
		return true;
	}
	auto role = GetActiveRole(context);
	uint64_t granted = GetPrivileges(context, DUCKLAKE_PUBLIC_GRANTEE, schema_id, optional_idx());
	if (!role.empty()) {
		granted |= GetPrivileges(context, role, schema_id, optional_idx());
	}
	const auto requested = static_cast<uint64_t>(privilege);
	return (granted & requested) == requested;
}

//! Bridge between the DuckDB core AuthorizationProvider and the DuckLake
//! role/grant store. Installed by the ducklake extension on load; active
//! only while at least one attached DuckLake catalog has RBAC enabled.
class DuckLakeAuthorizationProvider : public AuthorizationProvider {
public:
	static DuckLakePrivilege MapPrivileges(uint16_t privileges) {
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
		if (privileges & AUTH_EXECUTE) {
			result |= DUCKLAKE_PRIVILEGE_EXECUTE;
		}
		return static_cast<DuckLakePrivilege>(result);
	}

	//! Objects outside DuckLake are allowed only if every RBAC-enabled lake allows them
	static bool Check(ClientContext &context, uint16_t privileges, const string &object_desc) {
		for (auto &rbac : DuckLakeRbac::FindActiveRbacs(context)) {
			if (!rbac.get().CheckDuckDBObject(context, MapPrivileges(privileges), object_desc)) {
				return false;
			}
		}
		return true;
	}

	static void CheckAdminEverywhere(ClientContext &context) {
		for (auto &rbac : DuckLakeRbac::FindActiveRbacs(context)) {
			rbac.get().CheckAdmin(context);
		}
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
			throw PermissionException("Role \"%s\" does not have SELECT privilege on %s (DuckDB catalog \"%s\")",
			                          role.empty() ? DUCKLAKE_PUBLIC_GRANTEE : role,
			                          DescribeObject(schema_name, object_name), catalog.GetName().GetIdentifierName());
		}
	}

	void CheckModifyTable(ClientContext &context, Catalog &catalog, uint16_t privileges, const string &schema_name,
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
			                          DescribeObject(schema_name, table_name), catalog.GetName().GetIdentifierName());
		}
	}

	void CheckModifySchema(ClientContext &context, Catalog &catalog, uint16_t privileges, const string &schema_name,
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
			                          DescribeObject(schema_name, object_name), catalog.GetName().GetIdentifierName());
		}
	}

	void CheckEngineManagement(ClientContext &context) override {
		// ATTACH/DETACH while RBAC is active requires admin - this also
		// prevents detaching an RBAC-enabled catalog to disable enforcement
		CheckAdminEverywhere(context);
	}

	//! Calling a routine requires EXECUTE, whether or not it is SECURITY DEFINER.
	//! PostgreSQL requires EXECUTE in both modes. `security_definer` is not consulted
	//! yet: procedure SQL runs on a private connection that inherits the calling session's
	//! ducklake_role, so both modes currently execute under the caller.
	void CheckExecuteRoutine(ClientContext &context, Catalog &catalog, const string &schema_name,
	                         const string &routine_name, bool security_definer) override {
		bool allowed;
		if (catalog.GetCatalogType() == "ducklake") {
			// A routine lives in a schema of its own lake: only that lake's grants apply, and a
			// schema-scoped EXECUTE grant matches as well as a catalog-wide one.
			auto &ducklake = catalog.Cast<DuckLakeCatalog>();
			if (!ducklake.Rbac().IsEnabled()) {
				return;
			}
			auto &schema_entry = ducklake.GetSchema(ducklake.GetCatalogTransaction(context), Identifier(schema_name));
			auto schema_id = optional_idx(schema_entry.Cast<DuckLakeSchemaEntry>().GetSchemaId().index);
			allowed = ducklake.Rbac().CheckDuckDBObject(context, DUCKLAKE_PRIVILEGE_EXECUTE,
			                                            DescribeObject(schema_name, routine_name), schema_id);
		} else {
			allowed = Check(context, AUTH_EXECUTE, DescribeObject(schema_name, routine_name));
		}
		if (allowed) {
			return;
		}
		auto role = DuckLakeRbac::GetActiveRole(context);
		throw PermissionException("Role \"%s\" does not have EXECUTE privilege on procedure \"%s.%s\"",
		                          role.empty() ? DUCKLAKE_PUBLIC_GRANTEE : role, schema_name, routine_name);
	}

	bool RequireStatementRebind(ClientContext &context) override {
		return !DuckLakeRbac::FindActiveRbacs(context).empty();
	}

	void CheckReadFile(ClientContext &context, const string &function_name) override {
		CheckAdminEverywhere(context);
	}

	void CheckWriteFile(ClientContext &context, const string &path) override {
		CheckAdminEverywhere(context);
	}
};

void DuckLakeInstallAuthorizationProvider(DatabaseInstance &instance) {
	auto &config = DBConfig::GetConfig(instance);
	config.SetAuthorizationProvider(make_shared_ptr<DuckLakeAuthorizationProvider>());
}

} // namespace duckdb
