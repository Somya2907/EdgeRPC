# Wire protocol, version 1

Local `AF_UNIX` / `SOCK_STREAM` connections carry consecutive frames. Each frame has a 28-byte header followed by exactly `payload_size` bytes. There is no C++ struct padding on the wire. All integer fields and IEEE-754 binary32 tensor values use **big-endian** byte order.

| Offset | Bytes | Field | Values |
|---|---:|---|---|
| 0 | 4 | magic | `0x45525043` (`ERPC`) |
| 4 | 2 | version | `1` |
| 6 | 2 | type | `1` request, `2` response, `3` error |
| 8 | 8 | request_id | Nonzero unsigned 64-bit ID, echoed in response |
| 16 | 4 | payload_size | 0 through 8,388,608 bytes |
| 20 | 4 | status | `0` OK, `1` invalid request, `2` backend failure |
| 24 | 4 | flags | Operation: `0` noop/echo, `1` compute (`2*x + 1`) |

Requests must have OK status. Successful responses preserve operation, ID, and element count. Tensor payloads must be a multiple of four bytes; an empty tensor is valid. `noop` returns the original values, so it still includes the full response payload. Non-finite float bit patterns are representable; CLI-generated workloads use finite, exactly representable values and validate results within absolute tolerance `1e-6`.

Errors carry a diagnostic byte string instead of tensor values and echo the request ID and flags. Invalid request semantics produce status 1, and the connection can continue with the next frame. Backend failures produce status 2. Unknown magic, version, type, or oversized payload length closes that connection. Payload length is checked before allocation. Other clients can subsequently connect.

The library client increments IDs within each connection. It checks response type, ID, operation, status, and tensor size. It throws `RpcError` on an error response and closes the connection on any failed exchange, including timeouts and truncation. The server does not deduplicate or remember IDs across requests/connections. No retry is implicit.

`read_exact` and `write_all` handle short transfers, EINTR, and EAGAIN with `poll` and a monotonic deadline. A clean EOF at a frame boundary ends the connection normally. EOF mid-header or mid-payload is an error. SIGPIPE is suppressed for socket writes using `MSG_NOSIGNAL` on Linux or `SO_NOSIGPIPE` on macOS.

The deadline applies separately to each header and payload read/write; it is not a total RPC timeout. The default is five seconds. A timeout does not cancel backend execution. Framing provides no encryption or authentication; the supported boundary is a trusted local user.
