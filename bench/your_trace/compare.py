"""Replay a local JSONL trace against both RankPatch strategies and an exhaustive oracle."""
import argparse
from array import array
import hashlib
import json
import platform
import sys
from time import perf_counter_ns

from rankpatch import Index, __version__

# Bound the timing samples and the temporary list allocated to calculate percentiles.
MAX_SAMPLES = 1_000_000
MAX_LINE = 4 * 1024 * 1024
NAMES = ("buffered", "steady", "python_rebuild")


class Rebuild:
    def __init__(self, points, k):
        self.ballots = {}
        self.points = points
        self.k = k

    def replace(self, source, revision, ids):
        old = self.ballots.get(source)
        if old is not None and revision <= old[0]:
            return False
        self.ballots[source] = revision, ids
        return True

    def top(self):
        scores = {}
        for _, ids in self.ballots.values():
            for doc, points in zip(ids, self.points):
                scores[doc] = scores.get(doc, 0) + points
        return sorted(scores.items(), key=lambda pair: (-pair[1], pair[0]))[:self.k]


def uint(value, upper, field):
    if type(value) is not int or not 0 <= value < upper:
        raise ValueError(f"{field} must be an integer in range")
    return value


def pass_once(path, config, order, timings, limit):
    engines = {name: Index(**config, strategy=name) for name in ("buffered", "steady")}
    engines["python_rebuild"] = Rebuild(config["rank_points"], config["k"])
    sha = hashlib.sha256()
    events = ignored = retractions = 0
    with open(path, "rb") as stream:
        line_no = 0
        while raw := stream.readline(MAX_LINE + 1):
            line_no += 1
            sha.update(raw)
            if len(raw) > MAX_LINE:
                raise ValueError(f"line {line_no} exceeds byte limit")
            if not raw.strip():
                continue
            if events >= limit:
                raise ValueError("trace exceeds sample limit; lower --repeats or use a smaller trace")
            try:
                event = json.loads(raw)
                if not isinstance(event, dict) or not {"source", "revision", "ids"} <= event.keys():
                    raise ValueError("expected object with source, revision, ids")
                source = uint(event["source"], config["sources"], "source")
                rev = uint(event["revision"], 2**64, "revision")
                ids = event["ids"]
                if not isinstance(ids, list):
                    raise ValueError("ids must be an array")
                # Source/revision validation precedes payload validation in RankPatch.
                # Stale arrays are passed through unchanged, even when their IDs are invalid.
                old = engines["python_rebuild"].ballots.get(source)
                if old is None or rev > old[0]:
                    if len(ids) > len(config["rank_points"]):
                        raise ValueError("ballot too long")
                    for doc in ids:
                        uint(doc, config["universe"], "document ID")
                    if len(set(ids)) != len(ids):
                        raise ValueError("duplicate document ID")
            except (ValueError, RecursionError) as exc:
                raise ValueError(f"line {line_no}: {exc}") from None
            observed = {}
            for name in order:
                engine = engines[name]
                start = perf_counter_ns()
                applied = engine.replace(source, rev, ids)
                top = engine.top()
                elapsed = perf_counter_ns() - start
                if timings is not None:
                    timings[name].append(elapsed)
                observed[name] = applied, top
            if any(observed[n] != observed["python_rebuild"] for n in ("buffered", "steady")):
                raise ValueError(f"line {line_no}: accepted flag or ordered result diverged")
            applied = observed["python_rebuild"][0]
            ignored += not applied
            retractions += applied and not ids
            events += 1
    if not events:
        raise ValueError("empty trace")
    return {"events": events, "stale": ignored, "accepted_empty": retractions,
            "sha256": sha.hexdigest()}


def points_arg(raw):
    try:
        points = [int(v) for v in raw.split(",")]
        if not all(0 < v < 2**32 for v in points):
            raise ValueError()
        return points
    except ValueError:
        raise argparse.ArgumentTypeError("expected comma-separated positive uint32 values") from None


def run(args):
    config = dict(sources=args.sources, universe=args.universe, rank_points=args.rank_points, k=args.k)
    if args.sources < 0 or not 0 <= args.universe <= 2**32 or args.k < 0:
        raise ValueError("invalid sources, universe or k")
    if args.repeats <= 0 or args.repeats > MAX_SAMPLES:
        raise ValueError("repeats out of range")
    limit = MAX_SAMPLES // args.repeats
    trace = pass_once(args.trace, config, NAMES, None, limit)
    timings = {n: array("Q") for n in NAMES}
    for trial in range(args.repeats):
        offset = trial % len(NAMES)
        order = NAMES[offset:] + NAMES[:offset]
        if trace != pass_once(args.trace, config, order, timings, limit):
            raise ValueError("trace changed between passes")
    results = {}
    for name in NAMES:
        samples = sorted(timings[name])
        n = len(samples)
        results[name] = {"samples": n, "p50_ns": samples[(n - 1) // 2],
                         "p99_ns": samples[(99 * n + 99) // 100 - 1]}
    return {"schema": 1, "timing": "serial Python replace + top per event; nearest-rank, pooled",
            "warmup_passes": 1, "measured_passes": args.repeats, "trace": trace,
            "index": config, "rankpatch_version": __version__,
            "python": platform.python_version(), "system": platform.system(),
            "machine": platform.machine(), "results": results}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", help="local JSON Lines file")
    parser.add_argument("--sources", type=int, required=True)
    parser.add_argument("--universe", type=int, required=True)
    parser.add_argument("--k", type=int, required=True)
    parser.add_argument("--rank-points", type=points_arg, required=True)
    parser.add_argument("--repeats", type=int, default=3)
    args = parser.parse_args()
    try:
        report = run(args)
    except (ValueError, IndexError, OverflowError, OSError, MemoryError) as exc:
        parser.exit(2, f"trace comparison failed: {exc}\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
