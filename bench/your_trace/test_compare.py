import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from compare import Rebuild, run


HERE = Path(__file__).resolve().parent


class TraceTests(unittest.TestCase):
    def config(self, path, **changes):
        cfg = dict(trace=path, sources=3, universe=16, rank_points=[5, 3, 1], k=3, repeats=2)
        cfg.update(changes)
        return argparse.Namespace(**cfg)

    def test_example(self):
        report = run(self.config(HERE / "example.jsonl"))
        self.assertEqual(report["trace"]["events"], 7)
        self.assertEqual(report["trace"]["stale"], 1)
        self.assertEqual(report["trace"]["accepted_empty"], 1)
        self.assertEqual(len(report["trace"]["sha256"]), 64)
        for result in report["results"].values():
            self.assertEqual(result["samples"], 14)
            self.assertGreaterEqual(result["p99_ns"], result["p50_ns"])

    def test_cli_bundled_example(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = subprocess.check_output(
                [sys.executable, HERE / "compare.py"], cwd=tmp, text=True
            )
        report = json.loads(output)
        self.assertEqual(report["trace"]["events"], 7)
        self.assertEqual(report["measured_passes"], 3)
        for result in report["results"].values():
            self.assertEqual(result["samples"], 21)

    def test_cli_custom_trace_needs_bounds(self):
        result = subprocess.run(
            [sys.executable, HERE / "compare.py", HERE / "example.jsonl", "--k", "2"],
            capture_output=True, text=True,
        )
        self.assertEqual(result.returncode, 2)
        self.assertIn("--sources, --universe, --rank-points", result.stderr)

    def test_ties_stale_retract(self):
        oracle = Rebuild([5], 2)
        self.assertTrue(oracle.replace(0, 0, [7]))
        self.assertTrue(oracle.replace(1, 1, [4]))
        self.assertEqual(oracle.top(), [(4, 5), (7, 5)])
        self.assertFalse(oracle.replace(1, 1, []))
        self.assertTrue(oracle.replace(1, 2, []))
        self.assertEqual(oracle.top(), [(7, 5)])

    def test_stale_payload_and_errors(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "events.jsonl"
            first = '{"source":0,"revision":1,"ids":[2]}\n'
            path.write_text(first + '{"source":0,"revision":0,"ids":[99,99]}\n')
            self.assertEqual(run(self.config(path))["trace"]["stale"], 1)
            for bad in ("not json", "[]", '{"revision":2,"ids":[3]}',
                        '{"source":2,"revision":0,"ids":[1,1]}',
                        '{"source":0,"revision":2,"ids":[99]}',
                        '{"source":0,"revision":2,"ids":[true]}',
                        '{"source":3,"revision":2,"ids":[]}',
                        '{"source":0,"revision":-1,"ids":[]}',
                        '{"source":0,"revision":2,"ids":[0,1,2,3]}'):
                path.write_text(first + bad)
                with self.subTest(bad=bad), self.assertRaisesRegex(ValueError, "line 2"):
                    run(self.config(path))


if __name__ == "__main__":
    unittest.main()
