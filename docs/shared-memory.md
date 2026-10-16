# Shared-memory transport (v0.3)

This document describes `SharedMemory` / `--transport shm`: the client and server map one POSIX shared-memory object per request. Only a 70-byte descriptor crosses the request socket; success returns a 28-byte completion header with no tensor payload. Existing socket-copy modes remain available. The v0.4 `SharedPool` / `--transport pool` mode adds [registration and reusable slots](pipeline.md) while retaining the validation/trust rules below. The vector-based API and mock backend still copy data into and out of mappings.

## Layout and ownership

For a tensor of `N` float32 elements, the normal client requests `max(8, 8*N)` bytes, writes input at offset zero, and reserves output at offset `4*N`. macOS may round the allocation to a VM page; the client advertises the actual size returned by `fstat`. Each tensor is capped at 8 MiB and the actual region at 16 MiB. Empty tensors request eight bytes and have empty ranges. The protocol also accepts other aligned, disjoint layouts within those limits.

1. **Prepare:** the sender creates a unique `/erpc_` name followed by 24 lowercase hexadecimal digits, using `O_CREAT|O_EXCL` and mode 0600. It validates the actual permissions, requests the size, records the actual allocated size, maps it, and writes input. Each request owns a fresh object; no region is reused in this phase.
2. **Publish:** after all input writes finish, the sender registers the region in that request's pending state and sends its descriptor. The caller cannot access the internal mapping. The receiver waits for prepared requests; local allocation failures fail their own future without starting a receive deadline for an unsent request.
3. **Admit:** the server validates metadata, opens an existing same-user, mode-0600 object with the exact advertised size, and maps it. It never truncates or resizes attached objects. It immediately unlinks the name, so admitted work does not leave a named object after either process crashes. Existing mappings remain valid.
4. **Execute:** a bounded queue owns the server mapping. The worker reads input into the backend's vector, computes, and copies output to its disjoint region. It finishes those writes before sending completion with the matching request ID. Rejected queue submissions receive OVERLOADED and discard their mapping.
5. **Complete:** the client matches the response ID/type, reads output into `Response::output`, timestamps completion, and resolves the promise. RAII releases mappings and any still-owned name as references are dropped. Futures contain vectors, so result data remains valid after unmapping.

Socket control messages transfer exclusive access to the output range. The client never reads output before a valid completion. An ordinary `std::mutex` is not used as an inter-process lock. Per-request mode has no buffer slots or in-place reuse. Pool mode instead registers and retains mappings, because already-unlinked names cannot be reopened on subsequent requests.

## Representation and validation

Socket metadata uses explicit big-endian fields. Mapped tensors contain **native-endian IEEE-754 binary32**; both processes must run on the same host/kernel IPC namespace with matching native float byte order. Tensor values are moved with `memcpy`, rather than treating unconstructed mapped bytes as C++ float objects. See [the descriptor schema](protocol.md#shared-memory-descriptor).

The parser checks descriptor length/version, name prefix/characters/length, reserved bits, region size, float count/byte length, four-byte alignment, offset bounds without overflowing arithmetic, and non-overlapping input/output ranges. `shm_open` never uses `O_CREAT` or `O_TRUNC` when attaching. `fstat` checks effective UID, mode 0600, and exact actual size before mapping. Linux requires a regular-file type. Darwin's kernel PSXSHM objects may report no file-type bits, which is accepted only on macOS while retaining all other checks. Malformed or unavailable descriptors receive INVALID_REQUEST, leaving other requests usable. Oversized or incorrectly sized SHM control frames are rejected before payload allocation.

Creation validates the mode instead of calling `fchmod`, which macOS PSXSHM descriptors do not support. Use a normal umask preserving owner read/write (for example, 0022 or 0077); the runtime never changes the process-wide umask in its worker threads. These platform adaptations follow Apple's [POSIX SHM implementation](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/kern/posix_shm.c) and [file-mode syscall implementation](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/vfs/vfs_syscalls.c). They still require native Mac execution for validation.

This is a **same-user, trusted-client demo**, not a hardened shared-memory service. Permission checks and namespace restrictions do not protect against another malicious process with the same UID. Clients must not resize, truncate, replace, or concurrently mutate published regions; truncating a mapped object can cause SIGBUS. Both processes must see the same POSIX shared-memory namespace (for example, the same container).

## Limits, failures, and cleanup

Queue limits bound request counts, not just socket bytes. Each outstanding maximum-sized SHM call can reserve 16 MiB, in addition to input/output vectors and transient copies. Choose capacities and concurrency that fit `/dev/shm` on Linux and available memory on macOS.

On Linux, creation uses `posix_fallocate` after `ftruncate`, reserving backing storage before writing. This turns tmpfs space exhaustion into an exception instead of depending on sparse-file writes that can SIGBUS. There is no fallback that silently omits reservation. macOS uses POSIX `ftruncate`/`mmap`; platform memory pressure remains an OS resource constraint. Allocation/map errors are reported through the affected future.

Disconnect, timeout, close, and destruction fail unresolved futures and release the client's references. Queued server tasks are discarded; running backend calls finish before their mappings are released. Unlinking/closing a client mapping never resizes the backing object, so a server mapping already in use stays valid after a timeout. A late result is abandoned and never read by another request.

Admission removes the name even for overload rejection. If a client is killed **before admission**, an unadvertised or unread name can remain because SIGKILL cannot run RAII. Recovery requires stopping the relevant processes and unlinking only their specifically identified `/erpc_<24 hex>` objects with `shm_unlink`; do not sweep active or unrelated objects. A server crash after admission leaves no named object; the client detects EOF and drops its mapping. Server restart also retains the existing stale-socket recovery behavior. These limitations are tested where observable and are not described as automatic orphan recovery.

## Run and measure

```sh
./build/edgerpc_server --socket /tmp/edgerpc.sock --workers 1
# Another terminal:
./build/edgerpc_demo --socket /tmp/edgerpc.sock --transport shm --verify
./build/edgerpc_demo --socket /tmp/edgerpc.sock --mode async --transport shm --requests 32 --verify
mkdir -p results
./build/edgerpc_benchmark --socket /tmp/edgerpc.sock --mode shm-async \
  --size-kib 64 --inflight 16 --requests 1000 --warmup 100 --csv results/shm.csv
```

The `shm-async` latency includes allocation, storage reservation, mapping, input/output vector copies, and server unlinking for **every request**, including after warm-up. Client RAII teardown may occur after promise completion and is not fully included in that latency; teardown can overlap the measured throughput loop. It is not a buffer-reuse result and may be slower than socket copying. Pool mode separates initialization and records stage times; measured byte counters and repeated comparative reports remain phase 4. A successful per-request SHM call transfers 126 application-level socket bytes (`28 + 70 + 28`), regardless of tensor length; this is a schema-derived byte count, not a measured count of physical memory traffic.

Linux Release and ASan/UBSan validation cover independent C++/Python mappings, zero/max tensors, compute/noop, mixed socket/SHM requests, reversed replies, invalid bounds/names/permissions/sizes, overload unlinking, pending-call cleanup, server crash/restart, and a 1,000-request SHM benchmark with 16 in flight. Native macOS/ARM64 execution remains pending.
