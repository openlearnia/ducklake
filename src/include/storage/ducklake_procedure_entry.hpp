//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/ducklake_procedure_entry.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "common/index.hpp"
#include "storage/ducklake_metadata_info.hpp"
#include "duckdb/catalog/catalog_entry/procedure_catalog_entry.hpp"

namespace duckdb {

class DuckLakeProcedureEntry : public ProcedureCatalogEntry {
public:
	DuckLakeProcedureEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateProcedureInfo &info,
	                       ProcedureIndex procedure_index)
	    : ProcedureCatalogEntry(catalog, schema, info), index(procedure_index) {
	}

	ProcedureIndex GetIndex() const {
		return index;
	}

	ProcedureIndex index;
	//! definition-contract version of the persisted language/body/parameters
	uint64_t definition_version = CURRENT_PROCEDURE_DEFINITION_VERSION;
};
} // namespace duckdb
