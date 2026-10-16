# Synchronous baseline architecture

`edgerpc` separates the protocol, transport, execution backend, client, server, and metrics. Applications own CLI parsing and process signal handling. The runtime has no external C++ dependencies.

1. `EdgeRpcClient` opens a persistent Unix domain socket and owns it through `UniqueFd`.
2. `infer_sync` serializes an input tensor, assigns a request ID, and sends a request frame.
3. The server validates the header before allocating a bounded payload, then validates the operation and tensor alignment.
4. `ExecutionBackend` executes independently of transport. `MockExecutor` either echoes input or computes `2*x + 1`, optionally after a configured sleep.
5. The server serializes the output and echoes the request ID. The client validates the response and reconstructs the output vector.

## Ownership and failure handling

Descriptors and listening socket paths use RAII. Only a socket created by the listener is removed on graceful exit, with an inode/device check to avoid removing a replacement path. Stale sockets are probed before removal; regular files and symlinks are refused. The server app applies umask 0077 and the socket is chmod 0600.

A connection failure is contained to that peer. On SIGINT/SIGTERM the server finishes or times out the current I/O before cleanup. SIGKILL cannot run destructors; a later start can recover the stale socket path. No persistent model state or shared memory exists in this milestone.

This intentionally serial baseline accepts one connection and executes requests sequentially. The OS listen backlog (16) is not an application execution queue. There is no claim of fairness or concurrent-client service. Per-client state is bounded by the payload limit. The benchmark stores bounded samples and refuses more than one million measured requests.

## Next milestones

Follow [Plan.md](../Plan.md) in order:

1. A bounded client submission queue, sender/receiver threads, and `request_id -> promise` matching for `infer_async`.
2. A bounded execution queue with explicit overload status and deterministic lifecycle/disconnect tests. Start with one worker.
3. POSIX `shm_open`/`mmap` tensor buffers with validated names and bounds; sockets retain the control path.
4. Explicit buffer ownership and two-slot pipelining, with no slot reuse until completion.
5. Matched-workload transport comparisons, repeated trials, charts, and optional ARM64 Linux validation.

These features are not yet implemented. Keep the synchronous transport as a correctness and measurement reference. Qualcomm QNN/FastRPC integration requires supported hardware and SDK access; CPU simulator measurements cannot establish NPU behavior.
