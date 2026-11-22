# Update-and-read microbenchmark

On this CPU, replacing a 32-item ballot among 1,024 sources drawing from 131,072 document IDs took **742 ns at p50 and 1,482 ns at p99** with RankPatch. The indexed heap took **2,003 ns and 3,475 ns** on the same replays. At four sources, rebuilding from scratch had the lowest p50 in three of six scenarios.

## Reproduce

Run these commands from the project root. The output contains all 216 implementation/trial/scenario rows, with seeds and correctness checksums.

```sh
g++ -std=c++17 -O3 -DNDEBUG -Wall -Wextra -Wpedantic bench/bench.cpp -o /tmp/rankpatch-bench
/tmp/rankpatch-bench --events 2048 --repeats 3 --seed 20261122 > /tmp/rankpatch-results.csv
```

The released run used GCC 14.2.0 (Debian 14.2.0-19) on Linux 6.18.57 x86-64, in a shared environment reporting an AMD EPYC 9V74. One unpinned process ran the implementations sequentially with the default allocator. The allowed CPU list was 0–8. CPU frequency and other workloads were uncontrolled. Small differences, particularly 921 versus 922 ns, are within run-to-run variability. [Raw measurements](results.csv).

## Workload and measurement

- Each source starts with 32 distinct IDs sampled uniformly from the universe. High overlap uses 96 IDs across all sources. Dilute overlap uses 128 IDs per source in a shared universe. Each update chooses one source uniformly. The emitted top 10 orders documents by descending score and ascending ID. The score at zero-based rank `i` is the integer quotient `1000000 / (61 + i)`.
- **Swap** exchanges two adjacent positions selected uniformly. **Replace** generates 32 fresh distinct IDs. **Retract/restore** toggles a source between an empty ballot and 32 freshly sampled IDs. Each accepted revision advances its source's revision. All updates and expected snapshots are prepared before timing. Each implementation in a trial sees the same stream.
- Each engine first ingests the initial ballots and materializes top 10. Timings start after this warm-up. The harness measures one replacement and retrieval of the ordered top 10 with two `std::chrono::steady_clock` calls. It then compares every ID and score to a complete rebuild and updates a result checksum outside the timed interval. All **442,368** timed snapshots matched. Snapshot comparison, initial indexing, query generation, format conversion, and network transport are excluded.
- Each trial reports nearest-rank p50 and p99 in nanoseconds. Timed operations/second equals the number of updates divided by the sum of their measured durations. Individual samples include timer overhead. Each scenario has three distinct seeds, and implementation order rotates. The table shows the median of the three **trial-level** statistics. “Closest / RP” divides the lowest alternative p50 by RankPatch p50. A value above 1 favors RankPatch.
- RankPatch validates input ballots. The benchmark baselines assume that the generated IDs are valid and unique. The full rebuild uses a dense accumulator, clears only active IDs and partial-sorts the top 10. The ordered set accumulates net changes, skips unchanged positions and reinserts extracted tree nodes. The indexed heap keeps its winners sorted and maintains the remaining active IDs in a position-indexed max heap. All four implementations compile into one C++17 executable. [Harness](bench.cpp) · [Indexed heap](indexed.hpp).

## Full matrix

RankPatch has the lowest observed p50 in 14 of 18 scenarios, the lowest p99 in 9 of 18 and the highest timed operation rate in 11 of 18. The full rebuild and indexed heap win the remaining four p50 comparisons. In the 1,024-source dilute replacement scenario, the rate was 1.18 million timed updates and reads per second. The indexed heap reached 0.32 million. For 1,024 high-overlap sources under retractions/restores, the heap had the lower p99 (1,032 vs 1,162 ns).

| Sources | Overlap | Update | RP p50 ns | Heap p50 ns | Set p50 ns | Rebuild p50 ns | RP p99 ns | Closest / RP | Lowest observed p50 |
|---:|---|---|---:|---:|---:|---:|---:|---:|---|
| 4 | dilute | swap | 50 | 100 | 271 | 551 | 701 | 2.00× | RankPatch |
| 4 | dilute | replace | 981 | 1,212 | 3,385 | 851 | 1,622 | 0.87× | Full rebuild |
| 4 | dilute | retract/restore | 571 | 681 | 1,492 | 431 | 1,071 | 0.75× | Full rebuild |
| 4 | high | swap | 60 | 100 | 241 | 521 | 391 | 1.67× | RankPatch |
| 4 | high | replace | 1,062 | 1,482 | 3,816 | 1,191 | 1,423 | 1.12× | RankPatch |
| 4 | high | retract/restore | 751 | 881 | 2,103 | 571 | 1,262 | 0.76× | Full rebuild |
| 64 | dilute | swap | 50 | 100 | 341 | 5,028 | 311 | 2.00× | RankPatch |
| 64 | dilute | replace | 891 | 1,142 | 5,238 | 5,949 | 1,412 | 1.28× | RankPatch |
| 64 | dilute | retract/restore | 631 | 851 | 2,674 | 4,387 | 1,132 | 1.35× | RankPatch |
| 64 | high | swap | 60 | 90 | 240 | 2,274 | 231 | 1.50× | RankPatch |
| 64 | high | replace | 1,242 | 1,372 | 4,748 | 3,475 | 1,613 | 1.10× | RankPatch |
| 64 | high | retract/restore | 941 | 811 | 2,844 | 2,383 | 1,623 | 0.86× | Indexed heap |
| 1,024 | dilute | swap | 70 | 140 | 931 | 108,490 | 401 | 2.00× | RankPatch |
| 1,024 | dilute | replace | 742 | 2,003 | 11,697 | 114,009 | 1,482 | 2.70× | RankPatch |
| 1,024 | dilute | retract/restore | 510 | 1,382 | 6,911 | 79,558 | 1,222 | 2.71× | RankPatch |
| 1,024 | high | swap | 60 | 100 | 240 | 30,325 | 221 | 1.67× | RankPatch |
| 1,024 | high | replace | 921 | 922 | 3,855 | 29,053 | 1,301 | 1.00× | RankPatch |
| 1,024 | high | retract/restore | 571 | 691 | 2,624 | 28,983 | 1,162 | 1.21× | RankPatch |

## Coverage

These streams isolate sensitivity to source count, edit shape and overlap. The [SciFact replay](../replay/README.md) exercises retrieved document IDs from six published runs. The timed synthetic runs are single-threaded, in-process measurements over dense integer IDs. Workloads with different universe sizes, hot-ID distributions, larger winners lists, frequent guard depletion or multiple writers need their own measurements.

## SHA-256 of measured inputs and output

```text
254df3175e3b94f516a3e9df39ae3d6a055158b02ee48331f5c794eeea491015  bench/bench.cpp
5de5703b0aa9dc28677382bf1364607bc388f6562e5e4945c80a9185561b8be2  bench/indexed.hpp
632dcc2a304f82e2284de6f39f37d4c9eb7d9d0f49ef9a29e564c73e0ee28596  include/rankpatch.hpp
b6dfb24db47b4ef0fd06f905b8ae402b63b86f7de56d675ae23783e1d83208af  bench/results.csv
```
