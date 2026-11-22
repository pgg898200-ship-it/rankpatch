# Public retrieval-run replay

The fixture contains 3,072 observed document ranks from six SciFact runs across 16 queries. Document identifiers and order come from the published runs. Two simulated delivery schedules turn the lists into 480 score updates. Each update is followed by a top-10 read. The verifier checks every ID and integer score, including ties, against two C++ score-array references, an indexed-heap implementation and optionally SQLite.

## Reproduce

Run from the project root. The fixture is included, so the benchmark runs offline.

```sh
g++ -std=c++17 -O3 -DNDEBUG replay/replay.cpp -o /tmp/rankpatch-replay
/tmp/rankpatch-replay replay/scifact.txt
```

With SQLite development headers and library installed, include the indexed SQL comparator.

```sh
g++ -std=c++17 -O3 -DNDEBUG -DRANKPATCH_REPLAY_SQLITE replay/replay.cpp -lsqlite3 -o /tmp/rankpatch-replay
/tmp/rankpatch-replay replay/scifact.txt
```

Each query uses Borda points 32 down to 1 and a 10-document output, with ties resolved by ascending numeric document ID. All methods receive compact IDs assigned from the union for that query, retaining the original tie order. This gives the dense baselines a universe of just 46–86 IDs.

* **Progressive** models six retrievers publishing prefixes of lengths 4, 8, 16 and 32, in the same source order at each stage. This produces 384 updates.
* **Refinement** starts with BM25, int8 cosine and binary Hamming lists. Those three source slots then receive the published BM25-to-Hamming rerank, float cosine and Hamming-to-float rerank respectively. This produces 96 updates. The choice and ordering of these replacements is part of the simulation.

## Measurements

The table gives median nanoseconds per replace and ordered top-10 read, including consumption of all returned scores. Values are medians of three launch medians on a shared AMD EPYC 9V74 x86-64 host pinned to one logical CPU. Each launch rotated benchmark order across six warmups and 75 measured samples per method. A sample averages every timed update in the given schedule. The individual launch medians, hardware, flags and source hashes are in [measurement.json](measurement.json).

| Schedule | RankPatch | Dense rebuild | Dense delta | Indexed heap | Indexed SQLite 3.46.1 |
|---|---:|---:|---:|---:|---:|
| Progressive | 286.5 | 820.1 | 788.7 | 203.2 | 11,391.7 |
| Refinement | 862.9 | 1,369.5 | 1,486.6 | 888.9 | 32,152.9 |

The indexed heap won all three progressive runs. On refinement, the lead changed hands between launches. The 3% difference between launch medians gives little reason to choose one implementation over the other for this workload.

Dense rebuild sums all current source lists. Dense delta changes only modified positions. Both use preallocated ranking storage and C++ partial sort. The indexed heap keeps the winners in sorted order and tracks all remaining candidates in a mutable heap. It skips positional matches on replacement and uses the known-valid input directly. The SQLite comparator coalesces score changes in C++, issues prepared updates only for net-changed IDs, and reads results via a covering partial index in one open transaction on an in-memory database. It incurs SQL execution and index-maintenance overhead. Construction, candidate-ID mapping, database setup, retrieval itself, networking and commits are outside timing for every method. The measurement compares reducer cost for short, monotonic, single-writer traces. It does not measure distributed latency, relevance quality, real arrival times or request latency percentiles.

## Data and attribution

The included [scifact.txt](scifact.txt) is an adaptation of the SciFact ranked runs in Timo Breuer's *SQLite is Enough. Lexical, Semantic, and Hybrid Search with scrydb* (2026). Those run artifacts are distributed under [Creative Commons Attribution 4.0 International](https://creativecommons.org/licenses/by/4.0/). The adapted fixture retains that license. We extracted the first 32 document IDs per method, discarded scores and run columns, and selected the first 16 numeric query IDs with complete ranks in all six downloaded ranges. Source citation and pinned revision are available at [the dataset card](https://huggingface.co/datasets/breuert/scrydb-eval/blob/ea5a009b6b0af4f2d164e95abca057090210db77/README.md) and [the paper](https://arxiv.org/abs/2608.24060).

The upstream evaluation uses [SciFact](https://aclanthology.org/2020.emnlp-main.609/) via [BEIR](https://arxiv.org/abs/2104.08663) and [Qwen3 Embedding](https://arxiv.org/abs/2506.05176). Six methods share a corpus and may share rankings. There are no document or query texts in this fixture. Results apply to this deterministic subset, which is selected by availability near the start of the run files.

To regenerate the fixture, run `python3 replay/fetch.py`. It fetches one 1 MiB range from each of six immutable public files. It validates range boundaries, per-file SHA-256 digests and the resulting fixture hash against [provenance.json](provenance.json).
