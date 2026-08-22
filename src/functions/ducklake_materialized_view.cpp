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
#include "duckdb/parser/expression/comparison_expression.hpp"
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

namespace duckdb {

//===--------------------------------------------------------------------===//
// Helpers
//===--------------------------------------------------------------------===//

static string SQLQuote(const string &input) {
	return KeywordHelper::WriteQuoted(input, '\'');
}

static string SQLIdentifier(const string &input) {
	return KeywordHelper::WriteQuoted(input, '"');
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

//! Recursively verify that a select-list expression either is a supported non-distinct aggregate
//! call (without nested aggregates) or contains no aggregates at all.
static bool SelectItemIsSupported(const ParsedExpression &expr, bool &is_aggregate) {
	if (expr.expression_class == ExpressionClass::FUNCTION) {
		auto &function = expr.Cast<const FunctionExpression>();
		if (IsSupportedAggregate(function.function_name)) {
			if (function.distinct) {
				return false;
			}
			for (auto &child : function.children) {
				if (child->expression_class == ExpressionClass::FUNCTION &&
				    IsSupportedAggregate(child->Cast<const FunctionExpression>().function_name)) {
					// nested aggregate
					return false;
				}
				if (child->HasSubquery()) {
					return false;
				}
			}
			is_aggregate = true;
			return true;
		}
		// scalar function - check children for unsupported aggregates
		for (auto &child : function.children) {
			bool child_aggregate = false;
			if (!SelectItemIsSupported(*child, child_aggregate) || child_aggregate) {
				return false;
			}
		}
		return true;
	}
	if (expr.expression_class == ExpressionClass::STAR) {
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
		if (join.condition->expression_class != ExpressionClass::COMPARISON) {
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
		if (item->expression_class == ExpressionClass::FUNCTION &&
		    IsSupportedAggregate(item->Cast<const FunctionExpression>().function_name)) {
			func = &item->Cast<const FunctionExpression>();
		} else {
			ParsedExpressionIterator::EnumerateChildren(*item, [&](const ParsedExpression &child) {
				if (!func && child.expression_class == ExpressionClass::FUNCTION &&
				    IsSupportedAggregate(child.Cast<const FunctionExpression>().function_name)) {
					func = &child.Cast<const FunctionExpression>();
				}
			});
		}
		if (!func) {
			only_delta_aggs = false;
			continue;
		}
		auto fname = StringUtil::Lower(func->function_name);
		MVAggregateInfo info;
		info.select_index = select_idx;
		if (fname == "sum") {
			info.kind = MVAggKind::SUM;
			info.child_sql = func->children.empty() ? string() : func->children[0]->ToString();
		} else if (fname == "count" || fname == "count_star") {
			if (fname == "count_star" || func->children.empty() ||
			    func->children[0]->expression_class == ExpressionClass::STAR) {
				info.kind = MVAggKind::COUNT_STAR;
			} else {
				info.kind = MVAggKind::COUNT_COL;
				info.child_sql = func->children[0]->ToString();
			}
		} else if (fname == "min") {
			info.kind = MVAggKind::MIN;
			only_delta_aggs = false;
			info.child_sql = func->children.empty() ? string() : func->children[0]->ToString();
		} else if (fname == "max") {
			info.kind = MVAggKind::MAX;
			only_delta_aggs = false;
			info.child_sql = func->children.empty() ? string() : func->children[0]->ToString();
		} else if (fname == "avg") {
			info.kind = MVAggKind::AVG;
			only_delta_aggs = false;
			info.child_sql = func->children.empty() ? string() : func->children[0]->ToString();
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

	result.base_schema = base.schema_name.empty() ? "main" : base.schema_name;
	result.base_table = base.table_name;
	result.base_alias = base.alias;
	result.base_catalog_qualified = !base.catalog_name.empty();
	if (result.join_eligible) {
		result.fact_schema = fact_ref->schema_name.empty() ? "main" : fact_ref->schema_name;
		result.fact_table = fact_ref->table_name;
		result.fact_alias = fact_ref->alias;
		result.dim_schema = dim_ref->schema_name.empty() ? "main" : dim_ref->schema_name;
		result.dim_table = dim_ref->table_name;
		result.dim_alias = dim_ref->alias;
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
	// Joins use join_incremental, not pure delta
	result.delta_eligible = !result.join_eligible && only_delta_aggs && !result.aggregates.empty();
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
	                  PhysicalOperator &child)
	    : PhysicalOperator(physical_plan, PhysicalOperatorType::EXTENSION, types, 0), table(table_p),
	      mv_view_id(mv_view_id_p), encryption_key(std::move(encryption_key_p)), partition_id(partition_id_p) {
		children.push_back(child);
	}

	DuckLakeTableEntry &table;
	TableIndex mv_view_id;
	string encryption_key;
	optional_idx partition_id;

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

		// record the refresh stats before moving the files into the transaction
		for (auto &file : global_state.written_files) {
			global_state.rows_flushed += file.row_count;
		}
		transaction.AppendFiles(table_id, std::move(global_state.written_files));
		// stamp last_refreshed_snapshot on the materialized view at commit
		transaction.RefreshMaterializedView(mv_view_id);
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
	DuckLakeLogicalMVRefresh(idx_t table_index_p, DuckLakeTableEntry &table_p, TableIndex mv_view_id_p,
	                         string encryption_key_p, optional_idx partition_id_p)
	    : table_index(table_index_p), table(table_p), mv_view_id(mv_view_id_p),
	      encryption_key(std::move(encryption_key_p)), partition_id(partition_id_p) {
	}

	idx_t table_index;
	DuckLakeTableEntry &table;
	TableIndex mv_view_id;
	string encryption_key;
	optional_idx partition_id;

public:
	PhysicalOperator &CreatePlan(ClientContext &context, PhysicalPlanGenerator &planner) override {
		auto &child = planner.CreatePlan(*children[0]);
		return planner.Make<DuckLakeMVRefresh>(types, table, mv_view_id, std::move(encryption_key), partition_id,
		                                       child);
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
			result.emplace_back(table_index, i);
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
static unique_ptr<LogicalOperator> BuildMVWritePlan(ClientContext &context, Binder &binder, idx_t bind_index,
                                                    unique_ptr<LogicalOperator> plan, DuckLakeTableEntry &table,
                                                    TableIndex mv_view_id, const string &refresh_mode,
                                                    vector<string> &return_names) {
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
	                                         std::move(copy_options.info));
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
	copy->names = std::move(copy_options.names);
	copy->expected_types = std::move(copy_options.expected_types);
	copy->hive_file_pattern = copy_input.catalog.UseHiveFilePattern(copy_input.encryption_key.empty(),
	                                                                copy_input.schema_id, copy_input.table_id);
	copy->children.push_back(std::move(plan));

	idx_t mv_index = binder.GenerateTableIndex();
	auto mv_op = make_uniq<DuckLakeLogicalMVRefresh>(mv_index, table, mv_view_id, std::move(copy_input.encryption_key),
	                                                 optional_idx());
	mv_op->children.push_back(std::move(copy));
	mv_op->ResolveOperatorTypes();

	// project (schema, name, mode constant, rows)
	vector<unique_ptr<Expression>> projections;
	auto mv_bindings = mv_op->GetColumnBindings();
	projections.push_back(make_uniq<BoundColumnRefExpression>(LogicalType::VARCHAR, mv_bindings[0]));
	projections.push_back(make_uniq<BoundColumnRefExpression>(LogicalType::VARCHAR, mv_bindings[1]));
	projections.push_back(make_uniq<BoundConstantExpression>(Value(refresh_mode)));
	projections.push_back(make_uniq<BoundColumnRefExpression>(LogicalType::BIGINT, mv_bindings[2]));
	auto projection = make_uniq<LogicalProjection>(bind_index, std::move(projections));
	projection->children.push_back(std::move(mv_op));
	projection->ResolveOperatorTypes();

	return_names = {"schema_name", "view_name", "refresh_mode", "rows_refreshed"};
	return std::move(projection);
}

//! Result row for statements that do not execute a plan (skip / drop).
static unique_ptr<LogicalOperator> BuildConstantResult(idx_t bind_index, const string &schema_name,
                                                       const string &view_name, const string &mode, idx_t rows,
                                                       vector<string> &return_names) {
	auto dummy = make_uniq<LogicalDummyScan>(bind_index);
	vector<ColumnBinding> bindings;
	bindings.emplace_back(bind_index, 0);
	vector<unique_ptr<Expression>> projections;
	projections.push_back(make_uniq<BoundConstantExpression>(Value(schema_name)));
	projections.push_back(make_uniq<BoundConstantExpression>(Value(view_name)));
	projections.push_back(make_uniq<BoundConstantExpression>(Value(mode)));
	projections.push_back(make_uniq<BoundConstantExpression>(Value::BIGINT(NumericCast<int64_t>(rows))));
	auto projection = make_uniq<LogicalProjection>(bind_index, std::move(projections));
	projection->children.push_back(std::move(dummy));
	projection->ResolveOperatorTypes();
	return_names = {"schema_name", "view_name", "refresh_mode", "rows_refreshed"};
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

static void CollectBaseTableRefs(TableRef &ref, vector<reference<BaseTableRef>> &out) {
	switch (ref.type) {
	case TableReferenceType::BASE_TABLE:
		out.push_back(ref.Cast<BaseTableRef>());
		break;
	case TableReferenceType::JOIN: {
		auto &join = ref.Cast<JoinRef>();
		CollectBaseTableRefs(*join.left, out);
		CollectBaseTableRefs(*join.right, out);
		break;
	}
	case TableReferenceType::SUBQUERY: {
		auto &subquery = ref.Cast<SubqueryRef>();
		if (subquery.subquery && subquery.subquery->node &&
		    subquery.subquery->node->type == QueryNodeType::SELECT_NODE) {
			auto &from = subquery.subquery->node->Cast<SelectNode>().from_table;
			if (from) {
				CollectBaseTableRefs(*from, out);
			}
		}
		break;
	}
	default:
		break;
	}
}

//! Qualify base table references that live inside the lake with the lake catalog so the statement
//! binds correctly regardless of the caller's search path. Also collects the dependency table ids.
static void QualifyBaseRefsInLake(ClientContext &context, DuckLakeCatalog &ducklake_catalog,
                                  SelectStatement &statement, vector<TableIndex> &dependencies) {
	vector<reference<BaseTableRef>> base_refs;
	if (statement.node && statement.node->type == QueryNodeType::SELECT_NODE) {
		auto &from = statement.node->Cast<SelectNode>().from_table;
		if (from) {
			CollectBaseTableRefs(*from, base_refs);
		}
	}
	auto &lake_name = ducklake_catalog.GetName();
	for (auto &base_ref : base_refs) {
		auto &ref = base_ref.get();
		if (!ref.catalog_name.empty() && !StringUtil::CIEquals(ref.catalog_name, lake_name)) {
			// references an object outside this lake - cannot be tracked as a dependency
			continue;
		}
		auto base_schema = ref.schema_name.empty() ? "main" : ref.schema_name;
		EntryLookupInfo lookup(CatalogType::TABLE_ENTRY, ref.table_name);
		auto dep_entry = ducklake_catalog.GetEntry(context, base_schema, lookup, OnEntryNotFound::RETURN_NULL);
		if (!dep_entry) {
			// 2-part names parse as schema.table - `<lake>.<table>` lands here with the catalog
			// name in the schema position. Search the lake's schemas for the table instead.
			for (auto &schema_entry : ducklake_catalog.GetSchemas(context)) {
				auto entry =
				    ducklake_catalog.GetEntry(context, schema_entry.get().name, lookup, OnEntryNotFound::RETURN_NULL);
				if (entry) {
					base_schema = schema_entry.get().name;
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
		ref.catalog_name = lake_name;
		ref.schema_name = base_schema;
		if (dep_entry->type == CatalogType::TABLE_ENTRY) {
			dependencies.push_back(dep_entry->Cast<DuckLakeTableEntry>().GetTableId());
		}
	}
}

static unique_ptr<LogicalOperator> CreateMaterializedViewBind(ClientContext &context, TableFunctionBindInput &input,
                                                              idx_t bind_index, vector<string> &return_names) {
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
		string column_name = name.empty() ? "unnamed" : name;
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
	auto &schema_entry = ducklake_catalog.GetSchema(ducklake_catalog.GetCatalogTransaction(context), schema);
	auto &dl_schema = schema_entry.Cast<DuckLakeSchemaEntry>();

	if (ducklake_catalog.GetMaterializedViewByName(transaction, schema, view_name)) {
		throw CatalogException("Materialized view \"%s.%s\" already exists!", schema, view_name);
	}

	auto mv_uuid = UUID::ToString(UUID::GenerateRandomUUID());
	auto backing_table_name = DuckLakeUtil::MaterializedViewBackingTableName(mv_uuid);

	auto create_info = make_uniq<CreateTableInfo>(schema_entry, backing_table_name);
	for (idx_t i = 0; i < bound.types.size(); i++) {
		create_info->columns.AddColumn(ColumnDefinition(column_names[i], bound.types[i]));
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
	return BuildMVWritePlan(context, *input.binder, bind_index, std::move(plan), table,
	                        transaction.GetNewMaterializedViews().back().id, "full", return_names);
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
                                   const vector<string> &bound_column_names, const vector<LogicalType> &bound_types,
                                   idx_t start_snapshot, idx_t end_snapshot) {
	auto &lake_name = catalog.GetName();
	string base_alias = analysis.base_alias.empty() ? "" : " AS " + SQLIdentifier(analysis.base_alias);
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
		default:
			throw InternalException("BuildDeltaRefreshSQL called with non-delta aggregate");
		}
		ins_measures += StringUtil::Format("%s AS __m%d", pos_expr, a);
		del_measures += StringUtil::Format("%s AS __m%d", neg_expr, a);
		delta_aggs += StringUtil::Format("SUM(__m%d) AS __d%d", a, a);
	}

	string kept_condition;
	for (idx_t i = 0; i < analysis.key_positions.size(); i++) {
		if (!kept_condition.empty()) {
			kept_condition += " AND ";
		}
		kept_condition +=
		    StringUtil::Format("ck.__k%d IS NOT DISTINCT FROM __mv.%s", i, SQLIdentifier(bound_column_names[i]));
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
		    StringUtil::Format("d.__k%d IS NOT DISTINCT FROM __mv.%s", i, SQLIdentifier(bound_column_names[i]));
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
		if (agg.kind == MVAggKind::COUNT_STAR || agg.kind == MVAggKind::COUNT_COL) {
			cnt_guard = StringUtil::Format("CAST(coalesce(__mv.%s, 0) + d.__d%d AS %s)",
			                               SQLIdentifier(bound_column_names[col]), agg_i, type_sql);
			updated_select += StringUtil::Format("CAST(coalesce(__mv.%s, 0) + d.__d%d AS %s) AS %s",
			                                    SQLIdentifier(bound_column_names[col]), agg_i, type_sql,
			                                    SQLIdentifier(bound_column_names[col]));
		} else {
			updated_select += StringUtil::Format("CAST(coalesce(__mv.%s, 0) + d.__d%d AS %s) AS %s",
			                                    SQLIdentifier(bound_column_names[col]), agg_i, type_sql,
			                                    SQLIdentifier(bound_column_names[col]));
		}
	}
	if (cnt_guard.empty()) {
		cnt_guard = "1"; // SUM-only views: keep row if any delta (still emit)
	}
	string updated = StringUtil::Format(
	    R"(
SELECT %s
FROM __deltas d
LEFT JOIN %s AS __mv ON %s
WHERE (%s) <> 0
)",
	    updated_select, mv_ref, join_condition, cnt_guard);

	string cte_columns;
	for (idx_t i = 0; i < analysis.key_expr_sql.size(); i++) {
		if (i > 0) {
			cte_columns += ", ";
		}
		cte_columns += StringUtil::Format("__k%d", i);
	}
	return StringUtil::Format(R"(
WITH __mv_changed(%s) AS (
	SELECT DISTINCT %s FROM %s%s%s
	UNION
	SELECT DISTINCT %s FROM %s%s%s
),
__cdc AS (
	SELECT %s, %s FROM %s%s%s
	UNION ALL
	SELECT %s, %s FROM %s%s%s
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

//! Build the incremental refresh SQL:
//!   WITH changed AS (distinct group keys touched by CDC in (last, current])
//!   (backing rows for untouched groups) UNION ALL (full recompute of the changed groups)
static string BuildIncrementalRefreshSQL(DuckLakeCatalog &catalog, const DuckLakeMaterializedViewInfo &mv,
                                         const string &mv_schema_name, const MaterializedViewAnalysis &analysis,
                                         const vector<string> &bound_column_names, idx_t start_snapshot,
                                         idx_t end_snapshot) {
	auto &lake_name = catalog.GetName();
	string base_alias = analysis.base_alias.empty() ? "" : " AS " + SQLIdentifier(analysis.base_alias);
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
		    StringUtil::Format("ck.__k%d IS NOT DISTINCT FROM __mv.%s", i, SQLIdentifier(bound_column_names[i]));
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
                                             const vector<string> &bound_column_names, idx_t start_snapshot,
                                             idx_t end_snapshot) {
	auto &lake_name = catalog.GetName();
	string fact_alias = analysis.fact_alias.empty() ? "" : " AS " + SQLIdentifier(analysis.fact_alias);
	string dim_alias = analysis.dim_alias.empty() ? "" : " AS " + SQLIdentifier(analysis.dim_alias);
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
		    StringUtil::Format("ck.__k%d IS NOT DISTINCT FROM __mv.%s", i, SQLIdentifier(bound_column_names[i]));
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
                                                               idx_t bind_index, vector<string> &return_names) {
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
	auto &schema_entry = ducklake_catalog.GetSchema(ducklake_catalog.GetCatalogTransaction(context), schema);
	auto schema_id = schema_entry.Cast<DuckLakeSchemaEntry>().GetSchemaId();
	for (auto &staged : transaction.GetNewMaterializedViews()) {
		if (staged.name == view_name && staged.schema_id == schema_id) {
			staged_copy = staged;
			mv = &staged_copy;
			break;
		}
	}
	if (!mv) {
		auto *persisted = ducklake_catalog.GetMaterializedViewByName(transaction, schema, view_name);
		if (!persisted) {
			throw InvalidInputException("Materialized view \"%s.%s\" does not exist", schema, view_name);
		}
		mv = persisted;
	}

	auto current_snapshot = transaction.GetSnapshot().snapshot_id;
	idx_t last_refreshed = 0;
	bool has_last_refreshed = mv->last_refreshed_snapshot.IsValid();
	if (has_last_refreshed) {
		last_refreshed = mv->last_refreshed_snapshot.GetIndex();
	}

	// skip when nothing can have changed
	if (has_last_refreshed && last_refreshed == current_snapshot) {
		return BuildConstantResult(bind_index, schema, view_name, "skipped", 0, return_names);
	}
	if (has_last_refreshed && !DependenciesChanged(transaction, *mv, last_refreshed, current_snapshot)) {
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
			if (StringUtil::Lower(dep_table.name) == StringUtil::Lower(table_name) &&
			    StringUtil::Lower(dep_schema.name) == StringUtil::Lower(schema_name)) {
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
				return BuildMVWritePlan(context, *input.binder, bind_index, std::move(join_plan), table, mv->id,
				                        "join_incremental", return_names);
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
		if (analysis.delta_eligible &&
		    analysis.aggregates.size() + analysis.key_positions.size() == bound.names.size()) {
			auto delta_sql = BuildDeltaRefreshSQL(ducklake_catalog, *mv, schema, analysis, bound.names, bound.types,
			                                      last_refreshed + 1, current_snapshot);
			auto delta_plan = BindDefinitionPlan(*input.binder, context, delta_sql, "delta refresh");
			return BuildMVWritePlan(context, *input.binder, bind_index, std::move(delta_plan), table, mv->id, "delta",
			                        return_names);
		}
		auto incremental_sql = BuildIncrementalRefreshSQL(ducklake_catalog, *mv, schema, analysis, bound.names,
		                                                  last_refreshed + 1, current_snapshot);
		auto incremental_plan = BindDefinitionPlan(*input.binder, context, incremental_sql, "incremental refresh");
		return BuildMVWritePlan(context, *input.binder, bind_index, std::move(incremental_plan), table, mv->id,
		                        "incremental", return_names);
	}

	auto binder = Binder::CreateBinder(context, input.binder);
	auto &sql_statement = static_cast<SQLStatement &>(*parsed_definition);
	auto bound = binder->Bind(sql_statement);
	return BuildMVWritePlan(context, *input.binder, bind_index, std::move(bound.plan), table, mv->id, "full",
	                        return_names);
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
                                                            idx_t bind_index, vector<string> &return_names) {
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
	auto &schema_entry = ducklake_catalog.GetSchema(ducklake_catalog.GetCatalogTransaction(context), schema);
	auto schema_id = schema_entry.Cast<DuckLakeSchemaEntry>().GetSchemaId();
	for (auto &staged : transaction.GetNewMaterializedViews()) {
		if (staged.name == view_name && staged.schema_id == schema_id) {
			staged_copy = staged;
			mv = &staged_copy;
			break;
		}
	}
	if (!mv) {
		auto *persisted = ducklake_catalog.GetMaterializedViewByName(transaction, schema, view_name);
		if (!persisted) {
			throw InvalidInputException("Materialized view \"%s.%s\" does not exist", schema, view_name);
		}
		mv = persisted;
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
                                                      vector<LogicalType> &return_types, vector<string> &names) {
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
			if (existing.name == staged.name && existing.schema_id == staged.schema_id) {
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
	names = {"schema_name", "view_name", "sql", "view_id", "backing_table_id", "is_stale",
	         "last_refreshed_snapshot", "refresh_mode"};
	for (auto &mv : entries) {
		string schema_name = "main";
		// resolve the schema name for the view at the transaction snapshot
		for (auto &schema_entry : ducklake_catalog.GetSchemaForSnapshot(transaction, transaction.GetSnapshot())
		                               .GetSchemaIdMap()) {
			if (schema_entry.first == mv.schema_id) {
				schema_name = schema_entry.second.get().name;
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

} // namespace duckdb
