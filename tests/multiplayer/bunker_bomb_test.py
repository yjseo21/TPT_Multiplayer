#!/usr/bin/env python3
"""Two real games: A pastes a bunker, B pastes a snow bomb onto it.

Each player builds on their own canvas (paused, as a player would while
building), copies it with the copy tool, switches to the room and pastes it:

  A: a titanium bunker on a diamond floor, insulation inside, stone and a
     water pool;
  B: a snow bomb: a C-5 charge (a cold explosive) packed in snow, with liquid
     nitrogen above it as the cold trigger, pasted on the bunker's roof.

A watcher shares the room. The bomb goes off in the room; all three copies
must agree through the blast (every fingerprint verified, no resync), and the
charge must actually be spent while the bunker still stands.

Usage: bunker_bomb_test.py TPT_SERVER TPT_REMOTE POWDER [SCREENSHOT_DIR]
"""
import json, os, shutil, socket, subprocess, sys, tempfile, time
from pathlib import Path

SERVER, CLIENT, GAME = (str(Path(p).resolve()) for p in sys.argv[1:4])
SHOTS = Path(sys.argv[4]).resolve() if len(sys.argv) > 4 else None
COUNTED = ["TTAN", "DMND", "C5", "SNOW", "LN2", "WATR"]


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def rows(path):
    return [json.loads(line) for line in path.read_text().splitlines() if line.strip()]


def tool(ident, action, x1, y1, x2=None, y2=None, at=0.0):
    return dict(at=at, do=action, x1=x1, y1=y1, x2=x2 if x2 is not None else x1, y2=y2 if y2 is not None else y1, held=False)


def bunker_steps(shots):
    s = [dict(at=1.4, do="nointro"),
         dict(at=1.5, do="toggle", synced=True),           # own canvas
         dict(at=1.5, do="pause", paused=True),            # build while paused, as players do
         dict(at=1.51, do="brush", id=1, rx=2, ry=2),       # square brush
         dict(at=1.52, do="tool", tool="DEFAULT_PT_TTAN")]
    for x1, y1, x2, y2 in ((200, 200, 400, 200), (200, 200, 200, 330), (400, 200, 400, 330)):
        s.append(dict(at=1.53, do="line", x1=x1, y1=y1, x2=x2, y2=y2))
    s += [dict(at=1.54, do="tool", tool="DEFAULT_PT_DMND"),
          dict(at=1.55, do="line", x1=196, y1=334, x2=404, y2=334),
          dict(at=1.56, do="brush", id=1, rx=1, ry=1),
          dict(at=1.56, do="tool", tool="DEFAULT_PT_INSL"),
          dict(at=1.57, do="line", x1=206, y1=206, x2=394, y2=206),
          dict(at=1.58, do="tool", tool="DEFAULT_PT_STNE"),
          dict(at=1.59, do="rect", x1=210, y1=280, x2=390, y2=290),
          dict(at=1.6, do="tool", tool="DEFAULT_PT_WATR"),
          dict(at=1.61, do="rect", x1=210, y1=300, x2=390, y2=326),
          dict(at=1.7, do="copy", x1=190, y1=190, x2=410, y2=340),
          dict(at=1.8, do="toggle"),                         # back to the room
          dict(at=1.9, do="place", x=190, y=190),            # paste the bunker
          dict(at=3.2, do="mark", name="bunker", count=COUNTED)]
    if shots:
        s.append(dict(at=3.25, do="capture", path=str(shots / "1-bunker.png")))
    return s


def bomb_steps(shots):
    s = [dict(at=1.4, do="nointro"),
         dict(at=1.5, do="toggle", synced=True),
         dict(at=1.5, do="pause", paused=True),
         dict(at=1.51, do="brush", id=0, rx=12, ry=12),
         dict(at=1.52, do="tool", tool="DEFAULT_PT_C5"),
         dict(at=1.53, do="points", x1=300, y1=178, x2=300, y2=178, held=False),   # the charge
         dict(at=1.54, do="tool", tool="DEFAULT_PT_LNTG"),
         dict(at=1.55, do="rect", x1=294, y1=160, x2=306, y2=165),                # the cold trigger, touching it
         dict(at=1.56, do="tool", tool="DEFAULT_PT_SNOW"),
         dict(at=1.57, do="rect", x1=280, y1=146, x2=320, y2=198),                # all packed in snow
         dict(at=1.7, do="copy", x1=276, y1=138, x2=324, y2=198),
         dict(at=1.8, do="toggle"),
         dict(at=3.0, do="mark", name="armed", count=COUNTED),
         dict(at=3.05, do="place", x=276, y=138)]                               # on the bunker's roof
    if shots:
        s += [dict(at=3.15, do="capture", path=str(shots / "2-bomb-lands.png")),
              dict(at=3.6, do="capture", path=str(shots / "3-blast.png")),
              dict(at=6.5, do="capture", path=str(shots / "4-after.png"))]
    return s + [dict(at=7.0, do="mark", name="after", count=COUNTED)]


def main():
    with tempfile.TemporaryDirectory(prefix="tpt-bunker-bomb-") as td:
        root = Path(td)
        if SHOTS:
            SHOTS.mkdir(parents=True, exist_ok=True)
        port = free_port()
        env = dict(os.environ, TPT_ROOM_TOKEN="room-test", SDL_VIDEODRIVER="dummy")
        env.pop("TPT_ADMIN_TOKEN", None)
        log = (root / "server.log").open("w+")
        server = subprocess.Popen([SERVER, "--port", str(port), "--seed", "1357", "--run-for", "60"], cwd=root, env=env,
                                  stdout=log, stderr=subprocess.STDOUT)
        procs = []
        try:
            deadline = time.monotonic() + 10
            while "Listening on" not in (root / "server.log").read_text():
                assert time.monotonic() < deadline and server.poll() is None, (root / "server.log").read_text()
                time.sleep(.1)
            watcher = subprocess.Popen([CLIENT, "--url", f"ws://127.0.0.1:{port}", "--name", "Watcher", "--headless",
                                        "--local-sim", "--run-for", "16", "--no-reconnect",
                                        "--metrics", str(root / "watcher.jsonl")], cwd=root, env=env,
                                       stdout=(root / "watcher.out").open("w"), stderr=subprocess.STDOUT)
            procs.append(watcher)

            def game(name, steps):
                folder = root / name; folder.mkdir()
                (folder / "powder.pref").write_text("{}")  # not a first run (no install prompt)
                (folder / "steps.jsonl").write_text("".join(json.dumps(step) + "\n" for step in steps + [dict(at=9.0, do="quit")]))
                genv = dict(env, TPT_MP_AUTOMATION=str(folder / "steps.jsonl"), TPT_MP_AUTOMATION_LOG=str(folder / "log.jsonl"))
                command = [GAME, f"ddir:{folder}", f"join:ws://127.0.0.1:{port}", f"name:{name}"]
                if not os.environ.get("DISPLAY") and not os.environ.get("WAYLAND_DISPLAY") and shutil.which("xvfb-run"):
                    command = ["xvfb-run", "-a", "-s", "-screen 0 1280x1024x24"] + command
                    genv.pop("SDL_VIDEODRIVER")
                p = subprocess.Popen(command, cwd=folder, env=genv, stdout=(folder / "out.txt").open("w"), stderr=subprocess.STDOUT)
                procs.append(p)
                return p

            time.sleep(1)
            a = game("PlayerA", bunker_steps(SHOTS))
            time.sleep(1)
            b = game("PlayerB", bomb_steps(SHOTS))
            for p, name in ((a, "PlayerA"), (b, "PlayerB")):
                try:
                    p.wait(timeout=60)
                except subprocess.TimeoutExpired:
                    p.kill(); p.wait()
                    raise AssertionError(f"{name} did not finish: " + (root / name / "out.txt").read_text()[-2000:])
                assert p.returncode == 0, (name, (root / name / "out.txt").read_text()[-2000:])
            watcher.wait(timeout=40)
            assert watcher.returncode == 0, (root / "watcher.out").read_text()

            a_rows, b_rows = rows(root / "PlayerA" / "log.jsonl"), rows(root / "PlayerB" / "log.jsonl")
            mark = lambda r, name: next(x for x in r if x.get("mark") == name)
            bunker, armed, after = mark(a_rows, "bunker"), mark(b_rows, "armed"), mark(b_rows, "after")
            w = rows(root / "watcher.jsonl")[-1]
            for name, last in (("PlayerA", a_rows[-1]), ("PlayerB", b_rows[-1]), ("Watcher", w)):
                assert last["resyncs"] == 0, (name, last)
                assert last["appliedCommands"] == 2, (name, last["appliedCommands"])  # the two pastes
                assert last["verifiedHashes"] >= 5, (name, last["verifiedHashes"])
            assert bunker["counts"]["TTAN"] > 500 and bunker["counts"]["WATR"] > 2000, bunker["counts"]
            assert armed["counts"]["TTAN"] == bunker["counts"]["TTAN"] and armed["counts"]["C5"] == 0, armed["counts"]
            # The bomb went off in the room: the charge is spent, the bunker still stands.
            assert after["counts"]["C5"] < 50 and after["counts"]["TTAN"] > 0, after["counts"]
        finally:
            for p in procs:
                if p.poll() is None:
                    p.kill(); p.wait()
            server.terminate(); server.wait(timeout=10); log.close()
        assert "diverged" not in (root / "server.log").read_text()
    print(f"PASS: A's bunker ({bunker['counts']['TTAN']} TTAN) and B's snow bomb, pasted from their own canvases, "
          f"blew up identically for all three players (C-5 left: {after['counts']['C5']}, TTAN left: {after['counts']['TTAN']})")


if __name__ == "__main__":
    main()
