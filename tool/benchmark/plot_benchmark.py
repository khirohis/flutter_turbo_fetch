"""Plot measured trials only. Requires matplotlib; never substitutes N/A with zero.

Usage: python tool/benchmark/plot_benchmark.py RAW_CSV OUTPUT_PNG
"""

import csv
import pathlib
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt


def plot(raw_path, output_path):
    with pathlib.Path(raw_path).open(newline="") as stream:
        rows = [r for r in csv.DictReader(stream) if r["warmup"] == "False"]
    assert len(rows) == 90, "Requires a validated complete session"
    presets = ["io_http1", "dio_http2", "turbo_fetch"]
    labels = ["IO / H1", "Dart H2", "TurboFetch"]
    colors = ["#64748b", "#d97706", "#0891b2"]
    fig, axes = plt.subplots(2, 3, figsize=(12, 7))
    for column, scenario in enumerate(["scenario_a", "scenario_b", "scenario_c"]):
        for row_index, (metric, scale, ylabel) in enumerate([
            ("total_ms", 1, "Total wall time (ms)"),
            ("rss_peak_bytes", 1024 ** 2, "Sampled peak process RSS (MiB)"),
        ]):
            ax = axes[row_index, column]
            groups = []
            for preset in presets:
                group = [float(r[metric]) / scale for r in rows
                         if r["scenario"] == scenario and r["preset"] == preset]
                assert len(group) == 10
                groups.append(group)
            boxes = ax.boxplot(groups, tick_labels=labels, patch_artist=True,
                               showfliers=False, medianprops={"color": "black"})
            for index, (group, color, box) in enumerate(zip(groups, colors, boxes["boxes"])):
                box.set_facecolor(color)
                box.set_alpha(.35)
                # Deterministic small offsets reveal every observed trial.
                offsets = [(i - 4.5) * .025 for i in range(10)]
                ax.scatter([index + 1 + x for x in offsets], group, c=color,
                           s=15, zorder=3)
            ax.set_title(scenario.replace("scenario_", "Scenario ").upper())
            ax.set_ylabel(ylabel)
            ax.grid(axis="y", alpha=.25)
            ax.set_ylim(0, max(max(group) for group in groups) * 1.1)
    fig.suptitle("Benchmark Comparison (Release)\n"
                 "10 trials per adapter/scenario; warmups excluded; split-pipeline protocol")
    fig.tight_layout()
    output_path = pathlib.Path(output_path)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(output_path, dpi=200)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(__doc__.strip(), file=sys.stderr)
        sys.exit(2)
    plot(sys.argv[1], sys.argv[2])
