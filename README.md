# EdgeRPC

A C++17 client/server runtime for studying inference RPC across processes. A client sends float32 tensors over a Unix domain socket to a **CPU-based simulated accelerator**, which returns either `2*x + 1` (`compute`) or the input unchanged (`noop`).

This is a software prototype inspired by heterogeneous inference communication. It does **not** implement Qualcomm FastRPC, execute on an NPU, or measure CPU–NPU transfers.

## Current milestone: synchronous baseline

- Reusable `edgerpc` library, server, demo client, and benchmark.
- Versioned binary frames, explicit big-endian integers and float32 values, unique client request IDs, and an 8 MiB tensor limit.
- Partial read/write and interruption handling, bounded I/O deadlines, disconnect errors, and response validation.
- Persistent connections, deterministic compute/noop workloads, and optional simulated execution delay.
- Safe socket-path cleanup, stale-socket recovery, and SIGINT/SIGTERM shutdown.
- Correctness and failure tests that run separate server/client processes.
- Warm-up-separated measurements, per-request CSV, p50/p95/p99, and throughput.

Async submission, bounded worker queues, shared memory, and double buffering are the next milestones in [Plan.md](Plan.md). This baseline serves one connection and one request at a time. Each client instance supports one caller at a time.

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
```

Expect `status=OK verified=true`. Stop the server with Ctrl-C. All binaries support `--help`; unsupported modes and concurrency settings fail explicitly. Tensor counts from zero through 2,097,152 float32 elements are supported.

For controlled simulated work, start the server with `--delay-us 1000`. Both sides accept `--timeout-ms` (default 5000): this is an I/O deadline per header or payload transfer, not a total inference deadline. A timed-out client closes its connection; work already executing may finish. Create a new client to reconnect. An idle connected peer can delay graceful shutdown until the read deadline.

The library API:

```cpp
#include <edgerpc/client.hpp>

edgerpc::EdgeRpcClient client("/tmp/edgerpc.sock");
auto response = client.infer_sync({-1.0f, 0.0f, 2.0f});
// response.output == {-1.0f, 1.0f, 5.0f}
```

## Benchmark

With a server running:

```sh
mkdir -p results
./build/edgerpc_benchmark --socket /tmp/edgerpc.sock --mode socket-sync \
  --size-kib 64 --inflight 1 --requests 1000 --warmup 100 \
  --workload compute --csv results/baseline.csv
```

Or let the script start and clean up its own server, use a private temporary socket, and save system/build metadata:

```sh
./scripts/run_benchmarks.sh
REQUESTS=5000 WARMUP=200 SIZES_KIB="4 64 1024 8192" ./scripts/run_benchmarks.sh
```

Results go in a unique directory under ignored `results/`. Run repeated trials before interpreting performance. [Benchmark methodology](docs/benchmarks.md) explains timer boundaries and limitations. No optimization or NPU speedup is claimed by this initial baseline.

## Reliability and architecture

```text
Client process                             Server process
EdgeRpcClient -> encode -> Unix socket -> validate -> MockExecutor
              <- decode <- Unix socket <- encode  <- 2*x+1 / noop
```

See [the wire protocol](docs/protocol.md) and [architecture and next steps](docs/architecture.md).

CTest includes nine C++ test groups and 17 Python integration tests, covering byte order, fragmented frames, large partial writes, zero/max tensors, malformed frames, invalid operations, disconnects, deadlines, response mismatches, restart, and CSV warm-up exclusion. Checks remain active in Release builds.

For a sanitizer build:

```sh
cmake -S . -B build-sanitize -DCMAKE_BUILD_TYPE=Debug -DEDGERPC_SANITIZERS=ON
cmake --build build-sanitize --parallel 4
ctest --test-dir build-sanitize --output-on-failure
```

This is a **same-user, trusted-client local demo**. Socket permissions are 0600; use a private directory for concurrent runs. One client can occupy the serial server until it disconnects or its read deadline expires. There is no authentication, application work queue, cancellation, or multi-tenant isolation. A killed process may leave a socket path; the next server reclaims it only after a refused connection probe. Existing files, symlinks, and active sockets are preserved.

The current version is validated on Linux x86_64. CI is configured for Linux and macOS; macOS/Apple Silicon behavior still needs a run on that platform. Docker/ARM64 and hardware backends remain future work.
