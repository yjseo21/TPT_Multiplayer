"""Capture representative original-engine RGB frames for codec benchmarking.
Usage: python3 tools/multiplayer/capture_reference_canvases.py SERVER CLIENT DIR
The server binary need not have the new codec; these are raw reference images.
"""
import os
import sys
import subprocess
import time
from pathlib import Path

server, client, folder = map(str, sys.argv[1:4])
folder = Path(folder).resolve()
folder.mkdir(parents=True, exist_ok=True)
sys.argv = ["network_test.py", server, client]
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tests' / 'multiplayer'))
import network_test as legacy

process, port, log, env = legacy.start(folder, '--update-rate', '20', '--upload-kbps', '100000', '--bandwidth-kbps', '100000')
client_ws = None
try:
    client_ws = legacy.WS(port); client_ws.hello(admin=True)
    client_ws.frame()
    seq = 0
    for scene, element in [('water', 2), ('fire', 4), ('dust', 1)]:
        seq += 1; client_ws.send(dict(seq=seq, op='clear')); client_ws.control('ack', seq)
        seq += 1; client_ws.send(dict(seq=seq, op='pause', paused=True)); client_ws.control('ack', seq)
        # 40,000 particles in confined rectangle. FIRE evolves quickly; water
        # develops turbulent motion. Send once, before collecting running frames.
        seq += 1
        client_ws.send(dict(seq=seq, op='tool', tool={'water':'DEFAULT_PT_WATR','fire':'DEFAULT_PT_FIRE','dust':'DEFAULT_PT_DUST'}[scene],
                            action='box', x1=120, y1=45, x2=370, y2=195, radius=0))
        client_ws.control('ack', seq)
        client_ws.frame(revision=seq)
        seq += 1; client_ws.send(dict(seq=seq, op='pause', paused=False)); client_ws.control('ack', seq)
        # Raw output: exactly 612*384*3 B,G,R planes per capture, no header.
        samples = 20
        path = folder / f'{scene}.raw'
        last = -1
        with path.open('wb') as out:
            while samples:
                kind, f = client_ws.receive()
                if kind == 'frame' and f['tick'] != last and f['revision'] >= seq:
                    out.write(f['pixels']); last = f['tick']; samples -= 1
        print(scene, '20 frames:', path.stat().st_size, 'bytes')
        seq += 1; client_ws.send(dict(seq=seq, op='pause', paused=True)); client_ws.control('ack', seq)
finally:
    if client_ws: client_ws.close()
    legacy.stop(process, log)
