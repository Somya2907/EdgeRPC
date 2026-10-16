# Wire protocol, version 1

Local `AF_UNIX` / `SOCK_STREAM` connections carry consecutive frames. Each frame has a 28-byte header followed by exactly `payload_size` bytes. There is no C++ struct padding on the wire. All integer fields and IEEE-754 binary32 tensor values use **big-endian** byte order.

| Offset | Bytes | Field | Values |
|---|---:|---|---|
| 0 | 4 | magic | `0x45525043` (`ERPC`) |
| 4 | 2 | version | `1` |
| 6 | 2 | type | `1` socket request, `2` socket response, `3` error, `4` SHM request, `5` SHM completion, `6` register, `7` registration ACK, `8` pool request, `9` pool completion, `10` timed socket response, `11` timed SHM response |
| 8 | 8 | request_id | Nonzero unsigned 64-bit ID, echoed in response |
| 16 | 4 | payload_size | 0 through 8,388,608 bytes; type 10 allows 24 additional trace bytes |
| 20 | 4 | status | `0` OK, `1` invalid request, `2` backend failure, `3` overloaded |
| 24 | 4 | flags | Operation: `0` noop/echo, `1` compute (`2*x + 1`); optional `0x100` timing flag on inference |

Requests must have OK status. Successful responses preserve operation, ID, and element count. Tensor payloads must be a multiple of four bytes; an empty tensor is valid. `noop` returns the original values, so it still includes the full response payload. Non-finite float bit patterns are representable; CLI-generated workloads use finite, exactly representable values and validate results within absolute tolerance `1e-6`.

Errors carry a diagnostic byte string instead of tensor values and echo the request ID and flags. Invalid request semantics produce status 1. Backend failures produce status 2. A full server execution queue produces status 3 without enqueueing that request. These errors do not end the connection. Unknown magic, version, type, or oversized payload length closes that connection. Payload length is checked before allocation. Other clients can subsequently connect. Status 3 is an additive extension; successful version-1 frames are unchanged.

The library client increments IDs within each connection and maps them to pending promises. Requests and responses may be outstanding concurrently; replies may complete in any order. One sender owns request writes and one receiver owns reply reads. All server writers serialize complete frames through a shared mutex, so response headers and payloads cannot interleave. The client checks ID, operation, type, status, and tensor size before settling a future. A valid remote error produces `RpcError` for that request only. Unknown/duplicate response IDs, malformed replies, timeouts, and disconnects fail every unresolved request and close the connection. The server does not deduplicate or remember IDs across requests/connections. No retry is implicit.

`read_exact` and `write_all` handle short transfers, EINTR, and EAGAIN with `poll` and a monotonic deadline. A clean EOF at a frame boundary ends the connection normally. EOF mid-header or mid-payload is an error. SIGPIPE is suppressed for socket writes using `MSG_NOSIGNAL` on Linux or `SO_NOSIGPIPE` on macOS.

The deadline applies separately to each header and payload read/write; it is not a total RPC timeout. The default is five seconds. The client starts receiving once a request is prepared for transmission; its receive deadline includes sending and waiting for the first reply. With no prepared requests awaiting replies, the receiver waits on a condition variable without reading. Local SHM allocation failures do not start that deadline or poison other requests. Continued replies can keep the connection active while another request waits: per-request execution deadlines are not implemented. Server idle deadlines run only when no execution is outstanding. A timeout does not cancel already-running backend execution. Half-closing/disconnecting abandons outstanding requests; the server cancels waiting tasks and joins executing tasks before reusing descriptors. Framing provides no encryption or authentication; the supported boundary is a trusted local user.

## Shared-memory descriptor

Frame version 1 is retained. Types 4 and 5 are additive: an older peer may reject those types, and there is no automatic negotiation or downgrade. Socket requests/responses remain wire-compatible. A SHM request contains **exactly 70 bytes**; a SHM completion contains zero payload bytes. Frame-length checks happen before allocation. Completion echoes the request ID and operation with OK status; errors use the existing type-3 frame and status codes.

| Payload offset | Bytes | Field | Constraint |
|---|---:|---|---|
| 0 | 2 | descriptor_version | 1 |
| 2 | 2 | name_length | 30 |
| 4 | 8 | region_bytes | 8 through 16 MiB; must equal the actual object's size |
| 12 | 8 | input_offset | Four-byte aligned; input range fits region |
| 20 | 8 | output_offset | Four-byte aligned; output range fits region and does not overlap input |
| 28 | 4 | tensor_bytes | At most 8 MiB; equals `elements * 4` |
| 32 | 4 | elements | 0 through 2,097,152 |
| 36 | 4 | reserved | 0 |
| 40 | 30 | name | ASCII `/erpc_` followed by 24 lowercase hex digits; no NUL terminator |

All descriptor integers are big-endian. Input/output are native-endian float32 in a shared object on the same host with matching byte order. No virtual pointers cross the socket. Zero-length ranges may coincide or end at the region boundary. Validation rejects all other overlaps and uses subtraction-based bounds checks before accessing offsets. After mapping and validating, the server unlinks the object's name at admission and retains its mapping through execution. See [ownership, cleanup, and trust boundaries](shared-memory.md).

## Registered buffer control (v0.4)

Types 6–9 are additive under frame version 1, with no negotiation or downgrade. Exact payload lengths are checked before allocation. Registration uses OK status, flags 0, and a nonzero request ID. Its 74-byte payload is a four-byte slot ID followed by the existing 70-byte shared descriptor. IDs 1–4 are permitted, with no replacement or unregister during that connection. The descriptor's count/length specify capacity, while offsets remain fixed. The server maps/validates/unlinks once and retains the mapping. A successful ACK echoes the ID and flags, with OK status and a four-byte slot ID payload. Registration failures use normal error frames. The client completes registration before launching inference I/O threads; control exchanges consume request IDs.

A type-8 request uses the usual operation flags, OK status, and a nonzero ID, with this exact 24-byte payload:

| Payload offset | Bytes | Field | Constraint |
|---|---:|---|---|
| 0 | 4 | slot | Registered ID, 1–4 |
| 4 | 4 | elements | At most registered capacity and 2,097,152 |
| 8 | 8 | generation | Nonzero; greater than last accepted use of this slot |
| 16 | 8 | reserved | 0 |

The reader marks an accepted slot busy before enqueueing. Busy slots, unknown IDs, stale generations, and invalid counts receive INVALID_REQUEST. Rejection must not release a different running generation. A full execution queue releases the acquired slot before OVERLOADED is sent; that generation remains consumed. The next use must increment it even after an error. Accepted slot ownership lasts through the final output write, before completion is sent. The client must consume output before sending another use of that slot. Same-user clients must obey this ownership contract; metadata checks cannot prevent a malicious mapping mutation.

A successful type-9 response echoes the request ID/operation with OK status. Its exact 48-byte payload starts with the same 24-byte slot reference, then three unsigned big-endian 64-bit timestamps at offsets 24, 32, and 40: queue admission, execution begin, execution end. All use the host's `CLOCK_MONOTONIC` nanoseconds, with `0 < queued <= begin <= end`. Execution includes input/output copies and backend work. The receiver validates slot, generation, and count before reading output. Errors retain type 3 and release the lease only for the corresponding pending ID. Malformed/stale completions fail all pending work and stop reuse on that connection. See [the pool state machine](pipeline.md).

## Optional timing telemetry (v0.5)

Inference requests may set flag `0x100` alongside operation 0/1. Other flag bits are rejected. Registration still requires flags zero. No negotiation/downgrade is implemented: older servers reject the new flag. Ordinary library clients default to timing off; the benchmark defaults on. Responses and errors echo the complete flags. Backends receive the operation with the timing bit removed.

For a timed socket request, success uses type 10: a 24-byte execution trace followed by the usual big-endian float32 output. Total payload is 24 through `8 MiB + 24`, aligned to four bytes; the tensor cap stays 8 MiB. For a timed per-request SHM call, success uses type 11 with exactly the 24-byte trace, while tensor output remains mapped. Type 9 already contains timing and retains its existing 48-byte encoding for pooled calls. Timing off keeps all previous response encodings.

The trace is three unsigned big-endian 64-bit values: `queued_ns`, `execute_begin_ns`, `execute_end_ns`. All use host `CLOCK_MONOTONIC`, with `0 < queued <= begin <= end`. Admission is after validation/mapping; execution includes decoding/input copying, backend work/delay, and output encoding/copying. Socket writing and trace-prefix insertion occur afterward. The client validates lengths, type, flags, and trace order before reading output or settling the future. A malformed trace fails all pending work. Timed response size limits are checked before allocation, including maximum-tensor responses. [Benchmark methodology](benchmarks.md) defines the counters and cold/steady-state boundaries.
