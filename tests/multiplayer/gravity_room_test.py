#!/usr/bin/env python3
"""Newtonian gravity in a room: every player's copy stays identical.

An admin turns Newtonian gravity on and fills the room with things that both
make and feel gravity (singularity, dust, gas, fire). Two local-simulation
players run it for several seconds; each verifies every fingerprint the server
sends (one per 60 ticks), so any difference in gravity forces would show up as
a divergence.

Usage: gravity_room_test.py TPT_SERVER TPT_REMOTE
"""
import json, os, re, socket, subprocess, sys, tempfile, time
from pathlib import Path

SERVER, CLIENT = (str(Path(p).resolve()) for p in sys.argv[1:3])


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def rows(path):
    return [json.loads(line) for line in path.read_text().splitlines() if line.strip()]


def main():
    with tempfile.TemporaryDirectory(prefix="tpt-gravity-room-") as td:
        root = Path(td)
        port = free_port()
        env = dict(os.environ, TPT_ROOM_TOKEN="room-test", TPT_ADMIN_TOKEN="admin-test")
        log = (root / "server.log").open("w+")
        server = subprocess.Popen([SERVER, "--port", str(port), "--seed", "4321", "--run-for", "30"], cwd=root, env=env,
                                  stdout=log, stderr=subprocess.STDOUT)
        procs = []
        try:
            deadline = time.monotonic() + 10
            while "Listening on" not in (root / "server.log").read_text():
                assert time.monotonic() < deadline and server.poll() is None, (root / "server.log").read_text()
                time.sleep(.1)
            tool = lambda ident, action, **kw: dict(op="tool", tool=ident, action=action, brush="ellipse", dragging=False, **kw)
            commands = [
                dict(op="settings", settings=dict(newtonianGravity=True, edgeMode=1)),
                tool("DEFAULT_PT_SING", "point", x1=306, y1=190, radius=4),
                tool("DEFAULT_PT_DUST", "box", x1=60, y1=60, x2=200, y2=110, radius=0),
                tool("DEFAULT_PT_GAS", "box", x1=420, y1=250, x2=540, y2=320, radius=0),
                tool("DEFAULT_PT_STNE", "line", x1=100, y1=300, x2=250, y2=340, radius=3),
                tool("DEFAULT_PT_FIRE", "point", x1=480, y1=80, radius=10),
                tool("DEFAULT_WL_GRVTY", "box", x1=560, y1=20, x2=600, y2=60, radius=0),
            ]
            (root / "edits.jsonl").write_text("".join(json.dumps(c) + "\n" for c in commands))

            def player(name, admin, *extra):
                penv = dict(env)
                if not admin:
                    penv.pop("TPT_ADMIN_TOKEN")
                p = subprocess.Popen([CLIENT, "--url", f"ws://127.0.0.1:{port}", "--name", name, "--headless",
                                      "--local-sim", "--run-for", "14", "--no-reconnect",
                                      "--metrics", str(root / f"{name}.jsonl"), *extra],
                                     cwd=root, env=penv, stdout=(root / f"{name}.out").open("w"), stderr=subprocess.STDOUT)
                procs.append(p)
                return p

            watcher = player("Watcher", False)
            time.sleep(1)
            editor = player("Editor", True, "--commands", str(root / "edits.jsonl"))
            for p, name in ((watcher, "Watcher"), (editor, "Editor")):
                p.wait(timeout=60)
                assert p.returncode == 0, (name, (root / f"{name}.out").read_text()[-3000:])
            for name in ("Watcher", "Editor"):
                last = rows(root / f"{name}.jsonl")[-1]
                assert last["resyncs"] == 0 and last["connections"] == 1, (name, last)
                assert last["appliedCommands"] == len(commands), (name, last["appliedCommands"])
                assert last["verifiedHashes"] >= 8, (name, last["verifiedHashes"])
        finally:
            for p in procs:
                if p.poll() is None:
                    p.kill(); p.wait()
            server.terminate(); server.wait(timeout=10); log.close()
        text = (root / "server.log").read_text()
        assert "diverged" not in text, text
        stats = [dict(re.findall(r"(\w+)=([\d.]+)", line)) for line in text.splitlines() if line.startswith("tick=")]
    print(f"PASS: two players run a room with Newtonian gravity in lockstep ({last['verifiedHashes']} fingerprints each, "
          f"room at {float(stats[-1]['sim_hz']):.0f} ticks/s, {float(stats[-1]['tick_ms']):.1f} ms per tick)")


if __name__ == "__main__":
    main()
