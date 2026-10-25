// tlx's B+-tree as a baseline for the benchmarks (the headers are in
// cpp/third_party/tlx; build with -Icpp/third_party/tlx, which the
// Makefile passes). bplus_tree.h is a B+-tree I wrote myself and never
// tuned, so beating it says little. tlx is a maintained library: it's
// the successor of the stx B+-tree that the SOSD learned-index benchmark
// uses.
//
// Two versions: tlx's default 256-byte nodes, and 512-byte nodes, which
// were about its fastest setting for lookups on this data (16- to
// 256-slot nodes were tried). Both count the memory they allocate.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <utility>

#include <tlx/container/btree_set.hpp>

// tlx keeps its node types private, so memory is counted through the
// allocator, by allocation size. Leaves and inner nodes are different
// sizes, and get_stats() says how many of each there are. Each tree type
// gets its own counters through Tag.
template <typename Tag>
struct TlxAllocBytes {
  static inline std::map<size_t, size_t> by_size;  // allocation size -> bytes currently allocated
};

template <typename T, typename Tag>
struct TlxCountingAlloc {
  using value_type = T;
  template <typename U>
  struct rebind {
    using other = TlxCountingAlloc<U, Tag>;
  };
  TlxCountingAlloc() = default;
  template <typename U>
  TlxCountingAlloc(const TlxCountingAlloc<U, Tag>&) {}
  T* allocate(size_t n) {
    TlxAllocBytes<Tag>::by_size[sizeof(T)] += n * sizeof(T);
    return std::allocator<T>().allocate(n);
  }
  void deallocate(T* p, size_t n) {
    TlxAllocBytes<Tag>::by_size[sizeof(T)] -= n * sizeof(T);
    std::allocator<T>().deallocate(p, n);
  }
  template <typename U>
  bool operator==(const TlxCountingAlloc<U, Tag>&) const { return true; }
  template <typename U>
  bool operator!=(const TlxCountingAlloc<U, Tag>&) const { return false; }
};

struct TlxDefaultNodes {};
struct TlxWideNodes {
  static const bool self_verify = false;
  static const bool debug = false;
  static const int leaf_slots = 64;  // 64 eight-byte keys: 512-byte nodes
  static const int inner_slots = 64;
  static const size_t binsearch_threshold = 256;
};

using TlxDefault = tlx::btree_set<int64_t, std::less<int64_t>, tlx::btree_default_traits<int64_t, int64_t>,
                                  TlxCountingAlloc<int64_t, TlxDefaultNodes>>;
using TlxWide = tlx::btree_set<int64_t, std::less<int64_t>, TlxWideNodes, TlxCountingAlloc<int64_t, TlxWideNodes>>;

// {total bytes, bytes in inner nodes} that a tlx tree has allocated. Its
// Tag has to be the one its type was declared with, and the counters are
// per type, so this is only right while one tree of that type exists.
template <typename Tag, typename Tree>
std::pair<size_t, size_t> tlx_bytes(const Tree& tree) {
  size_t total = 0, inner = 0;
  const size_t inner_nodes = tree.get_stats().inner_nodes;
  for (const auto& [size, bytes] : TlxAllocBytes<Tag>::by_size) {
    total += bytes;
    if (inner_nodes > 0 && bytes == size * inner_nodes) inner += bytes;
  }
  return {total, inner};
}
