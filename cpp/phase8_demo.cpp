// Phase 8 demo: creates two tables with different schemas, inserts
// real rows into each, verifies they decode back correctly, then
// forces the same kind of real restart Phase 5's demo used (destroy
// everything, rebuild from the same WAL file) to prove both the
// catalog itself and every table's row data survive intact.
#include <cstdio>
#include <cstdlib>
#include <string>

#include "catalog.h"
#include "durable_store.h"
#include "row_format.h"
#include "table_key.h"

int main() {
  const std::string wal_path = "results/phase8_demo.wal";
  DurableStore::destroy(wal_path);

  int32_t users_id, products_id;

  {
    DurableStore store(wal_path, 0.7, 64);
    Catalog catalog(store);

    users_id = catalog.create_table("users", {{"id", ColumnType::INT64},
                                               {"name", ColumnType::TEXT},
                                               {"age", ColumnType::INT64}});
    products_id = catalog.create_table("products", {{"id", ColumnType::INT64},
                                                      {"name", ColumnType::TEXT},
                                                      {"price_cents", ColumnType::INT64}});
    std::printf("created tables: users=%d, products=%d\n", users_id, products_id);

    for (int64_t i = 1; i <= 100; i++) {
      std::vector<Value> row = {Value::make_int(i), Value::make_text("user_" + std::to_string(i)),
                                 Value::make_int(20 + (i % 50))};
      store.put(pack_key(users_id, i), encode_row(row));
    }
    for (int64_t i = 1; i <= 50; i++) {
      std::vector<Value> row = {Value::make_int(i), Value::make_text("product_" + std::to_string(i)),
                                 Value::make_int(500 + i * 10)};
      store.put(pack_key(products_id, i), encode_row(row));
    }
    std::printf("inserted 100 users + 50 products\n");

    // spot-check one row from each table before the simulated restart
    std::string raw;
    store.get(pack_key(users_id, 42), raw);
    std::vector<Value> decoded = decode_row(raw);
    std::printf("users row 42 (before restart): id=%lld name=%s age=%lld\n",
                static_cast<long long>(decoded[0].int_val), decoded[1].text_val.c_str(),
                static_cast<long long>(decoded[2].int_val));
  }

  std::printf("\n-- simulated restart: rebuilding store AND catalog from the same WAL --\n");
  {
    DurableStore store(wal_path, 0.7, 64);
    Catalog catalog(store);

    bool ok = true;
    if (catalog.table_count() != 2) {
      std::fprintf(stderr, "FAIL: expected 2 tables, got %zu\n", catalog.table_count());
      ok = false;
    }
    const TableSchema* users_schema = catalog.get_table("users");
    const TableSchema* products_schema = catalog.get_table("products");
    if (!users_schema || users_schema->columns.size() != 3) {
      std::fprintf(stderr, "FAIL: users schema not recovered correctly\n");
      ok = false;
    }
    if (!products_schema || products_schema->columns.size() != 3) {
      std::fprintf(stderr, "FAIL: products schema not recovered correctly\n");
      ok = false;
    }
    std::printf("catalog recovered: %zu tables (users: %zu cols, products: %zu cols)\n",
                catalog.table_count(), users_schema ? users_schema->columns.size() : 0,
                products_schema ? products_schema->columns.size() : 0);

    for (int64_t i = 1; i <= 100 && ok; i++) {
      std::string raw;
      if (!store.get(pack_key(users_schema->table_id, i), raw)) {
        std::fprintf(stderr, "FAIL: users row %lld missing after restart\n", static_cast<long long>(i));
        ok = false;
        continue;
      }
      std::vector<Value> decoded = decode_row(raw);
      if (decoded[0].int_val != i || decoded[1].text_val != "user_" + std::to_string(i) ||
          decoded[2].int_val != 20 + (i % 50)) {
        std::fprintf(stderr, "FAIL: users row %lld decoded incorrectly after restart\n",
                     static_cast<long long>(i));
        ok = false;
      }
    }
    for (int64_t i = 1; i <= 50 && ok; i++) {
      std::string raw;
      if (!store.get(pack_key(products_schema->table_id, i), raw)) {
        std::fprintf(stderr, "FAIL: products row %lld missing after restart\n", static_cast<long long>(i));
        ok = false;
        continue;
      }
      std::vector<Value> decoded = decode_row(raw);
      if (decoded[0].int_val != i || decoded[1].text_val != "product_" + std::to_string(i) ||
          decoded[2].int_val != 500 + i * 10) {
        std::fprintf(stderr, "FAIL: products row %lld decoded incorrectly after restart\n",
                     static_cast<long long>(i));
        ok = false;
      }
    }

    if (!ok) {
      std::fprintf(stderr, "\nPHASE 8 CHECK FAILED\n");
      return 1;
    }
    std::printf("all 150 rows across both tables decoded correctly after restart\n");
  }

  std::printf("\nPhase 8 check passed: catalog + all table rows survive a real restart\n");
  return 0;
}
