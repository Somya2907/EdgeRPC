# Baseline benchmark methodology

Build with `CMAKE_BUILD_TYPE=Release` and sanitizers disabled for performance experiments. `scripts/run_benchmarks.sh` starts its own one-worker server with no simulated delay, verifies readiness through inference, and runs both noop and compute at 4 and 64 KiB by default. It retains CSV samples, stdout summaries, server logs, and JSON system/build metadata. Each invocation gets its own result directory. Repeat the script at least five times before comparing distributions.

The client generates a deterministic float32 tensor once and reuses it. Every warm-up and measured response is verified. Noop echoes data; it avoids the arithmetic transform but still performs allocation, serialization, copying, and transport. Both endpoints are CPU processes.

## Timer boundaries

- **Latency:** monotonic time immediately before `infer_sync` through its return, including tensor encoding, request send, execution, response receive, and decoding. Result verification occurs after the latency timestamp.
- **Throughput:** successful measured requests divided by wall-clock time for the entire measured loop, including verification and sample recording. Connection establishment, input creation, warm-up, and CSV writing are excluded.
- **Percentiles:** nearest rank, `sorted[ceil(q * N) - 1]`, using successful measured requests. p50, p95, and p99 are emitted alongside throughput.
- **Failures:** errors terminate the run with a nonzero exit status and a diagnostic. A failed run does not print a success summary; its CSV is incomplete and must not be treated as a valid experiment.

Warm-up requests consume request IDs but never appear in measured CSV rows. A run of 1,000 measured requests after 100 warm-ups therefore records IDs 101 through 1,100. No code creates invented results.

## CSV schema

```text
run_id,mode,tensor_bytes,inflight,workers,buffer_slots,request_id,status,latency_us,queue_us,execute_us,buffer_wait_us,workload
```

`mode=socket-sync`, `inflight=1`, `workers=1`, and `buffer_slots=0`. Successful request status is 0. Queue, backend, and buffer-wait timings are **empty**, because this milestone does not instrument those stages. Empty fields must not be interpreted as measured zeroes. Each CSV holds one workload and tensor size. The summary and metadata sit beside it.

At application level, a successful call transfers `2 * (28 + tensor_bytes)` bytes over the socket. This counts one input and output plus two headers; it does not measure physical memory traffic or kernel copy counts. Future shared-memory comparisons must preserve tensor size, workload, server delay, worker count, and in-flight concurrency, and separate allocation/map startup from buffer reuse.

These scripts validate the benchmark procedure; shared cloud timings are not evidence of Apple Silicon or NPU performance. Record power/thermal conditions and background load on the target Mac, and report variability. Neither asynchronous submission nor shared memory is assumed to improve latency until measured.
