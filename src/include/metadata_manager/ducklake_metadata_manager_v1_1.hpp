//===----------------------------------------------------------------------===//
//                         DuckDB
//
// metadata_manager/ducklake_metadata_manager_v1_1.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "storage/ducklake_metadata_manager.hpp"

namespace duckdb {

template <typename Base>
class DuckLakeMetadataManagerV1_1 : public Base {
public:
	//! version_tag is the catalog version string written for newly created lakes - the table
	//! shape is shared by the 1.1-dev1 and the port versions (1.1/1.2/1.3)
	explicit DuckLakeMetadataManagerV1_1(DuckLakeTransaction &transaction, string version_tag_p = "1.1-dev1")
	    : Base(transaction), version_tag(std::move(version_tag_p)) {
	}

	string GetDataFileTableStatement() override;
	string GetDeleteFileTableStatement() override;
	string GetFileColumnStatsTableStatement() override;
	string GetTableColumnStatsTableStatement() override;
	string GetCreateTableStatements() override;
	string GetVersionString() override;

private:
	string version_tag;
};

} // namespace duckdb
