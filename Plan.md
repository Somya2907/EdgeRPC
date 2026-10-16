# EdgeRPC — Complete Build Plan (macOS / Apple Silicon)

> **Project:** EdgeRPC — A Low-Latency RPC Runtime for Simulated Heterogeneous AI Inference  
> **Target machine:** Apple Silicon Mac (including MacBook Air M4)  
> **Primary language:** C++17 (C++20 optional)  
> **Main development environment:** macOS Terminal + VS Code + Clang + CMake  
> **Optional final environment:** Docker Desktop running Linux/ARM64  
> **Physical Qualcomm NPU required:** **No** for the core project  
> **Suggested duration:** 6 weeks at approximately 8–12 hours/week; adjust based on C++ experience  
> **Last updated:** October 16, 2026

---

## 0. Read this first: what you are actually building

You will build a small **C++ client/server inference runtime** in which an application process sends tensor-computation requests to a separate **simulated accelerator process**. You will implement the communication yourself, first with Unix domain sockets, then with asynchronous execution, shared-memory tensor buffers, and double buffering. Finally, you will **measure** which changes actually improve latency or throughput.

**Why this is relevant to Qualcomm:** The work resembles the *software design problems* encountered in CPU-to-accelerator communication layers: RPC messages, request lifecycles, queues, synchronization, and efficient data movement. Qualcomm's actual FastRPC architecture spans processor domains and includes platform-specific drivers, stubs/skeletons, and Qualcomm hardware. Your Mac prototype **does not implement FastRPC and does not measure real CPU–NPU transfers**. You should say that explicitly in interviews.

### One-sentence portfolio description

> Built EdgeRPC, a cross-platform C++ prototype for low-latency inference RPC between processes, supporting sync/async execution, shared-memory tensor exchange, backpressure, and reproducible end-to-end benchmarks using a simulated accelerator.

### What success looks like

- Two separate processes exchange correct inference requests and results.
- You expose `infer_sync()` and `infer_async()` APIs.
- You handle partial socket reads/writes, multiple outstanding requests, and error cases.
- You compare socket-based payload copying to **POSIX shared memory** while keeping control messages over RPC.
- A bounded double-buffer pipeline overlaps input preparation with execution without data races.
- You publish real, reproducible latency/throughput measurements, including p50/p95/p99.
- The code compiles on your Mac, has automated tests, and optionally runs in ARM64 Linux through Docker.
- You can defend **why** each optimization was made, including cases where it does not help.

### Scope: must-have vs. later

| Priority | Features | Reason |
|---|---|---|
| **Must-have** | Synchronous socket RPC, correctness tests | Establish a simple, trustworthy baseline |
| **Must-have** | Async client, request IDs, response matching | Match Qualcomm-relevant execution concerns |
| **Must-have** | POSIX shared-memory buffers | Explore reduced copying and buffer ownership |
| **Must-have** | Double buffering, queue bounds, benchmarks | Demonstrate throughput and system design |
| **Should-have** | Linux/ARM64 container build, stress tests, profiling | Demonstrate portability and reliability |
| **Optional** | Real Core ML application backend | Can demo local ML, but does **not** expose CPU–Neural Engine RPC |
| **Future / hardware-dependent** | Qualcomm QAIRT/QNN backend, FastRPC-aware integration | Requires supported device, SDK, runtime, and access |
| **Out of scope** | Writing an NPU driver, DMA engine, real NPU kernel, distributed cluster, custom tensor compiler | Too much complexity for a first embedded-systems project |

---

## 1. Prerequisites: using your Mac correctly

**Important:** macOS is UNIX-based. You already have access to Unix domain sockets, processes, threads, and APIs such as `shm_open()` and `mmap()`. You **do not** need to install Linux before starting.

### 1.1 Install developer tools

Open Terminal:

```bash
xcode-select --install
```

If you have [Homebrew](https://brew.sh/):

```bash
brew install cmake ninja python
```

Check that everything works:

```bash
clang++ --version
cmake --version
python3 --version
uname -m
```

On an Apple Silicon Mac, `uname -m` typically prints `arm64`.

Install the **C/C++** and **CMake Tools** extensions in VS Code if you use VS Code. Docker is **not** needed until the final portability phase.

### 1.2 First-time C++ concepts to learn (only what you need)

Learn these in order; build immediately after each topic rather than spending weeks on tutorials:

1. **Memory and ownership:** `std::vector`, `std::unique_ptr`, RAII, pointers vs. references.
2. **Processes vs. threads:** processes have separate address spaces; threads share a process's memory.
3. **Syscalls and file descriptors:** `socket`, `bind`, `listen`, `accept`, `connect`, `read`, `write`, `close`.
4. **Concurrency:** `std::thread`, `std::mutex`, `std::condition_variable`, `std::promise`, `std::future`.
5. **Shared memory:** `shm_open`, `ftruncate`, `mmap`, `munmap`, `shm_unlink`.
6. **Performance measurement:** `std::chrono::steady_clock`, histograms/percentiles, throughput vs. tail latency.

**Don't get stuck studying C++ templates, advanced lock-free algorithms, or OS internals before making the first client/server work.**

### 1.3 macOS portability traps

| Topic | Use in this project | Avoid assuming on macOS |
|---|---|---|
| Local RPC | Unix domain sockets (`AF_UNIX`) | Linux-only `io_uring` |
| Shared memory | POSIX `shm_open` + `mmap` | Linux-only `memfd_create` |
| Synchronization | C++ mutexes inside one process; socket messages for cross-process buffer ownership | Assuming a regular `std::mutex` coordinates independent processes |
| Timing | `std::chrono::steady_clock` | Linux-only timing utilities |
| Profiling | Benchmark timers, macOS Instruments optionally | Linux `perf` on native macOS |
| Linux portability | Docker Linux/ARM64 later | Pretending Docker provides a physical NPU |

**Key detail:** `std::mutex` used in the client cannot protect memory owned by another process. For your first shared-memory implementation, use **socket control messages plus exclusive buffer ownership** rather than writing your own inter-process lock.

---

## 2. System architecture

```text
                        macOS (Apple Silicon)

 +----------------------------------------------+
 | CLIENT PROCESS                               |
 |                                              |
 | App -> EdgeRPC Client API                    |
 |         |                                    |
 |         +-> Sync request / Async queue       |
 |         +-> Request-ID / pending-futures map  |
 |         +-> Buffer pool & metrics             |
 +---------|-----------------------|------------+
           | RPC control           | Shared tensor memory
           | Unix domain socket    | POSIX shm + mmap
           v                       v
 +----------------------------------------------+
 | SERVER PROCESS (MOCK ACCELERATOR)            |
 |                                              |
 | Socket protocol / request validation         |
 |    -> bounded work queue                     |
 |    -> one or more simulated execution workers|
 |    -> deterministic tensor calculation       |
 |    -> response with matching request ID      |
 +----------------------------------------------+

 Future optional backend: real Qualcomm QAIRT/QNN on supported hardware.
```

### 2.1 Define the boundary carefully

- **RPC control path:** socket messages containing request ID, operation type, tensor metadata, buffer handle/name/offset, status, and timing information.
- **Tensor data path (baseline):** tensor bytes serialized through the socket.
- **Tensor data path (optimized):** client and server map the same named shared-memory object; the socket carries only *metadata* describing the tensor location.
- **Execution backend:** deterministic `float32` operations on the server. For example, `output[i] = input[i] * 2 + 1`, plus an optional configurable simulated delay.
- **Result path:** either payload on socket (baseline) or result stored in another agreed shared-memory region (optimized), followed by a completion message.

### 2.2 Why use a simple mock rather than a heavy ML model?

A simple deterministic operation isolates communication overhead from model runtime overhead. Use **two workload modes**:

- `noop`: minimum possible server work, suitable for measuring transport + scheduler overhead.
- `compute`: deterministic floating-point transformation with optional controlled `sleep`/work delay, suitable for examining queueing and overlap.

Later, add a CPU matrix-multiplication backend if you want a more compute-heavy case. A CPU simulation is **not an emulation of the Qualcomm Hexagon instruction set or NPU hardware**.

### 2.3 Core APIs you should aim for

These are **interface sketches**, not drop-in implementations:

```cpp
struct Request {
    std::uint64_t id;
    std::vector<float> input;
};

struct Response {
    std::uint64_t id;
    std::vector<float> output;
    int status = 0;
};

class EdgeRpcClient {
public:
    Response infer_sync(const std::vector<float>& input);
    std::future<Response> infer_async(std::vector<float> input);
};

class ExecutionBackend {
public:
    virtual ~ExecutionBackend() = default;
    virtual void execute(const float* input, float* output,
                         std::size_t count) = 0;
};
```

**Why these choices?** Both APIs can use the same transport; the async call returns promptly with a future. Abstracting the backend makes it easier to replace the mock later without changing client code.

---

## 3. Source tree and module responsibilities

```text
EdgeRPC/
├── README.md
├── LICENSE
├── .gitignore
├── CMakeLists.txt
├── include/edgerpc/
│   ├── protocol.hpp            # Message schema and length limits
│   ├── transport.hpp           # Framed socket send / receive
│   ├── client.hpp              # infer_sync / infer_async
│   ├── server.hpp              # Accept + handle client requests
│   ├── executor.hpp            # Backend interface
│   ├── mock_executor.hpp       # Deterministic simulated accelerator
│   ├── thread_pool.hpp         # Bounded work queue
│   ├── shared_buffer.hpp       # POSIX shm RAII object
│   ├── buffer_pool.hpp         # Buffer ownership / double buffering
│   └── metrics.hpp             # Timers + structured samples
├── src/
│   ├── protocol.cpp
│   ├── transport_unix.cpp
│   ├── client.cpp
│   ├── server.cpp
│   ├── mock_executor.cpp
│   ├── thread_pool.cpp
│   ├── shared_buffer_posix.cpp
│   ├── buffer_pool.cpp
│   └── metrics.cpp
├── apps/
│   ├── edgerpc_server.cpp      # CLI server
│   └── edgerpc_demo.cpp        # CLI client
├── benchmarks/
│   ├── benchmark.cpp           # Reproducible workload harness
│   └── plot_results.py        # Read CSV, emit charts
├── tests/
│   ├── test_protocol.cpp
│   ├── test_sync.cpp
│   ├── test_async.cpp
│   ├── test_shared_memory.cpp
│   └── test_stress.cpp
├── scripts/
│   ├── run_smoke_test.sh
│   └── run_benchmarks.sh
├── docker/
│   └── Dockerfile              # Optional Linux/ARM64 build
└── docs/
    ├── architecture.md
    ├── protocol.md
    ├── benchmarks.md
    ├── tradeoffs.md
    └── interview_notes.md
```

**Build rule:** Keep protocol, networking, buffers, backend, and metrics in separate modules. Don't put the whole project in one `main.cpp`.

---

## 4. Protocol design (decide this before writing async code)

### 4.1 Message schema

A compact, versioned binary message is enough. For example:

| Field | Type | Purpose |
|---|---|---|
| magic | `uint32_t` | Reject unrelated/corrupt traffic |
| version | `uint16_t` | Protocol compatibility |
| type | `uint16_t` | REQUEST, RESPONSE, ERROR, SHM_REQUEST |
| request_id | `uint64_t` | Match replies to requests |
| payload_size | `uint32_t` | Exact payload length |
| status | `uint32_t` | Success, overload, invalid payload, etc. |
| flags | `uint32_t` | Sync/async, copied/shm modes |

**Implementation notes:**

- Serialize each field deliberately; do not transmit a raw C++ struct and assume its padding and byte order will match everywhere.
- Use a fixed byte order (for example, network byte order) and document it.
- Implement `read_exact()` and `write_all()` loops. A single socket `read()` is **not guaranteed** to return the whole message.
- Reject malformed frames, unsupported versions, negative/overflowed dimensions, and oversized payloads.
- Start with a fixed maximum tensor size (for example, 8 MiB).
- Give every request a unique `request_id`, including asynchronous requests.

### 4.2 Response matching

For async requests, track `request_id -> promise` (or another completion handle) in a thread-safe map. A dedicated receiver thread reads responses and completes the correct promise, regardless of completion order.

When the socket closes, fail every outstanding promise with an error rather than hanging forever.

### 4.3 Failure and backpressure policy

Specify simple, testable behavior from the beginning:

- **Queue full:** return an explicit `OVERLOADED` status or apply a documented bounded wait. Never allow infinite memory growth.
- **Timeout:** the caller gets a timeout, but a task may continue running unless cancellation is truly implemented.
- **Disconnected client:** free or reclaim outstanding state safely.
- **Malformed request:** reject without crashing the server.
- **Server restart:** new connection should be possible; no stale socket file should prevent launch.

---

## 5. Detailed six-week / 30-session plan

A *session* is about 1.5–2.5 focused hours. Move faster or slower based on your C++ comfort. Each week ends with a demonstrable, tested deliverable.

### Week 1 — Build a reliable synchronous RPC baseline

**Goal:** One process sends a tensor; a second process executes a deterministic operation and replies correctly.

**Session 1 — Tooling, repository, and CMake**

- [ ] Install Apple Clang and CMake; configure VS Code.
- [ ] Create the source tree and first Git commit.
- [ ] Make `cmake -S . -B build -G Ninja` succeed.
- [ ] Build two console executables: `edgerpc_server` and `edgerpc_demo`.
- [ ] Write the project scope and truthful simulator disclaimer in `README.md`.

**Session 2 — Unix domain socket basics**

- [ ] Learn `AF_UNIX`, `SOCK_STREAM`, socket paths, and file descriptors.
- [ ] Implement server `bind -> listen -> accept`.
- [ ] Implement client `connect`.
- [ ] Send a short string, read it back, close cleanly.
- [ ] Use RAII for socket descriptor cleanup.

**Session 3 — Framing and protocol**

- [ ] Implement explicit header serialization and deserialization.
- [ ] Implement `read_exact` and `write_all`.
- [ ] Add checks for protocol version and max payload size.
- [ ] Add one malformed-frame test and one multi-frame test.

**Session 4 — First inference operation**

- [ ] Send 1,024 `float32` values from client to server.
- [ ] Server computes `y[i] = 2*x[i] + 1`.
- [ ] Return the output and match the request ID.
- [ ] Verify all outputs using a defined floating-point tolerance.

**Session 5 — Baseline measurements and cleanup**

- [ ] Run 1,000 requests; separate warm-up from measurements.
- [ ] Record round-trip times with `std::chrono::steady_clock`.
- [ ] Test server shutdown and socket-file cleanup.
- [ ] Commit and tag `v0.1-sync`.

**Definition of done:** Synchronous inference consistently returns correct outputs, handles framing correctly, and logs latency to CSV.

**Interview concept:** Why a remote call is more expensive than an ordinary in-process function call.

---

### Week 2 — Asynchronous execution and concurrency

**Goal:** `infer_async()` lets callers submit multiple requests without waiting for each result.

**Session 6 — Concurrency fundamentals**

- [ ] Write a tiny exercise using `std::thread` and `std::mutex`.
- [ ] Write a producer-consumer queue using `std::condition_variable`.
- [ ] Learn the difference between blocking I/O and non-blocking API semantics.

**Session 7 — Client-side async submission**

- [ ] Implement a thread-safe bounded submission queue.
- [ ] Add `infer_async(std::vector<float>) -> std::future<Response>`.
- [ ] Worker thread serializes and sends queued requests.
- [ ] Implement clean shutdown with no abandoned threads.

**Session 8 — Receive thread and request matching**

- [ ] Build a synchronized map of pending request IDs.
- [ ] Have a dedicated receiver read and decode response frames.
- [ ] Resolve the matching promise/future.
- [ ] Correctly fail pending promises when the server disconnects.

**Session 9 — Server concurrency and backpressure**

- [ ] Add a bounded server queue.
- [ ] Start with **one execution worker** for an easy-to-understand accelerator analogy.
- [ ] Optionally add configurable multiple workers **as a separate experiment**, while clearly noting this no longer resembles a single serialized accelerator.
- [ ] Return explicit queue-full errors or use a documented blocking policy.

**Session 10 — Async tests and first comparisons**

- [ ] Submit 16–32 requests concurrently and verify correct request/response matching.
- [ ] Test shuffled completion order if your server has parallel workers or controlled delays.
- [ ] Test disconnect during outstanding requests.
- [ ] Compare sync and async **at matched workload, worker count, and concurrency**.
- [ ] Commit and tag `v0.2-async`.

**Definition of done:** Async calls return promptly, all futures resolve or error, and the system remains stable under concurrency.

**Interview concept:** Async submission can improve caller utilization and overlap, but it **does not necessarily speed up a single serialized executor**.

---

### Week 3 — Shared memory and avoiding socket payload copies

**Goal:** Use a shared tensor region rather than sending the entire tensor through the socket.

**Session 11 — Memory layout and RAII**

- [ ] Learn virtual address spaces and why two processes cannot use each other's raw pointers.
- [ ] Implement an RAII wrapper around `shm_open`, `ftruncate`, `mmap`, `munmap`, `close`, `shm_unlink`.
- [ ] Use unique, predictable-for-this-run shared-memory names such as `/edgerpc_<run_id>_<slot>` and appropriate permissions.

**Session 12 — Basic shared-memory proof of concept**

- [ ] Have the client create and map a region.
- [ ] Have the server independently open and map the **same named region**.
- [ ] Write a known float array from the client and read it from the server.
- [ ] Validate bounds and cleanup.

**Session 13 — Shared-memory RPC mode**

- [ ] Add protocol fields for buffer name/ID, offset, byte length, element count, and operation.
- [ ] Use a socket message to tell the server that data is ready.
- [ ] Server reads shared memory, computes, and writes to an agreed output region.
- [ ] Server sends a completion message with request ID.

**Session 14 — Make buffer ownership explicit**

- [ ] Implement state transitions: `FREE -> CLIENT_WRITING -> SERVER_OWNED -> RESULT_READY -> FREE`.
- [ ] Transfer ownership via socket messages; no simultaneous writes to the same slot.
- [ ] Ensure the client does **not** reuse a slot before completion is acknowledged.
- [ ] Define cleanup on failed requests and disconnects.

**Session 15 — Compare copy and shared-memory modes**

- [ ] Compare socket copy mode vs. shared-memory mode for 4 KiB, 64 KiB, 1 MiB, and 8 MiB tensors.
- [ ] Measure setup/mapping costs separately from steady-state buffer reuse.
- [ ] Validate bitwise-identical or tolerance-equivalent results.
- [ ] Commit and tag `v0.3-shm`.

**Definition of done:** Shared-memory mode returns correct outputs and avoids sending tensor payload bytes through the socket in steady state. Do **not** claim zero data copies inside hardware or the OS without measuring/knowing the full path.

**Interview concept:** Shared memory can eliminate application-level serialization/copying, but lifetime management, synchronization, and cache effects still matter.

---

### Week 4 — Double buffering and pipelining

**Goal:** Prepare request B while request A executes, avoiding one serial stage blocking all work.

**Session 16 — Draw the pipeline timeline**

```text
TIME --->
CPU prepares:  [Prepare A] [Prepare B] [Prepare C] [Prepare D]
Executor:                  [Execute A] [Execute B] [Execute C]
Buffer slot:       A            B           A           B
```

This is illustrative: actual overlap depends on the workload and available CPU resources.

**Session 17 — Buffer pool**

- [ ] Implement two fixed-size input slots and two output slots (or documented paired slots).
- [ ] Provide acquire/release operations with a condition variable in the client process.
- [ ] Track the buffer state associated with each outstanding request.
- [ ] Keep slot and request identifiers separate.

**Session 18 — Pipeline stages**

- [ ] Split request preparation, RPC submission, execution, and completion handling.
- [ ] Let the CPU prepare the next request in a free slot while the executor owns another slot.
- [ ] Record timestamps for each stage.

**Session 19 — Stress buffer lifecycle**

- [ ] Run many requests with only two slots.
- [ ] Slow down the executor deliberately to force backpressure.
- [ ] Verify the client never overwrites data still owned by the server.
- [ ] Test timeout, server crash, and process restart around slot reuse.

**Session 20 — Evaluate the pipeline**

- [ ] Compare 1 slot, 2 slots, and optionally 4 slots.
- [ ] Measure throughput, p99 latency, wait-for-buffer time, and peak memory.
- [ ] Determine if double buffering helped; record any tradeoff.
- [ ] Commit and tag `v0.4-pipeline`.

**Definition of done:** Double-buffer mode passes ownership tests, demonstrates overlapping timeline events when possible, and has measured performance relative to one-buffer mode.

**Interview concept:** A buffer pool isn't only a speed optimization; it enforces finite memory use and safe ownership.

---

### Week 5 — Benchmarking, profiling, reliability

**Goal:** Turn your prototype into a credible systems-engineering project rather than a demo with unverified claims.

**Session 21 — Benchmark harness**

- [ ] Expose CLI parameters for mode, input bytes, request count, in-flight count, worker count, simulated compute duration, buffer slots, and CSV output.
- [ ] Fix randomness/seed and computation across variants.
- [ ] Use a warm-up period (e.g., 200 requests) and a measured period (e.g., 5,000+ requests depending on workload).
- [ ] Perform at least 5 repeated trials for important comparisons.

**Session 22 — Metrics**

Capture:

- End-to-end latency from API submission to completed result.
- Queue waiting time.
- Transport serialization / send / receive time where feasible.
- Simulated compute time.
- Client-side buffer wait time.
- Requests per second, success/error rate, and peak outstanding requests.
- Process CPU usage and approximate RSS if easily measurable.

Use p50, p95, p99; don't publish only mean latency.

**Session 23 — Fair comparisons**

- [ ] Test socket-sync, socket-async, SHM-async, and SHM-double-buffer.
- [ ] Compare the same input sizes and backend compute settings.
- [ ] Keep the same executor worker count unless you explicitly label a worker-scaling experiment.
- [ ] Compare same in-flight concurrency for transport comparisons; separately show what higher concurrency changes.
- [ ] Save hardware/OS/compiler settings to a metadata file.

**Session 24 — Failure testing**

- [ ] Malformed header or length.
- [ ] Payload size beyond your maximum.
- [ ] Abrupt server termination and client disconnect.
- [ ] Queue full and request timeout.
- [ ] Repeated connect/disconnect and clean restart.
- [ ] Tensor of zero elements and largest supported size.
- [ ] Repeated run with sanitizers where available.

**Session 25 — Report and charts**

- [ ] Generate throughput vs. payload-size chart.
- [ ] Generate p50/p95/p99 vs. in-flight request chart.
- [ ] Generate copy-vs-SHM comparison, including setup costs.
- [ ] Record surprising/negative results and explain them.
- [ ] Commit and tag `v0.5-benchmarked`.

**Definition of done:** Anyone with your repo and the documented toolchain can reproduce the benchmark procedure, and the report distinguishes observed results from assumptions.

**Interview concept:** How you know the optimization worked and what limitations remain.

---

### Week 6 — Portability, Qualcomm mapping, and interview packaging

**Goal:** Make the project portable, truthful, and easy for a hiring manager to evaluate.

**Session 26 — Linux/ARM64 in Docker (optional but valuable)**

- [ ] Install [Docker Desktop for Apple Silicon](https://docs.docker.com/desktop/setup/install/mac-install/).
- [ ] Create a Dockerfile using a Linux C++ compiler + CMake.
- [ ] Build and run both client and server **inside the same Linux container or same container IPC namespace** for the simplest shared-memory test.
- [ ] Document platform-specific adjustments and rerun smoke tests.

Example *starting point* for a Dockerfile:

```dockerfile
FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build python3 ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY . .
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build --parallel
# Add an entrypoint script once the client/server CLIs are implemented.
```

Build it with:

```bash
docker build --platform linux/arm64 -f docker/Dockerfile -t edgerpc:arm64 .
```

Docker provides Linux user space/OS behavior through its VM, **not** Qualcomm NPU hardware.

**Session 27 — Compare architecture with Qualcomm FastRPC**

Study the [Qualcomm FastRPC repository](https://github.com/qualcomm/fastrpc) and the [official explanation of QAIRT/FastRPC/Hexagon](https://www.qualcomm.com/developer/blog/2026/10/how-ai-models-reach-hexagon-npu-qairt-fastrpc-hexagon-explained).

Create `docs/qualcomm_mapping.md`:

| EdgeRPC concept | Qualcomm architecture concept | Critical difference |
|---|---|---|
| Local client API | Application / RPC stub side | EdgeRPC has handcrafted IPC, not QAIC-generated FastRPC stubs |
| Unix socket RPC | FastRPC messaging path | Qualcomm crosses processor domains using platform drivers/transport |
| Mock server process | Remote execution service analogy | Not an actual DSP/NPU or skel binary |
| Shared `mmap` tensors | Shared-buffer design goals | No DSP-specific memory registration, cache, or IOMMU work |
| Sync + async operations | QNN sync/async graph execution | Queue behavior and hardware execution rules are backend-specific |
| Metrics | Runtime profiling | No HTP cycles, firmware metrics, or power measurements |

**Session 28 — Prepare backend extensibility**

- [ ] Ensure the backend is accessed through an `ExecutionBackend` interface.
- [ ] Add `MockBackend` as the only supported implementation for now.
- [ ] Write a **design note**, not pretend code, for a future `QnnBackend`.
- [ ] Consult official [QNN graph execution](https://docs.qualcomm.com/nav/home/function_QnnGraph_8h_1a3ea05f42a9295f9a74a2e3a0cdd64228.html?product=924033590759186372) and [QNN async API documentation](https://docs.qualcomm.com/nav/home/api-rst_file_include_QNN_QnnGraph_h.html?product=924033590759186372).

**Session 29 — Polish GitHub repository**

- [ ] Write a quick-start with real commands for *your* implemented CLI.
- [ ] Add `docs/architecture.md` diagram and communication sequence.
- [ ] Include test output and reproducible benchmark scripts.
- [ ] Put charts and a short results table at the top of the README.
- [ ] State limitations and next steps plainly.

**Session 30 — Interview rehearsal and release**

- [ ] Record a 3–5 minute screen demo.
- [ ] Practice the interview story in Section 11 aloud.
- [ ] Prepare answers to every question in Section 12.
- [ ] Publish `v1.0-mock-runtime` only when tests and docs pass.

**Definition of done:** A clean repository, an actual demo, documented measurements, realistic hardware disclaimers, and a concise explanation of Qualcomm relevance.

---

## 6. Exact build/run workflow

### 6.1 First terminal commands

```bash
mkdir EdgeRPC && cd EdgeRPC
git init
mkdir -p include/edgerpc src apps benchmarks tests docs scripts docker
printf '# EdgeRPC\n\nSimulated-accelerator RPC prototype.\n' > README.md
printf 'build/\n.DS_Store\n*.csv\n' > .gitignore
```

For **Session 1**, create two starter files (`apps/edgerpc_server.cpp` and `apps/edgerpc_demo.cpp`) containing a `main()` function each. This minimal `CMakeLists.txt` compiles immediately once those two files exist:

```cmake
cmake_minimum_required(VERSION 3.20)
project(EdgeRPC VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(edgerpc_server apps/edgerpc_server.cpp)
add_executable(edgerpc_demo apps/edgerpc_demo.cpp)
```

Build with:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

As reusable modules are implemented, introduce an `edgerpc` library with `add_library(edgerpc ...)`, publish headers using `target_include_directories`, and link both apps to the library with `target_link_libraries`. Add source files to CMake only after creating them.

### 6.2 Expected final CLI usage (you implement these flags)

```bash
# Terminal 1: start simulated accelerator service
./build/edgerpc_server --socket /tmp/edgerpc.sock --workers 1

# Terminal 2: one request and result verification
./build/edgerpc_demo --socket /tmp/edgerpc.sock --mode sync --elements 1024 --verify

# Example benchmark commands after implementing the benchmark tool
./build/edgerpc_benchmark --mode socket-sync --size-kib 64 --inflight 1 --requests 5000
./build/edgerpc_benchmark --mode socket-async --size-kib 64 --inflight 16 --requests 5000
./build/edgerpc_benchmark --mode shm-async --size-kib 64 --inflight 16 --requests 5000
./build/edgerpc_benchmark --mode shm-double-buffer --size-kib 64 --inflight 2 --requests 5000
```

These commands describe the **desired interface**; they will not work until the corresponding binaries and options exist.

### 6.3 Recommended Git commits

```text
chore: initialize CMake project and source layout
feat: add framed Unix-domain socket transport
feat: add synchronous inference RPC
feat: add bounded async requests and response matching
feat: add POSIX shared-memory buffer transport
feat: implement owned double-buffer pipeline
perf: add reproducible throughput and tail-latency benchmarks
test: add malformed-frame, disconnect, and contention tests
docs: add Qualcomm architecture comparison and limitations
```

---

## 7. Benchmark design: avoid misleading results

### 7.1 Experiment matrix

| Experiment | Tensor bytes | In-flight requests | Backend workers | Workload |
|---|---:|---:|---:|---|
| Transport overhead | 4 KiB, 64 KiB, 1 MiB, 8 MiB | 1 | 1 | No-op |
| Async request pressure | 64 KiB | 1, 4, 16, 32 | 1 | Fixed compute delay |
| Shared-memory benefit | 4 KiB, 64 KiB, 1 MiB, 8 MiB | Same for both modes | 1 | No-op + compute |
| Double buffering | 1 MiB | Buffer slots 1, 2, 4 | 1 | Prepare + compute |
| Worker scaling (separate) | 64 KiB | 16 | 1, 2, 4 | Parallelizable CPU work |

**Important fairness rules**

- Compare *transport types* while keeping request sizes, outstanding concurrency, and executor count constant.
- Compare *sync vs. async submission* under clearly described client concurrency conditions. A single synchronous caller submits serially; this isn't necessarily a fair comparison against 16 asynchronous requests unless the goal is explicitly to test concurrency.
- To measure **data-copy reduction**, record bytes transmitted over the socket: large payloads for copy mode; metadata only for shared-memory mode.
- Shared-memory allocation and map setup have costs. Show cold-start and buffer-reuse measurements separately.
- The Mac's workload scheduling, CPU frequency, thermal behavior, and background tasks affect results. Prefer plugged-in power, repeat runs, and report variability.
- Record failures and timeouts; a fast run that silently loses requests is not an improvement.

### 7.2 Minimum CSV schema

```csv
run_id,mode,tensor_bytes,inflight,workers,buffer_slots,request_id,status,latency_us,queue_us,execute_us,buffer_wait_us
```

Store one row per completed request (or a separate clearly documented summary file). Use Python to aggregate percentiles by experiment configuration. **Never invent benchmark results.**

### 7.3 Report formulae

- **Throughput (requests/s)** = successful completed requests / measured wall-clock seconds.
- **p99 latency** = 99th percentile of successful request end-to-end latencies.
- **Speedup (%)** = `(baseline_time / optimized_time - 1) * 100`, if describing speedup in multiplicative terms; for **latency reduction (%)**, use `(baseline_latency - optimized_latency) / baseline_latency * 100`.
- **Copy reduction:** quantify socket payload bytes avoided; avoid conflating this with total physical memory traffic.

### 7.4 What counts as a credible result?

A good write-up might say:

> For 1 MiB tensors with one server worker and 16 in-flight requests, shared-memory mode reduced application-level socket payload bytes and changed p99 latency from **[measured A]** to **[measured B]**. The improvement was smaller for 4 KiB tensors because control-message overhead dominated.

Fill in values **only after running the experiment**. It's also legitimate to report that an optimization did not help, then analyze why.

---

## 8. Reliability and security checklist

- [ ] Unique request IDs do not collide during the test lifetime.
- [ ] Protocol parser validates field sizes before allocation.
- [ ] Socket read/write loops handle partial reads/writes and interruptions.
- [ ] Disconnects fail pending futures and release buffers.
- [ ] Queues are bounded and overload has explicit behavior.
- [ ] Zero-sized and maximum-sized tensors are defined/tested.
- [ ] Shared-memory names, file permissions, sizes, and offsets are validated.
- [ ] No raw shared-memory pointer is used after `munmap`.
- [ ] No slot is written while another process owns it.
- [ ] Process crash leaves no permanent resource leak; document recovery behavior.
- [ ] Server does not accept arbitrary externally supplied shared-memory names without a documented trust boundary. For this local demo, restrict to same-user/local test usage.
- [ ] Verify clean termination using repeated start/stop tests.

For a local teaching project, a **same-user, trusted-client threat model** is reasonable. Do not present this prototype as a hardened multi-tenant RPC service.

---

## 9. Common beginner mistakes (and how to avoid them)

| Mistake | Why it causes problems | Better approach |
|---|---|---|
| Starting with QNN SDK or NPU drivers | Hardware/runtime restrictions distract from fundamentals | Build local RPC first; QNN later |
| Assuming `read()` returns a full frame | Sockets are byte streams, not message packets | Length-prefix messages + `read_exact()` |
| Using raw pointers across processes | Virtual addresses aren't shared across process boundaries | Use shared-memory offsets and region identifiers |
| Assuming async makes individual inference faster | It often changes overlap and concurrency, not execution time | Compare end-to-end latency and queueing separately |
| Using `std::mutex` as an inter-process lock | Its default semantics are intra-process | Use RPC ownership messages initially |
| Not bounding request queues | Overload can exhaust RAM and inflate tail latency | Fixed capacity + backpressure or errors |
| Recreating shared memory for each request | Setup cost hides steady-state benefits | Reuse a pool; separately report initialization |
| Measuring only average latency | Hides slow tail requests | Include p50/p95/p99 |
| Writing 'CPU–NPU' on an all-CPU benchmark chart | Implies hardware that wasn't tested | Label it 'CPU client ↔ simulated accelerator process' |
| Claiming 'zero-copy to NPU' | macOS prototype proves no such hardware transfer | Claim reduced socket-payload copying only |

---

## 10. What your finished GitHub README should contain

Use this outline:

1. **Title and two-line description.** State that this is a simulated-accelerator prototype.
2. **Motivation.** Why RPC and buffer movement matter for heterogeneous inference.
3. **Architecture diagram.** Client, Unix socket, shared memory, simulated server.
4. **Features implemented.** Separate implemented vs. future work.
5. **Quick start on macOS.** Copy/paste build/run commands.
6. **How the protocol works.** Frame, request ID, result matching.
7. **Sync vs. async.** How callbacks/futures and server queues behave.
8. **Shared memory and buffer ownership.** Explicit state machine and cleanup rules.
9. **Benchmarks.** Test hardware and environment, workloads, numbers, charts, methodology.
10. **Reliability.** Tests run, known limitations, timeout/backpressure behavior.
11. **Qualcomm mapping.** Design similarities, then clear non-equivalences.
12. **Future work.** Real QNN backend subject to device/SDK availability.

Bonus deliverables: a 60–90 second demo video, a short technical design PDF (optional), and a one-page benchmark summary.

---

## 11. Interview explanation: a natural 90-second story

> I wanted to understand where latency comes from in AI inference beyond the model itself, especially when a CPU delegates work to an accelerator. So I built EdgeRPC, a C++ runtime with two separate processes: a client and a simulated accelerator. I started with synchronous requests over Unix domain sockets and measured end-to-end latency. Then I added asynchronous execution and request IDs so multiple calls could be outstanding without mixing up responses. The next improvement was moving tensor payloads into shared memory, using RPC only for control messages, and then adding double buffering so preparation could overlap with execution. I benchmarked all of these under the same workloads and looked at throughput and tail latency, not just averages. The prototype doesn't use a Qualcomm NPU, but it helped me understand the communication, synchronization, and memory tradeoffs behind systems like FastRPC.

**Replace general claims with your actual measured findings** after the implementation. Don't memorize this word for word; tell the story using your own decisions.

### Explain your most important tradeoffs

- **Sockets vs. shared memory:** sockets are simple for control and isolation; shared memory can be more efficient for large payloads but complicates ownership.
- **Sync vs. async:** sync is simple for callers; async improves overlap and request pipelining but adds queue management and lifecycle complexity.
- **One worker vs. multiple:** a single worker mirrors serialized execution; multiple CPU workers test scheduling and scaling but don't faithfully simulate one NPU engine.
- **One buffer vs. two:** one slot is simpler; two may permit useful overlap, at the cost of extra memory and coordination.
- **Latency vs. throughput:** more outstanding work may raise throughput while increasing queue delays and tail latency.

---

## 12. Questions you must be ready to defend

| Question | Essential answer to understand |
|---|---|
| Why use RPC? | A different process/address space needs a communication interface; in real heterogeneous hardware the boundary is even more significant. |
| What's a Unix domain socket? | Local OS IPC endpoint; stream sockets carry ordered bytes, requiring message framing. |
| Why not use a function call? | Direct calls don't cross process boundaries; they don't model marshaling/IPC overhead. |
| What's synchronous execution? | Caller waits until result is available. |
| What's asynchronous execution? | Caller gets a handle/future before completion and retrieves the result later. |
| Do async calls execute in parallel? | Not necessarily. Async API, execution parallelism, and server worker count are distinct. |
| Why shared memory? | Both processes map the same backing storage, avoiding large application-level socket payload transfers. |
| What does `mmap()` do? | Maps a shared-memory object/file into a process's virtual address space. |
| What's a race condition? | Results depend on unsynchronized overlapping access; buffer reuse before completion is an example. |
| How do you ensure buffer safety? | Explicit ownership states, bounded slots, completion acknowledgment, and lifecycle tests. |
| What's backpressure? | Preventing unlimited pending work by blocking/rejecting/bounding queues. |
| Why p99? | Tail latency reveals slow outliers that averages can hide. |
| When might shared memory be slower? | Small tensors; setup, coordination, and cache costs may dominate. |
| How close is this to Qualcomm FastRPC? | It exercises related IPC/runtime design patterns, but it does not cross to Hexagon or use actual FastRPC drivers/stubs. |
| What would change on real Qualcomm hardware? | QNN graph setup, compatible buffer registration, backend-specific synchronization, hardware profiling, and device/runtime constraints. |

### Suggested five-minute whiteboard demo

1. **30 sec:** State the problem: large payload transfer + serialized inference requests.
2. **60 sec:** Draw the two-process architecture and basic protocol.
3. **60 sec:** Explain async requests, unique IDs, and pending futures.
4. **60 sec:** Draw shared-memory buffer states and double buffering.
5. **60 sec:** Show your measured charts and one surprising tradeoff.
6. **30 sec:** Distinguish simulated accelerator from real Qualcomm NPU execution.

---

## 13. Resume/GitHub bullet templates — fill in real numbers only

**Resume bullet option A:**

> Built **EdgeRPC**, a C++ inference communication runtime supporting synchronous/asynchronous requests, bounded queues, and shared-memory tensor buffers across isolated processes on macOS and Linux/ARM64.

**Resume bullet option B (after benchmark):**

> Optimized EdgeRPC tensor transfer using POSIX shared memory and double buffering, reducing **[measured metric]** by **[measured percentage]** for **[workload]**, validated with reproducible p50/p95/p99 latency benchmarks.

**Interview or README positioning:**

> Qualcomm-inspired heterogeneous inference **software prototype** (simulated accelerator; no physical NPU integration).

Avoid saying “implemented FastRPC,” “optimized Qualcomm NPU inference,” or “zero-copy CPU–NPU transfer” unless you truly integrate and verify those systems.

---

## 14. Optional extension paths (only after the core version is done)

### Extension A — Actual on-device inference via Core ML

Use Apple's Core ML for a separate application-level model execution backend. This can demonstrate model execution from your Mac, but Core ML abstracts the low-level Neural Engine communication and may select CPU, GPU, or Neural Engine according to model/runtime constraints. Keep this **separate** from claims about custom CPU–NPU RPC.

### Extension B — ARM64 Linux reproducibility

Run both processes in one Linux Docker container and retest IPC and shared-memory correctness. This demonstrates OS portability and differences between macOS and Linux APIs, not a hardware accelerator.

### Extension C — Actual Qualcomm QNN (future)

If you later obtain a supported Snapdragon/Dragonwing device and SDK/runtime access:

1. Confirm device, OS, SDK version, backend, and model compatibility.
2. Study QNN initialization and graph/context loading.
3. Add a backend that uses documented QNN synchronous graph execution.
4. Where supported, investigate asynchronous graph execution and buffer-registration APIs.
5. Profile *actual* hardware behavior, keeping simulator results separate.

### Extension D — Model / execution graph loading

If Qualcomm responsibilities emphasize weight and graph movement, extend the runtime with an **application-level graph/context cache** and benchmark cold starts, warm starts, peak memory, and switching overhead. Do not claim control of private NPU memory placement.

---

## 15. Links to authoritative reference material

These are for learning and later hardware mapping; you do **not** need them to finish Week 1.

1. **Qualcomm FastRPC GitHub:** https://github.com/qualcomm/fastrpc — architecture, IDL, stub/skeleton concept.
2. **Qualcomm QAIRT/FastRPC/Hexagon explainer:** https://www.qualcomm.com/developer/blog/2026/10/how-ai-models-reach-hexagon-npu-qairt-fastrpc-hexagon-explained — conceptual communication path.
3. **Qualcomm QNN sync graph execution:** https://docs.qualcomm.com/nav/home/function_QnnGraph_8h_1a3ea05f42a9295f9a74a2e3a0cdd64228.html?product=924033590759186372
4. **Qualcomm QNN graph API, including async:** https://docs.qualcomm.com/nav/home/api-rst_file_include_QNN_QnnGraph_h.html?product=924033590759186372
5. **Apple POSIX shared-memory (`shm_open`) manual:** https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/shm_open.2.html
6. **Docker Desktop installation for Mac:** https://docs.docker.com/desktop/setup/install/mac-install/
7. **CMake tutorial:** https://cmake.org/cmake/help/latest/guide/tutorial/index.html
8. **Apple Instruments overview:** https://developer.apple.com/tutorials/instruments

---

## 16. Single-page completion checklist

### Milestone 1: RPC fundamentals
- [ ] Mac toolchain working.
- [ ] Client/server build with CMake.
- [ ] Framed socket protocol with correct error handling.
- [ ] Synchronous tensor result verified.
- [ ] Initial benchmark logged.

### Milestone 2: Async requests
- [ ] Bounded async submission queue.
- [ ] Request ID -> future result matching.
- [ ] Disconnect/overload errors handled.
- [ ] Concurrency stress test passing.

### Milestone 3: Shared memory
- [ ] POSIX shared-memory region created and mapped in both processes.
- [ ] Socket carries metadata instead of tensor bytes.
- [ ] Ownership and cleanup are safe.
- [ ] Same results as socket-copy mode.

### Milestone 4: Pipeline
- [ ] Two-slot buffer pool.
- [ ] Preparation and execution stages overlap when applicable.
- [ ] Buffer reuse is safe under load.
- [ ] One-slot vs. two-slot measurements complete.

### Milestone 5: Evidence
- [ ] p50/p95/p99 and throughput reported.
- [ ] Fair workload and concurrency controls.
- [ ] Performance charts, raw CSV, and reproducible script.
- [ ] Limitations and negative results recorded.

### Milestone 6: Portfolio
- [ ] GitHub README with accurate claims.
- [ ] Architecture and protocol diagram.
- [ ] Automated correctness/stress tests.
- [ ] Docker Linux/ARM64 test if feasible.
- [ ] Qualcomm comparison note.
- [ ] Five-minute explanation rehearsed.

---

## 17. What to do **today** (your first 90–120 minutes)

1. Run the toolchain installation commands in Section 1.
2. Create the `EdgeRPC` directory and initialize Git.
3. Create the two first C++ programs: `edgerpc_server.cpp` and `edgerpc_demo.cpp`.
4. Make the server accept **one** Unix domain socket connection.
5. Make the client send `hello` and receive `ack`.
6. Commit that working pair before adding floats, framing, or inference.

**Next session:** replace `hello` with a framed request that carries 1,024 floats, executes `2*x + 1`, and returns a verified response.

**Build principle:** Each phase should leave you with a compiling, demonstrable, tested project. Optimize only after the simpler version is correct.
