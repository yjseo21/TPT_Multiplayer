"""Shared upload envelope and live membership allocation with four real sockets."""
from pathlib import Path
import runpy
import tempfile
import time

fixture = runpy.run_path(str(Path(__file__).with_name("network_test.py")))
Editor = runpy.run_path(str(Path(__file__).with_name("network_impairment_test.py")))["Editor"]


def probe(editor, number):
    editor.ws.send(dict(type="probe", id=number))
    value = editor.controls.get(timeout=5)
    assert value["type"] == "telemetry" and value["id"] == number, value
    assert value["inFlight"] <= value["streamWindow"] <= 2, value
    return value


with tempfile.TemporaryDirectory(prefix="tpt-traffic-") as tmp:
    server, port, log, env = fixture["start"](Path(tmp), "--upload-kbps", "512")
    players = []
    started = time.monotonic()
    try:
        players.append(Editor(fixture, port, admin=True))
        assert probe(players[0], 1)["shareKbps"] == 512
        for _ in range(3):
            players.append(Editor(fixture, port))
        time.sleep(.3)
        for i, player in enumerate(players):
            assert probe(player, i+2)["shareKbps"] == 128
        # Fill the top of the world with moving liquid, enough to exhaust burst credit.
        for y in (40, 74, 108):
            for x in range(40, 461, 70):
                players[0].edit(op="brush", x1=x, y1=y, x2=x+40, y2=y, radius=16, element=2)
                time.sleep(.15)
        players[0].edit(op="pause", paused=False)
        before = [p.frames for p in players]
        time.sleep(8)
        for player, count in zip(players, before):
            assert not player.failure, player.failure
            assert player.frames >= count+3, (count, player.frames)
        elapsed = time.monotonic()-started
        used = sum(p.ws.canvas_bytes for p in players)
        assert used <= 612*384*3+4096+elapsed*64000, (used, elapsed)
        assert used > 612*384*3+4096, "scene did not exercise the sustained budget"
        players[-1].close()
        time.sleep(.5)
        assert abs(probe(players[0], 10)["shareKbps"]-512/3) < .01
        revision = players[0].edit(op="pause", paused=True)
        frames = [p.frame(revision) for p in players[:-1]]
        assert all(f["pixels"] == frames[0]["pixels"] for f in frames)
        print(f"PASS: four-player shared 512 kbps envelope ({used} bytes/{elapsed:.2f}s), fair shares, leave reallocation and convergence")
    finally:
        for player in players:
            if not player.closed:
                player.close()
        fixture["stop"](server, log)
