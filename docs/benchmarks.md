# Transport benchmark methodology

Build with `CMAKE_BUILD_TYPE=Release` and sanitizers disabled for performance experiments. `scripts/run_benchmarks.sh` starts its own one-worker server with no simulated delay, verifies readiness through inference, and runs both noop and compute at 4 and 64 KiB by default. It retains CSV samples, stdout summaries, server logs, and JSON system/build metadata. Each invocation gets its own result directory. Repeat the script at least five times before comparing distributions.

The client generates a deterministic float32 tensor once and reuses it. Every warm-up and measured response is verified. Noop echoes data; it avoids the arithmetic transform but still performs allocation, serialization, copying, and transport. Both endpoints are CPU processes.

## Timer boundaries

- **Latency (v0.2+):** monotonic time immediately before submission through client-side response decoding/validation or shared-output copying, using `Response::completed_at`. It includes input copying, sender queueing, transport preparation, socket control, server queueing/execution, and output reconstruction. The timestamp is set immediately before promise notification; it excludes future-observer scheduling, result verification, CSV writing, and RAII teardown after completion. Reversed completion order does not turn a delayed `future.get()` into an inflated latency. The v0.1 synchronous baseline used API-return time; do not mix those samples into a v0.2/v0.3 comparison.
- **Throughput:** successful measured requests divided by wall-clock time for the entire measured loop, including verification and sample recording. Connection establishment, input creation, warm-up, and CSV writing are excluded.
- **Percentiles:** nearest rank, `sorted[ceil(q * N) - 1]`, using successful measured requests. p50, p95, and p99 are emitted alongside throughput.
- **Failures:** errors terminate the run with a nonzero exit status and a diagnostic. A failed run does not print a success summary; its CSV is incomplete and must not be treated as a valid experiment.

Warm-up requests consume request IDs but never appear in measured CSV rows. A run of 1,000 measured requests after 100 warm-ups records IDs 101 through 1,100 without registration; pool mode with two slots records IDs 103 through 1,102 because registration consumes two IDs. No code creates invented results.

`socket-sync` waits after each submission. `socket-async`, `shm-async`, and `shm-double-buffer` maintain a rolling window up to `--inflight` (1–1024) in both warm-up and measurement phases. Each future still holds bounded completion state. Warm-up completes before the measurement clock starts. Async windows are replenished in submission order; a slow earliest request can constrain throughput even when later replies are ready. This is a harness scheduling policy, not a latency timestamp artifact.

Both APIs use the same sender/receiver control transport. Compare sync/async at `--inflight 1` for matched concurrency, and compare socket/SHM with the same async window. Higher concurrency is a separate pipeline/queueing experiment, not evidence that an individual operation executes faster. Keep tensor size, operation, simulated delay, queue capacity, and worker count fixed. The default baseline script still runs only synchronous socket measurements; the binary accepts all four modes.

`shm-async` allocates and maps a **fresh region for every request**, including after warm-up. Allocation, Linux backing-storage reservation, mapping, and vector copying are included before completion. Final unmap/unlink teardown may finish after the corresponding promise is ready. These are allocation-inclusive measurements, distinct from pool mode. See [shared-memory details](shared-memory.md).

`shm-double-buffer` registers `--buffer-slots 1|2|4` regions once. Its default is two slots and two outstanding calls; choose `--inflight` explicitly for comparisons. `initialization_us` reports connection, allocation/registration, and client thread startup for all modes, outside measured latency/throughput. Pool steady-state timings include waiting, preparation, control traffic, execution, and vector copies, but exclude the initial registrations. The optional pool-only `--prepare-us` (default zero) models client preparation with an interruptible delay; this is not real preprocessing. Do not compare delayed pool runs against undelayed transports as a transport speed claim.

`scripts/run_pipeline_benchmarks.sh [BUILD_DIR]` demonstrates 1/2/4 slots with one worker, 4 KiB compute tensors, and a fixed four-call window. Defaults are 1,000 measured calls, 100 warm-ups, 1,000 μs simulated preparation and 1,000 μs simulated execution. Set `REQUESTS`, `WARMUP`, `PREPARE_US`, and `DELAY_US` to vary them. It owns/cleans its server and retains CSVs, summaries, metadata, and overlap analysis in a unique directory. The analyzer requires serialized execution. [The phase-3 trace](pipeline-evidence.md) is a single scheduling demonstration; five-trial comparisons and variability analysis remain phase 4.

## CSV schema

```text
run_id,mode,tensor_bytes,inflight,workers,buffer_slots,request_id,status,latency_us,queue_us,execute_us,buffer_wait_us,workload,slot,generation,prepare_begin_ns,prepare_end_ns,queued_ns,execute_begin_ns,execute_end_ns
```

`mode` is `socket-sync`, `socket-async`, `shm-async`, or `shm-double-buffer`; sync requires `inflight=1`. `buffer_slots` is 1/2/4 for pool mode and 0 otherwise. `workers` records the benchmark's `--workers` argument (default 1); it is a user declaration, not negotiated with the server, and must match the server configuration. Successful request status is 0. Each CSV holds one mode, concurrency, workload, and tensor size. Direct binary invocations require recording server settings separately.

For pool rows, `queue_us=(execute_begin_ns-queued_ns)/1000`; `execute_us=(execute_end_ns-execute_begin_ns)/1000` includes input/output copying and backend/delay; `buffer_wait_us` measures free-slot acquisition only, excluding prior sender queue time. Preparation begin/end bracket optional simulated preparation and input copy. All absolute stage timestamps use the same host's `CLOCK_MONOTONIC` nanoseconds; they can be compared across the two processes. Stage and slot fields are **empty** for other modes. Empty fields must not be interpreted as measured zeroes. See [pool timing and ownership](pipeline.md).

From the wire schema, a successful socket-copy call transfers `2 * (28 + tensor_bytes)` application-level socket bytes. A successful per-request SHM call transfers `28 + 70 + 28 = 126` bytes, with no tensor bytes on the socket. A pool call transfers `28 + 24 + 28 + 48 = 128` bytes, plus a one-time `28 + 74 + 28 + 4 = 134` bytes per slot registration. Pool completions include timing metadata. These are schema-derived values, not instrumented counters or measurements of physical memory traffic/kernel copying. Matched transport trials, measured counters, charts, and performance conclusions remain phase 4.

These scripts validate the benchmark procedure; shared cloud timings are not evidence of Apple Silicon or NPU performance. Record power/thermal conditions and background load on the target Mac, and report variability. Neither asynchronous submission nor shared memory is assumed to improve latency until measured.
