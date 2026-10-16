#!/bin/sh
set -eu
repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${1:-"$repo_dir/build"}
requests=${REQUESTS:-1000}
warmup=${WARMUP:-100}
prepare_us=${PREPARE_US:-1000}
delay_us=${DELAY_US:-1000}
output_dir="$repo_dir/results/pipeline-$(date -u +%Y%m%dT%H%M%SZ)-$$"
mkdir -p "$output_dir"
runtime_dir=$(mktemp -d /tmp/erpc-pipe.XXXXXX)
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
"$build_dir/edgerpc_server" --socket "$runtime_dir/rpc.sock" --workers 1 --delay-us "$delay_us" \
    >"$output_dir/server.log" 2>&1 &
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
python3 - "$output_dir/metadata.json" "$build_dir" "$requests" "$warmup" "$prepare_us" "$delay_us" <<'PY'
import json, platform, subprocess, sys
from datetime import datetime, timezone
from pathlib import Path
build = Path(sys.argv[2]).resolve()
metadata = {
    "timestamp_utc": datetime.now(timezone.utc).isoformat(),
    "system": platform.platform(), "machine": platform.machine(),
    "compiler": subprocess.check_output(["c++", "--version"], text=True).splitlines()[0],
    "build_dir": str(build), "requests": int(sys.argv[3]), "warmup": int(sys.argv[4]),
    "prepare_us": int(sys.argv[5]), "delay_us": int(sys.argv[6]),
    "mode": "shm-double-buffer", "slots": [1, 2, 4], "inflight": 4, "workers": 1, "instrumented": 1,
    "tensor_bytes": 4096, "workload": "compute",
    "note": "One trial per slot count; synthetic CPU preparation/execution, not hardware or a repeated performance study.",
    "cmake_settings": [line for line in (build / "CMakeCache.txt").read_text().splitlines()
                       if line.startswith(("CMAKE_BUILD_TYPE:", "CMAKE_CXX_COMPILER:", "CMAKE_CXX_FLAGS:",
                                           "CMAKE_CXX_FLAGS_RELEASE:", "EDGERPC_SANITIZERS:",
                                           "EDGERPC_THREAD_SANITIZER:"))],
}
Path(sys.argv[1]).write_text(json.dumps(metadata, indent=2) + "\n")
PY
for slots in 1 2 4; do
    "$build_dir/edgerpc_benchmark" --socket "$runtime_dir/rpc.sock" --mode shm-double-buffer \
        --buffer-slots "$slots" --inflight 4 --workers 1 --size-kib 4 --workload compute \
        --requests "$requests" --warmup "$warmup" --prepare-us "$prepare_us" \
        --csv "$output_dir/slots-$slots.csv" >"$output_dir/slots-$slots.txt"
    cat "$output_dir/slots-$slots.txt"
done
python3 "$repo_dir/scripts/analyze_pipeline.py" "$output_dir"
printf 'Saved pipeline samples and metadata to %s\n' "$output_dir"
