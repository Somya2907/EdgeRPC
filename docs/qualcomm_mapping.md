# How EdgeRPC relates to Qualcomm FastRPC

EdgeRPC exercises request lifecycles, buffer ownership and queueing across two
CPU processes. Qualcomm's FastRPC documentation describes CPU-to-DSP calls with
QAIC-generated stubs/skeletons and a platform driver stack. Those are related
design concerns, but EdgeRPC has no FastRPC driver, DSP code or NPU execution.

## Mapping the concepts

| EdgeRPC implementation | Related Qualcomm concept | Boundary of the comparison |
|---|---|---|
| Handwritten `EdgeRpcClient`, binary frames and request IDs | CPU application, generated stub, parameter marshaling | No IDL compiler, QAIC stub, remote handle ABI or FastRPC wire compatibility |
| Unix stream socket between same-host CPU processes | CPU/DSP messaging through FastRPC user/kernel/DSP components | No processor-domain crossing, rpmsg transport or DSP protection domain |
| `MockExecutor` running `2*x+1` or echo | Remote method implementation called through a generated skeleton | No DSP/HTP binary, tensor graph, model weights or accelerator instruction execution |
| `infer_async`, futures and a bounded execution queue | Asynchronous invocation and completion APIs | C++ future behavior is EdgeRPC's contract; it does not validate QNN/FastRPC scheduling or cancellation semantics |
| POSIX `shm_open`/`mmap`, slot IDs and generations | Shared buffers, registered mappings and lifecycle management | No `rpcmem` allocator, DMA/IOMMU mapping, device-memory import or CPU/DSP cache maintenance |
| Host monotonic timestamps and application socket-byte counts | Runtime profiling and transfer accounting | No HTP cycles, device transfer time, firmware tracing, energy or power measurements |

The upstream [FastRPC introduction and architecture][fastrpc] explain QAIC,
stubs/skeletons and the CPU/DSP stack. The published [`remote.h`][remote] defines
invocation, async descriptors, mapping/registration and cache-related APIs;
[`rpcmem.h`][rpcmem] defines allocation and file-descriptor APIs. These are
references for the conceptual comparison, not dependencies linked by EdgeRPC.
The links pin the inspected upstream revision rather than a moving branch.

## What the project demonstrates

A caller can submit multiple bounded requests, match out-of-order completions,
and receive explicit overload/disconnect failures. A registered slot remains
owned by its published generation until output is copied; another input can
prepare in a different slot while execution proceeds. All transports return
verified outputs and run through independent processes.

The [measured findings](benchmark-results.md) show application socket traffic
and transport tradeoffs on one Linux x86_64 cloud host. Native Mac/ARM64 tests
establish correctness on those runners. Neither establishes a Qualcomm transfer
cost or hardware speedup. POSIX shared storage reduces socket tensor payloads;
vectors and backend copies remain, so this is not a zero-copy inference path.

## A future QNN backend

QNN/QAIRT graph execution is a higher-level integration task than replacing a
Unix socket with a FastRPC call. Device, OS, SDK, backend, graph format and
supported memory APIs must be selected and tested together. The future
[backend design](backend-extension.md) starts with the existing vector interface
and keeps hardware-specific memory and asynchronous execution as separate work.

Qualcomm's [QNN graph API reference][qnn] is a starting point for that future
implementation. This project has not loaded a QNN library, initialized a graph,
registered device buffers or tested graph execution. Availability and behavior
must be checked against the SDK actually installed on a supported device.

A defensible project description is: “Built a C++ process-to-process inference
RPC prototype with bounded asynchronous calls and reusable shared-memory
buffers; validated transport tradeoffs using a CPU simulator.”

[fastrpc]: https://github.com/qualcomm/fastrpc/blob/a56e9d4de3614a8d8f4b0e15ff561350c4b686e3/README.md
[remote]: https://github.com/qualcomm/fastrpc/blob/a56e9d4de3614a8d8f4b0e15ff561350c4b686e3/inc/remote.h
[rpcmem]: https://github.com/qualcomm/fastrpc/blob/a56e9d4de3614a8d8f4b0e15ff561350c4b686e3/inc/rpcmem.h
[qnn]: https://docs.qualcomm.com/nav/home/api-rst_file_include_QNN_QnnGraph_h.html?product=924033590759186372
