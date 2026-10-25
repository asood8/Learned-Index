# Paper

`main.tex` is the preprint, in IEEE's conference format, with its
references in `refs.bib`. Build it with

```bash
pdflatex main && bibtex main && pdflatex main && pdflatex main
```

or upload both files to Overleaf, which has the `IEEEtran` class built
in. Anything still to be filled in is printed in red (the `\tbd`
command); right now that's only the Zenodo DOI of the tagged code
release.

Every number in it comes from the benchmark runs in
`results/runs/2026-10-25_1112/` and `results/runs/2026-10-25_1145/`,
which `scripts/run_benchmarks.sh` produces. If the code changes, rerun
the script and update the numbers from the new run rather than mixing
runs.
