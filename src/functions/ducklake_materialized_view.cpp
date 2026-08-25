#include "functions/ducklake_table_functions.hpp"
#include "storage/ducklake_transaction.hpp"
#include "storage/ducklake_catalog.hpp"
#include "storage/ducklake_schema_entry.hpp"
#include "storage/ducklake_table_entry.hpp"
#include "storage/ducklake_insert.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/tableref/joinref.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/parsed_data/bound_create_table_info.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/operator/logical_copy_to_file.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_dummy_scan.hpp"
#include "duckdb/planner/operator/logical_extension_operator.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "common/ducklake_util.hpp"
#include "common/ducklake_types.hpp"
#include <chrono>

namespace duckdb {

static idx_t RefreshClockMillis() {
	return NumericCast<idx_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
	                              std::chrono::steady_clock::now().time_since_epoch())
	                              .count());
}

static bool GetSnapshotTime(DuckLakeTransaction &transaction, DuckLakeSnapshot snapshot, timestamp_tz_t &result) {
	auto query = StringUtil::Format(
	    "SELECT snapshot_time FROM {METADATA_CATALOG}.ducklake_snapshot WHERE snapshot_id=%llu", snapshot.snapshot_id);
	auto rows = transaction.Query(snapshot, query);
	if (rows->HasError()) {
		return false;
	}
	auto &materialized = rows->Cast<MaterializedQueryResult>();
	if (materialized.RowCount() == 0 || materialized.GetValue(0, 0).IsNull()) {
		return false;
	}
	result = materialized.GetValue(0, 0).GetValue<timestamp_tz_t>();
	return true;
}

//===--------------------------------------------------------------------===//
// Helpers
//===--------------------------------------------------------------------===//

static string SQLQuote(const string &input) {
	return KeywordHelper::WriteQuoted(input, '\'');
}
static string SQLQuote(const Identifier &input) {
	return SQLQuote(input.GetIdentifierName());
}

static string SQLIdentifier(const string &input) {
	return KeywordHelper::WriteQuoted(input, '"');
}
static string SQLIdentifier(const Identifier &input) {
	return SQLIdentifier(input.GetIdentifierName());
}

static string MaterializedViewReference(DuckLakeCatalog &catalog, const string &schema_name, const string &view_name) {
	return StringUtil::Format("%s.%s.%s", SQLIdentifier(catalog.GetName()), SQLIdentifier(schema_name),
	                         SQLIdentifier(view_name));
}

static unique_ptr<SelectStatement> ParseSingleSelect(const string &sql, const string &context) {
	Parser parser;
	parser.ParseQuery(sql);
	if (parser.statements.size() != 1 || parser.statements[0]->type != StatementType::SELECT_STATEMENT) {
		throw InvalidInputException("Materialized view %s must be a single SELECT statement: \"%s\"", context, sql);
	}
	return unique_ptr_cast<SQLStatement, SelectStatement>(std::move(parser.statements[0]));
}

//! Substitute the {DUCKLAKE_CATALOG} placeholder with the attached catalog name
static string ResolveMaterializedViewSQL(const string &stored_sql, Catalog &catalog) {
	return DuckLakeUtil::ReplaceSkippingQuotes(stored_sql, "{DUCKLAKE_CATALOG}.", catalog.GetName() + ".");
}

//! Build a logical row diff for grouped materialized views. The GROUP BY output columns are
//! the stable row identity; all remaining output columns participate in changed-row detection.
static string BuildLogicalDiffSQL(const string &old_relation, const string &candidate_sql,
                                  const vector<Identifier> &column_names, const vector<idx_t> &key_positions) {
	if (key_positions.empty()) {
		return string();
	}

	string columns;
	for (idx_t i = 0; i < column_names.size(); i++) {
		if (i > 0) {
			columns += ", ";
		}
		columns += SQLIdentifier(column_names[i]);
	}
	string old_select;
	if (old_relation.empty()) {
		// A newly-created MV has no previous relation in the catalog yet. Reuse the
		// candidate only to obtain the output schema, then force the old side empty.
		old_select = StringUtil::Format("SELECT %s, TRUE AS __present FROM (%s) AS __empty_old WHERE FALSE",
		                              columns, candidate_sql);
	} else {
		old_select = StringUtil::Format("SELECT %s, TRUE AS __present FROM %s", columns, old_relation);
	}
	string new_select = StringUtil::Format("SELECT %s, TRUE AS __present FROM (%s) AS __candidate", columns, candidate_sql);

	string join_condition;
	for (auto key_position : key_positions) {
		if (!join_condition.empty()) {
			join_condition += " AND ";
		}
		join_condition += StringUtil::Format("__old.%s IS NOT DISTINCT FROM __new.%s",
		                                    SQLIdentifier(column_names[key_position]),
		                                    SQLIdentifier(column_names[key_position]));
	}

	string changed_condition;
	for (idx_t i = 0; i < column_names.size(); i++) {
		bool is_key = false;
		for (auto key_position : key_positions) {
			if (key_position == i) {
				is_key = true;
				break;
			}
		}
		if (is_key) {
			continue;
		}
		if (!changed_condition.empty()) {
			changed_condition += " OR ";
		}
		changed_condition += StringUtil::Format("NOT (__old.%s IS NOT DISTINCT FROM __new.%s)",
		                                      SQLIdentifier(column_names[i]), SQLIdentifier(column_names[i]));
	}
	if (changed_condition.empty()) {
		changed_condition = "FALSE";
	}

	return StringUtil::Format(R"(
WITH __old AS (%s),
__new AS (%s)
SELECT count(*) FILTER (WHERE __old.__present IS NULL) AS rows_added,
       count(*) FILTER (WHERE __new.__present IS NULL) AS rows_removed,
       count(*) FILTER (WHERE __old.__present IS NOT NULL AND __new.__present IS NOT NULL
                         AND (%s)) AS rows_changed
FROM __old FULL OUTER JOIN __new ON %s
)",
	                          old_select, new_select, changed_condition, join_condition);
}

//===--------------------------------------------------------------------===//
// Eligibility analysis
//===--------------------------------------------------------------------===//

//! Analysis of a materialized view definition: whether it can be maintained incrementally, plus
//! the structural pieces needed to (re)build the incremental refresh query.
enum class MVAggKind : uint8_t { SUM, COUNT_STAR, COUNT_COL, MIN, MAX, AVG };

struct MVAggregateInfo {
	MVAggKind kind;
	idx_t select_index = 0;
	string child_sql;
};

struct MaterializedViewAnalysis {
	//! whether the definition qualifies for incremental maintenance
	bool eligible = false;
	//! all aggregates are SUM/COUNT(/derivable AVG) — safe for pure delta path
	bool delta_eligible = false;
	//! all aggregate state is delta-maintainable, with MIN/MAX invalidations rebuilt per key
	bool conditional_delta_eligible = false;
	//! why not (when ineligible)
	string reason;
	//! base table location (schema normalized to "main" when unqualified)
	string base_schema;
	string base_table;
	//! base table alias (if the definition used one)
	string base_alias;
	//! whether the base table reference was explicitly catalog-qualified
	bool base_catalog_qualified = false;
	//! the verbatim select list (expressions, comma separated)
	string select_list_sql;
	//! the verbatim WHERE predicate (without the WHERE keyword)
	string where_sql;
	//! the verbatim GROUP BY expression list
	string group_by_sql;
	//! select-list positions of the group keys, in group order
	vector<idx_t> key_positions;
	//! group key expressions (verbatim), in group order
	vector<string> key_expr_sql;
	//! classified aggregates in select-list order
	vector<MVAggregateInfo> aggregates;
	//! single-table FROM clause pieces (empty when join_eligible)
	string from_sql;
	//! join-incremental: INNER equijoin of two base tables (fact + dim)
	bool join_eligible = false;
	string fact_schema;
	string fact_table;
	string fact_alias;
	string dim_schema;
	string dim_table;
	string dim_alias;
	string join_condition_sql;
};

static bool IsSupportedAggregate(const string &function_name) {
	auto lowered = StringUtil::Lower(function_name);
	return lowered == "sum" || lowered == "count" || lowered == "count_star" || lowered == "min" || lowered == "max" ||
	       lowered == "avg";
}

static bool ExpressionReferencesOnlyRelation(const ParsedExpression &expr, const string &alias, const string &table,
                                             bool &saw_column) {
	bool valid = true;
	std::function<void(const ParsedExpression &)> visit = [&](const ParsedExpression &child) {
		if (child.GetExpressionClass() != ExpressionClass::COLUMN_REF) {
			ParsedExpressionIterator::EnumerateChildren(child, visit);
			return;
		}
		saw_column = true;
		auto &column = child.Cast<const ColumnRefExpression>();
		if (!column.IsQualified()) {
			valid = false;
			return;
		}
		auto &column_names = column.ColumnNames();
		auto qualifier = column_names.size() > 1 ? StringUtil::Lower(column_names[column_names.size() - 2].GetIdentifierName()) : string();
		auto expected_alias = StringUtil::Lower(alias);
		auto expected_table = StringUtil::Lower(table);
		if ((!expected_alias.empty() && qualifier == expected_alias) || qualifier == expected_table) {
			return;
		}
		valid = false;
	};
	visit(expr);
	return valid;
}

//! Recursively verify that a select-list expression either is a supported non-distinct aggregate
//! call (without nested aggregates) or contains no aggregates at all.
static bool SelectItemIsSupported(const ParsedExpression &expr, bool &is_aggregate) {
	if (expr.GetExpressionClass() == ExpressionClass::FUNCTION) {
		auto &function = expr.Cast<const FunctionExpression>();
		if (IsSupportedAggregate(function.FunctionName().GetIdentifierName())) {
			if (function.Distinct()) {
				return false;
			}
			for (auto &argument : function.GetArguments()) {
				auto &child = argument.GetExpression();
				if (child.GetExpressionClass() == ExpressionClass::FUNCTION &&
				    IsSupportedAggregate(child.Cast<const FunctionExpression>().FunctionName().GetIdentifierName())) {
					// nested aggregate
					return false;
				}
				if (child.HasSubquery()) {
					return false;
				}
			}
			is_aggregate = true;
			return true;
		}
		// scalar function - check children for unsupported aggregates
		for (auto &argument : function.GetArguments()) {
			auto &child = argument.GetExpression();
			bool child_aggregate = false;
			if (!SelectItemIsSupported(child, child_aggregate) || child_aggregate) {
				return false;
			}
		}
		return true;
	}
	if (expr.GetExpressionClass() == ExpressionClass::STAR) {
		return false;
	}
	if (expr.HasSubquery() || expr.IsWindow()) {
		return false;
	}
	bool supported = true;
	std::function<void(const ParsedExpression &)> callback = [&](const ParsedExpression &child) {
		bool child_aggregate = false;
		if (!SelectItemIsSupported(child, child_aggregate) || child_aggregate) {
			supported = false;
		}
	};
	ParsedExpressionIterator::EnumerateChildren(expr, callback);
	return supported;
}

static MaterializedViewAnalysis AnalyzeMaterializedView(const SelectStatement &statement) {
	MaterializedViewAnalysis result;
	auto node_type = statement.node->type;
	if (node_type == QueryNodeType::SET_OPERATION_NODE) {
		result.reason = "definition uses a set operation (UNION/INTERSECT/EXCEPT)";
		return result;
	}
	if (node_type != QueryNodeType::SELECT_NODE) {
		result.reason = "definition is not a plain SELECT";
		return result;
	}
	if (!statement.node->cte_map.map.empty()) {
		result.reason = "definition uses CTEs";
		return result;
	}
	auto &node = statement.node->Cast<const SelectNode>();
	const BaseTableRef *single_base = nullptr;
	const BaseTableRef *fact_ref = nullptr;
	const BaseTableRef *dim_ref = nullptr;
	string join_condition_sql;
	if (node.from_table && node.from_table->type == TableReferenceType::BASE_TABLE) {
		single_base = &node.from_table->Cast<const BaseTableRef>();
	} else if (node.from_table && node.from_table->type == TableReferenceType::JOIN) {
		auto &join = node.from_table->Cast<const JoinRef>();
		if (join.type != JoinType::INNER || !join.condition || !join.using_columns.empty() ||
		    join.ref_type == JoinRefType::NATURAL) {
			result.reason = "definition join must be a single INNER equijoin";
			return result;
		}
		if (!join.left || join.left->type != TableReferenceType::BASE_TABLE || !join.right ||
		    join.right->type != TableReferenceType::BASE_TABLE) {
			result.reason = "definition join must be between two base tables";
			return result;
		}
		// require a single equality comparison
		if (join.condition->GetExpressionClass() != ExpressionClass::COMPARISON) {
			result.reason = "definition join condition must be a single equality";
			return result;
		}
		auto &cmp = join.condition->Cast<const ComparisonExpression>();
		if (cmp.GetExpressionType() != ExpressionType::COMPARE_EQUAL) {
			result.reason = "definition join condition must be a single equality";
			return result;
		}
		join_condition_sql = join.condition->ToString();
		fact_ref = &join.left->Cast<const BaseTableRef>();
		dim_ref = &join.right->Cast<const BaseTableRef>();
		bool left_fact_column = false;
		bool right_dim_column = false;
		bool left_dim_column = false;
		bool right_fact_column = false;
		auto left_is_fact = ExpressionReferencesOnlyRelation(cmp.Left(), fact_ref->alias.GetIdentifierName(),
		                                                 fact_ref->Table().GetIdentifierName(),
		                                                  left_fact_column);
		auto right_is_dim = ExpressionReferencesOnlyRelation(cmp.Right(), dim_ref->alias.GetIdentifierName(),
		                                                   dim_ref->Table().GetIdentifierName(),
		                                                   right_dim_column);
		auto left_is_dim = ExpressionReferencesOnlyRelation(cmp.Left(), dim_ref->alias.GetIdentifierName(),
		                                                 dim_ref->Table().GetIdentifierName(),
		                                                 left_dim_column);
		auto right_is_fact = ExpressionReferencesOnlyRelation(cmp.Right(), fact_ref->alias.GetIdentifierName(),
		                                                  fact_ref->Table().GetIdentifierName(),
		                                                   right_fact_column);
		if (!((left_is_fact && left_fact_column && right_is_dim && right_dim_column) ||
		      (left_is_dim && left_dim_column && right_is_fact && right_fact_column))) {
			result.reason = "definition join equality must connect the fact and dimension relations";
			return result;
		}
		if ((!fact_ref->GetQualifiedName().Catalog().empty() || !dim_ref->GetQualifiedName().Catalog().empty()) &&
		    StringUtil::Lower(fact_ref->GetQualifiedName().Catalog().GetIdentifierName()) !=
		        StringUtil::Lower(dim_ref->GetQualifiedName().Catalog().GetIdentifierName())) {
			result.reason = "definition join relations must belong to the same catalog";
			return result;
		}
		result.join_eligible = true;
	} else {
		result.reason = "definition must read from a single base table or one INNER equijoin";
		return result;
	}
	auto &base = single_base ? *single_base : *fact_ref;
	// ORDER BY does not change the materialized content - allow it; other modifiers do
	for (auto &modifier : node.modifiers) {
		if (modifier->type != ResultModifierType::ORDER_MODIFIER) {
			result.reason = "definition uses DISTINCT/LIMIT or similar modifiers";
			return result;
		}
	}
	if (node.having) {
		result.reason = "definition uses HAVING";
		return result;
	}
	if (node.qualify) {
		result.reason = "definition uses QUALIFY";
		return result;
	}
	if (node.sample) {
		result.reason = "definition uses SAMPLE";
		return result;
	}
	auto &groups = node.groups.group_expressions;
	if (groups.empty()) {
		result.reason = "definition has no GROUP BY";
		return result;
	}
	if (!node.groups.grouping_sets.empty()) {
		if (node.groups.grouping_sets.size() > 1 ||
		    node.groups.grouping_sets[0].size() != node.groups.group_expressions.size()) {
			result.reason = "definition uses GROUPING SETS / ROLLUP / CUBE";
			return result;
		}
	}
	if (node.where_clause && node.where_clause->HasSubquery()) {
		result.reason = "WHERE clause contains a subquery";
		return result;
	}

	// select list: each item must be a group key (verbatim) or a supported aggregate
	bool any_aggregate = false;
	bool only_delta_aggs = true;
	bool conditional_state_safe = true;
	bool has_minmax = false;
	bool has_count_star = false;
	for (idx_t select_idx = 0; select_idx < node.select_list.size(); select_idx++) {
		auto &item = node.select_list[select_idx];
		bool is_aggregate = false;
		if (!SelectItemIsSupported(*item, is_aggregate)) {
			result.reason = "select list contains an unsupported expression";
			return result;
		}
		any_aggregate = any_aggregate || is_aggregate;
		if (!is_aggregate) {
			continue;
		}
		const FunctionExpression *func = nullptr;
		if (item->GetExpressionClass() == ExpressionClass::FUNCTION &&
		    IsSupportedAggregate(item->Cast<const FunctionExpression>().FunctionName().GetIdentifierName())) {
			func = &item->Cast<const FunctionExpression>();
		} else {
			ParsedExpressionIterator::EnumerateChildren(*item, [&](const ParsedExpression &child) {
				if (!func && child.GetExpressionClass() == ExpressionClass::FUNCTION &&
				    IsSupportedAggregate(child.Cast<const FunctionExpression>().FunctionName().GetIdentifierName())) {
					func = &child.Cast<const FunctionExpression>();
				}
			});
		}
		if (!func) {
			only_delta_aggs = false;
			conditional_state_safe = false;
			continue;
		}
		if (result.join_eligible) {
			for (auto &argument : func->GetArguments()) {
				auto &child = argument.GetExpression();
				bool saw_column = false;
				if (!ExpressionReferencesOnlyRelation(child, fact_ref->alias.GetIdentifierName(),
				                                    fact_ref->Table().GetIdentifierName(), saw_column) ||
				    !saw_column) {
					result.reason = "join-incremental aggregates must reference only fact-side columns";
					return result;
				}
			}
		}
		// The delta expressions below model plain aggregate arguments only. FILTER,
		// aggregate ORDER BY and exported aggregate state retain incremental support
		// through changed-key recomputation, but are not safe for algebraic deltas.
		if (func->Filter() || (func->OrderBy() && !func->OrderBy()->orders.empty()) || func->ExportState()) {
			only_delta_aggs = false;
			conditional_state_safe = false;
		}
		auto fname = StringUtil::Lower(func->FunctionName().GetIdentifierName());
		MVAggregateInfo info;
		info.select_index = select_idx;
		if (fname == "sum") {
			info.kind = MVAggKind::SUM;
			info.child_sql = func->GetArguments().empty() ? string() : func->GetArguments()[0].GetExpression().ToString();
		} else if (fname == "count" || fname == "count_star") {
			if (fname == "count_star" || func->GetArguments().empty() ||
			    func->GetArguments()[0].GetExpression().GetExpressionClass() == ExpressionClass::STAR) {
				info.kind = MVAggKind::COUNT_STAR;
				has_count_star = true;
			} else {
				info.kind = MVAggKind::COUNT_COL;
				info.child_sql = func->GetArguments()[0].GetExpression().ToString();
			}
		} else if (fname == "min") {
			info.kind = MVAggKind::MIN;
			only_delta_aggs = false;
			has_minmax = true;
			info.child_sql = func->GetArguments().empty() ? string() : func->GetArguments()[0].GetExpression().ToString();
		} else if (fname == "max") {
			info.kind = MVAggKind::MAX;
			only_delta_aggs = false;
			has_minmax = true;
			info.child_sql = func->GetArguments().empty() ? string() : func->GetArguments()[0].GetExpression().ToString();
		} else if (fname == "avg") {
			info.kind = MVAggKind::AVG;
			info.child_sql = func->GetArguments().empty() ? string() : func->GetArguments()[0].GetExpression().ToString();
		} else {
			only_delta_aggs = false;
			continue;
		}
		result.aggregates.push_back(std::move(info));
	}
	if (!any_aggregate) {
		result.reason = "select list contains no aggregate functions";
		return result;
	}

	// map group keys onto select list positions
	for (auto &group : groups) {
		if (group->GetExpressionClass() == ExpressionClass::CONSTANT) {
			auto &constant = group->Cast<const ConstantExpression>();
			if (constant.GetValue().type().IsIntegral()) {
				auto select_index = constant.GetValue().GetValue<int64_t>();
				if (select_index >= 1 && NumericCast<idx_t>(select_index) <= node.select_list.size()) {
					result.key_positions.push_back(NumericCast<idx_t>(select_index - 1));
					result.key_expr_sql.push_back(node.select_list[NumericCast<idx_t>(select_index - 1)]->ToString());
					continue;
				}
			}
		}
		string group_sql = group->ToString();
		bool found = false;
		for (idx_t i = 0; i < node.select_list.size(); i++) {
			if (node.select_list[i]->ToString() == group_sql) {
				result.key_positions.push_back(i);
				found = true;
				break;
			}
		}
		if (!found) {
			result.reason = "every GROUP BY key must appear in the select list";
			return result;
		}
		result.key_expr_sql.push_back(std::move(group_sql));
	}

	result.base_schema = base.GetQualifiedName().Schema().empty() ? "main" : base.GetQualifiedName().Schema().GetIdentifierName();
	result.base_table = base.Table().GetIdentifierName();
	result.base_alias = base.alias.GetIdentifierName();
	result.base_catalog_qualified = !base.GetQualifiedName().Catalog().empty();
	if (result.join_eligible) {
		result.fact_schema = fact_ref->GetQualifiedName().Schema().empty() ? "main" : fact_ref->GetQualifiedName().Schema().GetIdentifierName();
		result.fact_table = fact_ref->Table().GetIdentifierName();
		result.fact_alias = fact_ref->alias.GetIdentifierName();
		result.dim_schema = dim_ref->GetQualifiedName().Schema().empty() ? "main" : dim_ref->GetQualifiedName().Schema().GetIdentifierName();
		result.dim_table = dim_ref->Table().GetIdentifierName();
		result.dim_alias = dim_ref->alias.GetIdentifierName();
		result.join_condition_sql = std::move(join_condition_sql);
		string fact_alias_sql = result.fact_alias.empty() ? "" : " AS " + SQLIdentifier(result.fact_alias);
		string dim_alias_sql = result.dim_alias.empty() ? "" : " AS " + SQLIdentifier(result.dim_alias);
		result.from_sql = StringUtil::Format("%s.%s%s JOIN %s.%s%s ON %s", SQLIdentifier(result.fact_schema),
		                                     SQLIdentifier(result.fact_table), fact_alias_sql,
		                                     SQLIdentifier(result.dim_schema), SQLIdentifier(result.dim_table),
		                                     dim_alias_sql, result.join_condition_sql);
	}
	for (auto &item : node.select_list) {
		if (!result.select_list_sql.empty()) {
			result.select_list_sql += ", ";
		}
		result.select_list_sql += item->ToString();
	}
	if (node.where_clause) {
		result.where_sql = node.where_clause->ToString();
	}
	for (auto &group : groups) {
		if (!result.group_by_sql.empty()) {
			result.group_by_sql += ", ";
		}
		result.group_by_sql += group->ToString();
	}
	result.eligible = true;
	bool has_derived_state = true;
	for (auto &aggregate : result.aggregates) {
		if (aggregate.kind != MVAggKind::SUM && aggregate.kind != MVAggKind::AVG) {
			continue;
		}
		bool has_matching_count = false;
		bool has_matching_sum = aggregate.kind == MVAggKind::SUM;
		for (auto &candidate : result.aggregates) {
			if (candidate.kind == MVAggKind::COUNT_COL && candidate.child_sql == aggregate.child_sql) {
				has_matching_count = true;
			}
			if (candidate.kind == MVAggKind::SUM && candidate.child_sql == aggregate.child_sql) {
				has_matching_sum = true;
			}
		}
		if (!has_matching_count || !has_matching_sum) {
			has_derived_state = false;
			break;
		}
	}
	// Joins use join_incremental, not pure delta
	// COUNT(*) tracks group liveness. Each SUM also needs COUNT(its argument) so
	// pure delta maintenance can distinguish a real zero from SQL's all-NULL result.
	result.delta_eligible =
	    !result.join_eligible && only_delta_aggs && has_count_star && has_derived_state && !result.aggregates.empty();
	result.conditional_delta_eligible = !result.join_eligible && has_minmax && conditional_state_safe &&
	                                    has_count_star && has_derived_state && !result.aggregates.empty();
	return result;
}

//===--------------------------------------------------------------------===//
// Materialized View Refresh Operator
//===--------------------------------------------------------------------===//

class DuckLakeMVRefreshSourceState : public GlobalSourceState {
public:
	DuckLakeMVRefreshSourceState() : returned_result(false) {
	}
	bool returned_result;
};

//! Terminal operator of a materialized view create/refresh: collects the files written by the
//! copy operator, retires the previous backing files (refresh only) and appends the new files -
//! all staged on the transaction so the swap is atomic in the resulting snapshot.
class DuckLakeMVRefresh : public PhysicalOperator {
public:
	DuckLakeMVRefresh(PhysicalPlan &physical_plan, const vector<LogicalType> &types, DuckLakeTableEntry &table_p,
	                  TableIndex mv_view_id_p, string encryption_key_p, optional_idx partition_id_p,
	                  string refresh_mode_p, string logical_diff_sql_p, PhysicalOperator &child)
	    : PhysicalOperator(physical_plan, PhysicalOperatorType::EXTENSION, types, 0), table(table_p),
	      mv_view_id(mv_view_id_p), encryption_key(std::move(encryption_key_p)), partition_id(partition_id_p),
	      refresh_mode(std::move(refresh_mode_p)), logical_diff_sql(std::move(logical_diff_sql_p)),
	      refresh_start_ms(RefreshClockMillis()) {
		children.push_back(child);
	}

	DuckLakeTableEntry &table;
	TableIndex mv_view_id;
	string encryption_key;
	optional_idx partition_id;
	string refresh_mode;
	string logical_diff_sql;
	idx_t refresh_start_ms;

public:
	unique_ptr<GlobalSourceState> GetGlobalSourceState(ClientContext &context) const override {
		return make_uniq<DuckLakeMVRefreshSourceState>();
	}

	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk,
	                                 OperatorSourceInput &input) const override {
		auto &source_state = input.global_state.Cast<DuckLakeMVRefreshSourceState>();
		if (source_state.returned_result) {
			return SourceResultType::FINISHED;
		}
		source_state.returned_result = true;
		auto &gstate = sink_state->Cast<DuckLakeInsertGlobalState>();
		chunk.SetCardinality(1);
		chunk.SetValue(0, 0, Value(table.schema.name));
		chunk.SetValue(1, 0, Value(table.name));
		chunk.SetValue(2, 0, Value::BIGINT(NumericCast<int64_t>(gstate.rows_flushed)));
		return SourceResultType::FINISHED;
	}

	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override {
		return make_uniq<DuckLakeInsertGlobalState>(table);
	}

	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override {
		auto &global_state = input.global_state.Cast<DuckLakeInsertGlobalState>();
		DuckLakeInsert::AddWrittenFiles(global_state, chunk, encryption_key, partition_id, false);
		return SinkResultType::NEED_MORE_INPUT;
	}

	SinkFinalizeType Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
	                          OperatorSinkFinalizeInput &input) const override {
		auto &global_state = input.global_state.Cast<DuckLakeInsertGlobalState>();
		auto &transaction = DuckLakeTransaction::Get(context, table.catalog);
		auto snapshot = transaction.GetSnapshot();
		auto table_id = table.GetTableId();

		// retire files written to the backing table earlier in this transaction
		if (transaction.HasTransactionLocalInserts(table_id)) {
			for (auto &local_file : transaction.GetTransactionLocalFiles(table_id)) {
				transaction.DropTransactionLocalFile(table_id, local_file.file_name);
			}
		}
		if (!table_id.IsTransactionLocal()) {
			// end-snapshot the currently live files of the backing table
			string live_files_query = StringUtil::Format(R"(
SELECT data_file_id, path, path_is_relative
FROM {METADATA_CATALOG}.ducklake_data_file
WHERE table_id=%d AND {SNAPSHOT_ID} >= begin_snapshot
  AND ({SNAPSHOT_ID} < end_snapshot OR end_snapshot IS NULL)
)",
			                                              table_id.index);
			auto result = transaction.Query(snapshot, live_files_query);
			if (result->HasError()) {
				result->GetErrorObject().Throw("Failed to query materialized view backing files for refresh: ");
			}
			for (auto &row : *result) {
				auto file_id = DataFileIndex(row.GetValue<uint64_t>(0));
				auto path = row.GetValue<string>(1);
				auto path_is_relative = row.GetValue<bool>(2);
				if (path_is_relative) {
					path = table.DataPath() + path;
				}
				transaction.DropFile(table_id, file_id, std::move(path));
			}
		}

		// Compute logical row diffs before retiring the old backing files. Physical file
		// replacement is not equivalent to logical row replacement.
		for (auto &file : global_state.written_files) {
			global_state.rows_flushed += file.row_count;
		}
		DuckLakeMaterializedViewRefreshInfo refresh;
		refresh.view_id = mv_view_id;
		refresh.refresh_mode = refresh_mode;
		refresh.rows_refreshed = global_state.rows_flushed;
		refresh.refresh_duration_ms = RefreshClockMillis() - refresh_start_ms;
		if (!logical_diff_sql.empty()) {
			auto diff_result = transaction.Query(snapshot, logical_diff_sql);
			if (diff_result->HasError()) {
				diff_result->GetErrorObject().Throw("Failed to compute materialized view logical refresh diff: ");
			}
			auto &diff = diff_result->Cast<MaterializedQueryResult>();
			if (diff.RowCount() != 1 || diff.ColumnCount() != 3) {
				throw InternalException("Materialized view logical refresh diff returned an unexpected shape");
			}
			refresh.rows_added = NumericCast<idx_t>(diff.GetValue(0, 0).GetValue<int64_t>());
			refresh.rows_removed = NumericCast<idx_t>(diff.GetValue(1, 0).GetValue<int64_t>());
			refresh.rows_changed = NumericCast<idx_t>(diff.GetValue(2, 0).GetValue<int64_t>());
		}
		transaction.AppendFiles(table_id, std::move(global_state.written_files));
		refresh.source_snapshot = snapshot.snapshot_id;
		timestamp_tz_t source_time;
		if (GetSnapshotTime(transaction, snapshot, source_time)) {
			refresh.source_snapshot_time = source_time;
			refresh.has_source_snapshot_time = true;
			auto now = Timestamp::GetCurrentTimestamp();
			if (now.value >= source_time.value) {
				refresh.lag_ms = NumericCast<idx_t>((now.value - source_time.value) / 1000);
			}
		}
		// Stamp last_refreshed_snapshot and history at commit.
		transaction.RefreshMaterializedView(std::move(refresh));
		return SinkFinalizeType::READY;
	}

	string GetName() const override {
		return "DUCKLAKE_MV_REFRESH";
	}

	bool IsSource() const override {
		return true;
	}

	bool IsSink() const override {
		return true;
	}
};

class DuckLakeLogicalMVRefresh : public LogicalExtensionOperator {
public:
	DuckLakeLogicalMVRefresh(TableIndex table_index_p, DuckLakeTableEntry &table_p, TableIndex mv_view_id_p,
	                         string encryption_key_p, optional_idx partition_id_p, string refresh_mode_p,
	                         string logical_diff_sql_p)
	    : table_index(table_index_p), table(table_p), mv_view_id(mv_view_id_p),
	      encryption_key(std::move(encryption_key_p)), partition_id(partition_id_p),
	      refresh_mode(std::move(refresh_mode_p)), logical_diff_sql(std::move(logical_diff_sql_p)) {
	}

	TableIndex table_index;
	DuckLakeTableEntry &table;
	TableIndex mv_view_id;
	string encryption_key;
	optional_idx partition_id;
	string refresh_mode;
	string logical_diff_sql;

public:
	PhysicalOperator &CreatePlan(ClientContext &context, PhysicalPlanGenerator &planner) override {
		auto &child = planner.CreatePlan(*children[0]);
		return planner.Make<DuckLakeMVRefresh>(types, table, mv_view_id, std::move(encryption_key), partition_id,
	                                       std::move(refresh_mode), std::move(logical_diff_sql), child);
	}

	string GetName() const override {
		return "DUCKLAKE_MV_REFRESH";
	}

	string GetExtensionName() const override {
		return "ducklake";
	}

	vector<ColumnBinding> GetColumnBindings() override {
		vector<ColumnBinding> result;
		for (idx_t i = 0; i < 3; i++) {
			result.emplace_back(table_index, ProjectionIndex(i));
		}
		return result;
	}

	void ResolveTypes() override {
		types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::BIGINT};
	}
};

//===--------------------------------------------------------------------===//
// Plan builder
//===--------------------------------------------------------------------===//

//! Wrap a bound plan producing the materialized view content with the write pipeline:
//! plan -> (casts) -> copy-to-files -> MV refresh operator -> projection.
//! The projection emits (schema_name, view_name, refresh_mode, rows_refreshed).
static unique_ptr<LogicalOperator> BuildMVWritePlan(ClientContext &context, Binder &binder, TableIndex bind_index,
                                                    unique_ptr<LogicalOperator> plan, DuckLakeTableEntry &table,
                                                    TableIndex mv_view_id, const string &schema_name,
                                                    const string &view_name, const string &refresh_mode,
                                                    const string &logical_diff_sql, vector<Identifier> &return_names) {
	plan->ResolveOperatorTypes();
	if (DuckLakeTypes::RequiresCast(plan->types)) {
		plan = DuckLakeInsert::InsertCasts(binder, plan);
	}

	DuckLakeCopyInput copy_input(context, table);
	auto copy_options = DuckLakeInsert::GetCopyOptions(context, copy_input);

	if (!copy_options.projection_list.empty()) {
		auto proj = make_uniq<LogicalProjection>(binder.GenerateTableIndex(), std::move(copy_options.projection_list));
		proj->children.push_back(std::move(plan));
		plan = std::move(proj);
	}

	auto copy = make_uniq<LogicalCopyToFile>(std::move(copy_options.copy_function), std::move(copy_options.bind_data),
	                                         std::move(copy_options.info), binder.GenerateTableIndex());
	copy->file_path = std::move(copy_options.file_path);
	copy->use_tmp_file = copy_options.use_tmp_file;
	copy->filename_pattern = std::move(copy_options.filename_pattern);
	copy->file_extension = std::move(copy_options.file_extension);
	copy->overwrite_mode = copy_options.overwrite_mode;
	copy->per_thread_output = copy_options.per_thread_output;
	copy->file_size_bytes = copy_options.file_size_bytes;
	copy->rotate = copy_options.rotate;
	copy->return_type = copy_options.return_type;
	copy->partition_output = copy_options.partition_output;
	copy->write_partition_columns = copy_options.write_partition_columns;
	copy->write_empty_file = copy_options.write_empty_file;
	copy->partition_columns = std::move(copy_options.partition_columns);
	for (auto &name : copy_options.names) {
		copy->names.emplace_back(name);
	}
	copy->expected_types = std::move(copy_options.expected_types);
	copy->hive_file_pattern = copy_input.catalog.UseHiveFilePattern(copy_input.encryption_key.empty(),
	                                                                copy_input.schema_id, copy_input.table_id);
	copy->children.push_back(std::move(plan));

	TableIndex mv_index = binder.GenerateTableIndex();
	auto mv_op = make_uniq<DuckLakeLogicalMVRefresh>(mv_index, table, mv_view_id, std::move(copy_input.encryption_key),
	                                                 optional_idx(), refresh_mode, logical_diff_sql);
	mv_op->children.push_back(std::move(copy));
	mv_op->ResolveOperatorTypes();

	// project (schema, name, mode constant, rows) — user-facing MV names, not backing table
	vector<unique_ptr<Expression>> projections;
	auto mv_bindings = mv_op->GetColumnBindings();
	projections.push_back(make_uniq<BoundConstantExpression>(Value(schema_name)));
	projections.push_back(make_uniq<BoundConstantExpression>(Value(view_name)));
	projections.push_back(make_uniq<BoundConstantExpression>(Value(refresh_mode)));
	projections.push_back(make_uniq<BoundColumnRefExpression>(LogicalType::BIGINT, mv_bindings[2]));
	auto projection = make_uniq<LogicalProjection>(bind_index, std::move(projections));
	projection->children.push_back(std::move(mv_op));
	projection->ResolveOperatorTypes();

	return_names = {Identifier("schema_name"), Identifier("view_name"), Identifier("refresh_mode"), Identifier("rows_refreshed")};
	return std::move(projection);
}

//! Result row for statements that do not execute a plan (skip / drop).
static unique_ptr<LogicalOperator> BuildConstantResult(TableIndex bind_index, const string &schema_name,
                                                       const string &view_name, const string &mode, idx_t rows,
                                                       vector<Identifier> &return_names) {
	auto dummy = make_uniq<LogicalDummyScan>(bind_index);
	vector<ColumnBinding> bindings;
	bindings.emplace_back(bind_index, ProjectionIndex(0));
	vector<unique_ptr<Expression>> projections;
	projections.push_back(make_uniq<BoundConstantExpression>(Value(schema_name)));
	projections.push_back(make_uniq<BoundConstantExpression>(Value(view_name)));
	projections.push_back(make_uniq<BoundConstantExpression>(Value(mode)));
	projections.push_back(make_uniq<BoundConstantExpression>(Value::BIGINT(NumericCast<int64_t>(rows))));
	auto projection = make_uniq<LogicalProjection>(bind_index, std::move(projections));
	projection->children.push_back(std::move(dummy));
	projection->ResolveOperatorTypes();
	return_names = {Identifier("schema_name"), Identifier("view_name"), Identifier("refresh_mode"), Identifier("rows_refreshed")};
	return std::move(projection);
}

static unique_ptr<LogicalOperator> BindDefinitionPlan(Binder &parent_binder, ClientContext &context,
                                                      const string &sql, const string &error_context) {
	auto statement = ParseSingleSelect(sql, error_context);
	auto binder = Binder::CreateBinder(context, &parent_binder);
	auto &sql_statement = static_cast<SQLStatement &>(*statement);
	return binder->Bind(sql_statement).plan;
}

//===--------------------------------------------------------------------===//
// Create
//===--------------------------------------------------------------------===//

static void CollectBaseTableRefs(QueryNode &node, vector<reference<BaseTableRef>> &out);

static void CollectBaseTableRefs(ParsedExpression &expression, vector<reference<BaseTableRef>> &out) {
	if (expression.GetExpressionClass() == ExpressionClass::SUBQUERY) {
		auto &subquery = expression.Cast<SubqueryExpression>();
				if (subquery.Subquery() && subquery.Subquery()->node) {
					CollectBaseTableRefs(*subquery.Subquery()->node, out);
		}
	}
	ParsedExpressionIterator::EnumerateChildren(expression,
	                                            [&](ParsedExpression &child) { CollectBaseTableRefs(child, out); });
}

static void CollectBaseTableRefs(QueryNode &node, vector<reference<BaseTableRef>> &out) {
	ParsedExpressionIterator::EnumerateQueryNodeChildren(
	    node, [&](unique_ptr<ParsedExpression> &expression) { CollectBaseTableRefs(*expression, out); },
	    [&](TableRef &ref) {
		    if (ref.type == TableReferenceType::BASE_TABLE) {
			    out.push_back(ref.Cast<BaseTableRef>());
		    }
	    });
}

//! Qualify base table references that live inside the lake with the lake catalog so the statement
//! binds correctly regardless of the caller's search path. Also collects the dependency table ids.
static void QualifyBaseRefsInLake(ClientContext &context, DuckLakeCatalog &ducklake_catalog,
                                  SelectStatement &statement, vector<TableIndex> &dependencies) {
	vector<reference<BaseTableRef>> base_refs;
	if (statement.node) {
		CollectBaseTableRefs(*statement.node, base_refs);
	}
	auto &lake_name = ducklake_catalog.GetName();
	for (auto &base_ref : base_refs) {
		auto &ref = base_ref.get();
		auto qualified_name = ref.GetQualifiedName();
		if (!qualified_name.Catalog().empty() &&
		    !StringUtil::CIEquals(qualified_name.Catalog().GetIdentifierName(), lake_name.GetIdentifierName())) {
			// references an object outside this lake - cannot be tracked as a dependency
			continue;
		}
		auto base_schema = qualified_name.Schema().empty() ? "main" : qualified_name.Schema().GetIdentifierName();
		EntryLookupInfo lookup(CatalogType::TABLE_ENTRY, QualifiedName(qualified_name.Name()));
		auto dep_entry = ducklake_catalog.GetEntry(context, Identifier(base_schema), lookup, OnEntryNotFound::RETURN_NULL);
		if (!dep_entry) {
			// 2-part names parse as schema.table - `<lake>.<table>` lands here with the catalog
			// name in the schema position. Search the lake's schemas for the table instead.
			for (auto &schema_entry : ducklake_catalog.GetSchemas(context)) {
				auto entry = ducklake_catalog.GetEntry(context, schema_entry.get().name, lookup,
			                                               OnEntryNotFound::RETURN_NULL);
				if (entry) {
					base_schema = schema_entry.get().name.GetIdentifierName();
					dep_entry = entry;
					break;
				}
			}
		}
		if (!dep_entry) {
			// not found in the lake - leave the reference alone, binding will resolve or report it
			continue;
		}
		// qualify so the (re)bind resolves inside the lake regardless of search path
		ref.SetQualifiedName(QualifiedName(Identifier(lake_name), Identifier(base_schema), qualified_name.Name()));
		if (dep_entry->type == CatalogType::TABLE_ENTRY) {
			auto table_id = dep_entry->Cast<DuckLakeTableEntry>().GetTableId();
			bool already_tracked = false;
			for (auto &dependency : dependencies) {
				if (dependency == table_id) {
					already_tracked = true;
					break;
				}
			}
			if (!already_tracked) {
				dependencies.push_back(table_id);
			}
		}
	}
}

static unique_ptr<LogicalOperator> CreateMaterializedViewBind(ClientContext &context, TableFunctionBindInput &input,
                                                              TableIndex bind_index, vector<Identifier> &return_names) {
	input.binder->SetAlwaysRequireRebind();
	auto &catalog = DuckLakeBaseMetadataFunction::GetCatalog(context, input.inputs[0]);
	auto &ducklake_catalog = catalog.Cast<DuckLakeCatalog>();
	auto &transaction = DuckLakeTransaction::Get(context, ducklake_catalog);

	string schema = "main";
	auto schema_entry_param = input.named_parameters.find("schema_name");
	if (schema_entry_param != input.named_parameters.end()) {
		schema = StringValue::Get(schema_entry_param->second);
	}
	auto view_name = StringValue::Get(input.named_parameters["view_name"]);
	auto query = StringValue::Get(input.named_parameters["query"]);

	auto statement = ParseSingleSelect(query, "definition");
	auto analysis = AnalyzeMaterializedView(*statement);

	// qualify base refs with the lake catalog + resolve dependencies
	vector<TableIndex> dependencies;
	QualifyBaseRefsInLake(context, ducklake_catalog, *statement, dependencies);

	// serialize the (qualified) definition before binding - binding mutates the parsed statement
	auto stored_sql = statement->ToString();

	// bind the definition to derive the backing table schema
	// (child binder of the outer binder so table indices stay unique within the statement)
	auto binder = Binder::CreateBinder(context, input.binder);
	auto &sql_statement = static_cast<SQLStatement &>(*statement);
	auto bound = binder->Bind(sql_statement);

	// dedupe output column names (same rule as CREATE TABLE AS)
	vector<string> column_names;
	case_insensitive_map_t<idx_t> name_counts;
	for (auto &name : bound.names) {
		string column_name = name.empty() ? "unnamed" : name.GetIdentifierName();
		auto count = name_counts.find(column_name);
		if (count != name_counts.end()) {
			count->second++;
			column_name = column_name + "_" + to_string(count->second);
			while (name_counts.find(column_name) != name_counts.end()) {
				count->second++;
				column_name = name + "_" + to_string(count->second);
			}
		}
		name_counts[column_name] = 0;
		column_names.push_back(std::move(column_name));
	}

	// resolve the target schema + name conflicts
	auto &schema_entry = ducklake_catalog.GetSchema(ducklake_catalog.GetCatalogTransaction(context), Identifier(schema));
	auto &dl_schema = schema_entry.Cast<DuckLakeSchemaEntry>();

	bool materialized_view_exists = ducklake_catalog.GetMaterializedViewByName(transaction, schema, view_name) != nullptr;
	for (auto &staged : transaction.GetNewMaterializedViews()) {
		if (staged.schema_id == dl_schema.GetSchemaId() && StringUtil::CIEquals(staged.name, view_name)) {
			materialized_view_exists = true;
			break;
		}
	}
	if (materialized_view_exists) {
		throw CatalogException("Materialized view \"%s.%s\" already exists!", schema, view_name);
	}
	auto existing_entry = dl_schema.GetEntry(ducklake_catalog.GetCatalogTransaction(context),
	                                       CatalogType::TABLE_ENTRY, Identifier(view_name));
	if (existing_entry) {
		throw CatalogException("%s with name \"%s\" already exists!", CatalogTypeToString(existing_entry->type),
		                       view_name);
	}

	auto mv_uuid = UUID::ToString(UUID::GenerateRandomUUID());
	auto backing_table_name = DuckLakeUtil::MaterializedViewBackingTableName(mv_uuid);

	// The backing table is represented in the DuckDB catalog by the logical MV
	// name. Its UUID-derived storage path remains private, while standard catalog
	// discovery exposes the same name that users query.
	auto create_info = make_uniq<CreateTableInfo>(schema_entry, Identifier(view_name));
	// DuckDB 2.0 keeps catalog-managed MV classification separate from native
	// MV semantics. Mark the backing relation immediately so discovery is
	// correct even before the transaction is reloaded from metadata.
	create_info->catalog_materialized_view = true;
	for (idx_t i = 0; i < bound.types.size(); i++) {
		create_info->columns.AddColumn(ColumnDefinition(Identifier(column_names[i]), bound.types[i]));
	}
	auto table_binder = Binder::CreateBinder(context, input.binder);
	auto bound_create = table_binder->BindCreateTableInfo(std::move(create_info));

	auto table_uuid = transaction.GenerateUUID();
	auto table_data_path =
	    dl_schema.DataPath() + ducklake_catalog.GeneratePathFromName(table_uuid, backing_table_name);
	auto entry = dl_schema.CreateTableExtended(ducklake_catalog.GetCatalogTransaction(context), *bound_create,
	                                           table_uuid, table_data_path);
	if (!entry) {
		throw InvalidInputException("Failed to create backing table for materialized view \"%s\"", view_name);
	}
	auto &table = entry->Cast<DuckLakeTableEntry>();

	// stage the materialized view metadata
	DuckLakeMaterializedViewInfo mv_info;
	mv_info.id = TableIndex(transaction.GetLocalCatalogId());
	mv_info.schema_id = dl_schema.GetSchemaId();
	mv_info.uuid = std::move(mv_uuid);
	mv_info.name = view_name;
	mv_info.dialect = "duckdb";
	mv_info.sql = DuckLakeUtil::ReplaceSkippingQuotes(stored_sql, ducklake_catalog.GetName() + ".",
	                                                  "{DUCKLAKE_CATALOG}.");
	mv_info.backing_table_id = table.GetTableId();
	mv_info.last_refreshed_snapshot = transaction.GetSnapshot().snapshot_id;

	// resolve dependencies from the parsed statement (all base tables referenced anywhere in the FROM)
	mv_info.dependencies = dependencies;
	transaction.CreateMaterializedView(std::move(mv_info));

	// write the initial content: definition plan -> files -> refresh operator
	auto plan = std::move(bound.plan);
	auto logical_diff_sql = BuildLogicalDiffSQL(
	    string(),
	    ResolveMaterializedViewSQL(stored_sql, ducklake_catalog), bound.names, analysis.key_positions);
	return BuildMVWritePlan(context, *input.binder, bind_index, std::move(plan), table,
	                        transaction.GetNewMaterializedViews().back().id, schema, view_name, "full",
	                        logical_diff_sql, return_names);
}

DuckLakeCreateMaterializedViewFunction::DuckLakeCreateMaterializedViewFunction()
    : TableFunction("ducklake_create_materialized_view", {LogicalType::VARCHAR}, nullptr, nullptr, nullptr) {
	named_parameters["schema_name"] = LogicalType::VARCHAR;
	named_parameters["view_name"] = LogicalType::VARCHAR;
	named_parameters["query"] = LogicalType::VARCHAR;
	bind_operator = CreateMaterializedViewBind;
}

//===--------------------------------------------------------------------===//
// Refresh
//===--------------------------------------------------------------------===//

//! Check the snapshot change feed for changes touching any dependency in (start_snapshot, end_snapshot].
static bool DependenciesChanged(DuckLakeTransaction &transaction, const DuckLakeMaterializedViewInfo &mv,
                                idx_t start_snapshot, idx_t end_snapshot) {
	return transaction.GetCatalog().MaterializedViewDependenciesChanged(transaction, mv, start_snapshot, end_snapshot);
}

//! Build a pure-delta refresh for SUM/COUNT definitions:
//! apply signed CDC measures onto existing MV rows for changed keys; keep untouched rows.
static string BuildDeltaRefreshSQL(DuckLakeCatalog &catalog, const DuckLakeMaterializedViewInfo &mv,
                                   const string &mv_schema_name, const MaterializedViewAnalysis &analysis,
                                   const vector<Identifier> &bound_column_names, const vector<LogicalType> &bound_types,
                                   idx_t start_snapshot, idx_t end_snapshot) {
	auto &lake_name = catalog.GetName();
	auto base_relation_name = analysis.base_alias.empty() ? analysis.base_table : analysis.base_alias;
	string base_alias = " AS " + SQLIdentifier(base_relation_name);
	string base_ref = StringUtil::Format("%s.%s.%s", SQLIdentifier(lake_name), SQLIdentifier(analysis.base_schema),
	                                     SQLIdentifier(analysis.base_table));
	string mv_ref = StringUtil::Format("%s.%s.%s", SQLIdentifier(lake_name), SQLIdentifier(mv_schema_name),
	                                   SQLIdentifier(mv.name));

	string cdc_key_select;
	for (idx_t i = 0; i < analysis.key_expr_sql.size(); i++) {
		if (i > 0) {
			cdc_key_select += ", ";
		}
		cdc_key_select += StringUtil::Format("%s AS __k%d", analysis.key_expr_sql[i], i);
	}
	string ins_call = StringUtil::Format("%s(%s, %s, %s, %d, %d)", "ducklake_table_insertions", SQLQuote(lake_name),
	                                     SQLQuote(analysis.base_schema), SQLQuote(analysis.base_table), start_snapshot,
	                                     end_snapshot);
	string del_call = StringUtil::Format("%s(%s, %s, %s, %d, %d)", "ducklake_table_deletions", SQLQuote(lake_name),
	                                     SQLQuote(analysis.base_schema), SQLQuote(analysis.base_table), start_snapshot,
	                                     end_snapshot);
	string cdc_where = analysis.where_sql.empty() ? "" : " WHERE " + analysis.where_sql;

	// measure expressions: one column per aggregate (__d0, __d1, ...)
	string ins_measures;
	string del_measures;
	string delta_aggs;
	for (idx_t a = 0; a < analysis.aggregates.size(); a++) {
		auto &agg = analysis.aggregates[a];
		if (a > 0) {
			ins_measures += ", ";
			del_measures += ", ";
			delta_aggs += ", ";
		}
		string pos_expr;
		string neg_expr;
		switch (agg.kind) {
		case MVAggKind::SUM:
			pos_expr = agg.child_sql;
			neg_expr = StringUtil::Format("(-(%s))", agg.child_sql);
			break;
		case MVAggKind::COUNT_STAR:
			pos_expr = "CAST(1 AS BIGINT)";
			neg_expr = "CAST(-1 AS BIGINT)";
			break;
		case MVAggKind::COUNT_COL:
			pos_expr = StringUtil::Format("CAST(CASE WHEN %s IS NULL THEN 0 ELSE 1 END AS BIGINT)", agg.child_sql);
			neg_expr = StringUtil::Format("CAST(CASE WHEN %s IS NULL THEN 0 ELSE -1 END AS BIGINT)", agg.child_sql);
			break;
		case MVAggKind::AVG:
			// AVG is derived from the matching persisted SUM and COUNT(argument) state.
			pos_expr = "CAST(0 AS BIGINT)";
			neg_expr = "CAST(0 AS BIGINT)";
			break;
		case MVAggKind::MIN:
		case MVAggKind::MAX:
			pos_expr = agg.child_sql;
			neg_expr = agg.child_sql;
			break;
		default:
			throw InternalException("BuildDeltaRefreshSQL called with non-delta aggregate");
		}
		ins_measures += StringUtil::Format("%s AS __m%d", pos_expr, a);
		del_measures += StringUtil::Format("%s AS __m%d", neg_expr, a);
		if (agg.kind == MVAggKind::SUM) {
			// A CDC batch containing only NULL inputs contributes zero to an existing SUM.
			delta_aggs += StringUtil::Format("COALESCE(SUM(__m%d), 0) AS __d%d", a, a);
		} else if (agg.kind == MVAggKind::MIN) {
			delta_aggs += StringUtil::Format("MIN(__m%d) FILTER (WHERE __sgn = 1) AS __d%d", a, a);
		} else if (agg.kind == MVAggKind::MAX) {
			delta_aggs += StringUtil::Format("MAX(__m%d) FILTER (WHERE __sgn = 1) AS __d%d", a, a);
		} else {
			delta_aggs += StringUtil::Format("SUM(__m%d) AS __d%d", a, a);
		}
	}

	string kept_condition;
	for (idx_t i = 0; i < analysis.key_positions.size(); i++) {
		if (!kept_condition.empty()) {
			kept_condition += " AND ";
		}
		kept_condition +=
		    StringUtil::Format("ck.__k%d IS NOT DISTINCT FROM __mv.%s", i,
		                       SQLIdentifier(bound_column_names[analysis.key_positions[i]]));
	}
	string kept_select;
	for (idx_t col = 0; col < bound_column_names.size(); col++) {
		if (col > 0) {
			kept_select += ", ";
		}
		kept_select += StringUtil::Format("__mv.%s", SQLIdentifier(bound_column_names[col]));
	}
	string kept = StringUtil::Format(
	    R"(SELECT %s FROM %s AS __mv WHERE NOT EXISTS (SELECT 1 FROM __mv_changed ck WHERE %s))", kept_select, mv_ref,
	    kept_condition);

	string join_condition;
	for (idx_t i = 0; i < analysis.key_positions.size(); i++) {
		if (!join_condition.empty()) {
			join_condition += " AND ";
		}
		join_condition +=
		    StringUtil::Format("d.__k%d IS NOT DISTINCT FROM __mv.%s", i,
		                       SQLIdentifier(bound_column_names[analysis.key_positions[i]]));
	}

	// build updated select list in bound column order
	string updated_select;
	string cnt_guard;
	idx_t next_agg = 0;
	for (idx_t col = 0; col < bound_column_names.size(); col++) {
		if (col > 0) {
			updated_select += ", ";
		}
		// key column?
		optional_idx key_i;
		for (idx_t i = 0; i < analysis.key_positions.size(); i++) {
			if (analysis.key_positions[i] == col) {
				key_i = i;
				break;
			}
		}
		if (key_i.IsValid()) {
			updated_select += StringUtil::Format("coalesce(__mv.%s, d.__k%d) AS %s",
			                                    SQLIdentifier(bound_column_names[col]), key_i.GetIndex(),
			                                    SQLIdentifier(bound_column_names[col]));
			continue;
		}
		if (next_agg >= analysis.aggregates.size()) {
			throw InternalException("BuildDeltaRefreshSQL: more non-key columns than aggregates");
		}
		idx_t agg_i = next_agg++;
		auto &agg = analysis.aggregates[agg_i];
		auto type_sql = bound_types[col].ToString();
		if (agg.kind == MVAggKind::COUNT_STAR) {
			cnt_guard = StringUtil::Format("CAST(coalesce(__mv.%s, 0) + d.__d%d AS %s)",
			                               SQLIdentifier(bound_column_names[col]), agg_i, type_sql);
			updated_select += StringUtil::Format("CAST(coalesce(__mv.%s, 0) + d.__d%d AS %s) AS %s",
			                                    SQLIdentifier(bound_column_names[col]), agg_i, type_sql,
			                                    SQLIdentifier(bound_column_names[col]));
		} else if (agg.kind == MVAggKind::SUM) {
			auto candidate = StringUtil::Format("coalesce(__mv.%s, 0) + d.__d%d",
			                                    SQLIdentifier(bound_column_names[col]), agg_i);
			optional_idx count_agg_i;
			for (idx_t candidate_i = 0; candidate_i < analysis.aggregates.size(); candidate_i++) {
				auto &count_agg = analysis.aggregates[candidate_i];
				if (count_agg.kind == MVAggKind::COUNT_COL && count_agg.child_sql == agg.child_sql) {
					count_agg_i = candidate_i;
					break;
				}
			}
			if (!count_agg_i.IsValid()) {
				throw InternalException("Delta SUM is missing matching COUNT argument state");
			}
			auto &count_agg = analysis.aggregates[count_agg_i.GetIndex()];
			auto non_null_count = StringUtil::Format("coalesce(__mv.%s, 0) + d.__d%d",
			                                             SQLIdentifier(bound_column_names[count_agg.select_index]),
			                                             count_agg_i.GetIndex());
			updated_select += StringUtil::Format(
			    "CASE WHEN (%s) > 0 "
			    "THEN CAST(%s AS %s) ELSE CAST(NULL AS %s) END AS %s",
			    non_null_count, candidate, type_sql, type_sql,
			    SQLIdentifier(bound_column_names[col]));
		} else if (agg.kind == MVAggKind::MIN || agg.kind == MVAggKind::MAX) {
			auto fn = agg.kind == MVAggKind::MIN ? "LEAST" : "GREATEST";
			updated_select += StringUtil::Format(
			    "CAST(CASE WHEN d.__d%d IS NULL THEN __mv.%s WHEN __mv.%s IS NULL THEN d.__d%d ELSE %s(__mv.%s, "
			    "d.__d%d) END AS %s) AS %s",
			    agg_i, SQLIdentifier(bound_column_names[col]), SQLIdentifier(bound_column_names[col]), agg_i, fn,
			    SQLIdentifier(bound_column_names[col]), agg_i, type_sql, SQLIdentifier(bound_column_names[col]));
		} else if (agg.kind == MVAggKind::AVG) {
			optional_idx sum_agg_i;
			optional_idx count_agg_i;
			for (idx_t candidate_i = 0; candidate_i < analysis.aggregates.size(); candidate_i++) {
				auto &state_agg = analysis.aggregates[candidate_i];
				if (state_agg.child_sql != agg.child_sql) {
					continue;
				}
				if (state_agg.kind == MVAggKind::SUM) {
					sum_agg_i = candidate_i;
				} else if (state_agg.kind == MVAggKind::COUNT_COL) {
					count_agg_i = candidate_i;
				}
			}
			if (!sum_agg_i.IsValid() || !count_agg_i.IsValid()) {
				throw InternalException("Delta AVG is missing matching SUM/COUNT argument state");
			}
			auto &sum_agg = analysis.aggregates[sum_agg_i.GetIndex()];
			auto &count_agg = analysis.aggregates[count_agg_i.GetIndex()];
			auto sum_value = StringUtil::Format("coalesce(__mv.%s, 0) + d.__d%d",
			                                        SQLIdentifier(bound_column_names[sum_agg.select_index]),
			                                        sum_agg_i.GetIndex());
			auto count_value = StringUtil::Format("coalesce(__mv.%s, 0) + d.__d%d",
			                                          SQLIdentifier(bound_column_names[count_agg.select_index]),
			                                          count_agg_i.GetIndex());
			updated_select += StringUtil::Format(
			    "CASE WHEN (%s) > 0 THEN CAST((%s) / (%s) AS %s) ELSE CAST(NULL AS %s) END AS %s", count_value,
			    sum_value, count_value, type_sql, type_sql, SQLIdentifier(bound_column_names[col]));
		} else {
			updated_select += StringUtil::Format("CAST(coalesce(__mv.%s, 0) + d.__d%d AS %s) AS %s",
			                                    SQLIdentifier(bound_column_names[col]), agg_i, type_sql,
			                                    SQLIdentifier(bound_column_names[col]));
		}
	}
	if (cnt_guard.empty()) {
		cnt_guard = "1"; // SUM-only views: keep row if any delta (still emit)
	}
	string invalid_filter;
	if (analysis.conditional_delta_eligible) {
		string invalid_key_match;
		for (idx_t i = 0; i < analysis.key_positions.size(); i++) {
			if (!invalid_key_match.empty()) {
				invalid_key_match += " AND ";
			}
			invalid_key_match += StringUtil::Format("ik.__k%d IS NOT DISTINCT FROM d.__k%d", i, i);
		}
		invalid_filter = " AND NOT EXISTS (SELECT 1 FROM __invalid ik WHERE " + invalid_key_match + ")";
	}
	string updated = StringUtil::Format(
	    R"(
SELECT %s
FROM __deltas d
LEFT JOIN %s AS __mv ON %s
WHERE (%s) <> 0%s
)",
	    updated_select, mv_ref, join_condition, cnt_guard, invalid_filter);

	string cte_columns;
	for (idx_t i = 0; i < analysis.key_expr_sql.size(); i++) {
		if (i > 0) {
			cte_columns += ", ";
		}
		cte_columns += StringUtil::Format("__k%d", i);
	}
	if (!analysis.conditional_delta_eligible) {
		return StringUtil::Format(R"(
WITH __mv_changed(%s) AS (
	SELECT DISTINCT %s FROM %s%s%s
	UNION
	SELECT DISTINCT %s FROM %s%s%s
),
__cdc AS (
	SELECT %s, %s, 1 AS __sgn FROM %s%s%s
	UNION ALL
	SELECT %s, %s, -1 AS __sgn FROM %s%s%s
),
__deltas AS (
	SELECT %s, %s FROM __cdc GROUP BY %s
)
SELECT * FROM (%s) UNION ALL SELECT * FROM (%s)
)",
	                          cte_columns, cdc_key_select, ins_call, base_alias, cdc_where, cdc_key_select, del_call,
	                          base_alias, cdc_where, cdc_key_select, ins_measures, ins_call, base_alias, cdc_where,
	                          cdc_key_select, del_measures, del_call, base_alias, cdc_where, cte_columns, delta_aggs,
	                          cte_columns, kept, updated);
	}

	string invalid_key_join;
	for (idx_t i = 0; i < analysis.key_positions.size(); i++) {
		if (!invalid_key_join.empty()) {
			invalid_key_join += " AND ";
		}
		invalid_key_join += StringUtil::Format("c.__k%d IS NOT DISTINCT FROM __mv.%s", i,
		                                      SQLIdentifier(bound_column_names[analysis.key_positions[i]]));
	}
	string extremum_deleted;
	for (idx_t a = 0; a < analysis.aggregates.size(); a++) {
		auto &agg = analysis.aggregates[a];
		if (agg.kind != MVAggKind::MIN && agg.kind != MVAggKind::MAX) {
			continue;
		}
		if (!extremum_deleted.empty()) {
			extremum_deleted += " OR ";
		}
		extremum_deleted += StringUtil::Format("c.__m%d IS NOT DISTINCT FROM __mv.%s", a,
		                                      SQLIdentifier(bound_column_names[agg.select_index]));
	}
	string invalid_columns;
	string invalid_select;
	string rebuild_condition;
	for (idx_t i = 0; i < analysis.key_positions.size(); i++) {
		if (i > 0) {
			invalid_columns += ", ";
			invalid_select += ", ";
			rebuild_condition += " AND ";
		}
		invalid_columns += StringUtil::Format("__k%d", i);
		invalid_select += StringUtil::Format("c.__k%d", i);
		rebuild_condition += StringUtil::Format("ik.__k%d IS NOT DISTINCT FROM %s", i, analysis.key_expr_sql[i]);
	}
	string rebuild_where = analysis.where_sql.empty() ? " WHERE " : " WHERE (" + analysis.where_sql + ") AND ";
	rebuild_where += "EXISTS (SELECT 1 FROM __invalid ik WHERE " + rebuild_condition + ")";
	string rebuild = StringUtil::Format("SELECT %s FROM %s%s%s GROUP BY %s", analysis.select_list_sql, base_ref,
	                                    base_alias, rebuild_where, analysis.group_by_sql);
	return StringUtil::Format(R"(
WITH __mv_changed(%s) AS (
	SELECT DISTINCT %s FROM %s%s%s
	UNION
	SELECT DISTINCT %s FROM %s%s%s
),
__cdc AS (
	SELECT %s, %s, 1 AS __sgn FROM %s%s%s
	UNION ALL
	SELECT %s, %s, -1 AS __sgn FROM %s%s%s
),
__deltas AS (
	SELECT %s, %s FROM __cdc GROUP BY %s
),
__invalid(%s) AS (
	SELECT DISTINCT %s FROM __cdc c JOIN %s AS __mv ON %s
	WHERE c.__sgn = -1 AND (%s)
)
SELECT * FROM (%s) UNION ALL SELECT * FROM (%s) UNION ALL SELECT * FROM (%s)
)",
	                          cte_columns, cdc_key_select, ins_call, base_alias, cdc_where, cdc_key_select, del_call,
	                          base_alias, cdc_where, cdc_key_select, ins_measures, ins_call, base_alias, cdc_where,
	                          cdc_key_select, del_measures, del_call, base_alias, cdc_where, cte_columns, delta_aggs,
	                          cte_columns, invalid_columns, invalid_select, mv_ref, invalid_key_join, extremum_deleted,
	                          kept, updated, rebuild);
}

//! Build the incremental refresh SQL:
//!   WITH changed AS (distinct group keys touched by CDC in (last, current])
//!   (backing rows for untouched groups) UNION ALL (full recompute of the changed groups)
static string BuildIncrementalRefreshSQL(DuckLakeCatalog &catalog, const DuckLakeMaterializedViewInfo &mv,
                                         const string &mv_schema_name, const MaterializedViewAnalysis &analysis,
                                         const vector<Identifier> &bound_column_names, idx_t start_snapshot,
                                         idx_t end_snapshot) {
	auto &lake_name = catalog.GetName();
	auto base_relation_name = analysis.base_alias.empty() ? analysis.base_table : analysis.base_alias;
	string base_alias = " AS " + SQLIdentifier(base_relation_name);
	string base_ref = StringUtil::Format("%s.%s.%s", SQLIdentifier(lake_name), SQLIdentifier(analysis.base_schema),
	                                     SQLIdentifier(analysis.base_table));
	string mv_ref = StringUtil::Format("%s.%s.%s", SQLIdentifier(lake_name), SQLIdentifier(mv_schema_name),
	                                     SQLIdentifier(mv.name));
	// key expressions applied over the CDC output (columns of the base table)
	string cdc_select_ins, cdc_select_del;
	for (idx_t i = 0; i < analysis.key_expr_sql.size(); i++) {
		if (i > 0) {
			cdc_select_ins += ", ";
			cdc_select_del += ", ";
		}
		cdc_select_ins += StringUtil::Format("%s AS __k%d", analysis.key_expr_sql[i], i);
		cdc_select_del += analysis.key_expr_sql[i];
	}
	string ins_call = StringUtil::Format("%s(%s, %s, %s, %d, %d)", "ducklake_table_insertions", SQLQuote(lake_name),
	                                     SQLQuote(analysis.base_schema), SQLQuote(analysis.base_table), start_snapshot,
	                                     end_snapshot);
	string del_call = StringUtil::Format("%s(%s, %s, %s, %d, %d)", "ducklake_table_deletions", SQLQuote(lake_name),
	                                     SQLQuote(analysis.base_schema), SQLQuote(analysis.base_table), start_snapshot,
	                                     end_snapshot);

	// kept part: backing rows whose keys are NOT in the changed set
	string kept_condition;
	for (idx_t i = 0; i < analysis.key_positions.size(); i++) {
		if (!kept_condition.empty()) {
			kept_condition += " AND ";
		}
		kept_condition +=
		    StringUtil::Format("ck.__k%d IS NOT DISTINCT FROM __mv.%s", i,
		                       SQLIdentifier(bound_column_names[analysis.key_positions[i]]));
	}
	string kept = StringUtil::Format(
	    R"(
SELECT * FROM %s AS __mv WHERE NOT EXISTS (SELECT 1 FROM __mv_changed ck WHERE %s
))",
	    mv_ref, kept_condition);

	// rebuild part: the original aggregate over base rows restricted to the changed keys
	string rebuild_condition;
	for (idx_t i = 0; i < analysis.key_expr_sql.size(); i++) {
		if (!rebuild_condition.empty()) {
			rebuild_condition += " AND ";
		}
		rebuild_condition += StringUtil::Format("ck.__k%d IS NOT DISTINCT FROM %s", i, analysis.key_expr_sql[i]);
	}
	string where_clause;
	if (!analysis.where_sql.empty()) {
		where_clause = " WHERE " + analysis.where_sql + " AND EXISTS (SELECT 1 FROM __mv_changed ck WHERE " +
		               rebuild_condition + ")";
	} else {
		where_clause = " WHERE EXISTS (SELECT 1 FROM __mv_changed ck WHERE " + rebuild_condition + ")";
	}
	string rebuild = StringUtil::Format(R"(
SELECT %s FROM %s%s%s GROUP BY %s
)",
	                                     analysis.select_list_sql, base_ref, base_alias, where_clause,
	                                     analysis.group_by_sql);

	// key column list of the CTE
	string cte_columns;
	for (idx_t i = 0; i < analysis.key_expr_sql.size(); i++) {
		if (i > 0) {
			cte_columns += ", ";
		}
		cte_columns += StringUtil::Format("__k%d", i);
	}
	return StringUtil::Format(R"(
WITH __mv_changed(%s) AS (
	SELECT DISTINCT %s FROM %s%s
	UNION
	SELECT DISTINCT %s FROM %s%s
)
SELECT * FROM (%s) UNION ALL SELECT * FROM (%s)
)",
	                          cte_columns, cdc_select_ins, ins_call, base_alias, cdc_select_del, del_call, base_alias,
	                          kept, rebuild);
}

//! Join-incremental: fact-only CDC → changed group keys via fact⋈dim, then kept ∪ recompute over the join.
static string BuildJoinIncrementalRefreshSQL(DuckLakeCatalog &catalog, const DuckLakeMaterializedViewInfo &mv,
                                             const string &mv_schema_name, const MaterializedViewAnalysis &analysis,
                                             const vector<Identifier> &bound_column_names, idx_t start_snapshot,
                                             idx_t end_snapshot) {
	auto &lake_name = catalog.GetName();
	auto fact_relation_name = analysis.fact_alias.empty() ? analysis.fact_table : analysis.fact_alias;
	auto dim_relation_name = analysis.dim_alias.empty() ? analysis.dim_table : analysis.dim_alias;
	string fact_alias = " AS " + SQLIdentifier(fact_relation_name);
	string dim_alias = " AS " + SQLIdentifier(dim_relation_name);
	string fact_ref = StringUtil::Format("%s.%s.%s", SQLIdentifier(lake_name), SQLIdentifier(analysis.fact_schema),
	                                     SQLIdentifier(analysis.fact_table));
	string dim_ref = StringUtil::Format("%s.%s.%s", SQLIdentifier(lake_name), SQLIdentifier(analysis.dim_schema),
	                                    SQLIdentifier(analysis.dim_table));
	string mv_ref = StringUtil::Format("%s.%s.%s", SQLIdentifier(lake_name), SQLIdentifier(mv_schema_name),
	                                   SQLIdentifier(mv.name));
	string join_from = StringUtil::Format("%s%s JOIN %s%s ON %s", fact_ref, fact_alias, dim_ref, dim_alias,
	                                      analysis.join_condition_sql);

	string cdc_select_ins, cdc_select_del;
	for (idx_t i = 0; i < analysis.key_expr_sql.size(); i++) {
		if (i > 0) {
			cdc_select_ins += ", ";
			cdc_select_del += ", ";
		}
		// group keys may reference dim columns — join CDC fact rows to dim to project keys
		cdc_select_ins += StringUtil::Format("%s AS __k%d", analysis.key_expr_sql[i], i);
		cdc_select_del += analysis.key_expr_sql[i];
	}
	string ins_call = StringUtil::Format("%s(%s, %s, %s, %d, %d)", "ducklake_table_insertions", SQLQuote(lake_name),
	                                     SQLQuote(analysis.fact_schema), SQLQuote(analysis.fact_table), start_snapshot,
	                                     end_snapshot);
	string del_call = StringUtil::Format("%s(%s, %s, %s, %d, %d)", "ducklake_table_deletions", SQLQuote(lake_name),
	                                     SQLQuote(analysis.fact_schema), SQLQuote(analysis.fact_table), start_snapshot,
	                                     end_snapshot);

	string kept_condition;
	for (idx_t i = 0; i < analysis.key_positions.size(); i++) {
		if (!kept_condition.empty()) {
			kept_condition += " AND ";
		}
		kept_condition +=
		    StringUtil::Format("ck.__k%d IS NOT DISTINCT FROM __mv.%s", i,
		                       SQLIdentifier(bound_column_names[analysis.key_positions[i]]));
	}
	string kept = StringUtil::Format(
	    R"(SELECT * FROM %s AS __mv WHERE NOT EXISTS (SELECT 1 FROM __mv_changed ck WHERE %s))", mv_ref,
	    kept_condition);

	string rebuild_condition;
	for (idx_t i = 0; i < analysis.key_expr_sql.size(); i++) {
		if (!rebuild_condition.empty()) {
			rebuild_condition += " AND ";
		}
		rebuild_condition += StringUtil::Format("ck.__k%d IS NOT DISTINCT FROM %s", i, analysis.key_expr_sql[i]);
	}
	string where_clause;
	if (!analysis.where_sql.empty()) {
		where_clause = " WHERE " + analysis.where_sql + " AND EXISTS (SELECT 1 FROM __mv_changed ck WHERE " +
		               rebuild_condition + ")";
	} else {
		where_clause = " WHERE EXISTS (SELECT 1 FROM __mv_changed ck WHERE " + rebuild_condition + ")";
	}
	string rebuild = StringUtil::Format(R"(SELECT %s FROM %s%s GROUP BY %s)", analysis.select_list_sql, join_from,
	                                    where_clause, analysis.group_by_sql);

	string cte_columns;
	for (idx_t i = 0; i < analysis.key_expr_sql.size(); i++) {
		if (i > 0) {
			cte_columns += ", ";
		}
		cte_columns += StringUtil::Format("__k%d", i);
	}
	// CDC rows are fact-shaped; join to dim to evaluate group keys that may live on dim
	return StringUtil::Format(R"(
WITH __mv_changed(%s) AS (
	SELECT DISTINCT %s FROM %s%s JOIN %s%s ON %s
	UNION
	SELECT DISTINCT %s FROM %s%s JOIN %s%s ON %s
)
SELECT * FROM (%s) UNION ALL SELECT * FROM (%s)
)",
	                          cte_columns, cdc_select_ins, ins_call, fact_alias, dim_ref, dim_alias,
	                          analysis.join_condition_sql, cdc_select_del, del_call, fact_alias, dim_ref, dim_alias,
	                          analysis.join_condition_sql, kept, rebuild);
}

static unique_ptr<LogicalOperator> RefreshMaterializedViewBind(ClientContext &context, TableFunctionBindInput &input,
                                                               TableIndex bind_index, vector<Identifier> &return_names) {
	input.binder->SetAlwaysRequireRebind();
	auto &catalog = DuckLakeBaseMetadataFunction::GetCatalog(context, input.inputs[0]);
	auto &ducklake_catalog = catalog.Cast<DuckLakeCatalog>();
	auto &transaction = DuckLakeTransaction::Get(context, ducklake_catalog);

	string schema = "main";
	auto schema_entry_param = input.named_parameters.find("schema_name");
	if (schema_entry_param != input.named_parameters.end()) {
		schema = StringValue::Get(schema_entry_param->second);
	}
	auto view_name = StringValue::Get(input.named_parameters["view_name"]);
	bool if_stale = false;
	auto if_stale_entry = input.named_parameters.find("if_stale");
	if (if_stale_entry != input.named_parameters.end()) {
		if_stale = BooleanValue::Get(if_stale_entry->second);
	}

	// find the materialized view: staged in this transaction first, then persisted
	optional_ptr<const DuckLakeMaterializedViewInfo> mv;
	DuckLakeMaterializedViewInfo staged_copy;
	unique_ptr<DuckLakeMaterializedViewInfo> persisted_mv;
	auto &schema_entry = ducklake_catalog.GetSchema(ducklake_catalog.GetCatalogTransaction(context), Identifier(schema));
	auto schema_id = schema_entry.Cast<DuckLakeSchemaEntry>().GetSchemaId();
	for (auto &staged : transaction.GetNewMaterializedViews()) {
		if (StringUtil::CIEquals(staged.name, view_name) && staged.schema_id == schema_id) {
			staged_copy = staged;
			mv = &staged_copy;
			break;
		}
	}
	if (!mv) {
		persisted_mv = ducklake_catalog.GetMaterializedViewByName(transaction, schema, view_name);
		if (!persisted_mv) {
			throw InvalidInputException("Materialized view \"%s.%s\" does not exist", schema, view_name);
		}
		mv = persisted_mv.get();
	}

	auto current_snapshot = transaction.GetSnapshot().snapshot_id;
	idx_t last_refreshed = 0;
	bool has_last_refreshed = mv->last_refreshed_snapshot.IsValid();
	if (has_last_refreshed) {
		last_refreshed = mv->last_refreshed_snapshot.GetIndex();
	}
	bool local_dependencies_dirty = false;
	for (auto &dependency : mv->dependencies) {
		if (dependency.IsTransactionLocal() || transaction.HasAnyLocalChanges(dependency)) {
			local_dependencies_dirty = true;
			break;
		}
	}

	// The committed snapshot can remain unchanged while this transaction has modified a dependency.
	// Snapshot CDC cannot represent those local changes, so continue to the safe full-refresh path.
	if (has_last_refreshed && last_refreshed == current_snapshot && !local_dependencies_dirty) {
		return BuildConstantResult(bind_index, schema, view_name, "skipped", 0, return_names);
	}
	if (has_last_refreshed && !local_dependencies_dirty &&
	    !DependenciesChanged(transaction, *mv, last_refreshed, current_snapshot)) {
		// covers plain REFRESH and REFRESH IF STALE / if_stale := true
		(void)if_stale;
		return BuildConstantResult(bind_index, schema, view_name, "skipped", 0, return_names);
	}
	(void)if_stale;

	// resolve the backing table entry
	auto snapshot = transaction.GetSnapshot();
	auto entry = ducklake_catalog.GetEntryById(transaction, snapshot, mv->backing_table_id);
	if (!entry) {
		throw InvalidInputException("Failed to find backing table for materialized view \"%s\"", view_name);
	}
	auto &table = entry->Cast<DuckLakeTableEntry>();

	auto resolved_sql = ResolveMaterializedViewSQL(mv->sql, ducklake_catalog);
	auto parsed_definition = ParseSingleSelect(resolved_sql, "definition");
	// Analyze before qualify so WHERE/keys stay CDC-friendly (unqualified column names)
	auto analysis = AnalyzeMaterializedView(*parsed_definition);
	vector<TableIndex> parsed_dependencies;
	QualifyBaseRefsInLake(context, ducklake_catalog, *parsed_definition, parsed_dependencies);

	// resolve dependency table ids by name (for join fact/dim split)
	auto find_dep = [&](const string &schema_name, const string &table_name) -> optional_idx {
		for (idx_t i = 0; i < mv->dependencies.size(); i++) {
			auto dep_entry = ducklake_catalog.GetEntryById(transaction, snapshot, mv->dependencies[i]);
			if (!dep_entry || dep_entry->type != CatalogType::TABLE_ENTRY) {
				continue;
			}
			auto &dep_table = dep_entry->Cast<DuckLakeTableEntry>();
			auto &dep_schema = dep_table.ParentSchema();
			if (StringUtil::Lower(dep_table.name.GetIdentifierName()) == StringUtil::Lower(table_name) &&
			    StringUtil::Lower(dep_schema.name.GetIdentifierName()) == StringUtil::Lower(schema_name)) {
				return i;
			}
		}
		return optional_idx();
	};

	// join-incremental when fact-only CDC; dim change → full
	if (analysis.join_eligible && has_last_refreshed && mv->dependencies.size() >= 2) {
		auto fact_i = find_dep(analysis.fact_schema, analysis.fact_table);
		auto dim_i = find_dep(analysis.dim_schema, analysis.dim_table);
		if (fact_i.IsValid() && dim_i.IsValid()) {
			auto &fact_dep = mv->dependencies[fact_i.GetIndex()];
			auto &dim_dep = mv->dependencies[dim_i.GetIndex()];
			DuckLakeMaterializedViewInfo dim_only = *mv;
			dim_only.dependencies = {dim_dep};
			DuckLakeMaterializedViewInfo fact_only = *mv;
			fact_only.dependencies = {fact_dep};
			bool dim_changed = DependenciesChanged(transaction, dim_only, last_refreshed, current_snapshot);
			bool fact_changed = DependenciesChanged(transaction, fact_only, last_refreshed, current_snapshot);
			if (!dim_changed && fact_changed && !fact_dep.IsTransactionLocal() &&
			    !transaction.HasAnyLocalChanges(fact_dep)) {
				auto binder = Binder::CreateBinder(context, input.binder);
				auto &sql_statement = static_cast<SQLStatement &>(*parsed_definition);
				auto bound = binder->Bind(sql_statement);
				auto join_sql = BuildJoinIncrementalRefreshSQL(ducklake_catalog, *mv, schema, analysis, bound.names,
				                                               last_refreshed + 1, current_snapshot);
					auto join_plan = BindDefinitionPlan(*input.binder, context, join_sql, "join-incremental refresh");
				auto logical_diff_sql = BuildLogicalDiffSQL(
				    MaterializedViewReference(ducklake_catalog, schema, view_name), join_sql, bound.names,
				    analysis.key_positions);
				return BuildMVWritePlan(context, *input.binder, bind_index, std::move(join_plan), table, mv->id, schema,
				                        view_name, "join_incremental", logical_diff_sql, return_names);
			}
		}
	}

	// incremental when eligible, single lake dependency, and no uncommitted base-table changes
	bool incremental = analysis.eligible && !analysis.join_eligible && mv->dependencies.size() == 1;
	if (incremental) {
		auto &dep = mv->dependencies[0];
		if (dep.IsTransactionLocal() || transaction.HasAnyLocalChanges(dep)) {
			incremental = false;
		}
	}

	if (incremental && has_last_refreshed) {
		// bind the definition once to obtain the authoritative output column names
		auto binder = Binder::CreateBinder(context, input.binder);
		auto &sql_statement = static_cast<SQLStatement &>(*parsed_definition);
		auto bound = binder->Bind(sql_statement);
		if ((analysis.delta_eligible || analysis.conditional_delta_eligible) &&
		    analysis.aggregates.size() + analysis.key_positions.size() == bound.names.size()) {
			auto delta_sql = BuildDeltaRefreshSQL(ducklake_catalog, *mv, schema, analysis, bound.names, bound.types,
			                                      last_refreshed + 1, current_snapshot);
			auto delta_plan = BindDefinitionPlan(*input.binder, context, delta_sql, "delta refresh");
			auto logical_diff_sql = BuildLogicalDiffSQL(
			    MaterializedViewReference(ducklake_catalog, schema, view_name), delta_sql, bound.names,
			    analysis.key_positions);
			return BuildMVWritePlan(context, *input.binder, bind_index, std::move(delta_plan), table, mv->id, schema,
			                        view_name, "delta", logical_diff_sql, return_names);
		}
		auto incremental_sql = BuildIncrementalRefreshSQL(ducklake_catalog, *mv, schema, analysis, bound.names,
		                                                  last_refreshed + 1, current_snapshot);
		auto incremental_plan = BindDefinitionPlan(*input.binder, context, incremental_sql, "incremental refresh");
		auto logical_diff_sql = BuildLogicalDiffSQL(
		    MaterializedViewReference(ducklake_catalog, schema, view_name), incremental_sql, bound.names,
		    analysis.key_positions);
		return BuildMVWritePlan(context, *input.binder, bind_index, std::move(incremental_plan), table, mv->id, schema,
		                        view_name, "incremental", logical_diff_sql, return_names);
	}

	auto binder = Binder::CreateBinder(context, input.binder);
	auto &sql_statement = static_cast<SQLStatement &>(*parsed_definition);
	auto bound = binder->Bind(sql_statement);
	auto logical_diff_sql = BuildLogicalDiffSQL(
	    MaterializedViewReference(ducklake_catalog, schema, view_name), resolved_sql, bound.names,
	    analysis.key_positions);
	return BuildMVWritePlan(context, *input.binder, bind_index, std::move(bound.plan), table, mv->id, schema,
	                        view_name, "full", logical_diff_sql, return_names);
}

DuckLakeRefreshMaterializedViewFunction::DuckLakeRefreshMaterializedViewFunction()
    : TableFunction("ducklake_refresh_materialized_view", {LogicalType::VARCHAR}, nullptr, nullptr, nullptr) {
	named_parameters["schema_name"] = LogicalType::VARCHAR;
	named_parameters["view_name"] = LogicalType::VARCHAR;
	named_parameters["if_stale"] = LogicalType::BOOLEAN;
	bind_operator = RefreshMaterializedViewBind;
}

//===--------------------------------------------------------------------===//
// Drop
//===--------------------------------------------------------------------===//

static unique_ptr<LogicalOperator> DropMaterializedViewBind(ClientContext &context, TableFunctionBindInput &input,
                                                            TableIndex bind_index, vector<Identifier> &return_names) {
	auto &catalog = DuckLakeBaseMetadataFunction::GetCatalog(context, input.inputs[0]);
	auto &ducklake_catalog = catalog.Cast<DuckLakeCatalog>();
	auto &transaction = DuckLakeTransaction::Get(context, ducklake_catalog);

	string schema = "main";
	auto schema_entry_param = input.named_parameters.find("schema_name");
	if (schema_entry_param != input.named_parameters.end()) {
		schema = StringValue::Get(schema_entry_param->second);
	}
	auto view_name = StringValue::Get(input.named_parameters["view_name"]);

	optional_ptr<const DuckLakeMaterializedViewInfo> mv;
	DuckLakeMaterializedViewInfo staged_copy;
	unique_ptr<DuckLakeMaterializedViewInfo> persisted_mv;
	auto &schema_entry = ducklake_catalog.GetSchema(ducklake_catalog.GetCatalogTransaction(context), Identifier(schema));
	auto schema_id = schema_entry.Cast<DuckLakeSchemaEntry>().GetSchemaId();
	for (auto &staged : transaction.GetNewMaterializedViews()) {
		if (StringUtil::CIEquals(staged.name, view_name) && staged.schema_id == schema_id) {
			staged_copy = staged;
			mv = &staged_copy;
			break;
		}
	}
	if (!mv) {
		persisted_mv = ducklake_catalog.GetMaterializedViewByName(transaction, schema, view_name);
		if (!persisted_mv) {
			throw InvalidInputException("Materialized view \"%s.%s\" does not exist", schema, view_name);
		}
		mv = persisted_mv.get();
	}

	// drop the backing table through the regular table drop path
	auto snapshot = transaction.GetSnapshot();
	optional_ptr<CatalogEntry> backing;
	if (mv->backing_table_id.IsTransactionLocal()) {
		backing = transaction.GetLocalEntryById(mv->backing_table_id);
	} else {
		backing = ducklake_catalog.GetEntryById(transaction, snapshot, mv->backing_table_id);
	}
	if (backing) {
		transaction.DropTable(backing->Cast<DuckLakeTableEntry>());
	}
	transaction.DropMaterializedView(mv->id);
	return BuildConstantResult(bind_index, schema, view_name, "dropped", 0, return_names);
}

DuckLakeDropMaterializedViewFunction::DuckLakeDropMaterializedViewFunction()
    : TableFunction("ducklake_drop_materialized_view", {LogicalType::VARCHAR}, nullptr, nullptr, nullptr) {
	named_parameters["schema_name"] = LogicalType::VARCHAR;
	named_parameters["view_name"] = LogicalType::VARCHAR;
	bind_operator = DropMaterializedViewBind;
}

//===--------------------------------------------------------------------===//
// Listing
//===--------------------------------------------------------------------===//

static unique_ptr<FunctionData> MaterializedViewsBind(ClientContext &context, TableFunctionBindInput &input,
                                                      vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto &catalog = DuckLakeBaseMetadataFunction::GetCatalog(context, input.inputs[0]);
	auto &ducklake_catalog = catalog.Cast<DuckLakeCatalog>();
	auto &transaction = DuckLakeTransaction::Get(context, ducklake_catalog);

	auto result = make_uniq<MetadataBindData>();
	auto current_snapshot = transaction.GetSnapshot().snapshot_id;
	// persisted views + views created in this transaction
	vector<DuckLakeMaterializedViewInfo> entries = ducklake_catalog.GetMaterializedViews(transaction);
	for (auto &staged : transaction.GetNewMaterializedViews()) {
		bool already_listed = false;
		for (auto &existing : entries) {
			if (StringUtil::CIEquals(existing.name, staged.name) && existing.schema_id == staged.schema_id) {
				already_listed = true;
				break;
			}
		}
		if (!already_listed) {
			entries.push_back(staged);
		}
	}

	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::BIGINT,
	                LogicalType::BIGINT,   LogicalType::BOOLEAN, LogicalType::BIGINT,  LogicalType::VARCHAR};
	names = {Identifier("schema_name"), Identifier("view_name"), Identifier("sql"), Identifier("view_id"),
	         Identifier("backing_table_id"), Identifier("is_stale"), Identifier("last_refreshed_snapshot"),
	         Identifier("refresh_mode")};
	for (auto &mv : entries) {
		string schema_name = "main";
		// resolve the schema name for the view at the transaction snapshot
		for (auto &schema_entry : ducklake_catalog.GetSchemaForSnapshot(transaction, transaction.GetSnapshot())
		                               .GetSchemaIdMap()) {
			if (schema_entry.first == mv.schema_id) {
				schema_name = schema_entry.second.get().name.GetIdentifierName();
				break;
			}
		}
		bool stale = true;
		if (mv.last_refreshed_snapshot.IsValid()) {
			stale = DependenciesChanged(transaction, mv, mv.last_refreshed_snapshot.GetIndex(), current_snapshot);
		}
		Value last_refreshed;
		if (mv.last_refreshed_snapshot.IsValid()) {
			last_refreshed = Value::BIGINT(NumericCast<int64_t>(mv.last_refreshed_snapshot.GetIndex()));
		}
		result->rows.emplace_back(vector<Value> {
		    Value(schema_name),
		    Value(mv.name),
		    Value(mv.sql),
		    Value::BIGINT(NumericCast<int64_t>(mv.id.index)),
		    Value::BIGINT(NumericCast<int64_t>(mv.backing_table_id.index)),
		    Value::BOOLEAN(stale),
		    std::move(last_refreshed),
		    Value("auto"),
		});
	}
	return std::move(result);
}

DuckLakeMaterializedViewsFunction::DuckLakeMaterializedViewsFunction()
    : DuckLakeBaseMetadataFunction("ducklake_materialized_views", MaterializedViewsBind) {
}

static unique_ptr<FunctionData> MaterializedViewRefreshHistoryBind(ClientContext &context,
                                                                    TableFunctionBindInput &input,
                                                                    vector<LogicalType> &return_types,
                                                                    vector<Identifier> &names) {
	auto &catalog = DuckLakeBaseMetadataFunction::GetCatalog(context, input.inputs[0]);
	auto &ducklake_catalog = catalog.Cast<DuckLakeCatalog>();
	auto &transaction = DuckLakeTransaction::Get(context, ducklake_catalog);
	auto result = make_uniq<MetadataBindData>();
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::BIGINT, LogicalType::TIMESTAMP,
	                LogicalType::VARCHAR, LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT,
	                LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT,
	                LogicalType::TIMESTAMP, LogicalType::BIGINT};
	names = {Identifier("schema_name"), Identifier("view_name"), Identifier("refresh_snapshot"),
	         Identifier("refresh_time"), Identifier("refresh_mode"), Identifier("rows_refreshed"),
	         Identifier("refresh_duration_ms"), Identifier("rows_written"), Identifier("rows_added"),
	         Identifier("rows_removed"), Identifier("rows_changed"), Identifier("source_snapshot"),
	         Identifier("source_snapshot_time"), Identifier("lag_ms")};

	string query = R"(
SELECT s.schema_name, v.view_name, h.refresh_snapshot, h.refresh_time, h.refresh_mode, h.rows_refreshed,
       h.refresh_duration_ms, h.rows_written, h.rows_added, h.rows_removed, h.rows_changed,
       h.source_snapshot, h.source_snapshot_time, h.lag_ms
FROM {METADATA_CATALOG}.ducklake_materialized_view_refresh_history h
JOIN {METADATA_CATALOG}.ducklake_materialized_view v ON h.view_id = v.view_id
JOIN {METADATA_CATALOG}.ducklake_schema s ON v.schema_id = s.schema_id
WHERE {SNAPSHOT_ID} >= v.begin_snapshot
  AND ({SNAPSHOT_ID} < v.end_snapshot OR v.end_snapshot IS NULL)
ORDER BY h.refresh_snapshot, h.view_id)";
	auto query_result = transaction.GetMetadataManager().Query(transaction.GetSnapshot(), query);
	bool extended_history = true;
	if (query_result->HasError()) {
		// A read-only attach of an older catalog cannot run the additive ALTER TABLE
		// migration. Fall back to the original six-column contract and expose the
		// new metrics as NULL instead of making history unreadable.
		extended_history = false;
		string fallback_query = R"(
SELECT s.schema_name, v.view_name, h.refresh_snapshot, h.refresh_time, h.refresh_mode, h.rows_refreshed
FROM {METADATA_CATALOG}.ducklake_materialized_view_refresh_history h
JOIN {METADATA_CATALOG}.ducklake_materialized_view v ON h.view_id = v.view_id
JOIN {METADATA_CATALOG}.ducklake_schema s ON v.schema_id = s.schema_id
WHERE {SNAPSHOT_ID} >= v.begin_snapshot
  AND ({SNAPSHOT_ID} < v.end_snapshot OR v.end_snapshot IS NULL)
ORDER BY h.refresh_snapshot, h.view_id)";
		query_result = transaction.GetMetadataManager().Query(transaction.GetSnapshot(), fallback_query);
		if (query_result->HasError()) {
			query_result->GetErrorObject().Throw("Failed to read DuckLake materialized view refresh history: ");
		}
	}
	auto nullable_bigint = [](const Value &value) -> Value {
		if (value.IsNull()) {
			return Value();
		}
		return Value::BIGINT(NumericCast<int64_t>(value.GetValue<idx_t>()));
	};
	for (auto &row : *query_result) {
		vector<Value> values {
		    Value(row.GetValue<string>(0)),
		    Value(row.GetValue<string>(1)),
		    Value::BIGINT(NumericCast<int64_t>(row.GetValue<idx_t>(2))),
		    Value::TIMESTAMP(row.GetValue<timestamp_t>(3)),
		    Value(row.GetValue<string>(4)),
		    Value::BIGINT(NumericCast<int64_t>(row.GetValue<idx_t>(5))),
		};
		if (extended_history) {
			values.emplace_back(nullable_bigint(row.GetChunk().GetValue(6, row.GetRowInChunk())));
			values.emplace_back(nullable_bigint(row.GetChunk().GetValue(7, row.GetRowInChunk())));
			values.emplace_back(nullable_bigint(row.GetChunk().GetValue(8, row.GetRowInChunk())));
			values.emplace_back(nullable_bigint(row.GetChunk().GetValue(9, row.GetRowInChunk())));
			values.emplace_back(nullable_bigint(row.GetChunk().GetValue(10, row.GetRowInChunk())));
			values.emplace_back(nullable_bigint(row.GetChunk().GetValue(11, row.GetRowInChunk())));
			values.emplace_back(row.IsNull(12) ? Value() : Value::TIMESTAMP(row.GetValue<timestamp_t>(12)));
			values.emplace_back(nullable_bigint(row.GetChunk().GetValue(13, row.GetRowInChunk())));
		} else {
			for (idx_t i = 0; i < 8; i++) {
				values.emplace_back(Value());
			}
		}
		result->rows.emplace_back(std::move(values));
	}
	return std::move(result);
}

DuckLakeMaterializedViewRefreshHistoryFunction::DuckLakeMaterializedViewRefreshHistoryFunction()
    : DuckLakeBaseMetadataFunction("ducklake_materialized_view_refresh_history", MaterializedViewRefreshHistoryBind) {
}

} // namespace duckdb
