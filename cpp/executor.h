// Phase 10 (extended for the stretch goals): the query executor.
// Walks the AST from Phase 9 and routes each query to the cheapest
// path the storage engine (Phases 0-3, 8) actually supports:
//   WHERE <primary key> = v       -> one learned-index point lookup
//   WHERE <primary key> BETWEEN.. -> a range scan
//   anything else, or no WHERE    -> a full table scan, filtering
//                                    each row in memory if there's a
//                                    WHERE clause on a non-key column
//
// The first column in a table's schema is always treated as its
// primary key -- this project's CREATE TABLE grammar has no explicit
// PRIMARY KEY syntax, so this is a deliberate, stated simplification
// rather than an accident.
//
// UPDATE and DELETE share this same routing via find_matches(), which
// returns the actual storage keys alongside decoded rows (SELECT only
// ever needs the rows themselves).
#pragma once

#include <algorithm>
#include <string>
#include <variant>
#include <vector>

#include "catalog.h"
#include "durable_store.h"
#include "row_format.h"
#include "sql_ast.h"
#include "table_key.h"

struct QueryResult {
  bool success = true;
  std::string error;
  std::vector<std::string> column_names;  // populated for SELECT results
  std::vector<std::vector<Value>> rows;
  std::string access_method;  // "point lookup", "range scan", "full scan", "full scan (filtered)"
  std::string explain_text;   // populated only for EXPLAIN SELECT
  int rows_affected = 0;      // populated for UPDATE/DELETE
};

class Executor {
 public:
  Executor(DurableStore& store, Catalog& catalog) : store_(store), catalog_(catalog) {}

  QueryResult execute(const Statement& stmt) {
    return std::visit([this](auto&& s) { return execute_one(s); }, stmt);
  }

 private:
  DurableStore& store_;
  Catalog& catalog_;

  static QueryResult failure(std::string message) {
    QueryResult result;
    result.success = false;
    result.error = std::move(message);
    return result;
  }

  static const char* type_name(ColumnType t) { return t == ColumnType::INT64 ? "INT" : "TEXT"; }

  // The largest primary key a table can hold: 48 bits normally, but only
  // 28 once it has a secondary index, because an index entry packs the
  // primary key into its low 28 bits (table_key.h).
  int64_t max_primary_key(const TableSchema& schema) const {
    return catalog_.indexes_for_table(schema.table_id).empty() ? PRIMARY_KEY_MASK : SECONDARY_PK_MASK;
  }

  // Why value `v` can't be stored in column `col`, or "" if it can.
  // Before these checks, only the primary key's type was verified: a
  // TEXT value could land in an INT column, and a key or indexed value
  // outside the bits table_key.h packs it into was masked into some
  // other key instead of being refused.
  std::string value_error(const TableSchema& schema, int col, const Value& v) const {
    const ColumnDef& def = schema.columns[col];
    if (v.type != def.type) {
      return "type mismatch for column '" + def.name + "': expected " + type_name(def.type) + ", got " +
             type_name(v.type);
    }
    if (col == 0) {
      const int64_t max = max_primary_key(schema);
      if (v.int_val < 0 || v.int_val > max) {
        return "primary key " + std::to_string(v.int_val) + " is out of range: it must be between 0 and " +
               std::to_string(max) + (max == SECONDARY_PK_MASK ? " on a table with a secondary index" : "");
      }
    } else if (def.type == ColumnType::INT64 && catalog_.find_index_for_column(schema.table_id, col) != nullptr &&
               (v.int_val < 0 || v.int_val > SECONDARY_VALUE_MASK)) {
      return "value " + std::to_string(v.int_val) + " is out of range for indexed column '" + def.name +
             "': it must be between 0 and " + std::to_string(SECONDARY_VALUE_MASK);
    }
    return "";
  }

  // A WHERE clause has to name a real column and compare it with a
  // literal of the same type. Comparing an INT column with TEXT used to
  // compare against a meaningless 0, and a column that doesn't exist
  // used to quietly match nothing.
  std::string where_error(const TableSchema& schema, const WhereClause& where) const {
    const int col = find_column_index(schema, where.column);
    if (col < 0) return "no such column: " + where.column;
    const ColumnType want = schema.columns[col].type;
    if (where.value.type != want || (where.op == CompareOp::BETWEEN && where.value2.type != want)) {
      return "type mismatch in WHERE: column '" + where.column + "' is " + type_name(want);
    }
    return "";
  }

  QueryResult execute_one(const CreateTableStmt& stmt) {
    QueryResult result;
    // The first column is the primary key (a stated simplification), and
    // keys are integers, so it has to be INT.
    if (stmt.columns.empty() || stmt.columns[0].type != ColumnType::INT64) {
      return failure("the first column is the primary key and must be INT");
    }
    for (size_t i = 0; i < stmt.columns.size(); i++) {
      for (size_t j = 0; j < i; j++) {
        if (stmt.columns[i].name == stmt.columns[j].name) return failure("duplicate column name: " + stmt.columns[i].name);
      }
    }
    std::vector<ColumnDef> cols;
    for (const auto& c : stmt.columns) cols.push_back({c.name, c.type});
    const int32_t id = catalog_.create_table(stmt.table_name, cols);
    if (id < 0) {
      result.success = false;
      result.error = "table already exists: " + stmt.table_name;
    }
    return result;
  }

  QueryResult execute_one(const InsertStmt& stmt) {
    QueryResult result;
    const TableSchema* schema = catalog_.get_table(stmt.table_name);
    if (!schema) {
      result.success = false;
      result.error = "no such table: " + stmt.table_name;
      return result;
    }
    if (stmt.values.size() != schema->columns.size()) {
      result.success = false;
      result.error = "column count mismatch: table has " + std::to_string(schema->columns.size()) +
                      ", got " + std::to_string(stmt.values.size());
      return result;
    }
    for (size_t i = 0; i < stmt.values.size(); i++) {
      const std::string problem = value_error(*schema, static_cast<int>(i), stmt.values[i]);
      if (!problem.empty()) return failure(problem);
    }

    const int64_t primary_key = stmt.values[0].int_val;
    const int64_t key = pack_key(schema->table_id, primary_key);
    // An INSERT on an existing primary key overwrites that row, so the
    // old row's index entries have to go too. Without this, a query on
    // the old value kept finding the row through the index even after
    // its value changed (found by python/sqlite_diff_test.py).
    std::string existing_raw;
    const bool overwriting = store_.get(key, existing_raw);
    const std::vector<Value> existing_row = overwriting ? decode_row(existing_raw) : std::vector<Value>();
    store_.put(key, encode_row(stmt.values));
    sync_indexes(*schema, primary_key, overwriting ? &existing_row : nullptr, primary_key, &stmt.values);
    return result;
  }

  QueryResult execute_one(const SelectStmt& stmt) {
    QueryResult result;
    const TableSchema* schema = catalog_.get_table(stmt.table_name);
    if (!schema) {
      result.success = false;
      result.error = "no such table: " + stmt.table_name;
      return result;
    }
    if (stmt.has_where) {
      const std::string problem = where_error(*schema, stmt.where);
      if (!problem.empty()) return failure(problem);
    }
    const std::string& pk_col = schema->columns[0].name;
    for (const auto& col : schema->columns) result.column_names.push_back(col.name);

    if (!stmt.has_where) {
      result.access_method = "full scan";
      scan_range(*schema, 0, PRIMARY_KEY_MASK, nullptr, result);
      if (stmt.is_explain) {
        result.explain_text =
            "full table scan, no index used -- returned all " + std::to_string(result.rows.size()) + " row(s)";
      }
    } else if (stmt.where.column == pk_col && stmt.where.op == CompareOp::EQ) {
      result.access_method = "point lookup";
      // A key outside what pack_key can represent can't have been
      // stored, so it can't match; packing it anyway would alias it to
      // some other key.
      const int64_t pk = stmt.where.value.int_val;
      const bool representable = pk >= 0 && pk <= PRIMARY_KEY_MASK;
      const int64_t key = pack_key(schema->table_id, pk);
      if (stmt.is_explain) {
        if (!representable) {
          result.explain_text = "point lookup -- primary key out of range, key not found";
        } else {
          GappedArray::SearchDiagnostics diag = store_.get_explain(key);
          result.explain_text = "point lookup via learned index -- predicted position " +
                                 std::to_string(diag.predicted_position) + ", corrected in " +
                                 std::to_string(diag.probes) + (diag.probes == 1 ? " probe, " : " probes, ") +
                                 (diag.found ? "key found" : "key not found");
        }
      }
      std::string raw;
      if (representable && store_.get(key, raw)) result.rows.push_back(decode_row(raw));
    } else if (stmt.where.column == pk_col && stmt.where.op == CompareOp::BETWEEN) {
      result.access_method = "range scan";
      scan_range(*schema, stmt.where.value.int_val, stmt.where.value2.int_val, nullptr, result);
      if (stmt.is_explain) {
        result.explain_text =
            "range scan over the gapped array -- returned " + std::to_string(result.rows.size()) + " row(s)";
      }
    } else if (stmt.where.op == CompareOp::EQ && stmt.where.value.int_val >= 0 &&
               stmt.where.value.int_val <= SECONDARY_VALUE_MASK &&
               catalog_.find_index_for_column(schema->table_id, find_column_index(*schema, stmt.where.column)) !=
                   nullptr) {
      // (A value outside the index's 20 bits can't be in the index, so
      // that case skips this branch and gets the filtered full scan
      // below, which gives the right answer instead of a masked one.)
      // A secondary index exists on this exact column -- this is the
      // whole point of this stretch goal made real: a non-key column
      // equality lookup no longer has to fall through to a full scan.
      result.access_method = "secondary index lookup";
      const IndexInfo* idx =
          catalog_.find_index_for_column(schema->table_id, find_column_index(*schema, stmt.where.column));
      const int64_t lo = pack_key(idx->index_storage_id, pack_secondary_composite(stmt.where.value.int_val, 0));
      const int64_t hi =
          pack_key(idx->index_storage_id, pack_secondary_composite(stmt.where.value.int_val, SECONDARY_PK_MASK));
      for (auto& [composite_key, unused_val] : store_.range_scan(lo, hi)) {
        (void)unused_val;
        const int64_t pk = unpack_secondary_pk(unpack_primary_key(composite_key));
        std::string raw;
        if (store_.get(pack_key(schema->table_id, pk), raw)) result.rows.push_back(decode_row(raw));
      }
      if (stmt.is_explain) {
        result.explain_text = "secondary index lookup on '" + stmt.where.column + "' -- returned " +
                               std::to_string(result.rows.size()) + " row(s), no full scan needed";
      }
    } else {
      // Anything else -- a non-key column, or an operator we didn't
      // give a dedicated path to (<, >, <=, >=  on the key) -- falls
      // through to a full scan with in-memory filtering. Extending
      // those comparison operators to also use scan_range would be a
      // small, natural addition (the machinery already supports
      // arbitrary bounds); deliberately not done here, to keep this
      // phase's scope matched to what was planned.
      result.access_method = "full scan (filtered)";
      scan_range(*schema, 0, PRIMARY_KEY_MASK, &stmt.where, result);
      if (stmt.is_explain) {
        result.explain_text = "full table scan, no index used -- filtered down to " +
                               std::to_string(result.rows.size()) + " matching row(s)";
      }
    }

    if (stmt.is_explain) return result;  // EXPLAIN reports the plan, not transformed data

    if (stmt.has_order_by) {
      const int col_idx = find_column_index(*schema, stmt.order_by_column);
      if (col_idx < 0) {
        result.success = false;
        result.error = "no such column: " + stmt.order_by_column;
        return result;
      }
      const bool desc = stmt.order_desc;
      std::sort(result.rows.begin(), result.rows.end(),
                [col_idx, desc](const std::vector<Value>& a, const std::vector<Value>& b) {
                  int cmp;
                  if (a[col_idx].type == ColumnType::INT64) {
                    cmp = (a[col_idx].int_val < b[col_idx].int_val) ? -1
                          : (a[col_idx].int_val > b[col_idx].int_val) ? 1
                                                                      : 0;
                  } else {
                    cmp = a[col_idx].text_val.compare(b[col_idx].text_val);
                  }
                  return desc ? (cmp > 0) : (cmp < 0);
                });
    }

    if (stmt.target == SelectTarget::COUNT_STAR) {
      const int64_t count = static_cast<int64_t>(result.rows.size());
      result.rows.clear();
      result.rows.push_back({Value::make_int(count)});
      result.column_names = {"COUNT(*)"};
    } else if (stmt.target == SelectTarget::SUM) {
      const int col_idx = find_column_index(*schema, stmt.sum_column);
      if (col_idx < 0) {
        result.success = false;
        result.error = "no such column: " + stmt.sum_column;
        return result;
      }
      if (schema->columns[col_idx].type != ColumnType::INT64) {
        result.success = false;
        result.error = "SUM requires an INT column, got TEXT column: " + stmt.sum_column;
        return result;
      }
      int64_t total = 0;
      for (const auto& row : result.rows) total += row[col_idx].int_val;
      result.rows.clear();
      result.rows.push_back({Value::make_int(total)});
      result.column_names = {"SUM(" + stmt.sum_column + ")"};
    }

    // LIMIT applies to the rows the query returns. For COUNT(*) and SUM
    // that's the one summary row, not the rows being counted, so it has
    // to come after the aggregate. It used to come before, which made
    // COUNT(*) ... LIMIT 3 top out at 3 (found by the SQLite
    // differential test).
    if (stmt.has_limit && static_cast<int64_t>(result.rows.size()) > stmt.limit_count) {
      result.rows.resize(std::max<int64_t>(0, stmt.limit_count));
    }

    return result;
  }

  QueryResult execute_one(const UpdateStmt& stmt) {
    QueryResult result;
    const TableSchema* schema = catalog_.get_table(stmt.table_name);
    if (!schema) {
      result.success = false;
      result.error = "no such table: " + stmt.table_name;
      return result;
    }
    for (const auto& assign : stmt.assignments) {
      const int col = find_column_index(*schema, assign.column);
      if (col < 0) return failure("no such column: " + assign.column);
      // every assigned value is a literal, so it can be checked once, up
      // front, before any row changes
      const std::string problem = value_error(*schema, col, assign.value);
      if (!problem.empty()) return failure(problem);
    }
    if (stmt.has_where) {
      const std::string problem = where_error(*schema, stmt.where);
      if (!problem.empty()) return failure(problem);
    }

    for (auto& [old_key, row] : find_matches(*schema, stmt.has_where ? &stmt.where : nullptr)) {
      const std::vector<Value> old_row = row;  // captured before modification, for index sync
      bool pk_changed = false;
      for (const auto& assign : stmt.assignments) {
        const int col_idx = find_column_index(*schema, assign.column);
        row[col_idx] = assign.value;
        if (col_idx == 0) pk_changed = true;
      }
      // If the primary key itself was reassigned, the row's storage
      // key has to move: remove the old key, put under the new one.
      // Otherwise it's a straightforward put under the same key --
      // DurableStore::put already overwrites an existing key's value.
      const int64_t new_key = pk_changed ? pack_key(schema->table_id, row[0].int_val) : old_key;
      if (pk_changed && new_key != old_key) {
        // Moving onto a primary key another row already has overwrites
        // that row, so its index entries have to be removed as well --
        // the same problem as an overwriting INSERT.
        std::string displaced_raw;
        if (store_.get(new_key, displaced_raw)) {
          const std::vector<Value> displaced = decode_row(displaced_raw);
          sync_indexes(*schema, unpack_primary_key(new_key), &displaced, 0, nullptr);
        }
      }
      if (pk_changed) store_.remove(old_key);
      store_.put(new_key, encode_row(row));
      sync_indexes(*schema, unpack_primary_key(old_key), &old_row, unpack_primary_key(new_key), &row);
      result.rows_affected++;
    }
    return result;
  }

  QueryResult execute_one(const DeleteStmt& stmt) {
    QueryResult result;
    const TableSchema* schema = catalog_.get_table(stmt.table_name);
    if (!schema) {
      result.success = false;
      result.error = "no such table: " + stmt.table_name;
      return result;
    }

    if (stmt.has_where) {
      const std::string problem = where_error(*schema, stmt.where);
      if (!problem.empty()) return failure(problem);
    }
    for (auto& [key, row] : find_matches(*schema, stmt.has_where ? &stmt.where : nullptr)) {
      if (store_.remove(key)) {
        sync_indexes(*schema, unpack_primary_key(key), &row, 0, nullptr);
        result.rows_affected++;
      }
    }
    return result;
  }

  QueryResult execute_one(const CreateIndexStmt& stmt) {
    QueryResult result;
    const TableSchema* schema = catalog_.get_table(stmt.table_name);
    if (!schema) {
      result.success = false;
      result.error = "no such table: " + stmt.table_name;
      return result;
    }
    const int col_idx = find_column_index(*schema, stmt.column_name);
    if (col_idx < 0) {
      result.success = false;
      result.error = "no such column: " + stmt.column_name;
      return result;
    }
    if (schema->columns[col_idx].type != ColumnType::INT64) {
      result.success = false;
      result.error = "secondary indexes are only supported on INT columns, got TEXT column: " + stmt.column_name;
      return result;
    }

    // Every existing row has to fit the index's packing: values in 20
    // bits, primary keys in 28. Checked before the index exists, so a
    // refusal leaves nothing half-built.
    const std::vector<std::pair<int64_t, std::vector<Value>>> rows =
        collect_matches(*schema, 0, PRIMARY_KEY_MASK, nullptr);
    for (const auto& [key, row] : rows) {
      const int64_t pk = unpack_primary_key(key);
      if (pk > SECONDARY_PK_MASK) {
        return failure("can't index this table: primary key " + std::to_string(pk) + " is over the " +
                       std::to_string(SECONDARY_PK_MASK) + " limit for tables with a secondary index");
      }
      const int64_t v = row[col_idx].int_val;
      if (v < 0 || v > SECONDARY_VALUE_MASK) {
        return failure("can't index column '" + stmt.column_name + "': the row with primary key " +
                       std::to_string(pk) + " has value " + std::to_string(v) + ", outside 0 to " +
                       std::to_string(SECONDARY_VALUE_MASK));
      }
    }

    const int32_t storage_id = catalog_.create_index(stmt.index_name, schema->table_id, col_idx);
    if (storage_id < 0) {
      result.success = false;
      result.error = "index already exists: " + stmt.index_name;
      return result;
    }

    // Backfill: an index created on a table that already has data
    // needs every existing row added, not just future ones. Sorted
    // by composite key first -- bulk-inserting in primary-key order
    // instead would swing the indexed value wildly from one insert to
    // the next (unlike a steady stream of ordinary single-row writes,
    // which this same insert path already handles correctly), and a
    // one-time bulk operation has the luxury of sorting first.
    std::vector<std::pair<int64_t, int64_t>> composites;  // (composite_key, unused)
    for (const auto& [key, row] : rows) {
      const int64_t pk = unpack_primary_key(key);
      composites.emplace_back(pack_secondary_composite(row[col_idx].int_val, pk), 0);
    }
    std::sort(composites.begin(), composites.end());
    for (const auto& [composite, unused] : composites) {
      (void)unused;
      store_.put(pack_key(storage_id, composite), "");
    }
    return result;
  }

  void scan_range(const TableSchema& schema, int64_t lo_pk, int64_t hi_pk, const WhereClause* filter,
                   QueryResult& result) {
    for (auto& [key, row] : collect_matches(schema, lo_pk, hi_pk, filter)) {
      (void)key;
      result.rows.push_back(std::move(row));
    }
  }

  // Finds every (key, row) matching an optional WHERE clause, using
  // the same point-lookup / range-scan / full-scan routing as SELECT.
  // Shared by UPDATE and DELETE, which both need the actual storage
  // keys (to re-put or remove), not just the decoded rows SELECT
  // returns to the caller.
  std::vector<std::pair<int64_t, std::vector<Value>>> find_matches(const TableSchema& schema,
                                                                     const WhereClause* where) {
    const std::string& pk_col = schema.columns[0].name;
    if (!where) return collect_matches(schema, 0, PRIMARY_KEY_MASK, nullptr);
    if (where->column == pk_col && where->op == CompareOp::EQ) {
      std::vector<std::pair<int64_t, std::vector<Value>>> result;
      const int64_t pk = where->value.int_val;
      const int64_t key = pack_key(schema.table_id, pk);
      std::string raw;
      if (pk >= 0 && pk <= PRIMARY_KEY_MASK && store_.get(key, raw)) result.emplace_back(key, decode_row(raw));
      return result;
    }
    if (where->column == pk_col && where->op == CompareOp::BETWEEN) {
      return collect_matches(schema, where->value.int_val, where->value2.int_val, nullptr);
    }
    return collect_matches(schema, 0, PRIMARY_KEY_MASK, where);
  }

  std::vector<std::pair<int64_t, std::vector<Value>>> collect_matches(const TableSchema& schema, int64_t lo_pk,
                                                                        int64_t hi_pk, const WhereClause* filter) {
    std::vector<std::pair<int64_t, std::vector<Value>>> results;
    // Clamp to the keys pack_key can represent. A bound outside that
    // range (BETWEEN -5 AND 10, say) used to be masked into some huge
    // key, which silently turned the scan into an empty one.
    lo_pk = std::max<int64_t>(lo_pk, 0);
    hi_pk = std::min<int64_t>(hi_pk, PRIMARY_KEY_MASK);
    if (lo_pk > hi_pk) return results;
    const int64_t lo = pack_key(schema.table_id, lo_pk);
    const int64_t hi = pack_key(schema.table_id, hi_pk);
    for (auto& [key, raw] : store_.range_scan(lo, hi)) {
      std::vector<Value> row = decode_row(raw);
      if (filter && !matches(schema, row, *filter)) continue;
      results.emplace_back(key, std::move(row));
    }
    return results;
  }

  int find_column_index(const TableSchema& schema, const std::string& name) const {
    for (size_t i = 0; i < schema.columns.size(); i++) {
      if (schema.columns[i].name == name) return static_cast<int>(i);
    }
    return -1;
  }

  // Keeps every secondary index on a table in sync with a single
  // write. Pass old_row (with the pre-write values) for an update or
  // delete, new_row (with the post-write values) for an insert or
  // update -- both for an update, since the indexed column's value
  // (or the primary key itself) may have changed and the old
  // composite entry has to be removed before the new one is added.
  void sync_indexes(const TableSchema& schema, int64_t old_pk, const std::vector<Value>* old_row, int64_t new_pk,
                     const std::vector<Value>* new_row) {
    for (const IndexInfo& idx : catalog_.indexes_for_table(schema.table_id)) {
      if (old_row) {
        const int64_t old_composite = pack_secondary_composite((*old_row)[idx.column_idx].int_val, old_pk);
        store_.remove(pack_key(idx.index_storage_id, old_composite));
      }
      if (new_row) {
        const int64_t new_composite = pack_secondary_composite((*new_row)[idx.column_idx].int_val, new_pk);
        store_.put(pack_key(idx.index_storage_id, new_composite), "");
      }
    }
  }

  bool matches(const TableSchema& schema, const std::vector<Value>& row, const WhereClause& where) {
    const int col_idx = find_column_index(schema, where.column);
    if (col_idx < 0) return false;  // unknown column: matches nothing rather than crashing
    const Value& v = row[col_idx];

    if (where.op == CompareOp::BETWEEN) {
      if (v.type == ColumnType::INT64) return v.int_val >= where.value.int_val && v.int_val <= where.value2.int_val;
      return v.text_val >= where.value.text_val && v.text_val <= where.value2.text_val;
    }
    int cmp = 0;
    if (v.type == ColumnType::INT64) {
      cmp = (v.int_val < where.value.int_val) ? -1 : (v.int_val > where.value.int_val ? 1 : 0);
    } else {
      int raw_cmp = v.text_val.compare(where.value.text_val);
      cmp = (raw_cmp < 0) ? -1 : (raw_cmp > 0 ? 1 : 0);
    }
    switch (where.op) {
      case CompareOp::EQ: return cmp == 0;
      case CompareOp::LT: return cmp < 0;
      case CompareOp::GT: return cmp > 0;
      case CompareOp::LE: return cmp <= 0;
      case CompareOp::GE: return cmp >= 0;
      default: return false;
    }
  }
};
