"""Authoritative room lock and kick moderation, including the native CLI."""
import base64
import json
import os
from pathlib import Path
import runpy
import struct
import subprocess
import sys
import tempfile
import time

fixture = runpy.run_path(str(Path(__file__).with_name("network_test.py")))


def until(ws, kind, predicate=lambda v: True):
    deadline = time.monotonic() + 6
    while time.monotonic() < deadline:
        tag, value = ws.receive()
        if tag == kind and predicate(value):
            return value
        if tag == "close" and kind != "close":
            raise AssertionError(f"unexpected disconnect: {value}")
    raise AssertionError(f"expected {kind}")


with tempfile.TemporaryDirectory(prefix="tpt-admin-") as tmp:
    folder = Path(tmp)
    server, port, log, env = fixture["start"](folder)
    clients = []
    try:
        admin = fixture["WS"](port); clients.append(admin)
        admin.hello(admin=True, name="Operator", presence=True)
        admin_id = until(admin, "text", lambda v: v["type"] == "presence")["players"][0]["player"]
        admin.frame()
        guest = fixture["WS"](port); clients.append(guest)
        guest.hello(name="Guest", presence=True)
        roster = until(admin, "text", lambda v: v["type"] == "presence" and len(v["players"]) == 2)["players"]
        guest_id = next(p["player"] for p in roster if p["name"] == "Guest")
        guest.frame()
        initial_revision = admin.canvas["revision"]

        # Non-admins cannot moderate; rejection consumes the sequence.
        guest.send(dict(seq=1, op="admin", action="lock", locked=True))
        assert "administrator" in guest.control("error", 1)["message"]
        guest.send(dict(seq=1, op="admin", action="kick", player=admin_id))
        guest.control("error", 1)
        guest.send(dict(seq=2, op="admin", action="kick", player=admin_id))
        guest.control("error", 2)
        guest.send(fixture["brush"](3))
        guest.control("ack", 3)
        edited = admin.frame(revision=initial_revision + 1)["revision"]

        # Pastes are charged before their save is unpacked, so a stream of broken
        # saves runs into the editing budget instead of costing the server a parse each.
        broken = base64.b64encode(b"OPS1" + bytes(4) + struct.pack("<I", 1000) + b"not bzip2 data").decode()
        reasons = []
        for upload in range(1, 9):
            guest.send(dict(type="paste_chunk", id=upload, data=broken))
            guest.send(dict(seq=3 + upload, op="paste", upload=upload, x1=0, y1=0))
            reasons.append(guest.control("error", 3 + upload)["message"])
        limited = sum("editing rate exceeded" in reason for reason in reasons)
        assert 3 <= limited <= 6, reasons

        admin.send(dict(seq=1, op="admin", action="lock", locked=True))
        admin.control("ack", 1)
        until(guest, "text", lambda v: v["type"] == "room" and v["locked"] is True)
        rejected = fixture["WS"](port); clients.append(rejected)
        rejected.send(dict(type="hello", protocol=3, build=fixture["BUILD"], token="room-test", name="LockedOut"))
        assert "locked" in until(rejected, "close")
        rejected.close(); clients.remove(rejected)

        # A second administrator may enter a locked room to restore access.
        second_admin = fixture["WS"](port); clients.append(second_admin)
        second_admin.hello(admin=True, name="Second", presence=True)
        second_admin.frame()
        second_admin_id = next(p["player"] for p in until(admin, "text", lambda v: v["type"] == "presence" and len(v["players"]) == 3)["players"] if p["name"] == "Second")
        admin.send(dict(seq=2, op="admin", action="kick", player=second_admin_id))
        assert "administrator" in admin.control("error", 2)["message"]
        admin.send(dict(seq=3, op="admin", action="kick", player=admin_id))
        assert "yourself" in admin.control("error", 3)["message"]
        admin.send(dict(seq=4, op="admin", action="kick", player=999999))
        assert "not connected" in admin.control("error", 4)["message"]
        admin.send(dict(seq=5, op="admin", action="kick", player=guest_id))
        admin.control("ack", 5)
        assert "removed by room administrator" in until(guest, "close")
        # Moderation must not alter physics state or increment its revision.
        assert admin.frame()["revision"] == edited
        guest.close(); clients.remove(guest)

        admin.send(dict(seq=6, op="admin", action="lock", locked=False))
        admin.control("ack", 6)
        until(second_admin, "text", lambda v: v["type"] == "room" and v["locked"] is False)
        replacement = fixture["WS"](port); clients.append(replacement)
        replacement.hello(name="Replacement", presence=True)
        replacement.frame(revision=edited)

        # Native client scripted moderation exercises the same command queue / ACK handling.
        command_file = folder / "admin-commands.jsonl"
        command_file.write_text('\n'.join(map(json.dumps, [
            dict(op="admin", action="lock", locked=True),
            dict(op="admin", action="lock", locked=False),
        ])) + '\n')
        replacement.close(); clients.remove(replacement)
        second_admin.close(); clients.remove(second_admin)
        command = [fixture["CLIENT"], "--url", f"ws://127.0.0.1:{port}", "--headless", "--run-for", "5",
                   "--no-reconnect", "--commands", str(command_file), "--metrics", str(folder / "client.jsonl")]
        result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=12)
        assert result.returncode == 0, result.stdout + result.stderr
        last = json.loads((folder / "client.jsonl").read_text().splitlines()[-1])
        assert not last["roomLocked"] and last["pendingEdits"] == 0

        # Guessing the room password: after 5 wrong tries in a minute, even the right one waits.
        for attempt in range(6):
            guess = fixture["WS"](port)
            guess.send(dict(type="hello", protocol=3, build=fixture["BUILD"], token=f"guess-{attempt}", name="Guesser"))
            reason = until(guess, "close")
            guess.close()
            assert ("too many wrong passwords" in reason) == (attempt == 5), (attempt, reason)
        right = fixture["WS"](port)
        right.send(dict(type="hello", protocol=3, build=fixture["BUILD"], token="room-test", name="Guesser"))
        assert "too many wrong passwords" in until(right, "close")
        right.close()
        print("PASS: admin permission, locks, admin bypass, removals, sequence rejection, unchanged physics, native CLI "
              "paste budgets and password guessing limits")
    finally:
        for ws in clients:
            ws.close()
        fixture["stop"](server, log)
