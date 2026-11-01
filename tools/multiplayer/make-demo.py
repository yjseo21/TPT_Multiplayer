"""Create a scene through two real clients and render it with tpt-remote.

Usage: python3 tools/multiplayer/make-demo.py SERVER CLIENT OUTPUT_DIRECTORY
"""
from pathlib import Path
import runpy
import subprocess
import sys
import time

repo = Path(__file__).resolve().parents[2]
fixture = runpy.run_path(str(repo / "tests/multiplayer/network_test.py"), run_name="fixtures")
out = Path(sys.argv[3]).resolve()
out.mkdir(parents=True, exist_ok=True)
process, port, log, env = fixture["start"](out, "--save", str(out / "shared-world.cps"))
clients = []
try:
    for i in range(2):
        c = fixture["WS"](port)
        c.hello(admin=i == 0)
        clients.append(c)
    sequences = [0, 0]

    def stroke(player, element, x1, y1, x2, y2, radius):
        sequences[player] += 1
        clients[player].send(dict(seq=sequences[player], op="brush", element=element,
                                 x1=x1, y1=y1, x2=x2, y2=y2, radius=radius))
        clients[player].control("ack", sequences[player])
        time.sleep(.04)

    # METL basin and divider; WATR on the left and DUST on the right.
    for segment in [(80, 180, 80, 330), (80, 330, 530, 330), (530, 180, 530, 330), (305, 235, 305, 330)]:
        stroke(0, 14, *segment, 3)
    for y in range(170, 260, 12):
        stroke(1, 2, 100, y, 280, y, 6)
    for y in range(90, 190, 12):
        stroke(0, 1, 340, y, 495, y, 6)
    sequences[0] += 1
    clients[0].send(dict(seq=sequences[0], op="pause", paused=False))
    clients[0].control("ack", sequences[0])
    result = subprocess.run([fixture["CLIENT"], "--url", f"ws://127.0.0.1:{port}", "--headless",
                             "--frames", "30", "--output", str(out / "shared-canvas.png")],
                            env=env, capture_output=True, text=True, timeout=25)
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    print(result.stdout.strip())
    print(out / "shared-canvas.png")
finally:
    for c in clients:
        c.close()
    fixture["stop"](process, log)
