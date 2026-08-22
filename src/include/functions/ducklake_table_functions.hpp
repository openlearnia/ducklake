//===----------------------------------------------------------------------===//
//                         DuckDB
//
// functions/ducklake_table_functions.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/parsed_data/create_macro_info.hpp"
#include "duckdb/function/function_set.hpp"

namespace duckdb {
class DuckLakeCatalog;
struct DuckLakeSnapshotInfo;

class DuckLakeTableFunctionUtil {
public:
	// Conform timestamp to ISO-8601 extended format with optional fractional seconds and timezone offset, e.g.:
	// "2025-12-26T06:13:30.673176+00:00" (UTC) or "2025-12-26T01:13:30.673176-05:00" (EST)
	static string FormatTimestampISO8601(const timestamp_t timestamp) {
		auto ts_string = Timestamp::ToString(timestamp);
		std::replace(ts_string.begin(), ts_string.end(), ' ', 'T');
		return ts_string + "+00";
	}
};

struct MetadataBindData : public TableFunctionData {
	MetadataBindData() {
	}

	vector<vector<Value>> rows;
};

class DuckLakeBaseMetadataFunction : public TableFunction {
public:
	DuckLakeBaseMetadataFunction(string name, table_function_bind_t bind);

	static Catalog &GetCatalog(ClientContext &context, const Value &input);
};

class DuckLakeSnapshotsFunction : public DuckLakeBaseMetadataFunction {
public:
	DuckLakeSnapshotsFunction();

	static void GetSnapshotTypes(vector<LogicalType> &return_types, vector<string> &names);
	static vector<Value> GetSnapshotValues(const DuckLakeSnapshotInfo &snapshot);
};

class DuckLakeTableInfoFunction : public DuckLakeBaseMetadataFunction {
public:
	DuckLakeTableInfoFunction();
};

class DuckLakeTableInsertionsFunction {
public:
	static TableFunctionSet GetFunctions();
	static unique_ptr<CreateMacroInfo> GetDuckLakeTableChanges();
};

class DuckLakeTableDeletionsFunction {
public:
	static TableFunctionSet GetFunctions();
};

class DuckLakeMergeAdjacentFilesFunction : public TableFunction {
public:
	static TableFunctionSet GetFunctions();
};

class DuckLakeRewriteDataFilesFunction : public TableFunction {
public:
	static TableFunctionSet GetFunctions();
};

class DuckLakeCleanupOldFilesFunction : public TableFunction {
public:
	DuckLakeCleanupOldFilesFunction();
};

class DuckLakeCleanupOrphanedFilesFunction : public TableFunction {
public:
	DuckLakeCleanupOrphanedFilesFunction();
};

class DuckLakeExpireSnapshotsFunction : public TableFunction {
public:
	DuckLakeExpireSnapshotsFunction();
};

class DuckLakeFlushInlinedDataFunction : public TableFunction {
public:
	DuckLakeFlushInlinedDataFunction();
};

class DuckLakeSetOptionFunction : public TableFunction {
public:
	DuckLakeSetOptionFunction();
};

class DuckLakeSetCommitMessage : public TableFunction {
public:
	DuckLakeSetCommitMessage();
};

class DuckLakeOptionsFunction : public DuckLakeBaseMetadataFunction {
public:
	DuckLakeOptionsFunction();
};

class DuckLakeLastCommittedSnapshotFunction : public DuckLakeBaseMetadataFunction {
public:
	DuckLakeLastCommittedSnapshotFunction();
};

class DuckLakeListFilesFunction : public DuckLakeBaseMetadataFunction {
public:
	DuckLakeListFilesFunction();
};

class DuckLakeCurrentSnapshotFunction : public DuckLakeBaseMetadataFunction {
public:
	DuckLakeCurrentSnapshotFunction();
};

class DuckLakeAddDataFilesFunction : public TableFunction {
public:
	static TableFunctionSet GetFunctions();
};

class DuckLakeSettingsFunction : public DuckLakeBaseMetadataFunction {
public:
	DuckLakeSettingsFunction();
};

class DuckLakeCommitFunction : public TableFunction {
public:
	DuckLakeCommitFunction();
};

class DuckLakeCreateMaterializedViewFunction : public TableFunction {
public:
	DuckLakeCreateMaterializedViewFunction();
};

class DuckLakeRefreshMaterializedViewFunction : public TableFunction {
public:
	DuckLakeRefreshMaterializedViewFunction();
};

class DuckLakeDropMaterializedViewFunction : public TableFunction {
public:
	DuckLakeDropMaterializedViewFunction();
};

class DuckLakeMaterializedViewsFunction : public DuckLakeBaseMetadataFunction {
public:
	DuckLakeMaterializedViewsFunction();
};

//! Role management: ducklake_create_role(catalog, role)
class DuckLakeCreateRoleFunction : public TableFunction {
public:
	DuckLakeCreateRoleFunction();
};

//! ducklake_drop_role(catalog, role)
class DuckLakeDropRoleFunction : public TableFunction {
public:
	DuckLakeDropRoleFunction();
};

//! ducklake_grant(catalog, grantee, privileges [, schema=..] [, table_name=..])
class DuckLakeGrantFunction : public TableFunction {
public:
	DuckLakeGrantFunction();
};

//! ducklake_revoke(catalog, grantee, privileges [, schema=..] [, table_name=..])
class DuckLakeRevokeFunction : public TableFunction {
public:
	DuckLakeRevokeFunction();
};

//! ducklake_roles(catalog) - list all roles
class DuckLakeRolesFunction : public DuckLakeBaseMetadataFunction {
public:
	DuckLakeRolesFunction();
};

//! ducklake_grants(catalog) - list all grants
class DuckLakeGrantsFunction : public DuckLakeBaseMetadataFunction {
public:
	DuckLakeGrantsFunction();
};

} // namespace duckdb
