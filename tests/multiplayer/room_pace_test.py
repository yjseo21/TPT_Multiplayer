#!/usr/bin/env python3
"""A player's computer is a little slower than the room: the room waits for it.

A local-simulation player whose every tick takes an extra 15 ms
(--debug-step-delay-ms) can follow about 40 ticks a second, not the room's 60.
It reports its pace, and the room slows down to it (ROOM SLOWED), so it never
falls behind, never jumps ahead and never resynchronizes: the endless
download-and-catch-up loop of a heavy room. Once it leaves, the room returns to
full speed. Another player follows the slowed room without trouble.

Usage: room_pace_test.py TPT_SERVER TPT_REMOTE
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
    with tempfile.TemporaryDirectory(prefix="tpt-room-pace-") as td:
        root = Path(td)
        port = free_port()
        env = dict(os.environ, TPT_ROOM_TOKEN="room-test", SDL_VIDEODRIVER="dummy")
        log = (root / "server.log").open("w+")
        server = subprocess.Popen([SERVER, "--port", str(port), "--seed", "78", "--run-for", "44"], cwd=root, env=env,
                                  stdout=log, stderr=subprocess.STDOUT)
        procs = []
        try:
            deadline = time.monotonic() + 10
            while "Listening on" not in (root / "server.log").read_text():
                assert time.monotonic() < deadline and server.poll() is None, (root / "server.log").read_text()
                time.sleep(.1)

            def player(name, seconds, *extra):
                p = subprocess.Popen([CLIENT, "--url", f"ws://127.0.0.1:{port}", "--name", name, "--headless",
                                      "--local-sim", "--no-reconnect", "--run-for", str(seconds),
                                      "--metrics", str(root / f"{name}.jsonl"), *extra],
                                     cwd=root, env=env, stdout=(root / f"{name}.out").open("w"), stderr=subprocess.STDOUT)
                procs.append(p)
                return p

            normal = player("Normal", 38)
            heavy = player("Heavy", 20, "--debug-step-delay-ms", "15")
            for p, name in ((heavy, "Heavy"), (normal, "Normal")):
                p.wait(timeout=70)
                assert p.returncode == 0, (name, (root / f"{name}.out").read_text()[-3000:])

            h = rows(root / "Heavy.jsonl")
            trace = [(round(r["elapsedSeconds"]), r.get("status"), round(r.get("behindSeconds", 0), 1),
                      r.get("skipAheads"), r.get("pacedHz") and round(r["pacedHz"])) for r in h]
            assert any(r.get("status") == "ROOM SLOWED" for r in h), trace
            assert all(r.get("skipAheads", 0) == 0 and r.get("resyncs", 0) == 0 for r in h), trace
            # Behind only while the room is still at full speed, before the first report lands.
            assert all(r.get("behindSeconds", 0) < 3 for r in h if r["elapsedSeconds"] > 8), trace
            assert h[-1]["verifiedHashes"] > 0, h[-1]

            n = rows(root / "Normal.jsonl")
            synced = [r for r in n if r.get("localTick")]
            assert len(synced) >= 25, [(r["elapsedSeconds"], r.get("status")) for r in n]
            assert all(r["resyncs"] == 0 and r.get("skipAheads", 0) == 0 for r in synced), synced[-1]
            assert n[-1]["connections"] == 1, n[-1]
        finally:
            for p in procs:
                if p.poll() is None:
                    p.kill(); p.wait()
            server.terminate(); server.wait(timeout=10); log.close()

        stats = [dict(re.findall(r"(\w+)=([\d.]+)", line)) for line in (root / "server.log").read_text().splitlines()
                 if line.startswith("tick=")]
        paced = [round(float(st["paced_hz"])) for st in stats]
        rates = [round(float(st["sim_hz"])) for st in stats]
        # Slowed for the heavy player (about 40/s), not to the floor; full speed after it left.
        assert any(30 <= r <= 52 for r in paced), (paced, rates)
        assert paced[-1] == 60 and rates[-1] > 50, (paced, rates)
    print(f"PASS: the room slowed to its slowest player (paced {min(paced)}/s) without anyone falling behind, "
          f"jumping ahead or resynchronizing, then returned to full speed ({paced[-1]}/s)")


if __name__ == "__main__":
    main()
