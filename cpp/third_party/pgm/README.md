# Vendored: PGM-index

Source: https://github.com/gvinciguerra/PGM-index (Apache 2.0, see LICENSE
in this directory). Copied in unmodified for Phase 7b's benchmark
comparison — this project's own segmented index (`segmented_model.h`)
is a from-scratch, simplified reimplementation of the same core idea;
these files are the real, original, published implementation, kept
separate for an honest side-by-side comparison rather than grading our
own homework.

Only `pgm_index.hpp` and its one dependency, `piecewise_linear_model.hpp`,
are included -- the rest of the upstream repo (tests, benchmarks, other
index variants) wasn't needed.
