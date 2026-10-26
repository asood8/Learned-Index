These are the CSV files the benchmarks appended to before October 26,
2026. Each file collects rows from many runs, from the earliest phases of
the project up to the run in `../runs/2026-10-26_1522/`, and nothing in a
row says which run it came from, so they're kept only as a record. In
`engine.csv`, some metric names contain commas and weren't quoted, so
those rows have extra columns (so does the "Phase 2 model, dense array"
row in `db_lookups.csv` and `db_memory.csv`). In
`phase7b_pgm_comparison.csv`, the header overwrote the first row: the
program had two streams open on the file, and the one holding the header
flushed last. `phase3_inserts.csv` has the insert timings from before
the insert fixes described in the main README.

Since then, `scripts/run_benchmarks.sh` writes each run's CSVs into that
run's own directory under `../runs/`, next to its text output, with a
header row and quoted fields.
