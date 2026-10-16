# Phase 4: measured findings

In this Linux cloud experiment, registered reuse lowered median warm latency for the selected medium/large tensors, while fresh SHM was slower than socket async for the smallest tensor. Startup cost and tail variability matter: the results do not establish a universal transport speedup.

The validated run on 2026-10-16 contains **665 successful trials across 133 configurations**, five trials each, with **133,000 measured requests and 13,300 warm-ups**. Every output was verified. Settings were 4/64/1,024 KiB, noop/compute, 0/1,000 μs execution delay, one worker, queue capacity 32, windows 1/4, and pool slots 1/2/4. Simulated client preparation was zero. Trial order was randomized with seed 20261016 plus the trial index. The extra timing-off control used 4 KiB noop, window 1, delay zero.

The host reports AMD EPYC 9V74, Linux x86_64, GCC 14.2.0, Release without sanitizers, a four-CPU cgroup quota and 32 GiB memory limit. Runtime/runner measurement sources match committed revision `14a56e1`; documentation was being edited, so metadata correctly records a dirty tree. The captured runtime/runner source hashes all match that commit. Chart/report improvements are separately hashed in `report-provenance.json`. This is a shared cloud environment; target Mac/ARM64 hardware, thermal/power behavior and NPU execution were not measured.

## Selected matched results

The table fixes 64 KiB compute, one worker, zero simulated delays, and timing on. Values are medians of five per-trial statistics; bracketed values are observed min–max ranges across trials, not confidence intervals. Compare rows within the same window; one slot still limits pooled publication to one active generation.

| Mode | Slots | Window | p50 μs | p95 μs | p99 μs [min–max] | Requests/s [min–max] |
|---|---:|---:|---:|---:|---:|---:|
| shm-async | 0 | 1 | 273.5 | 639.8 | 795.2 [538.4–1,690.9] | 2,556.6 [1,997.6–2,928.0] |
| shm-double-buffer | 1 | 1 | 77.7 | 232.5 | 467.4 [281.7–607.5] | 6,446.9 [4,932.4–8,165.6] |
| shm-double-buffer | 2 | 1 | 79.1 | 171.6 | 328.9 [233.0–530.0] | 6,895.9 [6,016.1–7,903.0] |
| shm-double-buffer | 4 | 1 | 86.6 | 323.2 | 421.2 [185.4–951.0] | 6,206.9 [3,361.6–8,871.3] |
| socket-async | 0 | 1 | 411.3 | 841.8 | 1,145.6 [762.4–23,442.0] | 1,789.4 [502.5–2,939.0] |
| socket-sync | 0 | 1 | 384.5 | 864.2 | 1,608.9 [594.7–4,581.8] | 1,841.9 [1,237.1–3,039.5] |
| shm-async | 0 | 4 | 391.1 | 653.4 | 1,072.5 [729.5–3,025.6] | 8,948.7 [2,884.2–9,821.8] |
| shm-double-buffer | 1 | 4 | 290.9 | 502.0 | 1,405.7 [293.2–2,781.6] | 10,744.4 [6,109.9–14,948.1] |
| shm-double-buffer | 2 | 4 | 164.2 | 655.0 | 1,124.6 [248.6–1,934.6] | 11,361.6 [9,387.5–20,760.0] |
| shm-double-buffer | 4 | 4 | 109.2 | 238.8 | 334.7 [222.9–2,146.1] | 17,151.1 [8,955.3–28,452.5] |
| socket-async | 0 | 4 | 786.6 | 1,523.0 | 1,867.6 [1,021.3–7,171.0] | 4,062.3 [2,350.7–6,334.5] |

At window 1, median p50 was 411.3 μs for socket async, 273.5 μs for fresh SHM, and 79.1 μs for the two-slot pool. The result changes with size: at 4 KiB, fresh SHM median p50 was 121.1 μs versus 80.1 μs for socket async. Allocation/mapping and control are plausible costs; timings do not isolate every syscall or memory copy.

At 64 KiB/window 4, four pooled slots reached a median 17,151.1 requests/s versus 4,062.3 for socket async in this run. The same configuration had substantial variation: its throughput ranged from 8,955.3 to 28,452.5 requests/s. More slots did not consistently improve tails: at 4 KiB/window 4, four-slot median p99 was 515.7 μs, compared with 429.1 μs for two slots and 502.9 μs for socket async. A universal latency or tail improvement is not supported.

With 1,000 μs simulated execution at 64 KiB/window 4, median throughput was 765.8 requests/s for socket async, 840.6 for fresh SHM, 882.8 for two slots, and 893.5 for four slots. The serialized worker delay constrains these modes; their observed ranges overlap. Higher submission concurrency can improve utilization while increasing queueing latency.

## Startup, traffic and observer effects

Pooling moves region setup into construction. At 64 KiB/window 1, two-slot median initialization was 759.4 μs versus 123.6 μs for fresh SHM, whose allocation happens inside each RPC. Their median first-client-call latencies were 455.6 and 770.6 μs; warm p50 values were 79.1 and 273.5 μs. Initialization and first-call values are separate per-trial observations, not summed medians or an isolated cold-server/model-load test. The persistent server and OS caches were not reset.

Actual syscall counters matched the wire contract in every trial. At 64 KiB with timing on, socket calls transferred 131,152 application bytes, fresh SHM 150, and pooled calls 128, excluding startup. Each pool slot registered with 134 bytes total control traffic. These are application socket bytes; SHM still copies vectors and uses memory bandwidth.

The 4 KiB noop/window-1 timing control had median p50 99.9 μs with timing on and 70.0 μs off; throughput medians were 6,356.4 and 6,897.1 requests/s. Throughput ranges overlapped widely (3,615.5–13,917.2 versus 3,459.9–9,975.2). This short control does not establish a stable instrumentation overhead across modes or sizes. Byte counters remain active with timing off, and existing pool timestamps remain present.

An initial batch was rejected because the runner omitted the workload argument and requested noop trials executed compute. It produced no comparative success report. The argument propagation bug was fixed, regression tests added, and the complete matrix rerun. Those initial samples are excluded; the [diagnostic](evidence/benchmarks-v0.5/rejected-batch-diagnostic.json) records the actual mismatch. Forced-timeout smoke validation also retained failures/logs and emitted no comparison report.

## Evidence and reproduction

- [Full comparison report](evidence/benchmarks-v0.5/report.md), [all aggregate statistics](evidence/benchmarks-v0.5/summary.csv), and [per-trial statistics](evidence/benchmarks-v0.5/trials.csv).
- [Raw trial archive](evidence/benchmarks-v0.5/raw-trials.tar.gz): all 665 measured CSVs, stdout/stderr, exact-command manifest, metadata and server logs.
- [Metadata](evidence/benchmarks-v0.5/metadata.json), [manifest](evidence/benchmarks-v0.5/manifest.json), [report provenance](evidence/benchmarks-v0.5/report-provenance.json), and [SHA-256 checksums](evidence/benchmarks-v0.5/SHA256SUMS).
- Charts in SVG and PNG: [latency](evidence/benchmarks-v0.5/latency.svg), [throughput](evidence/benchmarks-v0.5/throughput.svg), [p50/p95/p99 versus concurrency](evidence/benchmarks-v0.5/concurrency.svg), [client startup](evidence/benchmarks-v0.5/startup.svg), [stages](evidence/benchmarks-v0.5/stages.svg), and [socket traffic](evidence/benchmarks-v0.5/traffic.svg).

```sh
python3 scripts/run_comparisons.py
# Regenerate from the retained raw archive in a new directory:
mkdir -p results/review-phase4
tar -xzf docs/evidence/benchmarks-v0.5/raw-trials.tar.gz -C results/review-phase4
python3 benchmarks/report.py results/review-phase4
```

[Methodology](benchmarks.md) covers tool installation, controls, timer/counter boundaries, aggregation and CLI selections. Raw samples are not pooled across trials; no outliers were removed. With 200 measured calls per trial, nearest-rank p99 is only the 198th observation. Five short trials expose variability without establishing statistical significance or long-run reliability. Larger samples and native target-device runs remain necessary before using performance percentages in portfolio claims. Actual peak active requests, CPU/RSS and separate send/receive timing remain unmeasured; configured window limits and residual latency are reported honestly.

Release and ASan/UBSan validation pass all six suites, including 11 protocol groups, 30 async groups, six SHM groups, four pool groups, 39 process tests and 15 analysis tests. Analysis rejects missing/duplicate trials, configuration mistakes, non-finite latency, invalid timing and inconsistent byte counts. Native Mac/ARM64 and broader portability/reliability remain phase 5.
