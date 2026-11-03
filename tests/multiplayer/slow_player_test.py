#!/usr/bin/env python3
"""One player's computer is too slow for the room; nobody else may suffer.

A local-simulation player whose every tick takes an extra 100 ms
(--debug-step-delay-ms) cannot keep up with a 60 Hz room. It must say so
(TOO SLOW), jump to the room's present from a fresh snapshot instead of falling
ever further behind, and never diverge. Meanwhile a normal player and the
server must run as if it were not there.

Usage: slow_player_test.py TPT_SERVER TPT_REMOTE
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
    with tempfile.TemporaryDirectory(prefix="tpt-slow-player-") as td:
        root = Path(td)
        port = free_port()
        env = dict(os.environ, TPT_ROOM_TOKEN="room-test", SDL_VIDEODRIVER="dummy")
        log = (root / "server.log").open("w+")
        server = subprocess.Popen([SERVER, "--port", str(port), "--seed", "77", "--run-for", "40"], cwd=root, env=env,
                                  stdout=log, stderr=subprocess.STDOUT)
        procs = []
        try:
            deadline = time.monotonic() + 10
            while "Listening on" not in (root / "server.log").read_text():
                assert time.monotonic() < deadline and server.poll() is None, (root / "server.log").read_text()
                time.sleep(.1)

            def player(name, *extra):
                p = subprocess.Popen([CLIENT, "--url", f"ws://127.0.0.1:{port}", "--name", name, "--headless",
                                      "--local-sim", "--run-for", "26", "--metrics", str(root / f"{name}.jsonl"), *extra],
                                     cwd=root, env=env, stdout=(root / f"{name}.out").open("w"), stderr=subprocess.STDOUT)
                procs.append(p)
                return p

            normal = player("Normal", "--no-reconnect")
            slow = player("Slow", "--debug-step-delay-ms", "100")  # about 10 ticks/s against the room's 60
            for p, name in ((normal, "Normal"), (slow, "Slow")):
                p.wait(timeout=60)
                assert p.returncode == 0, (name, (root / f"{name}.out").read_text()[-3000:])

            s = rows(root / "Slow.jsonl")
            trace = [(round(r["elapsedSeconds"]), r.get("status"), round(r.get("behindSeconds", 0), 1), r.get("skipAheads")) for r in s]
            assert any(r.get("status") == "TOO SLOW" for r in s), trace
            assert max(r.get("skipAheads", 0) for r in s) >= 1, trace
            # Jumping ahead is a fresh start, not a divergence, and it really caught up.
            assert all(r.get("resyncs", 0) == 0 for r in s), trace
            skipped = next(i for i, r in enumerate(s) if r.get("skipAheads", 0) >= 1)
            assert any(r.get("localTick") and r.get("behindSeconds", 99) < 3 for r in s[skipped:]), trace
            assert max(r.get("behindSeconds", 0) for r in s) < 15, trace
            assert s[-1]["verifiedHashes"] > 0, s[-1]

            n = rows(root / "Normal.jsonl")
            synced = [r for r in n if r.get("localTick")]
            assert len(synced) >= 20, [(r["elapsedSeconds"], r.get("status")) for r in n]
            assert all(r["status"] in ("LOCAL SIM", "SERVER SLOW", "ROOM SLOWED", "CATCHING UP") and r["resyncs"] == 0 for r in synced), \
                [(round(r["elapsedSeconds"]), r["status"], r["resyncs"]) for r in synced]
            assert n[-1]["connections"] == 1, n[-1]
            assert max(r.get("behindSeconds", 0) for r in synced) < 1, [r.get("behindSeconds") for r in synced]
        finally:
            for p in procs:
                if p.poll() is None:
                    p.kill(); p.wait()
            server.terminate(); server.wait(timeout=10); log.close()

        # The room's own rate never dropped, and each snapshot was a short copy
        # on its thread (the slow player's join and its jump ahead).
        stats = [dict(re.findall(r"(\w+)=([\d.]+)", line)) for line in (root / "server.log").read_text().splitlines()
                 if line.startswith("tick=")]
        assert stats and all(float(st["sim_hz"]) > 50 for st in stats[1:]), stats
        captures = [float(st["snapshot_capture_ms"]) for st in stats if int(st["snapshots"])]
        assert sum(int(st["snapshots"]) for st in stats) >= 2, stats  # the joins (one shared snapshot), the jump ahead
        assert max(captures) < 500, captures  # wall time; typically 4 ms, more on a contended CI machine
    print(f"PASS: a too-slow player shows TOO SLOW and jumps ahead ({max(r['skipAheads'] for r in s)}x) without "
          f"diverging; the other player and the room keep full speed (snapshot copies {max(captures):.1f} ms at most)")


if __name__ == "__main__":
    main()
