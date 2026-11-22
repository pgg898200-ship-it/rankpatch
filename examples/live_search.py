"""Fuse revisions from concurrent search workers, or feed live updates on stdin.

After installing RankPatch, run:

    python examples/live_search.py
    echo '{"source":0,"revision":1,"ids":[4,6]}' | python examples/live_search.py --stdin

Each input is one JSON object on its own line. IDs form the source's complete
replacement list; [] retracts it. Each output line includes the current top-k
as [document, score] pairs, including for ignored revisions. Send EOF to exit.
"""
import argparse
import asyncio
import json
import sys

from rankpatch import Index


def emit(index, source, revision, ids):
    applied = index.replace(source=source, revision=revision, ids=ids)
    print(json.dumps({
        "source": source,
        "revision": revision,
        "applied": applied,
        "top": index.top(),
    }, separators=(",", ":")), flush=True)


async def demo():
    # All producers run on one event loop and call the index without an await
    # between replace() and top(). Each emitted snapshot includes that update.
    index = Index(sources=3, universe=16, rank_points=[5, 3, 1], k=3)

    async def retriever(source, responses):
        for delay, revision, ids in responses:
            await asyncio.sleep(delay)
            emit(index, source, revision, ids)

    await asyncio.gather(
        # A reranker improves its order; a delayed first-page replay follows.
        retriever(0, [(0.00, 1, [2, 5, 7]), (0.04, 2, [5, 2, 7]), (0.03, 1, [2, 5, 7])]),
        # A worker invalidates its earlier result.
        retriever(1, [(0.01, 0, [2, 8]), (0.04, 1, [])]),
        # A second page extends the source's candidate list.
        retriever(2, [(0.02, 7, [7, 5]), (0.04, 8, [7, 5, 9])]),
    )
    expected = [(5, 8), (7, 6), (2, 3)]
    if index.top() != expected:
        raise RuntimeError(f"demo ended with {index.top()}, expected {expected}")


def replay(lines, index):
    for line_no, line in enumerate(lines, start=1):
        if not line.strip():
            continue
        try:
            event = json.loads(line)
            if not isinstance(event, dict):
                raise ValueError("expected a JSON object")
            ids = event["ids"]
            if not isinstance(ids, list):
                raise ValueError("ids must be a JSON array")
            emit(index, event["source"], event["revision"], ids)
        except (KeyError, TypeError, ValueError, OverflowError, IndexError) as exc:
            raise ValueError(f"line {line_no}: {exc}") from exc


def rank_points(value):
    try:
        points = [int(part) for part in value.split(",")]
    except ValueError:
        raise argparse.ArgumentTypeError("expected comma-separated positive integers") from None
    if any(point < 1 or point >= 2**32 for point in points):
        raise argparse.ArgumentTypeError("rank points must be positive uint32 values")
    return points


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--stdin", action="store_true", help="consume versioned source lists as JSON Lines")
    parser.add_argument("--sources", type=int, help="number of sources (stdin default 3)")
    parser.add_argument("--universe", type=int, help="exclusive document ID limit (stdin default 16)")
    parser.add_argument("--rank-points", type=rank_points, help="points per position (stdin default 5,3,1)")
    parser.add_argument("--k", type=int, help="number of winners (stdin default 3)")
    args = parser.parse_args()
    if not args.stdin:
        if any(v is not None for v in (args.sources, args.universe, args.rank_points, args.k)):
            parser.error("use --stdin with index configuration options")
        asyncio.run(demo())
        return

    try:
        index = Index(
            sources=args.sources if args.sources is not None else 3,
            universe=args.universe if args.universe is not None else 16,
            rank_points=args.rank_points if args.rank_points is not None else [5, 3, 1],
            k=args.k if args.k is not None else 3,
        )
        replay(sys.stdin, index)
    except (ValueError, OverflowError, IndexError) as exc:
        parser.exit(2, f"live_search: {exc}\n")


if __name__ == "__main__":
    main()
