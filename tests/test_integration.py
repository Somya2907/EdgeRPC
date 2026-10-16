"""Exercise real server/client processes and independently encoded wire traffic."""

import argparse
from contextlib import ExitStack
import ctypes
import csv
import errno
import math
import mmap
import os
from pathlib import Path
import socket
import secrets
import struct
import subprocess
import tempfile
import threading
import time
import unittest


HEADER = struct.Struct("!IHHQIII")
SHARED_DESCRIPTOR = struct.Struct("!HHQQQIII")
MAGIC = 0x45525043
MAX_BYTES = 8 * 1024 * 1024
BIN = None


class SharedRegion:
    """Independent POSIX mapping; no multiprocessing resource tracker or C++ helpers."""
    libc = ctypes.CDLL(None, use_errno=True)
    libc.shm_open.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_uint]
    libc.shm_open.restype = ctypes.c_int
    libc.shm_unlink.argtypes = [ctypes.c_char_p]
    libc.shm_unlink.restype = ctypes.c_int

    def __init__(self, size=32, mode=0o600):
        self.name = "/erpc_" + secrets.token_hex(12)
        self.size = size
        self.mapping = None
        previous_mask = os.umask(0)  # This test helper runs without other local threads.
        try:
            fd = self.libc.shm_open(self.name.encode(), os.O_RDWR | os.O_CREAT | os.O_EXCL, mode)
        finally:
            os.umask(previous_mask)
        if fd < 0:
            raise OSError(ctypes.get_errno(), "test shm_open failed")
        try:
            os.ftruncate(fd, size)
            self.size = os.fstat(fd).st_size  # macOS reports a page-rounded size.
            self.mapping = mmap.mmap(fd, self.size)
        except BaseException:
            self.close()
            raise
        finally:
            os.close(fd)

    def descriptor(self, *, version=1, name_length=30, region_bytes=None, input_offset=0,
                   output_offset=16, tensor_bytes=16, elements=4, reserved=0, name=None):
        return SHARED_DESCRIPTOR.pack(version, name_length, self.size if region_bytes is None else region_bytes,
                                      input_offset, output_offset, tensor_bytes, elements, reserved) + (name or self.name).encode()

    def exists(self):
        fd = self.libc.shm_open(self.name.encode(), os.O_RDWR, 0)
        if fd >= 0:
            os.close(fd)
            return True
        if ctypes.get_errno() != errno.ENOENT:
            raise OSError(ctypes.get_errno(), "test cannot inspect shared name")
        return False

    def close(self):
        if self.mapping is not None:
            self.mapping.close()
            self.mapping = None
        if self.libc.shm_unlink(self.name.encode()) < 0 and ctypes.get_errno() != errno.ENOENT:
            raise OSError(ctypes.get_errno(), "test shm_unlink failed")

    def __enter__(self):
        return self

    def __exit__(self, *unused):
        self.close()


def frame(payload=b"", request_id=1, kind=1, status=0, flags=1, magic=MAGIC, version=1, size=None):
    return HEADER.pack(magic, version, kind, request_id, len(payload) if size is None else size, status, flags) + payload


def exact(peer, length):
    data = bytearray()
    while len(data) < length:
        part = peer.recv(length - len(data))
        if not part:
            raise EOFError("truncated frame")
        data.extend(part)
    return bytes(data)


def receive(peer):
    header = HEADER.unpack(exact(peer, HEADER.size))
    if header[4] > MAX_BYTES:
        raise ValueError("response too large")
    return header, exact(peer, header[4])


class Server:
    def __init__(self, path, *options):
        self.path = str(path)
        self.log = tempfile.TemporaryFile(mode="w+t")
        server_options = list(options)
        if "--timeout-ms" not in server_options:
            server_options += ["--timeout-ms", "1000"]
        self.process = subprocess.Popen(
            [str(BIN / "edgerpc_server"), "--socket", self.path, *server_options],
            stdout=self.log, stderr=self.log,
        )
        deadline = time.monotonic() + 5
        readiness_error = "no functional response yet"
        try:
            while time.monotonic() < deadline:
                if self.process.poll() is not None:
                    raise RuntimeError("server exited during startup: " + self.output())
                if Path(self.path).exists():
                    # A successful functional request is the readiness check.
                    # A stale path can exist before the restarted process has rebound it.
                    try:
                        self.demo("--elements", "1", "--timeout-ms", "1000", "--verify")
                        return
                    except subprocess.CalledProcessError as error:
                        readiness_error = error.stderr
                time.sleep(0.01)
            raise RuntimeError("server startup timed out: " + readiness_error)
        except BaseException:
            self.stop()
            self.log.close()
            raise

    def output(self):
        self.log.seek(0)
        return self.log.read()

    def demo(self, *options):
        return subprocess.run([str(BIN / "edgerpc_demo"), "--socket", self.path, *options],
                              text=True, capture_output=True, check=True, timeout=10)

    def stop(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=4)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
                raise

    def close(self):
        try:
            self.stop()
        finally:
            self.log.close()


class Integration(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="erpc-", dir="/tmp")
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / "rpc.sock"
        self.server = Server(self.path)
        self.addCleanup(self.server.close)

    def peer(self):
        peer = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.addCleanup(peer.close)
        peer.settimeout(3)
        peer.connect(str(self.path))
        return peer

    def assert_closed(self, peer):
        try:
            self.assertEqual(peer.recv(1), b"")
        except ConnectionResetError:
            pass

    def test_compute_and_noop(self):
        for workload in ("compute", "noop"):
            with self.subTest(workload=workload):
                result = self.server.demo("--elements", "1024", "--workload", workload, "--verify")
                self.assertIn("status=OK verified=true", result.stdout)

    def test_tensor_boundaries(self):
        for elements in (0, MAX_BYTES // 4):
            with self.subTest(elements=elements):
                result = self.server.demo("--elements", str(elements), "--verify")
                self.assertIn(f"elements={elements}", result.stdout)
                self.assertIn("verified=true", result.stdout)

    def test_wire_result_and_persistent_connection(self):
        with self.peer() as peer:
            for request_id in range(1, 33):
                values = (-4.0, request_id / 2, 7.25)
                peer.sendall(frame(struct.pack("!3f", *values), request_id=request_id))
                header, payload = receive(peer)
                self.assertEqual(header, (MAGIC, 1, 2, request_id, 12, 0, 1))
                self.assertEqual(struct.unpack("!3f", payload), tuple(2 * x + 1 for x in values))

    def test_fragmented_request(self):
        with self.peer() as peer:
            for byte in frame(struct.pack("!f", 3.5), request_id=77):
                peer.sendall(bytes([byte]))
                time.sleep(0.0002)
            header, payload = receive(peer)
            self.assertEqual(header[3], 77)
            self.assertEqual(struct.unpack("!f", payload), (8.0,))

    def restart_server(self, *options):
        self.server.stop()
        self.server = Server(self.path, *options)
        self.addCleanup(self.server.close)

    def test_async_demo_and_worker_configuration(self):
        self.restart_server("--workers", "2")
        result = self.server.demo("--mode", "async", "--requests", "32", "--verify")
        lines = result.stdout.splitlines()
        self.assertEqual(len(lines), 32)
        self.assertTrue(all("status=OK verified=true" in line for line in lines))
        self.assertEqual(len({line.split()[0] for line in lines}), 32)

    def test_server_overload_and_recovery(self):
        self.restart_server("--queue-capacity", "1", "--delay-us", "100000")
        with self.peer() as peer:
            peer.sendall(b"".join(frame(struct.pack("!f", float(i)), request_id=i) for i in range(1, 33)))
            replies = [receive(peer) for _ in range(32)]
            self.assertEqual({header[3] for header, _ in replies}, set(range(1, 33)))
            statuses = [header[5] for header, _ in replies]
            self.assertIn(3, statuses)
            self.assertIn(0, statuses)
            for header, payload in replies:
                if header[5] == 3:
                    self.assertEqual(header[2], 3)
                    self.assertIn(b"queue is full", payload)
                else:
                    self.assertEqual(header[2], 2)
                    self.assertEqual(struct.unpack("!f", payload), (2 * header[3] + 1,))
            peer.sendall(frame(struct.pack("!f", 3), request_id=100))
            header, payload = receive(peer)
            self.assertEqual((header[3], header[5]), (100, 0))
            self.assertEqual(struct.unpack("!f", payload), (7.0,))

    def test_disconnect_cancels_waiting_server_work(self):
        self.restart_server("--delay-us", "100000", "--queue-capacity", "32")
        with self.peer() as peer:
            peer.sendall(b"".join(frame(struct.pack("!f", 1), request_id=i) for i in range(1, 33)))
        # Cancelling the 32 queued tasks should permit a new client without draining all 3.2s of work.
        self.server.demo("--timeout-ms", "1000", "--verify")

    def test_execution_is_not_cancelled_by_idle_read_timeout(self):
        self.restart_server("--timeout-ms", "50", "--delay-us", "100000")
        result = self.server.demo("--requests", "2", "--timeout-ms", "1000", "--verify")
        self.assertEqual(result.stdout.count("status=OK verified=true"), 2)

    def test_async_benchmark_stress(self):
        destination = Path(self.directory.name) / "async.csv"
        result = subprocess.run(
            [str(BIN / "edgerpc_benchmark"), "--socket", str(self.path), "--mode", "socket-async",
             "--inflight", "16", "--requests", "1000", "--warmup", "32", "--size-kib", "4",
             "--csv", str(destination)], text=True, capture_output=True, check=True, timeout=15,
        )
        with destination.open(newline="") as stream:
            rows = list(csv.DictReader(stream))
        self.assertEqual(len(rows), 1000)
        self.assertEqual({int(row["request_id"]) for row in rows}, set(range(33, 1033)))
        self.assertTrue(all(row["mode"] == "socket-async" and row["inflight"] == "16" for row in rows))
        self.assertTrue(all(row["status"] == "0" and float(row["latency_us"]) > 0 for row in rows))
        self.assertIn("completed=1000 failures=0", result.stdout)

    def test_shared_mapping_and_metadata_only_reply(self):
        with SharedRegion() as region, self.peer() as peer:
            region.mapping[:16] = struct.pack("=4f", -4, 0, 2.5, 8)
            metadata = region.descriptor()
            self.assertEqual(len(metadata), 70)
            peer.sendall(frame(metadata, kind=4, request_id=73))
            header, payload = receive(peer)
            self.assertEqual(header, (MAGIC, 1, 5, 73, 0, 0, 1))
            self.assertEqual(payload, b"")
            self.assertEqual(struct.unpack("=4f", region.mapping[16:32]), (-7, 1, 6, 17))
            self.assertEqual(struct.unpack("=4f", region.mapping[:16]), (-4, 0, 2.5, 8))
            self.assertFalse(region.exists(), "admission must unlink the attached object")
            peer.sendall(frame(struct.pack("!f", 2), request_id=74))
            header, payload = receive(peer)
            self.assertEqual((header[2], header[3]), (2, 74))
            self.assertEqual(struct.unpack("!f", payload), (5.0,))

    def test_shared_demo_boundaries_and_modes(self):
        for workload in ("noop", "compute"):
            for elements in (0, 1024, MAX_BYTES // 4):
                with self.subTest(workload=workload, elements=elements):
                    result = self.server.demo("--transport", "shm", "--elements", str(elements),
                                              "--workload", workload, "--verify")
                    self.assertIn(f"elements={elements} status=OK verified=true", result.stdout)
        result = self.server.demo("--mode", "async", "--transport", "shm", "--requests", "32", "--verify")
        self.assertEqual(result.stdout.count("status=OK verified=true"), 32)

    def test_shared_invalid_metadata_and_recovery(self):
        cases = [dict(version=2), dict(name_length=29), dict(reserved=1), dict(region_bytes=7),
                 dict(region_bytes=2 * MAX_BYTES + 1), dict(region_bytes=64), dict(input_offset=1),
                 dict(output_offset="outside"), dict(output_offset=0), dict(input_offset=2**64 - 4),
                 dict(tensor_bytes=15), dict(elements=MAX_BYTES // 4 + 1),
                 dict(name="/evil_" + "0" * 24), dict(name="/erpc_" + "0" * 23 + "/")]
        with self.peer() as peer:
            for options in cases:
                with self.subTest(options=options), SharedRegion() as region:
                    fields = dict(options)
                    if fields.get("output_offset") == "outside":
                        fields["output_offset"] = region.size - 12
                    peer.sendall(frame(region.descriptor(**fields), kind=4))
                    header, _ = receive(peer)
                    self.assertEqual((header[2], header[5]), (3, 1))
                    self.assertTrue(region.exists(), "invalid metadata must not unlink an unvalidated name")
            peer.sendall(frame(struct.pack("!f", 2), request_id=40))
            header, payload = receive(peer)
            self.assertEqual((header[2], header[5]), (2, 0))
            self.assertEqual(struct.unpack("!f", payload), (5.0,))

    def test_shared_permissions_and_missing_region(self):
        with self.peer() as peer:
            with SharedRegion(mode=0o640) as region:
                peer.sendall(frame(region.descriptor(), kind=4))
                header, _ = receive(peer)
                self.assertEqual((header[2], header[5]), (3, 1))
                self.assertTrue(region.exists())
            with SharedRegion() as region:
                region.mapping[:16] = struct.pack("=4f", 1, 2, 3, 4)
                peer.sendall(frame(region.descriptor(), kind=4, request_id=2))
                header, _ = receive(peer)
                self.assertEqual((header[2], header[3], header[5]), (5, 2, 0))
                peer.sendall(frame(region.descriptor(), kind=4, request_id=3))  # Name was unlinked on admission.
                header, _ = receive(peer)
                self.assertEqual((header[2], header[5]), (3, 1))

    def test_shared_overload_reclaims_names(self):
        self.restart_server("--queue-capacity", "1", "--delay-us", "100000")
        with ExitStack() as stack, self.peer() as peer:
            regions = [stack.enter_context(SharedRegion(8)) for _ in range(16)]
            for index, region in enumerate(regions, 1):
                region.mapping[:4] = struct.pack("=f", float(index))
            peer.sendall(b"".join(frame(region.descriptor(output_offset=4, tensor_bytes=4, elements=1),
                                       kind=4, request_id=index) for index, region in enumerate(regions, 1)))
            replies = [receive(peer) for _ in regions]
            self.assertEqual({header[3] for header, _ in replies}, set(range(1, 17)))
            self.assertIn(3, [header[5] for header, _ in replies])
            self.assertIn(0, [header[5] for header, _ in replies])
            for header, payload in replies:
                self.assertIn(header[5], (0, 3))
                if header[5] == 0:
                    self.assertEqual((header[2], payload), (5, b""))
                    output = regions[header[3] - 1].mapping[4:8]
                    self.assertEqual(struct.unpack("=f", output), (2 * header[3] + 1,))
            self.assertTrue(all(not region.exists() for region in regions))

    def test_shared_server_crash_and_restart(self):
        self.restart_server("--delay-us", "300000")
        with SharedRegion() as region, self.peer() as peer:
            region.mapping[:16] = struct.pack("=4f", 1, 2, 3, 4)
            peer.sendall(frame(region.descriptor(), kind=4))
            deadline = time.monotonic() + 2
            while region.exists() and time.monotonic() < deadline:
                time.sleep(0.005)
            self.assertFalse(region.exists())
            self.server.process.kill()
            self.server.process.wait(timeout=3)
            self.assert_closed(peer)
            self.assertEqual(struct.unpack("=4f", region.mapping[:16]), (1, 2, 3, 4))
            self.restart_server()
            result = self.server.demo("--transport", "shm", "--verify")
            self.assertIn("status=OK verified=true", result.stdout)

    def test_shared_async_benchmark(self):
        destination = Path(self.directory.name) / "shared.csv"
        result = subprocess.run(
            [str(BIN / "edgerpc_benchmark"), "--socket", str(self.path), "--mode", "shm-async",
             "--inflight", "16", "--requests", "1000", "--warmup", "32", "--size-kib", "4",
             "--csv", str(destination)], text=True, capture_output=True, check=True, timeout=20,
        )
        with destination.open(newline="") as stream:
            rows = list(csv.DictReader(stream))
        self.assertEqual(len(rows), 1000)
        self.assertEqual({int(row["request_id"]) for row in rows}, set(range(33, 1033)))
        self.assertTrue(all(row["mode"] == "shm-async" and row["inflight"] == "16" and
                            row["status"] == "0" and float(row["latency_us"]) > 0 for row in rows))
        self.assertIn("completed=1000 failures=0", result.stdout)

    def test_shared_control_frame_size_limits(self):
        for kind, size in ((4, 0), (4, 69), (4, 71), (4, MAX_BYTES), (5, 4)):
            with self.subTest(kind=kind, size=size), self.peer() as peer:
                peer.sendall(frame(kind=kind, size=size))
                self.assert_closed(peer)
        self.server.demo("--verify")

    def test_malformed_headers_and_recovery(self):
        bad_headers = [frame(magic=0), frame(version=9), frame(kind=9), frame(size=MAX_BYTES + 1)]
        for bad in bad_headers:
            with self.subTest(header=bad), self.peer() as peer:
                peer.sendall(bad)
                self.assert_closed(peer)
            self.server.demo("--verify")

    def test_invalid_requests_return_errors(self):
        cases = [frame(b"abc"), frame(flags=99), frame(request_id=0), frame(status=1), frame(kind=2)]
        with self.peer() as peer:
            for bad in cases:
                peer.sendall(bad)
                header, payload = receive(peer)
                self.assertEqual(header[2], 3)
                self.assertEqual(header[5], 1)
                self.assertIn(b"invalid request", payload)
            peer.sendall(frame(struct.pack("!f", 2.0), request_id=12))
            header, payload = receive(peer)
            self.assertEqual((header[3], header[5]), (12, 0))
            self.assertEqual(struct.unpack("!f", payload), (5.0,))

    def test_truncated_frames_and_recovery(self):
        for bad in (frame()[:7], frame(size=4), frame(b"ab", size=4)):
            with self.subTest(data=bad), self.peer() as peer:
                peer.sendall(bad)
                peer.shutdown(socket.SHUT_WR)
                self.assert_closed(peer)
            self.server.demo("--verify")

    def test_client_disconnect_during_response(self):
        with self.peer() as peer:
            peer.sendall(frame(struct.pack("!f", 2)))
            peer.shutdown(socket.SHUT_RDWR)
        self.server.demo("--verify")

    def test_idle_timeout_reclaims_connection(self):
        with self.peer() as peer:
            self.assert_closed(peer)  # Server has a one-second idle read deadline.
        self.server.demo("--verify")

    def test_benchmark_csv_and_warmup(self):
        destination = Path(self.directory.name) / "samples.csv"
        result = subprocess.run(
            [str(BIN / "edgerpc_benchmark"), "--socket", str(self.path), "--size-kib", "4",
             "--requests", "12", "--warmup", "4", "--csv", str(destination)],
            text=True, capture_output=True, check=True, timeout=10,
        )
        with destination.open(newline="") as stream:
            rows = list(csv.DictReader(stream))
        self.assertEqual(len(rows), 12)
        self.assertEqual([int(row["request_id"]) for row in rows], list(range(5, 17)))
        self.assertTrue(all(row["status"] == "0" and row["tensor_bytes"] == "4096" for row in rows))
        self.assertTrue(all(math.isfinite(float(row["latency_us"])) and float(row["latency_us"]) > 0 for row in rows))
        self.assertTrue(all(row["queue_us"] == row["execute_us"] == row["buffer_wait_us"] == "" for row in rows))
        fields = dict(field.split("=", 1) for field in result.stdout.split())
        self.assertEqual(fields["completed"], "12")
        self.assertLessEqual(float(fields["p50_us"]), float(fields["p95_us"]))
        self.assertLessEqual(float(fields["p95_us"]), float(fields["p99_us"]))
        self.assertGreater(float(fields["throughput_rps"]), 0)

    def test_clean_shutdown_and_restart(self):
        self.server.stop()
        self.assertEqual(self.server.process.returncode, 0)
        self.assertFalse(self.path.exists())
        restarted = Server(self.path)
        self.addCleanup(restarted.close)
        restarted.demo("--verify")

    def test_stale_socket_after_crash(self):
        self.server.process.kill()
        self.server.process.wait(timeout=3)
        self.assertTrue(self.path.exists())
        restarted = Server(self.path)
        self.addCleanup(restarted.close)
        restarted.demo("--verify")

    def test_refuses_active_socket(self):
        result = subprocess.run([str(BIN / "edgerpc_server"), "--socket", str(self.path)],
                                text=True, capture_output=True, timeout=3)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("already in use", result.stderr)
        self.server.demo("--verify")

    def test_preserves_regular_file(self):
        regular = Path(self.directory.name) / "keep.txt"
        regular.write_text("do not remove")
        result = subprocess.run([str(BIN / "edgerpc_server"), "--socket", str(regular)],
                                text=True, capture_output=True, timeout=3)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(regular.read_text(), "do not remove")

    def test_rejects_invalid_cli_options(self):
        for binary, options in [
            ("edgerpc_demo", ["--mode", "unknown"]),
            ("edgerpc_demo", ["--transport", "unknown"]),
            ("edgerpc_demo", ["--elements", str(MAX_BYTES // 4 + 1)]),
            ("edgerpc_demo", ["--elements", "-1"]),
            ("edgerpc_demo", ["--elements"]),
            ("edgerpc_demo", ["--unknown"]),
            ("edgerpc_server", ["--workers", "17"]),
            ("edgerpc_benchmark", ["--requests", "0"]),
            ("edgerpc_benchmark", ["--inflight", "2"]),
        ]:
            with self.subTest(binary=binary, options=options):
                result = subprocess.run([str(BIN / binary), *options], capture_output=True, timeout=3)
                self.assertNotEqual(result.returncode, 0)

    def test_client_rejects_broken_responses(self):
        cases = {
            "disconnect": b"",
            "truncated": frame(kind=2)[:5],
            "wrong_id": frame(struct.pack("!f", 1), kind=2, request_id=99),
            "wrong_operation": frame(struct.pack("!f", 1), kind=2, flags=0),
            "wrong_size": frame(kind=2),
            "wrong_type": frame(struct.pack("!f", 1)),
            "wrong_status": frame(struct.pack("!f", 1), kind=2, status=2),
            "remote_error": frame(b"backend failed", kind=3, status=2),
            "invalid_error_status": frame(kind=3),
            "oversized": frame(kind=2, size=MAX_BYTES + 1),
        }
        for name, response in cases.items():
            with self.subTest(case=name):
                result = self.fake_server(response)
                self.assertNotEqual(result.returncode, 0)

    def fake_server(self, response, delay=0):
        path = Path(self.directory.name) / "fake.sock"
        if path.exists():
            path.unlink()
        errors = []
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as listener:
            listener.settimeout(3)
            listener.bind(str(path))
            listener.listen(1)

            def serve():
                try:
                    peer, _ = listener.accept()
                    with peer:
                        peer.settimeout(3)
                        receive(peer)
                        time.sleep(delay)
                        peer.sendall(response)
                except BrokenPipeError:
                    pass
                except Exception as error:
                    errors.append(error)

            thread = threading.Thread(target=serve)
            thread.start()
            try:
                result = subprocess.run(
                    [str(BIN / "edgerpc_demo"), "--socket", str(path), "--elements", "1",
                     "--timeout-ms", "100", "--verify"], text=True, capture_output=True, timeout=3,
                )
            finally:
                thread.join(timeout=4)
            self.assertFalse(thread.is_alive(), "fake server did not finish")
            self.assertEqual(errors, [])
            return result

    def test_client_timeout(self):
        result = self.fake_server(frame(struct.pack("!f", -31.0), kind=2), delay=0.3)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("timed out", result.stderr)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin-dir", type=Path, required=True)
    options, unittest_args = parser.parse_known_args()
    BIN = options.bin_dir.resolve()
    unittest.main(argv=[__file__, *unittest_args], verbosity=2)
