// Phase 8: the catalog -- table ID 0 is reserved for a table that
// describes every other table's schema, stored as ordinary rows in
// the exact same storage engine everything else uses. This is not a
// shortcut; it's the standard pattern (SQLite's sqlite_master,
// Postgres's pg_catalog both work this way).
//
// One real gap this phase doesn't paper over: DurableStore only
// supports point lookups right now, not "list every key in a range"
// (that's Phase 10's range-scan job). So rebuilding the in-memory
// catalog on startup can't ask the index "show me every catalog
// row" -- instead, Catalog does its own independent pass over the
// same WAL file DurableStore already replayed, filtering for entries
// whose packed key belongs to the catalog table. A little redundant
// (the WAL gets replayed twice on startup), but simple and correct,
// and it doesn't require inventing a capability that legitimately
// belongs to a later phase.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "durable_store.h"
#include "row_format.h"
#include "table_key.h"
#include "write_ahead_log.h"

constexpr int32_t CATALOG_TABLE_ID = 0;

struct ColumnDef {
  std::string name;
  ColumnType type;
};

struct TableSchema {
  int32_t table_id;
  std::string name;
  std::vector<ColumnDef> columns;
};

class Catalog {
 public:
  Catalog(DurableStore& store, const std::string& wal_path) : store_(store) { load_existing(wal_path); }

  // Creates a new table and returns its table_id, or -1 if the name
  // is already taken.
  int32_t create_table(const std::string& name, const std::vector<ColumnDef>& columns) {
    if (name_to_id_.count(name)) return -1;
    const int32_t id = next_table_id_++;
    TableSchema schema{id, name, columns};
    persist_schema(schema);
    name_to_id_[name] = id;
    id_to_schema_[id] = std::move(schema);
    return id;
  }

  const TableSchema* get_table(const std::string& name) const {
    auto it = name_to_id_.find(name);
    if (it == name_to_id_.end()) return nullptr;
    return &id_to_schema_.at(it->second);
  }

  size_t table_count() const { return id_to_schema_.size(); }

 private:
  DurableStore& store_;
  int32_t next_table_id_ = 1;  // 0 is reserved for the catalog itself
  std::unordered_map<std::string, int32_t> name_to_id_;
  std::unordered_map<int32_t, TableSchema> id_to_schema_;

  void persist_schema(const TableSchema& schema) {
    std::vector<Value> row;
    row.push_back(Value::make_text(schema.name));
    row.push_back(Value::make_int(static_cast<int64_t>(schema.columns.size())));
    for (const auto& col : schema.columns) {
      row.push_back(Value::make_text(col.name));
      row.push_back(Value::make_int(static_cast<int64_t>(col.type)));
    }
    const int64_t key = pack_key(CATALOG_TABLE_ID, schema.table_id);
    store_.put(key, encode_row(row));
  }

  void load_existing(const std::string& wal_path) {
    for (auto& [key, value] : WriteAheadLog::replay(wal_path)) {
      if (unpack_table_id(key) != CATALOG_TABLE_ID) continue;
      const int32_t table_id = static_cast<int32_t>(unpack_primary_key(key));

      std::vector<Value> row = decode_row(value);
      TableSchema schema;
      schema.table_id = table_id;
      schema.name = row[0].text_val;
      const int64_t num_cols = row[1].int_val;
      for (int64_t i = 0; i < num_cols; i++) {
        ColumnDef col;
        col.name = row[2 + i * 2].text_val;
        col.type = static_cast<ColumnType>(row[2 + i * 2 + 1].int_val);
        schema.columns.push_back(std::move(col));
      }

      name_to_id_[schema.name] = table_id;
      id_to_schema_[table_id] = std::move(schema);
      if (table_id >= next_table_id_) next_table_id_ = table_id + 1;
    }
  }
};
