"""Exporter contract tests. All observations here are synthetic test fixtures."""

import contextlib
import csv
import io
import json
import pathlib
import tempfile
import unittest

from export_ios_benchmark import export


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
            with contextlib.redirect_stdout(io.StringIO()):
                export(device, peer, root / "synthetic")
            text = (root / "synthetic-raw.csv").read_text()
            self.assertNotIn("must-not-export", text)
            rows = list(csv.DictReader(io.StringIO(text)))
            self.assertEqual(len(rows), 99)
            self.assertTrue(all(r["jank_frames"] == "N/A" for r in rows))
            self.assertEqual(sum(r["warmup"] == "False" for r in rows), 90)

    def test_incomplete_session_cannot_create_performance_files(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            device, peer, records = self.fixture(root)
            device.write_text("\n".join("IOS_BENCHMARK " + json.dumps(r) for r in records[:-1]))
            with self.assertRaisesRegex(AssertionError, "Incomplete session"):
                export(device, peer, root / "synthetic")
            self.assertFalse((root / "synthetic-raw.csv").exists())

    def test_protocol_mismatch_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            device, peer, _ = self.fixture(root)
            peer.write_text(peer.read_text().replace('"http_version":"2.0"', '"http_version":"1.1"', 1))
            with self.assertRaises(AssertionError):
                export(device, peer, root / "synthetic")


if __name__ == "__main__":
    unittest.main()
