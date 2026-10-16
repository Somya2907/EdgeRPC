"""Export standalone Matplotlib SVGs from validated per-trial aggregates."""
import os
import tempfile


def label(row):
    return row["mode"] + (f" ({row['buffer_slots']} slots)" if row["buffer_slots"] else "")


def plot(directory, summaries):
    os.environ.setdefault("MPLCONFIGDIR", tempfile.gettempdir() + "/edgerpc-matplotlib")
    os.environ.setdefault("XDG_CACHE_HOME", tempfile.gettempdir() + "/edgerpc-cache")
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    plt.rcParams.update({"svg.fonttype":"none", "font.size":9})
    rows = [r for r in summaries if r["instrumented"] == 1 and r["workload"] == "compute"]
    if not rows:
        raise ValueError("charts require instrumented compute measurements")
    windows = sorted({r["inflight"] for r in rows})
    delays = sorted({r["delay_us"] for r in rows})
    labels = sorted({label(r) for r in rows})
    colors = dict(zip(labels, plt.get_cmap("tab10").colors))
    for filename, metric, title, ylabel in (
        ("latency.svg","p99_us","Per-trial p99 latency","Latency (μs)"),
        ("throughput.svg","throughput_rps","Verified request throughput","Requests / second")):
        fig, axes = plt.subplots(len(windows),len(delays),squeeze=False,figsize=(6*len(delays),3.6*len(windows)))
        handles = {}
        for ri, window in enumerate(windows):
            for ci, delay in enumerate(delays):
                ax=axes[ri][ci]
                selected=[r for r in rows if r["inflight"] == window and r["delay_us"] == delay]
                for name in labels:
                    series=sorted((r for r in selected if label(r)==name),key=lambda r:r["tensor_bytes"])
                    if not series:
                        continue
                    y=[r[metric+"_median"] for r in series]
                    error=[[r[metric+"_median"]-r[metric+"_min"] for r in series],
                           [r[metric+"_max"]-r[metric+"_median"] for r in series]]
                    handles[name]=ax.errorbar([r["tensor_bytes"]/1024 for r in series],y,yerr=error,
                                               marker="o",capsize=3,label=name,color=colors[name])
                ax.set(xlabel="Tensor size (KiB)",ylabel=ylabel,title=f"Window {window}; execution delay {delay} μs")
                if all(r["tensor_bytes"]>0 for r in selected):
                    ax.set_xscale("log",base=2)
                ax.grid(alpha=.25)
        fig.suptitle(title+" — compute workload; median and min–max across trials")
        fig.legend(list(handles.values()),list(handles),loc="lower center",ncol=3)
        fig.tight_layout(rect=(0,.11,1,.95))
        fig.savefig(directory/filename)
        fig.savefig(directory/filename.replace(".svg",".png"),dpi=120)
        plt.close(fig)
    # Stage means are additive within each trial; show mean across trial means.
    size=sorted({r["tensor_bytes"] for r in rows})[len({r["tensor_bytes"] for r in rows})//2]
    selected=sorted((r for r in rows if r["tensor_bytes"]==size and r["delay_us"]==min(delays) and
                     r["inflight"]==max(windows)),key=label)
    fig,ax=plt.subplots(figsize=(11,5))
    left=[0.0]*len(selected)
    for metric,name in (("prepare_mean_us","Preparation"),("buffer_wait_mean_us","Slot acquisition"),
                        ("queue_mean_us","Server queue"),("execute_mean_us","Execution + copies"),
                        ("residual_mean_us","Other latency")):
        values=[r[metric+"_mean"] for r in selected]
        ax.barh([label(r) for r in selected],values,left=left,label=name)
        left=[a+b for a,b in zip(left,values)]
    ax.set(xlabel="Mean latency (μs)",title=f"Stage attribution — {size//1024} KiB compute, delay {min(delays)} μs, window {max(windows)}")
    ax.legend(loc="upper center",bbox_to_anchor=(.5,-.15),ncol=3)
    fig.tight_layout()
    fig.savefig(directory/"stages.svg",bbox_inches="tight")
    fig.savefig(directory/"stages.png",bbox_inches="tight",dpi=120)
    plt.close(fig)
    fig,ax=plt.subplots(figsize=(10,5))
    for name in labels:
        series=sorted((r for r in rows if label(r)==name and r["inflight"]==min(windows) and
                       r["delay_us"]==min(delays)),key=lambda r:r["tensor_bytes"])
        ax.plot([r["tensor_bytes"]/1024 for r in series],
                [r["socket_tx_bytes_per_request_median"]+r["socket_rx_bytes_per_request_median"] for r in series],
                marker="o",label=name,color=colors[name])
    ax.set(xlabel="Tensor size (KiB)",ylabel="Measured send + recv bytes / call",
           title="Application socket traffic — warm-up and registration excluded")
    if all(r["tensor_bytes"]>0 for r in rows):
        ax.set_xscale("log",base=2)
    ax.set_yscale("log")
    ax.grid(alpha=.25)
    ax.legend()
    fig.tight_layout()
    fig.savefig(directory/"traffic.svg")
    fig.savefig(directory/"traffic.png",dpi=120)
    plt.close(fig)
    # Fix size/workload/delay, then compare all latency quantiles across windows.
    selected=[r for r in rows if r["tensor_bytes"]==size and r["delay_us"]==min(delays)]
    fig,axes=plt.subplots(1,3,figsize=(16,5))
    handles={}
    for ax,metric in zip(axes,("p50_us","p95_us","p99_us")):
        for name in labels:
            series=sorted((r for r in selected if label(r)==name),key=lambda r:r["inflight"])
            if not series:
                continue
            handles[name]=ax.errorbar([r["inflight"] for r in series],[r[metric+"_median"] for r in series],
                yerr=[[r[metric+"_median"]-r[metric+"_min"] for r in series],
                      [r[metric+"_max"]-r[metric+"_median"] for r in series]],
                marker="o",capsize=3,color=colors[name],label=name)
        ax.set(xlabel="Configured in-flight window",ylabel="Latency (μs)",title=metric.replace("_us",""))
        ax.set_xticks(windows)
        ax.grid(alpha=.25)
    fig.suptitle(f"Latency vs concurrency — {size//1024} KiB compute, delay {min(delays)} μs; median and min–max")
    fig.legend(list(handles.values()),list(handles),loc="lower center",ncol=3)
    fig.tight_layout(rect=(0,.14,1,.93))
    fig.savefig(directory/"concurrency.svg")
    fig.savefig(directory/"concurrency.png",dpi=120)
    plt.close(fig)
    # Startup and first-call costs stay distinct from the warm measured loop.
    selected=[r for r in rows if r["inflight"]==min(windows) and r["delay_us"]==min(delays)]
    fig,axes=plt.subplots(1,2,figsize=(13,5))
    handles={}
    for ax,metric,title in zip(axes,("initialization_us","first_request_us"),("Client initialization","First client request")):
        for name in labels:
            series=sorted((r for r in selected if label(r)==name),key=lambda r:r["tensor_bytes"])
            if not series:
                continue
            handles[name]=ax.errorbar([r["tensor_bytes"]/1024 for r in series],[r[metric+"_median"] for r in series],
                yerr=[[r[metric+"_median"]-r[metric+"_min"] for r in series],
                      [r[metric+"_max"]-r[metric+"_median"] for r in series]],
                marker="o",capsize=3,color=colors[name],label=name)
        ax.set(xlabel="Tensor size (KiB)",ylabel="Duration (μs)",title=title)
        if all(r["tensor_bytes"]>0 for r in selected):
            ax.set_xscale("log",base=2)
        ax.grid(alpha=.25)
    fig.suptitle(f"Client startup costs — window {min(windows)}, persistent server, compute; median and min–max")
    fig.legend(list(handles.values()),list(handles),loc="lower center",ncol=3)
    fig.tight_layout(rect=(0,.14,1,.93))
    fig.savefig(directory/"startup.svg")
    fig.savefig(directory/"startup.png",dpi=120)
    plt.close(fig)
    # Matplotlib paths contain trailing spaces; keep exported text clean in Git.
    for path in directory.glob("*.svg"):
        path.write_text("\n".join(line.rstrip() for line in path.read_text().splitlines())+"\n")
