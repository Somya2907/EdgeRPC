"""Validate slot reuse and cross-process timestamp overlap in pipeline CSVs."""
import bisect
import csv
import json
from pathlib import Path
import sys


def analyze(path):
    with path.open(newline="") as stream:
        rows = list(csv.DictReader(stream))
    if not rows:
        raise ValueError(f"{path}: no samples")
    previous = {}
    intervals = []
    for row in rows:
        if row["status"] != "0" or None in row:
            raise ValueError(f"{path}: failed or malformed sample")
        slot, gen = int(row["slot"]), int(row["generation"])
        p0, p1, q, e0, e1 = (int(row[field]) for field in
            ("prepare_begin_ns", "prepare_end_ns", "queued_ns", "execute_begin_ns", "execute_end_ns"))
        if not (0 < p0 <= p1 <= q <= e0 <= e1):
            raise ValueError(f"{path}: inconsistent clock ordering")
        if slot in previous and (gen <= previous[slot][0] or p0 < previous[slot][1]):
            raise ValueError(f"{path}: stale generation or early slot reuse")
        previous[slot] = gen, e1
        intervals.append((slot, p0, p1, e0, e1))
    # One execution worker gives ordered, non-overlapping execution intervals.
    executions = sorted((e0, e1, slot) for slot, _, _, e0, e1 in intervals)
    if any(a[1] > b[0] for a, b in zip(executions, executions[1:])):
        raise ValueError(f"{path}: analyzer requires a single serialized execution worker")
    ends = [item[1] for item in executions]
    overlaps = 0
    example = None
    for slot, p0, p1, _, _ in intervals:
        index = bisect.bisect_right(ends, p0)
        while index < len(executions) and executions[index][0] < p1:
            e0, e1, executing_slot = executions[index]
            if slot != executing_slot:
                overlaps += 1
                if example is None:
                    example = {"preparing_slot": slot, "executing_slot": executing_slot,
                               "overlap_begin_ns": max(p0, e0), "overlap_end_ns": min(p1, e1)}
                break
            index += 1
    return {"samples": len(rows), "slots_used": sorted(previous), "stale_or_early_reuses": 0,
            "preparations_overlapping_other_slot_execution": overlaps, "overlap_example": example}


if __name__ == "__main__":
    directory = Path(sys.argv[1])
    result = {path.stem: analyze(path) for path in sorted(directory.glob("slots-*.csv"))}
    if not result:
        raise ValueError("no slots-*.csv files")
    (directory / "overlap.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))
