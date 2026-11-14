"""Validate a complete device session and export public, allowlisted evidence.

Usage: python3 example/tool/export_ios_benchmark.py DEVICE_LOG SERVER_LOG OUTPUT_PREFIX
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
        assert key not in seen
        seen.add(key)
        assert r["error"] is None and r["status"] == 200
        expected_protocol = "1.1" if r["preset"] == "io_http1" else "2.0"
        assert r["http_version"] == expected_protocol
        peer = server[key]
        assert peer["http_version"] == expected_protocol and peer["status"] == 200
        if expected_protocol == "2.0":
            assert peer["alpn"] == "h2", "HTTP/2 requires server-observed h2 ALPN"
        assert int(peer["response_bytes"]) == r["response_bytes"]
        assert r["response_bytes"] >= r["size_kib"] * 1024
        sizes.setdefault(r["size_kib"], r["response_bytes"])
        assert sizes[r["size_kib"]] == r["response_bytes"], "Unequal payloads"
        assert all(isinstance(r[k], (int, float)) and math.isfinite(r[k]) and r[k] >= 0
                   for k in ("network_ms", "parse_ms", "request_ms"))

    trial_fields = ["source_sha", "mode", "scenario", "preset", "trial", "warmup", "position",
                    "total_ms", "rps", "success", "failure", "http_version", "response_bytes",
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
               "alpn", "status", "response_bytes"], server.values())
    summary = []
    for s in ("scenario_a", "scenario_b", "scenario_c"):
        for p in ("io_http1", "dio_http2", "turbo_fetch"):
            group = [r for r in public_trials if r["scenario"] == s and r["preset"] == p and r["trial"] >= 0]
            assert len(group) == 10
            for metric in trial_fields[7:]:
                values = [r[metric] for r in group if isinstance(r[metric], (int, float))]
                if metric in ("http_version", "success", "failure", "rss_samples"):
                    continue
                summary.append({"scenario": s, "preset": p, "metric": metric,
                                "n": len(values), "mean": statistics.mean(values) if values else None,
                                "median": statistics.median(values) if values else None,
                                "sample_stddev": statistics.stdev(values) if len(values) > 1 else None,
                                "min": min(values) if values else None, "max": max(values) if values else None})
    write_csv(prefix.with_name(prefix.name + "-summary.csv"),
              ["scenario", "preset", "metric", "n", "mean", "median", "sample_stddev", "min", "max"], summary)
    print(json.dumps({"measured_trials": 90, "warmup_trials": 9, "measured_requests": 8700,
                      "source_sha": start["source_sha"], "utc": start["utc"], "payload_bytes": sizes}, indent=2))


def write_csv(path, fields, rows):
    with path.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields, extrasaction="ignore", lineterminator="\n")
        writer.writeheader()
        for row in rows:
            writer.writerow({key: "N/A" if row.get(key) is None else row[key] for key in fields})


if __name__ == "__main__":
    export(*sys.argv[1:])
