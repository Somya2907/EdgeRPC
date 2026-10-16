#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${1:-"$repo_dir/build"}
ctest --test-dir "$build_dir" --output-on-failure
