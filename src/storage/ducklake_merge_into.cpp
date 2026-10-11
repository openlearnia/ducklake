#include "storage/ducklake_catalog.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/planner/operator/logical_merge_into.hpp"

namespace duckdb {

PhysicalOperator &DuckLakeCatalog::PlanMergeInto(ClientContext &context, PhysicalPlanGenerator &planner,
                                                 LogicalMergeInto &op, PhysicalOperator &plan) {
	VerifyNotMaterializedViewBackingTable(context, op.table.Cast<DuckLakeTableEntry>(), "merge into");
	// DuckLake writes a deletion file per data file, so it can apply at most one UPDATE/DELETE to a given row
	idx_t update_delete_count = 0;
	// only the privileges of the actions the statement contains are required
	uint64_t privileges = DUCKLAKE_PRIVILEGE_NONE;
	for (auto &entry : op.actions) {
		for (auto &action : entry.second) {
			switch (action->action_type) {
			case MergeActionType::MERGE_UPDATE:
				privileges |= DUCKLAKE_PRIVILEGE_UPDATE;
				update_delete_count++;
				break;
			case MergeActionType::MERGE_DELETE:
				privileges |= DUCKLAKE_PRIVILEGE_DELETE;
				update_delete_count++;
				break;
			case MergeActionType::MERGE_INSERT:
				privileges |= DUCKLAKE_PRIVILEGE_INSERT;
				break;
			default:
				break;
			}
		}
	}
	if (privileges != DUCKLAKE_PRIVILEGE_NONE) {
		Rbac().CheckTablePrivilege(context, static_cast<DuckLakePrivilege>(privileges),
		                           op.table.Cast<DuckLakeTableEntry>());
	}
	if (update_delete_count > 1) {
		throw NotImplementedException("MERGE INTO with DuckLake only supports a single UPDATE/DELETE action currently");
	}
	return Catalog::PlanMergeInto(context, planner, op, plan);
}

} // namespace duckdb
