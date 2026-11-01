"""End-to-end exact checkpoint and command-only Linux replica tests.

Usage: python3 tests/multiplayer/replica_network_test.py SERVER REPLICA
Requires the Python standard library; uses the existing independent WebSocket
client instead of the C++ replica transport stack for command injection.
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

SERVER, REPLICA = (str(Path(p).resolve()) for p in sys.argv[1:3])


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def run_case(paused):
    with tempfile.TemporaryDirectory(prefix="tpt-replica-network-") as td:
        root = Path(td)
        port = free_port()
        env = dict(os.environ, TPT_ROOM_TOKEN="room-test", TPT_ADMIN_TOKEN="admin-test")
        log = (root / "server.log").open("w+")
        args = [SERVER, "--port", str(port), "--seed", "9001", "--run-for", "30", "--tick-rate", "30"]
        if paused:
            args.append("--paused")
        server = subprocess.Popen(args, cwd=root, env=env, stdout=log, stderr=subprocess.STDOUT)
        replica = None
        editor = None
        try:
            for _ in range(100):
                log.seek(0)
                if "Listening on" in log.read():
                    break
                if server.poll() is not None:
                    raise AssertionError("server terminated: " + (root / "server.log").read_text())
                time.sleep(0.1)
            else:
                raise AssertionError("server never started")

            output = root / "replica.png"
            # Paused: edits applied within one tick share a seal, so the seal count says
            # nothing; run for a fixed time and check edits, ticks and fingerprints.
            # Active: the initial seal is at the join tick, so 76 seals span 75 ticks.
            limit = ["--run-for", "6"] if paused else ["--run-for", "18", "--min-seals", "76"]
            replica = subprocess.Popen([REPLICA, "--url", f"ws://127.0.0.1:{port}", *limit, "--output", str(output)],
                                       cwd=root, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            time.sleep(0.8)  # Allow checkpoint transfer before the commands.
            editor = WS(port)
            editor.hello(admin=True)
            seq = 0
            for i in range(3):
                seq += 1
                editor.send(brush(seq, x=100+i*33, y=50+i*20, element=1+i))
                editor.control("ack", seq)
                if paused:
                    seq += 1
                    editor.send(dict(seq=seq, op="step"))
                    editor.control("ack", seq)
            out, err = replica.communicate(timeout=24)
            assert replica.returncode == 0, f"replica failed: {out}\n{err}\n{(root/'server.log').read_text()}"
            assert output.read_bytes().startswith(b"\x89PNG\r\n\x1a\n")
            metrics = {k: int(v) for k, v in re.findall(r"(tick|seals|edits|matching_hashes)=(\d+)", out)}
            assert metrics["seals"] >= (2 if paused else 75), metrics
            assert metrics["edits"] >= 3, metrics
            assert metrics["matching_hashes"] >= 1, metrics
            if paused:
                assert metrics["tick"] >= 3, metrics
            else:
                assert metrics["tick"] >= 75, metrics
            traffic = {k:int(v) for k,v in re.findall(r"(wire_payload_bytes|checkpoint_payload_bytes|control_payload_bytes)=(\d+)",out)}
            assert sum(traffic[k] for k in ("checkpoint_payload_bytes","control_payload_bytes")) == traffic["wire_payload_bytes"]
            print("PASS", "paused with step" if paused else "active", metrics,
                  "network bytes", traffic, "local PNG size", output.stat().st_size, flush=True)
        finally:
            if editor:
                editor.close()
            if replica and replica.poll() is None:
                replica.kill()
                replica.wait()
            if server.poll() is None:
                server.send_signal(signal.SIGINT)
                try:
                    server.wait(timeout=6)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait()
            log.close()


if __name__ == "__main__":
    run_case(True)
    run_case(False)
