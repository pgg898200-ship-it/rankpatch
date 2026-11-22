"""Run independent Linux processes and retain every timed observation."""
import argparse
import csv
import gzip
import io
import json
import math
from pathlib import Path
import statistics
import subprocess


def run(exe, scenario, engine):
    proc = subprocess.run([exe, engine, scenario], check=True, capture_output=True, text=True)
    rows = list(csv.DictReader(io.StringIO(proc.stdout)))
    return rows, int(proc.stderr.strip())


def summarize(rows, rss):
    times = sorted(int(row["ns"]) for row in rows)
    def percentile(p):
        return times[math.ceil(p * len(times)) - 1]
    return dict(samples=len(rows), p50_ns=percentile(.5), p99_ns=percentile(.99),
                p999_ns=percentile(.999), max_ns=max(times),
                timed_ops_per_second=round(1e9/statistics.mean(times), 2), peak_rss_kib=rss)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("executable", type=Path)
    parser.add_argument("output_dir", type=Path)
    args = parser.parse_args()
    exe = str(args.executable.resolve())
    args.output_dir.mkdir(parents=True, exist_ok=True)
    data = io.StringIO()
    writer = csv.writer(data, lineterminator="\n")
    writer.writerow(["scenario", "engine", "event", "ns", "hash"])
    result = {"sources": 65536, "depth": 16, "universe": 1048576, "k": 10,
              "seed": 20261122, "scenarios": {}}
    for scenario in ["replace", "withdraw"]:
        samples = {}
        metrics = {}
        order = ["rankpatch", "heap", "steady"] if scenario == "replace" else ["steady", "heap", "rankpatch"]
        for engine in order + ["oracle"]:
            rows, rss = run(exe, scenario, engine)
            for row in rows:
                writer.writerow([scenario, engine, row["event"], row["ns"], row["hash"]])
            samples[engine] = {int(row["event"]): row["hash"] for row in rows}
            if engine != "oracle":
                metrics[engine] = summarize(rows, rss)
        expected = set(range(1, 4097))
        if samples["rankpatch"].keys() != expected or any(
                samples[key] != samples["rankpatch"] for key in ("heap", "steady")):
            raise ValueError("per-event hash divergence")
        if samples["oracle"].keys() != ({1} | set(range(256, 4097, 256))):
            raise ValueError("oracle checkpoint missing")
        if any(samples["rankpatch"][i] != h for i,h in samples["oracle"].items()):
            raise ValueError("full rebuild disagreement")
        metrics["full_rebuild_checkpoints"] = len(samples["oracle"])
        result["scenarios"][scenario] = metrics
    raw = data.getvalue().encode("utf-8")
    (args.output_dir / "events.csv.gz").write_bytes(gzip.compress(raw, mtime=0))
    (args.output_dir / "results.json").write_text(json.dumps(result, indent=2) + "\n")


if __name__ == "__main__":
    main()
