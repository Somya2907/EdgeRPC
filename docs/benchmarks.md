# Benchmark methodology (v0.5)

Both endpoints are CPU processes running deterministic float32 compute (`2*x+1`) or noop/echo, returning the full tensor. Every warm-up and measured output is verified. Measurements describe this local RPC runtime; they do not establish NPU, Apple Silicon, DMA, or physical memory traffic performance.

## Repeated comparisons

Build Release with sanitizers disabled. The runtime and CSV analysis have no third-party dependencies. Charts additionally require Matplotlib:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DEDGERPC_SANITIZERS=OFF -DEDGERPC_THREAD_SANITIZER=OFF
cmake --build build --parallel 4
python3 -m venv .venv
.venv/bin/python -m pip install -r benchmarks/requirements.txt
.venv/bin/python scripts/run_comparisons.py
```

In the prepared cloud environment, activate `/workspace/.edgerpc-tools/bin` first. Defaults are:

| Setting | Values |
|---|---|
| Tensor sizes | 4, 64, 1,024 KiB |
| Workloads | noop, compute |
| Server execution delays | 0, 1,000 μs |
| Submission windows | 1, 4 |
| Workers / waiting queue | 1 / 32 |
| Pool slots | 1, 2, 4 |
| Simulated client preparation | 0 for all comparisons |
| Repetitions | 5 per configuration |
| Requests per repetition | 200 measured, 20 warm-up |

All four modes run at window 1. At window 4, socket async, per-request SHM, and pool variants run; socket sync requires window 1. A timing-off socket-async control matches the smallest size/delay/window and first selected workload. Defaults total 133 configurations, 665 trials, 133,000 measured and 13,300 warm-up calls. Each repetition shuffles configuration order with a recorded seed; trials execute sequentially. Separate one-worker servers serve each delay and are idle unless selected. The runner checks real inference readiness and cleans its own private sockets/servers.

```sh
# Longer runs for finer tail estimates:
python3 scripts/run_comparisons.py --requests 1000 --warmup 100
# Focused matched comparisons:
python3 scripts/run_comparisons.py --sizes-kib 4 64 --delays-us 0 \
  --inflight 1 4 --workloads compute --slots 2 4
# One short repetition per mode/window; insufficient for performance conclusions:
python3 scripts/run_comparisons.py --smoke
```

Options include `--build-dir`, `--output` (a new directory), `--seed`, `--run-timeout`, and `--no-plots`. Comparison runs enforce at least five trials, Release, and sanitizers off. Smoke uses one eight-call repetition and marks its report accordingly. Matrix values must be unique; windows are capped at 32 for this queue. Large SHM tensors may require lower concurrency to fit available backing storage.

Outputs go to a unique `results/comparison-*` directory. Metadata records matrix/settings, source/binary SHA-256 hashes, Git revision/dirty state, actual compiler/CMake flags, host/CPU/affinity/cgroup information, and Matplotlib version. The manifest records execution order, exact commands, status, and stdout/stderr/sample paths for each trial. Timeouts, nonzero exits, interruptions, and incomplete samples retain failure details and produce a nonzero result; no successful comparative report is generated. The benchmark writes rows after its measured loop, so failed runs can have only a header or missing/partial files.

## Modes and controls

`socket-sync` waits on each future; `socket-async` maintains a rolling window over the same transport. `shm-async` allocates a fresh region for every request, including after warm-up. Allocation, Linux backing reservation, mapping and vector copies remain in latency. `shm-double-buffer` registers 1/2/4 slots once during construction; vector copies and backend allocations still occur per call.

The harness replenishes futures in submission order. A slow earliest call can constrain throughput even if later responses are ready. Match size, workload, simulated delays, worker/queue limits, window, and timing settings. A window is a submission limit: one pool slot permits only one published pooled call, while four slots support four. Slot-capacity differences are explicit experiment factors. Comparing sync/window 1 with async/window 4 measures concurrency as well as API behavior.

Benchmark `--workers` and `--delay-us` declare server settings; they do not configure or negotiate them. The runner supplies matching settings to both binaries. Direct users must do the same. Pool-only `--prepare-us` adds simulated preparation, default zero. Comparing delayed pool preparation against undelayed transports is not a fair transport comparison. The separate [phase-3 demonstration](pipeline-evidence.md) intentionally used that delay to establish scheduling overlap.

## Timer and counter boundaries

- **Latency:** `steady_clock` immediately before API submission through response validation/output copying, using `Response::completed_at` before promise notification. Includes API input copying, sender waiting, preparation, acquisition, transport, server work/copies and receiver reconstruction. Excludes future-observer scheduling, harness verification, CSV writing and teardown after notification. The receiver waits for its sender's write bookkeeping before settling the future, so completed-call byte boundaries are stable.
- **Throughput:** measured calls divided by wall time for the measured loop, including verification/future observation/sample bookkeeping. Initialization, tensor creation, warm-up and CSV writing are excluded. `measured_seconds` supports independent checking.
- **Initialization:** `initialization_us` covers connect, pool allocation/registration when enabled, and thread creation. It is a startup aggregate, not isolated allocation time. `first_request_us` is the first warm-up response latency, or the first measured call with zero warm-up. It uses the selected window; other initial calls can overlap it. These cold observations are separate from steady-state distributions.
- **Preparation:** common-host `CLOCK_MONOTONIC` timestamps bracket sender encoding/allocation/mapping/input copying. Pool preparation starts after acquisition and includes optional simulated delay. Earlier FIFO sender waiting remains in residual latency.
- **Queue:** `queued_ns` is immediately before enqueue, after validation/mapping. Queue duration ends when a worker begins execution. It excludes socket receive, admission work and queue-full failures.
- **Execution:** input decoding/copying, backend/delay, and output encoding/copying. Excludes socket response writing and timing-prefix insertion; this is not pure backend computation time.
- **Buffer wait:** pool acquisition only; measured zero for instrumented non-pool calls, which acquire no slot.
- **Traffic:** `IoCounters` add each positive `send`/`recv` return, including headers, control, errors and partial transfers. `client.traffic()` gives cumulative directional snapshots. Quiescent boundaries after construction, warm-up and measurement separate initialization/warm-up/measured bytes. Active-I/O snapshots are not an atomic pair. These count application socket bytes, not kernel overhead, memory transactions or copies.

Timing defaults to benchmark `--instrumentation on`, using an optional wire flag/response prefix. The library constructor's fifth argument enables it; ordinary clients default off. `off` removes added socket/per-request-SHM stage timing and metadata, while byte counters and completion bookkeeping remain. Pool completion timestamps already existed in v0.4 and remain with timing off. The matched off control estimates added timing/prefix overhead, not a fully uninstrumented runtime. Timing can change results, including extra prefix copying.

Servers persist across trials and OS caches are not flushed. “Cold” observations therefore describe a new client connection/pool and its first call, rather than a cold server, process cache or model load.

## Samples, aggregation and charts

```text
run_id,mode,tensor_bytes,inflight,workers,buffer_slots,request_id,status,latency_us,queue_us,execute_us,buffer_wait_us,workload,slot,generation,prepare_begin_ns,prepare_end_ns,queued_ns,execute_begin_ns,execute_end_ns,instrumented,delay_us,prepare_delay_us
```

Non-pool instrumented slot/generation values are zero. Unavailable stage fields are empty with non-pool timing off; empty is not measured zero. Absolute timestamps share the kernel's monotonic clock. Control/warm-up IDs are excluded: two registrations plus 100 warm-ups produce measured IDs 103 onward. Successful rows have status zero; failures remain in trial logs/status and are never silently dropped from successful comparisons.

```sh
python3 benchmarks/report.py results/comparison-EXAMPLE
python3 benchmarks/report.py results/comparison-EXAMPLE --no-plots
```

Analysis validates matrix coverage, five distinct repetitions, unique run/request IDs, sample counts/configuration, finite latency, stage ordering/durations, summary consistency, and measured traffic against independent protocol arithmetic. Percentiles use nearest rank, `sorted[ceil(q*N)-1]`. `trials.csv` retains per-trial p50/p95/p99, throughput, startup/first call, stage means and byte rates. `summary.csv` reports median, mean, sample standard deviation, min and max across per-trial statistics. Samples are **not pooled** into one percentile distribution. Error bars are observed min–max ranges, not confidence intervals.

`report.md` includes every configuration plus six standalone Matplotlib SVG/PNG charts: p99 versus size, throughput versus size, p50/p95/p99 versus concurrency, client initialization/first-call costs, mean stages, and socket traffic. Performance plots show compute results; noop remains in tables. Stage bars use means of per-trial means. “Other latency” subtracts non-overlapping measured stages from each call's duration, covering transport, validation, FIFO waiting and reconstruction. Quantiles are never added together. Charts/reports regenerate from retained raw CSVs and manifests. `report-provenance.json` hashes the current report/chart code separately from the original measurement metadata, so chart improvements do not rewrite measurement provenance.

| Successful call, tensor bytes B | Request bytes | Response bytes with timing on | Startup bytes |
|---|---:|---:|---:|
| Socket | `28+B` | `28+24+B` | 0 |
| Per-request SHM | 98 | 52 | 0 |
| Pool | 52 | 76 | 134 per slot |

Timing off removes 24 response bytes for socket/per-request SHM; pool traffic stays the same. These formulas cross-check actual syscall counters, including warm-up and registration boundaries.

## Interpretation

See [measured findings](benchmark-results.md) for retained five-trial data and charts. The older baseline and pipeline shell scripts remain short sync and synthetic overlap demonstrations; they do not replace the comparison runner.

With 200 samples, p99 is the 198th observation, a coarse tail estimate. Five short shared-cloud trials expose variability but do not establish statistical significance, universal speedups, thermal behavior or long-run stability. Retain outliers/negative results, record external load and target-device power conditions, and use longer runs on target hardware before making portfolio claims. v0.1 used API-return latency; v0.2–v0.4 had different telemetry/wire overhead. Do not silently mix historical samples into v0.5 comparisons.

The configured window is recorded, rather than an observed peak of active requests. Per-process CPU/RSS and separate serialization/send/receive durations are not collected; unassigned transport/scheduler costs remain in residual latency. More detailed resource profiling remains follow-up work.
