"""End-to-end tests using a small independent, standard-library WebSocket client."""
import base64
import hashlib
import json
import os
import pathlib
import re
from pathlib import Path
import shutil
import signal
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import threading
import time
import zlib

def _build_id():
    # Single source of truth: the C++ BuildId the binaries were compiled with.
    header = pathlib.Path(__file__).resolve().parents[2] / "src" / "multiplayer" / "Protocol.h"
    match = re.search(r'BuildId\s*=\s*"([^"]+)"', header.read_text())
    if not match:
        raise RuntimeError("cannot find BuildId in " + str(header))
    return match.group(1)
BUILD = _build_id()
SERVER, CLIENT = map(lambda p: str(Path(p).resolve()), sys.argv[1:3])


class WS:
    def __init__(self, port, ca=None):
        s = socket.create_connection(("127.0.0.1", port), timeout=5)
        if ca:
            s = ssl.create_default_context(cafile=str(ca)).wrap_socket(s, server_hostname="127.0.0.1")
        self.s, self.buffer = s, b""
        self.canvas, self.auto_ack = None, True
        self.send_lock = threading.Lock()
        self.canvas_bytes = 0
        key = base64.b64encode(os.urandom(16)).decode()
        s.sendall((f"GET / HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nUpgrade: websocket\r\n"
                   f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
        while b"\r\n\r\n" not in self.buffer:
            self.buffer += s.recv(4096)
        header, self.buffer = self.buffer.split(b"\r\n\r\n", 1)
        assert b"101" in header.split(b"\r\n")[0], header
        accept = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest())
        assert accept in header, header

    def read(self, n):
        while len(self.buffer) < n:
            data = self.s.recv(max(4096, n-len(self.buffer)))
            if not data:
                raise EOFError("websocket closed")
            self.buffer += data
        out, self.buffer = self.buffer[:n], self.buffer[n:]
        return out

    def send_raw(self, data, opcode=1, fin=True):
        mask = os.urandom(4)
        n = len(data)
        size = bytes([n | 128]) if n < 126 else (b"\xfe" + struct.pack("!H", n) if n < 65536 else b"\xff" + struct.pack("!Q", n))
        with self.send_lock:
            self.s.sendall(bytes([opcode | (128 if fin else 0)]) + size + mask + bytes(c ^ mask[i % 4] for i, c in enumerate(data)))

    def send(self, value):
        self.send_raw(json.dumps(value).encode())

    def receive(self):
        chunks, kind = [], None
        while True:
            a, b = self.read(2)
            op, n = a & 15, b & 127
            assert not b & 128, "server must not mask"
            if n == 126:
                n = struct.unpack("!H", self.read(2))[0]
            elif n == 127:
                n = struct.unpack("!Q", self.read(8))[0]
            assert n <= 32*1024*1024
            data = self.read(n)
            if op == 8:
                return "close", data[2:].decode(errors="replace")
            if op == 9:
                self.send_raw(data, opcode=10)
                continue
            if op == 10:
                continue
            if op:
                kind = op
            chunks.append(data)
            if a & 128:
                data = b"".join(chunks)
                if kind == 2:
                    self.canvas_bytes += len(data)
                    assert data[:4] == b"TPC3" and len(data) >= 52
                    mode, view, paused, reserved = data[4:8]
                    fid, base, tick, revision, crc = struct.unpack_from("<QQQQI", data, 8)
                    assert mode <= 2 and view <= 2 and paused <= 1 and reserved == 0
                    assert not self.canvas or fid > self.canvas["id"]
                    if mode:
                        assert self.canvas and base == self.canvas["id"] and view == self.canvas["view"]
                    else:
                        assert base == 0
                    if mode == 2:
                        assert len(data) == 52
                        pixels = self.canvas["pixels"]
                    else:
                        pixels = zlib.decompress(data[52:])
                        assert len(pixels) == 612*384*3
                        if mode == 1:
                            pixels = bytes(a ^ b for a, b in zip(pixels, self.canvas["pixels"]))
                    assert zlib.crc32(pixels) == crc
                    self.canvas = dict(id=fid, mode=mode, view=view, tick=tick, revision=revision, pixels=pixels,
                                       captured_us=struct.unpack_from("<Q", data, 44)[0])
                    if self.auto_ack:
                        self.send(dict(type="frame_ack", frame=fid))
                    return "frame", self.canvas
                return "text", json.loads(data)

    def hello(self, admin=False, **changes):
        value = dict(type="hello", protocol=3, build=BUILD, name="Test", token="room-test",
                     adminToken="admin-test" if admin else "")
        value.update(changes)
        self.send(value)
        kind, message = self.receive()
        assert kind == "text" and message["type"] == "welcome", (kind, message)

    def control(self, kind, seq):
        for _ in range(200):
            t, v = self.receive()
            assert t != "close", v
            if t == "text" and v.get("seq") == seq:
                assert v["type"] == kind, v
                return v
        raise AssertionError("no control response")

    def frame(self, revision=0, tick=0, view=0):
        for _ in range(200):
            if self.canvas and self.canvas["revision"] >= revision and self.canvas["tick"] >= tick and self.canvas["view"] == view:
                return self.canvas
            t, packet = self.receive()
            assert t != "close", packet
        raise AssertionError("no suitable frame")

    def close(self):
        self.s.close()


def port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def start(directory, *args, listen_port=None):
    p = listen_port or port()
    log = open(directory / f"server-{p}.log", "w+")
    env = dict(os.environ, TPT_ROOM_TOKEN="room-test", TPT_ADMIN_TOKEN="admin-test")
    process = subprocess.Popen([SERVER, "--port", str(p), "--paused", "--run-for", "60", *args],
                               cwd=directory, env=env, stdout=log, stderr=subprocess.STDOUT,
                               creationflags=subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0)
    for _ in range(100):
        log.seek(0)
        if "Listening on" in log.read():
            return process, p, log, env
        if process.poll() is not None:
            log.seek(0)
            raise AssertionError(log.read())
        time.sleep(.1)
    process.kill()
    raise AssertionError("server did not start")


def stop(process, log):
    if process.poll() is None:
        process.send_signal(signal.CTRL_BREAK_EVENT if os.name == "nt" else signal.SIGINT)
        try:
            process.wait(timeout=8)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
            raise AssertionError("server failed to stop promptly")
    log.seek(0)
    text = log.read()
    log.close()
    assert process.returncode == 0, f"server exit code {process.returncode}\n{text}"


def brush(seq, x=100, y=40, element=1):
    return dict(seq=seq, op="brush", x1=x, y1=y, x2=x+10, y2=y, radius=2, element=element)


def run():
    with tempfile.TemporaryDirectory(prefix="tpt-mp-") as tmp:
        d = Path(tmp)
        process, p, log, env = start(d, "--save", str(d / "world.cps"))
        clients = []
        try:
            bad = WS(p)
            bad.send(dict(type="hello", protocol=999, build=BUILD))
            assert bad.receive()[0] == "close"
            bad.close()
            bad = WS(p)
            bad.send(dict(type="hello", protocol=3, build=BUILD, token="wrong", name="Test"))
            assert bad.receive()[0] == "close"
            bad.close()
            for i in range(4):
                c = WS(p); c.hello(admin=i == 0); clients.append(c)
            extra = WS(p)
            extra.send(dict(type="hello", protocol=3, build=BUILD, name="Fifth", token="room-test"))
            kind, reason = extra.receive()
            assert kind == "close" and "full" in reason
            extra.close()
            clients[0].send(brush(1))
            clients[0].control("ack", 1)
            clients[1].send(brush(1, x=300, element=2))
            clients[1].control("ack", 1)
            frames = [c.frame(revision=2) for c in clients]
            assert all(f["pixels"] == frames[0]["pixels"] for f in frames), "four clients disagree on the paused canvas"
            assert any(frames[0]["pixels"])
            clients[0].send(brush(1, x=400))
            clients[0].control("error", 1)
            clients[1].send(dict(seq=2, op="pause", paused=False))
            clients[1].control("error", 2)
            invalid = brush(2); invalid["radius"] = 1000000
            clients[0].send(invalid); clients[0].control("error", 2)
            clients[0].send_raw(b'{"seq":3,"seq":4}')
            clients[0].control("error", 0)
            for seq in range(3, 13):
                clients[0].send(dict(seq=seq, op="step"))
                clients[0].control("ack", seq)
            advanced = clients[0].frame(revision=12, tick=10)
            assert advanced["pixels"] != frames[0]["pixels"], "stepping did not move particles"
            clients[3].close(); clients.pop()
            time.sleep(.2)
            joined = WS(p); joined.hello(); clients.append(joined)
            assert joined.frame(revision=12, tick=10)["pixels"] == advanced["pixels"], "late join lost current canvas"
            # Switching one viewer must preserve everybody else's normal view.
            joined.send(dict(type="view", view=1))
            heat = joined.frame(revision=12, tick=10, view=1)
            assert heat["mode"] == 0 and heat["pixels"] != advanced["pixels"]
            assert clients[0].frame(revision=12)["pixels"] == advanced["pixels"]
            joined.send(dict(type="view", view=2))
            assert joined.frame(revision=12, view=2)["mode"] == 0
            joined.send(dict(type="view", view=0))
            assert joined.frame(revision=12)["pixels"] == advanced["pixels"]
            joined.send(dict(type="resync"))
            while True:
                kind, value = joined.receive()
                if kind == "frame" and value["mode"] == 0:
                    assert value["pixels"] == advanced["pixels"]
                    break
            # A paused canvas sends only tiny heartbeat frames.
            before = joined.canvas_bytes
            for _ in range(2):
                assert joined.receive()[0] == "frame"
            assert joined.canvas_bytes-before == 104
            clients[3].close(); clients.pop()
            time.sleep(.2)
            # The actual C++ client decodes and renders remote frames, including SDL's dummy backend.
            result = subprocess.run([CLIENT, "--url", f"ws://127.0.0.1:{p}", "--frames", "3", "--output", str(d / "frame.png")],
                                    env=dict(env, SDL_VIDEODRIVER="dummy"), capture_output=True, text=True, timeout=20)
            assert result.returncode == 0, result.stdout + result.stderr
            assert (d / "frame.png").read_bytes().startswith(b"\x89PNG")
            time.sleep(.2)
            # An oversized frame declaration must be rejected before its payload is allocated/read.
            oversized = WS(p); oversized.hello()
            oversized.s.sendall(b"\x82\xff" + struct.pack("!Q", 33*1024*1024) + b"mask")
            for _ in range(10):
                if oversized.receive()[0] == "close":
                    break
            else:
                raise AssertionError("oversized frame was not closed")
            oversized.close()
            print("PASS: 4 clients, room limit, auth/version, editing, lossless canvas, pause/step, late join, independent views, resync, idle traffic, native rendering, transport limit")
        finally:
            for c in clients:
                c.close()
            stop(process, log)
        assert (d / "world.cps").stat().st_size > 100
        process, p, log, env = start(d, "--load", str(d / "world.cps"))
        try:
            c = WS(p); c.hello()
            assert any(c.frame()["pixels"])
            c.close()
        finally:
            stop(process, log)
        print("PASS: world save and load")

        # Busy liquid scene: one stalled receiver must not stall another player or physics.
        process, p, log, env = start(d, "--bandwidth-kbps", "128")
        fast = slow = None
        try:
            fast = WS(p); fast.hello(admin=True)
            seq = 0
            for y in (50, 82, 114):
                for x in range(40, 461, 70):
                    seq += 1
                    fast.send(dict(seq=seq, op="brush", x1=x, y1=y, x2=x+40, y2=y, radius=16, element=2))
                    fast.control("ack", seq)
                    time.sleep(.15)
            fast.frame(revision=seq)
            slow = WS(p); slow.hello(); slow.auto_ack = False
            seq += 1
            fast.send(dict(seq=seq, op="pause", paused=False)); fast.control("ack", seq)
            start_tick = fast.canvas["tick"]
            started, before, count = time.monotonic(), fast.canvas_bytes, 0
            while time.monotonic()-started < 6:
                kind, value = fast.receive()
                assert kind != "close", value
                count += kind == "frame"
            elapsed = time.monotonic()-started
            used = fast.canvas_bytes-before
            # Token bucket permits one worst-case keyframe burst plus sustained 16 KB/s.
            assert used <= 612*384*3+4096 + elapsed*16000, (used, elapsed)
            assert fast.canvas["tick"] > start_tick+60 and count >= 4, (fast.canvas, count)
            for _ in range(1):
                assert slow.receive()[0] == "frame"
            slow.s.settimeout(.2)
            try:
                extra = slow.receive()
                raise AssertionError(f"unacknowledged initial stream exceeded one packet: {extra[0]}")
            except socket.timeout:
                pass
            slow.s.settimeout(5)
            slow.auto_ack = True
            slow.send(dict(type="frame_ack", frame=slow.canvas["id"]))
            old_tick = slow.canvas["tick"]
            assert slow.frame(tick=old_tick+1)["tick"] > old_tick
            print(f"PASS: slow-player isolation, initial ACK gate, resuming deltas, 128 kbps budget ({used} payload bytes/{elapsed:.2f}s including burst)")
            # Clearing an animated scene must also clear the renderer's accumulated glow.
            seq += 1
            fast.send(dict(seq=seq, op="pause", paused=True)); fast.control("ack", seq)
            seq += 1
            fast.send(dict(seq=seq, op="clear")); fast.control("ack", seq)
            seq += 1
            fast.send(brush(seq, element=4)); fast.control("ack", seq)
            assert any(fast.frame(revision=seq)["pixels"])
            seq += 1
            fast.send(dict(seq=seq, op="clear")); fast.control("ack", seq)
            assert not any(fast.frame(revision=seq)["pixels"]), "clearing left stale canvas/glow pixels"
        finally:
            if fast: fast.close()
            if slow: slow.close()
            stop(process, log)

        if not shutil.which("openssl"):
            print("SKIP: TLS integration requires openssl to generate a temporary certificate")
            return
        cert, key = d / "cert.pem", d / "key.pem"
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                        "-subj", "/CN=127.0.0.1", "-addext", "subjectAltName=IP:127.0.0.1",
                        "-keyout", str(key), "-out", str(cert)], check=True, capture_output=True)
        process, p, log, env = start(d, "--cert", str(cert), "--key", str(key))
        try:
            c = WS(p, cert); c.hello(); c.frame(); c.close()
            command = [CLIENT, "--url", f"wss://127.0.0.1:{p}", "--headless", "--frames", "2"]
            trusted = subprocess.run(command + ["--ca", str(cert)], env=env, capture_output=True, text=True, timeout=20)
            assert trusted.returncode == 0, trusted.stdout + trusted.stderr
            untrusted = subprocess.run(command, env=env, capture_output=True, text=True, timeout=20)
            assert untrusted.returncode != 0, "client accepted untrusted server certificate"
        finally:
            stop(process, log)
        print("PASS: TLS with certificate verification and rejection of untrusted certificate")


if __name__ == "__main__":
    run()
