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

  QueryResult execute_one(const CreateTableStmt& stmt) {
    QueryResult result;
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
    if (stmt.values[0].type != ColumnType::INT64) {
      result.success = false;
      result.error = "primary key (first column) must be INT";
      return result;
    }

    const int64_t primary_key = stmt.values[0].int_val;
    const int64_t key = pack_key(schema->table_id, primary_key);
    store_.put(key, encode_row(stmt.values));
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
      const int64_t key = pack_key(schema->table_id, stmt.where.value.int_val);
      if (stmt.is_explain) {
        GappedArray::SearchDiagnostics diag = store_.get_explain(key);
        result.explain_text = "point lookup via learned index -- predicted position " +
                               std::to_string(diag.predicted_position) + ", corrected in " +
                               std::to_string(diag.probes) + (diag.probes == 1 ? " probe, " : " probes, ") +
                               (diag.found ? "key found" : "key not found");
      }
      std::string raw;
      if (store_.get(key, raw)) result.rows.push_back(decode_row(raw));
    } else if (stmt.where.column == pk_col && stmt.where.op == CompareOp::BETWEEN) {
      result.access_method = "range scan";
      scan_range(*schema, stmt.where.value.int_val, stmt.where.value2.int_val, nullptr, result);
      if (stmt.is_explain) {
        result.explain_text =
            "range scan over the gapped array -- returned " + std::to_string(result.rows.size()) + " row(s)";
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

    if (stmt.has_limit && static_cast<int64_t>(result.rows.size()) > stmt.limit_count) {
      result.rows.resize(std::max<int64_t>(0, stmt.limit_count));
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
      if (find_column_index(*schema, assign.column) < 0) {
        result.success = false;
        result.error = "no such column: " + assign.column;
        return result;
      }
    }

    for (auto& [old_key, row] : find_matches(*schema, stmt.has_where ? &stmt.where : nullptr)) {
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
      if (pk_changed) store_.remove(old_key);
      store_.put(new_key, encode_row(row));
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

    for (auto& [key, row] : find_matches(*schema, stmt.has_where ? &stmt.where : nullptr)) {
      (void)row;
      if (store_.remove(key)) result.rows_affected++;
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
      const int64_t key = pack_key(schema.table_id, where->value.int_val);
      std::string raw;
      if (store_.get(key, raw)) result.emplace_back(key, decode_row(raw));
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
    const int64_t lo = pack_key(schema.table_id, lo_pk);
    const int64_t hi = pack_key(schema.table_id, hi_pk);
    for (auto& [key, raw] : store_.range_scan(lo, hi)) {
      std::vector<Value> row = decode_row(raw);
      if (filter && !matches(schema, row, *filter)) continue;
      results.emplace_back(key, std::move(row));
    }
    return results;
  }

  int find_column_index(const TableSchema& schema, const std::string& name) {
    for (size_t i = 0; i < schema.columns.size(); i++) {
      if (schema.columns[i].name == name) return static_cast<int>(i);
    }
    return -1;
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
