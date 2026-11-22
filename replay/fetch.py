"""Regenerate the ID-only SciFact fixture from byte ranges of six pinned public runs."""
import concurrent.futures
import hashlib
import json
from pathlib import Path
from urllib.request import Request, urlopen

HERE = Path(__file__).resolve().parent
REV = "ea5a009b6b0af4f2d164e95abca057090210db77"
BASE = "https://huggingface.co/datasets/breuert/scrydb-eval/resolve/" + REV
METHODS = ["bm25", "cosine_float", "cosine_int8", "hamming", "bm25-hamming", "hamming-cosine_float"]
DEPTH = 32
RANGE = 1024 * 1024
QUERIES = 16


def fetch(method):
    path = "runs/beir/scifact-" + method + ".txt"
    req = Request(BASE + "/" + path + "?download=true", headers={"Range": f"bytes=0-{RANGE-1}"})
    with urlopen(req, timeout=90) as resp:
        if resp.status != 206 or not resp.headers["Content-Range"].startswith(f"bytes 0-{RANGE-1}/"):
            raise RuntimeError(f"Unexpected HTTP range response for {method}")
        data = resp.read(RANGE + 1)
    if len(data) != RANGE:
        raise ValueError("Wrong number of bytes")
    runs = {}
    for line in data[: data.rfind(b"\n")].decode("ascii").splitlines():
        query, _, document, rank, score, run = line.split()
        if int(rank) <= DEPTH:
            runs.setdefault(int(query), {})[int(rank)] = int(document)
    return runs, {"path": path, "range_sha256": hashlib.sha256(data).hexdigest()}


def main():
    with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
        results = list(pool.map(fetch, METHODS))
    queries = sorted(set.intersection(*[{q for q, docs in runs.items() if len(docs) == DEPTH}
                                          for runs, _ in results]))[:QUERIES]
    if len(queries) != QUERIES:
        raise ValueError("Too few shared queries")
    rows = []
    for q in queries:
        for method, (runs, _) in zip(METHODS, results):
            ids = [runs[q][r] for r in range(1, DEPTH + 1)]
            if len(set(ids)) != DEPTH:
                raise ValueError("A source has duplicate documents")
            rows.append(f"{q} {method} " + " ".join(map(str, ids)))
    content = ("\n".join(rows) + "\n").encode("ascii")
    manifest = {"repo": "breuert/scrydb-eval", "revision": REV, "range_bytes": RANGE,
                "runs": [meta for _, meta in results], "depth": DEPTH, "queries": queries,
                "fixture_sha256": hashlib.sha256(content).hexdigest()}
    manifest_file = HERE / "provenance.json"
    if manifest_file.exists() and manifest != json.loads(manifest_file.read_text()):
        raise ValueError("Regenerated fixture differs from pinned manifest")
    manifest_file.write_text(json.dumps(manifest, indent=2) + "\n")
    (HERE / "scifact.txt").write_bytes(content)
    print(manifest["fixture_sha256"])


if __name__ == "__main__":
    main()
