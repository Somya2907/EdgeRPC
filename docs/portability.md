# Portability and reliability validation

Phase 5 validates the existing CPU simulator, socket transport, per-request SHM,
and registered 1/2/4-slot pools. Native platform execution includes Linux x86_64,
Linux ARM64 and macOS on Apple Silicon. Performance findings in
[the phase-4 report](benchmark-results.md) still describe their recorded Linux
x86_64 host; correctness checks on another platform do not extend those timings.

## Verified platforms

| Environment | Execution | Result |
|---|---|---|
| Cloud Linux x86_64, GCC 14.2 | Release | Seven CTest suites and 18 platform checks pass |
| Same cloud host | Debug ASan/UBSan | Seven suites pass |
| Same cloud host | Debug ThreadSanitizer | Six suites pass; async reports a C++ library exception-lifetime warning (see below) |
| GitHub `ubuntu-24.04`, x86_64 | Native Release | Seven suites and platform checks pass |
| GitHub `ubuntu-24.04-arm`, ARM64 | Native Release | Seven suites and platform checks pass |
| GitHub `macos-15`, ARM64 | Native Apple Silicon Release | Seven suites and platform checks pass |
| Ubuntu 24.04 Docker, x86_64 | Native build and unprivileged runtime | Build-time CTest and runtime platform checks pass |
| Ubuntu 24.04 Docker, ARM64 | Native ARM64 GitHub runner | Build-time CTest and runtime platform checks pass |

The first complete native matrix passed at
[`a58604d`](https://github.com/Somya2907/EdgeRPC/actions/runs/38967338954).
The [retained evidence](evidence/phase5/README.md) records the final code revision,
job URLs and step conclusions, plus local logs and source/binary hashes.
The architecture is checked explicitly by the native validator. Container jobs
build on native runners; they do not use QEMU. The local cloud Docker daemon
cannot execute ARM64 images, so ARM64 evidence comes from the native CI runner.
macOS Intel, Windows, other operating systems, and macOS sanitizer builds have
not been validated. Apple Silicon validation uses a hosted ARM64 runner, not a
named consumer device or a hardware accelerator.

## Reproduce the platform checks

Requires CMake 3.20+, a C++17 compiler and Python 3.8+. Run from the checkout:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DEDGERPC_SANITIZERS=OFF -DEDGERPC_THREAD_SANITIZER=OFF
cmake --build build --parallel 4
python3 scripts/validate_platform.py
```

The validator creates a new `results/platform-*` directory, runs all CTest suites,
starts its own four-worker server in a private directory, and verifies:

- Zero, 1,024 and maximum 2,097,152-element tensors over socket, SHM and pool.
- Thirty-two asynchronous requests with each of 1/2/4 pool slots.
- All four benchmark modes, each with 64 measured calls and eight warm-ups.
- Eight concurrent callers making 1,024 mixed-transport requests.
- Graceful server shutdown and removal of its own socket path.

There are 18 recorded checks including CTest. `validation.json` records status,
commands, return codes, native architecture, build options and source/binary
SHA-256 hashes; adjacent logs and CSVs retain outputs. A failure returns nonzero.
Use `--output NEW_DIRECTORY` to choose a destination; existing directories are
refused. CI additionally passes `--expected-system Linux|Darwin` and
`--expected-machine x86_64|arm64` to reject a mismatched runner.

The seven CTest suites contain 115 Linux checks/groups: 11 protocol, 30 async,
six shared-memory, four pool, 39 process integration, 16 benchmark-analysis and
nine reliability. macOS has 114: the allocation-failure injection in async is
Linux-only. Assertions remain active in Release builds.

## Concurrency and failure coverage

`edgerpc_stress` shares one client among concurrent callers. Every input varies
with its submission ordinal; successful outputs and unique response IDs are
checked. Each thread keeps a bounded window of futures. Completion accounting
requires every submission to settle before the threads join and the client is
destroyed. Unexpected overload, protocol errors and incorrect outputs fail the
run. Disconnect failures are allowed only when explicitly requested for a crash
or close scenario.

For example, against an existing private server with sufficient queue capacity:

```sh
./build/edgerpc_stress --socket /path/to/private/rpc.sock --transport mixed \
  --threads 8 --requests 128 --window 4 --buffer-slots 2
```

`--requests` and `--window` are per thread; the example checks 1,024 total calls.
Run the self-contained process tests instead when no server is running:

```sh
ctest --test-dir build -R '^reliability$' --output-on-failure
```

The nine reliability cases cover mixed transports with four workers; 2,048
submissions sharing two slots; close/submission races for all three transports;
server SIGKILL with pending socket/SHM/pool work and restart; killing a registered
pool client followed by a new client; twelve reconnects; ten overload/recovery
cycles; preserving a replacement socket-path file; and refusing a symlink without
modifying its target. Existing suites retain malformed-wire, invalid mapping,
reversed completion, timeout and maximum tensor coverage.

Release async, pool and reliability suites also passed ten consecutive
repetitions each on the cloud host:

```sh
ctest --test-dir build --repeat until-fail:10 \
  -R '^(async|buffer_pool|reliability)$' --output-on-failure
```

Tests own their subprocesses and private paths. They do not scan/delete unrelated
SHM objects. Killing a creator before admission/registration can still leave an
orphan name; [identified-name recovery](shared-memory.md) remains necessary.
There is no forced cancellation of backend execution or fair multi-client
scheduling.

## Native Docker workflow

```sh
docker build -f docker/Dockerfile -t edgerpc:validation .
docker run --rm --shm-size=256m edgerpc:validation
```

The multi-stage Ubuntu 24.04 image compiles and runs CTest in its build stage.
The default runtime command runs the same platform validator as UID 10001.
Client and server share that container's IPC namespace. `--shm-size=256m`
leaves space for the maximum tensor and pool checks; this is separate from
normal process memory. The validation image retains tests and build files for
CTest and is not a minimal deployment image. Base tags and apt packages are not
lockfiles; evidence records the local image ID and resolved base digest.

To keep runtime logs, use a unique container name, copy its output directory,
then remove that container:

```sh
docker run --name edgerpc-check --shm-size=256m edgerpc:validation
docker cp edgerpc-check:/tmp/edgerpc-validation ./container-validation
docker rm edgerpc-check
```

On a native ARM64 Docker host, the same commands select ARM64 Ubuntu and produce
native ARM64 binaries. Containers do not validate Darwin behavior; the native
macOS CI job does that separately. No image is published to a registry by CI.

In this cloud workspace, use a writable `DOCKER_CONFIG` such as
`/workspace/.edgerpc-docker` if the default directory is read-only. Docker build
networking may also need the environment's existing proxy route; the saved cloud
startup instructions record the tested forwarding procedure without storing
proxy values. No package-signature or TLS checks were disabled.

## Sanitizers

Use separate build directories; enabling both sanitizer options is rejected:

```sh
cmake -S . -B build-sanitize -DCMAKE_BUILD_TYPE=Debug \
  -DEDGERPC_SANITIZERS=ON -DEDGERPC_THREAD_SANITIZER=OFF
cmake --build build-sanitize --parallel 4
ctest --test-dir build-sanitize --output-on-failure

cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug \
  -DEDGERPC_SANITIZERS=OFF -DEDGERPC_THREAD_SANITIZER=ON
cmake --build build-tsan --parallel 4
ctest --test-dir build-tsan --output-on-failure
```

ASan/UBSan passes all seven suites on the recorded Linux GCC 14.2 host. The final
unsuppressed ThreadSanitizer run passes six suites but flags `async` while the
allocation-failure test reads a caught `std::system_error`: the conflicting free
is in libstdc++ `std::__exception_ptr::exception_ptr::_M_release`. An earlier run
passed seven suites, so a single green run did not establish stable readiness.

A standalone 1,000-iteration program using only standard-library promises,
futures and exceptions reproduces the same warning and exits 66. It contains no
EdgeRPC code. GCC's exception reference counting lives inside the uninstrumented
system libstdc++; its synchronization is not visible to this sanitizer build.
This supports a library instrumentation limitation rather than an identified
EdgeRPC race, but the final unsuppressed suite is still recorded as failed.
The [evidence](evidence/phase5/README.md) retains both reports, the reproducer,
and commands. No warning suppression, assertion removal, ASLR or sysctl change
was used. Validation with a fully instrumented C++ runtime remains a follow-up.

ThreadSanitizer checks thread synchronization within a process; it does
not prove interprocess SHM ordering. The independent-process output, ownership,
generation and crash checks provide separate coverage. Sanitizer availability
depends on the compiler/runtime and OS. Comparative timing requires Release with
both sanitizer options OFF; the comparison runner rejects sanitized builds.

## Issues found by native execution

- Darwin defines `sigemptyset` as a macro. Global qualification (`::sigemptyset`)
  broke the server build; the app now calls it without that qualifier.
- Darwin's `shm_open` creation mode is a variadic argument. On Apple ARM64,
  Python ctypes must declare only the two fixed arguments and pass an explicit
  `c_uint` mode. The independent Python mapping test now does this and verifies
  the created permissions before using the object. Linux retains its fixed
  three-argument declaration.
- Darwin's socket send path can wait for buffer space despite `MSG_DONTWAIT`.
  `write_all` now ensures `O_NONBLOCK` on Darwin before using its poll/deadline
  loop, including when handed a blocking socket. This descriptor setting remains
  in effect. Production connections already use nonblocking sockets. The native
  blocked-write test now returns at its deadline instead of hanging. See Apple's
  [`sosendcheck` implementation](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/kern/uipc_socket.c).

The complete native suites and maximum-size process checks passed after these
fixes. Phase 6 subsequently completed [documentation and fresh-checkout
verification](evidence/phase6/README.md), with release notes prepared and
tagging/publication deferred by the user's policy. Hardware backends and actual
accelerator measurements remain separate follow-ups.
