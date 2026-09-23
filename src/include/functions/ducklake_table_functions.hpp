//===----------------------------------------------------------------------===//
//                         DuckDB
//
// functions/ducklake_table_functions.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/function/table_function.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/parser/parsed_data/create_macro_info.hpp"
#include "duckdb/function/function_set.hpp"

namespace duckdb {
class DuckLakeCatalog;
class DuckLakeTableEntry;
class DuckLakeTransaction;
struct DuckLakeDataFile;
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
	DuckLakeBaseMetadataFunction(Identifier name, table_function_bind_t bind);

	static Catalog &GetCatalog(ClientContext &context, const Value &input);
};

class DuckLakeSnapshotsFunction : public DuckLakeBaseMetadataFunction {
public:
	DuckLakeSnapshotsFunction();

	static void GetSnapshotTypes(vector<LogicalType> &return_types, vector<Identifier> &names);
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

//! Reads file footers and builds DuckLakeDataFile entries (column stats, name maps) for external
//! files without registering them - shared between ducklake_add_data_files and share-mode replication.
vector<DuckLakeDataFile> DuckLakePrepareExternalFiles(DuckLakeTransaction &transaction, ClientContext &context,
                                                      Catalog &catalog, DuckLakeTableEntry &table,
                                                      const vector<string> &paths, const string &file_format);

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

class DuckLakeMaterializedViewRefreshHistoryFunction : public DuckLakeBaseMetadataFunction {
public:
	DuckLakeMaterializedViewRefreshHistoryFunction();
};

class DuckLakeReplicateCreateFunction : public TableFunction {
public:
	DuckLakeReplicateCreateFunction();
};

class DuckLakeReplicateCatchupFunction : public TableFunction {
public:
	DuckLakeReplicateCatchupFunction();
};

class DuckLakeReplicateStatusFunction : public TableFunction {
public:
	DuckLakeReplicateStatusFunction();
};

class DuckLakeReplicateTablesFunction : public TableFunction {
public:
	DuckLakeReplicateTablesFunction();
};

class DuckLakeReplicateDropFunction : public TableFunction {
public:
	DuckLakeReplicateDropFunction();
};

class DuckLakeReplicateStartFunction : public TableFunction {
public:
	DuckLakeReplicateStartFunction();
};

class DuckLakeReplicateStopFunction : public TableFunction {
public:
	DuckLakeReplicateStopFunction();
};

class DuckLakeReplicatePauseFunction : public TableFunction {
public:
	DuckLakeReplicatePauseFunction();
};

class DuckLakeReplicateResumeFunction : public TableFunction {
public:
	DuckLakeReplicateResumeFunction();
};

class DuckLakeReplicateResumeAllFunction : public TableFunction {
public:
	DuckLakeReplicateResumeAllFunction();
};

} // namespace duckdb
