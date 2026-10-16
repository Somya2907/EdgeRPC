"""Exercise real server/client processes and independently encoded wire traffic."""

import argparse
import csv
import math
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import threading
import time
import unittest


HEADER = struct.Struct("!IHHQIII")
MAGIC = 0x45525043
MAX_BYTES = 8 * 1024 * 1024
BIN = None


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
        self.process = subprocess.Popen(
            [str(BIN / "edgerpc_server"), "--socket", self.path, "--timeout-ms", "1000", *options],
            stdout=self.log, stderr=self.log,
        )
        deadline = time.monotonic() + 5
        try:
            while time.monotonic() < deadline:
                if self.process.poll() is not None:
                    raise RuntimeError("server exited during startup: " + self.output())
                if Path(self.path).exists():
                    # A successful functional request is the readiness check.
                    self.demo("--elements", "1", "--verify")
                    return
                time.sleep(0.01)
            raise RuntimeError("server startup timed out")
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
            ("edgerpc_demo", ["--mode", "async"]),
            ("edgerpc_demo", ["--elements", str(MAX_BYTES // 4 + 1)]),
            ("edgerpc_demo", ["--elements", "-1"]),
            ("edgerpc_demo", ["--elements"]),
            ("edgerpc_demo", ["--unknown"]),
            ("edgerpc_server", ["--workers", "2"]),
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
