#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/common/string_util.hpp"
#include "storage/ducklake_catalog.hpp"

namespace duckdb {

static string QuoteString(const string &input) {
	return KeywordHelper::WriteQuoted(input, '\'');
}

//! CREATE / REFRESH / DROP MATERIALIZED VIEW that resolved to this lake bind as calls to the matching table function
unique_ptr<SQLStatement> DuckLakeCatalog::RewriteMaterializedViewStatement(ClientContext &context,
                                                                           MaterializedViewStatementType type,
                                                                           SchemaCatalogEntry &schema,
                                                                           const Identifier &name,
                                                                           optional_ptr<CreateTableInfo> info) {
	auto catalog_name = QuoteString(GetName().GetIdentifierName());
	auto schema_name = QuoteString(schema.name.GetIdentifierName());
	auto view_name = QuoteString(name.GetIdentifierName());
	string rewrite;
	switch (type) {
	case MaterializedViewStatementType::CREATE: {
		D_ASSERT(info && info->query);
		if (info->on_conflict == OnCreateConflict::IGNORE_ON_CONFLICT) {
			throw NotImplementedException(
			    "IF NOT EXISTS is not supported for CREATE MATERIALIZED VIEW in a DuckLake catalog");
		}
		if (info->on_conflict == OnCreateConflict::REPLACE_ON_CONFLICT) {
			throw NotImplementedException("CREATE OR REPLACE is not supported for MATERIALIZED VIEW in a DuckLake "
			                              "catalog - drop the view first");
		}
		rewrite = StringUtil::Format(
		    "SELECT * FROM ducklake_create_materialized_view(%s, schema_name := %s, view_name := %s, query := %s)",
		    catalog_name, schema_name, view_name, QuoteString(info->query->ToString()));
		break;
	}
	case MaterializedViewStatementType::REFRESH:
		D_ASSERT(info);
		rewrite = StringUtil::Format(
		    "SELECT * FROM ducklake_refresh_materialized_view(%s, schema_name := %s, view_name := %s, if_stale := %s)",
		    catalog_name, schema_name, view_name, info->materialized_view_if_stale ? "true" : "false");
		break;
	case MaterializedViewStatementType::DROP:
		rewrite =
		    StringUtil::Format("SELECT * FROM ducklake_drop_materialized_view(%s, schema_name := %s, view_name := %s)",
		                       catalog_name, schema_name, view_name);
		break;
	}
	Parser parser;
	parser.ParseQuery(rewrite);
	if (parser.statements.size() != 1) {
		throw InternalException("Failed to rewrite materialized view statement");
	}
	return std::move(parser.statements[0]);
}

} // namespace duckdb
