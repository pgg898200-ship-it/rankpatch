"""Compare two released Python packages on identical Borda scores over SciFact runs."""
from collections import Counter, defaultdict
from dataclasses import dataclass
import hashlib
import importlib.metadata
import json
from pathlib import Path
import platform
import statistics
import sys
from time import perf_counter_ns

import rankops
import rankpatch

FIXTURE = Path(__file__).resolve().parents[2] / "replay" / "scifact.txt"
HASH = "2c01f62b83a0e38e782298fa85d63f179495ad132abd485e8574bd91f24a6891"
ASC = list(range(1, 33))
DESC = ASC[::-1]
K = 10
PACKAGES = {
    "rankpatch": {
        "version": "0.1.0", "file": "rankpatch-0.1.0.tar.gz",
        "sha256": "6ea94931937e3714e662647b8744789d758c7224393e1cabeebeb33a814eee57",
        "url": "https://github.com/pgg898200-ship-it/rankpatch/releases/download/v0.1.0/rankpatch-0.1.0.tar.gz",
    },
    "rankops": {
        "version": "0.1.26", "file": "rankops-0.1.26-cp312-cp312-manylinux2014_x86_64.manylinux_2_17_x86_64.whl",
        "sha256": "e2b4cfb11bb274c6165f93c2b4c204a769635935a6c3d2a4fc44c1e841c312f5",
        "url": "https://files.pythonhosted.org/packages/69/2a/f538d97153c0a403793060faf88aeffd6e3f54fad81e4a048752fa708baa/rankops-0.1.26-cp312-cp312-manylinux2014_x86_64.manylinux_2_17_x86_64.whl",
    },
}


@dataclass
class Event:
    source: int
    revision: int
    ids: list
    pairs: list


@dataclass
class Query:
    sources: int
    external: list
    events: list
    weights: list
    reverse: bool
    cold_strings: bool = False


def make_cases():
    data = FIXTURE.read_bytes()
    if hashlib.sha256(data).hexdigest() != HASH:
        raise ValueError("SciFact fixture hash differs")
    queries = defaultdict(dict)
    for line in data.decode("ascii").splitlines():
        query, method, *ids = line.split()
        queries[query][method] = list(map(int, ids))
    cases = defaultdict(list)
    for runs in queries.values():
        universe = sorted({doc for ids in runs.values() for doc in ids})
        ids_by_doc = {doc: i for i, doc in enumerate(universe)}
        external = [f"{doc:010d}" for doc in universe]
        def event(method, source, revision, length):
            ids = [ids_by_doc[doc] for doc in runs[method][:length]]
            # Fixed-width strings encode the same ascending ID order under lexical sort.
            return Event(source, revision, ids, [(external[doc], float(length-i)) for i, doc in enumerate(ids)])
        prog = [event(method, s, length, length) for length in [4,8,16,32]
                for s,method in enumerate(runs)]
        ref = [event(method,i%3,1+i//3,32) for i,method in enumerate(
            ["bm25", "cosine_int8", "hamming", "bm25-hamming", "cosine_float", "hamming-cosine_float"])]
        cases["progressive"].append(Query(6,external,prog,ASC,True))
        cases["refinement"].append(Query(3,external,ref,DESC,False))
        cases["one merge (6 lists)"].append(Query(6,external,prog[-6:],DESC,False))
        cases["one merge (3 lists)"].append(Query(3,external,ref[-3:],DESC,False))
        cases["one merge (6 lists, cold strings)"].append(Query(6,external,prog[-6:],DESC,False,True))
        cases["one merge (3 lists, cold strings)"].append(Query(3,external,ref[-3:],DESC,False,True))
    return cases


def patch(query, one_merge, strategy, visit=None):
    if query.cold_strings:
        # Both methods start with the same upstream (string ID, score) rows.
        # RankPatch pays to intern IDs in tie order and translate them back.
        keys = sorted({doc for event in query.events for doc, _ in event.pairs})
        intern = {doc: i for i, doc in enumerate(keys)}
        index = rankpatch.Index(query.sources,len(keys),query.weights,K,strategy=strategy)
        for e in query.events:
            index.replace(e.source,e.revision,[intern[doc] for doc,_ in e.pairs])
        rows = [(keys[i],v) for i,v in index.top()]
        if visit: visit(query.events[-1],rows)
        return
    index = rankpatch.Index(query.sources,len(query.external),query.weights,K,strategy=strategy)
    for e in query.events:
        ids = e.ids[::-1] if query.reverse else e.ids
        index.replace(e.source,e.revision,ids)
        if not one_merge:
            result = index.top()
            if visit: visit(e, result)
    if one_merge:
        result = index.top()
        if visit: visit(query.events[-1], result)


def batch(query, one_merge, canonical, visit=None):
    active = [[] for _ in range(query.sources)]
    def read():
        if not canonical:
            return rankops.borda_multi(active, K)
        rows = rankops.borda_multi(active)
        return sorted(rows,key=lambda r:(-r[1],r[0]))[:K]
    for e in query.events:
        active[e.source] = e.pairs
        if not one_merge:
            result = read()
            if visit: visit(e,result)
    if one_merge:
        result = read()
        if visit: visit(query.events[-1],result)


def check(query, one_merge):
    active = [[] for _ in range(query.sources)]
    expected = []
    for i, event in enumerate(query.events):
        active[event.source] = event.ids
        if one_merge and i+1 < len(query.events):
            continue
        sums = Counter()
        for docs in active:
            for rank,doc in enumerate(docs):
                sums[doc] += len(docs)-rank
        expected.append((sums,sorted(sums.items(),key=lambda r:(-r[1],r[0]))[:K]))

    raw_exact = 0
    for engine,mode in [(patch,'buffered'),(patch,'steady'),(batch,False),(batch,True)]:
        returned=[]
        engine(query,one_merge,mode,lambda _,rows: returned.append(rows))
        if len(returned) != len(expected):
            raise AssertionError("Snapshot count differs")
        mapping = {doc:i for i,doc in enumerate(query.external)}
        for (scores,canonical),rows in zip(expected,returned):
            # Convert IDs only for verification. Float Borda scores are exact
            # integers at the tested scales and are compared without rounding.
            rows = [(mapping[doc] if isinstance(doc,str) else doc, score) for doc,score in rows]
            if engine == batch and not mode:
                if ([s for d,s in rows] != [s for d,s in canonical]
                    or len({d for d,s in rows}) != len(rows)
                    or any(scores.get(d) != s for d,s in rows)):
                    raise AssertionError("Raw native top-k is invalid even allowing score ties")
                raw_exact += (rows == canonical)
            elif rows != canonical:
                raise AssertionError("Canonical ID/score results differ")
    return len(expected),raw_exact


def main():
    for name, release in PACKAGES.items():
        if importlib.metadata.version(name) != release["version"]:
            raise RuntimeError(f"Expected {name} {release['version']}")
    cases = make_cases()
    methods = [("buffered",patch,'buffered'),("steady",patch,'steady'),
               ("rankops native",batch,False),("rankops canonical",batch,True)]
    report = {"python":sys.version,"platform":platform.platform(),"fixture_sha256":HASH,
              "packages":PACKAGES,"units":"ns per emitted snapshot", "cases":{}}
    for name,queries in cases.items():
        one_merge = name.startswith("one merge")
        validation = [check(q,one_merge) for q in queries]
        reads = sum(x for x,y in validation)
        exact = sum(y for x,y in validation)
        samples = {m:[] for m,_,_ in methods}
        for iteration in range(-4,31):
            for offset in range(len(methods)):
                m,engine,mode = methods[(iteration+offset)%len(methods)]
                start = perf_counter_ns()
                for q in queries:
                    engine(q,one_merge,mode)
                elapsed = perf_counter_ns()-start
                if iteration>=0: samples[m].append(elapsed/reads)
        report["cases"][name] = {"queries":len(queries),"snapshots":reads,"rankops_native_exact":exact,
                                 "samples":samples,"medians":{m:statistics.median(v) for m,v in samples.items()}}
    print(json.dumps(report,indent=2))


if __name__ == "__main__":
    main()
