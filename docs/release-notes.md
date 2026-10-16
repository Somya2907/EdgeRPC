# Mock-runtime milestone release notes

**Status:** prepared, unpublished. The selected release policy is to prepare
notes and verification evidence and leave tagging/publication for later. No Git
tag, GitHub release, package or container image is published by this phase.
CMake's project version remains `0.5.0`; this document is not a `1.0` release or
a stable C++ ABI promise. `v1.0-mock-runtime` in the original plan is a future
tag name, subject to an explicit release decision.

## Delivered scope

| Area | Implemented behavior | Evidence / contract |
|---|---|---|
| RPC | C++17 library, separate client/server, versioned Unix stream frames, partial I/O and validation | [Protocol](protocol.md), 11 protocol groups |
| Async runtime | Request-ID futures, concurrent callers, bounded client/server queues, overload, orderly close | [Architecture](architecture.md), 30 Linux async groups |
| Shared tensors | Per-request POSIX SHM; UID/mode/name/size/range validation and unlink-at-admission | [SHM contract](shared-memory.md), six buffer groups and independent process tests |
| Reuse/pipeline | Registered 1/2/4 slots, generations and explicit ownership, preparation/execution overlap | [Pipeline](pipeline.md), four pool groups and [retained traces](pipeline-evidence.md) |
| Measurement | Four modes, warm-up separation, stage timings, actual socket bytes, five-trial reports/charts | [Findings](benchmark-results.md), 665 retained trials / 133 configurations |
| Reliability | Concurrent mixed calls, thousands of two-slot submissions, close/crash/restart, repeated overload, path preservation | Nine reliability tests; [platform evidence](portability.md) |
| Portability | Native Linux x86_64/ARM64 and macOS Apple Silicon; native Linux validation containers on both architectures | Seven Release suites and 18 platform checks per native environment |
| Documentation | Reproducible quick start, diagrams, design tradeoffs, Qualcomm comparison and future backend design | [Quick start](quickstart.md), [mapping](qualcomm_mapping.md), [backend design](backend-extension.md) |

The final Linux suite contains 115 checks/groups; macOS contains 114 because
allocation-failure injection is Linux-only. Linux ASan/UBSan passes seven suites.
The final optional unsuppressed TSan run passes six suites and flags `async`
with a system libstdc++ exception-lifetime warning. The same report reproduces
in a standalone standard-library program. This is a documented instrumentation
limitation, not clean TSan validation; see [the retained diagnosis](portability.md#sanitizers).

## Findings and compatibility

The phase-4 Linux x86_64 measurements show that reuse can lower warm latency,
while fresh SHM can lose to sockets for small tensors. Five short shared-cloud
trials expose broad tail/throughput variability; these results support no
universal speedup or device/NPU claim. Raw samples, commands, metadata and
checksums accompany every published result. See [the measured table and charts](benchmark-results.md).

Wire version 1 remains. SHM, registration, pooling and timing add types/flags;
there is no protocol negotiation or downgrade. Deploy matching peers when using
these features. Socket float payloads are big-endian; mapped tensors are
native-endian float32 on one host. The 8 MiB tensor limit is unchanged. No
installable package, `find_package` export, public C++ ABI stability or model
graph format is promised.

Native testing fixed Darwin signal macro compilation, Apple ARM64 ctypes
variadic SHM mode handling, and Darwin blocked-write deadlines. `write_all`
leaves supplied Darwin descriptors nonblocking; production sockets already use
that mode. These changes preserve the existing frame layouts.

## Known limits and follow-ups

- Both endpoints execute on CPUs. The only backend is `MockExecutor`; there is
  no QNN/Core ML/FastRPC integration, model loading, DSP or NPU execution.
- The server serves one active client connection. Queue limits count requests,
  not memory. No fair multi-client scheduling or tenant isolation is provided.
- Same-user trusted-client usage is required. Metadata validation does not stop
  a malicious owner from changing mapped storage. Killing a creator before
  admission/registration can leave a name needing identified-name recovery.
- I/O deadlines are not per-request execution deadlines. There is no implicit
  retry or forced cancellation of running work.
- Per-process CPU/RSS, observed peak active requests and separate send/receive
  duration profiling remain unmeasured. Longer target-device experiments and
  an instrumented C++ runtime for TSan remain follow-ups.
- macOS Intel, Windows and other operating systems are unvalidated. Container
  builds use moving Ubuntu/apt inputs and do not promise identical image hashes.
- Demo videos and spoken interview rehearsal are optional owner activities;
  no recording or rehearsal is claimed by this implementation.

## Verification and later publication

The [fresh-checkout evidence](evidence/phase6/README.md) records cloning from
GitHub, building without old artifacts, the full platform validator, interactive
demo commands, chart smoke and regeneration of the retained performance report.
[Phase-5 evidence](evidence/phase5/README.md) retains native CI and sanitizer
results. The [roadmap](completion-plan.md) is the current status source; the
original `Plan.md` remains a historical design checklist.

Before a later release, select the exact version/tag and revision, rerun the
documented checks and CI for that revision, and decide whether a Git tag alone
or a GitHub release is intended. Publish only after an explicit instruction;
do not reinterpret this prepared note as publication authorization.
