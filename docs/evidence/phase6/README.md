# Phase 6 fresh-checkout verification

An independent GitHub clone of
`f43d429341bb60631bbab220b722da6cb7a48d8b` passed the quick start on Linux x86_64.
It began without build artifacts or a chart Python environment. Host compiler,
CMake and Python were existing prerequisites, not newly provisioned hardware.
Subsequent commits finalize status/evidence; runtime, tests and quick-start
commands are unchanged.

| Check | Observed outcome |
|---|---|
| Fresh clone, Release configure/build | Pass; GCC 14.2.0, CMake 3.31.6, Python 3.12.14 |
| Full platform validator | Seven CTest suites pass; 18 checks pass |
| Interactive demos | Sync, async socket, fresh SHM and two-slot pool outputs verified |
| README C++ API example | Compiled against the freshly built library and verified all outputs |
| New `.venv` chart dependencies | Installed from the existing requirements; resolved versions retained |
| Comparison smoke | 15 completed trials; six SVG and six PNG charts exported |
| Retained measurement regeneration | All 665 trials validated; regenerated `summary.csv` / `trials.csv` exactly match retained bytes |
| Historical checksums | Benchmark and phase-5 evidence checksums pass |
| `BUILD_TESTING=OFF` configure/build | Pass |
| Cleanup/source status | Owned server exits gracefully, socket removed, clone's Git status clean |
| Native CI | All five native/container jobs pass this revision |

Smoke checks are functional only. No new comparative timing or performance claim
is derived from this run. Prior Linux ASan/UBSan and the unsuppressed TSan warning
remain recorded in [phase-5 evidence](../phase5/README.md); those sanitizer runs
were not repeated for documentation edits.

## Retained files

- `verification.json`: exact revision, platform/tools, commands, statuses,
  source hashes, chart package version and cleanup checks.
- `verification-logs.tar.gz`: command logs, full CTest transcript, platform
  validator JSON/logs/CSVs, all comparison smoke files/charts, regenerated
  aggregate tables/report/provenance, and the compiled README example's source.
- `ci-status.json`: observed public GitHub run/job/step conclusions and URLs.
- `reference-sources.json`: pinned upstream Qualcomm source links and hashes.
- `SHA256SUMS`: checksums for these retained files.

```sh
(cd docs/evidence/phase6 && sha256sum -c SHA256SUMS)
tar -tzf docs/evidence/phase6/verification-logs.tar.gz
```

Extract into a new empty directory for inspection. The archive omits duplicated
raw phase-4 inputs because the original checked archive remains in
`docs/evidence/benchmarks-v0.5/raw-trials.tar.gz`. Regenerated aggregate tables
are included. Original temporary paths in logs describe the observed run; they
are not current services. Configured proxy values and URL user information are
redacted from install logs if present. No credentials or process environment
dump is retained.

CI observations are from public job pages; full remote logs/artifacts were not
downloaded. Each native job's Build and Validate steps and each container job's
Build native container / Run container checks steps have conclusion `success`.
The workflow uploads full logs for normal authorized GitHub access. These
observations confirm the jobs, not a separate local ARM64/macOS reproduction.

## Release policy

The user selected **prepare release notes; leave tagging and publication for
later**. No tag, GitHub release, package or container image was published. CMake
remains 0.5.0. [Release notes](../../release-notes.md) define delivered scope,
compatibility and follow-ups; [the quick start](../../quickstart.md) contains
reproduction commands. Optional hardware integration, longer measurements,
videos and spoken interview rehearsal are not claimed as executed.
