//===----------------------------------------------------------------------===//
//                         DuckDB
//
// replication/ducklake_replication.cpp
//
//
//===----------------------------------------------------------------------===//

#include "replication/ducklake_replication.hpp"
#include "common/ducklake_util.hpp"
#include "storage/ducklake_catalog.hpp"
#include "storage/ducklake_transaction.hpp"
#include "storage/ducklake_metadata_manager.hpp"
#include "storage/ducklake_table_entry.hpp"
#include "storage/ducklake_delete.hpp"
#include "storage/ducklake_delete_filter.hpp"
#include "functions/ducklake_table_functions.hpp"
#include "common/ducklake_data_file.hpp"
#include "duckdb/planner/tableref/bound_at_clause.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/case_insensitive_map.hpp"

#include <condition_variable>
#include <cstdlib>
#include <thread>

#include "duckdb/main/connection_manager.hpp"

namespace duckdb {

//! Set when the process is tearing down - in-flight catchup work aborts at the next check
//! instead of running statements against catalogs/globals that are being destroyed.
static atomic<bool> replication_shutdown {false};

static bool IsReplicationAborting(const atomic<bool> *abort) {
	return replication_shutdown.load() || (abort && abort->load());
}

//===--------------------------------------------------------------------===//
// Helpers
//===--------------------------------------------------------------------===//

static string SQLId(const string &text) {
	return DuckLakeUtil::SQLIdentifierToString(text);
}

static string SQLLit(const string &text) {
	return DuckLakeUtil::SQLLiteralToString(text);
}

static string MetaTableName(DuckLakeCatalog &catalog, const string &table) {
	auto schema = catalog.MetadataSchemaName().GetIdentifierName();
	if (schema.empty()) {
		schema = "main";
	}
	return SQLId(catalog.MetadataDatabaseName()) + "." + SQLId(schema) + "." + SQLId(table);
}

static unique_ptr<Connection> MakeConnection(ClientContext &context, DatabaseInstance &db) {
	auto con = make_uniq<Connection>(db);
	con->context->registered_state->GetOrCreate<DuckLakeInternalConnectionState>(DuckLakeInternalConnectionState::KEY);
	DuckLakeUtil::CopyExtensionSettings(context, *con->context);
	return con;
}

static void CheckResult(QueryResult &result, const string &context) {
	if (result.HasError()) {
		result.GetErrorObject().Throw(context + ": ");
	}
}

//! `*` matches any run, `?` a single character.
static bool GlobMatch(const string &pattern, const string &text) {
	idx_t p = 0;
	idx_t t = 0;
	idx_t star_p = DConstants::INVALID_INDEX;
	idx_t star_t = 0;
	while (t < text.size()) {
		if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t])) {
			p++;
			t++;
		} else if (p < pattern.size() && pattern[p] == '*') {
			star_p = p;
			star_t = t;
			p++;
		} else if (star_p != DConstants::INVALID_INDEX) {
			p = star_p + 1;
			star_t++;
			t = star_t;
		} else {
			return false;
		}
	}
	while (p < pattern.size() && pattern[p] == '*') {
		p++;
	}
	return p == pattern.size();
}

static vector<string> SplitPatterns(const string &patterns) {
	vector<string> result;
	for (auto &piece : StringUtil::Split(patterns, ',')) {
		auto trimmed = piece;
		StringUtil::Trim(trimmed);
		if (!trimmed.empty()) {
			result.push_back(trimmed);
		}
	}
	return result;
}

static bool PatternListMatches(const vector<string> &patterns, const string &schema, const string &table) {
	auto full = schema + "." + table;
	for (auto &pattern : patterns) {
		if (pattern == "*" || pattern == "*.*") {
			return true;
		}
		if (GlobMatch(pattern, full)) {
			return true;
		}
		// bare `schema` is shorthand for `schema.*`
		if (pattern.find('.') == string::npos && GlobMatch(pattern, schema)) {
			return true;
		}
	}
	return false;
}

static bool IsSkippedSchema(const string &schema) {
	return schema == "system" || schema == "information_schema" || schema == "pg_catalog" || schema == "temp";
}

static bool IsExcluded(const string &schema, const string &table, const vector<string> &include,
                       const vector<string> &exclude) {
	if (IsSkippedSchema(schema)) {
		return true;
	}
	if (!exclude.empty() && PatternListMatches(exclude, schema, table)) {
		return true;
	}
	if (!include.empty() && !PatternListMatches(include, schema, table)) {
		return true;
	}
	return false;
}

static Catalog &GetAttachedCatalog(ClientContext &context, const string &name) {
	auto &db_manager = DatabaseManager::Get(*context.db);
	auto db = db_manager.GetDatabase(Identifier(name));
	if (!db) {
		throw BinderException("Failed to find attached database \"%s\"", name);
	}
	return db->GetCatalog();
}

struct ReplColumnInfo {
	string name;
	string type;
	string default_value; // empty when NULL
	bool has_default = false;
};

static vector<ReplColumnInfo> ListColumns(ClientContext &context, Catalog &catalog, const string &database,
                                          const string &schema, const string &table) {
	auto con = MakeConnection(context, catalog.GetDatabase());
	auto query = StringUtil::Format("SELECT column_name, data_type, column_default FROM duckdb_columns() WHERE "
	                                "database_name = %s AND schema_name = %s AND table_name = %s ORDER BY "
	                                "column_index",
	                                SQLLit(database), SQLLit(schema), SQLLit(table));
	auto result = con->Query(query);
	CheckResult(*result, "Failed to read columns for " + database + "." + schema + "." + table);
	vector<ReplColumnInfo> columns;
	for (auto &row : *result) {
		ReplColumnInfo col;
		col.name = row.GetValue<string>(0);
		col.type = row.GetValue<string>(1);
		if (!row.IsNull(2)) {
			col.default_value = row.GetValue<string>(2);
			col.has_default = true;
		}
		columns.push_back(std::move(col));
	}
	return columns;
}

//! Declared primary-key columns of a table in key order (empty when none).
static vector<string> PrimaryKeyColumns(ClientContext &context, Catalog &catalog, const string &database,
                                        const string &schema, const string &table) {
	auto con = MakeConnection(context, catalog.GetDatabase());
	auto qualified = SQLLit(database + "." + schema + "." + table);
	auto query = StringUtil::Format("SELECT name FROM pragma_table_info(%s) WHERE pk", qualified);
	auto result = con->Query(query);
	CheckResult(*result, "Failed to read primary key for " + database + "." + schema + "." + table);
	vector<string> columns;
	for (auto &row : *result) {
		columns.push_back(row.GetValue<string>(0));
	}
	return columns;
}

//! Parses a comma-separated "schema.table=column" watermark spec.
static case_insensitive_map_t<string> ParseWatermarkSpec(const string &spec) {
	case_insensitive_map_t<string> result;
	for (auto &piece : SplitPatterns(spec)) {
		auto eq = piece.find('=');
		if (eq == string::npos) {
			throw InvalidInputException("Invalid watermark_columns entry %s - expected \"schema.table=column\"", piece);
		}
		auto key = piece.substr(0, eq);
		auto col = piece.substr(eq + 1);
		StringUtil::Trim(key);
		StringUtil::Trim(col);
		if (key.empty() || col.empty()) {
			throw InvalidInputException("Invalid watermark_columns entry %s - expected \"schema.table=column\"", piece);
		}
		result[key] = col;
	}
	return result;
}

struct SourceTableRef {
	string schema;
	string table;
};

//! NULL-safe column equality between two relation aliases.
static string NullSafeEq(const string &lhs_alias, const string &rhs_alias, const string &column) {
	auto col = SQLId(column);
	return StringUtil::Format("(%s.%s IS NOT DISTINCT FROM %s.%s)", lhs_alias, col, rhs_alias, col);
}

//! NULL-safe equality between differently named columns on two aliases.
static string NullSafeEqCols(const string &lhs_alias, const string &lhs_col, const string &rhs_alias,
                             const string &rhs_col) {
	return StringUtil::Format("(%s.%s IS NOT DISTINCT FROM %s.%s)", lhs_alias, SQLId(lhs_col), rhs_alias,
	                          SQLId(rhs_col));
}

static string ColumnListMatch(const vector<ReplColumnInfo> &columns, const string &lhs_alias, const string &rhs_alias) {
	vector<string> terms;
	for (auto &col : columns) {
		terms.push_back(NullSafeEq(lhs_alias, rhs_alias, col.name));
	}
	return StringUtil::Join(terms, " AND ");
}

static string ColumnListMatch(const vector<string> &columns, const string &lhs_alias, const string &rhs_alias) {
	vector<string> terms;
	for (auto &col : columns) {
		terms.push_back(NullSafeEq(lhs_alias, rhs_alias, col));
	}
	return StringUtil::Join(terms, " AND ");
}

static bool SameColumns(const vector<ReplColumnInfo> &lhs, const vector<ReplColumnInfo> &rhs) {
	if (lhs.size() != rhs.size()) {
		return false;
	}
	for (idx_t i = 0; i < lhs.size(); i++) {
		if (!StringUtil::CIEquals(lhs[i].name, rhs[i].name) || lhs[i].type != rhs[i].type) {
			return false;
		}
	}
	return true;
}

//! True when the destination columns are a strict name+type prefix of the source columns.
static bool IsAdditiveSchemaChange(const vector<ReplColumnInfo> &source, const vector<ReplColumnInfo> &dest) {
	if (dest.size() >= source.size()) {
		return false;
	}
	for (idx_t i = 0; i < dest.size(); i++) {
		if (!StringUtil::CIEquals(dest[i].name, source[i].name) || dest[i].type != source[i].type) {
			return false;
		}
	}
	return true;
}

//! Reads the first cell of a single-row query as a (possibly NULL) Value.
static Value ScalarValue(Connection &con, const string &sql) {
	auto result = con.Query(sql);
	CheckResult(*result, "Scalar query failed");
	if (result->RowCount() == 0) {
		return Value();
	}
	return result->GetValue(0, 0);
}

//! Renders a Value as a SQL literal (NULL, unquoted BIGINT, or quoted string form).
static string ValueSql(const Value &value) {
	if (value.IsNull()) {
		return "NULL";
	}
	if (value.type().id() == LogicalTypeId::BIGINT || value.type().id() == LogicalTypeId::UBIGINT) {
		return to_string(value.GetValue<int64_t>());
	}
	return SQLLit(value.ToString());
}

//! Result of one table sync - written to ducklake_replication_table after COMMIT.
struct TableSyncOutcome {
	string strategy = "full";
	string watermark_column;
	string pk_columns;
	Value last_watermark;
	Value last_source_snapshot;
	Value last_full_sync_snapshot;
	int64_t row_count = 0;
};

//! Diffs destination against source on primary keys: removes missing/changed rows, inserts new ones.
static void ApplyPkMerge(Connection &apply, const string &dst_table, const string &src_table,
                         const vector<ReplColumnInfo> &columns, const vector<string> &pk_columns) {
	auto pk_match = ColumnListMatch(pk_columns, "s", "d");
	vector<string> non_pk;
	for (auto &col : columns) {
		bool is_pk = false;
		for (auto &pk : pk_columns) {
			if (StringUtil::CIEquals(col.name, pk)) {
				is_pk = true;
				break;
			}
		}
		if (!is_pk) {
			non_pk.push_back(col.name);
		}
	}
	auto del_missing = StringUtil::Format("DELETE FROM %s d WHERE NOT EXISTS (SELECT 1 FROM %s s WHERE %s)", dst_table,
	                                      src_table, pk_match);
	CheckResult(*apply.Query(del_missing), "Failed to merge-delete rows in " + dst_table);
	if (!non_pk.empty()) {
		vector<string> changed_terms;
		for (auto &col : non_pk) {
			auto col_id = SQLId(col);
			changed_terms.push_back(StringUtil::Format("(s.%s IS DISTINCT FROM d.%s)", col_id, col_id));
		}
		auto del_changed = StringUtil::Format("DELETE FROM %s d WHERE EXISTS (SELECT 1 FROM %s s WHERE %s AND (%s))",
		                                      dst_table, src_table, pk_match, StringUtil::Join(changed_terms, " OR "));
		CheckResult(*apply.Query(del_changed), "Failed to merge-update rows in " + dst_table);
	}
	auto ins_new =
	    StringUtil::Format("INSERT INTO %s SELECT * FROM %s s WHERE NOT EXISTS (SELECT 1 FROM %s d WHERE %s)",
	                       dst_table, src_table, dst_table, pk_match);
	CheckResult(*apply.Query(ins_new), "Failed to merge-insert rows into " + dst_table);
}

//! Applies a DuckLake snapshot delta window: per snapshot, deduped inserts then deletes, in commit order.
//! ducklake_table_changes works at file granularity so inserts are deduped by full-row match to absorb
//! rewrite churn and lower-bound replay. Deletes apply unconditionally, except update preimages whose
//! paired postimage is value-identical (a no-op update must not remove the row). The delta is staged in
//! a temp table and the delete side precomputed with an anti-join: correlated EXISTS over a join hits a
//! planner bug on DELETE, and positional #N refs dodge source columns named snapshot_id/rowid/change_type.
static bool ApplySnapshotDelta(Connection &apply, const string &src_table, const string &dst_table,
                               const DuckLakeReplicationJob &job, const SourceTableRef &ref, int64_t low, int64_t high,
                               const vector<ReplColumnInfo> &columns) {
	auto changes = StringUtil::Format("ducklake_table_changes(%s, %s, %s, %lld, %lld)", SQLLit(job.source_catalog),
	                                  SQLLit(ref.schema), SQLLit(ref.table), low, high);
	string delta_cols = "#1 AS __snap, #2 AS __rid, #3 AS __type";
	vector<string> delta_col_names;
	for (idx_t i = 0; i < columns.size(); i++) {
		auto name = "__c" + to_string(i);
		delta_cols += StringUtil::Format(", #%llu AS %s", i + 4, SQLId(name));
		delta_col_names.push_back(name);
	}
	auto stage_sql = StringUtil::Format("CREATE OR REPLACE TEMP TABLE __ducklake_repl_delta AS SELECT %s FROM %s",
	                                    delta_cols, changes);
	CheckResult(*apply.Query(stage_sql), "Failed to stage snapshot delta for " + ref.schema + "." + ref.table);

	// Effective delete rows: every 'delete' plus update preimages without a value-identical postimage.
	auto pair_match = ColumnListMatch(delta_col_names, "c2", "c");
	auto effdel_sql = StringUtil::Format(
	    "CREATE OR REPLACE TEMP TABLE __ducklake_repl_effdel AS SELECT c.* FROM __ducklake_repl_delta c LEFT JOIN "
	    "__ducklake_repl_delta c2 ON c.__type = 'update_preimage' AND c2.__snap = c.__snap AND c2.__rid = c.__rid "
	    "AND c2.__type = 'update_postimage' AND %s WHERE c.__type IN ('delete', 'update_preimage') AND c2.__rid "
	    "IS NULL",
	    pair_match);
	CheckResult(*apply.Query(effdel_sql), "Failed to compute effective deletes for " + ref.schema + "." + ref.table);

	vector<string> ins_cols, ins_terms, del_terms;
	for (idx_t i = 0; i < columns.size(); i++) {
		ins_cols.push_back("c." + SQLId(delta_col_names[i]));
		ins_terms.push_back(NullSafeEqCols("c", delta_col_names[i], "d", columns[i].name));
		del_terms.push_back(NullSafeEqCols("e", delta_col_names[i], "d", columns[i].name));
	}
	auto ins_select = StringUtil::Join(ins_cols, ", ");
	auto ins_pred = StringUtil::Join(ins_terms, " AND ");
	auto del_pred = StringUtil::Join(del_terms, " AND ");

	auto snaps = apply.Query("SELECT DISTINCT __snap FROM __ducklake_repl_delta ORDER BY 1");
	CheckResult(*snaps, "Failed to list changed snapshots for " + ref.schema + "." + ref.table);
	vector<int64_t> snapshot_ids;
	for (auto &row : *snaps) {
		snapshot_ids.push_back(row.GetValue<int64_t>(0));
	}
	for (auto snapshot_id : snapshot_ids) {
		auto ins_sql = StringUtil::Format(
		    "INSERT INTO %s SELECT %s FROM __ducklake_repl_delta c WHERE c.__snap = %lld AND c.__type IN "
		    "('insert', 'update_postimage') AND NOT EXISTS (SELECT 1 FROM %s d WHERE %s)",
		    dst_table, ins_select, snapshot_id, dst_table, ins_pred);
		CheckResult(*apply.Query(ins_sql),
		            "Failed to apply inserts at snapshot " + to_string(snapshot_id) + " to " + dst_table);
		auto del_sql =
		    StringUtil::Format("DELETE FROM %s d USING __ducklake_repl_effdel e WHERE e.__snap = %lld AND %s",
		                       dst_table, snapshot_id, del_pred);
		CheckResult(*apply.Query(del_sql),
		            "Failed to apply deletes at snapshot " + to_string(snapshot_id) + " to " + dst_table);
	}
	if (snapshot_ids.empty()) {
		return false;
	}
	const auto source_at = StringUtil::Format("%s AT (VERSION => %lld)", src_table, high);
	const auto missing =
	    ScalarValue(apply, StringUtil::Format("SELECT EXISTS(SELECT * FROM %s EXCEPT ALL SELECT * FROM %s)", source_at,
	                                          dst_table))
	        .GetValue<bool>();
	const auto extra =
	    ScalarValue(apply, StringUtil::Format("SELECT EXISTS(SELECT * FROM %s EXCEPT ALL SELECT * FROM %s)", dst_table,
	                                          source_at))
	        .GetValue<bool>();
	if (!missing && !extra) {
		return false;
	}
	CheckResult(*apply.Query("DELETE FROM " + dst_table), "Failed to reseed " + dst_table);
	CheckResult(*apply.Query("INSERT INTO " + dst_table + " SELECT * FROM " + source_at),
	            "Failed to reseed " + dst_table);
	return true;
}

static vector<SourceTableRef> ListSourceTables(ClientContext &context, Catalog &source_catalog,
                                               const string &source_name, const vector<string> &include,
                                               const vector<string> &exclude) {
	auto con = MakeConnection(context, source_catalog.GetDatabase());
	auto query = StringUtil::Format(
	    "SELECT schema_name, table_name FROM duckdb_tables() WHERE database_name = %s ORDER BY schema_name, "
	    "table_name",
	    SQLLit(source_name));
	auto result = con->Query(query);
	CheckResult(*result, "Failed to list tables in source catalog \"" + source_name + "\"");
	vector<SourceTableRef> tables;
	for (auto &row : *result) {
		SourceTableRef ref;
		ref.schema = row.GetValue<string>(0);
		ref.table = row.GetValue<string>(1);
		if (IsExcluded(ref.schema, ref.table, include, exclude)) {
			continue;
		}
		tables.push_back(std::move(ref));
	}
	return tables;
}

static void UpsertTableSyncState(ClientContext &context, DuckLakeCatalog &dest, const DuckLakeReplicationJob &job,
                                 const SourceTableRef &ref, const TableSyncOutcome &outcome) {
	auto con = MakeConnection(context, dest.GetDatabase());
	auto state_table = MetaTableName(dest, "ducklake_replication_table");
	auto delete_sql = StringUtil::Format("DELETE FROM %s WHERE replication_id = %llu AND source_schema = %s AND "
	                                     "source_table = %s",
	                                     state_table, job.replication_id, SQLLit(ref.schema), SQLLit(ref.table));
	auto del_res = con->Query(delete_sql);
	CheckResult(*del_res, "Failed to clear replication table state");
	auto insert_sql = StringUtil::Format(
	    "INSERT INTO %s (replication_id, source_schema, source_table, dest_schema, dest_table, strategy, "
	    "watermark_column, pk_columns, last_watermark, last_source_snapshot, last_full_sync_snapshot, "
	    "row_count_dest, last_error, synced_at) "
	    "VALUES (%llu, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %lld, NULL, now())",
	    state_table, job.replication_id, SQLLit(ref.schema), SQLLit(ref.table), SQLLit(ref.schema), SQLLit(ref.table),
	    SQLLit(outcome.strategy), outcome.watermark_column.empty() ? "NULL" : SQLLit(outcome.watermark_column),
	    outcome.pk_columns.empty() ? "NULL" : SQLLit(outcome.pk_columns), ValueSql(outcome.last_watermark),
	    ValueSql(outcome.last_source_snapshot), ValueSql(outcome.last_full_sync_snapshot), outcome.row_count);
	auto ins_res = con->Query(insert_sql);
	CheckResult(*ins_res, "Failed to insert replication table state");
}

//! Records a per-table failure without touching row_count_dest (preserved via ON CONFLICT).
static void MarkTableError(ClientContext &context, DuckLakeCatalog &dest, const DuckLakeReplicationJob &job,
                           const SourceTableRef &ref, const string &error) {
	auto con = MakeConnection(context, dest.GetDatabase());
	auto state_table = MetaTableName(dest, "ducklake_replication_table");
	auto insert_sql = StringUtil::Format(
	    "INSERT INTO %s (replication_id, source_schema, source_table, dest_schema, dest_table, strategy, "
	    "watermark_column, pk_columns, last_watermark, last_source_snapshot, last_full_sync_snapshot, "
	    "row_count_dest, last_error, synced_at) "
	    "VALUES (%llu, %s, %s, %s, %s, 'full', NULL, NULL, NULL, NULL, NULL, NULL, %s, now()) "
	    "ON CONFLICT (replication_id, source_schema, source_table) DO UPDATE SET last_error = "
	    "excluded.last_error, synced_at = excluded.synced_at",
	    state_table, job.replication_id, SQLLit(ref.schema), SQLLit(ref.table), SQLLit(ref.schema), SQLLit(ref.table),
	    SQLLit(error));
	auto ins_res = con->Query(insert_sql);
	CheckResult(*ins_res, "Failed to record replication table error");
}

static void DeleteTableState(ClientContext &context, DuckLakeCatalog &dest, uint64_t replication_id,
                             const string &schema, const string &table) {
	auto con = MakeConnection(context, dest.GetDatabase());
	auto state_table = MetaTableName(dest, "ducklake_replication_table");
	auto delete_sql = StringUtil::Format("DELETE FROM %s WHERE replication_id = %llu AND source_schema = %s AND "
	                                     "source_table = %s",
	                                     state_table, replication_id, SQLLit(schema), SQLLit(table));
	auto del_res = con->Query(delete_sql);
	CheckResult(*del_res, "Failed to delete replication table state");
}

//! Returns the replication_id of another job that already manages this destination table, or 0.
static uint64_t ConflictingOwner(ClientContext &context, DuckLakeCatalog &dest, uint64_t replication_id,
                                 const string &schema, const string &table) {
	auto con = MakeConnection(context, dest.GetDatabase());
	auto state_table = MetaTableName(dest, "ducklake_replication_table");
	auto query = StringUtil::Format("SELECT replication_id FROM %s WHERE replication_id <> %llu AND dest_schema = %s "
	                                "AND dest_table = %s",
	                                state_table, replication_id, SQLLit(schema), SQLLit(table));
	auto result = con->Query(query);
	CheckResult(*result, "Failed to check replication table ownership");
	if (result->RowCount() == 0) {
		return 0;
	}
	return result->GetValue(0, 0).GetValue<uint64_t>();
}

static bool SourceListContains(const vector<SourceTableRef> &tables, const string &schema, const string &table) {
	for (auto &ref : tables) {
		if (ref.schema == schema && ref.table == table) {
			return true;
		}
	}
	return false;
}

static void UpdateJobStatus(ClientContext &context, DuckLakeCatalog &dest, uint64_t replication_id,
                            const string &status, const string &last_error, bool mark_ok) {
	auto con = MakeConnection(context, dest.GetDatabase());
	auto table = MetaTableName(dest, "ducklake_replication");
	// A concurrent pause wins over the cycle's own transition.
	string sql = StringUtil::Format("UPDATE %s SET status = CASE WHEN status = 'paused' THEN 'paused' ELSE %s END, "
	                                "last_error = %s, last_run_ts = now()",
	                                table, SQLLit(status), last_error.empty() ? "NULL" : SQLLit(last_error));
	if (mark_ok) {
		sql += ", last_ok_ts = now()";
	}
	sql += StringUtil::Format(" WHERE replication_id = %llu", replication_id);
	auto result = con->Query(sql);
	CheckResult(*result, "Failed to update replication job status");
}

static bool HasLiveSupervisor(DatabaseInstance &db, const string &dest_name);

//! Guards against overlapping catchup cycles on the same job (manual call vs supervisor).
static mutex in_flight_lock;
static unordered_set<string> in_flight_cycles;

class CycleGuard {
public:
	CycleGuard(DatabaseInstance &db, const string &dest_name, uint64_t replication_id)
	    : key(to_string(reinterpret_cast<uintptr_t>(&db)) + "|" + dest_name + "|" + to_string(replication_id)) {
		lock_guard<mutex> guard(in_flight_lock);
		if (!in_flight_cycles.insert(key).second) {
			throw InvalidInputException("replication job %llu is already running a catchup cycle", replication_id);
		}
	}
	~CycleGuard() {
		lock_guard<mutex> guard(in_flight_lock);
		in_flight_cycles.erase(key);
	}

private:
	string key;
};

//===--------------------------------------------------------------------===//
// State tables
//===--------------------------------------------------------------------===//

void DuckLakeReplication::EnsureStateTables(ClientContext &context, DuckLakeCatalog &dest) {
	auto con = MakeConnection(context, dest.GetDatabase());
	auto job_table = MetaTableName(dest, "ducklake_replication");
	auto table_table = MetaTableName(dest, "ducklake_replication_table");
	auto job_sql = StringUtil::Format(
	    "CREATE TABLE IF NOT EXISTS %s (replication_id BIGINT PRIMARY KEY, source_catalog VARCHAR NOT NULL, "
	    "dest_catalog VARCHAR NOT NULL, status VARCHAR NOT NULL, data_sync_mode VARCHAR NOT NULL, interval_ms "
	    "BIGINT NOT NULL, include_patterns VARCHAR, exclude_patterns VARCHAR, watermark_columns VARCHAR, "
	    "last_run_ts TIMESTAMPTZ, last_ok_ts TIMESTAMPTZ, last_error VARCHAR, created_at TIMESTAMPTZ NOT NULL "
	    "DEFAULT now())",
	    job_table);
	auto job_res = con->Query(job_sql);
	CheckResult(*job_res, "Failed to create DuckLake replication state table");
	// Upgrade lakes whose state table predates watermark_columns.
	auto alter_sql = StringUtil::Format("ALTER TABLE %s ADD COLUMN IF NOT EXISTS watermark_columns VARCHAR", job_table);
	auto alter_res = con->Query(alter_sql);
	CheckResult(*alter_res, "Failed to migrate DuckLake replication state table");
	auto table_sql = StringUtil::Format(
	    "CREATE TABLE IF NOT EXISTS %s (replication_id BIGINT NOT NULL, source_schema VARCHAR NOT NULL, "
	    "source_table VARCHAR NOT NULL, dest_schema VARCHAR NOT NULL, dest_table VARCHAR NOT NULL, strategy "
	    "VARCHAR NOT NULL, watermark_column VARCHAR, pk_columns VARCHAR, last_watermark VARCHAR, "
	    "last_source_snapshot BIGINT, last_full_sync_snapshot BIGINT, row_count_dest BIGINT, last_error VARCHAR, "
	    "synced_at TIMESTAMPTZ, PRIMARY KEY (replication_id, source_schema, source_table))",
	    table_table);
	auto table_res = con->Query(table_sql);
	CheckResult(*table_res, "Failed to create DuckLake replication table-state table");
}

//===--------------------------------------------------------------------===//
// Job CRUD
//===--------------------------------------------------------------------===//

uint64_t DuckLakeReplication::CreateJob(ClientContext &context, DuckLakeCatalog &dest, const string &source_catalog,
                                        uint64_t interval_ms, const string &data_sync_mode, const string &include,
                                        const string &exclude, const string &watermark_columns) {
	if (StringUtil::CIEquals(source_catalog, dest.GetName().GetIdentifierName())) {
		throw InvalidInputException("Source and destination catalogs must differ");
	}
	if (!StringUtil::CIEquals(data_sync_mode, "copy") && !StringUtil::CIEquals(data_sync_mode, "share")) {
		throw InvalidInputException("ducklake_replicate_create: data_sync_mode '%s' is not supported - expected "
		                            "'copy' or 'share'",
		                            data_sync_mode);
	}
	auto &source = GetAttachedCatalog(context, source_catalog);
	if (StringUtil::CIEquals(data_sync_mode, "share") && !StringUtil::CIEquals(source.GetCatalogType(), "ducklake")) {
		throw InvalidInputException("ducklake_replicate_create: share mode requires a DuckLake source catalog "
		                            "(source \"%s\" is %s)",
		                            source_catalog, source.GetCatalogType());
	}
	ParseWatermarkSpec(watermark_columns);

	EnsureStateTables(context, dest);
	auto con = MakeConnection(context, dest.GetDatabase());
	auto table = MetaTableName(dest, "ducklake_replication");
	auto insert_sql = StringUtil::Format(
	    "INSERT INTO %s (replication_id, source_catalog, dest_catalog, status, data_sync_mode, interval_ms, "
	    "include_patterns, exclude_patterns, watermark_columns, created_at) "
	    "SELECT COALESCE(MAX(replication_id), 0) + 1, %s, %s, 'created', %s, %llu, %s, %s, %s, now() FROM %s",
	    table, SQLLit(source_catalog), SQLLit(dest.GetName().GetIdentifierName()), SQLLit(data_sync_mode), interval_ms,
	    SQLLit(include), SQLLit(exclude), watermark_columns.empty() ? "NULL" : SQLLit(watermark_columns), table);
	auto ins_res = con->Query(insert_sql);
	CheckResult(*ins_res, "Failed to create replication job");
	auto id_sql = StringUtil::Format("SELECT MAX(replication_id) FROM %s", table);
	auto id_res = con->Query(id_sql);
	CheckResult(*id_res, "Failed to read new replication id");
	return id_res->GetValue(0, 0).GetValue<uint64_t>();
}

void DuckLakeReplication::DropJob(ClientContext &context, DuckLakeCatalog &dest, uint64_t replication_id) {
	EnsureStateTables(context, dest);
	auto con = MakeConnection(context, dest.GetDatabase());
	auto job_table = MetaTableName(dest, "ducklake_replication");
	auto table_table = MetaTableName(dest, "ducklake_replication_table");
	auto table_sql = StringUtil::Format("DELETE FROM %s WHERE replication_id = %llu", table_table, replication_id);
	auto table_res = con->Query(table_sql);
	CheckResult(*table_res, "Failed to drop replication table state");
	auto job_sql = StringUtil::Format("DELETE FROM %s WHERE replication_id = %llu", job_table, replication_id);
	auto job_res = con->Query(job_sql);
	CheckResult(*job_res, "Failed to drop replication job");
}

static DuckLakeReplicationJob JobFromRow(const vector<Value> &values) {
	DuckLakeReplicationJob job;
	job.replication_id = values[0].GetValue<uint64_t>();
	job.source_catalog = values[1].IsNull() ? "" : values[1].GetValue<string>();
	job.dest_catalog = values[2].IsNull() ? "" : values[2].GetValue<string>();
	job.status = values[3].IsNull() ? "" : values[3].GetValue<string>();
	job.data_sync_mode = values[4].IsNull() ? "" : values[4].GetValue<string>();
	job.interval_ms = values[5].IsNull() ? 0 : values[5].GetValue<uint64_t>();
	job.include_patterns = values[6].IsNull() ? "" : values[6].GetValue<string>();
	job.exclude_patterns = values[7].IsNull() ? "" : values[7].GetValue<string>();
	job.watermark_columns = values[8].IsNull() ? "" : values[8].GetValue<string>();
	job.last_run_ts = values[9];
	job.last_ok_ts = values[10];
	job.last_error = values[11].IsNull() ? "" : values[11].GetValue<string>();
	job.created_at = values[12];
	return job;
}

static const char *JOB_COLUMNS = "replication_id, source_catalog, dest_catalog, status, data_sync_mode, interval_ms, "
                                 "include_patterns, exclude_patterns, watermark_columns, last_run_ts, last_ok_ts, "
                                 "last_error, created_at";

vector<DuckLakeReplicationJob> DuckLakeReplication::ListJobs(ClientContext &context, DuckLakeCatalog &dest) {
	EnsureStateTables(context, dest);
	auto con = MakeConnection(context, dest.GetDatabase());
	auto table = MetaTableName(dest, "ducklake_replication");
	auto query = StringUtil::Format("SELECT %s FROM %s ORDER BY replication_id", JOB_COLUMNS, table);
	auto result = con->Query(query);
	CheckResult(*result, "Failed to list replication jobs");
	vector<DuckLakeReplicationJob> jobs;
	for (auto &row : *result) {
		vector<Value> values;
		for (idx_t c = 0; c < 13; c++) {
			values.push_back(row.GetBaseValue(c));
		}
		jobs.push_back(JobFromRow(values));
	}
	return jobs;
}

bool DuckLakeReplication::TryGetJob(ClientContext &context, DuckLakeCatalog &dest, uint64_t replication_id,
                                    DuckLakeReplicationJob &result_job) {
	EnsureStateTables(context, dest);
	auto con = MakeConnection(context, dest.GetDatabase());
	auto table = MetaTableName(dest, "ducklake_replication");
	auto query =
	    StringUtil::Format("SELECT %s FROM %s WHERE replication_id = %llu", JOB_COLUMNS, table, replication_id);
	auto result = con->Query(query);
	CheckResult(*result, "Failed to load replication job");
	if (result->RowCount() == 0) {
		return false;
	}
	vector<Value> values;
	for (idx_t c = 0; c < 13; c++) {
		values.push_back(result->GetValue(c, 0));
	}
	result_job = JobFromRow(values);
	return true;
}

vector<DuckLakeReplicationTableState>
DuckLakeReplication::ListTableStates(ClientContext &context, DuckLakeCatalog &dest, uint64_t replication_id) {
	EnsureStateTables(context, dest);
	auto con = MakeConnection(context, dest.GetDatabase());
	auto table = MetaTableName(dest, "ducklake_replication_table");
	auto query = StringUtil::Format(
	    "SELECT replication_id, source_schema, source_table, dest_schema, dest_table, strategy, watermark_column, "
	    "pk_columns, last_watermark, last_source_snapshot, last_full_sync_snapshot, row_count_dest, last_error, "
	    "synced_at FROM %s WHERE replication_id = %llu ORDER BY source_schema, source_table",
	    table, replication_id);
	auto result = con->Query(query);
	CheckResult(*result, "Failed to list replication table states");
	vector<DuckLakeReplicationTableState> states;
	for (auto &row : *result) {
		DuckLakeReplicationTableState state;
		state.replication_id = row.GetValue<uint64_t>(0);
		state.source_schema = row.IsNull(1) ? "" : row.GetValue<string>(1);
		state.source_table = row.IsNull(2) ? "" : row.GetValue<string>(2);
		state.dest_schema = row.IsNull(3) ? "" : row.GetValue<string>(3);
		state.dest_table = row.IsNull(4) ? "" : row.GetValue<string>(4);
		state.strategy = row.IsNull(5) ? "" : row.GetValue<string>(5);
		state.watermark_column = row.IsNull(6) ? "" : row.GetValue<string>(6);
		state.pk_columns = row.IsNull(7) ? "" : row.GetValue<string>(7);
		state.last_watermark = row.GetBaseValue(8);
		state.last_source_snapshot = row.GetBaseValue(9);
		state.last_full_sync_snapshot = row.GetBaseValue(10);
		state.row_count_dest = row.IsNull(11) ? 0 : row.GetValue<int64_t>(11);
		state.last_error = row.IsNull(12) ? "" : row.GetValue<string>(12);
		state.synced_at = row.GetBaseValue(13);
		states.push_back(std::move(state));
	}
	return states;
}

//===--------------------------------------------------------------------===//
// Catchup
//===--------------------------------------------------------------------===//

//! Picks the per-table sync strategy: configured watermark > ducklake snapshot > PK merge > full.
static string ChooseStrategy(ClientContext &context, Catalog &source_catalog, const DuckLakeReplicationJob &job,
                             const SourceTableRef &ref, const case_insensitive_map_t<string> &watermark_spec,
                             const vector<ReplColumnInfo> &source_columns, string &watermark_column,
                             vector<string> &pk_columns) {
	if (StringUtil::CIEquals(job.data_sync_mode, "share")) {
		if (!StringUtil::CIEquals(source_catalog.GetCatalogType(), "ducklake")) {
			throw InvalidInputException("share mode requires a DuckLake source catalog (source \"%s\" is %s)",
			                            job.source_catalog, source_catalog.GetCatalogType());
		}
		return "share";
	}
	auto spec = watermark_spec.find(ref.schema + "." + ref.table);
	if (spec != watermark_spec.end()) {
		for (auto &col : source_columns) {
			if (StringUtil::CIEquals(col.name, spec->second)) {
				watermark_column = col.name;
				return "watermark";
			}
		}
		throw InvalidInputException("watermark column \"%s\" not found on source table %s.%s", spec->second, ref.schema,
		                            ref.table);
	}
	if (StringUtil::CIEquals(source_catalog.GetCatalogType(), "ducklake")) {
		return "snapshot";
	}
	pk_columns = PrimaryKeyColumns(context, source_catalog, job.source_catalog, ref.schema, ref.table);
	if (!pk_columns.empty()) {
		return "merge";
	}
	return "full";
}

//! Applies source inserts into `dst_table` for rows whose watermark falls in (low, high]; a NULL low
//! watermark means this is the first incremental pass and copies everything up to `high`.
static void ApplyWatermarkDelta(Connection &apply, const string &dst_table, const string &src_table,
                                const string &watermark_column, const string &watermark_type, const Value &low,
                                const Value &high) {
	auto wm = SQLId(watermark_column);
	string predicate = StringUtil::Format("%s <= CAST(%s AS %s)", wm, SQLLit(high.ToString()), watermark_type);
	if (!low.IsNull()) {
		predicate =
		    StringUtil::Format("%s > CAST(%s AS %s) AND %s", wm, SQLLit(low.ToString()), watermark_type, predicate);
	}
	auto sql = StringUtil::Format("INSERT INTO %s SELECT * FROM %s WHERE %s", dst_table, src_table, predicate);
	CheckResult(*apply.Query(sql), "Failed to watermark-sync into " + dst_table);
}

//! Resolves the snapshot to list at - the catalog head when at_snapshot is not set.
static DuckLakeSnapshot ResolveSnapshot(DuckLakeTransaction &transaction, optional_idx at_snapshot) {
	unique_ptr<BoundAtClause> at_clause;
	if (at_snapshot.IsValid()) {
		at_clause = make_uniq<BoundAtClause>("version", Value::BIGINT(at_snapshot.GetIndex()));
	}
	return transaction.GetSnapshot(at_clause.get());
}

//! Lists the live files of a DuckLake table, optionally at a specific snapshot. Paths come back
//! absolute (resolved against the catalog data path).
static vector<DuckLakeFileListExtendedEntry> ListLakeFiles(Connection &apply, Catalog &catalog,
                                                           const string &schema_name, const string &table_name,
                                                           optional_idx at_snapshot) {
	auto entry = catalog.GetEntry<TableCatalogEntry>(
	    *apply.context, QualifiedName(catalog.GetName(), Identifier(schema_name), Identifier(table_name)),
	    OnEntryNotFound::RETURN_NULL);
	if (!entry) {
		return {};
	}
	auto &table_entry = entry->Cast<DuckLakeTableEntry>();
	auto &transaction = DuckLakeTransaction::Get(*apply.context, catalog);
	auto snapshot = ResolveSnapshot(transaction, at_snapshot);
	return transaction.GetMetadataManager().GetExtendedFilesForTable(table_entry, snapshot, nullptr);
}

//! Records paths the destination catalog references but does not own; expire/cleanup never
//! physically delete them. Runs on its own connection - writing to the metadata catalog from the
//! apply transaction would trip the single-catalog write rule, and stale markers are harmless.
static void MarkExternalFiles(ClientContext &context, DuckLakeCatalog &dest, const vector<string> &paths) {
	if (paths.empty()) {
		return;
	}
	auto con = MakeConnection(context, dest.GetDatabase());
	auto table = MetaTableName(dest, "ducklake_external_file");
	auto create_res = con->Query("CREATE TABLE IF NOT EXISTS " + table + " (path VARCHAR NOT NULL)");
	CheckResult(*create_res, "Failed to create external-file table");
	for (idx_t offset = 0; offset < paths.size(); offset += 500) {
		auto count = MinValue<idx_t>(500, paths.size() - offset);
		string values;
		for (idx_t i = offset; i < offset + count; i++) {
			if (!values.empty()) {
				values += ", ";
			}
			values += "(" + SQLLit(StringUtil::Replace(paths[i], "\\", "/")) + ")";
		}
		auto insert_sql = StringUtil::Format("INSERT INTO %s SELECT v.path FROM (VALUES %s) v(path) WHERE NOT EXISTS "
		                                     "(SELECT 1 FROM %s e WHERE e.path = v.path)",
		                                     table, values, table);
		CheckResult(*con->Query(insert_sql), "Failed to mark external files");
	}
}

//! Share mode: align the dest table's registered file set with the source's at high_snapshot.
//! Data bytes are never copied - source paths are registered into the dest catalog directly and
//! marked externally owned so expire/cleanup never touches them.
static void ApplyShareSync(ClientContext &context, Connection &apply, Catalog &source_catalog, DuckLakeCatalog &dest,
                           const SourceTableRef &ref, optional_idx high_snapshot) {
	auto src_entry = source_catalog.GetEntry<TableCatalogEntry>(
	    *apply.context, QualifiedName(source_catalog.GetName(), Identifier(ref.schema), Identifier(ref.table)),
	    OnEntryNotFound::THROW_EXCEPTION);
	auto &src_table = src_entry->Cast<DuckLakeTableEntry>();
	auto partition_data = src_table.GetPartitionData();
	if (partition_data && !partition_data->fields.empty()) {
		throw InvalidInputException("share mode cannot replicate partitioned table %s.%s - partition values are "
		                            "not carried by the data files",
		                            ref.schema, ref.table);
	}
	auto &src_txn = DuckLakeTransaction::Get(*apply.context, source_catalog);
	auto src_snapshot = ResolveSnapshot(src_txn, high_snapshot);
	// Inlined rows never appear in the file listing - reject instead of silently syncing zero files.
	auto &src_meta = src_txn.GetMetadataManager();
	for (auto &inlined : src_meta.GetInlinedDataTablesForTable(src_table.GetTableId())) {
		if (src_meta.GetNetInlinedRowCount(inlined.table_name, src_snapshot) > 0) {
			throw InvalidInputException("share mode cannot replicate table %s.%s: it has inlined data - use copy "
			                            "mode or flush the source with ducklake_flush_inlined_data",
			                            ref.schema, ref.table);
		}
	}
	// Files may carry partial_max (embedded per-row snapshot ids). The staged registration never
	// propagates it: every source row is committed-visible at share time, so no snapshot filter may
	// apply on the dest - dest snapshot ids are unrelated to the embedded source ids.
	auto src_files = ListLakeFiles(apply, source_catalog, ref.schema, ref.table, high_snapshot);
	for (auto &file : src_files) {
		if (file.data_type != DuckLakeDataType::DATA_FILE) {
			throw InvalidInputException("share mode cannot replicate table %s.%s: it has inlined data - use copy "
			                            "mode or flush the source",
			                            ref.schema, ref.table);
		}
	}
	auto dst_files = ListLakeFiles(apply, dest, ref.schema, ref.table, optional_idx());

	// Path canonicalization differs between catalogs (glob expansion resolves symlinks) - match on
	// the final component instead; DuckLake file names are generated unique ids.
	auto base_name = [](const string &path) {
		return path.substr(path.find_last_of("/\\") + 1);
	};
	unordered_map<string, idx_t> src_by_name;
	for (idx_t i = 0; i < src_files.size(); i++) {
		src_by_name[base_name(src_files[i].file.path)] = i;
	}
	auto &dest_txn = DuckLakeTransaction::Get(*apply.context, dest);
	auto &dest_table =
	    dest.GetEntry<TableCatalogEntry>(*apply.context,
	                                     QualifiedName(dest.GetName(), Identifier(ref.schema), Identifier(ref.table)),
	                                     OnEntryNotFound::THROW_EXCEPTION)
	        ->Cast<DuckLakeTableEntry>();
	auto table_id = dest_table.GetTableId();

	// Dest delete files are written under a deterministic name derived from the source delete file -
	// "ducklake-share-<source stem>.<dest ext>" - so reconciliation can detect a changed source
	// delete file without extra state.
	auto dest_format = dest.GetDataFileFormat(*apply.context, dest_table);
	auto share_delete_name = [&](const string &src_delete_path) {
		auto base = base_name(src_delete_path);
		auto dot = base.find_last_of('.');
		auto stem = dot == string::npos ? base : base.substr(0, dot);
		return "ducklake-share-" + stem + (StringUtil::CIEquals(dest_format, "vortex") ? ".vortex" : ".parquet");
	};

	// Dest registrations absent from the source set - or whose delete file no longer matches - are
	// dropped (metadata tombstone only; the physical bytes stay with the source).
	for (auto &file : dst_files) {
		auto it = src_by_name.find(base_name(file.file.path));
		bool delete_mismatch = false;
		if (it != src_by_name.end()) {
			auto expected = src_files[it->second].delete_file.path.empty()
			                    ? string()
			                    : share_delete_name(src_files[it->second].delete_file.path);
			delete_mismatch = base_name(file.delete_file.path) != expected;
		}
		if (it == src_by_name.end() || delete_mismatch) {
			dest_txn.DropFile(table_id, file.file_id, file.file.path, file.row_count, file.file.file_size_bytes);
		}
	}

	// Register missing source files, grouped by file format. Source delete files ride along on the
	// staged data file so the commit binds them to the freshly assigned data_file_id.
	unordered_map<string, vector<idx_t>> missing_by_format;
	for (idx_t i = 0; i < src_files.size(); i++) {
		auto &file = src_files[i];
		auto expected_delete = file.delete_file.path.empty() ? string() : share_delete_name(file.delete_file.path);
		auto registered = false;
		for (auto &dst_file : dst_files) {
			if (base_name(dst_file.file.path) == base_name(file.file.path) &&
			    base_name(dst_file.delete_file.path) == expected_delete) {
				registered = true;
				break;
			}
		}
		if (!registered) {
			missing_by_format[file.file.file_format].push_back(i);
		}
	}
	vector<string> external_paths;
	vector<DuckLakeDataFile> staged_files;
	auto &fs = FileSystem::GetFileSystem(*apply.context);
	for (auto &entry : missing_by_format) {
		vector<string> paths;
		for (auto idx : entry.second) {
			paths.push_back(src_files[idx].file.path);
		}
		auto new_files = DuckLakePrepareExternalFiles(dest_txn, *apply.context, dest, dest_table, paths, entry.first);
		for (auto &new_file : new_files) {
			auto &src_file = src_files[src_by_name[base_name(new_file.file_name)]];
			new_file.encryption_key = src_file.file.encryption_key;
			// Preserve the source's row-id span - required for files that physically embed row ids.
			if (src_file.row_id_start.IsValid()) {
				new_file.flush_row_id_start = src_file.row_id_start;
			}
			if (!src_file.delete_file.path.empty()) {
				// Source delete files can embed per-delete snapshot ids in the source's snapshot
				// space, which dest snapshot filtering would wrongly apply - so the deletes are
				// materialized into a fresh dest-owned delete file instead of sharing the bytes.
				auto scan = DuckLakeDeleteFilter::ScanDeleteFile(*apply.context, src_file.delete_file);
				set<idx_t> positions(scan.deleted_rows.begin(), scan.deleted_rows.end());
				WriteDeleteFileInput del_input(*apply.context, dest_txn, fs, dest_table.DataPath(),
				                               src_file.file.encryption_key, new_file.file_name, std::move(positions),
				                               DeleteFileSource::REGULAR, dest_format);
				del_input.file_name = share_delete_name(src_file.delete_file.path);
				new_file.delete_files.push_back(DuckLakeDeleteFileWriter::WriteDeleteFile(*apply.context, del_input));
			}
			staged_files.push_back(std::move(new_file));
		}
	}
	// Mark every source-owned data path the dest references as external - a path must never become
	// eligible for physical deletion while the source still owns it. Delete files are dest-owned
	// rewrites and need no marker. Use the dest-side (glob-canonicalized) form so expire/cleanup
	// comparisons match the stored path.
	for (auto &file : dst_files) {
		external_paths.push_back(file.file.path);
	}
	for (auto &file : staged_files) {
		external_paths.push_back(file.file_name);
	}
	MarkExternalFiles(context, dest, external_paths);
	if (!staged_files.empty()) {
		dest_txn.AppendFiles(table_id, std::move(staged_files));
	}
}

DuckLakeReplicationCatchupResult DuckLakeReplication::Catchup(ClientContext &context, DuckLakeCatalog &dest,
                                                              uint64_t replication_id, const atomic<bool> *abort) {
	DuckLakeReplicationJob job;
	if (!TryGetJob(context, dest, replication_id, job)) {
		throw InvalidInputException("No replication job with id %llu in catalog \"%s\"", replication_id,
		                            dest.GetName().GetIdentifierName());
	}
	if (StringUtil::CIEquals(job.status, "paused")) {
		throw InvalidInputException("Replication job %llu is paused - resume it before catching up", replication_id);
	}
	CycleGuard cycle_guard(*context.db, dest.GetName().GetIdentifierName(), replication_id);

	// The destination writes data files; supervisor cycles never pass through function bind.
	auto &db = *context.db;
	if (!db.ExtensionIsLoaded("parquet")) {
		DuckDB instance(db);
		ExtensionHelper::LoadExtension(instance, "parquet");
	}

	DuckLakeReplicationCatchupResult result;
	result.replication_id = replication_id;

	try {
		if (IsReplicationAborting(abort)) {
			result.message = "cycle aborted";
			return result;
		}
		auto include = SplitPatterns(job.include_patterns);
		auto exclude = SplitPatterns(job.exclude_patterns);
		auto watermark_spec = ParseWatermarkSpec(job.watermark_columns);
		auto &source_catalog = GetAttachedCatalog(context, job.source_catalog);
		auto source_is_ducklake = StringUtil::CIEquals(source_catalog.GetCatalogType(), "ducklake");
		auto source_tables = ListSourceTables(context, source_catalog, job.source_catalog, include, exclude);
		auto states = ListTableStates(context, dest, replication_id);

		auto apply = MakeConnection(context, dest.GetDatabase());
		auto dest_name = dest.GetName().GetIdentifierName();
		auto src_id = SQLId(job.source_catalog);
		auto dst_id = SQLId(dest_name);

		vector<string> failures;
		idx_t dropped = 0;

		// Drop dest tables whose source table disappeared (or became excluded).
		for (auto &state : states) {
			if (IsReplicationAborting(abort)) {
				break;
			}
			if (SourceListContains(source_tables, state.source_schema, state.source_table)) {
				continue;
			}
			auto label = state.source_schema + "." + state.source_table;
			auto dst_table = dst_id + "." + SQLId(state.dest_schema) + "." + SQLId(state.dest_table);
			bool in_transaction = false;
			try {
				auto begin = apply->Query("BEGIN TRANSACTION");
				CheckResult(*begin, "Failed to start replication transaction");
				in_transaction = true;
				auto drop_res = apply->Query("DROP TABLE IF EXISTS " + dst_table);
				CheckResult(*drop_res, "Failed to drop stale table " + dst_table);
				auto commit_res = apply->Query("COMMIT");
				CheckResult(*commit_res, "Failed to commit replication transaction");
				in_transaction = false;
				DeleteTableState(context, dest, replication_id, state.source_schema, state.source_table);
				dropped++;
			} catch (std::exception &ex) {
				if (IsReplicationAborting(abort)) {
					break;
				}
				if (in_transaction) {
					apply->Query("ROLLBACK");
				}
				failures.push_back(label + " - " + ex.what());
				SourceTableRef ref {state.source_schema, state.source_table};
				MarkTableError(context, dest, job, ref, ex.what());
			}
		}

		// Sync each source table in its own transaction so one failure cannot abort the cycle.
		for (auto &ref : source_tables) {
			if (IsReplicationAborting(abort)) {
				break;
			}
			auto label = ref.schema + "." + ref.table;
			auto src_table = src_id + "." + SQLId(ref.schema) + "." + SQLId(ref.table);
			auto dst_table = dst_id + "." + SQLId(ref.schema) + "." + SQLId(ref.table);
			bool in_transaction = false;
			// Ownership rejections must not write a state row - the dest table is not ours.
			bool claims_dest = true;
			TableSyncOutcome outcome;
			try {
				auto owner = ConflictingOwner(context, dest, replication_id, ref.schema, ref.table);
				if (owner != 0) {
					claims_dest = false;
					throw InvalidInputException("destination table %s is already managed by replication job %llu",
					                            label, owner);
				}
				const DuckLakeReplicationTableState *prior = nullptr;
				for (auto &state : states) {
					if (state.source_schema == ref.schema && state.source_table == ref.table) {
						prior = &state;
						break;
					}
				}
				auto source_columns = ListColumns(context, source_catalog, job.source_catalog, ref.schema, ref.table);
				auto dest_columns = ListColumns(context, dest, dest_name, ref.schema, ref.table);
				if (!dest_columns.empty() && !prior) {
					claims_dest = false;
					throw InvalidInputException("destination table %s already exists and is not managed by this "
					                            "replication job",
					                            label);
				}

				vector<string> pk_columns;
				outcome.strategy = ChooseStrategy(context, source_catalog, job, ref, watermark_spec, source_columns,
				                                  outcome.watermark_column, pk_columns);
				if (!pk_columns.empty()) {
					outcome.pk_columns = StringUtil::Join(pk_columns, ",");
				}

				auto begin = apply->Query("BEGIN TRANSACTION");
				CheckResult(*begin, "Failed to start replication transaction");
				in_transaction = true;

				auto schema_sql = StringUtil::Format("CREATE SCHEMA IF NOT EXISTS %s.%s", dst_id, SQLId(ref.schema));
				auto schema_res = apply->Query(schema_sql);
				CheckResult(*schema_res, "Failed to create schema " + ref.schema + " on destination");

				// Schema sync: identical keeps data, trailing additions ALTER in place, anything
				// else drops and recreates (which forces a full re-seed).
				bool recreated = false;
				bool extended = false;
				if (dest_columns.empty()) {
					recreated = true;
				} else if (SameColumns(source_columns, dest_columns)) {
				} else if (IsAdditiveSchemaChange(source_columns, dest_columns)) {
					extended = true;
				} else {
					auto drop_res = apply->Query(StringUtil::Format("DROP TABLE %s", dst_table));
					CheckResult(*drop_res, "Failed to drop drifted table " + dst_table);
					recreated = true;
				}
				if (recreated) {
					auto create_sql =
					    StringUtil::Format("CREATE TABLE %s AS SELECT * FROM %s WHERE 1 = 0", dst_table, src_table);
					auto create_res = apply->Query(create_sql);
					CheckResult(*create_res, "Failed to create table " + dst_table);
				} else if (extended) {
					for (idx_t i = dest_columns.size(); i < source_columns.size(); i++) {
						auto &col = source_columns[i];
						auto alter_sql =
						    StringUtil::Format("ALTER TABLE %s ADD COLUMN %s %s", dst_table, SQLId(col.name), col.type);
						if (col.has_default) {
							alter_sql += " DEFAULT " + col.default_value;
						}
						auto alter_res = apply->Query(alter_sql);
						CheckResult(*alter_res, "Failed to add column " + col.name + " to " + dst_table);
					}
				}

				// Cursor bounds read inside the table transaction so the apply sees a consistent
				// source point-in-time. (ducklake_last_committed_snapshot only reports snapshots
				// committed by this connection - the catalog high-water mark comes from
				// ducklake_snapshots instead.)
				Value high_watermark;
				Value high_snapshot;
				string watermark_type;
				if (outcome.strategy == "watermark") {
					for (auto &col : source_columns) {
						if (StringUtil::CIEquals(col.name, outcome.watermark_column)) {
							watermark_type = col.type;
							break;
						}
					}
					high_watermark =
					    ScalarValue(*apply, StringUtil::Format("SELECT MAX(%s) FROM %s",
					                                           SQLId(outcome.watermark_column), src_table));
					if (!high_watermark.IsNull()) {
						high_watermark = high_watermark.DefaultCastAs(LogicalType::VARCHAR);
					}
				}
				if (source_is_ducklake) {
					high_snapshot = ScalarValue(*apply, "SELECT MAX(snapshot_id) FROM ducklake_snapshots(" +
					                                        SQLLit(job.source_catalog) + ")");
					high_snapshot = high_snapshot.DefaultCastAs(LogicalType::BIGINT);
				}

				bool reseed = recreated || outcome.strategy == "full";
				if (outcome.strategy == "watermark" &&
				    (!prior || prior->last_watermark.IsNull() ||
				     !StringUtil::CIEquals(prior->watermark_column, outcome.watermark_column))) {
					reseed = true;
				}
				if (outcome.strategy == "snapshot" && (!prior || prior->last_source_snapshot.IsNull())) {
					reseed = true;
				}
				// Merge backfills extended columns via its diff; watermark/snapshot must re-seed.
				if (extended && outcome.strategy != "merge") {
					reseed = true;
				}
				// Source snapshot expiry can strand the cursor; fall back to a reseed.
				if (!reseed && outcome.strategy == "snapshot") {
					auto min_snap = ScalarValue(*apply, "SELECT MIN(snapshot_id) FROM ducklake_snapshots(" +
					                                        SQLLit(job.source_catalog) + ")");
					if (!min_snap.IsNull() &&
					    prior->last_source_snapshot.GetValue<int64_t>() < min_snap.GetValue<int64_t>()) {
						reseed = true;
					}
				}

				if (outcome.strategy == "share") {
					// File-set reconcile replaces row copying entirely; reseed flags are unused.
					ApplyShareSync(context, *apply, source_catalog, dest, ref,
					               high_snapshot.IsNull() ? optional_idx()
					                                      : optional_idx(high_snapshot.GetValue<int64_t>()));
				} else if (reseed) {
					if (!dest_columns.empty()) {
						auto clear_res = apply->Query(StringUtil::Format("DELETE FROM %s", dst_table));
						CheckResult(*clear_res, "Failed to clear table " + dst_table);
					}
					if (outcome.strategy == "watermark") {
						// Rows above the bound arrive in-flight; the next pass picks them up.
						if (!high_watermark.IsNull()) {
							ApplyWatermarkDelta(*apply, dst_table, src_table, outcome.watermark_column, watermark_type,
							                    Value(), high_watermark);
						}
					} else if (outcome.strategy == "snapshot") {
						auto ins_sql = StringUtil::Format("INSERT INTO %s SELECT * FROM %s AT (VERSION => %lld)",
						                                  dst_table, src_table, high_snapshot.GetValue<int64_t>());
						CheckResult(*apply->Query(ins_sql), "Failed to seed table " + dst_table);
					} else {
						auto insert_res =
						    apply->Query(StringUtil::Format("INSERT INTO %s SELECT * FROM %s", dst_table, src_table));
						CheckResult(*insert_res, "Failed to copy into " + dst_table);
					}
					if (source_is_ducklake) {
						outcome.last_full_sync_snapshot = high_snapshot;
					}
				} else if (outcome.strategy == "watermark") {
					if (!high_watermark.IsNull()) {
						ApplyWatermarkDelta(*apply, dst_table, src_table, outcome.watermark_column, watermark_type,
						                    prior->last_watermark, high_watermark);
					}
				} else if (outcome.strategy == "merge") {
					ApplyPkMerge(*apply, dst_table, src_table, source_columns, pk_columns);
				} else {
					// snapshot
					auto low = prior->last_source_snapshot.GetValue<int64_t>();
					auto high = high_snapshot.GetValue<int64_t>();
					if (high > low &&
					    ApplySnapshotDelta(*apply, src_table, dst_table, job, ref, low, high, source_columns)) {
						outcome.last_full_sync_snapshot = high_snapshot;
					}
				}

				auto count_res = apply->Query(StringUtil::Format("SELECT COUNT(*) FROM %s", dst_table));
				CheckResult(*count_res, "Failed to count rows in " + dst_table);

				auto commit_res = apply->Query("COMMIT");
				CheckResult(*commit_res, "Failed to commit replication transaction");
				in_transaction = false;

				outcome.row_count = count_res->GetValue(0, 0).GetValue<int64_t>();
				if (outcome.strategy == "watermark") {
					outcome.last_watermark = high_watermark;
				}
				if (source_is_ducklake) {
					outcome.last_source_snapshot = high_snapshot;
				}
				result.rows_copied += NumericCast<uint64_t>(outcome.row_count);
				result.tables_synced++;
				// A job dropped mid-cycle must not collect orphan state rows.
				DuckLakeReplicationJob current;
				if (TryGetJob(context, dest, replication_id, current)) {
					UpsertTableSyncState(context, dest, job, ref, outcome);
				}
			} catch (std::exception &ex) {
				if (IsReplicationAborting(abort)) {
					break;
				}
				if (in_transaction) {
					apply->Query("ROLLBACK");
				}
				failures.push_back(label + " - " + ex.what());
				if (claims_dest) {
					MarkTableError(context, dest, job, ref, ex.what());
				}
			}
		}

		if (source_tables.empty() && dropped == 0) {
			result.message = "no source tables matched include/exclude patterns";
		} else {
			result.message =
			    StringUtil::Format("copied %llu tables / %llu rows", result.tables_synced, result.rows_copied);
			if (dropped > 0) {
				result.message += StringUtil::Format(", dropped %llu stale table(s)", dropped);
			}
		}
		result.tables_failed = failures.size();
		if (IsReplicationAborting(abort)) {
			result.message = "cycle aborted";
			return result;
		}
		if (failures.empty()) {
			// Supervised jobs stay 'running'; manual cycles on unsupervised jobs keep 'created'.
			bool supervised =
			    HasLiveSupervisor(*context.db, dest.GetName().GetIdentifierName()) &&
			    (StringUtil::CIEquals(job.status, "running") || StringUtil::CIEquals(job.status, "error"));
			UpdateJobStatus(context, dest, replication_id, supervised ? "running" : "created", string(), true);
		} else {
			auto detail =
			    StringUtil::Format("%llu table(s) failed: %s", failures.size(), StringUtil::Join(failures, "; "));
			result.message += "; " + detail;
			UpdateJobStatus(context, dest, replication_id, "error", detail, false);
		}
	} catch (std::exception &ex) {
		if (IsReplicationAborting(abort)) {
			result.message = "cycle aborted";
			return result;
		}
		UpdateJobStatus(context, dest, replication_id, "error", ex.what(), false);
		throw;
	}
	return result;
}

//===--------------------------------------------------------------------===//
// Background supervisor
//===--------------------------------------------------------------------===//

//! One supervisor thread per (DatabaseInstance, dest catalog) runs due catchup cycles for
//! every 'running'/'error' job on its own Connection, sleeping on a condition variable.
struct ReplicationSupervisor {
	~ReplicationSupervisor() {
		// A supervisor can outlive its registry entry (self-exit on detach); never leave a
		// joinable thread to ~thread, which calls std::terminate.
		if (thread.joinable()) {
			stop_requested = true;
			cv.notify_all();
			if (thread.get_id() == std::this_thread::get_id()) {
				thread.detach();
			} else {
				thread.join();
			}
		}
	}

	weak_ptr<DatabaseInstance> db;
	string dest_name;
	std::thread thread;
	mutex lock;
	std::condition_variable cv;
	atomic<bool> stop_requested {false};
	atomic<bool> exited {false};
	//! Asks the in-flight cycle to bail between tables when a stop path runs.
	atomic<bool> abort_cycle {false};
	//! Context of the pass currently executing - lets a stop path interrupt in-flight work so a
	//! join does not wait out a full catchup cycle (guarded by 'lock').
	shared_ptr<ClientContext> active_context;
	//! per job: next due time + consecutive failure count (exponential backoff)
	unordered_map<uint64_t, std::pair<std::chrono::steady_clock::time_point, uint32_t>> schedule;
};

//! Clears sup->active_context at scope exit so a pass never leaks a DatabaseInstance reference
//! through the tracked context once it ends.
struct ActiveContextGuard {
	explicit ActiveContextGuard(const shared_ptr<ReplicationSupervisor> &sup_p) : sup(sup_p) {
	}
	~ActiveContextGuard() {
		lock_guard<mutex> guard(sup->lock);
		sup->active_context.reset();
	}
	shared_ptr<ReplicationSupervisor> sup;
};

static mutex supervisor_registry_lock;
static unordered_map<string, shared_ptr<ReplicationSupervisor>> supervisor_registry;

static string SupervisorKey(DatabaseInstance &db, const string &dest_name) {
	return to_string(reinterpret_cast<uintptr_t>(&db)) + "|" + dest_name;
}

static bool HasLiveSupervisor(DatabaseInstance &db, const string &dest_name) {
	lock_guard<mutex> guard(supervisor_registry_lock);
	auto it = supervisor_registry.find(SupervisorKey(db, dest_name));
	return it != supervisor_registry.end() && !it->second->exited.load();
}

static constexpr const uint64_t MIN_CYCLE_INTERVAL_MS = 250;
static constexpr const uint64_t MAX_BACKOFF_MS = 60000;

static bool JobIsRunnable(const DuckLakeReplicationJob &job) {
	return StringUtil::CIEquals(job.status, "running") || StringUtil::CIEquals(job.status, "error");
}

static void SupervisorLoop(const shared_ptr<ReplicationSupervisor> &sup) {
	while (!sup->stop_requested.load()) {
		auto wake = std::chrono::steady_clock::now() + std::chrono::seconds(1);
		try {
			auto db = sup->db.lock();
			if (!db) {
				break;
			}
			if (!sup->stop_requested.load()) {
				ActiveContextGuard ctx_guard(sup);
				// Scoped per pass so the connection does not keep the DatabaseInstance alive between passes.
				auto con = make_uniq<Connection>(*db);
				con->context->registered_state->GetOrCreate<DuckLakeInternalConnectionState>(
				    DuckLakeInternalConnectionState::KEY);
				{
					lock_guard<mutex> guard(sup->lock);
					sup->active_context = con->context;
				}
				// Re-resolve each pass: a DETACH makes the catalog vanish, which is our shutdown signal.
				auto attached = DatabaseManager::Get(*db).GetDatabase(Identifier(sup->dest_name));
				if (!attached) {
					break;
				}
				auto &catalog = attached->GetCatalog();
				if (!StringUtil::CIEquals(catalog.GetCatalogType(), "ducklake")) {
					break;
				}
				auto &dest = catalog.Cast<DuckLakeCatalog>();
				auto jobs = DuckLakeReplication::ListJobs(*con->context, dest);
				auto now = std::chrono::steady_clock::now();
				for (auto &job : jobs) {
					if (sup->stop_requested.load() || sup->abort_cycle.load()) {
						break;
					}
					if (!JobIsRunnable(job)) {
						sup->schedule.erase(job.replication_id);
						continue;
					}
					auto &entry = sup->schedule[job.replication_id];
					if (now < entry.first) {
						continue;
					}
					bool ok = false;
					string cycle_error;
					try {
						auto cycle =
						    DuckLakeReplication::Catchup(*con->context, dest, job.replication_id, &sup->abort_cycle);
						ok = cycle.tables_failed == 0;
					} catch (std::exception &ex) {
						cycle_error = ex.what();
					}
					if (!ok && !cycle_error.empty() && !sup->abort_cycle.load()) {
						try {
							UpdateJobStatus(*con->context, dest, job.replication_id, "error", cycle_error, false);
						} catch (std::exception &) {
						}
					}
					auto interval = MaxValue<uint64_t>(job.interval_ms, MIN_CYCLE_INTERVAL_MS);
					if (ok) {
						entry.second = 0;
						entry.first = std::chrono::steady_clock::now() + std::chrono::milliseconds(interval);
					} else {
						entry.second = MinValue<uint32_t>(entry.second + 1, 8);
						auto backoff = MinValue<uint64_t>(interval << entry.second, MAX_BACKOFF_MS);
						entry.first = std::chrono::steady_clock::now() + std::chrono::milliseconds(backoff);
					}
				}
				// Sleep until the earliest due job (capped at 1s so stop/detach is noticed promptly).
				for (auto &job : jobs) {
					auto it = sup->schedule.find(job.replication_id);
					if (it != sup->schedule.end() && it->second.first < wake) {
						wake = it->second.first;
					}
				}
			}
		} catch (BinderException &) {
			// the destination catalog is gone: clean shutdown
			break;
		} catch (std::exception &) {
		}
		// db/con are released before sleeping so ~DatabaseInstance can run between passes -
		// that path stops and joins this supervisor via ~DuckLakeCatalog -> OnCatalogDetach.
		unique_lock<mutex> lk(sup->lock);
		sup->cv.wait_until(lk, wake, [&] { return sup->stop_requested.load(); });
	}
	sup->exited = true;
}

//! Signals a supervisor to stop, wakes it, and joins its thread. Called with the supervisor
//! already removed from the registry (or with the registry lock held nowhere). Detaches
//! instead of joining when invoked on the supervisor's own thread.
static void StopAndJoinSupervisor(const shared_ptr<ReplicationSupervisor> &sup) {
	sup->abort_cycle = true;
	sup->stop_requested = true;
	{
		lock_guard<mutex> guard(sup->lock);
		if (sup->active_context) {
			sup->active_context->Interrupt();
		}
		sup->cv.notify_all();
	}
	if (!sup->thread.joinable()) {
		return;
	}
	if (sup->thread.get_id() == std::this_thread::get_id()) {
		sup->thread.detach();
		return;
	}
	sup->thread.join();
}

static void StopSupervisorEntry(DatabaseInstance &db, const string &dest_name) {
	shared_ptr<ReplicationSupervisor> sup;
	{
		lock_guard<mutex> guard(supervisor_registry_lock);
		auto it = supervisor_registry.find(SupervisorKey(db, dest_name));
		if (it == supervisor_registry.end()) {
			return;
		}
		sup = it->second;
		supervisor_registry.erase(it);
	}
	StopAndJoinSupervisor(sup);
}

//! Process-exit backstop; normal teardown joins supervisors in DuckDB::~DuckDB before exit().
static void StopAllSupervisors() {
	replication_shutdown = true;
	vector<shared_ptr<ReplicationSupervisor>> all;
	{
		lock_guard<mutex> guard(supervisor_registry_lock);
		for (auto &entry : supervisor_registry) {
			all.push_back(entry.second);
		}
		supervisor_registry.clear();
	}
	for (auto &sup : all) {
		sup->abort_cycle = true;
		sup->stop_requested = true;
	}
	// Interrupt every live query on the supervisor's database - the pass may run on helper
	// connections (e.g. the apply connection) beyond the one tracked as active_context.
	for (auto &sup : all) {
		auto db = sup->db.lock();
		if (!db) {
			continue;
		}
		for (auto &context : ConnectionManager::Get(*db).GetConnectionList()) {
			if (context) {
				context->Interrupt();
			}
		}
		lock_guard<mutex> guard(sup->lock);
		sup->cv.notify_all();
	}
	for (auto &sup : all) {
		if (sup->thread.joinable()) {
			if (sup->thread.get_id() == std::this_thread::get_id()) {
				sup->thread.detach();
			} else {
				sup->thread.join();
			}
		}
	}
}

static void StopDatabaseSupervisors(DatabaseInstance &db) {
	vector<shared_ptr<ReplicationSupervisor>> targets;
	{
		lock_guard<mutex> guard(supervisor_registry_lock);
		for (auto it = supervisor_registry.begin(); it != supervisor_registry.end();) {
			auto owner = it->second->db.lock();
			if (owner && owner.get() == &db) {
				targets.push_back(it->second);
				it = supervisor_registry.erase(it);
			} else {
				++it;
			}
		}
	}
	for (auto &sup : targets) {
		StopAndJoinSupervisor(sup);
	}
}

static void EnsureSupervisor(ClientContext &context, DuckLakeCatalog &dest) {
	static atomic<bool> exit_hook_registered {false};
	if (!exit_hook_registered.exchange(true)) {
		std::atexit(StopAllSupervisors);
	}
	lock_guard<mutex> guard(supervisor_registry_lock);
	context.db->owner_close_callback = StopDatabaseSupervisors;
	auto key = SupervisorKey(*context.db, dest.GetName().GetIdentifierName());
	auto existing = supervisor_registry.find(key);
	if (existing != supervisor_registry.end()) {
		auto &sup = existing->second;
		if (!sup->exited.load()) {
			return;
		}
		supervisor_registry.erase(existing);
		if (sup->thread.joinable()) {
			sup->thread.join();
		}
	}
	auto sup = make_shared_ptr<ReplicationSupervisor>();
	sup->db = weak_ptr<DatabaseInstance>(context.db);
	sup->dest_name = dest.GetName().GetIdentifierName();
	sup->thread = std::thread(SupervisorLoop, sup);
	supervisor_registry[key] = sup;
}

static void NotifySupervisor(DatabaseInstance &db, const string &dest_name) {
	lock_guard<mutex> guard(supervisor_registry_lock);
	auto it = supervisor_registry.find(SupervisorKey(db, dest_name));
	if (it == supervisor_registry.end()) {
		return;
	}
	lock_guard<mutex> lk(it->second->lock);
	it->second->cv.notify_all();
}

vector<DuckLakeReplicationJobAction> DuckLakeReplication::Start(ClientContext &context, DuckLakeCatalog &dest,
                                                                uint64_t job_id) {
	EnsureStateTables(context, dest);
	if (job_id != 0) {
		DuckLakeReplicationJob job;
		if (!TryGetJob(context, dest, job_id, job)) {
			throw InvalidInputException("No replication job with id %llu in catalog \"%s\"", job_id,
			                            dest.GetName().GetIdentifierName());
		}
	}
	auto con = MakeConnection(context, dest.GetDatabase());
	auto table = MetaTableName(dest, "ducklake_replication");
	string update_sql;
	if (job_id == 0) {
		update_sql = StringUtil::Format(
		    "UPDATE %s SET status = 'running' WHERE status IN ('created', 'error', 'stopped')", table);
	} else {
		update_sql = StringUtil::Format("UPDATE %s SET status = 'running' WHERE replication_id = %llu", table, job_id);
	}
	auto res = con->Query(update_sql);
	CheckResult(*res, "Failed to mark replication jobs running");
	EnsureSupervisor(context, dest);
	NotifySupervisor(*context.db, dest.GetName().GetIdentifierName());
	vector<DuckLakeReplicationJobAction> actions;
	for (auto &job : ListJobs(context, dest)) {
		if (!JobIsRunnable(job)) {
			continue;
		}
		if (job_id != 0 && job.replication_id != job_id) {
			continue;
		}
		DuckLakeReplicationJobAction action;
		action.replication_id = job.replication_id;
		action.status = "running";
		action.message = "replication scheduled";
		actions.push_back(action);
	}
	return actions;
}

void DuckLakeReplication::Stop(ClientContext &context, DuckLakeCatalog &dest) {
	StopSupervisorEntry(*context.db, dest.GetName().GetIdentifierName());
	// Mark schedulable jobs 'stopped': an in-flight cycle may have written 'created' after the
	// supervisor entry was already erased, so normalize both.
	EnsureStateTables(context, dest);
	auto con = MakeConnection(context, dest.GetDatabase());
	auto table = MetaTableName(dest, "ducklake_replication");
	auto res = con->Query(
	    StringUtil::Format("UPDATE %s SET status = 'stopped' WHERE status IN ('running', 'created')", table));
	CheckResult(*res, "Failed to mark replication jobs stopped");
}

DuckLakeReplicationJobAction DuckLakeReplication::Pause(ClientContext &context, DuckLakeCatalog &dest,
                                                        uint64_t replication_id) {
	EnsureStateTables(context, dest);
	DuckLakeReplicationJob job;
	if (!TryGetJob(context, dest, replication_id, job)) {
		throw InvalidInputException("No replication job with id %llu in catalog \"%s\"", replication_id,
		                            dest.GetName().GetIdentifierName());
	}
	auto con = MakeConnection(context, dest.GetDatabase());
	auto table = MetaTableName(dest, "ducklake_replication");
	auto res = con->Query(
	    StringUtil::Format("UPDATE %s SET status = 'paused' WHERE replication_id = %llu", table, replication_id));
	CheckResult(*res, "Failed to pause replication job");
	DuckLakeReplicationJobAction action;
	action.replication_id = replication_id;
	action.status = "paused";
	action.message = "replication paused";
	return action;
}

DuckLakeReplicationJobAction DuckLakeReplication::Resume(ClientContext &context, DuckLakeCatalog &dest,
                                                         uint64_t replication_id) {
	EnsureStateTables(context, dest);
	DuckLakeReplicationJob job;
	if (!TryGetJob(context, dest, replication_id, job)) {
		throw InvalidInputException("No replication job with id %llu in catalog \"%s\"", replication_id,
		                            dest.GetName().GetIdentifierName());
	}
	auto con = MakeConnection(context, dest.GetDatabase());
	auto table = MetaTableName(dest, "ducklake_replication");
	auto res = con->Query(
	    StringUtil::Format("UPDATE %s SET status = 'running' WHERE replication_id = %llu", table, replication_id));
	CheckResult(*res, "Failed to resume replication job");
	EnsureSupervisor(context, dest);
	NotifySupervisor(*context.db, dest.GetName().GetIdentifierName());
	DuckLakeReplicationJobAction action;
	action.replication_id = replication_id;
	action.status = "running";
	action.message = "replication resumed";
	return action;
}

vector<DuckLakeReplicationJobAction> DuckLakeReplication::ResumeAll(ClientContext &context, DuckLakeCatalog &dest) {
	EnsureStateTables(context, dest);
	auto con = MakeConnection(context, dest.GetDatabase());
	auto table = MetaTableName(dest, "ducklake_replication");
	auto res = con->Query(
	    StringUtil::Format("UPDATE %s SET status = 'running' WHERE status IN ('paused', 'error', 'stopped')", table));
	CheckResult(*res, "Failed to resume replication jobs");
	EnsureSupervisor(context, dest);
	NotifySupervisor(*context.db, dest.GetName().GetIdentifierName());
	vector<DuckLakeReplicationJobAction> actions;
	for (auto &job : ListJobs(context, dest)) {
		if (!JobIsRunnable(job)) {
			continue;
		}
		DuckLakeReplicationJobAction action;
		action.replication_id = job.replication_id;
		action.status = "running";
		action.message = "replication resumed";
		actions.push_back(action);
	}
	return actions;
}

void DuckLakeReplication::OnCatalogDetach(DatabaseInstance &db, const string &catalog_name) {
	StopSupervisorEntry(db, catalog_name);
}

Value DuckLakeReplication::SnapshotsBehind(ClientContext &context, DuckLakeCatalog &dest,
                                           const DuckLakeReplicationJob &job) {
	try {
		auto &source = GetAttachedCatalog(context, job.source_catalog);
		if (!StringUtil::CIEquals(source.GetCatalogType(), "ducklake")) {
			return Value();
		}
		auto con = MakeConnection(context, source.GetDatabase());
		auto head =
		    ScalarValue(*con, "SELECT MAX(snapshot_id) FROM ducklake_snapshots(" + SQLLit(job.source_catalog) + ")");
		if (head.IsNull()) {
			return Value();
		}
		auto states = ListTableStates(context, dest, job.replication_id);
		int64_t behind = 0;
		bool any = false;
		for (auto &state : states) {
			if (!StringUtil::CIEquals(state.strategy, "snapshot") || state.last_source_snapshot.IsNull()) {
				continue;
			}
			behind += MaxValue<int64_t>(0, head.GetValue<int64_t>() - state.last_source_snapshot.GetValue<int64_t>());
			any = true;
		}
		return any ? Value::BIGINT(behind) : Value();
	} catch (std::exception &) {
		return Value();
	}
}

} // namespace duckdb
