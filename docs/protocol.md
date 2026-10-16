# Wire protocol, version 1

Local `AF_UNIX` / `SOCK_STREAM` connections carry consecutive frames. Each frame has a 28-byte header followed by exactly `payload_size` bytes. There is no C++ struct padding on the wire. All integer fields and IEEE-754 binary32 tensor values use **big-endian** byte order.

| Offset | Bytes | Field | Values |
|---|---:|---|---|
| 0 | 4 | magic | `0x45525043` (`ERPC`) |
| 4 | 2 | version | `1` |
| 6 | 2 | type | `1` socket request, `2` socket response, `3` error, `4` SHM request, `5` SHM completion |
| 8 | 8 | request_id | Nonzero unsigned 64-bit ID, echoed in response |
| 16 | 4 | payload_size | 0 through 8,388,608 bytes |
| 20 | 4 | status | `0` OK, `1` invalid request, `2` backend failure, `3` overloaded |
| 24 | 4 | flags | Operation: `0` noop/echo, `1` compute (`2*x + 1`) |

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
