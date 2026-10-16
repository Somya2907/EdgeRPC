"""Exercise real concurrent callers, bounded pools, crashes and recovery."""
import argparse
from pathlib import Path
import selectors
import subprocess
import tempfile
import time
import unittest

import test_integration as wire

BIN=None


class Reliability(unittest.TestCase):
    def setUp(self):
        self.directory=tempfile.TemporaryDirectory(prefix='erpc-stress-',dir='/tmp')
        self.addCleanup(self.directory.cleanup)
        self.path=Path(self.directory.name)/'rpc.sock'

    def server(self,*options):
        server=wire.Server(self.path,*options)
        self.addCleanup(server.close)
        return server

    def command(self,transport,*options):
        return [str(BIN/'edgerpc_stress'),'--socket',str(self.path),'--transport',transport,*options]

    def run_stress(self,transport,*options):
        result=subprocess.run(self.command(transport,*options),text=True,capture_output=True,timeout=25)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn('verified=true settled=true',result.stdout)
        summary=dict(item.split('=',1) for item in result.stdout.splitlines()[-1].split())
        self.assertEqual(int(summary['submitted']),int(summary['successful'])+int(summary['errors']))
        return summary

    def active_client(self,transport,*options):
        process=subprocess.Popen(self.command(transport,*options),stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        def stop():
            if process.poll() is None: process.kill()
            process.communicate(timeout=5)
        self.addCleanup(stop)
        output=b''
        with selectors.DefaultSelector() as selector:
            selector.register(process.stdout,selectors.EVENT_READ)
            deadline=time.monotonic()+5
            while b'ACTIVE submissions=' not in output or b'\n' not in output:
                if time.monotonic()>=deadline: self.fail('stress client did not reach active submissions')
                if not selector.select(.1): continue
                part=process.stdout.read1(4096)
                if not part: self.fail('stress client exited before injection: '+output.decode())
                output+=part
        return process,output

    def test_concurrent_mixed_transports_and_workers(self):
        self.server('--workers','4','--queue-capacity','64','--delay-us','100')
        result=self.run_stress('mixed','--threads','8','--requests','128','--buffer-slots','2')
        self.assertEqual((int(result['successful']),int(result['errors'])),(1024,0))

    def test_two_slots_under_thousands_of_submissions(self):
        self.server('--workers','1','--queue-capacity','64','--delay-us','100')
        result=self.run_stress('pool','--threads','16','--requests','128','--buffer-slots','2')
        self.assertEqual((int(result['successful']),int(result['errors'])),(2048,0))

    def test_close_races_with_real_submissions(self):
        self.server('--workers','1','--queue-capacity','64','--delay-us','10000')
        for transport in ('socket','shm','pool'):
            with self.subTest(transport=transport):
                result=self.run_stress(transport,'--requests','64','--close-after','16')
                self.assertGreater(int(result['errors']),0)

    def test_server_crash_settles_all_transports_and_restarts(self):
        for transport in ('socket','shm','pool'):
            with self.subTest(transport=transport):
                server=self.server('--delay-us','100000','--queue-capacity','64')
                process,active=self.active_client(transport,'--requests','64','--allow-disconnect')
                server.process.kill()
                server.process.wait(timeout=3)
                stdout,stderr=process.communicate(timeout=10)
                output=(active+stdout).decode()
                self.assertEqual(process.returncode,0,output+stderr.decode())
                summary=dict(item.split('=',1) for item in output.splitlines()[-1].split())
                self.assertEqual(int(summary['submitted']),512)
                self.assertGreater(int(summary['errors']),0)
                self.assertEqual(int(summary['successful'])+int(summary['errors']),512)
                restarted=wire.Server(self.path)
                self.addCleanup(restarted.close)
                restarted.demo('--transport',transport,'--verify')
                restarted.stop()

    def test_killed_pool_client_does_not_poison_server(self):
        server=self.server('--delay-us','100000','--queue-capacity','64')
        process,_=self.active_client('pool','--requests','128')
        process.kill()
        process.communicate(timeout=5)
        result=server.demo('--transport','pool','--elements','4','--timeout-ms','1000','--verify')
        self.assertIn('status=OK verified=true',result.stdout)

    def test_repeated_connect_disconnect_with_unique_outputs(self):
        self.server('--workers','4','--queue-capacity','64')
        for _ in range(4):
            for transport in ('socket','shm','pool'):
                result=self.run_stress(transport,'--threads','4','--requests','32')
                self.assertEqual((int(result['successful']),int(result['errors'])),(128,0))

    def test_repeated_overload_then_recovery(self):
        import struct
        server=self.server('--queue-capacity','1','--delay-us','5000')
        import socket
        with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as peer:
            peer.settimeout(3)
            peer.connect(str(self.path))
            for cycle in range(10):
                first=cycle*100+1
                peer.sendall(b''.join(wire.frame(struct.pack('!f',float(index)),request_id=index)
                                      for index in range(first,first+32)))
                replies=[wire.receive(peer) for _ in range(32)]
                self.assertEqual({header[3] for header,_ in replies},set(range(first,first+32)))
                self.assertIn(3,{header[5] for header,_ in replies})
                self.assertIn(0,{header[5] for header,_ in replies})
                for header,payload in replies:
                    self.assertIn(header[5],(0,3))
                    if header[5]==0: self.assertEqual(struct.unpack('!f',payload),(2*header[3]+1,))
                peer.sendall(wire.frame(struct.pack('!f',3),request_id=first+50))
                header,payload=wire.receive(peer)
                self.assertEqual((header[3],header[5],struct.unpack('!f',payload)),(first+50,0,(7.0,)))
        server.demo('--verify')

    def test_socket_replacement_survives_shutdown(self):
        server=self.server()
        self.path.unlink()
        self.path.write_text('replacement owned by caller')
        server.stop()
        self.assertEqual(self.path.read_text(),'replacement owned by caller')

    def test_symlink_target_is_preserved(self):
        target=Path(self.directory.name)/'target'
        target.write_text('do not remove')
        self.path.symlink_to(target)
        result=subprocess.run([str(BIN/'edgerpc_server'),'--socket',str(self.path)],text=True,capture_output=True,timeout=5)
        self.assertNotEqual(result.returncode,0)
        self.assertTrue(self.path.is_symlink())
        self.assertEqual(target.read_text(),'do not remove')


if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--bin-dir',type=Path,required=True)
    options,args=parser.parse_known_args()
    BIN=options.bin_dir.resolve()
    wire.BIN=BIN
    unittest.main(argv=[__file__,*args],verbosity=2)
