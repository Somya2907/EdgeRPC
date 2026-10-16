# Socket runtime architecture

`edgerpc` separates the protocol, transport, execution backend, client, server, worker pool, and metrics. Applications own CLI parsing and process signal handling. The runtime has no external C++ dependencies.

## Client lifecycle

1. `EdgeRpcClient` opens one persistent Unix domain socket and starts a sender and receiver.
2. `infer_async` validates the input, reserves a bounded pending slot, assigns an ID under a mutex, and enqueues the moved tensor. It returns a future without waiting for server execution. `infer_sync` uses the same API and waits on that future.
3. The sender encodes tensors and writes whole request frames in submission order. The receiver validates replies, finds the matching pending promise by ID, decodes the result, timestamps completion, and settles that promise. Completion order may differ from submission order.
4. A valid error response settles only its request. Protocol/transport failures atomically stop submission, interrupt I/O with `shutdown`, clear queued work, and fail all unresolved promises.
5. `close()` is idempotent and serialized across callers. It joins both I/O threads before closing the descriptor. Destruction closes automatically; futures may outlive the client and still receive a settled error. The caller must keep the object alive while methods execute.

The pending limit includes requests waiting for the sender, being encoded/sent, and awaiting replies. Capacity is released when the promise is ready. The default is 32 and the supported range is 1–1024. A full client returns an already-failed `RpcError(Overloaded)` future. One failed submission does not poison accepted work.

## Server lifecycle

The server accepts one active connection at a time. For that connection, its reader validates incoming requests and submits them to a bounded `ThreadPool`. The pool defaults to one worker and 32 waiting tasks. Queue capacity counts waiting tasks separately from running tasks. A rejected request receives `OVERLOADED` and is never executed.

Workers call `ExecutionBackend`, encode results, and serialize response writes through a connection mutex. The reader uses that same writer for validation and overload errors, preventing interleaved frame bytes. `MockExecutor` echoes values or computes `2*x+1`, with optional delay. It is thread-safe because each invocation owns its vectors and the configured delay is immutable.

Configure 1–16 workers for separate CPU-scaling experiments. A custom backend must support concurrent execution when more than one worker is enabled. Multiple CPU workers do not represent one serialized accelerator.

On EOF, malformed input, termination, or write failure, the connection is shut down, waiting tasks are discarded, and running tasks finish before the accepted descriptor or backend can be destroyed. There is no forced cancellation of running compute and no promise of graceful half-close/drain semantics. Fair multi-client scheduling is not implemented.

## Resource ownership and timing

Descriptors and socket paths use RAII. The listening socket is removed on graceful exit only if its device/inode still match. Stale sockets are probed before removal; regular files and symlinks are refused. The server app applies umask 0077 and the socket is chmod 0600. SIGKILL cannot run destructors; a subsequent server recovers the stale path.

The OS listen backlog (16) is separate from the execution queue. Payloads are capped at 8 MiB and request counts are bounded; transient copies and running workers add to queued tensor memory. No persistent model state or shared memory exists yet.

Per-header/payload I/O deadlines are separate from request execution deadlines. An unused client waits on a condition variable rather than timing out. With pending calls, a silent server eventually fails them all; continued unrelated replies may delay failure of a missing reply. The server polls while waiting for new work and suspends its idle deadline while tasks are outstanding. Partial frames still have read deadlines.

The benchmark measures submission through client decoding/validation completion using `Response::completed_at`, before promise notification. It does not confuse the order in which futures are inspected with the order replies arrived. Stage-level queue and execution timing are planned, not yet measured.

## Remaining milestones

See [the completion roadmap](completion-plan.md) for acceptance criteria and status. Next are POSIX shared-memory buffers, explicit ownership and two-slot pipelining, instrumented comparisons with charts/repeated trials, and native Mac/ARM64 validation. Qualcomm QNN/FastRPC integration requires supported hardware and SDK access; CPU simulator measurements cannot establish NPU behavior.
