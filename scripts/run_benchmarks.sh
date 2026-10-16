#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${1:-"$repo_dir/build"}
requests=${REQUESTS:-1000}
warmup=${WARMUP:-100}
sizes=${SIZES_KIB:-"4 64"}
output_dir="$repo_dir/results/$(date -u +%Y%m%dT%H%M%SZ)-$$"
mkdir -p "$output_dir"
runtime_dir=$(mktemp -d /tmp/edgerpc.XXXXXX)
server_pid=

cleanup() {
    if [ -n "$server_pid" ]; then
        kill -TERM "$server_pid" 2>/dev/null || true
        wait "$server_pid" 2>/dev/null || true
    fi
    rm -rf -- "$runtime_dir"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

"$build_dir/edgerpc_server" --socket "$runtime_dir/rpc.sock" --workers 1 >"$output_dir/server.log" 2>&1 &
server_pid=$!
attempt=0
until "$build_dir/edgerpc_demo" --socket "$runtime_dir/rpc.sock" --elements 1 --verify >/dev/null 2>&1; do
    attempt=$((attempt + 1))
    if [ "$attempt" -ge 100 ] || ! kill -0 "$server_pid" 2>/dev/null; then
        cat "$output_dir/server.log" >&2
        exit 1
    fi
    sleep 0.05
done

python3 - "$output_dir/metadata.json" "$build_dir" "$requests" "$warmup" "$sizes" <<'PY'
import json
import platform
from pathlib import Path
import subprocess
import sys
from datetime import datetime, timezone

compiler = subprocess.run(["c++", "--version"], capture_output=True, text=True, check=True)
metadata = {
    "timestamp_utc": datetime.now(timezone.utc).isoformat(),
    "system": platform.platform(), "machine": platform.machine(),
    "compiler": compiler.stdout.splitlines()[0], "build_dir": str(Path(sys.argv[2]).resolve()),
    "requests": sys.argv[3], "warmup": sys.argv[4], "sizes_kib": sys.argv[5].split(),
    "mode": "socket-sync", "inflight": 1, "workers": 1, "delay_us": 0, "instrumented": 1,
    "note": "CPU client and CPU simulator; not NPU measurements. Use a Release build.",
}
cache = Path(sys.argv[2]) / "CMakeCache.txt"
if cache.exists():
    metadata["cmake_settings"] = [line for line in cache.read_text().splitlines()
                                 if line.startswith(("CMAKE_BUILD_TYPE:", "CMAKE_CXX_COMPILER:",
                                                     "CMAKE_CXX_FLAGS:", "CMAKE_CXX_FLAGS_RELEASE:",
                                                     "EDGERPC_SANITIZERS:"))]
Path(sys.argv[1]).write_text(json.dumps(metadata, indent=2) + "\n")
PY

for workload in noop compute; do
    for size in $sizes; do
        "$build_dir/edgerpc_benchmark" --socket "$runtime_dir/rpc.sock" --mode socket-sync \
            --size-kib "$size" --inflight 1 --requests "$requests" --warmup "$warmup" \
            --workload "$workload" --csv "$output_dir/$workload-$size.csv" \
            >"$output_dir/$workload-$size.txt"
        printf '%s %s KiB: ' "$workload" "$size"
        cat "$output_dir/$workload-$size.txt"
    done
done
printf 'Saved raw samples, summaries, and metadata to %s\n' "$output_dir"
