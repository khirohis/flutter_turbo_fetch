"""Validate a complete device session and export public, allowlisted evidence.

Usage: python tool/benchmark/export_benchmark.py DEVICE_LOG SERVER_LOG OUTPUT_PREFIX
"""

import csv
import json
import math
import pathlib
import statistics
import sys


def read_device(path):
    records = []
    for line in pathlib.Path(path).read_text().splitlines():
        if "IOS_BENCHMARK " in line:
            records.append(json.loads(line.split("IOS_BENCHMARK ", 1)[1]))
    return records


def export(device_path, server_path, prefix):
    records = read_device(device_path)
    assert sum(r["type"] == "session_start" for r in records) == 1, "Expected one session"
    assert sum(r["type"] == "session_complete" for r in records) == 1, "Incomplete session"
    assert not any(r["type"] == "session_failed" for r in records), "Failed session"
    start = next(r for r in records if r["type"] == "session_start")
    assert start["mode"] == "release" and len(start["source_sha"]) == 40
    trials = [r for r in records if r["type"] == "trial"]
    requests = [r for r in records if r["type"] == "request"]
    assert len(trials) == 99 and len(requests) == 9570
    keys = [(r["scenario"], r["preset"], r["trial"]) for r in trials]
    expected_keys = {(s, p, t) for s in ("scenario_a", "scenario_b", "scenario_c")
                     for p in ("io_http1", "dio_http2", "turbo_fetch") for t in range(-1, 10)}
    assert len(set(keys)) == 99 and set(keys) == expected_keys
    server = {}
    for line in pathlib.Path(server_path).read_text().splitlines():
        if not line.startswith('{"type":"server_response"'):
            continue
        row = json.loads(line)
        if row["preset"] is None:
            continue
        key = (row["scenario"], row["preset"], int(row["trial"]), int(row["sequence"]))
        assert key not in server, "Duplicate server request; use a fresh server log"
        server[key] = row
    assert len(server) == len(requests), "Server/device request counts differ"
    sizes = {}
    seen = set()
    for r in requests:
        key = (r["scenario"], r["preset"], r["trial"], r["sequence"])
        assert key not in seen, f"Duplicate request {key}"
        seen.add(key)
        peer = server[key]
        assert r["status"] == 200 and int(peer["status"]) == 200
        assert r["response_bytes"] == int(peer["response_bytes"])
        expected_version = "1.1" if r["preset"] == "io_http1" else "2.0"
        assert r["http_version"] == expected_version
        assert peer["http_version"] == expected_version
        if r["preset"] != "io_http1":
            assert peer.get("alpn") == "h2"
        sizes.setdefault((r["scenario"], r["round"], r["size_kib"]), set()).add(r["response_bytes"])
    for key, observed in sizes.items():
        assert len(observed) == 1, f"Byte size mismatch across presets for {key}: {observed}"
    trial_fields = ["scenario", "preset", "trial", "warmup", "position", "http_version",
                    "mode", "source_sha", "total_ms", "success", "failure", "rps",
                    "response_bytes",
                    "network_avg_ms", "network_p50_ms", "network_p95_ms",
                    "parse_avg_ms", "parse_p50_ms", "parse_p95_ms",
                    "rss_start_bytes", "rss_peak_bytes", "rss_delta_bytes", "rss_samples",
                    "jank_frames", "max_frame_ms"]
    public_trials = []
    for r in trials:
        subset = [q for q in requests if (q["scenario"], q["preset"], q["trial"]) ==
                  (r["scenario"], r["preset"], r["trial"])]
        count = 90 if r["scenario"] == "scenario_a" else 100
        assert len(subset) == count and r["success"] == count and r["failure"] == 0
        expected_position = (list(("io_http1", "dio_http2", "turbo_fetch")).index(r["preset"]) - max(r["trial"], 0)) % 3
        assert r["position"] == expected_position
        row = {k: r.get(k) for k in trial_fields}
        row.update(source_sha=start["source_sha"], mode="release",
                   http_version=subset[0]["http_version"],
                   response_bytes=sum(q["response_bytes"] for q in subset))
        for kind in ("network", "parse"):
            values = sorted(q[f"{kind}_ms"] for q in subset)
            computed = {"avg": statistics.mean(values), "p50": statistics.median(values),
                        "p95": values[math.ceil(.95 * len(values)) - 1]}
            for label, value in computed.items():
                row[f"{kind}_{label}_ms"] = value
        assert math.isclose(row["rps"], count * 1000 / row["total_ms"])
        public_trials.append(row)
    prefix = pathlib.Path(prefix)
    prefix.parent.mkdir(parents=True, exist_ok=True)
    write_csv(prefix.with_name(prefix.name + "-raw.csv"), trial_fields, public_trials)
    request_fields = ["scenario", "preset", "trial", "sequence", "round", "size_kib",
                      "status", "response_bytes", "http_version", "network_ms", "parse_ms", "request_ms", "error"]
    write_csv(prefix.with_name(prefix.name + "-requests.csv"), request_fields, requests)
    write_csv(prefix.with_name(prefix.name + "-server.csv"),
              ["scenario", "preset", "trial", "sequence", "size_kib", "http_version",
               "alpn", "status", "response_bytes"],
              sorted(server.values(), key=lambda r: (r["scenario"], r["preset"],
                                                    int(r["trial"]), int(r["sequence"]))))
    write_summary(prefix.with_name(prefix.name + "-summary.csv"), public_trials)


def write_csv(path, fields, rows):
    with path.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        for row in rows:
            writer.writerow({f: row.get(f) if row.get(f) is not None else "N/A" for f in fields})


def write_summary(path, trials):
    measured = [t for t in trials if not t["warmup"]]
    groups = {}
    for t in measured:
        groups.setdefault((t["scenario"], t["preset"]), []).append(t)
    numeric_fields = ["total_ms", "rps", "network_avg_ms", "network_p50_ms", "network_p95_ms",
                      "parse_avg_ms", "parse_p50_ms", "parse_p95_ms",
                      "rss_start_bytes", "rss_peak_bytes", "rss_delta_bytes"]
    rows = []
    for (scenario, preset), subset in sorted(groups.items()):
        assert len(subset) == 10
        for field in numeric_fields:
            vals = [float(item[field]) for item in subset if item.get(field) not in (None, "N/A")]
            rows.append({
                "scenario": scenario, "preset": preset, "metric": field, "n": len(vals),
                "mean": statistics.mean(vals) if vals else "N/A",
                "median": statistics.median(vals) if vals else "N/A",
                "sample_sd": statistics.stdev(vals) if len(vals) > 1 else 0.0,
                "min": min(vals) if vals else "N/A", "max": max(vals) if vals else "N/A",
            })
        for absent in ("jank_frames", "max_frame_ms"):
            rows.append({"scenario": scenario, "preset": preset, "metric": absent,
                         "n": 0, "mean": "N/A", "median": "N/A", "sample_sd": "N/A",
                         "min": "N/A", "max": "N/A"})
    fields = ["scenario", "preset", "metric", "n", "mean", "median", "sample_sd", "min", "max"]
    write_csv(path, fields, rows)


if __name__ == "__main__":
    if len(sys.argv) != 4:
        print(__doc__.strip(), file=sys.stderr)
        sys.exit(2)
    export(sys.argv[1], sys.argv[2], sys.argv[3])
