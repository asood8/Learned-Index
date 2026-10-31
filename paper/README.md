# Paper

`main.tex` is the write-up of this project, in IEEE's conference format, with
its references in `refs.bib`. It hasn't been peer reviewed. Build it with

```bash
pdflatex main && bibtex main && pdflatex main && pdflatex main
```

or upload both files to Overleaf, which has the `IEEEtran` class built
in. `learned_index.pdf` is the compiled copy, so rebuild it whenever
`main.tex` changes.

Its results come from two benchmark runs, one per machine, in
`results/runs/2026-10-31_0201/` (laptop) and
`results/runs/2026-10-31_0514/` (AWS), which `scripts/run_benchmarks.sh`
produces. The process-to-process experiment is in `results/experiments/`.
If the code changes, rerun the script on both machines and update the
numbers from the new runs rather than mixing runs.
