// Phase 8 (extended for the secondary-index stretch goal): the
// catalog -- table ID 0 is reserved for a table that describes every
// other table's schema (and, now, every secondary index), stored as
// ordinary rows in the exact same storage engine everything else
// uses. This is not a shortcut; it's the standard pattern (SQLite's
// sqlite_master, Postgres's pg_catalog both work this way).
//
// Every catalog row now starts with a one-field "kind" discriminator
// (TABLE or INDEX) so both can share the same underlying table
// without a second catalog table or a second replay mechanism.
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

enum class CatalogEntryKind : int64_t { TABLE = 0, INDEX = 1 };

struct ColumnDef {
  std::string name;
  ColumnType type;
};

struct TableSchema {
  int32_t table_id;
  std::string name;
  std::vector<ColumnDef> columns;
};

struct IndexInfo {
  std::string index_name;
  int32_t base_table_id;
  int32_t column_idx;
  int32_t index_storage_id;  // virtual table id used for this index's own composite-key space
};

class Catalog {
 public:
  Catalog(DurableStore& store, const std::string& wal_path) : store_(store) { load_existing(wal_path); }

  // Creates a new table and returns its table_id, or -1 if the name
  // is already taken.
  int32_t create_table(const std::string& name, const std::vector<ColumnDef>& columns) {
    if (name_to_id_.count(name)) return -1;
    const int32_t id = next_id_++;
    TableSchema schema{id, name, columns};
    persist_schema(schema);
    name_to_id_[name] = id;
    id_to_schema_[id] = std::move(schema);
    return id;
  }

  // Registers a new secondary index and returns the virtual table id
  // allocated for its own composite-key storage, or -1 if the name is
  // taken. Drawing this id from the same counter as real table ids
  // guarantees it can never collide with one, without needing a
  // separate reserved id range.
  int32_t create_index(const std::string& index_name, int32_t base_table_id, int32_t column_idx) {
    if (index_name_to_info_.count(index_name)) return -1;
    const int32_t storage_id = next_id_++;
    IndexInfo info{index_name, base_table_id, column_idx, storage_id};
    persist_index(info);
    index_name_to_info_[index_name] = info;
    return storage_id;
  }

  const TableSchema* get_table(const std::string& name) const {
    auto it = name_to_id_.find(name);
    if (it == name_to_id_.end()) return nullptr;
    return &id_to_schema_.at(it->second);
  }

  // The one secondary index on this exact column, if any -- this
  // project only supports one index per column, not composite
  // multi-column indexes.
  const IndexInfo* find_index_for_column(int32_t table_id, int32_t column_idx) const {
    for (const auto& [name, info] : index_name_to_info_) {
      if (info.base_table_id == table_id && info.column_idx == column_idx) return &info;
    }
    return nullptr;
  }

  // Every secondary index on a table, for keeping them all in sync on
  // insert/update/delete.
  std::vector<IndexInfo> indexes_for_table(int32_t table_id) const {
    std::vector<IndexInfo> result;
    for (const auto& [name, info] : index_name_to_info_) {
      if (info.base_table_id == table_id) result.push_back(info);
    }
    return result;
  }

  size_t table_count() const { return id_to_schema_.size(); }

  std::vector<std::string> table_names() const {
    std::vector<std::string> names;
    for (const auto& [name, id] : name_to_id_) names.push_back(name);
    return names;
  }

 private:
  DurableStore& store_;
  int32_t next_id_ = 1;  // 0 is reserved for the catalog itself; tables and indexes share this counter
  std::unordered_map<std::string, int32_t> name_to_id_;
  std::unordered_map<int32_t, TableSchema> id_to_schema_;
  std::unordered_map<std::string, IndexInfo> index_name_to_info_;

  void persist_schema(const TableSchema& schema) {
    std::vector<Value> row;
    row.push_back(Value::make_int(static_cast<int64_t>(CatalogEntryKind::TABLE)));
    row.push_back(Value::make_text(schema.name));
    row.push_back(Value::make_int(static_cast<int64_t>(schema.columns.size())));
    for (const auto& col : schema.columns) {
      row.push_back(Value::make_text(col.name));
      row.push_back(Value::make_int(static_cast<int64_t>(col.type)));
    }
    const int64_t key = pack_key(CATALOG_TABLE_ID, schema.table_id);
    store_.put(key, encode_row(row));
  }

  void persist_index(const IndexInfo& info) {
    std::vector<Value> row;
    row.push_back(Value::make_int(static_cast<int64_t>(CatalogEntryKind::INDEX)));
    row.push_back(Value::make_text(info.index_name));
    row.push_back(Value::make_int(info.base_table_id));
    row.push_back(Value::make_int(info.column_idx));
    row.push_back(Value::make_int(info.index_storage_id));
    // index_storage_id came from the same shared counter as table
    // ids, so it's guaranteed unique as a catalog row key too.
    const int64_t key = pack_key(CATALOG_TABLE_ID, info.index_storage_id);
    store_.put(key, encode_row(row));
  }

  void load_existing(const std::string& wal_path) {
    for (const WalRecord& rec : WriteAheadLog::replay(wal_path)) {
      if (rec.type != WalRecordType::PUT) continue;  // a catalog row is never deleted via the SQL interface
      if (unpack_table_id(rec.key) != CATALOG_TABLE_ID) continue;

      std::vector<Value> row = decode_row(rec.value);
      const CatalogEntryKind kind = static_cast<CatalogEntryKind>(row[0].int_val);

      if (kind == CatalogEntryKind::TABLE) {
        const int32_t table_id = static_cast<int32_t>(unpack_primary_key(rec.key));
        TableSchema schema;
        schema.table_id = table_id;
        schema.name = row[1].text_val;
        const int64_t num_cols = row[2].int_val;
        for (int64_t i = 0; i < num_cols; i++) {
          ColumnDef col;
          col.name = row[3 + i * 2].text_val;
          col.type = static_cast<ColumnType>(row[3 + i * 2 + 1].int_val);
          schema.columns.push_back(std::move(col));
        }
        name_to_id_[schema.name] = table_id;
        id_to_schema_[table_id] = std::move(schema);
        if (table_id >= next_id_) next_id_ = table_id + 1;
      } else {
        IndexInfo info;
        info.index_name = row[1].text_val;
        info.base_table_id = static_cast<int32_t>(row[2].int_val);
        info.column_idx = static_cast<int32_t>(row[3].int_val);
        info.index_storage_id = static_cast<int32_t>(row[4].int_val);
        index_name_to_info_[info.index_name] = info;
        if (info.index_storage_id >= next_id_) next_id_ = info.index_storage_id + 1;
      }
    }
  }
};
