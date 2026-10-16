# Completion roadmap

This extends [the original build plan](../Plan.md) from the synchronous baseline at `1fb6ded`. Each phase must leave a buildable project, preserve existing correctness checks, document remaining limitations, and be committed in reviewable steps before pushing. A completed phase does not imply later phases are finished.

| Phase | Deliverables | Acceptance checks | State |
|---|---|---|---|
| 1. Async runtime | `infer_async`, sender/receiver threads, request-ID matching, bounded pending requests and server queue, explicit overload errors, orderly shutdown; sync uses the same transport | Multiple outstanding requests, reversed replies, concurrent callers, server/client queue saturation, disconnect and timeout with pending work, destructor/close with queued work; Release and ASan/UBSan pass | Complete on Linux x86_64 |
| 2. Shared-memory transport | RAII `shm_open`/`mmap` buffers, versioned metadata, bounds/name/size checks, same-user trust boundary, result completion over sockets | Independent processes map the same object; socket and SHM outputs agree for zero/max tensors; invalid offsets/sizes rejected; disconnect cleanup and restart verified | Pending |
| 3. Owned buffer pipeline | Fixed-capacity input/output pool, explicit FREE → FILLING → IN_FLIGHT → READY → FREE states, configurable 1/2/4 slots, preparation/execution overlap | Thousands of requests with two slots and delayed execution, no early reuse, reclamation after errors; timestamped overlap evidence; sanitizer checks | Pending |
| 4. Benchmark evidence | Socket sync/async and SHM/pipeline modes, queue/execute/buffer-wait timings, socket-byte counters, cold/reuse separation, repeated-trial scripts, CSV aggregation/charts and methodology | Compare equal size/workload/delay/workers/concurrency; at least five trials for published comparisons; raw samples and metadata retained; failures reported; measured p50/p95/p99 and throughput with variability | Pending |
| 5. Portability and reliability | Native macOS/Apple Silicon checks, Linux/ARM64 Docker image and smoke test, concurrency/crash/overload stress suite, optional thread sanitizer | Existing suites and representative IPC/SHM/pipeline runs pass on each claimed platform; CI results verified; unsupported platforms explicitly recorded | Pending; native Mac and ARM64 execution require available runners/hardware |
| 6. Documentation and release | Updated architecture/protocol/ownership docs, tradeoffs, truthful Qualcomm comparison, measured findings, reproducible quick start and release notes | Fresh-checkout reproduction, accurate implemented/future feature lists, every performance claim traceable to measurements; user-selected release/tag policy | Pending |

## Phase 1 implementation decisions

- Keep wire version 1 and the 8 MiB tensor limit. Add `OVERLOADED=3` as an error status; existing successful requests retain their encoding.
- One sender owns request writes and one receiver owns reply reads. `infer_sync` waits on `infer_async`, so responses cannot be consumed by competing readers.
- Bound **all outstanding client requests**, including queued and executing requests. Reject excess submissions with an already-failed future rather than block the caller or grow memory without limit.
- Bound queued server tasks separately from the tasks currently executing. Default to one worker. Extra workers are a separate CPU-scaling experiment and require a thread-safe backend.
- Match responses by ID, allowing completion order to differ from submission order. A valid remote error fails only its request; malformed replies or a broken connection fail all unresolved futures.
- On close/disconnect, stop submission, discard queued work, interrupt socket I/O, settle futures, and join owned threads before closing descriptors. Already-running backend work is not forcibly cancelled.
- Preserve the existing per-transfer I/O timeout. An idle client receiver waits for pending work rather than timing out unused connections. A silent peer fails pending calls; `future.wait_for` does not cancel remote execution. Per-request execution deadlines/cancellation are separate future work.
- This phase continues to serve one active client connection at a time. Multiple outstanding requests are supported within that connection; fair multi-client scheduling is outside this milestone.

## Work order and external dependencies

Finish and validate phase 1 first, then build shared memory on the same request lifecycle, followed by ownership/pipelining. Instrument timings as those stages appear. Start comparative reporting only after the modes are correct. Keep the local C++ build independent of hardware SDKs.

Core ML, Qualcomm QNN/FastRPC integration, real NPU profiling, demo videos, and interview rehearsal are optional follow-ups. Actual hardware claims require the corresponding device/SDK and measurements. Native macOS and ARM64 results cannot be inferred from this Linux x86_64 environment; prepare automation here and validate on those platforms when available.

## Progress log

- Synchronous baseline: implemented and pushed; Linux Release and ASan/UBSan suites passed; 4,000 baseline measured requests verified.
- Phase 1: implemented and verified on Linux x86_64. Release and ASan/UBSan each pass nine protocol/backend/metrics groups, 13 async/concurrency groups, and 22 process integration tests. Coverage includes 32 reversed replies, eight concurrent submitters, close/submission races, local/server overload, and a 1,000-request async benchmark stress run with 16 in-flight calls. The benchmark now accepts socket-sync/socket-async; comparative reports and stage instrumentation remain phase 4. Native macOS validation remains phase 5. Later phases remain open until their acceptance checks run.
