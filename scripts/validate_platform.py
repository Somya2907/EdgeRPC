#!/usr/bin/env python3
"""Record full tests and real-process socket/SHM/pool checks on the current platform."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import signal
import subprocess
import tempfile
import time

ROOT=Path(__file__).resolve().parents[1]


def write(path,value):
    path.write_text(json.dumps(value,indent=2)+'\n')


def normalize(machine):
    return {'aarch64':'arm64','amd64':'x86_64'}.get(machine.lower(),machine.lower())


def validate(options):
    build=options.bin_dir.resolve()
    output=(options.output or ROOT/'results'/('platform-'+datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')+f'-{os.getpid()}')).resolve()
    output.mkdir(parents=True,exist_ok=False)
    result=dict(status='running',system=platform.system(),machine=platform.machine(),byteorder=__import__('sys').byteorder,
                platform=platform.platform(),timestamp_utc=datetime.now(timezone.utc).isoformat(),checks=[])
    result['source_sha256']={str(path.relative_to(ROOT)):hashlib.sha256(path.read_bytes()).hexdigest()
                             for directory in ('src','include','apps','tests','scripts')
                             for path in sorted((ROOT/directory).rglob('*')) if path.suffix in ('.cpp','.hpp','.py','.sh')}
    result['binary_sha256']={name:hashlib.sha256((build/name).read_bytes()).hexdigest() for name in
                             ('edgerpc_server','edgerpc_demo','edgerpc_benchmark','edgerpc_stress')}
    cache=build/'CMakeCache.txt'
    if cache.exists():
        result['build_settings']=[line for line in cache.read_text().splitlines() if line.startswith(
            ('CMAKE_BUILD_TYPE:','CMAKE_CXX_COMPILER:','CMAKE_CXX_FLAGS:','CMAKE_CXX_FLAGS_RELEASE:',
             'EDGERPC_SANITIZERS:','EDGERPC_THREAD_SANITIZER:'))]
    write(output/'validation.json',result)

    def run(name,command,timeout=60):
        entry=dict(name=name,command=[str(part) for part in command],status='running',log=name+'.log')
        result['checks'].append(entry)
        write(output/'validation.json',result)
        process=None
        try:
            with (output/entry['log']).open('w') as log:
                process=subprocess.Popen(entry['command'],stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
                entry['returncode']=process.wait(timeout=timeout)
            if entry['returncode']!=0:
                raise RuntimeError(f'{name} exited {entry["returncode"]}; inspect {output/entry["log"]}')
            entry['status']='passed'
        except BaseException as error:
            if process is not None and process.poll() is None:
                os.killpg(process.pid,signal.SIGKILL)
                process.wait()
            entry.update(status='failed',error=str(error))
            if os.environ.get('GITHUB_ACTIONS')=='true':
                log_path=output/entry['log']
                detail='\n'.join(log_path.read_text().splitlines()[-60:]) if log_path.exists() else str(error)
                message=(name+': '+detail)[-7000:]
                print('::error::'+message.replace('%','%25').replace('\r','%0D').replace('\n','%0A'),flush=True)
            raise
        finally:
            write(output/'validation.json',result)
        print('PASS',name,flush=True)
        return (output/entry['log']).read_text()

    try:
        if options.expected_system and platform.system()!=options.expected_system:
            raise RuntimeError('host OS does not match expected runner')
        if options.expected_machine and normalize(platform.machine())!=normalize(options.expected_machine):
            raise RuntimeError('host architecture does not match expected runner')
        run('ctest',['ctest','--test-dir',build,'--output-on-failure'],300)
        with tempfile.TemporaryDirectory(prefix='erpc-platform-',dir='/tmp') as temporary:
            path=Path(temporary)/'rpc.sock'
            with (output/'server.log').open('w') as log:
                server=subprocess.Popen([str(build/'edgerpc_server'),'--socket',str(path),'--workers','4',
                                         '--queue-capacity','64','--delay-us','100'],stdout=log,stderr=log)
                try:
                    deadline=time.monotonic()+10
                    while True:
                        probe=subprocess.run([str(build/'edgerpc_demo'),'--socket',str(path),'--elements','1','--verify'],
                                             text=True,capture_output=True,timeout=2)
                        if probe.returncode==0 and 'status=OK verified=true' in probe.stdout: break
                        if server.poll() is not None or time.monotonic()>=deadline:
                            raise RuntimeError('private server failed inference readiness')
                        time.sleep(.02)
                    for transport in ('socket','shm','pool'):
                        for elements in (0,1024,2097152):
                            text=run(f'{transport}-{elements}',[build/'edgerpc_demo','--socket',path,'--transport',transport,
                                    '--elements',str(elements),'--verify'])
                            if 'status=OK verified=true' not in text: raise RuntimeError('missing verified result')
                    for slots in (1,2,4):
                        run(f'pool-slots-{slots}',[build/'edgerpc_demo','--socket',path,'--transport','pool','--mode','async',
                                                '--buffer-slots',str(slots),'--requests','32','--verify'])
                    for mode in ('socket-sync','socket-async','shm-async','shm-double-buffer'):
                        text=run(mode,[build/'edgerpc_benchmark','--socket',path,'--mode',mode,'--size-kib','4',
                                       '--inflight','1' if mode=='socket-sync' else '4','--workers','4',
                                       '--delay-us','100','--requests','64','--warmup','8','--csv',output/(mode+'.csv')])
                        if 'completed=64 failures=0' not in text: raise RuntimeError('benchmark did not complete')
                    run('concurrent-callers',[build/'edgerpc_stress','--socket',path,'--transport','mixed',
                                             '--threads','8','--requests','128'])
                finally:
                    if server.poll() is None:
                        server.terminate()
                        try: server.wait(timeout=5)
                        except subprocess.TimeoutExpired:
                            server.kill()
                            server.wait()
                            raise RuntimeError('server failed graceful shutdown')
            if path.exists(): raise RuntimeError('private socket remained after graceful shutdown')
        result['status']='passed'
    except BaseException as error:
        result.update(status='failed',error=str(error))
        raise
    finally:
        write(output/'validation.json',result)
    print(f'Validated {result["system"]}/{result["machine"]}; evidence: {output}',flush=True)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bin-dir',type=Path,default=ROOT/'build')
    parser.add_argument('--output',type=Path)
    parser.add_argument('--expected-system',choices=['Linux','Darwin'])
    parser.add_argument('--expected-machine',choices=['arm64','aarch64','x86_64','amd64'])
    options=parser.parse_args()
    def interrupted(signum,frame): raise KeyboardInterrupt(f'signal {signum}')
    signal.signal(signal.SIGTERM,interrupted)
    validate(options)
