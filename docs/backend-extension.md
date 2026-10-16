# Future hardware backend design

The only supported backend is `MockExecutor`. The server application constructs
it and passes it to `run_server`; no QNN library or SDK is required to build.
This note describes future work and adds no hardware API or CLI flag.

## Existing extension boundary

[`ExecutionBackend`](../include/edgerpc/executor.hpp) exposes:

```cpp
virtual std::vector<float> execute(const std::vector<float>& input,
                                 Operation operation) = 0;
```

`run_server` borrows a backend that outlives the server and all its worker tasks.
The current contract is synchronous at the backend: `execute` returns owned
output when the work is finished, or throws. Async client submission does not
change this contract. A backend must permit concurrent `execute` calls when
the server has more than one worker. The server joins running tasks before
returning; running backend work is not forcibly cancelled on disconnect.

The protocol currently has one float32 vector input/output and `compute`/`noop`
operations. A model graph with multiple tensors, shapes or quantized types
cannot be represented faithfully without an explicit API/protocol extension.
Do not silently reinterpret `compute` as arbitrary graph inference.

## Proposed first hardware milestone

1. Select an accessible supported device and record SoC, OS, SDK/runtime,
   backend version, model format and licensing/deployment prerequisites.
2. Prove synchronous graph loading and execution in a standalone SDK example,
   using its documented initialization, cleanup and error reporting.
3. Define a versioned graph/tensor contract and capability negotiation before
   exposing hardware graph execution to clients. Keep the mock operations and
   their tests available and require explicit hardware selection.
4. Implement an optional backend behind that contract. Start with copying into
   SDK-owned input/output storage. Keep graph/context lifetime outside the warm
   execution loop, and return a fully owned result only after execution finishes.
5. Use one worker initially. Add concurrency only when backend thread-safety and
   context rules are documented and verified on the selected device. Bound both
   request count and memory use; record hardware-specific failures explicitly.
6. Compare outputs against a known reference with appropriate dtype/error
   tolerance. Test initialization failure, invalid shapes, backend errors,
   disconnect during execution and complete shutdown before adding optimizations.

No guessed `QnnBackend` implementation is provided. Actual SDK symbols,
capabilities and ownership rules must come from the selected version's official
[graph API reference](https://docs.qualcomm.com/nav/home/api-rst_file_include_QNN_QnnGraph_h.html?product=924033590759186372).

## Memory and async work after that milestone

POSIX SHM storage is not automatically device-importable. Investigate documented
allocation/import/registration APIs, alignment, cache maintenance, mapping
lifetime and device synchronization on the target. A host socket completion
must follow the final device write and any required host visibility operation.
If the device is still using a buffer when a client disconnects, retain the
device registration and storage until completion. Existing generation numbers
do not supply device fences.

Native asynchronous graph execution would need an explicit completion owner,
bounded jobs, exactly-once promise settlement, callback lifetime protection and
shutdown/drain rules. Returning from `execute` while the device still accesses
its input violates the current interface; either wait or design a separate
asynchronous backend interface first.

Profile graph loading, allocation/import, input preparation, execution,
synchronization and output transfer separately. Retain device-side telemetry
alongside host end-to-end latency, failures and repeated trials. Publish those
as a separate hardware experiment rather than extending the existing CPU
measurements. [Qualcomm mapping](qualcomm_mapping.md) states the current boundary.
