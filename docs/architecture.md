# Tensor RPC runtime architecture

`edgerpc` separates the protocol, socket transport, POSIX shared buffers, execution backend, client, server, worker pool, and metrics. Applications own CLI parsing and process signal handling. The runtime has no external C++ dependencies.

## Client lifecycle

1. `EdgeRpcClient` opens one persistent Unix domain socket and starts a sender and receiver.
2. `infer_async` validates the input and transport selection, reserves a bounded pending slot, assigns an ID under a mutex, and enqueues the moved tensor. It returns a future without waiting for server execution. `infer_sync` uses the same API and waits on that future.
3. The sender prepares either an encoded socket tensor or a per-request shared region plus descriptor, then writes whole request frames in submission order. Shared regions are published to pending state under the mutex before transmission. The receiver waits for prepared calls, validates replies, matches the pending promise by ID, reconstructs output, timestamps completion, and settles the promise. Socket and SHM calls can coexist and complete in any order.
4. A valid error response settles only its request. Protocol/transport failures atomically stop submission, interrupt I/O with `shutdown`, clear queued work, and fail all unresolved promises.
5. `close()` is idempotent and serialized across callers. It joins both I/O threads before closing the descriptor. Destruction closes automatically; futures may outlive the client and still receive a settled error. The caller must keep the object alive while methods execute.

The pending limit includes requests waiting for the sender, being encoded/sent, and awaiting replies. Capacity is released when the promise is ready. The default is 32 and the supported range is 1–1024. A full client returns an already-failed `RpcError(Overloaded)` future. One failed submission does not poison accepted work.

Preparation failures occur before any frame bytes are sent and fail only that future. Receiver deadlines start after preparation, so allocation errors cannot time out an otherwise unused connection. Shared-buffer references remain alive through sender publication, pending state, and receiver output copying; descriptor access is protected by the pending-state mutex. A failed connection unlinks still-owned names without resizing objects that another process may already have mapped.

## Server lifecycle

The server accepts one active connection at a time. For that connection, its reader validates incoming requests and submits them to a bounded `ThreadPool`. A SHM request is independently opened and mapped at admission; UID, mode, actual size, name, and ranges are checked, and the name is unlinked before enqueueing. The queue closure owns the mapping until execution or cancellation. The pool defaults to one worker and 32 waiting tasks. Queue capacity counts waiting tasks separately from running tasks. A rejected request receives `OVERLOADED`, releases its mapping, and is never executed.

Workers call `ExecutionBackend`, then encode socket results or write to the mapped output range and send an empty SHM completion. All response frames use a connection writer mutex. The reader uses that same writer for validation and overload errors, preventing interleaved bytes. `MockExecutor` echoes values or computes `2*x+1`, with optional delay. It is thread-safe because each invocation owns its vectors and the configured delay is immutable. The vector backend still copies data; shared memory reduces socket tensor payloads without claiming zero-copy compute.

Configure 1–16 workers for separate CPU-scaling experiments. A custom backend must support concurrent execution when more than one worker is enabled. Multiple CPU workers do not represent one serialized accelerator.

On EOF, malformed input, termination, or write failure, the connection is shut down, waiting tasks are discarded, and running tasks finish before the accepted descriptor or backend can be destroyed. There is no forced cancellation of running compute and no promise of graceful half-close/drain semantics. Fair multi-client scheduling is not implemented.

## Resource ownership and timing

Descriptors and socket paths use RAII. The listening socket is removed on graceful exit only if its device/inode still match. Stale sockets are probed before removal; regular files and symlinks are refused. The server app applies umask 0077 and the socket is chmod 0600. SIGKILL cannot run destructors; a subsequent server recovers the stale path.

The OS listen backlog (16) is separate from the execution queue. Tensors are capped at 8 MiB, shared objects at 16 MiB, and request counts are bounded; transient copies and running workers add to queued memory. Linux reserves SHM backing storage before writes; macOS size reporting and file-type bits are handled according to PSXSHM behavior. No process-wide umask change occurs in library threads. See [shared ownership and crash recovery](shared-memory.md), including possible orphan names if the creating client is killed before admission.

Per-header/payload I/O deadlines are separate from request execution deadlines. A client without prepared requests awaiting replies waits on a condition variable rather than timing out. With transmitted calls, a silent server eventually fails them all; continued unrelated replies may delay failure of a missing reply. The server polls while waiting for new work and suspends its idle deadline while tasks are outstanding. Partial frames still have read deadlines.

The benchmark measures submission through client decoding/validation or shared-output copying using `Response::completed_at`, before promise notification. It does not confuse the order in which futures are inspected with the order replies arrived. SHM allocates/maps per request, and teardown can finish after promise completion. Stage-level queue/execution timing and pooled reuse measurements are planned, not yet measured.

## Remaining milestones

See [the completion roadmap](completion-plan.md) for acceptance criteria and status. Next are registered reusable mappings with explicit ownership and two-slot pipelining, instrumented comparisons with charts/repeated trials, and native Mac/ARM64 validation. Registration must retain server mappings because per-request SHM names are already unlinked after admission. Qualcomm QNN/FastRPC integration requires supported hardware and SDK access; CPU simulator measurements cannot establish NPU behavior.
