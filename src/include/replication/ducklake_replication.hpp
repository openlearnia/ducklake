//===----------------------------------------------------------------------===//
//                         DuckDB
//
// replication/ducklake_replication.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/types/value.hpp"

namespace duckdb {
class ClientContext;
class DatabaseInstance;
class DuckLakeCatalog;

struct DuckLakeReplicationJob {
	uint64_t replication_id = 0;
	string source_catalog;
	string dest_catalog;
	string status;
	string data_sync_mode;
	uint64_t interval_ms = 0;
	string include_patterns;
	string exclude_patterns;
	string watermark_columns;
	string last_error;
	Value last_run_ts;
	Value last_ok_ts;
	Value created_at;
};

struct DuckLakeReplicationTableState {
	uint64_t replication_id = 0;
	string source_schema;
	string source_table;
	string dest_schema;
	string dest_table;
	string strategy;
	string watermark_column;
	string pk_columns;
	Value last_watermark;
	Value last_source_snapshot;
	Value last_full_sync_snapshot;
	int64_t row_count_dest = 0;
	string last_error;
	Value synced_at;
};

struct DuckLakeReplicationCatchupResult {
	uint64_t replication_id = 0;
	uint64_t tables_synced = 0;
	uint64_t tables_failed = 0;
	uint64_t rows_copied = 0;
	string message;
};

//! Lifecycle action result: the affected job (0 for supervisor-level actions) and its new status.
struct DuckLakeReplicationJobAction {
	uint64_t replication_id = 0;
	string status;
	string message;
};

//! Catalog-to-catalog replication: any attached DuckDB catalog → DuckLake destination.
class DuckLakeReplication {
public:
	//! Create state tables in the destination metadata catalog if missing (idempotent).
	static void EnsureStateTables(ClientContext &context, DuckLakeCatalog &dest);

	//! Register a new replication job; returns the new replication_id.
	//! watermark_columns is a comma-separated "schema.table=column" list for opt-in watermark sync.
	static uint64_t CreateJob(ClientContext &context, DuckLakeCatalog &dest, const string &source_catalog,
	                          uint64_t interval_ms, const string &data_sync_mode, const string &include,
	                          const string &exclude, const string &watermark_columns);

	//! Remove a job and its per-table state. Leaves replicated data in place.
	static void DropJob(ClientContext &context, DuckLakeCatalog &dest, uint64_t replication_id);

	//! All jobs stored in this destination catalog.
	static vector<DuckLakeReplicationJob> ListJobs(ClientContext &context, DuckLakeCatalog &dest);

	//! Load one job; returns false when the id is unknown.
	static bool TryGetJob(ClientContext &context, DuckLakeCatalog &dest, uint64_t replication_id,
	                      DuckLakeReplicationJob &result);

	//! Per-table state rows for a job.
	static vector<DuckLakeReplicationTableState> ListTableStates(ClientContext &context, DuckLakeCatalog &dest,
	                                                             uint64_t replication_id);

	//! Run one full (Phase 1) sync cycle for the job. `abort`, when set, asks the cycle to
	//! bail between tables instead of planning new statements (supervisor quiesce/teardown).
	static DuckLakeReplicationCatchupResult Catchup(ClientContext &context, DuckLakeCatalog &dest,
	                                                uint64_t replication_id, const atomic<bool> *abort = nullptr);

	//! Spawn (idempotently) the supervisor thread for `dest` and mark jobs runnable.
	//! job_id = 0 targets all non-paused jobs; otherwise the named job is marked running.
	static vector<DuckLakeReplicationJobAction> Start(ClientContext &context, DuckLakeCatalog &dest, uint64_t job_id);

	//! Signal the supervisor for `dest` to stop and join it. Job statuses are left as-is.
	static void Stop(ClientContext &context, DuckLakeCatalog &dest);

	//! Mark a job paused (supervisor skips it; manual catchup is rejected).
	static DuckLakeReplicationJobAction Pause(ClientContext &context, DuckLakeCatalog &dest, uint64_t replication_id);

	//! Mark a job running and ensure the supervisor is up.
	static DuckLakeReplicationJobAction Resume(ClientContext &context, DuckLakeCatalog &dest, uint64_t replication_id);

	//! Mark every paused/error job running and ensure the supervisor is up.
	static vector<DuckLakeReplicationJobAction> ResumeAll(ClientContext &context, DuckLakeCatalog &dest);

	//! Stop the supervisor bound to a detaching destination catalog (clean shutdown).
	static void OnCatalogDetach(DatabaseInstance &db, const string &catalog_name);

	//! Best-effort lag: head-vs-cursor commit distance summed over snapshot tables (NULL-able).
	static Value SnapshotsBehind(ClientContext &context, DuckLakeCatalog &dest, const DuckLakeReplicationJob &job);
};

} // namespace duckdb
