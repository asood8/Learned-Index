"""Summarizes engine_benchmark runs: the median of each metric over the
rounds, for each index, and the ratio between them.

usage: python3 scripts/summarize_engine.py results/runs/<run>/

Reads every engine_*.txt in that directory (written by
scripts/run_benchmarks.sh) and prints a Markdown table per dataset,
which it also saves as engine_summary.md in the same directory.
"""

import re
import statistics
import sys
from collections import defaultdict
from pathlib import Path

HEADER = re.compile(r"^== (?P<index>.+?), (?P<data>\S+): ")
LINE = re.compile(r"^\s+(?P<metric>.+?)\s+(?P<value>-?[\d.]+) (?P<unit>\S+)$")


def main() -> None:
    run_dir = Path(sys.argv[1])
    # values[data][metric][index] = [one value per round]
    values = defaultdict(lambda: defaultdict(lambda: defaultdict(list)))
    order = defaultdict(list)  # metrics in the order they're printed
    for path in sorted(run_dir.glob("engine_*.txt")):
        index = data = None
        for line in path.read_text().splitlines():
            m = HEADER.match(line)
            if m:
                index, data = m["index"], m["data"]
                continue
            m = LINE.match(line)
            if m and index:
                metric = m["metric"].strip()
                values[data][metric][index].append(float(m["value"]))
                if metric not in order[data]:
                    order[data].append(metric)

    out = []
    for data, metrics in values.items():
        indexes = sorted({i for per in metrics.values() for i in per}, key=lambda i: not i.startswith("learned"))
        rounds = max(len(v) for per in metrics.values() for v in per.values())
        runs = "1 run each" if rounds == 1 else f"median of {rounds} runs each, with the lowest and highest"
        out.append(f"### {data} ({runs})\n")
        out.append("| | " + " | ".join(indexes) + " | learned / B+-tree |")
        out.append("|---" * (len(indexes) + 2) + "|")
        for metric in order[data]:
            med = {i: statistics.median(metrics[metric][i]) for i in indexes if metrics[metric][i]}

            def cell(i):
                if i not in med:
                    return ""
                v = metrics[metric][i]
                spread = f" ({min(v):,.0f}–{max(v):,.0f})" if len(v) > 1 else ""
                return f"{med[i]:,.1f}{spread}"

            cells = [cell(i) for i in indexes]
            learned = next((med[i] for i in med if i.startswith("learned")), None)
            btree = next((med[i] for i in med if i.startswith("B+")), None)
            ratio = f"{learned / btree:.2f}" if learned and btree else ""
            out.append(f"| {metric} | " + " | ".join(cells) + f" | {ratio} |")
        out.append("")
    text = "\n".join(out)
    print(text)
    (run_dir / "engine_summary.md").write_text(text + "\n")


if __name__ == "__main__":
    main()
