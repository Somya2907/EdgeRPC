#!/usr/bin/env python3
"""Run randomized repeated Release trials, retaining failures and exact commands."""
import argparse
from datetime import datetime, timezone
import hashlib
import importlib.util
import importlib.metadata
import json
import os
from pathlib import Path
import platform
import random
import signal
import subprocess
import sys
import tempfile
import time

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/"benchmarks"))
from report import report


def save(path, value):
    temporary=path.with_suffix(".tmp")
    temporary.write_text(json.dumps(value,indent=2)+"\n")
    temporary.replace(path)


def configurations(options):
    result=[]
    for size in options.sizes_kib:
        for workload in options.workloads:
            for delay in options.delays_us:
                for window in options.inflight:
                    for mode in ("socket-sync","socket-async","shm-async","shm-double-buffer"):
                        if mode=="socket-sync" and window!=1:
                            continue
                        for slots in options.slots if mode=="shm-double-buffer" else [0]:
                            result.append(dict(mode=mode,tensor_bytes=size*1024,inflight=window,workers=1,
                                               buffer_slots=slots,workload=workload,delay_us=delay,prepare_delay_us=0,instrumented=1))
    # An otherwise identical timing-off control measures observer overhead.
    controls=[dict(value,instrumented=0) for value in result if value["mode"]=="socket-async" and
              value["tensor_bytes"]==min(options.sizes_kib)*1024 and value["inflight"]==min(options.inflight) and
              value["delay_us"]==min(options.delays_us) and value["workload"]==options.workloads[0]]
    return result+controls


class Servers:
    def __init__(self, build, output, runtime):
        self.build,self.output,self.runtime=build,output,Path(runtime)
        self.running={}

    def get(self, delay):
        if delay in self.running:
            if self.running[delay][0].poll() is not None:
                raise RuntimeError("server exited between trials")
            return self.running[delay][2]
        path=self.runtime/f"delay-{delay}.sock"
        log=(self.output/f"server-delay-{delay}.log").open("w")
        command=[str(self.build/"edgerpc_server"),"--socket",str(path),"--workers","1","--queue-capacity","32",
                 "--delay-us",str(delay)]
        process=subprocess.Popen(command,stdin=subprocess.DEVNULL,stdout=log,stderr=log)
        self.running[delay]=process,log,path,command
        deadline=time.monotonic()+5
        while time.monotonic()<deadline and process.poll() is None:
            probe=subprocess.run([str(self.build/"edgerpc_demo"),"--socket",str(path),"--elements","1","--verify"],
                                 text=True,capture_output=True,timeout=2)
            if probe.returncode==0 and "status=OK verified=true" in probe.stdout:
                return path
            time.sleep(.02)
        raise RuntimeError(f"server readiness failed; inspect server-delay-{delay}.log")

    def close(self):
        for process,log,_,_ in self.running.values():
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            log.close()


def run(options):
    if not options.no_plots and importlib.util.find_spec("matplotlib") is None:
        raise RuntimeError("charts require: python3 -m pip install -r benchmarks/requirements.txt (or use --no-plots)")
    build=options.build_dir.resolve()
    cache=(build/"CMakeCache.txt").read_text()
    if "CMAKE_BUILD_TYPE:STRING=Release" not in cache or "EDGERPC_SANITIZERS:BOOL=OFF" not in cache:
        raise ValueError("comparisons require a Release build with sanitizers OFF")
    output=options.output or ROOT/"results"/("comparison-"+datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")+f"-{os.getpid()}")
    output=output.resolve()
    output.mkdir(parents=True,exist_ok=False)
    configs=configurations(options)
    metadata=dict(timestamp_utc=datetime.now(timezone.utc).isoformat(),system=platform.platform(),machine=platform.machine(),
                  logical_cpus=os.cpu_count(),cpu_affinity=sorted(os.sched_getaffinity(0)) if hasattr(os,"sched_getaffinity") else None,
                  build_dir=str(build),trials=options.trials,requests=options.requests,warmup=options.warmup,seed=options.seed,
                  configurations=configs,cmake_settings=[line for line in cache.splitlines() if line.startswith(
                      ("CMAKE_BUILD_TYPE:","CMAKE_CXX_COMPILER:","CMAKE_CXX_FLAGS:","CMAKE_CXX_FLAGS_RELEASE:","EDGERPC_SANITIZERS:"))],
                  note="CPU simulator in shared cloud; randomized sequential trials; no NPU/Mac/ARM64 results.")
    if not options.no_plots:
        metadata["matplotlib_version"]=importlib.metadata.version("matplotlib")
    metadata["cpu_model"]=platform.processor()
    if Path("/proc/cpuinfo").exists():
        metadata["cpu_model"]=next((line.split(":",1)[1].strip() for line in Path("/proc/cpuinfo").read_text().splitlines()
                                    if line.startswith("model name")),metadata["cpu_model"])
    metadata["cgroup_limits"]={name: (Path("/sys/fs/cgroup")/name).read_text().strip()
                               for name in ("cpu.max","memory.max") if (Path("/sys/fs/cgroup")/name).is_file()}
    compiler=next(line.split("=",1)[1] for line in cache.splitlines() if line.startswith("CMAKE_CXX_COMPILER:FILEPATH="))
    metadata["compiler"]=subprocess.check_output([compiler,"--version"],text=True).splitlines()[0]
    metadata["git_head"]=subprocess.check_output(["git","rev-parse","HEAD"],cwd=ROOT,text=True).strip()
    metadata["git_dirty"]=bool(subprocess.check_output(["git","status","--porcelain"],cwd=ROOT,text=True))
    sources=[ROOT/"CMakeLists.txt",*ROOT.glob("src/*.cpp"),*ROOT.glob("include/edgerpc/*.hpp"),
             *ROOT.glob("apps/*"),*ROOT.glob("benchmarks/*.py"),ROOT/"benchmarks/benchmark.cpp",ROOT/"benchmarks/requirements.txt",Path(__file__)]
    metadata["source_sha256"]={str(path.relative_to(ROOT)):hashlib.sha256(path.read_bytes()).hexdigest() for path in sorted(sources) if path.is_file()}
    metadata["binary_sha256"]={name:hashlib.sha256((build/name).read_bytes()).hexdigest() for name in
                               ("edgerpc_server","edgerpc_demo","edgerpc_benchmark")}
    save(output/"metadata.json",metadata)
    manifest=dict(status="running",runs=[],server_commands=[])
    save(output/"manifest.json",manifest)
    failed=False
    try:
        with tempfile.TemporaryDirectory(prefix="erpc-compare-",dir="/tmp") as runtime:
            servers=Servers(build,output,runtime)
            try:
                for trial in range(1,options.trials+1):
                    order=list(configs)
                    random.Random(options.seed+trial).shuffle(order)
                    for config in order:
                        index=len(manifest["runs"])+1
                        name=f"trial-{trial:02d}-run-{index:04d}"
                        record=dict(name=name,trial=trial,config=config,status="running",returncode=None,
                                    samples=name+".csv",stdout=name+".stdout",stderr=name+".stderr")
                        manifest["runs"].append(record)
                        save(output/"manifest.json",manifest)
                        command=[]
                        try:
                            path=servers.get(config["delay_us"])
                            command=[str(build/"edgerpc_benchmark"),"--socket",str(path),"--mode",config["mode"],
                                     "--size-kib",str(config["tensor_bytes"]//1024),"--inflight",str(config["inflight"]),
                                     "--workers","1","--requests",str(options.requests),"--warmup",str(options.warmup),
                                     "--delay-us",str(config["delay_us"]),"--instrumentation","on" if config["instrumented"] else "off",
                                     "--csv",str(output/record["samples"])]
                            if config["buffer_slots"]:
                                command += ["--buffer-slots",str(config["buffer_slots"])]
                            record["command"]=command
                            with (output/record["stdout"]).open("w") as stdout, (output/record["stderr"]).open("w") as stderr:
                                process=subprocess.run(command,stdout=stdout,stderr=stderr,timeout=options.run_timeout)
                            record["returncode"]=process.returncode
                            if process.returncode!=0:
                                raise RuntimeError(f"benchmark exited {process.returncode}")
                            record["summary"]=dict(part.split("=",1) for part in (output/record["stdout"]).read_text().split())
                            record["status"]="completed"
                        except Exception as error:
                            record.update(status="failed",error=str(error),command=command)
                            failed=True
                        manifest["server_commands"]=[value[3] for value in servers.running.values()]
                        save(output/"manifest.json",manifest)
                        if index%20==0 or index==options.trials*len(configs):
                            print(f"Finished {index}/{options.trials*len(configs)} trials; failures={sum(r['status']=='failed' for r in manifest['runs'])}",flush=True)
            finally:
                servers.close()
        manifest["status"]="failed" if failed else "completed"
        save(output/"manifest.json",manifest)
        if failed:
            raise RuntimeError(f"failed runs retained at {output}; no comparative report generated")
        report(output,1 if options.smoke else 5,not options.no_plots)
    except BaseException:
        manifest["status"]="failed"
        save(output/"manifest.json",manifest)
        raise
    print(f"Saved validated comparisons to {output}",flush=True)
    return output


if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir",type=Path,default=ROOT/"build")
    parser.add_argument("--output",type=Path)
    parser.add_argument("--trials",type=int,default=5)
    parser.add_argument("--requests",type=int,default=200)
    parser.add_argument("--warmup",type=int,default=20)
    parser.add_argument("--sizes-kib",nargs="+",type=int,default=[4,64,1024])
    parser.add_argument("--delays-us",nargs="+",type=int,default=[0,1000])
    parser.add_argument("--inflight",nargs="+",type=int,default=[1,4])
    parser.add_argument("--slots",nargs="+",type=int,default=[1,2,4])
    parser.add_argument("--workloads",nargs="+",choices=["noop","compute"],default=["noop","compute"])
    parser.add_argument("--seed",type=int,default=20261016)
    parser.add_argument("--run-timeout",type=float,default=30)
    parser.add_argument("--no-plots",action="store_true")
    parser.add_argument("--smoke",action="store_true",help="One short trial per mode; no performance conclusions")
    options=parser.parse_args()
    if options.smoke:
        options.trials,options.requests,options.warmup=1,8,2
        options.sizes_kib,options.delays_us,options.slots,options.workloads=[4],[0],[2],["compute"]
    if not (1<=options.trials<=100 and 1<=options.requests<=1000000 and 0<=options.warmup<=1000000 and
            options.run_timeout>0 and options.run_timeout<float("inf")):
        parser.error("invalid trial count, request count, warm-up or timeout")
    for values,low,high in ((options.sizes_kib,0,8192),(options.delays_us,0,1000000),(options.inflight,1,32)):
        if len(set(values))!=len(values) or any(value<low or value>high for value in values):
            parser.error("matrix values must be unique and within documented limits")
    if any(value not in (1,2,4) for value in options.slots) or len(set(options.slots))!=len(options.slots) or len(set(options.workloads))!=len(options.workloads):
        parser.error("slots must be unique choices from 1/2/4 and workloads must be unique")
    if not options.smoke and options.trials<5:
        parser.error("comparisons require at least five trials; use --smoke for a short validation")
    def interrupted(signum,frame):
        raise KeyboardInterrupt(f"signal {signum}")
    signal.signal(signal.SIGTERM,interrupted)
    run(options)
