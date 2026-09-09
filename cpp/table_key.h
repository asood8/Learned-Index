// Phase 8: the storage engine only knows one thing -- int64 -> bytes.
// To get tables out of it, every row's key packs a table ID into the
// high bits and the row's primary key into the low bits. This keeps
// every table's rows contiguous and sorted together (Phase 2's
// segments already specialize on exactly this kind of contiguous
// range), and it's the same trick used to fit multiple logical
// streams into one physical keyspace elsewhere in systems design.
//
// Layout: 16 bits table ID (up to 65,536 tables) + 48 bits primary
// key. Real constraint, stated plainly: primary keys must be
// non-negative and fit in 48 bits (~281 trillion values) -- plenty
// for auto-increment-style IDs, not a claim of supporting arbitrary
// 64-bit primary keys.
#pragma once

#include <cstdint>

constexpr int64_t PRIMARY_KEY_BITS = 48;
constexpr int64_t PRIMARY_KEY_MASK = (int64_t{1} << PRIMARY_KEY_BITS) - 1;

constexpr int64_t pack_key(int32_t table_id, int64_t primary_key) {
  return (static_cast<int64_t>(table_id) << PRIMARY_KEY_BITS) | (primary_key & PRIMARY_KEY_MASK);
}

constexpr int32_t unpack_table_id(int64_t packed) {
  return static_cast<int32_t>(static_cast<uint64_t>(packed) >> PRIMARY_KEY_BITS);
}

constexpr int64_t unpack_primary_key(int64_t packed) { return packed & PRIMARY_KEY_MASK; }
