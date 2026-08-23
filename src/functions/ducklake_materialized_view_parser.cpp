#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/parser_extension.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/config.hpp"

#include <cctype>

namespace duckdb {

namespace {

char LowerChar(char c) {
	return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

ParserOverrideResult ExtensionError(const string &message) {
	std::runtime_error error(message);
	return ParserOverrideResult(error);
}

void SkipWhitespaceAndComments(const string &query, idx_t &pos) {
	while (pos < query.size()) {
		char c = query[pos];
		if (StringUtil::CharacterIsSpace(c)) {
			pos++;
			continue;
		}
		if (c == '-' && pos + 1 < query.size() && query[pos + 1] == '-') {
			pos += 2;
			while (pos < query.size() && query[pos] != '\n') {
				pos++;
			}
			continue;
		}
		if (c == '/' && pos + 1 < query.size() && query[pos + 1] == '*') {
			pos += 2;
			while (pos + 1 < query.size() && !(query[pos] == '*' && query[pos + 1] == '/')) {
				pos++;
			}
			pos = MinValue<idx_t>(pos + 2, query.size());
			continue;
		}
		break;
	}
}

//! Case-insensitive match of `keyword` at pos, followed by whitespace or end-of-input.
bool MatchKeyword(const string &query, idx_t &pos, const string &keyword) {
	idx_t local = pos;
	SkipWhitespaceAndComments(query, local);
	if (local + keyword.size() > query.size()) {
		return false;
	}
	for (idx_t i = 0; i < keyword.size(); i++) {
		if (LowerChar(query[local + i]) != keyword[i]) {
			return false;
		}
	}
	idx_t after = local + keyword.size();
	if (after < query.size() && !StringUtil::CharacterIsSpace(query[after]) && query[after] != '(') {
		return false;
	}
	pos = after;
	return true;
}

//! Read one (optionally double-quoted) identifier at pos.
bool ReadIdentifier(const string &query, idx_t &pos, string &result) {
	SkipWhitespaceAndComments(query, pos);
	if (pos >= query.size()) {
		return false;
	}
	if (query[pos] == '"') {
		pos++;
		string identifier;
		while (pos < query.size()) {
			if (query[pos] == '"') {
				if (pos + 1 < query.size() && query[pos + 1] == '"') {
					identifier += '"';
					pos += 2;
					continue;
				}
				pos++;
				result = std::move(identifier);
				return true;
			}
			identifier += query[pos++];
		}
		return false;
	}
	idx_t start = pos;
	while (pos < query.size() && query[pos] != '.' && query[pos] != '(' && query[pos] != ')' &&
	       !StringUtil::CharacterIsSpace(query[pos]) && query[pos] != ';') {
		pos++;
	}
	if (pos == start) {
		return false;
	}
	result = query.substr(start, pos - start);
	return true;
}

struct ParsedMVStatement {
	enum class Type { CREATE, REFRESH, DROP };
	Type type;
	string catalog;
	string schema;
	string name;
	//! the definition SQL (CREATE ... AS <definition>) - empty for REFRESH/DROP
	string query;
	//! REFRESH MATERIALIZED VIEW IF STALE
	bool if_stale = false;
};

string QuoteString(const string &input) {
	return KeywordHelper::WriteQuoted(input, '\'');
}

//! Rewrite the intercepted statement into a call to the matching table function.
string BuildRewrite(const ParsedMVStatement &parsed) {
	switch (parsed.type) {
	case ParsedMVStatement::Type::CREATE:
		return StringUtil::Format(
		    "SELECT * FROM ducklake_create_materialized_view(%s, schema_name := %s, view_name := %s, query := %s)",
		    QuoteString(parsed.catalog), QuoteString(parsed.schema), QuoteString(parsed.name),
		    QuoteString(parsed.query));
	case ParsedMVStatement::Type::REFRESH:
		return StringUtil::Format(
		    "SELECT * FROM ducklake_refresh_materialized_view(%s, schema_name := %s, view_name := %s, if_stale := %s)",
		    QuoteString(parsed.catalog), QuoteString(parsed.schema), QuoteString(parsed.name),
		    parsed.if_stale ? "true" : "false");
	case ParsedMVStatement::Type::DROP:
		return StringUtil::Format("SELECT * FROM ducklake_drop_materialized_view(%s, schema_name := %s, view_name := %s)",
		                          QuoteString(parsed.catalog), QuoteString(parsed.schema), QuoteString(parsed.name));
	}
	throw InternalException("Unhandled materialized view statement type");
}

} // namespace

//! Parser override enabling CREATE / REFRESH / DROP MATERIALIZED VIEW syntax. Only statements that
//! start with one of those keyword sequences are rewritten into table-function calls; everything
//! else is declined back to the default parser. DuckLake opts this narrowly scoped override into
//! DuckDB's default parser policy; FALLBACK and STRICT remain supported explicit policies.
ParserOverrideResult DuckLakeMaterializedViewParserOverride(ParserExtensionInfo *info, const string &query,
                                                            ParserOptions &options) {
	idx_t pos = 0;
	ParsedMVStatement parsed;
	if (MatchKeyword(query, pos, "create")) {
		if (!MatchKeyword(query, pos, "materialized") || !MatchKeyword(query, pos, "view")) {
			return ParserOverrideResult();
		}
		parsed.type = ParsedMVStatement::Type::CREATE;
		idx_t if_not_exists = pos;
		if (MatchKeyword(query, if_not_exists, "if") && MatchKeyword(query, if_not_exists, "not") &&
		    MatchKeyword(query, if_not_exists, "exists")) {
			return ExtensionError("IF NOT EXISTS is not supported for CREATE MATERIALIZED VIEW - use CREATE OR REPLACE semantics via "
			    "ducklake_drop_materialized_view + create");
		}
		pos = if_not_exists;
	} else if (MatchKeyword(query, pos, "refresh")) {
		if (!MatchKeyword(query, pos, "materialized") || !MatchKeyword(query, pos, "view")) {
			return ParserOverrideResult();
		}
		parsed.type = ParsedMVStatement::Type::REFRESH;
		idx_t if_stale_pos = pos;
		if (MatchKeyword(query, if_stale_pos, "if") && MatchKeyword(query, if_stale_pos, "stale")) {
			parsed.if_stale = true;
			pos = if_stale_pos;
		}
	} else if (MatchKeyword(query, pos, "drop")) {
		if (!MatchKeyword(query, pos, "materialized") || !MatchKeyword(query, pos, "view")) {
			return ParserOverrideResult();
		}
		parsed.type = ParsedMVStatement::Type::DROP;
		idx_t if_exists = pos;
		if (MatchKeyword(query, if_exists, "if") && MatchKeyword(query, if_exists, "exists")) {
			return ExtensionError("IF EXISTS is not supported for DROP MATERIALIZED VIEW - use ducklake_drop_materialized_view");
		}
		pos = if_exists;
	} else {
		return ParserOverrideResult();
	}

	// read the (optionally catalog/schema qualified) name
	vector<string> parts;
	for (idx_t i = 0; i < 3; i++) {
		string part;
		idx_t part_pos = pos;
		if (!ReadIdentifier(query, part_pos, part)) {
			break;
		}
		if (part_pos < query.size() && query[part_pos] == '.') {
			pos = part_pos + 1;
			parts.push_back(std::move(part));
			continue;
		}
		pos = part_pos;
		parts.push_back(std::move(part));
		break;
	}
	if (parts.empty()) {
		return ExtensionError("MATERIALIZED VIEW statement is missing a view name - qualify it with the lake catalog "
		                      "(e.g. CREATE MATERIALIZED VIEW mylake.mv_name AS SELECT ...)");
	}
	if (parts.size() == 1) {
		// Unqualified materialized views belong to DuckDB's native catalog. Let the
		// default parser handle them instead of claiming them for DuckLake.
		return ParserOverrideResult();
	}
	if (parts.size() == 2) {
		parsed.catalog = parts[0];
		parsed.schema = "main";
		parsed.name = parts[1];
	} else {
		parsed.catalog = parts[0];
		parsed.schema = parts[1];
		parsed.name = parts[2];
	}

	if (parsed.type == ParsedMVStatement::Type::CREATE) {
		if (!MatchKeyword(query, pos, "as")) {
			return ExtensionError("CREATE MATERIALIZED VIEW requires an AS <select statement> clause");
		}
		SkipWhitespaceAndComments(query, pos);
		parsed.query = query.substr(pos);
		StringUtil::Trim(parsed.query);
		// strip a single trailing statement terminator
		if (!parsed.query.empty() && parsed.query.back() == ';') {
			parsed.query.pop_back();
			StringUtil::Trim(parsed.query);
		}
		if (parsed.query.empty()) {
			return ExtensionError("CREATE MATERIALIZED VIEW has an empty definition");
		}
	} else {
		SkipWhitespaceAndComments(query, pos);
		if (pos < query.size() && query[pos] == ';') {
			pos++;
			SkipWhitespaceAndComments(query, pos);
		}
		if (pos < query.size()) {
			// trailing content after the name - not a plain statement we can rewrite
			return ParserOverrideResult();
		}
	}

	auto rewrite = BuildRewrite(parsed);
	Parser parser;
	parser.ParseQuery(rewrite);
	if (parser.statements.size() != 1) {
		return ExtensionError("Failed to rewrite materialized view statement");
	}
	return ParserOverrideResult(std::move(parser.statements));
}

void DuckLakeRegisterMaterializedViewParser(DBConfig &config) {
	ParserExtension extension;
	extension.parser_override = DuckLakeMaterializedViewParserOverride;
	ParserExtension::Register(config, std::move(extension));
}

} // namespace duckdb
