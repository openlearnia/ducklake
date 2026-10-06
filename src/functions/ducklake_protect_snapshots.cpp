#include "functions/ducklake_table_functions.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "storage/ducklake_catalog.hpp"
#include "storage/ducklake_rbac.hpp"
#include "storage/ducklake_transaction.hpp"
#include "common/ducklake_util.hpp"

namespace duckdb {

struct ProtectSnapshotsBindData : public TableFunctionData {
	explicit ProtectSnapshotsBindData(Catalog &catalog) : catalog(catalog) {
	}

	Catalog &catalog;
	vector<DuckLakeSnapshotInfo> snapshots;
	vector<idx_t> snapshot_ids;
	bool protect = true;
	bool valid = true;
};

static unique_ptr<FunctionData> DuckLakeProtectSnapshotsBind(ClientContext &context, TableFunctionBindInput &input,
                                                             vector<LogicalType> &return_types,
                                                             vector<Identifier> &names) {
	auto &catalog = DuckLakeBaseMetadataFunction::GetCatalog(context, input);
	auto result = make_uniq<ProtectSnapshotsBindData>(catalog);
	DuckLakeSnapshotsFunction::GetSnapshotTypes(return_types, names);

	timestamp_tz_t from_timestamp;
	string snapshot_list;
	bool has_timestamp = false;
	bool has_versions = false;
	for (auto &entry : input.named_parameters) {
		if (entry.first == "versions") {
			has_versions = true;
			if (entry.second.IsNull()) {
				continue;
			}
			for (auto &snapshot_id : ListValue::GetChildren(entry.second)) {
				if (snapshot_id.IsNull()) {
					continue;
				}
				if (!snapshot_list.empty()) {
					snapshot_list += ", ";
				}
				snapshot_list += snapshot_id.ToString();
			}
		} else if (entry.first == "older_than") {
			if (entry.second.IsNull()) {
				throw BinderException("The older_than option must be a non-null timestamp.");
			}
			from_timestamp = entry.second.GetValue<timestamp_tz_t>();
			has_timestamp = true;
		} else if (entry.first == "protect") {
			if (entry.second.IsNull()) {
				throw BinderException("The protect option must be a non-null boolean.");
			}
			result->protect = BooleanValue::Get(entry.second);
		} else {
			throw InternalException("Unsupported named parameter for ducklake_protect_snapshots");
		}
	}
	if (has_versions && has_timestamp) {
		throw InvalidInputException(
		    "ducklake_protect_snapshots: cannot specify both 'versions' and 'older_than' parameters at the "
		    "same time. Please use only one criterion.");
	}
	// An explicitly empty version set selects no snapshots.
	if (has_versions && snapshot_list.empty()) {
		result->valid = false;
		return std::move(result);
	}
	// No criteria given: silently no-op.
	if (!has_versions && !has_timestamp) {
		result->valid = false;
		return std::move(result);
	}

	string filter;
	if (has_timestamp) {
		auto timestamp_filter = DuckLakeTableFunctionUtil::FormatTimestampISO8601(timestamp_t(from_timestamp.value));
		filter += StringUtil::Format("snapshot_time::TIMESTAMPTZ < '%s'", timestamp_filter);
	} else {
		filter += StringUtil::Format("snapshot_id IN (%s)", snapshot_list);
	}
	// Unlike expiry, this does not exclude the most recent snapshot: a hold is a statement about
	// the current state of the lake, and holding the newest snapshot is the common case.
	auto &transaction = DuckLakeTransaction::Get(context, catalog);
	auto &metadata_manager = transaction.GetMetadataManager();
	auto snapshots = metadata_manager.GetAllSnapshots(filter);

	vector<idx_t> snapshot_ids;
	vector<DuckLakeSnapshotInfo> selected;
	for (auto &snapshot : snapshots) {
		if (result->protect == snapshot.is_protected) {
			// already in the requested state, so no write is needed
			continue;
		}
		snapshot.is_protected = result->protect;
		snapshot_ids.push_back(snapshot.id);
		selected.push_back(std::move(snapshot));
	}
	result->snapshot_ids = std::move(snapshot_ids);
	result->snapshots = std::move(selected);
	return std::move(result);
}

struct DuckLakeProtectSnapshotsData : public GlobalTableFunctionState {
	DuckLakeProtectSnapshotsData() : offset(0), executed(false) {
	}

	idx_t offset;
	bool executed;
};

unique_ptr<GlobalTableFunctionState> DuckLakeProtectSnapshotsInit(ClientContext &context,
                                                                  TableFunctionInitInput &input) {
	return make_uniq<DuckLakeProtectSnapshotsData>();
}

void DuckLakeProtectSnapshotsExecute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.bind_data->Cast<ProtectSnapshotsBindData>();
	if (!data.valid) {
		return;
	}
	auto &state = data_p.global_state->Cast<DuckLakeProtectSnapshotsData>();
	if (!state.executed) {
		// Releasing a hold is as consequential as expiring a snapshot, so both need ADMIN.
		auto &ducklake_catalog = data.catalog.Cast<DuckLakeCatalog>();
		ducklake_catalog.Rbac().CheckAdmin(context);
		auto &transaction = DuckLakeTransaction::Get(context, data.catalog);
		transaction.GetMetadataManager().SetSnapshotProtection(data.snapshot_ids, data.protect);
		state.executed = true;
	}
	idx_t count = 0;
	while (state.offset < data.snapshots.size() && count < STANDARD_VECTOR_SIZE) {
		auto row_values = DuckLakeSnapshotsFunction::GetSnapshotValues(data.snapshots[state.offset++]);
		for (idx_t col_idx = 0; col_idx < row_values.size(); col_idx++) {
			output.data[col_idx].Append(row_values[col_idx]);
		}
		count++;
	}
	output.SetChildCardinality(count);
}

DuckLakeProtectSnapshotsFunction::DuckLakeProtectSnapshotsFunction()
    : TableFunction(Identifier("ducklake_protect_snapshots"), {LogicalType::VARCHAR}, DuckLakeProtectSnapshotsExecute,
                    DuckLakeProtectSnapshotsBind, DuckLakeProtectSnapshotsInit) {
	named_parameters["older_than"] = LogicalType::TIMESTAMP_TZ;
	named_parameters["versions"] = LogicalType::LIST(LogicalType::UBIGINT);
	named_parameters["protect"] = LogicalType::BOOLEAN;
}

} // namespace duckdb
