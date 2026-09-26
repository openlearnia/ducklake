#include "storage/ducklake_rbac.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "storage/ducklake_catalog.hpp"
#include "storage/ducklake_metadata_manager.hpp"
#include "storage/ducklake_schema_entry.hpp"
#include "storage/ducklake_table_entry.hpp"
#include "storage/ducklake_transaction.hpp"

namespace duckdb {

DuckLakeRbac::DuckLakeRbac(DuckLakeCatalog &catalog_p) : catalog(catalog_p) {
}

bool DuckLakeRbac::IsInternalConnection(ClientContext &context) {
	return context.registered_state &&
	       context.registered_state->Get<DuckLakeInternalConnectionState>(DuckLakeInternalConnectionState::KEY);
}

void DuckLakeRbac::SetEnabled(bool value) {
	if (!value && enabled) {
		// one-way like DuckDB's own security settings: once RBAC is
		// enabled for an attached catalog it cannot be disabled from SQL
		throw PermissionException(
		    "Cannot disable ducklake RBAC while the catalog is attached - detach and re-attach to change it");
	}
	enabled = value;
}

void DuckLakeRbac::EnsureMetadataTables(ClientContext &context) {
	auto &transaction = DuckLakeTransaction::Get(context, catalog);
	auto result = transaction.GetMetadataManager().Query(R"(
CREATE TABLE IF NOT EXISTS {METADATA_CATALOG}.ducklake_role(role_id BIGINT PRIMARY KEY, role_name VARCHAR UNIQUE NOT NULL);
CREATE TABLE IF NOT EXISTS {METADATA_CATALOG}.ducklake_grant(grant_id BIGINT PRIMARY KEY, grantee VARCHAR NOT NULL, schema_id BIGINT, table_id BIGINT, privileges BIGINT NOT NULL);
	)");
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to initialize DuckLake RBAC metadata: ");
	}
}

static bool ParseSinglePrivilege(const string &privilege, uint64_t &result) {
	string entry = privilege;
	StringUtil::Trim(entry);
	entry = StringUtil::Upper(entry);
	if (entry == "SELECT") {
		result |= DUCKLAKE_PRIVILEGE_SELECT;
	} else if (entry == "INSERT") {
		result |= DUCKLAKE_PRIVILEGE_INSERT;
	} else if (entry == "UPDATE") {
		result |= DUCKLAKE_PRIVILEGE_UPDATE;
	} else if (entry == "DELETE") {
		result |= DUCKLAKE_PRIVILEGE_DELETE;
	} else if (entry == "CREATE") {
		result |= DUCKLAKE_PRIVILEGE_CREATE;
	} else if (entry == "DROP") {
		result |= DUCKLAKE_PRIVILEGE_DROP;
	} else if (entry == "ALTER") {
		result |= DUCKLAKE_PRIVILEGE_ALTER;
	} else if (entry == "ADMIN") {
		result |= DUCKLAKE_PRIVILEGE_ADMIN;
	} else if (entry == "ALL") {
		result |= DUCKLAKE_PRIVILEGE_ALL;
	} else {
		return false;
	}
	return true;
}

uint64_t DuckLakeRbac::ParsePrivileges(const string &privileges) {
	uint64_t result = DUCKLAKE_PRIVILEGE_NONE;
	auto entries = StringUtil::Split(privileges, ',');
	for (auto &entry : entries) {
		if (!ParseSinglePrivilege(entry, result)) {
			string trimmed = entry;
			StringUtil::Trim(trimmed);
			throw InvalidInputException(
			    "Unknown privilege \"%s\" - supported privileges are SELECT, INSERT, UPDATE, DELETE, CREATE, DROP, "
			    "ALTER, ADMIN, ALL",
			    trimmed);
		}
	}
	if (result == DUCKLAKE_PRIVILEGE_NONE) {
		throw InvalidInputException("No privileges specified");
	}
	return result;
}

string DuckLakeRbac::PrivilegesToString(uint64_t privileges) {
	if (privileges == DUCKLAKE_PRIVILEGE_ALL) {
		return "ALL";
	}
	vector<string> entries;
	if (privileges & DUCKLAKE_PRIVILEGE_SELECT) {
		entries.push_back("SELECT");
	}
	if (privileges & DUCKLAKE_PRIVILEGE_INSERT) {
		entries.push_back("INSERT");
	}
	if (privileges & DUCKLAKE_PRIVILEGE_UPDATE) {
		entries.push_back("UPDATE");
	}
	if (privileges & DUCKLAKE_PRIVILEGE_DELETE) {
		entries.push_back("DELETE");
	}
	if (privileges & DUCKLAKE_PRIVILEGE_CREATE) {
		entries.push_back("CREATE");
	}
	if (privileges & DUCKLAKE_PRIVILEGE_DROP) {
		entries.push_back("DROP");
	}
	if (privileges & DUCKLAKE_PRIVILEGE_ALTER) {
		entries.push_back("ALTER");
	}
	if (privileges & DUCKLAKE_PRIVILEGE_ADMIN) {
		entries.push_back("ADMIN");
	}
	return StringUtil::Join(entries, ",");
}

string DuckLakeRbac::GetActiveRole(ClientContext &context) {
	Value role_setting;
	if (context.TryGetCurrentSetting("ducklake_role", role_setting) && !role_setting.IsNull()) {
		auto role = role_setting.GetValue<string>();
		if (!role.empty()) {
			return role;
		}
	}
	return string();
}

string DuckLakeRbac::GetAdminRole(ClientContext &context) {
	Value admin_setting;
	if (context.TryGetCurrentSetting("ducklake_admin_role", admin_setting) && !admin_setting.IsNull()) {
		auto admin = admin_setting.GetValue<string>();
		if (!admin.empty()) {
			return admin;
		}
	}
	return "admin";
}

void DuckLakeRbac::InvalidateCache() {
}

//! Does the grant row apply to the requested scope?
//! - table-scoped grants (table_id set) match only that exact table
//! - schema-scoped grants (schema_id set, table_id null) match everything in the schema
//! - wildcard grants match everything
static bool GrantApplies(const optional_idx &grant_schema, const optional_idx &grant_table, optional_idx schema_id,
                         optional_idx table_id) {
	if (grant_table.IsValid()) {
		return table_id.IsValid() && grant_table.GetIndex() == table_id.GetIndex();
	}
	if (grant_schema.IsValid()) {
		return schema_id.IsValid() && grant_schema.GetIndex() == schema_id.GetIndex();
	}
	return true;
}

uint64_t DuckLakeRbac::GetPrivileges(ClientContext &context, const string &grantee, optional_idx schema_id,
                                      optional_idx table_id) {
	auto &transaction = DuckLakeTransaction::Get(context, catalog);
	auto query_result = transaction.GetMetadataManager().Query(
	    "SELECT grantee, schema_id, table_id, privileges FROM {METADATA_CATALOG}.ducklake_grant");
	if (query_result->HasError()) {
		query_result->GetErrorObject().Throw("Failed to load DuckLake grants: ");
	}
	uint64_t granted = DUCKLAKE_PRIVILEGE_NONE;
	for (auto &row : *query_result) {
		if (!StringUtil::CIEquals(row.GetValue<string>(0), grantee)) {
			continue;
		}
		auto grant_schema = row.IsNull(1) ? optional_idx() : optional_idx(row.GetValue<idx_t>(1));
		auto grant_table = row.IsNull(2) ? optional_idx() : optional_idx(row.GetValue<idx_t>(2));
		if (GrantApplies(grant_schema, grant_table, schema_id, table_id)) {
			granted |= row.GetValue<uint64_t>(3);
		}
	}
	return granted;
}

bool DuckLakeRbac::HasAdminInternal(ClientContext &context) {
	auto role = GetActiveRole(context);
	// bootstrap: the role named by ducklake_admin_role is always admin
	if (StringUtil::CIEquals(role, GetAdminRole(context))) {
		return true;
	}
	uint64_t privileges = GetPrivileges(context, DUCKLAKE_PUBLIC_GRANTEE, optional_idx(), optional_idx());
	if (!role.empty()) {
		privileges |= GetPrivileges(context, role, optional_idx(), optional_idx());
	}
	return (privileges & DUCKLAKE_PRIVILEGE_ADMIN) != 0;
}

bool DuckLakeRbac::HasAdmin(ClientContext &context) {
	if (!enabled) {
		return true;
	}
	return HasAdminInternal(context);
}

void DuckLakeRbac::CheckAdmin(ClientContext &context) {
	if (!enabled || IsInternalConnection(context)) {
		return;
	}
	if (!HasAdminInternal(context)) {
		auto role = GetActiveRole(context);
		throw PermissionException("Role \"%s\" does not have ADMIN privileges on DuckLake catalog \"%s\"",
		                          role.empty() ? DUCKLAKE_PUBLIC_GRANTEE : role, catalog.GetName().GetIdentifierName());
	}
}

static void ThrowDenied(DuckLakePrivilege privilege, const string &role, const string &object, const string &catalog) {
	throw PermissionException("Role \"%s\" does not have %s privilege on %s of DuckLake catalog \"%s\"",
	                          role.empty() ? DUCKLAKE_PUBLIC_GRANTEE : role,
	                          DuckLakeRbac::PrivilegesToString(static_cast<uint64_t>(privilege)), object, catalog);
}

void DuckLakeRbac::CheckTablePrivilege(ClientContext &context, DuckLakePrivilege privilege,
                                        DuckLakeTableEntry &table) {
	if (!enabled || IsInternalConnection(context)) {
		return;
	}
	if (table.GetTableId().IsTransactionLocal()) {
		// transaction-local tables: creation was already gated by the CREATE
		// check, the table has no persisted id to grant against
		return;
	}
	auto role = GetActiveRole(context);
	if (HasAdminInternal(context)) {
		return;
	}
	auto table_id = optional_idx(table.GetTableId().index);
	auto &schema_entry = table.ParentSchema().Cast<DuckLakeSchemaEntry>();
	auto schema_id = optional_idx(schema_entry.GetSchemaId().index);
	uint64_t privileges = GetPrivileges(context, DUCKLAKE_PUBLIC_GRANTEE, schema_id, table_id);
	if (!role.empty()) {
		privileges |= GetPrivileges(context, role, schema_id, table_id);
	}
	if ((privileges & static_cast<uint64_t>(privilege)) != static_cast<uint64_t>(privilege) &&
	    (privileges & DUCKLAKE_PRIVILEGE_ADMIN) == 0) {
		ThrowDenied(privilege, role, StringUtil::Format("table %s.%s", schema_entry.name, table.name),
		            catalog.GetName().GetIdentifierName());
	}
}

void DuckLakeRbac::CheckSchemaPrivilege(ClientContext &context, DuckLakePrivilege privilege,
                                         DuckLakeSchemaEntry &schema) {
	if (!enabled || IsInternalConnection(context)) {
		return;
	}
	auto role = GetActiveRole(context);
	if (HasAdminInternal(context)) {
		return;
	}
	auto schema_id = optional_idx(schema.GetSchemaId().index);
	uint64_t privileges = GetPrivileges(context, DUCKLAKE_PUBLIC_GRANTEE, schema_id, optional_idx());
	if (!role.empty()) {
		privileges |= GetPrivileges(context, role, schema_id, optional_idx());
	}
	if ((privileges & static_cast<uint64_t>(privilege)) != static_cast<uint64_t>(privilege) &&
	    (privileges & DUCKLAKE_PRIVILEGE_ADMIN) == 0) {
		ThrowDenied(privilege, role, StringUtil::Format("schema %s", schema.name), catalog.GetName().GetIdentifierName());
	}
}

void DuckLakeRbac::CheckCatalogPrivilege(ClientContext &context, DuckLakePrivilege privilege) {
	if (!enabled || IsInternalConnection(context)) {
		return;
	}
	auto role = GetActiveRole(context);
	if (HasAdminInternal(context)) {
		return;
	}
	// catalog-scope operations (e.g. CREATE SCHEMA) match wildcard grants only
	uint64_t privileges = GetPrivileges(context, DUCKLAKE_PUBLIC_GRANTEE, optional_idx(), optional_idx());
	if (!role.empty()) {
		privileges |= GetPrivileges(context, role, optional_idx(), optional_idx());
	}
	if ((privileges & static_cast<uint64_t>(privilege)) != static_cast<uint64_t>(privilege) &&
	    (privileges & DUCKLAKE_PRIVILEGE_ADMIN) == 0) {
		ThrowDenied(privilege, role, "catalog", catalog.GetName().GetIdentifierName());
	}
}

} // namespace duckdb
