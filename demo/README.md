---
title: RankPatch
emoji: 📊
colorFrom: blue
colorTo: green
sdk: static
app_file: index.html
license: apache-2.0
short_description: Exact top-k after search workers revise their ranked lists
tags:
- information-retrieval
- rank-fusion
- streaming
- cpp
---
# RankPatch

Edit a retriever’s ranked list, send a revision, or withdraw it. The demo shows which sources contributed each point and exports a runnable C++17 example for the state on screen. It runs locally in your browser using a JavaScript reference implementation.

The native library is a single header with a CPython binding. Across 1,024 simulated sources with low overlap, replacing one 32-item list and reading the top ten measured 742 ns median versus 2,003 ns for an indexed heap. The heap was faster on the progressive SciFact workload. These are native kernel measurements on one host.

[Install RankPatch](https://github.com/pgg898200-ship-it/rankpatch) · [Compare all workloads](https://github.com/pgg898200-ship-it/rankpatch/tree/main/bench) · [Reproduce the SciFact experiment](https://github.com/pgg898200-ship-it/rankpatch/tree/main/replay)
