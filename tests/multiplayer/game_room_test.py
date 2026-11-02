#!/usr/bin/env python3
"""The real game (powder) in a room, driven through its own controller.

Runs the game headless (SDL dummy video) with join:ws://... and the
TPT_MP_AUTOMATION script, which calls the same GameController entry points the
mouse and keyboard use. A tpt-remote local-simulation watcher shares the room.
Both must verify every fingerprint without resynchronizing and apply the same
edits; the game's own canvas must keep its own drawing.

Usage: game_room_test.py TPT_SERVER TPT_REMOTE POWDER
"""
import json, os, shutil, socket, subprocess, sys, tempfile, time
from pathlib import Path

SERVER, CLIENT, GAME = (str(Path(p).resolve()) for p in sys.argv[1:4])


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def rows(path):
    return [json.loads(line) for line in path.read_text().splitlines() if line.strip()]


STEPS = [
    # This player's stickman, first, on a clean floor: the stickman tool places it,
    # arrow keys walk it (slow CI machines: wait until the room is synchronized).
    dict(at=1.5, do="tool", tool="DEFAULT_PT_DMND", synced=True),
    dict(at=1.5, do="brush", id=1, rx=4, ry=4),
    dict(at=1.51, do="line", x1=300, y1=360, x2=600, y2=360),
    dict(at=1.55, do="tool", tool="DEFAULT_PT_STKM"),
    dict(at=1.56, do="points", x1=320, y1=330, x2=320, y2=330, held=False),
    dict(at=1.9, do="mark", name="stickman-placed"),
    dict(at=1.95, do="key", key="right", pressed=True),
    dict(at=3.4, do="key", key="right", pressed=False),
    dict(at=3.45, do="mark", name="stickman-walked"),
    # Copy the stickman's floor and paste it higher up: the save travels to everyone.
    dict(at=3.5, do="mark", name="before-paste"),
    dict(at=3.55, do="paste", x1=300, y1=354, x2=600, y2=366, x=300, y=150),
    dict(at=3.95, do="mark", name="after-paste"),
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
    dict(at=8.56, do="reset", what="air"),  # admin: the room's air is reset for everyone
    # Console commands change the room's world for everyone (admin).
    dict(at=8.57, do="console", text="bubble 300,60"),
    dict(at=8.58, do="console", text="create metl 500,60"),
    dict(at=8.59, do="console", text="set tmp metl 9"),
    # Decoration and property tools, and signs (with a button), reach the room too.
    dict(at=8.591, do="decocolour", r=255, g=0, b=0, a=255),
    dict(at=8.592, do="tool", tool="DEFAULT_DECOR_SET"),
    dict(at=8.593, do="brush", id=0, rx=2, ry=2),
    dict(at=8.594, do="points", x1=500, y1=60, x2=500, y2=60, held=False),
    dict(at=8.595, do="property", name="tmp2", value="77"),
    dict(at=8.596, do="tool", tool="DEFAULT_UI_PROPERTY"),
    dict(at=8.597, do="points", x1=500, y1=60, x2=500, y2=60, held=False),
    dict(at=8.598, do="sign", text="Hello room", x1=120, y1=40),
    dict(at=8.599, do="sign", text="{b|press}", x1=200, y1=40),
    dict(at=8.5995, do="console", text="create metl 200,40"),  # a button sparks the conductor under it
    # As in single-player, set reports how many particles it changes (the floor).
    dict(at=8.5997, do="console", text="set life dmnd 0"),
    # The room dialog over the game: the room keeps running underneath.
    dict(at=8.6, do="dialog"),
    dict(at=8.8, do="mark", name="dialog-open"),
    dict(at=10.6, do="mark", name="dialog-later"),
    dict(at=10.8, do="dialogcancel"),
    # Mode shortcuts change the room's settings, for everyone (the game is admin).
    dict(at=10.9, do="gravity"),   # vertical -> off
    dict(at=10.9, do="gravity"),   # off -> radial, both before the first applies
    dict(at=10.9, do="air"),       # on -> pressure off
    dict(at=10.9, do="edge"),      # void -> solid
    dict(at=10.9, do="aheat"),     # off -> on
    dict(at=10.92, do="options", gravityMode=3, customGravityX=0.5),  # the options dialog: custom
    dict(at=10.95, do="newtonian"),  # Newtonian gravity on, for everyone (room solver)
    # Administrator controls become room commands.
    dict(at=11.0, do="pause", paused=True),
    dict(at=11.5, do="signbutton"),  # paused: the spark is still there at the mark
    dict(at=12.0, do="step"),
    dict(at=13.5, do="mark", name="before-leave"),
    # Leave, then join again through the dialog, which remembers the room.
    dict(at=13.6, do="leave"),
    dict(at=13.9, do="mark", name="left"),
    dict(at=14.2, do="dialog"),
    dict(at=14.5, do="dialogjoin"),
    dict(at=16.0, do="quit", synced=True),  # once the rejoin has synchronized (slow CI machines)
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
                                        "--local-sim", "--run-for", "24", "--no-reconnect",
                                        "--metrics", str(root / "watcher.jsonl")],
                                       cwd=root, env=watcher_env, stdout=(root / "watcher.out").open("w"),
                                       stderr=subprocess.STDOUT)
            procs.append(watcher)
            time.sleep(1)
            (root / "steps.jsonl").write_text("".join(json.dumps(s) + "\n" for s in STEPS))
            game_dir = root / "game"; game_dir.mkdir()
            # Not a first run: otherwise the game opens a modal install prompt on its
            # first tick, which a headless test cannot answer.
            (game_dir / "powder.pref").write_text("{}")
            game_env = dict(env, TPT_MP_AUTOMATION=str(root / "steps.jsonl"), TPT_MP_AUTOMATION_LOG=str(root / "game.jsonl"))
            # Own data directory: hermetic, and never a real player's preferences.
            command = [GAME, f"ddir:{game_dir}", f"join:ws://127.0.0.1:{port}", "name:Game"]
            # SDL's dummy video driver can stop the game's frame loop after one frame
            # (seen on CI and on Windows). Without a display, use a virtual X server.
            if not os.environ.get("DISPLAY") and not os.environ.get("WAYLAND_DISPLAY") and shutil.which("xvfb-run"):
                command = ["xvfb-run", "-a", "-s", "-screen 0 1280x1024x24"] + command
                game_env.pop("SDL_VIDEODRIVER")
            game = subprocess.Popen(command, cwd=game_dir, env=game_env,
                                    stdout=(root / "game.out").open("w"), stderr=subprocess.STDOUT)
            procs.append(game)
            try:
                game.wait(timeout=50)
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
            # Rejoined through the dialog: a fresh, synchronized connection.
            assert final["inRoom"] and final["showingRoom"] and final["shownIsRoomWorld"], final
            assert final["resyncs"] == 0 and final["status"] == "PAUSED", final
            assert [r["inRoom"] for r in g if r.get("mark") == "left"] == [False], "leaving the room was never seen"
            marks = [r for r in g if r.get("mark") == "before-leave"]
            assert len(marks) == 1, g
            before = marks[0]
            assert before["resyncs"] == 0 and before["verifiedHashes"] >= 3, before
            assert before["status"] == "PAUSED" and before["pendingEdits"] == 0, (before, output[-3000:])
            assert before["editLatencySamples"] >= 5, before
            assert any("not available in rooms" in r.get("lastNotice", "") for r in g), "no refused action was reported"
            # The room's settings changed for this player and (fingerprints verified, no
            # resync) for the watcher; the player's own world kept its own.
            settings = (before["gravityMode"], before["airMode"], before["edgeMode"], before["ambientHeat"], before["newtonianGravity"])
            assert settings == (3, 1, 1, True, True) and before["customGravityX"] == 0.5, before
            assert before["ownGravityMode"] == 0, before
            assert final["gravityMode"] == 3 and final["edgeMode"] == 1, "a rejoined player gets the room's settings"
            assert final["ownParts"] > 0, "drawing on the own canvas did not reach the own world"
            # Player list in the HUD, and the preview of edits still in flight.
            assert "Game (you, admin)" in before["roomStatus"] and "Watcher" in before["roomStatus"], before["roomStatus"]
            assert max(r.get("previewPeak", 0) for r in g) > 0, "no pending edit was ever previewed"
            assert final["previewPeak"] == 0 or final["pendingEdits"] > 0, final
            # Under the dialog the room's world kept advancing.
            under = [r for r in g if r.get("mark") in ("dialog-open", "dialog-later")]
            assert len(under) == 2 and all(r["dialogOpen"] and r["status"] != "PAUSED" for r in under), under
            # Advancing at all is the point (slow CI machines run few frames under a dialog).
            assert under[1]["localTick"] > under[0]["localTick"], [(r["elapsedSeconds"], r["localTick"]) for r in under]
            # The stickman tool placed this player's own stickman; arrow keys walked it.
            placed = next(r for r in g if r.get("mark") == "stickman-placed")
            walked = next(r for r in g if r.get("mark") == "stickman-walked")
            assert placed["stickmanSlot"] in (1, 2, 3, 4) and "stickmanX" in placed, placed
            assert walked["stickmanX"] > placed["stickmanX"] + 20, (placed.get("stickmanX"), walked.get("stickmanX"))
            # Decoration (red), property (tmp2 77) and two signs reached the room; the button sparked.
            assert before["metlDeco"] == 0xFFFF0000 and before["metlTmp2"] == 77, (before.get("metlDeco"), before.get("metlTmp2"))
            assert before["signs"] == 2 and before["buttonSparked"], (before["signs"], before["buttonSparked"])
            # The console's bubble and create reached the room (a ring of 31 SOAP, one METL).
            # set answers with the number of particles it changes, not 0.
            setting = next(r for r in g if r.get("console") == "set life dmnd 0")
            assert setting["result"].isdigit() and int(setting["result"]) >= 1000, setting
            assert before["soapCount"] >= 25 and before["metlCount"] >= 1, (before["soapCount"], before["metlCount"])
            # The pasted copy of the floor reached the room (and the watcher, by fingerprint).
            pasted = [r for r in g if r.get("mark") in ("before-paste", "after-paste")]
            assert len(pasted) == 2 and pasted[1]["dmndCount"] >= 1.8 * pasted[0]["dmndCount"] > 0,                 [(r["mark"], r["dmndCount"], r.get("lastNotice")) for r in pasted]
            prefs = json.loads((game_dir / "powder.pref").read_text())
            assert prefs.get("Multiplayer", {}).get("Room") == f"ws://127.0.0.1:{port}", prefs
            assert any("too large" in r.get("lastNotice", "") for r in g), [(round(r.get("elapsedSeconds", -1), 2), r.get("lastNotice"), r.get("appliedCommands")) for r in g]
            assert any(r.get("shownIsRoomWorld") for r in g), "the room's world was never shown"
            assert any(r.get("showingRoom") is False for r in g), "the own canvas was never shown"
            assert "Server:" not in output, output  # no edit was rejected (the huge stroke was split)
            assert "diverged" not in output, output

            w = rows(root / "watcher.jsonl")[-1]
            assert w["resyncs"] == 0 and w["status"] == "PAUSED", w
            # Nothing follows the mark: the game's stickman stood still when it left, so the
            # server had no stickman to stop.
            assert w["appliedCommands"] == before["appliedCommands"] >= 9, (w["appliedCommands"], before["appliedCommands"])
            text = (root / "server.log").read_text()
            assert "diverged" not in text, text
        finally:
            for p in procs:
                if p.poll() is None:
                    p.kill(); p.wait()
            server.terminate(); server.wait(timeout=10); log.close()
    print("PASS: the real game joins a room, edits through its own controller, keeps its own canvas, "
          f"agrees with another player, and leaves and rejoins through its dialog ({before['appliedCommands']} edits, "
          f"{before['verifiedHashes']} fingerprints, own-edit latency mean {before['editLatencyMeanMs']:.1f} ms)")


if __name__ == "__main__":
    main()
