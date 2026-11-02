#!/usr/bin/env python3
"""A public room against hostile and broken clients.

Two authenticated attackers (one of them with the admin key, since an admin's
messages must not crash the server either) and a stream of unauthenticated
connections send thousands of random and malformed messages: broken JSON,
deep nesting, wrong types, huge and negative numbers, unknown operations,
mutated valid commands, oversized and invalid text, bogus paste uploads,
binary frames, garbage control messages. Meanwhile an honest local-simulation
player keeps verifying every fingerprint, and an honest editor's edits must
all be applied. Afterwards the server still accepts players and stops cleanly.

Usage: hostile_input_test.py TPT_SERVER TPT_REMOTE [SEED [SECONDS]]
"""
import json, os, random, runpy, subprocess, sys, tempfile, threading, time
from pathlib import Path

fixture = runpy.run_path(str(Path(__file__).with_name("network_test.py")))
WS, BUILD = fixture["WS"], fixture["BUILD"]
SEED = int(sys.argv[3]) if len(sys.argv) > 3 else 4242
SECONDS = float(sys.argv[4]) if len(sys.argv) > 4 else 10


def junk_value(r, depth=0):
    kind = r.randrange(12)
    if kind == 0: return None
    if kind == 1: return r.choice([True, False])
    if kind == 2: return r.choice([0, -1, 1, 2**31, -2**31, 2**53 + 1, 2**63, -2**63, 10**30])
    if kind == 3: return r.choice([0.5, -1e308, 1e308, 3.4e39, -0.0, 1e-45])
    if kind == 4: return r.randint(-1000, 1000)
    if kind == 5: return "".join(chr(r.randint(32, 0x2FFF)) for _ in range(r.randint(0, 60)))
    if kind == 6: return "x" * r.choice([46, 300, 5000])
    if kind == 7 and depth < 3: return [junk_value(r, depth + 1) for _ in range(r.randint(0, 4))]
    if kind == 8 and depth < 3: return {r.choice(FIELDS): junk_value(r, depth + 1) for _ in range(r.randint(0, 4))}
    if kind == 9: return r.choice(TOOLS)
    if kind == 10: return r.choice(OPS)
    return r.randint(0, 700)


OPS = ["tool", "brush", "pause", "step", "clear", "settings", "reset", "console", "sign", "sign_button", "stickman",
       "stickman_spawn", "paste", "admin", "nonsense", ""]
FIELDS = ["seq", "op", "type", "tool", "action", "brush", "x1", "y1", "x2", "y2", "radius", "radiusY", "element", "strength",
          "flags", "replace", "colour", "property", "value", "selector", "index", "cmd", "what", "settings", "text", "slot",
          "comm", "pcomm", "save", "upload", "id", "data", "paused", "justification", "remove", "player", "locked", "frame",
          "visible", "x", "y", "drawing", "view", "tick", "revision", "fields"]
TOOLS = ["DEFAULT_PT_DUST", "DEFAULT_PT_STKM", "DEFAULT_PT_FIGH", "DEFAULT_PT_LIGH", "DEFAULT_WL_FAN", "DEFAULT_TOOL_HEAT",
         "DEFAULT_DECOR_SET", "DEFAULT_UI_PROPERTY", "DEFAULT_UI_SIGN", "DEFAULT_UI_SAMPLE", "DEFAULT_PT_NONE", "BOGUS"]
VALID = [
    dict(op="tool", tool="DEFAULT_PT_DUST", action="line", x1=100, y1=100, x2=200, y2=120, radius=3),
    dict(op="tool", tool="DEFAULT_DECOR_SET", action="point", x1=300, y1=200, radius=2, colour=4278190335),
    dict(op="tool", tool="DEFAULT_UI_PROPERTY", action="fill", x1=300, y1=200, radius=0, property=1, value=5),
    dict(op="sign", index=-1, text="hello", x1=50, y1=50, justification=1),
    dict(op="settings", settings=dict(gravityMode=2)),
    dict(op="console", cmd="bubble", x1=200, y1=200),
    dict(op="stickman_spawn", x1=300, y1=100),
    dict(op="paste", upload=1, x1=10, y1=10),
    dict(op="reset", what="air"),
]


def message(r, seq):
    """One hostile message: raw bytes, or JSON of some shape."""
    kind = r.randrange(10)
    if kind == 0:
        return ("raw", bytes(r.getrandbits(8) for _ in range(r.randint(0, 200))))
    if kind == 1:
        return ("raw", ("[" * r.randint(10, 5000)).encode())  # nesting bomb
    if kind == 2:
        return ("binary", bytes(r.getrandbits(8) for _ in range(r.randint(1, 100))))
    if kind == 3:
        return ("json", junk_value(r))  # not even an object, usually
    if kind == 4:  # a valid command with one field mutated
        c = dict(r.choice(VALID)); c["seq"] = seq
        c[r.choice(FIELDS)] = junk_value(r)
        return ("json", c)
    if kind == 5:  # paste uploads with bogus ids, sizes and data
        return ("json", dict(type="paste_chunk", id=junk_value(r) if r.random() < .5 else r.randint(0, 3),
                             data=r.choice(["!!!", "A" * 2999, "A" * 3001, "QUJD", junk_value(r)])))
    if kind == 6:  # control messages with garbage
        t = r.choice(["cursor", "frame_ack", "probe", "resync", "desync_report", "hello", "view", "nonsense"])
        return ("json", dict({k: junk_value(r) for k in r.sample(FIELDS, r.randint(0, 6))}, type=t))
    if kind == 7:  # sequence numbers out of order, repeated, zero, huge
        c = dict(r.choice(VALID)); c["seq"] = r.choice([0, 1, seq, seq - 1, 2**53, -5])
        return ("json", c)
    c = {k: junk_value(r) for k in r.sample(FIELDS, r.randint(0, 10))}
    c["op"] = r.choice(OPS); c["seq"] = seq
    return ("json", c)


def attacker(port, seed, admin, stop, stats):
    r = random.Random(seed)
    seq = 1
    while not stop.is_set():
        try:
            ws = WS(port)
            if r.random() < .9:  # usually logged in: the dangerous case
                ws.send(dict(type="hello", protocol=3, build=BUILD, name=f"Attacker{seed % 100}", token="room-test",
                             adminToken="admin-test" if admin else ""))
            ws.s.settimeout(.2)
            for _ in range(r.randint(1, 200)):
                if stop.is_set():
                    break
                kind, payload = message(r, seq)
                seq += 1
                if admin and isinstance(payload, dict) and payload.get("op") == "admin":
                    continue  # an admin may legitimately kick or lock out the honest players
                if isinstance(payload, dict) and payload.get("type") == "hello" and payload.get("token") != "room-test":
                    continue  # wrong passwords would lock out this address (all players share it here)
                if kind == "raw":
                    ws.send_raw(payload)
                elif kind == "binary":
                    ws.send_raw(payload, opcode=2)
                else:
                    ws.send_raw(json.dumps(payload).encode())
                stats["sent"] += 1
                if r.random() < .05:
                    time.sleep(.01)
            ws.close()
        except (OSError, EOFError, AssertionError, ValueError):
            stats["dropped"] += 1  # the server closed it: expected for many of these
            time.sleep(.02)


def main():
    with tempfile.TemporaryDirectory(prefix="tpt-hostile-") as tmp:
        folder = Path(tmp)
        server, port, log, env = fixture["start"](folder, "--run-for", "300")
        began = time.monotonic()
        stop, stats = threading.Event(), {"sent": 0, "dropped": 0}
        procs, threads = [], []
        try:
            # The honest players: a local-simulation watcher, then an editor.
            wenv = dict(env); wenv.pop("TPT_ADMIN_TOKEN")
            watcher = subprocess.Popen([fixture["CLIENT"], "--url", f"ws://127.0.0.1:{port}", "--name", "Honest", "--headless",
                                        "--local-sim", "--run-for", str(int(SECONDS + 16)), "--no-reconnect",
                                        "--metrics", str(folder / "honest.jsonl")], cwd=folder, env=wenv,
                                       stdout=(folder / "honest.out").open("w"), stderr=subprocess.STDOUT)
            procs.append(watcher)
            time.sleep(1.5)
            editor = WS(port); editor.hello(admin=True, name="Editor")
            editor.send(dict(seq=1, op="pause", paused=False)); editor.control("ack", 1)
            for i, (seed, admin) in enumerate(((SEED, False), (SEED + 1, True))):
                t = threading.Thread(target=attacker, args=(port, seed, admin, stop, stats), daemon=True)
                t.start(); threads.append(t)
            for i in range(4):  # unauthenticated noise
                t = threading.Thread(target=attacker, args=(port, SEED + 10 + i, False, stop, stats), daemon=True)
                t.start(); threads.append(t)
            # The honest editor keeps editing through the storm; every edit is acknowledged.
            seq, deadline = 2, time.monotonic() + SECONDS
            while time.monotonic() < deadline:
                editor.send(dict(seq=seq, op="tool", tool="DEFAULT_PT_WATR", action="point", x1=100 + seq % 400,
                                 y1=60, radius=2))
                editor.control("ack", seq)
                seq += 1
                time.sleep(.05)
            # A video client must keep reading to stay connected: close it before waiting.
            editor.close()
            stop.set()
            for t in threads:
                t.join(timeout=10)
            assert server.poll() is None, f"the server stopped after {time.monotonic() - began:.0f} s"
            time.sleep(6)  # attackers' half-open connections time out (5 s without a hello)
            # Still accepting players, and edits still work.
            late = WS(port); late.hello(admin=True, name="Late")
            late.send(dict(seq=1, op="tool", tool="DEFAULT_PT_SAND", action="point", x1=300, y1=60, radius=2))
            late.control("ack", 1)
            late.close()
            watcher.wait(timeout=30)
            assert watcher.returncode == 0, (folder / "honest.out").read_text()[-3000:]
            honest = [json.loads(l) for l in (folder / "honest.jsonl").read_text().splitlines() if l.strip()][-1]
            assert honest["resyncs"] == 0 and honest["verifiedHashes"] >= 5, honest
            assert honest["appliedCommands"] >= seq, (honest["appliedCommands"], seq)  # the editor's, and Late's
            assert stats["sent"] > 1000, stats
        except BaseException:
            log.seek(0)
            print("server log (end):\n" + "".join(log.readlines()[-40:]), file=sys.stderr)
            raise
        finally:
            stop.set()
            for p in procs:
                if p.poll() is None:
                    p.kill(); p.wait()
            fixture["stop"](server, log)  # clean exit, code 0
    print(f"PASS: seed {SEED}: {stats['sent']} hostile messages ({stats['dropped']} connections dropped by the server); "
          f"the honest player stayed in lockstep and all {seq - 1} honest edits were applied")


if __name__ == "__main__":
    main()
