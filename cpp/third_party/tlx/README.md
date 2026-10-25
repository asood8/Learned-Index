# Vendored: tlx B+-tree

Source: https://github.com/tlx/tlx at commit 2dd63ab (2025-01-10), Boost
Software License 1.0 (see LICENSE in this directory). Copied in
unmodified.

`cpp/bplus_tree.h` is a B+-tree I wrote myself, so beating it doesn't
say much. tlx's B+-tree is a maintained library (it's the successor of
the stx B+-tree that the SOSD learned-index benchmark uses), so the
benchmarks compare against it as well.

Only the B+-tree headers (`btree.hpp`, `btree_map.hpp`, `btree_set.hpp`)
and the one header they include, `die/core.hpp`, are here. Programs that
use them need `-Icpp/third_party/tlx`, which the Makefile passes.
