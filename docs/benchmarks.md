# Socket benchmark methodology

Build with `CMAKE_BUILD_TYPE=Release` and sanitizers disabled for performance experiments. `scripts/run_benchmarks.sh` starts its own one-worker server with no simulated delay, verifies readiness through inference, and runs both noop and compute at 4 and 64 KiB by default. It retains CSV samples, stdout summaries, server logs, and JSON system/build metadata. Each invocation gets its own result directory. Repeat the script at least five times before comparing distributions.

The client generates a deterministic float32 tensor once and reuses it. Every warm-up and measured response is verified. Noop echoes data; it avoids the arithmetic transform but still performs allocation, serialization, copying, and transport. Both endpoints are CPU processes.

## Timer boundaries

- **Latency (v0.2):** monotonic time immediately before submission through client-side response decoding/validation completion, using `Response::completed_at`. It includes input copying, sender queueing, encoding, socket transport, server queueing/execution, and decoding. The timestamp is set immediately before promise notification; it excludes future-observer scheduling, result verification, and CSV writing. Reversed completion order does not turn a delayed `future.get()` into an inflated latency. The v0.1 synchronous baseline used API-return time; do not mix those samples into a v0.2 comparison.
- **Throughput:** successful measured requests divided by wall-clock time for the entire measured loop, including verification and sample recording. Connection establishment, input creation, warm-up, and CSV writing are excluded.
- **Percentiles:** nearest rank, `sorted[ceil(q * N) - 1]`, using successful measured requests. p50, p95, and p99 are emitted alongside throughput.
- **Failures:** errors terminate the run with a nonzero exit status and a diagnostic. A failed run does not print a success summary; its CSV is incomplete and must not be treated as a valid experiment.

Warm-up requests consume request IDs but never appear in measured CSV rows. A run of 1,000 measured requests after 100 warm-ups therefore records IDs 101 through 1,100. No code creates invented results.

`socket-sync` waits after each submission. `socket-async` maintains a rolling window up to `--inflight` (1–1024) in both warm-up and measurement phases. Each future still holds bounded completion state. Warm-up completes before the measurement clock starts. Async windows are replenished in submission order; a slow earliest request can constrain throughput even when later replies are ready. This is a harness scheduling policy, not a latency timestamp artifact.

Both APIs use the same sender/receiver transport. Compare them at `--inflight 1` for matched concurrency. Higher async concurrency is a separate pipeline/queueing experiment, not evidence that an individual operation executes faster. Keep tensor size, operation, simulated delay, queue capacity, and worker count fixed across comparisons. The default script still runs only the synchronous baseline; the binary accepts both modes.

## CSV schema

```text
run_id,mode,tensor_bytes,inflight,workers,buffer_slots,request_id,status,latency_us,queue_us,execute_us,buffer_wait_us,workload
```

`mode` is `socket-sync` or `socket-async`; sync requires `inflight=1`. `buffer_slots=0`. `workers` records the benchmark's `--workers` argument (default 1); it is a user declaration, not negotiated with the server, and must match the server configuration. Successful request status is 0. Queue, backend, and buffer-wait timings are **empty**, because this milestone does not instrument those stages. Empty fields must not be interpreted as measured zeroes. Each CSV holds one mode, concurrency, workload, and tensor size. The baseline script saves summary and metadata beside each CSV; direct binary invocations require recording server settings separately.

At application level, a successful call transfers `2 * (28 + tensor_bytes)` bytes over the socket. This counts one input and output plus two headers; it does not measure physical memory traffic or kernel copy counts. Future shared-memory comparisons must preserve tensor size, workload, server delay, worker count, and in-flight concurrency, and separate allocation/map startup from buffer reuse.

These scripts validate the benchmark procedure; shared cloud timings are not evidence of Apple Silicon or NPU performance. Record power/thermal conditions and background load on the target Mac, and report variability. Neither asynchronous submission nor shared memory is assumed to improve latency until measured.
