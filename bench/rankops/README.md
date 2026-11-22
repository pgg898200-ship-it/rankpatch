# Python RankPatch and rankops on public SciFact rankings

We compared the released RankPatch 0.1.0 CPython extension and the published rankops 0.1.26 Rust wheel on the same 16 SciFact queries. With integer document IDs already available, the steady strategy took 1.50 µs per progressive update and read, compared with 10.93 µs for `rankops.borda_multi(lists, 10)`. When starting a single fusion from string IDs, native rankops won on both three-list and six-list inputs.

## Results

Median microseconds per emitted snapshot. Each entry is the median of three launch medians. Every timed live replay includes a new index per query, every replacement, the Python/native calls, Python result objects and top-10 reads. A one-merge operation includes a fresh index plus all insertions and one read. Imports and package initialization have already completed.

| Input and output schedule | Snapshots per replay | RankPatch buffered | RankPatch steady | rankops native top 10 | rankops canonical top 10 |
|---|---:|---:|---:|---:|---:|
| Progressive prefixes, read each revision | 384 | 1.79 | 1.50 | 10.93 | 14.24 |
| Coarse-to-refined lists, read each revision | 96 | 2.84 | 2.63 | 15.77 | 26.38 |
| One merge of six final lists, IDs prepared | 16 | 9.39 | 11.42 | 35.10 | 52.10 |
| One merge of three final lists, IDs prepared | 16 | 4.18 | 4.58 | 13.06 | 19.82 |
| One merge of six final lists, start with string IDs | 16 | 27.09 | 30.37 | 25.60 | 35.00 |
| One merge of three final lists, start with string IDs | 16 | 19.55 | 19.76 | 18.43 | 27.94 |

For prepared inputs, each library receives its native payload. RankPatch takes dense integer IDs and rankops takes string-ID/score pairs. Query-specific compression, fixed-width string formatting and score-tuple construction happen before timing. In the cold-string cases both receive the same prebuilt string-ID/score pairs. RankPatch pays to identify distinct IDs, sort them into tie order, build a dictionary, translate every list and translate the output back. This models a new application query without a reusable ID map. Native rankops was faster in all three cold-string launches for both source counts. The canonical adapter costs extra when the caller requires one specific tie ordering.

## Same fusion scores

The oracle sums `length - zero_based_position` for every document in every active list, following rankops Borda semantics. The progressive schedule changes list lengths, so RankPatch reverses each incoming prefix **inside the timed loop** and applies increasing weights 1 through 32. All refinement and one-merge inputs have length 32, so ordinary decreasing weights apply. The native C++ replay elsewhere uses a fixed 32-point first rank throughout, a separate scoring convention.

The rankops fast path calls its native `borda_multi(active, 10)` without postprocessing. It can select or order tied IDs differently from RankPatch. Every result had the oracle's top ten score sequence, distinct valid IDs and the correct score for every returned ID. Native order and membership exactly matched the oracle on 251 of 384 progressive snapshots and 60 of 96 refinement snapshots in this run. To obtain the same canonical top 10 even when a tie straddles the cutoff, the rankops adapter asks for the complete ranking, sorts by score descending then ID ascending and takes ten. Both RankPatch strategies and the canonical adapter matched the exact ordered IDs and scores on every snapshot.

The ranked lists contain actual document IDs and order from published retrieval runs. The delivery schedule is simulated. Python's host-language conversion overhead, string versus integer keys and repeated batch fusion all contribute to this measurement. Timing provides no estimate of retrieval latency, LLM inference, network latency, or gains on a whole search stack. Corpus attribution and the reproducible source data are in [the SciFact replay](../../replay/README.md).

## Reproduce

Run from the project root with Linux x86-64, CPython 3.12 and a C++17 compiler. The lock file uses the public RankPatch GitHub source distribution and official rankops manylinux wheel, each pinned by SHA-256.

```sh
python3.12 -m venv /tmp/rankpatch-rankops
/tmp/rankpatch-rankops/bin/python -m pip install setuptools wheel
CC=gcc CXX=g++ /tmp/rankpatch-rankops/bin/python -m pip install --no-deps --no-build-isolation --require-hashes -r bench/rankops/requirements.txt
/tmp/rankpatch-rankops/bin/python bench/rankops/compare.py > /tmp/rankpatch-rankops-result.json
```

The release run used GCC 14.2 and CPython 3.12.14 on a shared x86-64 AMD EPYC 9V74 host. Three fresh Python processes were pinned to CPU 0. Each discarded four warmup replays and timed 31 complete replays per method, rotating execution order each round. A replay averages query-level work across all 16 queries and returns nanoseconds per emitted result. The warmup, small corpus and shared-host variability limit generalization. The source releases, fixture, benchmark script and raw CSV are recorded by SHA-256 in [the receipt](receipt.json). The artifact download and installation inputs are listed in [the package lock](requirements.txt). All 2,232 measured replay averages are in [samples.csv](samples.csv).
