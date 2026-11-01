"""Three physics replicas plus one remote editor: live four-player room test.

Usage: python3 tests/multiplayer/replica_four_player_test.py SERVER REPLICA
No third-party Python dependencies. This checks correctness and instrumented
application-payload bytes, not real network capacity or cross-OS determinism.
"""
import os
from pathlib import Path
import re
import signal
import socket
import subprocess
import sys
import tempfile
import time
from network_test import WS, brush

SERVER, REPLICA = (str(Path(a).resolve()) for a in sys.argv[1:3])

def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]

with tempfile.TemporaryDirectory(prefix='tpt-4-player-') as directory:
    root = Path(directory)
    port = free_port()
    env = dict(os.environ, TPT_ROOM_TOKEN='room-test', TPT_ADMIN_TOKEN='admin-test')
    serverlog = (root/'server.log').open('w+')
    server = subprocess.Popen([SERVER, '--port', str(port), '--seed', '4132', '--run-for', '25', '--tick-rate', '30'],
                              cwd=root, env=env, stdout=serverlog, stderr=subprocess.STDOUT,
                              creationflags=subprocess.CREATE_NEW_PROCESS_GROUP if os.name == 'nt' else 0)  # CTRL_BREAK on Windows
    replicas=[]
    editor=None
    try:
        for _ in range(100):
            serverlog.seek(0)
            if 'Listening on' in serverlog.read(): break
            if server.poll() is not None: raise RuntimeError('server died')
            time.sleep(.1)
        else: raise RuntimeError('server never listened')
        for i in range(3):
            replicas.append(subprocess.Popen([REPLICA, '--url', f'ws://127.0.0.1:{port}', '--name', f'Physics-{i+1}',
                '--run-for', '20', '--min-seals', '82', '--output', str(root/f'replica{i+1}.png')],
                cwd=root, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True))
        time.sleep(1.0)
        editor=WS(port)
        editor.hello(admin=True)
        for i in range(6):
            editor.send(brush(i+1, x=85+i*35, y=80+i*8, element=1+i%3))
            editor.control('ack', i+1)
            time.sleep(.05)
        records=[]
        for i,replica in enumerate(replicas):
            stdout,stderr=replica.communicate(timeout=25)
            assert replica.returncode == 0, f'replica #{i+1}: {stderr}\n{stdout}\n{(root/"server.log").read_text()}'
            metrics={key:int(value) for key,value in re.findall(
                r'(tick|seals|edits|matching_hashes|wire_payload_bytes|checkpoint_payload_bytes|control_payload_bytes)=(\d+)', stdout)}
            assert metrics['tick']>=81 and metrics['edits']>=6 and metrics['matching_hashes']>=1,metrics
            assert metrics['wire_payload_bytes']==metrics['checkpoint_payload_bytes']+metrics['control_payload_bytes']
            assert (root/f'replica{i+1}.png').read_bytes().startswith(b'\x89PNG\r\n\x1a\n')
            records.append(metrics)
        print('FOUR-PLAYER LOCAL PHYSICS PASS: 3 verified replicas + 1 remote editor')
        for i,m in enumerate(records):
            print('replica',i+1,m)
        print('aggregate payload bytes',sum(m['wire_payload_bytes'] for m in records),
              'aggregate post-bootstrap bytes',sum(m['control_payload_bytes'] for m in records), flush=True)
    finally:
        if editor: editor.close()
        for r in replicas:
            if r.poll() is None:r.kill()
            r.wait()
        if server.poll() is None:
            server.send_signal(signal.CTRL_BREAK_EVENT if os.name == 'nt' else signal.SIGINT)
            try:server.wait(timeout=6)
            except subprocess.TimeoutExpired:server.kill();server.wait()
        serverlog.close()
