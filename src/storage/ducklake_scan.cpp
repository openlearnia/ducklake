#include "storage/ducklake_scan.hpp"
#include "storage/ducklake_catalog.hpp"
#include "storage/ducklake_multi_file_reader.hpp"
#include "storage/ducklake_multi_file_list.hpp"
#include "storage/ducklake_table_entry.hpp"
#include "storage/ducklake_stats.hpp"
#include "storage/ducklake_transaction.hpp"

#include "duckdb/catalog/catalog_entry/table_function_catalog_entry.hpp"
#include "duckdb/common/multi_file/multi_file_data.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/function/partition_stats.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/main/query_profiler.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/parser/expression/function_expression.hpp"

namespace duckdb {

static InsertionOrderPreservingMap<string> DuckLakeFunctionToString(TableFunctionToStringInput &input) {
	InsertionOrderPreservingMap<string> result;

	if (input.table_function.function_info) {
		auto &table_info = input.table_function.function_info->Cast<DuckLakeFunctionInfo>();
		result["Table"] = table_info.table_name;
	}

	return result;
}

unique_ptr<BaseStatistics> DuckLakeStatistics(ClientContext &context, const FunctionData *bind_data,
                                              column_t column_index) {
	if (IsVirtualColumn(column_index)) {
		return nullptr;
	}
	auto &multi_file_data = bind_data->Cast<MultiFileBindData>();
	auto &file_list = multi_file_data.file_list->Cast<DuckLakeMultiFileList>();
	if (file_list.HasTransactionLocalData()) {
		// don't read stats if we have transaction-local inserts
		// FIXME: we could unify the stats with the global stats
		return nullptr;
	}
	auto &table = file_list.GetTable();
	return table.GetStatistics(context, column_index);
}

BindInfo DuckLakeBindInfo(const optional_ptr<FunctionData> bind_data) {
	auto &multi_file_data = bind_data->Cast<MultiFileBindData>();
	auto &file_list = multi_file_data.file_list->Cast<DuckLakeMultiFileList>();
	return BindInfo(file_list.GetTable());
}

virtual_column_map_t DuckLakeVirtualColumns(ClientContext &context, optional_ptr<FunctionData> bind_data_p) {
	auto &bind_data = bind_data_p->Cast<MultiFileBindData>();
	auto &file_list = bind_data.file_list->Cast<DuckLakeMultiFileList>();
	auto result = file_list.GetTable().GetVirtualColumns();
	bind_data.virtual_columns = result;
	return result;
}

vector<column_t> DuckLakeGetRowIdColumn(ClientContext &context, optional_ptr<FunctionData> bind_data) {
	vector<column_t> result;
	result.emplace_back(MultiFileReader::COLUMN_IDENTIFIER_FILENAME);
	result.emplace_back(MultiFileReader::COLUMN_IDENTIFIER_FILE_ROW_NUMBER);
	return result;
}

// Exposes DuckLake catalog column statistics to the optimizer's MIN/MAX aggregate pushdown.
// DuckDB's StatisticsPropagator::TryExecuteAggregates folds MIN(col)/MAX(col) over a single scan into a
// constant when the partition exposes (exact) column statistics through this interface.
struct DuckLakePartitionRowGroup : public PartitionRowGroup {
	DuckLakePartitionRowGroup(ClientContext &context, DuckLakeTableEntry &table, bool min_max_exact)
	    : context(context), table(table), min_max_exact(min_max_exact) {
	}

	ClientContext &context;
	DuckLakeTableEntry &table;
	bool min_max_exact;

	unique_ptr<BaseStatistics> GetColumnStatistics(const StorageIndex &storage_index) override {
		if (storage_index.HasChildren()) {
			// MIN/MAX over a nested sub-field - we only track stats for top-level columns, fall back to a scan
			return nullptr;
		}
		return table.GetStatistics(context, storage_index.GetPrimaryIndex());
	}

	bool MinMaxIsExact(const StorageIndex &storage_index) override {
		return min_max_exact;
	}
	bool HasPendingWrites() override { return false; }
};

vector<PartitionStatistics> DuckLakeGetPartitionStats(ClientContext &context, GetPartitionStatsInput &input) {
	vector<PartitionStatistics> result;

	if (!input.table_function.function_info) {
		return result;
	}
	auto &func_info = input.table_function.function_info->Cast<DuckLakeFunctionInfo>();

	// Only use partition stats for regular table scans
	if (func_info.scan_type != DuckLakeScanType::SCAN_TABLE) {
		return result;
	}

	auto &bind_data = input.bind_data->Cast<MultiFileBindData>();
	auto &file_list = bind_data.file_list->Cast<DuckLakeMultiFileList>();
	auto &table = file_list.GetTable();
	auto transaction = func_info.GetTransaction();

	auto table_id = table.GetTableId();

	// Check if this is a time travel query - if so, fall back to scanning
	// After merge_adjacent_files, multiple files are merged into one with a combined record_count.
	// The merged file contains an embedded snapshot_id column for time travel filtering,
	// but the metadata record_count represents ALL rows, not per-snapshot counts.
	// Only a full scan can filter by snapshot_id to get the correct historical count.
	// Time travel can occur via: (1) per-query AT clause, or (2) catalog attached at historical snapshot
	auto current_snapshot = transaction->GetSnapshot();
	if (func_info.snapshot.snapshot_id != current_snapshot.snapshot_id || transaction->GetCatalog().CatalogSnapshot()) {
		return result;
	}

	// Check if this is a transaction-local table (no committed stats)
	if (table.IsTransactionLocal()) {
		return result;
	}

	// If there are any transaction-local changes fall back to scanning
	// Accounting for transaction local changes gets difficult, especially when entire
	// files are dropped.
	if (transaction->HasAnyLocalChanges(table_id)) {
		return result;
	}

	idx_t net_count = table.GetNetDataFileRowCount(*transaction) + table.GetNetInlinedRowCount(*transaction);

	// MIN/MAX can be answered from the catalog column stats, but only when those stats are exact.
	// Global column stats only ever widen on insert (via MergeStats) and are never tightened by deletes
	// or compaction - so they are exact for the live data iff no row has ever been deleted, i.e. the gross
	// record_count (total ever inserted) equals the net (delete-adjusted) row count. count(*) is unaffected
	// either way: it does not consult MinMaxIsExact and subtracts delete counts independently.
	auto table_stats = table.GetTableStats(*transaction);
	bool min_max_exact = table_stats && table_stats->record_count == net_count;

	// Return single partition with total count
	PartitionStatistics stats;
	stats.count = net_count;
	stats.count_type = CountType::COUNT_EXACT;
	stats.partition_row_group = make_shared_ptr<DuckLakePartitionRowGroup>(context, table, min_max_exact);
	result.push_back(std::move(stats));
	return result;
}

TableFunction DuckLakeFunctions::GetDuckLakeScanFunction(DatabaseInstance &instance, const string &file_format) {
	// Construct ducklake_scan from the selected format's MultiFileFunction and inject DuckLake's file reader.
	if (file_format == "parquet") {
		ExtensionHelper::TryAutoLoadExtension(instance, "parquet");
	}
	ExtensionLoader loader(instance, "ducklake");

	TableFunction function("ducklake_scan", {LogicalType::VARCHAR}, nullptr, nullptr);
	auto scan_name = file_format == "vortex" ? "vortex_multi_file_scan" : "parquet_scan";
	auto scan_entry = loader.TryGetTableFunction(scan_name);
	if (scan_entry) {
		auto &scan = scan_entry->Cast<TableFunctionCatalogEntry>();
		function = scan.functions.GetFunctionByOffset(0);
		function.get_multi_file_reader = DuckLakeMultiFileReader::CreateInstance;
	}

	function.statistics = DuckLakeStatistics;
	function.get_bind_info = DuckLakeBindInfo;
	function.get_virtual_columns = DuckLakeVirtualColumns;
	function.get_row_id_columns = DuckLakeGetRowIdColumn;
	function.get_partition_stats = DuckLakeGetPartitionStats;

	function.serialize = DuckLakeScanSerialize;
	function.deserialize = DuckLakeScanDeserialize;

	function.to_string = DuckLakeFunctionToString;

	function.name = "ducklake_scan";
	return function;
}

DuckLakeFunctionInfo::DuckLakeFunctionInfo(DuckLakeTableEntry &table, DuckLakeTransaction &transaction_p,
                                           DuckLakeSnapshot snapshot)
    : table(table), transaction(transaction_p.shared_from_this()), snapshot(snapshot) {
}

shared_ptr<DuckLakeFunctionInfo>
DuckLakeFunctionInfo::Create(DuckLakeTableEntry &table, DuckLakeTransaction &transaction, DuckLakeSnapshot snapshot) {
	auto result = make_shared_ptr<DuckLakeFunctionInfo>(table, transaction, snapshot);
	result->table_name = table.name.GetIdentifierName();
	for (auto &col : table.GetColumns().Logical()) {
		result->column_names.push_back(col.Name().GetIdentifierName());
		result->column_types.push_back(col.Type());
	}
	result->table_id = table.GetTableId();
	return result;
}

shared_ptr<DuckLakeTransaction> DuckLakeFunctionInfo::GetTransaction() {
	auto result = transaction.lock();
	if (!result) {
		throw NotImplementedException(
		    "Scanning a DuckLake table after the transaction has ended - this use case is not yet supported");
	}
	return result;
}

void DuckLakeScanSerialize(Serializer &serializer, const optional_ptr<FunctionData> bind_data,
                           const TableFunction &function) {
	auto &func_info = function.function_info->Cast<DuckLakeFunctionInfo>();
	auto &catalog = func_info.table.ParentCatalog();
	serializer.WriteProperty(100, "catalog_name", catalog.GetName());
	serializer.WriteProperty(101, "schema_name", func_info.table.ParentSchema().name);
	serializer.WriteProperty(102, "table_name", func_info.table_name);
	serializer.WriteObject(103, "snapshot", [&](Serializer &obj) { func_info.snapshot.Serialize(obj); });
	serializer.WriteProperty(104, "scan_type", static_cast<uint8_t>(func_info.scan_type));
	bool has_start_snapshot = func_info.start_snapshot != nullptr;
	serializer.WriteProperty(105, "has_start_snapshot", has_start_snapshot);
	if (has_start_snapshot) {
		serializer.WriteObject(106, "start_snapshot",
		                       [&](Serializer &obj) { func_info.start_snapshot->Serialize(obj); });
	}
	serializer.WriteProperty(107, "file_format", func_info.file_format);
}

unique_ptr<FunctionData> DuckLakeScanDeserialize(Deserializer &deserializer, TableFunction &function) {
	auto &context = deserializer.Get<ClientContext &>();
	auto catalog_name = deserializer.ReadProperty<string>(100, "catalog_name");
	auto schema_name = deserializer.ReadProperty<string>(101, "schema_name");
	auto table_name = deserializer.ReadProperty<string>(102, "table_name");
	DuckLakeSnapshot snapshot;
	deserializer.ReadObject(103, "snapshot", [&](Deserializer &obj) { snapshot = DuckLakeSnapshot::Deserialize(obj); });
	auto scan_type = static_cast<DuckLakeScanType>(deserializer.ReadPropertyWithExplicitDefault<uint8_t>(
	    104, "scan_type", static_cast<uint8_t>(DuckLakeScanType::SCAN_TABLE)));
	auto has_start_snapshot = deserializer.ReadPropertyWithExplicitDefault<bool>(105, "has_start_snapshot", false);
	unique_ptr<DuckLakeSnapshot> start_snapshot;
	if (has_start_snapshot) {
		deserializer.ReadObject(106, "start_snapshot", [&](Deserializer &obj) {
			start_snapshot = make_uniq<DuckLakeSnapshot>(DuckLakeSnapshot::Deserialize(obj));
		});
	}
	auto file_format = deserializer.ReadPropertyWithExplicitDefault<string>(107, "file_format", "parquet");

	function = DuckLakeFunctions::GetDuckLakeScanFunction(*context.db, file_format);
	if (!function.bind) {
		throw InvalidInputException("ducklake_scan requires the %s extension to be loaded", file_format);
	}

	// Look up the DuckLake catalog and table
	auto &catalog = Catalog::GetCatalog(context, Identifier(catalog_name));
	auto &transaction = DuckLakeTransaction::Get(context, catalog);

		auto &table_entry = Catalog::GetEntry<TableCatalogEntry>(context,
		    QualifiedName(Identifier(catalog_name), Identifier(schema_name), Identifier(table_name))).Cast<DuckLakeTableEntry>();

	auto function_info = DuckLakeFunctionInfo::Create(table_entry, transaction, snapshot);
	function_info->scan_type = scan_type;
	function_info->file_format = file_format;
	function_info->start_snapshot = std::move(start_snapshot);
	function.function_info = std::move(function_info);

	return DuckLakeFunctions::BindDuckLakeScan(context, function);
}

} // namespace duckdb
