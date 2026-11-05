"""Crash/restart, reconnect and rotating autosave integration tests."""
import json
import re
import os
from pathlib import Path
import runpy
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import zlib

fixture = runpy.run_path(str(Path(__file__).with_name("network_test.py")), run_name="fixtures")


class DisconnectProxy:
    """Transparent local TCP relay that can drop existing connections on demand."""
    def __init__(self, target):
        self.target, self.sockets, self.threads = target, [], []
        self.lock, self.stopping = threading.Lock(), threading.Event()
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0)); self.listener.listen()
        self.port = self.listener.getsockname()[1]
        self.listener.settimeout(.2)
        self.acceptor = threading.Thread(target=self.accept, daemon=True)
        self.acceptor.start()

    @staticmethod
    def shutdown(s):
        try: s.shutdown(socket.SHUT_RDWR)
        except OSError: pass
        s.close()

    def pump(self, source, destination):
        try:
            while not self.stopping.is_set():
                data = source.recv(8192)
                if not data: break
                destination.sendall(data)
        except OSError:
            pass
        finally:
            self.shutdown(source); self.shutdown(destination)

    def accept(self):
        while not self.stopping.is_set():
            try: client, _ = self.listener.accept()
            except socket.timeout: continue
            except OSError: break
            try:
                upstream = socket.create_connection(("127.0.0.1", self.target), timeout=1)
                upstream.settimeout(None)
            except OSError:
                self.shutdown(client); continue
            with self.lock:
                self.sockets.extend((client, upstream))
            for a, b in ((client, upstream), (upstream, client)):
                worker = threading.Thread(target=self.pump, args=(a, b), daemon=True)
                self.threads.append(worker); worker.start()

    def disconnect(self):
        with self.lock:
            for s in self.sockets: self.shutdown(s)
            self.sockets.clear()

    def close(self):
        self.stopping.set(); self.listener.close(); self.acceptor.join(timeout=3)
        self.disconnect()
        for worker in self.threads: worker.join(timeout=3)
        assert not self.acceptor.is_alive() and all(not t.is_alive() for t in self.threads)


def until(predicate, message, timeout=12):
    deadline = time.monotonic()+timeout
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(.1)
    raise AssertionError(message)


def rows(path):
    if not path.exists():
        return []
    # The writer flushes one record per line; ignore an in-progress final line.
    result = []
    for line in path.read_text().splitlines():
        try:
            result.append(json.loads(line))
        except json.JSONDecodeError:
            pass
    return result


def run(d):
    directory = d / "autosaves"
    port = fixture["port"]()
    proxy = DisconnectProxy(port)
    env = dict(os.environ, TPT_ROOM_TOKEN="room-test", TPT_ADMIN_TOKEN="admin-test", SDL_VIDEODRIVER="dummy")
    client_log = open(d / "client.log", "w")
    metrics = d / "metrics.jsonl"
    # Start before the server exists: transient initial failures must also retry.
    native = subprocess.Popen([fixture["CLIENT"], "--url", f"ws://127.0.0.1:{proxy.port}", "--view", "1",
                               "--run-for", "35", "--metrics", str(metrics),
                               "--output-hud", str(d / "recovered.png")],
                              env=env, stdout=client_log, stderr=subprocess.STDOUT)
    server = log = editor = None
    starts = 0

    def launch():
        nonlocal starts
        starts += 1
        folder = d / f"server-{starts}"
        folder.mkdir()
        return fixture["start"](folder, "--autosave-dir", str(directory), "--autosave-interval", "1",
                                "--autosave-keep", "3", "--recover", listen_port=port)

    def saved_after(previous):
        files = sorted(directory.glob("autosave-*.cps"))
        return files if files and files[-1].name > previous else None

    def connected(count):
        return [r for r in rows(metrics) if r.get("connected") and r["connections"] >= count and r["view"] == 1]

    def crash():
        nonlocal server, log, editor
        if editor:
            editor.close(); editor = None
        server.kill(); server.wait(timeout=5)
        assert server.returncode != 0
        log.close(); server = log = None

    try:
        time.sleep(1.3)
        server, _, log, _ = launch()
        editor = fixture["WS"](port); editor.hello(admin=True)
        previous = ""
        for seq in range(1, 5):
            editor.send(fixture["brush"](seq, x=60+seq*60, element=14)); editor.control("ack", seq)
            files = until(lambda: saved_after(previous), "periodic autosave did not commit")
            previous = files[-1].name
        until(lambda: len(list(directory.glob("*.cps"))) == 3, "backup retention was not applied")
        until(lambda: connected(1), "native client did not recover from initial connection refusal")
        proxy.disconnect()
        until(lambda: connected(2), "native client did not reconnect after a live socket drop")
        assert server.poll() is None, "connection drop unexpectedly stopped the server"
        contender = subprocess.run([fixture["SERVER"], "--autosave-dir", str(directory), "--run-for", "1"],
                                   env=env, capture_output=True, text=True, timeout=5)
        assert contender.returncode != 0 and "already in use" in contender.stderr, contender.stderr
        # A failed write must leave all committed backups untouched and be retried.
        originals = {p.name: p.read_bytes() for p in directory.glob("*.cps")}
        directory.chmod(0o500)
        editor.send(fixture["brush"](5, x=400, element=14)); editor.control("ack", 5)
        log_path = Path(log.name)
        until(lambda: "Autosave failed" in log_path.read_text(), "save failure was not reported")
        assert all((directory / name).read_bytes() == value for name, value in originals.items())
        directory.chmod(0o700)
        files = until(lambda: saved_after(previous), "autosave did not retry after storage recovered")
        previous = files[-1].name
        # The new backup is committed before the oldest is removed (never fewer than
        # three good ones), so for a moment there are four.
        until(lambda: len(list(directory.glob("*.cps"))) <= 3, "backup retention was not applied after the retry")
        crash()
        # Incomplete temp files are ignored; a corrupt committed file falls back.
        latest = sorted(directory.glob("*.cps"))[-1]
        latest.write_bytes(b"intentionally corrupt test save")
        incomplete = directory / "autosave-99999999999999999999.cps.tmp"
        incomplete.write_bytes(b"interrupted write")
        time.sleep(2.5)
        server, _, log, _ = launch()
        assert "Skipping invalid autosave" in Path(log.name).read_text()
        editor = fixture["WS"](port); editor.hello(admin=True)
        editor.send(dict(type="view", view=1))
        recovered = editor.frame(revision=1, view=1)
        assert recovered["revision"] == 1, "a previous session replayed an edit"
        current = until(lambda: connected(3), "native client did not reconnect after crash/fallback")[-1]
        assert current["canvasChecksum"] == zlib.crc32(recovered["pixels"])
        assert incomplete.read_bytes() == b"interrupted write", "incomplete file was incorrectly used"
        editor.send(fixture["brush"](1, x=500, element=14)); ack = editor.control("ack", 1)
        assert ack["revision"] == 2, "new connection did not get fresh command sequencing"
        edited = editor.frame(revision=2, view=1)
        # Wait for the commit that contains revision 2. Comparing file names
        # raced: a paused world that was already saved is never saved again.
        log_path = Path(log.name)
        until(lambda: re.search(r"Autosave committed generation=\d+ revision=2\b", log_path.read_text()),
              "edited recovered world was not saved")
        crash()
        time.sleep(1.5)
        server, _, log, _ = launch()
        editor = fixture["WS"](port); editor.hello()
        editor.send(dict(type="view", view=1))
        final = editor.frame(revision=1, view=1)
        assert final["pixels"] == edited["pixels"], "second restart lost the committed edit"
        until(lambda: connected(4), "native client did not survive a second restart")
        editor.close(); editor = None
        native.wait(timeout=35)
        client_log.flush()
        assert native.returncode == 0, (d / "client.log").read_text()
        samples = rows(metrics)
        assert any(r["status"] == "RECONNECTING" and not r["connected"] for r in samples)
        assert samples[-1]["connections"] == 4 and samples[-1]["view"] == 1
        assert samples[-1]["canvasChecksum"] == zlib.crc32(final["pixels"])
        assert (d / "recovered.png").read_bytes().startswith(b"\x89PNG")
        # Authentication failures are permanent and must not loop forever.
        rejected = subprocess.run([fixture["CLIENT"], "--url", f"ws://127.0.0.1:{port}", "--headless", "--frames", "1"],
                                  env=dict(env, TPT_ROOM_TOKEN="wrong"), capture_output=True, text=True, timeout=8)
        assert rejected.returncode != 0 and "retry in" not in rejected.stdout, (rejected.returncode, rejected.stdout, rejected.stderr)
        print("PASS: initial retry, live connection drop, two crashes, fresh baselines, view restoration, rotating backups, write failure/retry, corrupt fallback, directory lock, permanent auth rejection", flush=True)
    finally:
        if directory.exists(): directory.chmod(0o700)
        if editor: editor.close()
        if native.poll() is None:
            native.terminate()
            try: native.wait(timeout=5)
            except subprocess.TimeoutExpired: native.kill(); native.wait()
        client_log.close()
        proxy.close()
        if server: fixture["stop"](server, log)
    # No valid backup must fail closed rather than silently overwrite with an empty world.
    corrupt = d / "all-corrupt"; corrupt.mkdir()
    bad = corrupt / "autosave-00000000000000000001.cps"; bad.write_bytes(b"bad")
    result = subprocess.run([fixture["SERVER"], "--autosave-dir", str(corrupt), "--recover", "--run-for", "1"],
                            env=env, capture_output=True, text=True, timeout=8)
    assert result.returncode != 0 and "no valid autosave" in result.stderr
    assert bad.read_bytes() == b"bad" and len(list(corrupt.glob("*.cps"))) == 1
    offline = fixture["port"]()
    result = subprocess.run([fixture["CLIENT"], "--url", f"ws://127.0.0.1:{offline}", "--headless", "--frames", "1", "--no-reconnect"],
                            env=env, capture_output=True, text=True, timeout=8)
    assert result.returncode != 0 and "retry in" not in result.stdout
    print("PASS: all-corrupt backups preserved; --no-reconnect fails promptly", flush=True)


if __name__ == "__main__":
    if sys.platform != "linux" or os.geteuid() == 0:
        print("SKIP: storage-permission failure test requires an unprivileged Linux user")
        sys.exit(77)
    with tempfile.TemporaryDirectory(prefix="tpt-recovery-") as tmp:
        try:
            run(Path(tmp))
        finally:
            if len(sys.argv) > 3:
                shutil.copytree(tmp, Path(sys.argv[3]).resolve(), dirs_exist_ok=True)
