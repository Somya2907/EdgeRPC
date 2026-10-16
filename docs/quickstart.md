# Build and run EdgeRPC

EdgeRPC runs a CPU client and a CPU simulated executor in separate processes.
Both must run on the same host and, for shared memory, in the same IPC namespace.
No model download, hardware SDK, external service or credentials are needed.

## Prerequisites

Use Linux or macOS with a C++17 compiler, CMake 3.20+ and Python 3.8+.
There are no third-party C++ libraries. Ninja and Matplotlib are optional.

On macOS, install Command Line Tools if absent (`xcode-select --install`),
then install CMake and Python through your package manager, for example
`brew install cmake python`. On Ubuntu, the packages are `build-essential`,
`cmake` and `python3`. The prepared cloud environment already has these tools;
activate its CMake with `export PATH="/workspace/.edgerpc-tools/bin:$PATH"`.

## Fresh checkout and full functional run

Run in a directory without an existing `EdgeRPC` checkout:

```sh
git clone https://github.com/Somya2907/EdgeRPC.git
cd EdgeRPC
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DEDGERPC_SANITIZERS=OFF -DEDGERPC_THREAD_SANITIZER=OFF
cmake --build build --parallel 4
python3 scripts/validate_platform.py
```

The validator runs seven CTest suites, then starts and stops its own private
server. Expect 18 `PASS` lines and `Validated Linux/...` or `Validated Darwin/...`.
It checks socket/SHM/pool tensors from zero through 8 MiB, 1/2/4 pool slots,
four benchmark modes, concurrent callers, and graceful socket cleanup. Logs,
commands, hashes and `status=passed` are saved in a new `results/platform-*`
directory. A failing check returns nonzero. These short benchmark calls check
functionality; they do not establish performance.

`main` can advance. The [fresh-checkout record](evidence/phase6/README.md)
identifies the exact tested revision, tools and commands. Use that full revision
with `git checkout --detach REVISION` before building when reproducing that
snapshot. Use a separate build directory when changing CMake generators.
To run only the existing suites, use `ctest --test-dir build --output-on-failure`.

## Interactive two-process demo

The following uses a private directory with a short socket path, including on
macOS where `$TMPDIR` can be too long for a Unix socket. In terminal 1, from
the checkout:

```sh
edgerpc_demo_dir=$(mktemp -d /tmp/erpc-demo.XXXXXX)
printf 'Socket path for terminal 2: %s/rpc.sock\n' "$edgerpc_demo_dir"
./build/edgerpc_server --socket "$edgerpc_demo_dir/rpc.sock" --workers 1
```

In terminal 2, set `edgerpc_socket` to the printed path, then run:

```sh
edgerpc_socket=/tmp/erpc-demo.EXAMPLE/rpc.sock
./build/edgerpc_demo --socket "$edgerpc_socket" --mode sync --elements 1024 --verify
./build/edgerpc_demo --socket "$edgerpc_socket" --mode async --requests 32 --verify
./build/edgerpc_demo --socket "$edgerpc_socket" --mode async --transport shm --requests 32 --verify
./build/edgerpc_demo --socket "$edgerpc_socket" --mode async --transport pool \
  --buffer-slots 2 --requests 32 --verify
```

Replace `EXAMPLE` with the actual printed directory. Every result must say
`status=OK verified=true`; the default compute operation is `2*x+1`.
Add `--workload noop` to return the input unchanged. Stop terminal 1 with Ctrl-C,
then run `rmdir "$edgerpc_demo_dir"` there. The server removes its own socket
on graceful exit. All four binaries support `--help`.

The default server has one worker and 32 waiting tasks. Client capacity is 32
total outstanding calls. Extra server workers run CPU work in parallel; they
do not model a single serialized NPU. I/O deadlines are per header/payload,
not per inference; `future.wait_for` does not cancel execution. See
[lifecycle and overload behavior](architecture.md).

## Short comparison smoke and report regeneration

Runtime tests and CSV analysis use only the Python standard library. For chart
export, create a separate Python environment without changing system packages:

```sh
python3 -m venv .venv
.venv/bin/python -m pip install -r benchmarks/requirements.txt
.venv/bin/python scripts/run_comparisons.py --smoke
```

Expect 15 completed short trials, no failures, a validated report and six SVG/PNG
chart pairs in a new `results/comparison-*` directory. Use `python3
scripts/run_comparisons.py --smoke --no-plots` to check the runner without chart
dependencies. Smoke uses one repetition and is insufficient for performance
claims. Full five-trial experiments use `scripts/run_comparisons.py` without
`--smoke`; avoid concurrent builds/tests while measuring.

To regenerate the existing five-trial report without rerunning measurements:

```sh
mkdir -p results/review-phase4
tar -xzf docs/evidence/benchmarks-v0.5/raw-trials.tar.gz -C results/review-phase4
python3 benchmarks/report.py results/review-phase4 --no-plots
```

Use a new empty destination for each extraction. With Matplotlib installed,
omit `--no-plots` to export charts too. [Measured findings](benchmark-results.md)
and [methodology](benchmarks.md) define the host, controls and limitations.

## Container alternative

With Docker available on a native x86_64 or ARM64 Linux container host:

```sh
docker build -f docker/Dockerfile -t edgerpc:validation .
docker run --rm --shm-size=256m edgerpc:validation
```

The image runs client and server as the same unprivileged user and performs the
full validator. Docker Desktop on Apple Silicon runs Linux ARM64 inside its VM;
native Darwin checks are separate. See [portability](portability.md) for evidence,
SHM capacity, retained container logs and the cloud's proxy/config-directory
setup. Container timing is not target-device timing.

## Common failures

| Symptom | Action |
|---|---|
| CMake too old or missing | Install CMake 3.20+; in the cloud activate the prepared tools above |
| Connection refused / missing socket | Check the printed private path and server output; complete a verified demo request |
| SHM allocation failure | Check available backing storage and umask; reduce tensor size/concurrency or enlarge Docker `--shm-size` |
| Comparison rejects build settings | Reconfigure Release with both sanitizer options OFF, then rebuild |
| Matplotlib or `venv` unavailable | Use `--no-plots`; install your platform's Python venv support for charts |
| Optional TSan warning in `async` | Read the retained standard-library reproducer and unsuppressed result in [portability](portability.md#sanitizers) |

Use trusted clients under one user. Do not truncate/mutate published regions or
delete unrelated socket/SHM names. Crash recovery and possible pre-admission
orphan names are documented in [shared-memory ownership](shared-memory.md).
