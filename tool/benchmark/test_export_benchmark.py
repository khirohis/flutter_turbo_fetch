"""Exporter contract tests. All observations here are synthetic test fixtures."""

import contextlib
import csv
import io
import json
import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from export_benchmark import export


class ExportTests(unittest.TestCase):
    def fixture(self, root):
        records = [{"type": "session_start", "mode": "release", "source_sha": "a" * 40,
                    "utc": "synthetic-test-only", "private_metadata": "must-not-export"}]
        server = []
        presets = ["io_http1", "dio_http2", "turbo_fetch"]
        for scenario in ["scenario_a", "scenario_b", "scenario_c"]:
            count = 90 if scenario == "scenario_a" else 100
            for trial in range(-1, 10):
                for index, preset in enumerate(presets):
                    records.append({"type": "trial", "mode": "release", "scenario": scenario,
                                    "preset": preset, "trial": trial, "warmup": trial == -1,
                                    "position": (index - max(trial, 0)) % 3, "total_ms": 100,
                                    "rps": count * 10, "success": count, "failure": 0,
                                    "network_ms": {}, "parse_ms": {}})
                    for sequence in range(count):
                        protocol = "1.1" if preset == "io_http1" else "2.0"
                        records.append({"type": "request", "scenario": scenario, "preset": preset,
                                        "trial": trial, "sequence": sequence, "round": 0,
                                        "size_kib": 5, "status": 200, "response_bytes": 6000,
                                        "http_version": protocol, "network_ms": 1,
                                        "parse_ms": 2, "request_ms": 3, "error": None})
                        server.append({"type": "server_response", "scenario": scenario,
                                       "preset": preset, "trial": str(trial), "sequence": str(sequence),
                                       "http_version": protocol, "status": 200, "response_bytes": "6000"})
                        server[-1]["alpn"] = "h2" if protocol == "2.0" else False
        records.append({"type": "session_complete"})
        device = root / "device.log"
        peer = root / "server.log"
        device.write_text("\n".join("IOS_BENCHMARK " + json.dumps(r) for r in records))
        peer.write_text("\n".join(json.dumps(r, separators=(",", ":")) for r in server))
        return device, peer, records

    def test_complete_export_preserves_na_and_excludes_private_metadata(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            device, peer, _ = self.fixture(root)
            export(device, peer, root / "out")
            with (root / "out-summary.csv").open(newline="") as stream:
                rows = list(csv.DictReader(stream))
            jank = [r for r in rows if r["metric"] == "jank_frames"]
            self.assertTrue(all(r["n"] == "0" and r["median"] == "N/A" for r in jank))
            total = [r for r in rows if r["metric"] == "total_ms"]
            self.assertTrue(all(r["n"] == "10" and float(r["median"]) == 100.0 for r in total))
            with (root / "out-raw.csv").open(newline="") as stream:
                raw = list(csv.DictReader(stream))
            self.assertEqual(len(raw), 99)
            self.assertNotIn("private_metadata", raw[0])
            self.assertEqual(raw[0]["source_sha"], "a" * 40)

    def test_rejects_missing_server_evidence(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            device, peer, _ = self.fixture(root)
            peer.write_text("")
            with self.assertRaises(AssertionError):
                export(device, peer, root / "out")

    def test_rejects_incomplete_session(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            device, peer, _ = self.fixture(root)
            lines = [l for l in device.read_text().splitlines() if "session_complete" not in l]
            device.write_text("\n".join(lines))
            with self.assertRaises(AssertionError):
                export(device, peer, root / "out")

    def test_rejects_protocol_mismatch(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            device, peer, _ = self.fixture(root)
            bad = device.read_text().replace('"preset": "io_http1", "trial": 0, "sequence": 0, "round": 0, "size_kib": 5, "status": 200, "response_bytes": 6000, "http_version": "1.1"',
                                             '"preset": "io_http1", "trial": 0, "sequence": 0, "round": 0, "size_kib": 5, "status": 200, "response_bytes": 6000, "http_version": "2.0"')
            device.write_text(bad)
            with self.assertRaises(AssertionError):
                export(device, peer, root / "out")


if __name__ == "__main__":
    unittest.main()
