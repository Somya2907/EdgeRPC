"""Validate repeated trials and aggregate per-trial statistics without pooling samples."""
import argparse
import csv
import gzip
import hashlib
import json
import math
from pathlib import Path
import statistics

KEYS = ("mode", "tensor_bytes", "inflight", "workers", "buffer_slots", "workload", "delay_us", "prepare_delay_us", "instrumented")
METRICS = ("p50_us", "p95_us", "p99_us", "throughput_rps", "initialization_us", "first_request_us",
           "prepare_mean_us", "queue_mean_us", "execute_mean_us", "buffer_wait_mean_us", "residual_mean_us",
           "socket_tx_bytes_per_request", "socket_rx_bytes_per_request")


def percentile(values, fraction):
    return sorted(values)[math.ceil(len(values) * fraction) - 1]


def number(value):
    result = float(value)
    if not math.isfinite(result) or result < 0:
        raise ValueError(f"invalid nonnegative finite measurement: {value}")
    return result


def load_trial(directory, record, requests, warmup):
    if record["status"] != "completed" or record["returncode"] != 0:
        raise ValueError(f"failed/incomplete trial: {record['name']}; inspect its stdout/stderr")
    path = directory / record["samples"]
    opener = gzip.open if path.suffix == ".gz" else open
    with opener(path, "rt", newline="") as stream:
        rows = list(csv.DictReader(stream))
    summary = record["summary"]
    config = record["config"]
    for key in ("mode", "tensor_bytes", "inflight", "workers", "buffer_slots", "instrumented", "delay_us"):
        if str(summary[key]) != str(config[key]):
            raise ValueError("summary/configuration mismatch")
    if int(summary["warmup"]) != warmup or int(summary["prepare_us"]) != config["prepare_delay_us"]:
        raise ValueError("warm-up/preparation summary mismatch")
    if len(rows) != requests or int(summary["completed"]) != requests or int(summary["failures"]) != 0:
        raise ValueError(f"wrong sample count or failures: {record['name']}")
    slots = int(config["buffer_slots"])
    if {int(row["request_id"]) for row in rows} != set(range(slots + warmup + 1, slots + warmup + requests + 1)):
        raise ValueError("duplicate, missing or unexpected request IDs")
    latencies, stages = [], []
    for row in rows:
        if None in row or row["status"] != "0" or row["run_id"] != summary["run_id"]:
            raise ValueError("malformed, failed or mixed-run sample")
        mismatch={key:(str(config[key]),row[key]) for key in KEYS if str(config[key]) != row[key]}
        if mismatch:
            raise ValueError(f"sample/configuration mismatch in {record['name']}: {mismatch}")
        latency = number(row["latency_us"])
        latencies.append(latency)
        if config["instrumented"] or slots:
            p0, p1, q, e0, e1 = (int(row[field]) for field in
                ("prepare_begin_ns", "prepare_end_ns", "queued_ns", "execute_begin_ns", "execute_end_ns"))
            if not 0 < p0 <= p1 <= q <= e0 <= e1:
                raise ValueError("invalid stage ordering")
            prepare, queue, execute, wait = (p1-p0)/1000, number(row["queue_us"]), number(row["execute_us"]), number(row["buffer_wait_us"])
            if abs(queue - (e0-q)/1000) > .002 or abs(execute - (e1-e0)/1000) > .002:
                raise ValueError("stage durations disagree with timestamps")
            residual = latency - prepare - queue - execute - wait
            if residual < -.01:
                raise ValueError("stages exceed end-to-end latency")
            stages.append((prepare, queue, execute, wait, max(0, residual)))
        elif any(row[field] != "" for field in ("queue_us", "execute_us", "buffer_wait_us", "prepare_begin_ns")):
            raise ValueError("uninstrumented stage must be unavailable")
    result = {key: config[key] for key in KEYS}
    for name, fraction in (("p50_us",.5), ("p95_us",.95), ("p99_us",.99)):
        result[name] = percentile(latencies, fraction)
        if abs(result[name] - number(summary[name])) > .002:
            raise ValueError("reported percentile disagrees with raw samples")
    seconds = number(summary["measured_seconds"])
    if seconds <= 0:
        raise ValueError("nonpositive wall duration")
    result["throughput_rps"] = number(summary["throughput_rps"])
    if not math.isclose(result["throughput_rps"], requests/seconds, rel_tol=.0001, abs_tol=.01):
        raise ValueError("throughput/wall duration mismatch")
    for key in ("initialization_us", "first_request_us"):
        result[key] = number(summary[key])
    tx = 52 if slots else 98 if config["mode"] == "shm-async" else 28 + config["tensor_bytes"]
    rx = 76 if slots else 28 + (0 if config["mode"] == "shm-async" else config["tensor_bytes"]) + 24 * config["instrumented"]
    # Cross-check measured syscalls against independent protocol arithmetic.
    for prefix, count in (("socket",requests), ("warmup",warmup)):
        if int(summary[prefix+"_tx_bytes"]) != tx*count or int(summary[prefix+"_rx_bytes"]) != rx*count:
            raise ValueError("measured socket traffic disagrees with wire contract")
    if int(summary["initialization_tx_bytes"]) != slots*102 or int(summary["initialization_rx_bytes"]) != slots*32:
        raise ValueError("registration traffic mismatch")
    result["socket_tx_bytes_per_request"], result["socket_rx_bytes_per_request"] = tx, rx
    for index, key in enumerate(("prepare_mean_us", "queue_mean_us", "execute_mean_us", "buffer_wait_mean_us", "residual_mean_us")):
        result[key] = statistics.mean(stage[index] for stage in stages) if stages else None
    result.update(trial=record["trial"], run_id=summary["run_id"], samples=requests)
    return result


def aggregate(directory, min_trials=5):
    directory = Path(directory)
    metadata = json.loads((directory / "metadata.json").read_text())
    manifest = json.loads((directory / "manifest.json").read_text())
    expected = {tuple(config[key] for key in KEYS) for config in metadata["configurations"]}
    seen, run_ids, sample_paths, groups, trials = set(), set(), set(), {}, []
    if manifest["status"] != "completed":
        raise ValueError("experiment failed or is incomplete; no comparative report generated")
    for record in manifest["runs"]:
        value = load_trial(directory, record, metadata["requests"], metadata["warmup"])
        key = tuple(value[item] for item in KEYS)
        identity = key, value["trial"]
        if (key not in expected or identity in seen or value["run_id"] in run_ids or record["samples"] in sample_paths or
                not 1 <= value["trial"] <= metadata["trials"]):
            raise ValueError("unexpected/duplicate configuration or trial")
        seen.add(identity)
        run_ids.add(value["run_id"])
        sample_paths.add(record["samples"])
        groups.setdefault(key, []).append(value)
        trials.append(value)
    if set(groups) != expected or any(len(values) != metadata["trials"] or len(values) < min_trials for values in groups.values()):
        raise ValueError("missing configurations or insufficient repetitions")
    summaries = []
    for key, values in sorted(groups.items()):
        summary = dict(zip(KEYS,key), trials=len(values), samples=sum(v["samples"] for v in values))
        for metric in METRICS:
            series = [v[metric] for v in values if v[metric] is not None]
            if series and len(series) != len(values):
                raise ValueError("mixed availability of stage metrics")
            for stat, fn in (("median", statistics.median), ("mean", statistics.mean), ("min", min), ("max", max),
                             ("stdev", lambda x: statistics.stdev(x) if len(x)>1 else 0)):
                summary[metric+"_"+stat] = fn(series) if series else None
        summaries.append(summary)
    return metadata, trials, summaries


def write_csv(path, rows):
    with path.open("w",newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]), lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)


def report(directory, min_trials=5, plots=True):
    directory = Path(directory)
    metadata, trials, summaries = aggregate(directory,min_trials)
    write_csv(directory/"trials.csv",trials)
    write_csv(directory/"summary.csv",summaries)
    lines = ["# Repeated transport measurements", "", f"{len(trials)} successful trials; {metadata['requests']} measured requests per trial; "
             f"{metadata['trials']} repetitions per configuration. All warm-up/measured outputs verified.", "",
             "Values below are the median of per-trial statistics, with the minimum–maximum range across trials. "
             "Raw latencies are not pooled across trials. Queue depth, slot capacity, and instrumentation remain explicit. "
             "CPU simulator on the recorded host; no native Mac, ARM64 or NPU inference is established.", "",
             "| Mode / slots | KiB | In flight | Workload | Delay μs | Timed | p50 μs | p95 μs | p99 μs [min–max] | requests/s [min–max] |",
             "|---|---:|---:|---|---:|---:|---:|---:|---:|---:|"]
    for value in summaries:
        def format_range(metric):
            return f"{value[metric+'_median']:.1f} [{value[metric+'_min']:.1f}–{value[metric+'_max']:.1f}]"
        lines.append(f"| {value['mode']} / {value['buffer_slots']} | {value['tensor_bytes']//1024} | {value['inflight']} | "
                     f"{value['workload']} | {value['delay_us']} | {value['instrumented']} | {value['p50_us_median']:.1f} | "
                     f"{value['p95_us_median']:.1f} | {format_range('p99_us')} | {format_range('throughput_rps')} |")
    lines += ["", "`summary.csv` retains mean, sample standard deviation, median, min and max of each per-trial metric, "
              "including initialization, first request, stage means and measured socket bytes. "
              "`trials.csv` preserves each trial's statistics. Manifest entries link exact commands, raw CSVs, summaries and errors.", ""]
    if metadata["trials"] < 5:
        lines += ["**Smoke validation only: fewer than five repetitions; do not publish performance comparisons.**", ""]
    if plots:
        from plot_results import plot
        plot(directory, summaries)
        lines += ["![Latency](latency.svg)", "![Throughput](throughput.svg)", "![Concurrency](concurrency.svg)",
                  "![Client startup](startup.svg)", "![Stages](stages.svg)", "![Socket traffic](traffic.svg)", ""]
    (directory/"report.md").write_text("\n".join(lines))
    # Report code can evolve without rewriting original measurement provenance.
    code=Path(__file__).resolve().parent
    provenance=dict(measurement_metadata_sha256=hashlib.sha256((directory/"metadata.json").read_bytes()).hexdigest(),
                    report_source_sha256={name:hashlib.sha256((code/name).read_bytes()).hexdigest()
                                          for name in ("report.py","plot_results.py")}, plots=plots)
    (directory/"report-provenance.json").write_text(json.dumps(provenance,indent=2)+"\n")
    return summaries


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory",type=Path)
    parser.add_argument("--min-trials",type=int,default=5)
    parser.add_argument("--no-plots",action="store_true")
    options=parser.parse_args()
    if options.min_trials < 1:
        parser.error("--min-trials must be positive")
    report(options.directory, options.min_trials, not options.no_plots)
