# EdgeRPC

A C++17 client/server runtime for studying inference RPC across processes. A client exchanges float32 tensors through socket payloads or POSIX shared memory with a **CPU-based simulated accelerator**, which returns either `2*x + 1` (`compute`) or the input unchanged (`noop`). Unix socket frames carry request and completion control messages in both modes.

This is a software prototype inspired by heterogeneous inference communication. It does **not** implement Qualcomm FastRPC, execute on an NPU, or measure CPU–NPU transfers.

## Current milestone: native portability and reliability

- Reusable `edgerpc` library, server, demo client, and benchmark.
- Thread-safe `infer_async()` submissions, futures, request-ID matching, and shared sync/async transport.
- Bounded outstanding client requests and server task queues with explicit overload errors.
- Per-request POSIX shared-memory tensors, validated descriptors, and metadata-only socket traffic.
- Registered 1/2/4-slot pools, generation validation, explicit buffer ownership, and preparation/execution overlap traces.
- Versioned binary frames, explicit big-endian integers and float32 values, unique client request IDs, and an 8 MiB tensor limit.
- Partial read/write and interruption handling, bounded I/O deadlines, disconnect errors, and response validation.
- Persistent connections, deterministic compute/noop workloads, and optional simulated execution delay.
- Safe socket-path cleanup, stale-socket recovery, and SIGINT/SIGTERM shutdown.
- Correctness and failure tests that run separate server/client processes.
- Warm-up-separated measurements, per-request CSV, p50/p95/p99, and throughput.
- Optional stage telemetry across transports, measured socket bytes, repeated-trial reports and exported charts.
- Native Linux x86_64/ARM64 and macOS Apple Silicon CI, an Ubuntu validation image, and concurrent/crash/overload stress checks.

Synchronous RPC, async execution, shared memory, reusable buffers, repeated benchmark comparisons, and native platform validation are implemented. Documentation/release completion is phase 6 in [the completion roadmap](docs/completion-plan.md). The server serves one active client connection at a time and supports multiple outstanding requests within it. Client submissions may come from multiple threads.

## Build and test

Requires a C++17 compiler, CMake 3.20+, and Python 3.8+ for integration tests. Ninja is optional. There are no third-party C++ dependencies.

On macOS:

```sh
xcode-select --install  # If Command Line Tools are not installed
brew install cmake ninja python
```

Build on macOS or Linux from the repository root:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
```

Add `-G Ninja` on the initial configure if preferred. Use a fresh build directory when changing generators. Python is unnecessary for a library/app-only build with `-DBUILD_TESTING=OFF`.

In the prepared cloud environment, activate its installed CMake and Ninja first:

```sh
export PATH="/workspace/.edgerpc-tools/bin:$PATH"
```

## Run two processes

Terminal 1:

```sh
./build/edgerpc_server --socket /tmp/edgerpc.sock --workers 1
```

Terminal 2:

```sh
./build/edgerpc_demo --socket /tmp/edgerpc.sock --mode sync --elements 1024 --verify
./build/edgerpc_demo --socket /tmp/edgerpc.sock --workload noop --elements 1024 --verify
./build/edgerpc_demo --socket /tmp/edgerpc.sock --mode async --requests 32 --elements 1024 --verify
./build/edgerpc_demo --socket /tmp/edgerpc.sock --mode async --transport shm --requests 32 --elements 1024 --verify
./build/edgerpc_demo --socket /tmp/edgerpc.sock --mode async --transport pool --buffer-slots 2 --requests 32 --elements 1024 --verify
```

Expect `status=OK verified=true` for every requested result. Stop the server with Ctrl-C. All binaries support `--help`. Tensor counts from zero through 2,097,152 float32 elements are supported. The server defaults to one worker and 32 queued tasks; configure `--queue-capacity` as needed. `--workers 2` through `16` is an optional parallel CPU experiment; it does not model a single serialized accelerator.

For controlled simulated work, start the server with `--delay-us 1000`. Both sides accept `--timeout-ms` (default 5000): this is an I/O deadline per header or payload transfer, not a total inference deadline. A client with no pending work does not run a receive deadline. A silent peer causes pending calls to fail and the connection to close; create a new client to reconnect. `future.wait_for()` does not cancel work. Already-running backend execution may finish after a disconnect. A partial incoming frame can delay graceful shutdown until its read deadline.

The library API:

```cpp
#include <edgerpc/client.hpp>

edgerpc::EdgeRpcClient client("/tmp/edgerpc.sock");
auto response = client.infer_sync({-1.0f, 0.0f, 2.0f});
// response.output == {-1.0f, 1.0f, 5.0f}
auto first = client.infer_async({1.0f, 2.0f});
auto second = client.infer_async({3.0f, 4.0f});
auto first_result = first.get();
auto second_result = second.get();
auto shared = client.infer_async({1.0f, 2.0f}, edgerpc::Operation::Compute,
                               edgerpc::TensorTransport::SharedMemory);
auto shared_result = shared.get();
```

The third argument selects `Socket` (default), `SharedMemory`, or `SharedPool`; they can be mixed on one connection. Per-request SHM allocates a fresh object for each call. To reuse buffers, pass `PoolOptions{2, 1024}` as the constructor's fourth argument, then select `SharedPool` for calls with at most 1,024 elements. Registration happens once during construction; generations and states prevent early slot reuse. Both modes copy vectors into/out of native-endian mapped tensors; the backend still copies data. See [pool registration and ownership](docs/pipeline.md) and [shared-memory limits/crash recovery](docs/shared-memory.md).

Client capacity defaults to 32 **total outstanding calls**, including queued and sent requests; the constructor's third argument changes it (1–1024). A full client returns an already-failed future with `RpcError(Status::Overloaded)`. A full server queue returns the same status for that request. Valid remote errors leave other requests and the connection usable. Protocol errors, I/O failures, `close()`, and destruction settle outstanding futures with exceptions. `close()` is idempotent and joins owned threads. Do not destroy the client while another thread is calling its methods.

## Benchmark

With a server running:

```sh
mkdir -p results
./build/edgerpc_benchmark --socket /tmp/edgerpc.sock --mode socket-sync \
  --size-kib 64 --inflight 1 --requests 1000 --warmup 100 \
  --workload compute --csv results/baseline.csv
./build/edgerpc_benchmark --socket /tmp/edgerpc.sock --mode socket-async \
  --size-kib 64 --inflight 16 --requests 1000 --warmup 100 \
  --workload compute --csv results/async.csv
./build/edgerpc_benchmark --socket /tmp/edgerpc.sock --mode shm-async \
  --size-kib 64 --inflight 16 --requests 1000 --warmup 100 \
  --workload compute --csv results/shm.csv
./build/edgerpc_benchmark --socket /tmp/edgerpc.sock --mode shm-double-buffer \
  --buffer-slots 2 --size-kib 64 --inflight 4 --requests 1000 --warmup 100 \
  --workload compute --csv results/pool.csv
```

Or let the script start and clean up its own server, use a private temporary socket, and save system/build metadata:

```sh
./scripts/run_benchmarks.sh
REQUESTS=5000 WARMUP=200 SIZES_KIB="4 64 1024 8192" ./scripts/run_benchmarks.sh
./scripts/run_pipeline_benchmarks.sh  # 1/2/4 slots with synthetic preparation/execution
```

For repeated comparisons across all modes, with five trials, controlled settings, variability, failure retention and charts:

```sh
python3 -m pip install -r benchmarks/requirements.txt  # Optional chart dependencies
python3 scripts/run_comparisons.py
python3 scripts/run_comparisons.py --smoke  # Short functional check only
```

The baseline shell script runs socket sync; the pipeline shell script records synthetic overlap intervals. The comparison runner owns its servers and randomizes trial order. Results go in ignored `results/`; [retained measurements and findings](docs/benchmark-results.md) include raw samples, metadata, summaries and SVG/PNG charts. Benchmark timing defaults on (`--instrumentation off` provides a control); ordinary library clients default off. Per-request SHM includes fresh allocation/map setup; pool initialization is reported separately. [Methodology](docs/benchmarks.md) defines timer boundaries, actual socket counters, matching controls and limitations.

## Reliability and architecture

```text
Client process                             Server process
API -> bounded submissions -> sender -> Unix socket -> bounded execution queue
future <- ID matching <- receiver <- Unix socket <- worker(s): 2*x+1 / noop
       \____________ native float32 shared-memory input/output ______________/
```

See [the wire protocol](docs/protocol.md), [shared-memory ownership](docs/shared-memory.md), and [architecture and next steps](docs/architecture.md).

Linux CTest includes 11 protocol/backend/metrics groups, 30 async/concurrency groups, six buffer/descriptor groups, four pool/wire/state groups, 39 process integration tests, 16 benchmark-analysis tests, and nine reliability tests. Coverage includes zero/max tensors, independent mappings, invalid metadata/permissions, reversed replies, allocation/registration failures, pool ownership, overload, disconnect/crash recovery, thousands of concurrent submissions, timed maximum responses, actual/partial byte counts, configuration propagation, and invalid/incomplete report rejection. The allocation-failure injection is Linux-specific; other groups are shared with macOS. Checks remain active in Release builds.

For full tests plus real-process socket/SHM/pool smoke checks, or the unprivileged container workflow:

```sh
python3 scripts/validate_platform.py
docker build -f docker/Dockerfile -t edgerpc:validation .
docker run --rm --shm-size=256m edgerpc:validation
```

[Portability and reliability](docs/portability.md) records verified CI jobs, stress scenarios, Docker details, sanitizer commands and limitations.

For a sanitizer build:

```sh
cmake -S . -B build-sanitize -DCMAKE_BUILD_TYPE=Debug -DEDGERPC_SANITIZERS=ON
cmake --build build-sanitize --parallel 4
ctest --test-dir build-sanitize --output-on-failure
```

This is a **same-user, trusted-client local demo**. Socket/SHM permissions are 0600; use a private socket directory. One client can occupy the server until it disconnects or becomes idle with no work outstanding. Queue limits bound request counts; choose lower capacities for large tensors (each maximum SHM request reserves 16 MiB before vector copies). There is no authentication, per-request cancellation, fair multi-client scheduling, or multi-tenant isolation. A killed process may leave a socket path; the next server reclaims it after a refused connection probe. SHM names are removed at server admission; killing a client before admission can leave an orphan requiring identified-name cleanup. Existing regular files, symlinks, and active sockets are preserved.

Release validation passes on native Linux x86_64, Linux ARM64 and macOS ARM64, including native Linux Docker builds on both architectures. Linux ASan/UBSan passes. The optional ThreadSanitizer run has a documented system C++ library exception warning reproduced independently of EdgeRPC; its final run is not recorded as clean. Accelerator hardware backends remain future work.
