"""Render report tables from validated trial CSVs using Python's standard library.

Usage: python3 example/tool/summarize_ios_benchmark.py RAW_CSV
"""

import csv
import statistics
import sys


def render(path):
    with open(path, newline="") as stream:
        rows = [r for r in csv.DictReader(stream) if r["warmup"] == "False"]
    assert len(rows) == 90
    groups = []
    for scenario in ("scenario_a", "scenario_b", "scenario_c"):
        for preset, label in (("io_http1", "IO / HTTP/1.1"),
                              ("dio_http2", "dio_http2"),
                              ("turbo_fetch", "TurboFetch")):
            group = [r for r in rows if r["scenario"] == scenario and r["preset"] == preset]
            assert len(group) == 10
            groups.append((scenario[-1].upper(), label, group))

    def median(group, field):
        return statistics.median(float(r[field]) for r in group)

    print("## Total time and throughput\n")
    print("All times are milliseconds. Each row summarizes ten measured trials. "
          "RPS is the median of per-trial successful requests divided by trial wall time.\n")
    print("| Scenario | Adapter | Median total | Mean total | Sample SD | Min | Max | Median req/s |")
    print("| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |")
    for scenario, label, group in groups:
        values = [float(r["total_ms"]) for r in group]
        print(f"| {scenario} | {label} | {statistics.median(values):.2f} | "
              f"{statistics.mean(values):.2f} | {statistics.stdev(values):.2f} | "
              f"{min(values):.2f} | {max(values):.2f} | {median(group, 'rps'):.2f} |")
    print("\n## Network and JSON transformation\n")
    print("Milliseconds; each cell is the median across ten trials of that trial's "
          "request mean/P50/P95. These are not percentiles pooled across all requests. "
          "Network ends at full-body reception; Parse includes transformer scheduling, "
          "stream consolidation, decoding, and applicable Isolate/semaphore overhead.\n")
    print("| Scenario | Adapter | Network avg / P50 / P95 | Parse avg / P50 / P95 |")
    print("| --- | --- | ---: | ---: |")
    for scenario, label, group in groups:
        cells = [" / ".join(f"{median(group, f'{kind}_{stat}_ms'):.3f}"
                            for stat in ("avg", "p50", "p95"))
                 for kind in ("network", "parse")]
        print(f"| {scenario} | {label} | {' | '.join(cells)} |")
    print("\n## Memory and frame observations\n")
    print("RSS values are medians of ten per-trial observations in MiB. "
          "The 25 ms RSS sampler can miss short peaks; RSS is process-wide. "
          "Jank Frames and Max Frame Duration are **N/A for every run**: "
          "this protocol did not instrument frames or perform a separate Profile study.\n")
    print("| Scenario | Adapter | Sampled peak RSS | RSS delta |")
    print("| --- | --- | ---: | ---: |")
    for scenario, label, group in groups:
        print(f"| {scenario} | {label} | {median(group, 'rss_peak_bytes') / 1048576:.2f} | "
              f"{median(group, 'rss_delta_bytes') / 1048576:.2f} |")


if __name__ == "__main__":
    render(*sys.argv[1:])
