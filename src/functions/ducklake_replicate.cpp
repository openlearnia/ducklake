//===----------------------------------------------------------------------===//
//                         DuckDB
//
// functions/ducklake_replicate.cpp
//
//
//===----------------------------------------------------------------------===//

#include "functions/ducklake_table_functions.hpp"
#include "replication/ducklake_replication.hpp"
#include "storage/ducklake_catalog.hpp"
#include "common/ducklake_util.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension_helper.hpp"

namespace duckdb {

// Loads the statically linked parquet extension when it is not registered yet (autoload may be disabled).
static void EnsureParquetLoaded(ClientContext &context) {
	auto &db = *context.db;
	if (db.ExtensionIsLoaded("parquet")) {
		return;
	}
	DuckDB instance(db);
	ExtensionHelper::LoadExtension(instance, "parquet");
}

//===--------------------------------------------------------------------===//
// Shared execution state
//===--------------------------------------------------------------------===//

struct ReplicateOnceState : public GlobalTableFunctionState {
	idx_t offset = 0;
	bool executed = false;
	uint64_t replication_id = 0;
	DuckLakeReplicationCatchupResult catchup_result;
};

static unique_ptr<GlobalTableFunctionState> ReplicateOnceInit(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<ReplicateOnceState>();
}

struct ReplicateRowsState : public GlobalTableFunctionState {
	idx_t offset = 0;
	bool loaded = false;
	vector<vector<Value>> rows;
};

static unique_ptr<GlobalTableFunctionState> ReplicateRowsInit(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<ReplicateRowsState>();
}

static void AppendSingletonRow(DataChunk &output, const vector<Value> &values) {
	for (idx_t c = 0; c < values.size(); c++) {
		output.data[c].Append(values[c]);
	}
	output.SetChildCardinality(1);
}

static void StreamRows(DataChunk &output, idx_t &offset, const vector<vector<Value>> &rows) {
	if (offset >= rows.size()) {
		output.SetChildCardinality(0);
		return;
	}
	idx_t count = 0;
	while (offset < rows.size() && count < STANDARD_VECTOR_SIZE) {
		auto &row = rows[offset++];
		for (idx_t c = 0; c < row.size(); c++) {
			output.data[c].Append(row[c]);
		}
		count++;
	}
	output.SetChildCardinality(count);
}

//===--------------------------------------------------------------------===//
// ducklake_replicate_create
//===--------------------------------------------------------------------===//

struct ReplicateCreateData : public TableFunctionData {
	Catalog *dest_catalog = nullptr;
	string source_catalog;
	uint64_t interval_ms = 5000;
	string data_sync_mode = "copy";
	string include_patterns;
	string exclude_patterns;
	string watermark_columns;
};

static unique_ptr<FunctionData> ReplicateCreateBind(ClientContext &context, TableFunctionBindInput &input,
                                                    vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto &catalog = DuckLakeBaseMetadataFunction::GetCatalog(context, input.inputs[0]);
	auto result = make_uniq<ReplicateCreateData>();
	result->dest_catalog = &catalog;
	if (input.inputs[1].IsNull()) {
		throw BinderException("Source catalog cannot be NULL");
	}
	result->source_catalog = input.inputs[1].GetValue<string>();

	for (auto &entry : input.named_parameters) {
		auto name = StringUtil::Lower(entry.first.GetIdentifierName());
		if (name == "interval_ms") {
			if (!entry.second.IsNull()) {
				auto interval_ms = entry.second.GetValue<int64_t>();
				if (interval_ms < 0) {
					throw BinderException("interval_ms cannot be negative");
				}
				result->interval_ms = static_cast<uint64_t>(interval_ms);
			}
		} else if (name == "data_sync_mode") {
			if (!entry.second.IsNull()) {
				result->data_sync_mode = entry.second.GetValue<string>();
			}
		} else if (name == "include") {
			if (!entry.second.IsNull()) {
				result->include_patterns = entry.second.GetValue<string>();
			}
		} else if (name == "exclude") {
			if (!entry.second.IsNull()) {
				result->exclude_patterns = entry.second.GetValue<string>();
			}
		} else if (name == "watermark_columns") {
			if (!entry.second.IsNull()) {
				result->watermark_columns = entry.second.GetValue<string>();
			}
		} else {
			throw BinderException("Unsupported named parameter for ducklake_replicate_create: %s", entry.first);
		}
	}

	names.emplace_back("replication_id");
	return_types.emplace_back(LogicalType::UBIGINT);
	return std::move(result);
}

static void ReplicateCreateExecute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.bind_data->Cast<ReplicateCreateData>();
	auto &state = data_p.global_state->Cast<ReplicateOnceState>();
	if (state.offset > 0) {
		output.SetChildCardinality(0);
		return;
	}
	if (!state.executed) {
		auto &ducklake = data.dest_catalog->Cast<DuckLakeCatalog>();
		state.replication_id = DuckLakeReplication::CreateJob(context, ducklake, data.source_catalog, data.interval_ms,
		                                                      data.data_sync_mode, data.include_patterns,
		                                                      data.exclude_patterns, data.watermark_columns);
		state.executed = true;
	}
	state.offset++;
	AppendSingletonRow(output, {Value::UBIGINT(state.replication_id)});
}

DuckLakeReplicateCreateFunction::DuckLakeReplicateCreateFunction()
    : TableFunction(Identifier("ducklake_replicate_create"), {LogicalType::VARCHAR, LogicalType::VARCHAR},
                    ReplicateCreateExecute, ReplicateCreateBind, ReplicateOnceInit) {
	named_parameters["interval_ms"] = LogicalType::BIGINT;
	named_parameters["data_sync_mode"] = LogicalType::VARCHAR;
	named_parameters["include"] = LogicalType::VARCHAR;
	named_parameters["exclude"] = LogicalType::VARCHAR;
	named_parameters["watermark_columns"] = LogicalType::VARCHAR;
}

//===--------------------------------------------------------------------===//
// ducklake_replicate_catchup
//===--------------------------------------------------------------------===//

struct ReplicateCatchupData : public TableFunctionData {
	Catalog *dest_catalog = nullptr;
	uint64_t replication_id = 0;
};

static unique_ptr<FunctionData> ReplicateCatchupBind(ClientContext &context, TableFunctionBindInput &input,
                                                     vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto &catalog = DuckLakeBaseMetadataFunction::GetCatalog(context, input.inputs[0]);
	EnsureParquetLoaded(context);
	auto result = make_uniq<ReplicateCatchupData>();
	result->dest_catalog = &catalog;
	if (input.inputs[1].IsNull()) {
		throw BinderException("replication_id cannot be NULL");
	}
	auto replication_id = input.inputs[1].GetValue<int64_t>();
	if (replication_id < 0) {
		throw BinderException("replication_id cannot be negative");
	}
	result->replication_id = static_cast<uint64_t>(replication_id);

	names.emplace_back("replication_id");
	return_types.emplace_back(LogicalType::UBIGINT);
	names.emplace_back("tables_synced");
	return_types.emplace_back(LogicalType::UBIGINT);
	names.emplace_back("tables_failed");
	return_types.emplace_back(LogicalType::UBIGINT);
	names.emplace_back("rows_copied");
	return_types.emplace_back(LogicalType::UBIGINT);
	names.emplace_back("message");
	return_types.emplace_back(LogicalType::VARCHAR);
	return std::move(result);
}

static void ReplicateCatchupExecute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.bind_data->Cast<ReplicateCatchupData>();
	auto &state = data_p.global_state->Cast<ReplicateOnceState>();
	if (state.offset > 0) {
		output.SetChildCardinality(0);
		return;
	}
	if (!state.executed) {
		auto &ducklake = data.dest_catalog->Cast<DuckLakeCatalog>();
		state.catchup_result = DuckLakeReplication::Catchup(context, ducklake, data.replication_id);
		state.executed = true;
	}
	state.offset++;
	auto &result = state.catchup_result;
	AppendSingletonRow(output, {Value::UBIGINT(result.replication_id), Value::UBIGINT(result.tables_synced),
	                            Value::UBIGINT(result.tables_failed), Value::UBIGINT(result.rows_copied),
	                            Value(result.message)});
}

DuckLakeReplicateCatchupFunction::DuckLakeReplicateCatchupFunction()
    : TableFunction(Identifier("ducklake_replicate_catchup"), {LogicalType::VARCHAR, LogicalType::BIGINT},
                    ReplicateCatchupExecute, ReplicateCatchupBind, ReplicateOnceInit) {
}

//===--------------------------------------------------------------------===//
// ducklake_replicate_status
//===--------------------------------------------------------------------===//

struct ReplicateStatusData : public TableFunctionData {
	Catalog *dest_catalog = nullptr;
};

static void GetReplicationStatusColumns(vector<LogicalType> &return_types, vector<Identifier> &names) {
	names.emplace_back("replication_id");
	return_types.emplace_back(LogicalType::UBIGINT);
	names.emplace_back("source_catalog");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("dest_catalog");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("status");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("data_sync_mode");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("interval_ms");
	return_types.emplace_back(LogicalType::UBIGINT);
	names.emplace_back("include_patterns");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("exclude_patterns");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("watermark_columns");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("last_run_ts");
	return_types.emplace_back(LogicalType::TIMESTAMP_TZ);
	names.emplace_back("last_ok_ts");
	return_types.emplace_back(LogicalType::TIMESTAMP_TZ);
	names.emplace_back("last_error");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("created_at");
	return_types.emplace_back(LogicalType::TIMESTAMP_TZ);
	names.emplace_back("dest_snapshot");
	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("snapshots_behind");
	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("lag_ms");
	return_types.emplace_back(LogicalType::BIGINT);
}

static vector<Value> JobStatusValues(const DuckLakeReplicationJob &job, const Value &dest_snapshot,
                                     const Value &snapshots_behind) {
	Value lag_ms;
	if (!job.last_ok_ts.IsNull()) {
		auto now_ms = Timestamp::GetEpochMs(Timestamp::GetCurrentTimestamp());
		lag_ms = Value::BIGINT(now_ms - Timestamp::GetEpochMs(job.last_ok_ts.GetValue<timestamp_t>()));
	}
	return {Value::UBIGINT(job.replication_id),
	        Value(job.source_catalog),
	        Value(job.dest_catalog),
	        Value(job.status),
	        Value(job.data_sync_mode),
	        Value::UBIGINT(job.interval_ms),
	        Value(job.include_patterns),
	        Value(job.exclude_patterns),
	        job.watermark_columns.empty() ? Value() : Value(job.watermark_columns),
	        job.last_run_ts,
	        job.last_ok_ts,
	        job.last_error.empty() ? Value() : Value(job.last_error),
	        job.created_at,
	        dest_snapshot,
	        snapshots_behind,
	        lag_ms};
}

static unique_ptr<FunctionData> ReplicateStatusBind(ClientContext &context, TableFunctionBindInput &input,
                                                    vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto &catalog = DuckLakeBaseMetadataFunction::GetCatalog(context, input.inputs[0]);
	auto result = make_uniq<ReplicateStatusData>();
	result->dest_catalog = &catalog;
	GetReplicationStatusColumns(return_types, names);
	return std::move(result);
}

static void ReplicateStatusExecute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.bind_data->Cast<ReplicateStatusData>();
	auto &state = data_p.global_state->Cast<ReplicateRowsState>();
	if (!state.loaded) {
		auto &ducklake = data.dest_catalog->Cast<DuckLakeCatalog>();
		auto jobs = DuckLakeReplication::ListJobs(context, ducklake);
		// Replica snapshot position: the dest lake's own head snapshot.
		Value dest_snapshot;
		{
			Connection con(*context.db);
			auto snap =
			    con.Query(StringUtil::Format("SELECT MAX(snapshot_id) FROM ducklake_snapshots(%s)",
			                                 DuckLakeUtil::SQLLiteralToString(ducklake.GetName().GetIdentifierName())));
			if (!snap->HasError() && snap->RowCount() > 0) {
				dest_snapshot = snap->GetValue(0, 0);
			}
		}
		for (auto &job : jobs) {
			state.rows.push_back(
			    JobStatusValues(job, dest_snapshot, DuckLakeReplication::SnapshotsBehind(context, ducklake, job)));
		}
		state.loaded = true;
	}
	StreamRows(output, state.offset, state.rows);
}

DuckLakeReplicateStatusFunction::DuckLakeReplicateStatusFunction()
    : TableFunction(Identifier("ducklake_replicate_status"), {LogicalType::VARCHAR}, ReplicateStatusExecute,
                    ReplicateStatusBind, ReplicateRowsInit) {
}

//===--------------------------------------------------------------------===//
// ducklake_replicate_tables
//===--------------------------------------------------------------------===//

struct ReplicateTablesData : public TableFunctionData {
	Catalog *dest_catalog = nullptr;
	uint64_t replication_id = 0;
};

static unique_ptr<FunctionData> ReplicateTablesBind(ClientContext &context, TableFunctionBindInput &input,
                                                    vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto &catalog = DuckLakeBaseMetadataFunction::GetCatalog(context, input.inputs[0]);
	auto result = make_uniq<ReplicateTablesData>();
	result->dest_catalog = &catalog;
	if (input.inputs[1].IsNull()) {
		throw BinderException("replication_id cannot be NULL");
	}
	auto replication_id = input.inputs[1].GetValue<int64_t>();
	if (replication_id < 0) {
		throw BinderException("replication_id cannot be negative");
	}
	result->replication_id = static_cast<uint64_t>(replication_id);

	names.emplace_back("replication_id");
	return_types.emplace_back(LogicalType::UBIGINT);
	names.emplace_back("source_schema");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("source_table");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("dest_schema");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("dest_table");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("strategy");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("watermark_column");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("pk_columns");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("last_watermark");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("last_source_snapshot");
	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("last_full_sync_snapshot");
	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("row_count_dest");
	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("last_error");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("synced_at");
	return_types.emplace_back(LogicalType::TIMESTAMP_TZ);
	return std::move(result);
}

static void ReplicateTablesExecute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.bind_data->Cast<ReplicateTablesData>();
	auto &state = data_p.global_state->Cast<ReplicateRowsState>();
	if (!state.loaded) {
		auto &ducklake = data.dest_catalog->Cast<DuckLakeCatalog>();
		auto states = DuckLakeReplication::ListTableStates(context, ducklake, data.replication_id);
		for (auto &entry : states) {
			state.rows.push_back(
			    {Value::UBIGINT(entry.replication_id), Value(entry.source_schema), Value(entry.source_table),
			     Value(entry.dest_schema), Value(entry.dest_table), Value(entry.strategy),
			     entry.watermark_column.empty() ? Value() : Value(entry.watermark_column),
			     entry.pk_columns.empty() ? Value() : Value(entry.pk_columns), entry.last_watermark,
			     entry.last_source_snapshot, entry.last_full_sync_snapshot, Value::BIGINT(entry.row_count_dest),
			     entry.last_error.empty() ? Value() : Value(entry.last_error), entry.synced_at});
		}
		state.loaded = true;
	}
	StreamRows(output, state.offset, state.rows);
}

DuckLakeReplicateTablesFunction::DuckLakeReplicateTablesFunction()
    : TableFunction(Identifier("ducklake_replicate_tables"), {LogicalType::VARCHAR, LogicalType::BIGINT},
                    ReplicateTablesExecute, ReplicateTablesBind, ReplicateRowsInit) {
}

//===--------------------------------------------------------------------===//
// ducklake_replicate_drop
//===--------------------------------------------------------------------===//

struct ReplicateDropData : public TableFunctionData {
	Catalog *dest_catalog = nullptr;
	uint64_t replication_id = 0;
};

static unique_ptr<FunctionData> ReplicateDropBind(ClientContext &context, TableFunctionBindInput &input,
                                                  vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto &catalog = DuckLakeBaseMetadataFunction::GetCatalog(context, input.inputs[0]);
	auto result = make_uniq<ReplicateDropData>();
	result->dest_catalog = &catalog;
	if (input.inputs[1].IsNull()) {
		throw BinderException("replication_id cannot be NULL");
	}
	auto replication_id = input.inputs[1].GetValue<int64_t>();
	if (replication_id < 0) {
		throw BinderException("replication_id cannot be negative");
	}
	result->replication_id = static_cast<uint64_t>(replication_id);
	names.emplace_back("replication_id");
	return_types.emplace_back(LogicalType::UBIGINT);
	return std::move(result);
}

static void ReplicateDropExecute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.bind_data->Cast<ReplicateDropData>();
	auto &state = data_p.global_state->Cast<ReplicateOnceState>();
	if (state.offset > 0) {
		output.SetChildCardinality(0);
		return;
	}
	if (!state.executed) {
		auto &ducklake = data.dest_catalog->Cast<DuckLakeCatalog>();
		DuckLakeReplication::DropJob(context, ducklake, data.replication_id);
		state.replication_id = data.replication_id;
		state.executed = true;
	}
	state.offset++;
	AppendSingletonRow(output, {Value::UBIGINT(state.replication_id)});
}

DuckLakeReplicateDropFunction::DuckLakeReplicateDropFunction()
    : TableFunction(Identifier("ducklake_replicate_drop"), {LogicalType::VARCHAR, LogicalType::BIGINT},
                    ReplicateDropExecute, ReplicateDropBind, ReplicateOnceInit) {
}

//===--------------------------------------------------------------------===//
// ducklake_replicate_start / stop / pause / resume / resume_all
//===--------------------------------------------------------------------===//

static void GetReplicationActionColumns(vector<LogicalType> &return_types, vector<Identifier> &names) {
	names.emplace_back("replication_id");
	return_types.emplace_back(LogicalType::UBIGINT);
	names.emplace_back("status");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("message");
	return_types.emplace_back(LogicalType::VARCHAR);
}

static vector<Value> ActionRow(const DuckLakeReplicationJobAction &action) {
	return {action.replication_id == 0 ? Value() : Value::UBIGINT(action.replication_id), Value(action.status),
	        Value(action.message)};
}

//! Bind helper shared by start/stop: catalog arg + optional named job_id for start.
static unique_ptr<FunctionData> ReplicateStartBind(ClientContext &context, TableFunctionBindInput &input,
                                                   vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto &catalog = DuckLakeBaseMetadataFunction::GetCatalog(context, input.inputs[0]);
	auto result = make_uniq<ReplicateDropData>();
	result->dest_catalog = &catalog;
	for (auto &entry : input.named_parameters) {
		auto name = StringUtil::Lower(entry.first.GetIdentifierName());
		if (name != "job_id") {
			throw BinderException("Unsupported named parameter for ducklake_replicate_start: %s", entry.first);
		}
		if (!entry.second.IsNull()) {
			auto job_id = entry.second.GetValue<int64_t>();
			if (job_id < 0) {
				throw BinderException("job_id cannot be negative");
			}
			result->replication_id = static_cast<uint64_t>(job_id);
		}
	}
	GetReplicationActionColumns(return_types, names);
	return std::move(result);
}

static void ReplicateStartExecute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.bind_data->Cast<ReplicateDropData>();
	auto &state = data_p.global_state->Cast<ReplicateRowsState>();
	if (!state.loaded) {
		auto &ducklake = data.dest_catalog->Cast<DuckLakeCatalog>();
		for (auto &action : DuckLakeReplication::Start(context, ducklake, data.replication_id)) {
			state.rows.push_back(ActionRow(action));
		}
		if (state.rows.empty()) {
			state.rows.push_back({Value(), Value("running"), Value("supervisor running - no runnable jobs")});
		}
		state.loaded = true;
	}
	StreamRows(output, state.offset, state.rows);
}

DuckLakeReplicateStartFunction::DuckLakeReplicateStartFunction()
    : TableFunction(Identifier("ducklake_replicate_start"), {LogicalType::VARCHAR}, ReplicateStartExecute,
                    ReplicateStartBind, ReplicateRowsInit) {
	named_parameters["job_id"] = LogicalType::BIGINT;
}

static unique_ptr<FunctionData> ReplicateLifecycleBind(ClientContext &context, TableFunctionBindInput &input,
                                                       vector<LogicalType> &return_types, vector<Identifier> &names,
                                                       bool needs_job) {
	auto &catalog = DuckLakeBaseMetadataFunction::GetCatalog(context, input.inputs[0]);
	auto result = make_uniq<ReplicateDropData>();
	result->dest_catalog = &catalog;
	if (needs_job) {
		if (input.inputs.size() < 2 || input.inputs[1].IsNull()) {
			throw BinderException("replication_id cannot be NULL");
		}
		auto replication_id = input.inputs[1].GetValue<int64_t>();
		if (replication_id < 0) {
			throw BinderException("replication_id cannot be negative");
		}
		result->replication_id = static_cast<uint64_t>(replication_id);
	}
	GetReplicationActionColumns(return_types, names);
	return std::move(result);
}

static unique_ptr<FunctionData> ReplicateStopBind(ClientContext &context, TableFunctionBindInput &input,
                                                  vector<LogicalType> &return_types, vector<Identifier> &names) {
	return ReplicateLifecycleBind(context, input, return_types, names, false);
}

static void ReplicateStopExecute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.bind_data->Cast<ReplicateDropData>();
	auto &state = data_p.global_state->Cast<ReplicateRowsState>();
	if (!state.loaded) {
		auto &ducklake = data.dest_catalog->Cast<DuckLakeCatalog>();
		DuckLakeReplication::Stop(context, ducklake);
		state.rows.push_back({Value(), Value("stopped"), Value("replication supervisor stopped")});
		state.loaded = true;
	}
	StreamRows(output, state.offset, state.rows);
}

DuckLakeReplicateStopFunction::DuckLakeReplicateStopFunction()
    : TableFunction(Identifier("ducklake_replicate_stop"), {LogicalType::VARCHAR}, ReplicateStopExecute,
                    ReplicateStopBind, ReplicateRowsInit) {
}

static unique_ptr<FunctionData> ReplicatePauseBind(ClientContext &context, TableFunctionBindInput &input,
                                                   vector<LogicalType> &return_types, vector<Identifier> &names) {
	return ReplicateLifecycleBind(context, input, return_types, names, true);
}

static void ReplicatePauseExecute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.bind_data->Cast<ReplicateDropData>();
	auto &state = data_p.global_state->Cast<ReplicateRowsState>();
	if (!state.loaded) {
		auto &ducklake = data.dest_catalog->Cast<DuckLakeCatalog>();
		state.rows.push_back(ActionRow(DuckLakeReplication::Pause(context, ducklake, data.replication_id)));
		state.loaded = true;
	}
	StreamRows(output, state.offset, state.rows);
}

DuckLakeReplicatePauseFunction::DuckLakeReplicatePauseFunction()
    : TableFunction(Identifier("ducklake_replicate_pause"), {LogicalType::VARCHAR, LogicalType::BIGINT},
                    ReplicatePauseExecute, ReplicatePauseBind, ReplicateRowsInit) {
}

static unique_ptr<FunctionData> ReplicateResumeBind(ClientContext &context, TableFunctionBindInput &input,
                                                    vector<LogicalType> &return_types, vector<Identifier> &names) {
	return ReplicateLifecycleBind(context, input, return_types, names, true);
}

static void ReplicateResumeExecute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.bind_data->Cast<ReplicateDropData>();
	auto &state = data_p.global_state->Cast<ReplicateRowsState>();
	if (!state.loaded) {
		auto &ducklake = data.dest_catalog->Cast<DuckLakeCatalog>();
		state.rows.push_back(ActionRow(DuckLakeReplication::Resume(context, ducklake, data.replication_id)));
		state.loaded = true;
	}
	StreamRows(output, state.offset, state.rows);
}

DuckLakeReplicateResumeFunction::DuckLakeReplicateResumeFunction()
    : TableFunction(Identifier("ducklake_replicate_resume"), {LogicalType::VARCHAR, LogicalType::BIGINT},
                    ReplicateResumeExecute, ReplicateResumeBind, ReplicateRowsInit) {
}

static unique_ptr<FunctionData> ReplicateResumeAllBind(ClientContext &context, TableFunctionBindInput &input,
                                                       vector<LogicalType> &return_types, vector<Identifier> &names) {
	return ReplicateLifecycleBind(context, input, return_types, names, false);
}

static void ReplicateResumeAllExecute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.bind_data->Cast<ReplicateDropData>();
	auto &state = data_p.global_state->Cast<ReplicateRowsState>();
	if (!state.loaded) {
		auto &ducklake = data.dest_catalog->Cast<DuckLakeCatalog>();
		for (auto &action : DuckLakeReplication::ResumeAll(context, ducklake)) {
			state.rows.push_back(ActionRow(action));
		}
		if (state.rows.empty()) {
			state.rows.push_back({Value(), Value("running"), Value("no jobs to resume")});
		}
		state.loaded = true;
	}
	StreamRows(output, state.offset, state.rows);
}

DuckLakeReplicateResumeAllFunction::DuckLakeReplicateResumeAllFunction()
    : TableFunction(Identifier("ducklake_replicate_resume_all"), {LogicalType::VARCHAR}, ReplicateResumeAllExecute,
                    ReplicateResumeAllBind, ReplicateRowsInit) {
}

} // namespace duckdb
