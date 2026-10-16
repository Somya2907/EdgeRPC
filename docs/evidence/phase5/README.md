# Phase 5 validation evidence

This directory records correctness/reliability checks, not comparative timings.
The tested runtime and test sources are code revision
`f2542f3ca39bbf09810baefd841876fa1fe284c6`. Subsequent phase-5 documentation and
evidence commits do not change those runtime/test sources.

| File | Contents |
|---|---|
| `ci-status.json` | Public GitHub job/step observations, exact commit, observation time and URLs; all five jobs passed |
| `provenance.json` | Local compiler/platform, commands, expected outcomes, source hashes and Docker image metadata |
| `local-validation.tar.gz` | Host/container validator JSON, all 18 check logs and four benchmark smoke CSVs each; local Release/ASan/TSan build and CTest logs; repeated-stress output; comparison smoke/rejection logs |
| `tsan-standard-library.cpp` | Standalone promise/future/system_error reproducer with no EdgeRPC dependencies |
| `tsan-standard-library.log` | Its unsuppressed warning, exit status 66 recorded in provenance |
| `SHA256SUMS` | SHA-256 checksums for the retained files |

Verify and inspect from the repository root:

```sh
(cd docs/evidence/phase5 && sha256sum -c SHA256SUMS)
tar -tzf docs/evidence/phase5/local-validation.tar.gz
```

Extract into a new empty directory when inspecting logs. Files retain the paths
and commands of their original runs; temporary paths are not current services.
The archive's `host/validation.json` and `container/validation.json` both report
`status=passed`, 18 passed checks and zero failed checks. Benchmark CSVs here are
short functional samples; use [phase-4 evidence](../../benchmark-results.md) for
published performance analysis.

Local Release and ASan/UBSan pass all seven suites. Async, pool and reliability
also passed ten consecutive Release repetitions each. The final **unsuppressed
TSan run fails one of seven suites**, reporting a libstdc++ exception-lifetime
warning in `async`. All 30 async correctness groups themselves pass. The same
warning reproduces with only the C++ standard library:

```sh
c++ -std=c++17 -g -fsanitize=thread -pthread \
  docs/evidence/phase5/tsan-standard-library.cpp -o /tmp/edgerpc-tsan-reproducer
/tmp/edgerpc-tsan-reproducer
```

The diagnosis is an instrumentation limitation in the system libstdc++ exception
reference-count implementation. GCC's corresponding implementation is
[`libsupc++/eh_ptr.cc`](https://github.com/gcc-mirror/gcc/blob/releases/gcc-14.2.0/libstdc++-v3/libsupc++/eh_ptr.cc).
This does not establish clean TSan validation; an instrumented C++ runtime is a
follow-up. Neither report is suppressed, and no production change or test
weakening was made to avoid it. See [the portability report](../../portability.md).

CI status was observed through the public run/job pages because API/log downloads
were unavailable to this session. `ci-status.json` records the required Build
and Validate steps (native jobs), and Build native container / Run container
checks steps (container jobs). All have conclusion `success`; the native
validator checks expected OS/architecture and runs the full suites before its
process checks. This record is not a copy of remote artifacts. Full remote logs
are uploaded by CI for authorized GitHub download. Deprecation notices for the
artifact action are warnings, not failed checks.

No Docker image, release or tag was published. The local image metadata identifies
the built validation image and resolved Ubuntu base digest. Future builds can
resolve updated Ubuntu/apt contents; this archive does not claim bit-for-bit
container reproducibility. Unsupported platforms and remaining runtime limits
are listed in the portability report.
