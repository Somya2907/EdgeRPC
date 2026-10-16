# Phase 3: recorded buffer-pipeline evidence

On 2026-10-16, the Linux x86_64 cloud environment ran one Release trial for each of 1, 2, and 4 registered slots, using the implementation committed in `30d316d` and `ab80ac0`. Every warm-up and measured response was verified. Each trial used 4 KiB compute tensors, four outstanding calls, one execution worker, 100 warm-ups, and 1,000 measured requests. Client preparation and server execution each included an explicit 1,000 μs simulated delay. This demonstrates scheduling/ownership with synthetic CPU work; it is not a transport speed comparison, a repeated performance study, or a hardware/NPU result.

| Slots | Verified samples | p50 (μs) | p99 (μs) | Throughput (requests/s) | Preparations overlapping another slot's execution |
|---:|---:|---:|---:|---:|---:|
| 1 | 1,000 | 9,472.698 | 11,029.821 | 416.380 | 0 |
| 2 | 1,000 | 4,705.370 | 5,943.235 | 828.767 | 996 |
| 4 | 1,000 | 4,373.474 | 5,485.128 | 897.639 | 999 |

These are the observed values for this single invocation. Cloud load and timer scheduling affect them. The four-call window includes calls waiting for slots, so individual latency includes this queueing. A two-slot example has preparation of slot 2 intersecting execution of slot 1 over monotonic interval `[2419534718697, 2419535763712)` ns, about 1,045 μs. One slot has no overlap because it cannot be filled while executing. Additional slots allowed overlap in this run, but repeated trials and real preprocessing are needed to assess any broader benefit.

The analyzer checked consistent stage ordering, strictly advancing per-slot generations, and zero cases where a reused slot's next preparation began before its preceding execution ended. The ownership FSM separately requires output copying before reuse. Unit/fake-peer tests exercise that lifecycle, reversed replies, and failure reclamation. The process integration suite adds 1,000 delayed two-slot calls and compares timestamp overlap for 1/2/4 slots. Release and ASan/UBSan both pass all five suites on this Linux host. Native Mac/ARM64 checks remain pending.

Retained artifacts from the actual run:

- [Environment/build metadata](evidence/pipeline-v0.4/metadata.json), [server log](evidence/pipeline-v0.4/server.log), and [overlap analysis](evidence/pipeline-v0.4/overlap.json).
- Raw CSVs: [1 slot](evidence/pipeline-v0.4/slots-1.csv.gz), [2 slots](evidence/pipeline-v0.4/slots-2.csv.gz), [4 slots](evidence/pipeline-v0.4/slots-4.csv.gz). Gzip compression is lossless; these include every measured row.
- Original summaries: [1 slot](evidence/pipeline-v0.4/slots-1.txt), [2 slots](evidence/pipeline-v0.4/slots-2.txt), [4 slots](evidence/pipeline-v0.4/slots-4.txt).

Reproduce the procedure from the repository root:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
./scripts/run_pipeline_benchmarks.sh
```

The script starts/cleans its own server and reports a unique directory under `results/`. For independent analysis of the retained traces, copy and decompress all three `.csv.gz` files into a temporary directory, then run `python3 scripts/analyze_pipeline.py DIRECTORY`. The analyzer expects one serialized worker. CSV stage definitions and cold/steady-state boundaries are documented in [benchmark methodology](benchmarks.md).

Phase 4 remains open: matched socket/SHM/pool comparisons, at least five trials for published comparisons, variability analysis, socket-byte instrumentation, stage timings beyond pool mode, aggregation, and charts. No release or target-platform performance claim follows from these traces.
