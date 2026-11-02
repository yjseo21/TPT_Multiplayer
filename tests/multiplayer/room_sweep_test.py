#!/usr/bin/env python3
"""Every element, wall and tool in one room, over the real protocol.

An admin draws a box of every element, every wall type and every simulation
tool (heat, cool, air, vacuum, gravity, wind...), sparks a wire, and changes the
room's settings while it runs (Newtonian gravity, edges, ambient heat, gravity
mode). Two local-simulation players run the result for a while: each must
verify every fingerprint without a resync, and both must apply every command.

Usage: room_sweep_test.py TPT_SERVER TPT_REMOTE
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


def tool(ident, action, **kw):
    return dict(dict(op="tool", tool=ident, action=action, brush="ellipse", dragging=False), **kw)


def commands(catalog):
    elements = [t["identifier"] for t in catalog if t["identifier"].startswith("DEFAULT_PT_") and t["identifier"] != "DEFAULT_PT_NONE"]
    walls = [t["identifier"] for t in catalog if t["identifier"].startswith("DEFAULT_WL_")]
    tools = [t["identifier"] for t in catalog if t["identifier"].startswith("DEFAULT_TOOL_")]
    out = [dict(op="settings", settings=dict(edgeMode=1))]  # solid edges keep the scene in the room
    # A box of every element on a grid, then every wall type in a row below.
    columns, w, h = 26, 22, 14
    for i, ident in enumerate(elements):
        x, y = 12 + (i % columns) * w, 12 + (i // columns) * h
        out.append(tool(ident, "box", x1=x, y1=y, x2=x + w - 6, y2=y + h - 5, radius=0))
    y = 12 + (len(elements) // columns + 1) * h + 8
    for i, ident in enumerate(walls):
        x = 12 + i * 30
        out.append(tool(ident, "box", x1=x, y1=y, x2=x + 22, y2=y + 10, radius=0))
    # A metal wire under the grid, sparked at one end.
    out.append(tool("DEFAULT_PT_METL", "line", x1=20, y1=y + 40, x2=590, y2=y + 40, radius=1))
    out.append(tool("DEFAULT_PT_SPRK", "point", x1=20, y1=y + 40, radius=0))
    # Every simulation tool, dragged across the elements.
    for i, ident in enumerate(tools):
        yy = 20 + i * 18
        out.append(tool(ident, "line", x1=30, y1=yy, x2=580, y2=yy + 30, radius=6, dragging=True, strength=500))
    # Room settings change while it all runs.
    out += [dict(op="settings", settings=dict(newtonianGravity=True, ambientHeat=True)),
            dict(op="reset", what="sparks"),
            dict(op="settings", settings=dict(gravityMode=2, airMode=2)),
            dict(op="console", cmd="bubble", x1=300, y1=330),
            dict(op="settings", settings=dict(edgeMode=2, waterEqualisation=True))]
    return out


def main():
    with tempfile.TemporaryDirectory(prefix="tpt-room-sweep-") as td:
        root = Path(td)
        port = free_port()
        env = dict(os.environ, TPT_ROOM_TOKEN="room-test", TPT_ADMIN_TOKEN="admin-test", SDL_VIDEODRIVER="dummy")
        catalog = json.loads(subprocess.run([CLIENT, "--list-tools"], env=env, capture_output=True, text=True, check=True).stdout)
        script = commands(catalog)
        (root / "edits.jsonl").write_text("".join(json.dumps(c) + "\n" for c in script))
        log = (root / "server.log").open("w+")
        server = subprocess.Popen([SERVER, "--port", str(port), "--seed", "2468", "--run-for", "60"], cwd=root, env=env,
                                  stdout=log, stderr=subprocess.STDOUT)
        procs = []
        try:
            deadline = time.monotonic() + 10
            while "Listening on" not in (root / "server.log").read_text():
                assert time.monotonic() < deadline and server.poll() is None, (root / "server.log").read_text()
                time.sleep(.1)

            def player(name, admin, *extra):
                penv = dict(env)
                if not admin:
                    penv.pop("TPT_ADMIN_TOKEN")
                p = subprocess.Popen([CLIENT, "--url", f"ws://127.0.0.1:{port}", "--name", name, "--headless",
                                      "--local-sim", "--run-for", "25", "--no-reconnect",
                                      "--metrics", str(root / f"{name}.jsonl"), *extra],
                                     cwd=root, env=penv, stdout=(root / f"{name}.out").open("w"), stderr=subprocess.STDOUT)
                procs.append(p)
                return p

            watcher = player("Watcher", False)
            time.sleep(1)
            editor = player("Editor", True, "--commands", str(root / "edits.jsonl"))
            for p, name in ((watcher, "Watcher"), (editor, "Editor")):
                p.wait(timeout=90)
                assert p.returncode == 0, (name, (root / f"{name}.out").read_text()[-3000:])
            last = {}
            for name in ("Watcher", "Editor"):
                last[name] = rows(root / f"{name}.jsonl")[-1]
                assert last[name]["resyncs"] == 0 and last[name]["connections"] == 1, (name, last[name])
                assert last[name]["verifiedHashes"] >= 15, (name, last[name]["verifiedHashes"])
            assert last["Watcher"]["appliedCommands"] == last["Editor"]["appliedCommands"] == len(script), \
                (last["Watcher"]["appliedCommands"], last["Editor"]["appliedCommands"], len(script))
        finally:
            for p in procs:
                if p.poll() is None:
                    p.kill(); p.wait()
            server.terminate(); server.wait(timeout=10); log.close()
        text = (root / "server.log").read_text()
        assert "diverged" not in text, text
        stats = [dict(re.findall(r"(\w+)=([\d.]+)", line)) for line in text.splitlines() if line.startswith("tick=")]
    print(f"PASS: {len(script)} commands (every element, wall and tool, settings changes) run in lockstep for two players "
          f"({last['Watcher']['verifiedHashes']} fingerprints each; room peak {max(float(s['tick_ms']) for s in stats):.1f} ms per tick)")


if __name__ == "__main__":
    main()
