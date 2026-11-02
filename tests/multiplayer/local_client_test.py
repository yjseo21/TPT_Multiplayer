"""Interactive-client local simulation tests (tpt-remote --local-sim).

Usage: python3 tests/multiplayer/local_client_test.py SERVER CLIENT

Scenario A (four players): two local-simulation clients (one editing, one
watching), one video client watching and one video client editing with admin
rights. The watchers use the heat view and the editors the normal view. Each
locally simulated, locally rendered canvas must be byte-identical to the
server's own rendering of the same paused world in the same view.

Scenario B (divergence recovery): a local client deliberately perturbs its own
physics state once. The next fingerprint must detect it, the server must name
the differing state groups, and the client must discard its world, rejoin from
a fresh exact checkpoint and keep verifying fingerprints afterwards.
"""
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time

SERVER, CLIENT = (str(Path(p).resolve()) for p in sys.argv[1:3])


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def start_server(root, port, *extra):
    env = dict(os.environ, TPT_ROOM_TOKEN="room-test", TPT_ADMIN_TOKEN="admin-test")
    log = (root / "server.log").open("w+")
    server = subprocess.Popen([SERVER, "--port", str(port), "--seed", "4242", *extra],
                              cwd=root, env=env, stdout=log, stderr=subprocess.STDOUT)
    deadline = time.monotonic()+10
    while time.monotonic() < deadline:
        log.seek(0)
        if "Listening on" in log.read():
            return server, log
        if server.poll() is not None:
            break
        time.sleep(.1)
    raise AssertionError("server did not start: " + (root / "server.log").read_text())


def client(root, port, name, *args, admin=False):
    env = dict(os.environ, TPT_ROOM_TOKEN="room-test", SDL_VIDEODRIVER="dummy")
    env.pop("TPT_ADMIN_TOKEN", None)
    if admin:
        env["TPT_ADMIN_TOKEN"] = "admin-test"
    out = (root / f"{name}.log").open("w")
    return subprocess.Popen([CLIENT, "--url", f"ws://127.0.0.1:{port}", "--name", name, "--headless", *args],
                            cwd=root, env=env, stdout=out, stderr=subprocess.STDOUT), root / f"{name}.log"


def finish(proc, log, timeout=40):
    try:
        proc.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.kill(); proc.wait()
        raise AssertionError("client timed out: " + log.read_text())
    assert proc.returncode == 0, log.read_text()
    return log.read_text()


def rows(path):
    return [json.loads(line) for line in path.read_text().splitlines() if line.strip()]


def write_script(path, commands):
    path.write_text("".join(json.dumps(c) + "\n" for c in commands))


def scenario_four_players():
    with tempfile.TemporaryDirectory(prefix="tpt-local-client-") as td:
        root = Path(td)
        port = free_port()
        # Batched seals here; the divergence scenario covers the unbatched default.
        server, log = start_server(root, port, "--seal-batch-ms", "50")
        procs = []
        try:
            write_script(root / "local-edits.jsonl", [
                dict(op="brush", x1=150, y1=80, x2=260, y2=80, radius=5, element=2),     # water
                dict(op="tool", tool="DEFAULT_PT_STNE", action="box", brush="ellipse", radius=0,
                     x1=120, y1=300, x2=420, y2=310, dragging=False),                    # extended tool path
            ])
            write_script(root / "video-edits.jsonl", [
                dict(op="brush", x1=300, y1=120, x2=400, y2=140, radius=4, element=1),   # dust
                dict(op="brush", x1=330, y1=60, x2=360, y2=60, radius=3, element=3),     # oil (fire glow uses renderer-local randomness)
                dict(op="wait", seconds=1.5),  # let the room run a moment (particles still on screen) before the pause
                dict(op="pause", paused=True),
            ])
            procs.append(client(root, port, "LocalEditor", "--local-sim", "--run-for", "14", "--no-reconnect",
                                "--commands", str(root / "local-edits.jsonl"), "--output", str(root / "local-editor.png"),
                                "--metrics", str(root / "local-editor.jsonl")))
            procs.append(client(root, port, "LocalWatcher", "--local-sim", "--view", "1", "--run-for", "14", "--no-reconnect",
                                "--output", str(root / "local-watcher.png"), "--metrics", str(root / "local-watcher.jsonl"),
                                "--output-hud", str(root / "local-watcher-hud.png")))
            procs.append(client(root, port, "VideoWatcher", "--view", "1", "--run-for", "14", "--no-reconnect",
                                "--output", str(root / "video-watcher.png")))
            time.sleep(1.5)  # All four join before the admin script starts.
            procs.append(client(root, port, "VideoEditor", "--run-for", "12", "--no-reconnect",
                                "--commands", str(root / "video-edits.jsonl"), "--output", str(root / "video-editor.png"),
                                admin=True))
            # The admin pauses after its edits; the last local edit may follow while paused.
            outputs = [finish(*p) for p in procs]
            images = {name: (root / f"{name}.png").read_bytes()
                      for name in ("local-editor", "local-watcher", "video-watcher", "video-editor")}
            # Watchers use the heat view; editors the normal view. Local rendering of
            # the locally simulated world must equal the server's rendering of each.
            assert images["local-watcher"] == images["video-watcher"], "local heat view differs from the server's"
            assert images["local-editor"] == images["video-editor"], "local normal view differs from the server's"
            assert images["local-editor"] != images["local-watcher"], "views were not distinct"
            for name in ("local-editor", "local-watcher"):
                final = rows(root / f"{name}.jsonl")[-1]
                assert final["localSimulation"] and final["connected"] and final["status"] == "PAUSED", final
                assert final["resyncs"] == 0 and final["verifiedHashes"] >= 2, final
                assert final["view"] == (1 if name == "local-watcher" else 0), final
                assert final["appliedCommands"] == 5, final
                assert final["uncertainEdits"] == 0 and final["pendingEdits"] == 0, final
            # Command-only traffic: no canvas stream after the initial checkpoint.
            watcher = rows(root / "local-watcher.jsonl")
            assert max(r["receiveMbps"] for r in watcher[2:]) < 0.5, [r["receiveMbps"] for r in watcher]
            # Seal batching: far fewer seal messages than physics ticks while running.
            running = [r for r in watcher if r.get("status") in ("LOCAL SIM", "SERVER SLOW", "CATCHING UP") and "localTick" in r]
            # Seals per second, not per tick: a slow server (about 20 ticks/s on some CI
            # machines) leaves nothing to merge. 50 ms batches allow about 20/s plus
            # fingerprint seals; unbatched would be about one per tick (60/s).
            seconds = running[-1]["elapsedSeconds"] - running[0]["elapsedSeconds"]
            seals = running[-1]["seals"] - running[0]["seals"]
            assert seconds >= 1 and seals / seconds < 30, (seconds, seals, [(r.get("status"), r.get("localTick"), r.get("seals")) for r in watcher])
            # Own edits reach the local world quickly: loopback, so nearly all of it is overhead.
            editor = rows(root / "local-editor.jsonl")
            latency = sorted(ms for r in editor for ms in r.get("editLatencyMs", []))
            assert len(latency) >= 2 and editor[-1]["editLatencySamples"] >= 2, editor[-1]
            # The (lower) median; 50 ms seal batches add up to one batch, and CI machines are slow.
            assert latency[(len(latency) - 1) // 2] < 150, latency
            assert "resyncs=0" in outputs[1]
            text = (root / "server.log").read_text()
            assert "diverged" not in text, text
        finally:
            for proc, _ in procs:
                if proc.poll() is None:
                    proc.kill(); proc.wait()
            server.terminate(); server.wait(timeout=10); log.close()
    print("PASS: 2 local-simulation + 2 video players edit concurrently; local normal/heat canvases equal the server's")


def scenario_divergence_recovery():
    with tempfile.TemporaryDirectory(prefix="tpt-local-desync-") as td:
        root = Path(td)
        port = free_port()
        server, log = start_server(root, port)
        procs = []
        try:
            write_script(root / "edits.jsonl", [dict(op="brush", x1=200, y1=100, x2=300, y2=100, radius=6, element=2)])
            procs.append(client(root, port, "Corrupted", "--local-sim", "--run-for", "11",
                                "--debug-corrupt-at-tick", "30", "--metrics", str(root / "corrupted.jsonl")))
            procs.append(client(root, port, "Honest", "--local-sim", "--run-for", "11", "--no-reconnect",
                                "--metrics", str(root / "honest.jsonl")))
            time.sleep(1)
            procs.append(client(root, port, "Editor", "--run-for", "4", "--no-reconnect",
                                "--commands", str(root / "edits.jsonl")))
            outputs = [finish(*p) for p in procs]
            corrupted = outputs[0]
            assert "DEBUG: perturbed local pressure" in corrupted, corrupted
            assert "Local simulation diverged at tick" in corrupted, corrupted
            assert "differing state groups: " in corrupted and "air-fields" in corrupted, corrupted
            assert corrupted.count("Local simulation synchronized") == 2, corrupted
            server_text = (root / "server.log").read_text()
            assert "diverged at tick" in server_text and "air-fields" in server_text, server_text
            samples = rows(root / "corrupted.jsonl")
            final = samples[-1]
            assert final["resyncs"] == 1 and final["connections"] == 2 and final["connected"], final
            assert any(r["status"] in ("RECONNECTING", "RESYNCHRONIZING", "SYNCHRONIZING") for r in samples), samples
            # Fingerprints keep verifying after the fresh checkpoint.
            # Any synchronized, running state counts (slow CI machines report SERVER SLOW).
            resynced = [r for r in samples if r["resyncs"] == 1 and r["status"] in ("LOCAL SIM", "SERVER SLOW", "CATCHING UP")]
            assert resynced and resynced[-1]["verifiedHashes"] > resynced[0]["verifiedHashes"], samples
            honest = rows(root / "honest.jsonl")[-1]
            # One fingerprint per 60 ticks; an overloaded CI machine may run the room slower.
            assert honest["resyncs"] == 0 and honest["connections"] == 1, honest
            assert honest["verifiedHashes"] >= max(2, honest["localTick"] // 60 - 1), honest
        finally:
            for proc, _ in procs:
                if proc.poll() is None:
                    proc.kill(); proc.wait()
            server.terminate(); server.wait(timeout=10); log.close()
    print("PASS: injected local divergence detected, diagnosed by state group, and recovered from a fresh checkpoint")


if __name__ == "__main__":
    scenario_four_players()
    scenario_divergence_recovery()
