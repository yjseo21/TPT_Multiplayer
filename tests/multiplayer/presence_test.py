"""Presence negotiation, cursor validation/expiry, membership and native HUD."""
import json
import os
from pathlib import Path
import runpy
import subprocess
import tempfile
import time
import sys
from contextlib import nullcontext

fixture = runpy.run_path(str(Path(__file__).with_name("network_test.py")))


def receive_until(ws, predicate):
    deadline = time.monotonic()+5
    while time.monotonic() < deadline:
        kind, value = ws.receive()
        assert kind != "close", value
        if kind == "text" and predicate(value):
            return value
    raise AssertionError("expected presence response not received")


def roster(ws, predicate):
    return receive_until(ws, lambda v: v["type"] == "presence" and predicate(v["players"]))["players"]


with (nullcontext(sys.argv[3]) if len(sys.argv) > 3 else tempfile.TemporaryDirectory(prefix="tpt-presence-")) as tmp:
    folder = Path(tmp)
    folder.mkdir(parents=True, exist_ok=True)
    server, port, log, env = fixture["start"](folder)
    sockets, native = [], None
    try:
        a = fixture["WS"](port); sockets.append(a); a.hello(admin=True, name="Alice\n", presence=True)
        first = roster(a, lambda p: len(p) == 1)[0]
        aid = first["player"]
        assert first["name"] == "Alice?" and first["admin"] and not first["visible"]
        initial = a.frame()["revision"]
        b = fixture["WS"](port); sockets.append(b); b.hello(name="Bob", presence=True)
        bid = next(p["player"] for p in roster(a, lambda p: len(p) == 2) if p["name"] == "Bob")
        cursor = dict(type="cursor", visible=True, drawing=True, x=611, y=383, radius=16, player=bid)
        a.send(cursor)
        players = roster(b, lambda ps: any(p["player"] == aid and p["visible"] for p in ps))
        alice = next(p for p in players if p["player"] == aid)
        assert alice["x"] == 611 and alice["y"] == 383 and alice["drawing"]
        assert not next(p for p in players if p["player"] == bid)["visible"], "client spoofed another player's cursor"
        a.send(dict(cursor, x=612))
        assert receive_until(a, lambda v: v["type"] == "error")["message"] == "invalid x"
        time.sleep(2.2)
        roster(b, lambda ps: all(not p["visible"] for p in ps))
        # Cursor messages do not consume edit sequences or modify the world.
        assert b.frame()["revision"] == initial
        a.send(fixture["brush"](1)); a.control("ack", 1)
        b.close(); sockets.remove(b)
        roster(a, lambda ps: len(ps) == 1)
        b = fixture["WS"](port); sockets.append(b); b.hello(name="Bob", presence=True)
        assert next(p["player"] for p in roster(a, lambda ps: len(ps) == 2) if p["name"] == "Bob") != bid
        native = subprocess.Popen([fixture["CLIENT"], "--url", f"ws://127.0.0.1:{port}", "--name", "Viewer",
                                   "--run-for", "4", "--metrics", str(folder/"metrics.jsonl"),
                                   "--output-hud", str(folder/"hud.png")],
                                  env=dict(env, SDL_VIDEODRIVER="dummy"), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        roster(a, lambda ps: len(ps) == 3)
        # Flood cosmetic input; server drops excess and continues accepting edits.
        for _ in range(80):
            a.send(cursor)
        a.send(fixture["brush"](2)); a.control("ack", 2)
        deadline = time.monotonic()+6
        while native.poll() is None and time.monotonic() < deadline:
            a.send(dict(cursor, x=180, y=80))
            roster(a, lambda ps: len(ps) >= 2)
            time.sleep(.4)
        output, _ = native.communicate(timeout=12)
        assert native.returncode == 0, output
        rows = [json.loads(line) for line in (folder/"metrics.jsonl").read_text().splitlines()]
        assert any({p["name"] for p in row.get("players", [])} == {"Alice?", "Bob", "Viewer"} for row in rows)
        assert (folder/"hud.png").read_bytes().startswith(b"\x89PNG")
        print("PASS: names/admin roster, join/leave/rejoin, cursor identity/bounds/expiry, flood isolation, edit sequences and native HUD")
    finally:
        if native and native.poll() is None:
            native.kill(); native.wait()
        for ws in sockets:
            ws.close()
        fixture["stop"](server, log)
