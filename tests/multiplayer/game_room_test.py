#!/usr/bin/env python3
"""The real game (powder) in a room, driven through its own controller.

Runs the game headless (SDL dummy video) with join:ws://... and the
TPT_MP_AUTOMATION script, which calls the same GameController entry points the
mouse and keyboard use. A tpt-remote local-simulation watcher shares the room.
Both must verify every fingerprint without resynchronizing and apply the same
edits; the game's own canvas must keep its own drawing.

Usage: game_room_test.py TPT_SERVER TPT_REMOTE POWDER
"""
import json, os, socket, subprocess, sys, tempfile, time
from pathlib import Path

SERVER, CLIENT, GAME = (str(Path(p).resolve()) for p in sys.argv[1:4])


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def rows(path):
    return [json.loads(line) for line in path.read_text().splitlines() if line.strip()]


STEPS = [
    # Room: an elliptical brush with different radii, a held stroke.
    dict(at=2.0, do="tool", tool="DEFAULT_PT_DUST"),
    dict(at=2.0, do="brush", id=0, rx=6, ry=2),
    dict(at=2.1, do="points", x1=100, y1=100, x2=100, y2=100, held=False),
    dict(at=2.2, do="points", x1=100, y1=100, x2=250, y2=120, held=True),
    # Rectangle brush and the rectangle action.
    dict(at=2.5, do="tool", tool="DEFAULT_PT_WATR"),
    dict(at=2.5, do="brush", id=1, rx=3, ry=3),
    dict(at=2.6, do="rect", x1=300, y1=150, x2=360, y2=190),
    # A wall line.
    dict(at=3.0, do="tool", tool="DEFAULT_WL_FAN"),
    dict(at=3.1, do="line", x1=60, y1=330, x2=560, y2=330),
    # Triangle brush.
    dict(at=3.5, do="tool", tool="DEFAULT_PT_STNE"),
    dict(at=3.5, do="brush", id=2, rx=5, ry=5),
    dict(at=3.6, do="points", x1=400, y1=80, x2=400, y2=80, held=False),
    # A huge brush dragged across the canvas: the client must split it.
    dict(at=4.0, do="tool", tool="DEFAULT_PT_WATR"),
    dict(at=4.0, do="brush", id=0, rx=200, ry=200),
    dict(at=4.1, do="points", x1=50, y1=200, x2=560, y2=220, held=True),
    # An action rooms do not support yet is refused, not applied.
    dict(at=5.8, do="local"),  # after a log row has recorded the trim notice
    # Own canvas: draw there while the room keeps running.
    dict(at=6.2, do="toggle"),
    dict(at=6.3, do="tool", tool="DEFAULT_PT_DUST"),
    dict(at=6.3, do="brush", id=0, rx=3, ry=3),
    dict(at=6.4, do="points", x1=100, y1=200, x2=100, y2=200, held=False),
    dict(at=6.5, do="points", x1=100, y1=200, x2=150, y2=200, held=True),
    dict(at=8.4, do="toggle"),  # own canvas for 2 s: at least one log row
    # Administrator controls become room commands.
    dict(at=8.9, do="pause", paused=True),
    dict(at=9.9, do="step"),
    dict(at=11.9, do="quit"),
]


def main():
    with tempfile.TemporaryDirectory(prefix="tpt-game-room-") as td:
        root = Path(td)
        port = free_port()
        env = dict(os.environ, TPT_ROOM_TOKEN="room-test", TPT_ADMIN_TOKEN="admin-test", SDL_VIDEODRIVER="dummy")
        log = (root / "server.log").open("w+")
        server = subprocess.Popen([SERVER, "--port", str(port), "--seed", "4242"], cwd=root, env=env,
                                  stdout=log, stderr=subprocess.STDOUT)
        procs = []
        try:
            deadline = time.monotonic() + 10
            while "Listening on" not in (root / "server.log").read_text():
                assert time.monotonic() < deadline and server.poll() is None, (root / "server.log").read_text()
                time.sleep(.1)
            watcher_env = dict(env); watcher_env.pop("TPT_ADMIN_TOKEN")
            watcher = subprocess.Popen([CLIENT, "--url", f"ws://127.0.0.1:{port}", "--name", "Watcher", "--headless",
                                        "--local-sim", "--run-for", "18", "--no-reconnect",
                                        "--metrics", str(root / "watcher.jsonl")],
                                       cwd=root, env=watcher_env, stdout=(root / "watcher.out").open("w"),
                                       stderr=subprocess.STDOUT)
            procs.append(watcher)
            time.sleep(1)
            (root / "steps.jsonl").write_text("".join(json.dumps(s) + "\n" for s in STEPS))
            game_dir = root / "game"; game_dir.mkdir()
            game_env = dict(env, TPT_MP_AUTOMATION=str(root / "steps.jsonl"), TPT_MP_AUTOMATION_LOG=str(root / "game.jsonl"))
            game = subprocess.Popen([GAME, f"join:ws://127.0.0.1:{port}", "name:Game"], cwd=game_dir, env=game_env,
                                    stdout=(root / "game.out").open("w"), stderr=subprocess.STDOUT)
            procs.append(game)
            try:
                game.wait(timeout=40)
            except subprocess.TimeoutExpired:
                game.kill(); game.wait()
                log_rows = (root / "game.jsonl").read_text().splitlines() if (root / "game.jsonl").exists() else []
                raise AssertionError("the game did not finish its script:\n" + (root / "game.out").read_text()[-3000:]
                                     + "\nlast status: " + (log_rows[-1][:1500] if log_rows else "(none)"))
            output = (root / "game.out").read_text()
            assert game.returncode == 0, output
            watcher.wait(timeout=30)
            assert watcher.returncode == 0, (root / "watcher.out").read_text()

            g = rows(root / "game.jsonl")
            final = g[-1]
            assert final.get("final"), final
            assert final["resyncs"] == 0 and final["verifiedHashes"] >= 3, final
            assert final["status"] == "PAUSED" and final["pendingEdits"] == 0, (final, output[-3000:])
            assert final["editLatencySamples"] >= 5, final
            assert "not available in rooms" in final["lastNotice"], final["lastNotice"]
            assert final["ownParts"] > 0, "drawing on the own canvas did not reach the own world"
            assert any("too large" in r.get("lastNotice", "") for r in g), [(round(r.get("elapsedSeconds", -1), 2), r.get("lastNotice"), r.get("appliedCommands")) for r in g]
            assert any(r.get("shownIsRoomWorld") for r in g), "the room's world was never shown"
            assert any(r.get("showingRoom") is False for r in g), "the own canvas was never shown"
            assert "Server:" not in output, output  # no edit was rejected (the huge stroke was split)
            assert "diverged" not in output, output

            w = rows(root / "watcher.jsonl")[-1]
            assert w["resyncs"] == 0 and w["status"] == "PAUSED", w
            assert w["appliedCommands"] == final["appliedCommands"] >= 9, (w["appliedCommands"], final["appliedCommands"])
            text = (root / "server.log").read_text()
            assert "diverged" not in text, text
        finally:
            for p in procs:
                if p.poll() is None:
                    p.kill(); p.wait()
            server.terminate(); server.wait(timeout=10); log.close()
    print("PASS: the real game joins a room, edits through its own controller, keeps its own canvas, "
          f"and agrees with another player ({final['appliedCommands']} edits, {final['verifiedHashes']} fingerprints, "
          f"own-edit latency mean {final['editLatencyMeanMs']:.1f} ms)")


if __name__ == "__main__":
    # The Windows build of the game stops after its first frame under SDL's
    # headless drivers (dummy and offscreen), with or without a room, so it can
    # only be tested with a real window. macOS headless is not verified yet.
    if not sys.platform.startswith("linux"):
        print("SKIP: the headless real-game test runs on Linux only (see LATENCY / P3 notes)")
        sys.exit(77)
    main()
