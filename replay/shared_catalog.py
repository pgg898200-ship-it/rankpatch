"""Measure catalog reuse across queries on the bundled SciFact rankings."""
from collections import Counter, defaultdict
from pathlib import Path
from statistics import median
from time import perf_counter_ns

from rankpatch.catalog import CatalogFusion, DocumentCatalog


def replay(queries, documents, shared):
    catalog = DocumentCatalog(documents) if shared else documents
    snapshots = []
    for runs in queries:
        fusion = CatalogFusion(sources=runs, documents=catalog, rank_points=range(32, 0, -1), k=10)
        for source, ids in runs.items():
            fusion.replace(source, 0, ids)
        snapshots.append(fusion.top())
    return snapshots


def main():
    queries = defaultdict(dict)
    documents = {}
    for line in Path(__file__).with_name("scifact.txt").read_text().splitlines():
        query, source, *ids = line.split()
        queries[query][source] = ids
        documents.update((key, key) for key in ids)
    print(f"{len(documents)} string IDs across {len(queries)} queries")
    for count in (1, len(queries)):
        batch = list(queries.values())[:count]
        expected = []
        for runs in batch:
            scores = Counter()
            for ids in runs.values():
                scores.update({key: 32 - i for i, key in enumerate(ids)})
            expected.append(sorted(scores.items(), key=lambda row: (-row[1], row[0]))[:10])

        samples = {False: [], True: []}
        for trial in range(32):
            order = (False, True) if trial % 2 == 0 else (True, False)
            for shared in order:
                start = perf_counter_ns()
                snapshots = replay(batch, documents, shared)
                elapsed = perf_counter_ns() - start
                actual = [[(hit.id, hit.score) for hit in hits] for hits in snapshots]
                if actual != expected:
                    raise ValueError("top ten differs from full score rebuild")
                del snapshots
                if trial >= 2:
                    samples[shared].append(elapsed / (1000 * count))
        print(f"{count:2} queries, median us/query: "
              f"mapping {median(samples[False]):.2f}, shared {median(samples[True]):.2f}")


if __name__ == "__main__":
    main()
