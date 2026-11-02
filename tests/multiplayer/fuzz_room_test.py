#!/usr/bin/env python3
"""Randomized room: two editors, every kind of command, three copies in lockstep.

A seeded generator writes long streams of valid but random commands:
- every element, wall and tool, with every action, brush shape, radius,
  strength and replace mode, decoration (with colours) and the property tool;
- room settings, resets and console commands (admin);
- signs, stickman placing and steering;
- pause, frame steps and clear (admin).

An admin and a regular player send their streams at the same time, so their
edits interleave; a watcher only watches. All three run the room locally:
each must verify every fingerprint without a resync, and all must apply
every command.

Usage: fuzz_room_test.py TPT_SERVER TPT_REMOTE [SEED [SECONDS]]
"""
import json, os, random, socket, subprocess, sys, tempfile, time
from pathlib import Path

SERVER, CLIENT = (str(Path(p).resolve()) for p in sys.argv[1:3])
SEED = int(sys.argv[3]) if len(sys.argv) > 3 else 20261102
SECONDS = int(sys.argv[4]) if len(sys.argv) > 4 else 24
W, H, CELL = 612, 384, 4


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def rows(path):
    return [json.loads(line) for line in path.read_text().splitlines() if line.strip()]


class Generator:
    def __init__(self, seed, catalog, numbers, admin, count):
        self.rng = random.Random(seed)
        self.catalog = catalog
        self.numbers = numbers  # enabled element numbers, for console commands
        self.admin = admin
        self.count = count
        self.paused = False
        self.elements = [t for t in catalog if t.startswith("DEFAULT_PT_")]
        self.walls = [t for t in catalog if t.startswith("DEFAULT_WL_")]
        self.tools = [t for t in catalog if t.startswith("DEFAULT_TOOL_")]
        self.decorations = ["DEFAULT_DECOR_SET", "DEFAULT_DECOR_CLR", "DEFAULT_DECOR_ADD", "DEFAULT_DECOR_SUB",
                            "DEFAULT_DECOR_MUL", "DEFAULT_DECOR_DIV", "DEFAULT_DECOR_SMDG"]

    def point(self):
        r = self.rng
        return r.randint(CELL, W - CELL - 1), r.randint(CELL, H - CELL - 1)

    def tool(self):
        r = self.rng
        kind = r.random()
        ident = (r.choice(self.elements) if kind < .6 else r.choice(self.walls) if kind < .72 else
                 r.choice(self.tools) if kind < .85 else r.choice(self.decorations) if kind < .93 else "DEFAULT_UI_PROPERTY")
        action = r.choices(["point", "line", "box", "fill", "click", "drag"], [30, 40, 15, 3, 6, 6])[0]
        if ident.startswith("DEFAULT_DECOR_") and action == "fill":
            action = "line"  # decoration fill is refused in rooms
        (x1, y1), (x2, y2) = self.point(), self.point()
        if action in ("line", "drag"):
            x2, y2 = max(CELL, min(W - CELL - 1, x1 + r.randint(-80, 80))), max(CELL, min(H - CELL - 1, y1 + r.randint(-80, 80)))
        if action == "box":
            x2, y2 = max(CELL, min(W - CELL - 1, x1 + r.randint(-60, 60))), max(CELL, min(H - CELL - 1, y1 + r.randint(-60, 60)))
        c = dict(op="tool", tool=ident, action=action, x1=x1, y1=y1, x2=x2, y2=y2,
                 brush=r.choice(["ellipse", "ellipse-perfect", "rectangle", "triangle"]),
                 radius=r.choice([0, 1, 2, 3, 5, 8, 12, 20]), radiusY=r.choice([0, 1, 2, 4, 8, 15]),
                 strength=r.choice([1, 50, 100, 300, 1000]), dragging=r.random() < .5,
                 shift=r.random() < .1, ctrl=r.random() < .1, alt=r.random() < .1,
                 flags=r.choice([0, 0, 0, 1, 2, 3]), replace=r.randint(0, 150))
        if ident.startswith("DEFAULT_DECOR_"):
            c["colour"] = r.getrandbits(32)
        if ident == "DEFAULT_UI_PROPERTY":
            prop = r.choice([("life", 1), ("tmp", 9), ("tmp2", 10), ("ctype", 2), ("temp", 7)])
            c["property"] = prop[1]
            c["value"] = r.uniform(0, 5000) if prop[0] == "temp" else r.randint(-50, 300) if prop[0] != "ctype" else r.randint(0, 150)
        return c

    def admin_command(self):
        r = self.rng
        kind = r.random()
        if kind < .3:
            keys = dict(gravityMode=lambda: r.randint(0, 3), airMode=lambda: r.randint(0, 4), edgeMode=lambda: r.randint(0, 2),
                        convectionMode=lambda: r.randint(0, 2), heat=lambda: r.random() < .5, ambientHeat=lambda: r.random() < .5,
                        waterEqualisation=lambda: r.random() < .5, newtonianGravity=lambda: r.random() < .5,
                        prettyPowder=lambda: r.random() < .5, ambientAirTemp=lambda: r.uniform(0, 9999),
                        edgePressure=lambda: r.uniform(-256, 256), edgeVelocityX=lambda: r.uniform(-50, 50),
                        edgeVelocityY=lambda: r.uniform(-50, 50), vorticityCoeff=lambda: r.random(),
                        customGravityX=lambda: r.uniform(-2, 2), customGravityY=lambda: r.uniform(-2, 2))
            chosen = r.sample(sorted(keys), r.randint(1, 4))
            return dict(op="settings", settings={k: keys[k]() for k in chosen})
        if kind < .42:
            return dict(op="reset", what=r.choice(["air", "pressure", "velocity", "sparks", "ambientHeat", "temperature", "invertAir"]))
        if kind < .7:
            x, y = self.point()
            cmd = r.choice(["bubble", "create", "delete", "set"])
            if cmd == "create":
                return dict(op="console", cmd=cmd, element=r.choice(self.numbers), x1=x, y1=y)
            if cmd == "set":
                prop, value = r.choice([(1, r.randint(0, 500)), (9, r.randint(-100, 100)), (7, r.uniform(0, 9000))])
                sel = r.choice(["all", "point", "type"])
                c = dict(op="console", cmd="set", property=prop, value=value, selector=sel)
                if sel == "point":
                    c.update(x1=x, y1=y)
                if sel == "type":
                    c["element"] = r.choice(self.numbers)
                return c
            return dict(op="console", cmd=cmd, x1=x, y1=y)
        if kind < .82:
            self.paused = not self.paused
            return dict(op="pause", paused=self.paused)
        if kind < .97:
            if not self.paused:
                self.paused = True
                return dict(op="pause", paused=True)
            return dict(op="step")
        return dict(op="clear")

    def player_command(self):
        r = self.rng
        kind = r.random()
        x, y = self.point()
        if kind < .4:
            return dict(op="sign", index=-1, text=r.choice(["hi", "{b|go}", "{t}", "room", "x" * 45]), x1=x, y1=y,
                        justification=r.randint(0, 3))
        if kind < .55:
            return dict(op="sign", index=r.randint(0, 15), x2=x, y2=y, remove=True)
        if kind < .7:
            return dict(op="stickman_spawn", x1=x, y1=y)
        return dict(op="stickman", comm=r.randint(0, 15), pcomm=r.randint(0, 15))

    def commands(self):
        out = []
        for _ in range(self.count):
            kind = self.rng.random()
            if self.admin and kind < .18:
                out.append(self.admin_command())
            elif kind < .3:
                out.append(self.player_command())
            else:
                out.append(self.tool())
        if self.admin and self.paused:
            out.append(dict(op="pause", paused=False))
        return out


def main():
    rng = random.Random(SEED)
    with tempfile.TemporaryDirectory(prefix="tpt-fuzz-room-") as td:
        root = Path(td)
        port = free_port()
        env = dict(os.environ, TPT_ROOM_TOKEN="room-test", TPT_ADMIN_TOKEN="admin-test", SDL_VIDEODRIVER="dummy")
        tools = json.loads(subprocess.run([CLIENT, "--list-tools"], env=env, capture_output=True, text=True, check=True).stdout)
        catalog = [t["identifier"] for t in tools]
        numbers = [t["element"] for t in tools if t.get("element", 0) > 0]
        count = max(60, SECONDS * 9)
        scripts = {"Admin": Generator(rng.getrandbits(32), catalog, numbers, True, count).commands(),
                   "Player": Generator(rng.getrandbits(32), catalog, numbers, False, count).commands()}
        for name, script in scripts.items():
            (root / f"{name}.jsonl").write_text("".join(json.dumps(c) + "\n" for c in script))
        log = (root / "server.log").open("w+")
        server = subprocess.Popen([SERVER, "--port", str(port), "--seed", str(SEED % 2**31), "--run-for", str(SECONDS + 40)],
                                  cwd=root, env=env, stdout=log, stderr=subprocess.STDOUT)
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
                                      "--local-sim", "--run-for", str(SECONDS), "--no-reconnect",
                                      "--metrics", str(root / f"{name}.jsonl.metrics"), *extra],
                                     cwd=root, env=penv, stdout=(root / f"{name}.out").open("w"), stderr=subprocess.STDOUT)
                procs.append(p)
                return p

            watcher = player("Watcher", False)
            time.sleep(1)
            admin = player("Admin", True, "--commands", str(root / "Admin.jsonl"))
            other = player("Player", False, "--commands", str(root / "Player.jsonl"))
            for p, name in ((watcher, "Watcher"), (admin, "Admin"), (other, "Player")):
                p.wait(timeout=SECONDS + 60)
                assert p.returncode == 0, (SEED, name, (root / f"{name}.out").read_text()[-3000:])
            last = {name: rows(root / f"{name}.jsonl.metrics")[-1] for name in ("Watcher", "Admin", "Player")}
            for name, row in last.items():
                assert row["resyncs"] == 0 and row["connections"] == 1, (SEED, name, row)
                # The admin pauses a lot, and a paused room makes few ticks: some fingerprints suffice.
                assert row["verifiedHashes"] >= 3, (SEED, name, row["verifiedHashes"])
            total = len(scripts["Admin"]) + len(scripts["Player"])
            applied = {name: row["appliedCommands"] for name, row in last.items()}
            assert len(set(applied.values())) == 1 and applied["Watcher"] == total, (SEED, applied, total)
        finally:
            for p in procs:
                if p.poll() is None:
                    p.kill(); p.wait()
            server.terminate(); server.wait(timeout=10); log.close()
        text = (root / "server.log").read_text()
        assert "diverged" not in text, (SEED, text[-2000:])
    print(f"PASS: seed {SEED}: {total} random commands from two editors, three copies in lockstep "
          f"({last['Watcher']['verifiedHashes']} fingerprints each)")


if __name__ == "__main__":
    main()
