// A minimal but real in-memory B+-tree: internal nodes hold routing
// keys only, all (key, value) pairs live in leaves, and leaves are
// linked left-to-right. This is the classic structure real databases
// use for indexes — it's the thing Phase 1+ is trying to beat, so it
// needs to be correct and reasonably representative, not a toy.
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

class BPlusTree {
 public:
  static constexpr int ORDER = 64;  // max keys per node before it splits

  BPlusTree() { root_ = new Node(true); }

  // Returns true and sets out_value if key is present.
  bool search(int64_t key, int64_t& out_value) const {
    Node* leaf = find_leaf(key);
    auto it = std::lower_bound(leaf->keys.begin(), leaf->keys.end(), key);
    if (it != leaf->keys.end() && *it == key) {
      out_value = leaf->values[it - leaf->keys.begin()];
      return true;
    }
    return false;
  }

  void insert(int64_t key, int64_t value) {
    std::vector<Node*> path;  // ancestors from root down to the leaf
    Node* node = root_;
    while (!node->is_leaf) {
      path.push_back(node);
      int i = std::upper_bound(node->keys.begin(), node->keys.end(), key) -
              node->keys.begin();
      node = node->children[i];
    }

    insert_into_leaf(node, key, value);
    if (static_cast<int>(node->keys.size()) > ORDER) {
      split_leaf_and_propagate(node, path);
    }
  }

  size_t size() const { return count_; }

  // Walks the whole tree, summing each node's actual vector
  // capacities plus struct overhead. This counts real allocated
  // memory (not just live element count), but doesn't account for
  // the memory allocator's own internal bookkeeping per allocation --
  // a reasonable, clearly-labeled approximation, not a claim of exact
  // byte-for-byte process memory usage.
  size_t memory_bytes() const { return node_memory_bytes(root_); }

 private:
  struct Node {
    bool is_leaf;
    std::vector<int64_t> keys;
    std::vector<Node*> children;  // internal only: children.size() == keys.size()+1
    std::vector<int64_t> values;  // leaf only: parallel to keys
    Node* next = nullptr;         // leaf only: link to next leaf
    explicit Node(bool leaf) : is_leaf(leaf) {}
  };

  size_t node_memory_bytes(const Node* node) const {
    size_t bytes = sizeof(Node);
    bytes += node->keys.capacity() * sizeof(int64_t);
    if (node->is_leaf) {
      bytes += node->values.capacity() * sizeof(int64_t);
    } else {
      bytes += node->children.capacity() * sizeof(Node*);
      for (const Node* child : node->children) bytes += node_memory_bytes(child);
    }
    return bytes;
  }

  Node* root_;
  size_t count_ = 0;

  Node* find_leaf(int64_t key) const {
    Node* node = root_;
    while (!node->is_leaf) {
      int i = std::upper_bound(node->keys.begin(), node->keys.end(), key) -
              node->keys.begin();
      node = node->children[i];
    }
    return node;
  }

  void insert_into_leaf(Node* leaf, int64_t key, int64_t value) {
    auto it = std::lower_bound(leaf->keys.begin(), leaf->keys.end(), key);
    size_t idx = it - leaf->keys.begin();
    if (it != leaf->keys.end() && *it == key) {
      leaf->values[idx] = value;  // key already present: overwrite
      return;
    }
    leaf->keys.insert(leaf->keys.begin() + idx, key);
    leaf->values.insert(leaf->values.begin() + idx, value);
    count_++;
  }

  void split_leaf_and_propagate(Node* leaf, std::vector<Node*>& path) {
    size_t mid = leaf->keys.size() / 2;
    Node* new_leaf = new Node(true);
    new_leaf->keys.assign(leaf->keys.begin() + mid, leaf->keys.end());
    new_leaf->values.assign(leaf->values.begin() + mid, leaf->values.end());
    leaf->keys.resize(mid);
    leaf->values.resize(mid);
    new_leaf->next = leaf->next;
    leaf->next = new_leaf;

    int64_t up_key = new_leaf->keys.front();
    Node* right = new_leaf;

    // walk back up toward the root, inserting the new separator key
    // into each parent, splitting that parent too if it overflows
    for (int level = static_cast<int>(path.size()) - 1; level >= 0; --level) {
      Node* parent = path[level];
      int i = std::upper_bound(parent->keys.begin(), parent->keys.end(), up_key) -
              parent->keys.begin();
      parent->keys.insert(parent->keys.begin() + i, up_key);
      parent->children.insert(parent->children.begin() + i + 1, right);

      if (static_cast<int>(parent->keys.size()) <= ORDER) return;  // done

      size_t pmid = parent->keys.size() / 2;
      up_key = parent->keys[pmid];

      Node* new_internal = new Node(false);
      new_internal->keys.assign(parent->keys.begin() + pmid + 1, parent->keys.end());
      new_internal->children.assign(parent->children.begin() + pmid + 1,
                                     parent->children.end());
      parent->keys.resize(pmid);
      parent->children.resize(pmid + 1);

      right = new_internal;
    }

    // the root itself split: grow the tree by one level
    Node* new_root = new Node(false);
    new_root->keys.push_back(up_key);
    new_root->children.push_back(root_);
    new_root->children.push_back(right);
    root_ = new_root;
  }
};
