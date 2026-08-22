#include "ducklake_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "storage/ducklake_storage.hpp"
#include "storage/ducklake_scan.hpp"
#include "storage/ducklake_catalog.hpp"
#include "functions/ducklake_table_functions.hpp"
#include "storage/ducklake_secret.hpp"
#include "duckdb/logging/log_manager.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/storage/storage_extension.hpp"
#include "storage/ducklake_log_type.hpp"

namespace duckdb {

ScalarFunction DuckLakeMurmur3Function();
void DuckLakeRegisterMaterializedViewParser(DBConfig &config);

static void LoadInternal(ExtensionLoader &loader) {
	loader.SetDescription("Adds support for DuckLake, SQL as a Lakehouse Format");

	auto &instance = loader.GetDatabaseInstance();
	instance.GetLogManager().RegisterLogType(make_uniq<DuckLakeMetadataLogType>());

	auto &config = DBConfig::GetConfig(instance);
	StorageExtension::Register(config, "ducklake", make_shared_ptr<DuckLakeStorageExtension>());

	// CREATE / REFRESH / DROP MATERIALIZED VIEW sugar (needs allow_parser_override_extension='FALLBACK')
	DuckLakeRegisterMaterializedViewParser(config);

	config.AddExtensionOption("ducklake_max_retry_count",
	                          "The maximum amount of retry attempts for a ducklake transaction", LogicalType::UBIGINT,
	                          Value::UBIGINT(10), nullptr, SetScope::GLOBAL);
	config.AddExtensionOption("ducklake_retry_wait_ms", "Time between retries", LogicalType::UBIGINT,
	                          Value::UBIGINT(100), nullptr, SetScope::GLOBAL);
	config.AddExtensionOption("ducklake_retry_backoff", "Backoff factor for exponentially increasing retry wait time",
	                          LogicalType::DOUBLE, Value::DOUBLE(1.5), nullptr, SetScope::GLOBAL);
	config.AddExtensionOption("ducklake_default_data_inlining_row_limit",
	                          "Default row limit for data inlining (0 disables inlining)", LogicalType::UBIGINT,
	                          Value::UBIGINT(10), nullptr, SetScope::GLOBAL);
	auto set_default_data_file_format = [](ClientContext &, SetScope, Value &parameter) {
		if (parameter.IsNull()) {
			return;
		}
		auto format = StringUtil::Lower(parameter.DefaultCastAs(LogicalType::VARCHAR).GetValue<string>());
		if (format != "parquet" && format != "vortex") {
			throw InvalidInputException(
			    "Unsupported ducklake_default_data_file_format \"%s\"; supported options are parquet, vortex", format);
		}
		parameter = Value(format);
	};
	config.AddExtensionOption("ducklake_default_data_file_format",
	                          "Default managed data-file format for new empty DuckLake tables (parquet or vortex)",
	                          LogicalType::VARCHAR, Value("parquet"), set_default_data_file_format, SetScope::GLOBAL);
	auto set_target_file_size = [](ClientContext &, SetScope, Value &parameter) {
		if (!parameter.IsNull() && !parameter.ToString().empty()) {
			DBConfig::ParseMemoryLimit(parameter.ToString());
		}
	};
	config.AddExtensionOption("ducklake_target_file_size", "Target file size for insertion and compaction",
	                          LogicalType::VARCHAR, Value(), set_target_file_size, SetScope::GLOBAL);
	config.AddExtensionOption(
	    "ducklake_write_deletion_vectors",
	    "[EXPERIMENTAL] Write Iceberg V3 deletion vectors (puffin) instead of positional delete files (parquet)",
	    LogicalType::BOOLEAN, Value::BOOLEAN(false), nullptr, SetScope::GLOBAL);
	auto set_mv_stale_read = [](ClientContext &, SetScope, Value &parameter) {
		if (parameter.IsNull()) {
			return;
		}
		auto mode = StringUtil::Lower(parameter.DefaultCastAs(LogicalType::VARCHAR).GetValue<string>());
		if (mode != "allow" && mode != "warn" && mode != "error") {
			throw InvalidInputException(
			    "Unsupported ducklake_mv_stale_read \"%s\"; supported options are allow, warn, error", mode);
		}
		parameter = Value(mode);
	};
	config.AddExtensionOption("ducklake_mv_stale_read",
	                          "Behavior when querying a stale DuckLake materialized view: allow, warn, or error",
	                          LogicalType::VARCHAR, Value("allow"), set_mv_stale_read, SetScope::GLOBAL);
	config.AddExtensionOption("ducklake_role",
	                          "Active DuckLake RBAC role for this session (empty = PUBLIC grants only)",
	                          LogicalType::VARCHAR, Value(), nullptr, SetScope::LOCAL);
	config.AddExtensionOption("ducklake_admin_role",
	                          "Name of the DuckLake RBAC role that bypasses privilege checks", LogicalType::VARCHAR,
	                          Value("admin"), nullptr, SetScope::GLOBAL);
	auto set_enable_rbac = [](ClientContext &context, SetScope, Value &parameter) {
		bool enable = parameter.IsNull() ? false : BooleanValue::Get(parameter.DefaultCastAs(LogicalType::BOOLEAN));
		auto databases = context.db->GetDatabaseManager().GetDatabases(context);
		for (auto &attached : databases) {
			auto &catalog = attached->GetCatalog();
			if (catalog.GetCatalogType() != "ducklake") {
				continue;
			}
			auto &ducklake_catalog = catalog.Cast<DuckLakeCatalog>();
			// one-way like DuckDB's security settings: once RBAC is enabled
			// for an attached catalog, setting this to false is an error
			ducklake_catalog.Rbac().SetEnabled(enable);
		}
	};
	config.AddExtensionOption("ducklake_enable_rbac",
	                          "Enable role-based access control on all attached DuckLake catalogs (one-way: cannot be "
	                          "disabled without detaching)",
	                          LogicalType::BOOLEAN, Value::BOOLEAN(false), set_enable_rbac, SetScope::GLOBAL);

	DuckLakeSnapshotsFunction snapshots;
	loader.RegisterFunction(snapshots);

	DuckLakeTableInfoFunction table_info;
	loader.RegisterFunction(table_info);

	auto table_insertions = DuckLakeTableInsertionsFunction::GetFunctions();
	loader.RegisterFunction(table_insertions);

	auto table_deletions = DuckLakeTableDeletionsFunction::GetFunctions();
	loader.RegisterFunction(table_deletions);

	auto merge_adjacent_files = DuckLakeMergeAdjacentFilesFunction::GetFunctions();
	loader.RegisterFunction(merge_adjacent_files);

	auto rewrite_files = DuckLakeRewriteDataFilesFunction::GetFunctions();
	loader.RegisterFunction(rewrite_files);

	DuckLakeCleanupOldFilesFunction cleanup_old_files;
	loader.RegisterFunction(cleanup_old_files);

	DuckLakeCleanupOrphanedFilesFunction cleanup_orphaned_files;
	loader.RegisterFunction(cleanup_orphaned_files);

	DuckLakeExpireSnapshotsFunction expire_snapshots;
	loader.RegisterFunction(expire_snapshots);

	DuckLakeFlushInlinedDataFunction flush_inlined_data;
	loader.RegisterFunction(flush_inlined_data);

	DuckLakeSetOptionFunction set_options;
	loader.RegisterFunction(set_options);

	DuckLakeOptionsFunction options;
	loader.RegisterFunction(options);

	DuckLakeSetCommitMessage set_commit_message;
	loader.RegisterFunction(set_commit_message);

	auto table_changes = DuckLakeTableInsertionsFunction::GetDuckLakeTableChanges();
	loader.RegisterFunction(*table_changes);

	DuckLakeListFilesFunction list_files;
	loader.RegisterFunction(list_files);

	auto add_files = DuckLakeAddDataFilesFunction::GetFunctions();
	loader.RegisterFunction(add_files);

	DuckLakeCurrentSnapshotFunction current_snapshot;
	loader.RegisterFunction(current_snapshot);

	DuckLakeLastCommittedSnapshotFunction last_committed;
	loader.RegisterFunction(last_committed);

	DuckLakeSettingsFunction settings;
	loader.RegisterFunction(settings);

	DuckLakeCommitFunction commit;
	loader.RegisterFunction(commit);

	DuckLakeCreateMaterializedViewFunction create_materialized_view;
	loader.RegisterFunction(create_materialized_view);

	DuckLakeRefreshMaterializedViewFunction refresh_materialized_view;
	loader.RegisterFunction(refresh_materialized_view);

	DuckLakeDropMaterializedViewFunction drop_materialized_view;
	loader.RegisterFunction(drop_materialized_view);

	DuckLakeMaterializedViewsFunction materialized_views;
	loader.RegisterFunction(materialized_views);

	// RBAC management
	DuckLakeCreateRoleFunction create_role;
	loader.RegisterFunction(create_role);

	DuckLakeDropRoleFunction drop_role;
	loader.RegisterFunction(drop_role);

	DuckLakeGrantFunction grant_privileges;
	loader.RegisterFunction(grant_privileges);

	DuckLakeRevokeFunction revoke_privileges;
	loader.RegisterFunction(revoke_privileges);

	DuckLakeRolesFunction list_roles;
	loader.RegisterFunction(list_roles);

	DuckLakeGrantsFunction list_grants;
	loader.RegisterFunction(list_grants);

	// Register ducklake_scan so it can be found during deserialization
	auto ducklake_scan = DuckLakeFunctions::GetDuckLakeScanFunction(loader.GetDatabaseInstance());
	loader.RegisterFunction(ducklake_scan);

	// secrets
	auto secret_type = DuckLakeSecret::GetSecretType();
	loader.RegisterSecretType(secret_type);

	auto ducklake_secret_function = DuckLakeSecret::GetFunction();
	loader.RegisterFunction(ducklake_secret_function);

	// Register murmur3_32 scalar function for Iceberg-compatible bucket partitioning
	auto murmur3_func = DuckLakeMurmur3Function();
	loader.RegisterFunction(murmur3_func);
}

void DucklakeExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}
std::string DucklakeExtension::Name() {
	return "ducklake";
}

std::string DucklakeExtension::Version() const {
#ifdef EXT_VERSION_DUCKLAKE
	return EXT_VERSION_DUCKLAKE;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(ducklake, loader) {
	LoadInternal(loader);
}
}
