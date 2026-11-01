"""Four-player tests with real kernel latency, jitter, loss and rate limiting.

Runs in a disposable unprivileged user/network namespace. Never changes host
networking. Linux with iproute2 and enabled user namespaces is required (skip 77
otherwise). Optional third positional argument retains logs and HUD screenshots.
"""
import json
import os
from pathlib import Path
import queue
import runpy
import shutil
import socket
import statistics
import subprocess
import sys
import tempfile
import threading
import time
import zlib


def command(*args):
    return subprocess.run(args, check=True, text=True, capture_output=True).stdout


def entry():
    if "--isolated" not in sys.argv:
        if sys.platform != "linux" or any(not shutil.which(t) for t in ("unshare", "ip", "tc")):
            print("SKIP: network impairment needs Linux, unshare and iproute2")
            return 77
        parent = os.readlink("/proc/self/ns/net")
        # Probe permission before launching the actual test; failures of the test itself
        # are never reported as skips.
        # A namespace can be creatable even when the kernel has no sch_netem
        # qdisc available. Probe that too, inside a disposable namespace, so
        # missing host kernel support is a clear SKIP rather than a false test
        # failure (and never change host interface configuration).
        probe = subprocess.run(["unshare", "--user", "--map-root-user", "--net", "sh", "-c",
                                "ip link set lo up && tc qdisc add dev lo root netem delay 1ms && tc qdisc del dev lo root"],
                               capture_output=True)
        if probe.returncode:
            print("SKIP: private user/network namespace or netem support unavailable:", probe.stderr.decode().strip())
            return 77
        return subprocess.call(["unshare", "--user", "--map-root-user", "--net", sys.executable,
                                str(Path(__file__).resolve()), *sys.argv[1:], "--isolated", parent])
    split = sys.argv.index("--isolated")
    parent = sys.argv[split+1]
    assert os.readlink("/proc/self/ns/net") != parent, "refusing to modify the parent network namespace"
    assert os.geteuid() == 0, "expected mapped namespace root"
    output = Path(sys.argv[3]).resolve() if split > 3 else None
    command("ip", "link", "set", "lo", "up")
    fixture = runpy.run_path(str(Path(__file__).with_name("network_test.py")), run_name="fixtures")
    if output:
        output.mkdir(parents=True, exist_ok=True)
        run(fixture, output)
    else:
        with tempfile.TemporaryDirectory(prefix="tpt-impaired-") as tmp:
            run(fixture, Path(tmp))
    return 0


class Editor:
    def __init__(self, fixture, port, admin=False):
        self.ws = fixture["WS"](port)
        self.ws.s.settimeout(20)
        self.ws.hello(admin=admin)
        self.sequence, self.latest, self.frames = 0, None, 0
        self.controls = queue.Queue()
        self.condition = threading.Condition()
        self.failure, self.closed = None, False
        self.reader = threading.Thread(target=self.read, daemon=True)
        self.reader.start()

    def read(self):
        try:
            while not self.closed:
                kind, value = self.ws.receive()
                if kind == "close":
                    raise RuntimeError("unexpected disconnect: " + value)
                with self.condition:
                    if kind == "frame":
                        self.latest = value
                        self.frames += 1
                    else:
                        self.controls.put(value)
                    self.condition.notify_all()
        except Exception as exc:
            if not self.closed:
                with self.condition:
                    self.failure = exc
                    self.condition.notify_all()

    def edit(self, **fields):
        self.sequence += 1
        self.ws.send(dict(seq=self.sequence, **fields))
        deadline = time.monotonic()+12
        while time.monotonic() < deadline:
            if self.failure:
                raise self.failure
            try:
                value = self.controls.get(timeout=.1)
            except queue.Empty:
                continue
            if value.get("seq") == self.sequence:
                assert value["type"] == "ack", value
                return value["revision"]
        raise AssertionError("command acknowledgement did not arrive")

    def frame(self, revision, view=0):
        with self.condition:
            assert self.condition.wait_for(lambda: self.failure or
                (self.latest and self.latest["revision"] >= revision and self.latest["view"] == view), timeout=15), "canvas did not catch up"
            if self.failure:
                raise self.failure
            return self.latest

    def close(self):
        self.closed = True
        try:
            self.ws.s.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.ws.close()
        self.reader.join(timeout=3)
        assert not self.reader.is_alive(), "reader did not stop"


def netem(delay, jitter, loss, rate, operation="replace"):
    command("tc", "qdisc", operation, "dev", "lo", "root", "netem", "limit", "200",
            "delay", f"{delay}ms", f"{jitter}ms", "distribution", "normal",
            "loss", f"{loss}%", "rate", rate)


def run(fixture, root):
    report = []
    for rtt, jitter, loss, rate in [(50, 3, 1, "8mbit"), (100, 5, 1, "4mbit"), (200, 10, 3, "2mbit")]:
        folder = root / f"rtt-{rtt}"
        folder.mkdir(exist_ok=True)
        netem(rtt/2, jitter, loss, rate)
        server, port, log, env = fixture["start"](folder)
        editors, natives, streams = [], [], []
        try:
            editors.append(Editor(fixture, port, admin=True))
            editors.append(Editor(fixture, port))
            # Two independent protocol clients and two actual native clients share the room.
            for n in range(2):
                output = open(folder / f"client-{n}.log", "w")
                streams.append(output)
                args = [fixture["CLIENT"], "--url", f"ws://127.0.0.1:{port}", "--run-for", str(22+rtt//25),
                        "--metrics", str(folder / f"metrics-{n}.jsonl"),
                        "--output-hud", str(folder / f"hud-{n}.png")]
                if n:
                    args.append("--headless")
                natives.append(subprocess.Popen(args, env=dict(env, SDL_VIDEODRIVER="dummy"),
                                                stdout=output, stderr=subprocess.STDOUT))
            admin, other = editors
            # Seed two material patches via separately sequenced edits, then run physics.
            for n in range(8):
                player = editors[n % 2]
                player.edit(op="brush", x1=60+250*(n%2), y1=45+18*(n//2),
                            x2=210+250*(n%2), y2=45+18*(n//2), radius=7, element=2 if n%2 else 1)
            revision = admin.edit(op="pause", paused=False)
            initial = admin.frame(revision)["tick"]
            other.ws.send(dict(type="view", view=1))
            other.frame(revision, view=1)
            # Each editor continues receiving while new actions traverse the impaired path.
            for n in range(6):
                editors[n%2].edit(op="brush", x1=150+n*20, y1=30, x2=160+n*20, y2=35, radius=3, element=1)
                time.sleep(.15)
            if rtt == 200:
                # Change capacity mid-session without resetting the TCP connections.
                netem(rtt/2, jitter, loss, "512kbit", "change")
                time.sleep(2)
                netem(rtt/2, jitter, loss, rate, "change")
            time.sleep(1)
            revision = admin.edit(op="pause", paused=True)
            final = admin.frame(revision)
            assert final["tick"] > initial+30, "physics stalled under impairment"
            other.ws.send(dict(type="view", view=0))
            assert other.frame(revision)["pixels"] == final["pixels"], "editors diverged after impaired delivery"
            other.ws.send(dict(type="resync"))
            # Native clients must catch up to exactly the same paused authoritative canvas.
            for process in natives:
                process.wait(timeout=35)
                assert process.returncode == 0, f"native client failed; see {folder}"
            checksum = zlib.crc32(final["pixels"])
            measurements = []
            for n in range(2):
                rows = [json.loads(line) for line in (folder / f"metrics-{n}.jsonl").read_text().splitlines()]
                assert rows[-1]["revision"] == revision and rows[-1]["canvasChecksum"] == checksum, "native canvas diverged"
                running = [v for v in rows if v.get("telemetryFresh") and v.get("frame") and not v["paused"] and v["elapsedSeconds"] > 2]
                assert len(running) >= 2, "not enough live telemetry samples"
                assert all(0 <= v["inFlight"] <= v["streamWindow"] <= 2 for v in running)
                assert all(0 < v["pacingHz"] <= v["updateHz"] and v["shareKbps"] <= v["uploadKbps"] for v in running)
                assert statistics.median(v["pingMs"] for v in running) >= rtt*.45, "latency was not measured"
                assert any(v["simHz"] > 30 for v in running), "simulation progress not reported"
                assert any(v["receiveMbps"] > 0 and v["updatesHz"] > 0 for v in running)
                assert all(v["frameAgeMs"] >= 0 for v in running)
                assert any(v["status"] == "PAUSED" for v in rows), "paused state not visible"
                assert (folder / f"hud-{n}.png").read_bytes().startswith(b"\x89PNG")
                measurements.extend(running)
            qdisc = json.loads(command("tc", "-s", "-j", "qdisc", "show", "dev", "lo"))
            drops = sum(v.get("drops", 0) for v in qdisc)
            assert drops > 0, "test did not exercise actual packet loss"
            item = dict(targetRttMs=rtt, jitterPerLegMs=jitter, lossPercent=loss, sharedRate=rate, droppedPackets=drops,
                        medianPingMs=round(statistics.median(v["pingMs"] for v in measurements), 1),
                        medianFrameAgeMs=round(statistics.median(v["frameAgeMs"] for v in measurements), 1),
                        medianUpdatesHz=round(statistics.median(v["updatesHz"] for v in measurements), 1),
                        tick=final["tick"], revision=revision, players=4)
            report.append(item)
            print("PASS:", json.dumps(item), flush=True)
        finally:
            # Restore the namespace's loopback before cleanup so TCP close is prompt.
            subprocess.run(["tc", "qdisc", "del", "dev", "lo", "root"], capture_output=True)
            for process in natives:
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill(); process.wait()
            for editor in editors:
                editor.close()
            for stream in streams:
                stream.close()
            fixture["stop"](server, log)
    (root / "report.json").write_text(json.dumps(report, indent=2)+"\n")


if __name__ == "__main__":
    sys.exit(entry())
