#include "functions/ducklake_table_functions.hpp"
#include "storage/ducklake_catalog.hpp"
#include "storage/ducklake_metadata_manager.hpp"
#include "storage/ducklake_rbac.hpp"
#include "storage/ducklake_schema_entry.hpp"
#include "storage/ducklake_table_entry.hpp"
#include "storage/ducklake_transaction.hpp"

namespace duckdb {

namespace {

enum class RbacAction : uint8_t { CREATE_ROLE, DROP_ROLE, GRANT, REVOKE };

struct RbacFunctionData : public TableFunctionData {
	RbacFunctionData(Catalog &catalog_p, RbacAction action_p) : catalog(catalog_p), action(action_p) {
	}

	Catalog &catalog;
	RbacAction action;
	//! role name (CREATE_ROLE / DROP_ROLE) or grantee (GRANT / REVOKE)
	string name;
	//! privileges bitmask for GRANT / REVOKE
	uint64_t privileges = DUCKLAKE_PRIVILEGE_NONE;
	optional_idx schema_id;
	optional_idx table_id;
	bool finished = false;
};

string EscapeSingleQuote(const string &input) {
	return StringUtil::Replace(input, "'", "''");
}

//! Resolve the schema/table scope for a grant/revoke call. Named parameters
//! are optional; providing table_name without schema defaults to "main".
void ResolveGrantScope(Catalog &catalog, ClientContext &context, TableFunctionBindInput &input,
                       optional_idx &schema_id, optional_idx &table_id) {
	auto schema_param = input.named_parameters.find("schema");
	auto table_param = input.named_parameters.find("table_name");
	bool has_schema = schema_param != input.named_parameters.end() && !schema_param->second.IsNull();
	bool has_table = table_param != input.named_parameters.end() && !table_param->second.IsNull();
	if (!has_schema && !has_table) {
		return; // wildcard grant
	}
	string schema_name = has_schema ? StringValue::Get(schema_param->second) : "main";
	auto schema = catalog.GetSchema(context, schema_name, OnEntryNotFound::THROW_EXCEPTION);
	auto &duck_schema = schema->Cast<DuckLakeSchemaEntry>();
	schema_id = optional_idx(duck_schema.GetSchemaId().index);
	if (has_table) {
		auto table_name = StringValue::Get(table_param->second);
		auto table_entry =
		    catalog.GetEntry<TableCatalogEntry>(context, schema_name, table_name, OnEntryNotFound::THROW_EXCEPTION);
		table_id = optional_idx(table_entry->Cast<DuckLakeTableEntry>().GetTableId().index);
	}
}

//! WHERE clause matching exactly one (grantee, scope) grant row
string GrantScopePredicate(const string &grantee, optional_idx schema_id, optional_idx table_id) {
	string result = StringUtil::Format("grantee = '%s'", EscapeSingleQuote(grantee));
	if (table_id.IsValid()) {
		result += StringUtil::Format(" AND table_id = %llu", table_id.GetIndex());
	} else if (schema_id.IsValid()) {
		result += StringUtil::Format(" AND schema_id = %llu AND table_id IS NULL", schema_id.GetIndex());
	} else {
		result += " AND schema_id IS NULL AND table_id IS NULL";
	}
	return result;
}

string ScopeValues(optional_idx schema_id, optional_idx table_id) {
	string schema_value = schema_id.IsValid() ? to_string(schema_id.GetIndex()) : "NULL";
	string table_value = table_id.IsValid() ? to_string(table_id.GetIndex()) : "NULL";
	return StringUtil::Format("%s, %s", schema_value, table_value);
}

idx_t NextId(DuckLakeMetadataManager &manager, const string &table, const string &column) {
	string query = StringUtil::Format("SELECT COALESCE(MAX(%s), 0) + 1 FROM {METADATA_CATALOG}.%s", column, table);
	auto result = manager.Query(query);
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to allocate DuckLake RBAC id: ");
	}
	for (auto &row : *result) {
		return row.GetValue<idx_t>(0);
	}
	return 1;
}

static void RbacMutateExecute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = const_cast<RbacFunctionData &>(data_p.bind_data->Cast<RbacFunctionData>());
	if (bind_data.finished) {
		return;
	}
	auto &duck_catalog = bind_data.catalog.Cast<DuckLakeCatalog>();
	duck_catalog.Rbac().CheckAdmin(context);
	auto &transaction = DuckLakeTransaction::Get(context, bind_data.catalog);
	auto &manager = transaction.GetMetadataManager();
	auto name = EscapeSingleQuote(bind_data.name);

	switch (bind_data.action) {
	case RbacAction::CREATE_ROLE: {
		string query = StringUtil::Format(
		    "INSERT INTO {METADATA_CATALOG}.ducklake_role VALUES (%llu, '%s')",
		    NextId(manager, "ducklake_role", "role_id"), name);
		auto result = manager.Query(query);
		if (result->HasError()) {
			auto message = result->GetErrorObject().RawMessage();
			if (message.find("duplicate key") != string::npos ||
			    StringUtil::Contains(StringUtil::Lower(message), "unique")) {
				throw InvalidInputException("Role \"%s\" already exists", bind_data.name);
			}
			result->GetErrorObject().Throw("Failed to create DuckLake role: ");
		}
		break;
	}
	case RbacAction::DROP_ROLE: {
		string query = StringUtil::Format("DELETE FROM {METADATA_CATALOG}.ducklake_role WHERE role_name = '%s'", name);
		auto result = manager.Query(query);
		if (result->HasError()) {
			result->GetErrorObject().Throw("Failed to drop DuckLake role: ");
		}
		string drop_grants = StringUtil::Format("DELETE FROM {METADATA_CATALOG}.ducklake_grant WHERE grantee = '%s'",
		                                        name);
		result = manager.Query(drop_grants);
		if (result->HasError()) {
			result->GetErrorObject().Throw("Failed to drop DuckLake role grants: ");
		}
		break;
	}
	case RbacAction::GRANT: {
		string predicate = GrantScopePredicate(bind_data.name, bind_data.schema_id, bind_data.table_id);
		string query = StringUtil::Format(
		    "SELECT 1 FROM {METADATA_CATALOG}.ducklake_grant WHERE %s LIMIT 1", predicate);
		auto result = manager.Query(query);
		if (result->HasError()) {
			result->GetErrorObject().Throw("Failed to grant DuckLake privileges: ");
		}
		bool exists = false;
		for (auto &row : *result) {
			(void)row;
			exists = true;
			break;
		}
		if (exists) {
			query = StringUtil::Format(
			    "UPDATE {METADATA_CATALOG}.ducklake_grant SET privileges = privileges | %llu WHERE %s",
			    bind_data.privileges, predicate);
		} else {
			query = StringUtil::Format("INSERT INTO {METADATA_CATALOG}.ducklake_grant VALUES (%llu, '%s', %s, %llu)",
			                           NextId(manager, "ducklake_grant", "grant_id"), name,
			                           ScopeValues(bind_data.schema_id, bind_data.table_id), bind_data.privileges);
		}
		result = manager.Query(query);
		if (result->HasError()) {
			result->GetErrorObject().Throw("Failed to grant DuckLake privileges: ");
		}
		break;
	}
	case RbacAction::REVOKE: {
		string predicate = GrantScopePredicate(bind_data.name, bind_data.schema_id, bind_data.table_id);
		string query = StringUtil::Format(
		    "UPDATE {METADATA_CATALOG}.ducklake_grant SET privileges = privileges & %llu WHERE %s",
		    static_cast<uint64_t>(~bind_data.privileges), predicate);
		auto result = manager.Query(query);
		if (result->HasError()) {
			result->GetErrorObject().Throw("Failed to revoke DuckLake privileges: ");
		}
		// drop rows that no longer carry any privilege
		query = StringUtil::Format("DELETE FROM {METADATA_CATALOG}.ducklake_grant WHERE privileges = 0");
		result = manager.Query(query);
		if (result->HasError()) {
			result->GetErrorObject().Throw("Failed to revoke DuckLake privileges: ");
		}
		break;
	}
	}
	duck_catalog.Rbac().InvalidateCache();
	bind_data.finished = true;

	output.SetValue(0, 0, Value::BOOLEAN(true));
	output.SetCardinality(1);
}

struct RbacFunctionState : public GlobalTableFunctionState {
};

static unique_ptr<GlobalTableFunctionState> RbacMutateInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<RbacFunctionState>();
}

} // namespace

// ------------------------------------------------------------------------ //
// ducklake_create_role(catalog, role) / ducklake_drop_role(catalog, role)
// ------------------------------------------------------------------------ //

static unique_ptr<FunctionData> RoleBind(RbacAction action, ClientContext &context, TableFunctionBindInput &input,
                                         vector<LogicalType> &return_types, vector<string> &names) {
	auto &catalog = DuckLakeBaseMetadataFunction::GetCatalog(context, input.inputs[0]);
	auto role = StringValue::Get(input.inputs[1]);
	if (role.empty() || StringUtil::CIEquals(role, DUCKLAKE_PUBLIC_GRANTEE)) {
		throw InvalidInputException("Invalid role name \"%s\"", role);
	}
	return_types.push_back(LogicalType::BOOLEAN);
	names.push_back("Success");
	auto data = make_uniq<RbacFunctionData>(catalog, action);
	data->name = std::move(role);
	return data;
}

static unique_ptr<FunctionData> CreateRoleBind(ClientContext &context, TableFunctionBindInput &input,
                                               vector<LogicalType> &return_types, vector<string> &names) {
	return RoleBind(RbacAction::CREATE_ROLE, context, input, return_types, names);
}

static unique_ptr<FunctionData> DropRoleBind(ClientContext &context, TableFunctionBindInput &input,
                                             vector<LogicalType> &return_types, vector<string> &names) {
	return RoleBind(RbacAction::DROP_ROLE, context, input, return_types, names);
}

DuckLakeCreateRoleFunction::DuckLakeCreateRoleFunction()
    : TableFunction("ducklake_create_role", {LogicalType::VARCHAR, LogicalType::VARCHAR}, RbacMutateExecute,
                    CreateRoleBind, RbacMutateInit) {
}

DuckLakeDropRoleFunction::DuckLakeDropRoleFunction()
    : TableFunction("ducklake_drop_role", {LogicalType::VARCHAR, LogicalType::VARCHAR}, RbacMutateExecute, DropRoleBind,
                    RbacMutateInit) {
}

// ------------------------------------------------------------------------ //
// ducklake_grant / ducklake_revoke
// (catalog, grantee, privileges [, schema=..] [, table_name=..])
// ------------------------------------------------------------------------ //

static unique_ptr<FunctionData> GrantBind(RbacAction action, ClientContext &context, TableFunctionBindInput &input,
                                          vector<LogicalType> &return_types, vector<string> &names) {
	auto &catalog = DuckLakeBaseMetadataFunction::GetCatalog(context, input.inputs[0]);
	auto grantee = StringValue::Get(input.inputs[1]);
	if (grantee.empty()) {
		throw InvalidInputException("Grantee cannot be empty");
	}
	auto privileges = DuckLakeRbac::ParsePrivileges(StringValue::Get(input.inputs[2]));
	if (!StringUtil::CIEquals(grantee, DUCKLAKE_PUBLIC_GRANTEE)) {
		// hardening: grantees must be existing roles (or the reserved PUBLIC)
		auto &transaction = DuckLakeTransaction::Get(context, catalog);
		string role_query = StringUtil::Format(
		    "SELECT 1 FROM {METADATA_CATALOG}.ducklake_role WHERE role_name = '%s' LIMIT 1",
		    EscapeSingleQuote(grantee));
		auto role_result = transaction.GetMetadataManager().Query(role_query);
		if (role_result->HasError()) {
			role_result->GetErrorObject().Throw("Failed to look up DuckLake role: ");
		}
		bool role_exists = false;
		for (auto &row : *role_result) {
			(void)row;
			role_exists = true;
			break;
		}
		if (!role_exists) {
			throw InvalidInputException("Role \"%s\" does not exist - create it with ducklake_create_role first",
			                            grantee);
		}
	}
	optional_idx schema_id;
	optional_idx table_id;
	ResolveGrantScope(catalog, context, input, schema_id, table_id);
	return_types.push_back(LogicalType::BOOLEAN);
	names.push_back("Success");
	auto data = make_uniq<RbacFunctionData>(catalog, action);
	data->name = std::move(grantee);
	data->privileges = privileges;
	data->schema_id = schema_id;
	data->table_id = table_id;
	return data;
}

static unique_ptr<FunctionData> GrantPrivilegesBind(ClientContext &context, TableFunctionBindInput &input,
                                                    vector<LogicalType> &return_types, vector<string> &names) {
	return GrantBind(RbacAction::GRANT, context, input, return_types, names);
}

static unique_ptr<FunctionData> RevokePrivilegesBind(ClientContext &context, TableFunctionBindInput &input,
                                                     vector<LogicalType> &return_types, vector<string> &names) {
	return GrantBind(RbacAction::REVOKE, context, input, return_types, names);
}

static void GrantFunctionParameters(TableFunction &function) {
	function.named_parameters["schema"] = LogicalType::VARCHAR;
	function.named_parameters["table_name"] = LogicalType::VARCHAR;
}

DuckLakeGrantFunction::DuckLakeGrantFunction()
    : TableFunction("ducklake_grant", {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
                    RbacMutateExecute, GrantPrivilegesBind, RbacMutateInit) {
	// set in the constructor body below via GrantFunctionParameters
	GrantFunctionParameters(*this);
}

DuckLakeRevokeFunction::DuckLakeRevokeFunction()
    : TableFunction("ducklake_revoke", {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
                    RbacMutateExecute, RevokePrivilegesBind, RbacMutateInit) {
	GrantFunctionParameters(*this);
}

// ------------------------------------------------------------------------ //
// ducklake_roles(catalog) / ducklake_grants(catalog)
// ------------------------------------------------------------------------ //

static unique_ptr<FunctionData> RolesBind(ClientContext &context, TableFunctionBindInput &input,
                                          vector<LogicalType> &return_types, vector<string> &names) {
	auto &catalog = DuckLakeBaseMetadataFunction::GetCatalog(context, input.inputs[0]);
	auto &transaction = DuckLakeTransaction::Get(context, catalog);
	string query = "SELECT role_id, role_name FROM {METADATA_CATALOG}.ducklake_role ORDER BY role_id";
	auto result = transaction.GetMetadataManager().Query(query);
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to list DuckLake roles: ");
	}
	auto bind_data = make_uniq<MetadataBindData>();
	for (auto &row : *result) {
		bind_data->rows.emplace_back(vector<Value> {
		    Value::BIGINT(NumericCast<int64_t>(row.GetValue<idx_t>(0))),
		    Value(row.GetValue<string>(1)),
		});
	}
	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("role_id");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("role_name");
	return bind_data;
}

static unique_ptr<FunctionData> GrantsBind(ClientContext &context, TableFunctionBindInput &input,
                                           vector<LogicalType> &return_types, vector<string> &names) {
	auto &catalog = DuckLakeBaseMetadataFunction::GetCatalog(context, input.inputs[0]);
	auto &transaction = DuckLakeTransaction::Get(context, catalog);
	string query =
	    "SELECT grantee, schema_id, table_id, privileges FROM {METADATA_CATALOG}.ducklake_grant ORDER BY grantee, "
	    "grant_id";
	auto result = transaction.GetMetadataManager().Query(query);
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to list DuckLake grants: ");
	}
	auto bind_data = make_uniq<MetadataBindData>();
	for (auto &row : *result) {
		Value schema_id = row.IsNull(1) ? Value() : Value::BIGINT(NumericCast<int64_t>(row.GetValue<idx_t>(1)));
		Value table_id = row.IsNull(2) ? Value() : Value::BIGINT(NumericCast<int64_t>(row.GetValue<idx_t>(2)));
		bind_data->rows.emplace_back(vector<Value> {
		    Value(row.GetValue<string>(0)),
		    std::move(schema_id),
		    std::move(table_id),
		    Value(DuckLakeRbac::PrivilegesToString(row.GetValue<uint64_t>(3))),
		});
	}
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("grantee");
	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("schema_id");
	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("table_id");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("privileges");
	return bind_data;
}

DuckLakeRolesFunction::DuckLakeRolesFunction() : DuckLakeBaseMetadataFunction("ducklake_roles", RolesBind) {
}

DuckLakeGrantsFunction::DuckLakeGrantsFunction() : DuckLakeBaseMetadataFunction("ducklake_grants", GrantsBind) {
}

} // namespace duckdb
