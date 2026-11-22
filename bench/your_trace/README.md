# Choose a strategy with your revision trace

Measure the time your Python caller spends replacing a worker's ranked list and reading the winners. Each event runs through the buffered index, the steady index, and a Python implementation that rebuilds all scores and sorts every active document. The tool checks the applied flag and the entire ordered output after every event. A mismatch stops the run with a line number.

From a checkout with RankPatch installed, replay the bundled example.

```sh
python bench/your_trace/compare.py
```

The fixture replays seven updates from three workers, including one stale revision and one withdrawal. Three measured passes give 21 timing samples per strategy. Use the example to check the tool and a longer trace to estimate p99. [Live search example](../../examples/live_search.py)

## Replay a local file

Each JSON line supplies a bounded integer source, an unsigned 64-bit revision and an array of unique document IDs in ranked order. Map document IDs to dense integers from zero to `universe - 1`. Every fresh update replaces one worker's entire list. An empty list clears it. Equal and older revisions leave that source unchanged. Blank lines are skipped.

```sh
python bench/your_trace/compare.py events.jsonl \
  --sources 1024 --universe 131072 --rank-points 10,6,3,1 --k 10 \
  --repeats 6 > timings.json
```

The JSON report contains the SHA-256 of the original file bytes, event counts, configuration, package and Python versions, machine architecture, and p50/p99 nanoseconds per strategy. Higher-confidence comparisons use longer representative traces, multiple runs on an idle machine, and the same trace hash. For a tail-latency objective, compare the two native strategies' p99; use p50 for the common case. The Python rebuild provides a correctness oracle and a simple application baseline.

## Measurement scope

One warmup and each measured pass start with empty indexes. Event order is preserved. Strategy execution order rotates across passes. Timing covers calls through the Python interface and building the Python result list. File parsing, input checks, the oracle comparisons, initial index allocations, and upstream retrieval are outside the timed interval. Measurements pool events from all measured passes, including stale events and the first insertion. Serial, interleaved calls measure single-process latency. They do not measure concurrency, network latency, or server throughput.

The reader streams the input once per pass and retains no history of past results. Current ballots and native dense indexes consume memory according to the supplied bounds. The timer retains three arrays of measurements and sorts one at a time. The limit is one million event samples per strategy across measured passes and four MiB per input line. The exhaustive Python oracle can dominate replay time for large active populations. Everything runs locally. Reports contain no document IDs, source IDs, or input file paths.
