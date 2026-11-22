# 65,536-source stress test

With 65,536 active sources submitting sparse rank-list replacements, RankPatch’s default `Index` took **1.78 µs p50 and 3.02 µs p99**. At p50 the production `SteadyIndex` took 4.27 µs and the benchmark heap took 4.34 µs. When the stream repeatedly withdrew tied leaders, the default index repeatedly scanned the active universe. Choosing `SteadyIndex` reduced p99 from **73.24 ms to 206.93 µs** on that replay. The benchmark heap reached **149.55 µs p99** with input validity assumed. On sparse replacements, the benchmark heap also had the lowest measured p99.9.

| Update regime | Engine | p50 µs | p99 µs | p99.9 µs | Maximum µs | Timed updates/s | Peak RSS MiB |
|---|---|---:|---:|---:|---:|---:|---:|
| Sparse full replacement | RankPatch Index | 1.782 | 3.015 | 52.397 | 182.180 | 493,115 | 42.25 |
| Sparse full replacement | RankPatch SteadyIndex | 4.267 | 6.850 | 43.014 | 79.788 | 212,121 | 39.62 |
| Sparse full replacement | Benchmark indexed heap | 4.337 | 5.778 | 26.149 | 39.959 | 223,615 | 44.25 |
| Tied leader withdrawal | RankPatch Index | 574.349 | 73,243.116 | 193,564.532 | 359,690.774 | 78 | 49.50 |
| Tied leader withdrawal | RankPatch SteadyIndex | 7.581 | 206.927 | 421.514 | 5,417.775 | 51,411 | 45.50 |
| Tied leader withdrawal | Benchmark indexed heap | 11.227 | 149.551 | 272.764 | 346.643 | 56,721 | 50.12 |

## Replay

Each engine starts with 65,536 logical sources, 16 documents per source, 1,048,576 possible dense document IDs and a requested top 10. One thread in one Linux process simulates all the writers. Each engine ingests the entire initial state and reads the winners before timing 4,096 updates. The results cover local fusion latency. Network fan-in, serialization, backpressure and concurrent writes require separate measurements.

- **Sparse full replacements** use a fixed MT19937-64 seed of 20261122. The generator selects uniform unique document IDs per ballot and one uniform source per update. At zero-based rank `i`, positional points equal the integer quotient `1000000 / (61 + i)`.
- **Tied leader withdrawals** initialize source `s` with IDs `16*s` through `16*s+15` and use one point at every rank. All documents initially tie, so ascending ID determines their order. Events clear sources 0 through 4095 in order. The shutdown repeatedly removes leaders and drains the candidate set. The production `SteadyIndex` maintains a heap of runners-up and returns the sorted winners directly. It avoids full-universe refill scans. Its withdrawal p50 was 7.58 µs, versus 11.23 µs for the benchmark heap. The benchmark heap had lower p99, p99.9 and higher timed throughput.
- **Correctness** uses independent processes for both production strategies, the benchmark heap and the oracle. Every event produces a 64-bit hash over the ordered IDs and scores. The three implementations agreed on all 8,192 replay events, comprising 24,576 timed engine snapshots. A dense full rebuild and partial sort agreed at event 1 and every 256th event in both regimes, for 34 checkpoints. Hash comparison has a theoretical collision risk.
- **Measurement** uses one trial per engine and scenario, unpinned in a shared environment. For each event, two steady-clock reads bracket the accepted revision and materialization of top 10. Hash and output work run outside the interval. Throughput divides event count by the sum of the measured durations. Quantiles use nearest rank. At 4,096 events, p99.9 is the fifth-largest measurement. Long-tail values are sensitive to scheduling noise. Peak RSS comes from Linux `getrusage(RUSAGE_SELF)` over each independent engine process, including initialization and runtime overhead. Oracle memory is excluded.

## Reproduce on Linux

Run from the repository root. The release measurements used GCC 14.2.0 (Debian 14.2.0-19), Linux 6.18.57 x86-64 and an AMD EPYC 9V74 reporting nine available logical CPUs. For replacements the runner tests Index, benchmark heap, SteadyIndex, then oracle. For withdrawals it reverses the three timed engines and again runs the oracle last.

```sh
g++ -std=c++17 -O3 -DNDEBUG -Wall -Wextra -Wpedantic bench/stress/stress.cpp -o /tmp/rankpatch-stress
python3 bench/stress/run.py /tmp/rankpatch-stress /tmp/rankpatch-stress-results
```

[Raw per-event timings and hashes](events.csv.gz) · [Summary in machine-readable form](results.json) · [Input and result hashes](SHA256SUMS)
